// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_video_thumbnail.h"
#include <deque>
#include <utility>
#include "crFiles/crfiles_video_thumbnail.mojom.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPixmap.h"
namespace crfiles {
namespace {
struct VideoRequest {
  VideoRequest(const base::FilePath& source, int size, std::shared_ptr<std::atomic_bool> token);
  ~VideoRequest();
  base::FilePath path;
  int edge;
  std::shared_ptr<std::atomic_bool> canceled;
  std::atomic_bool abandoned{false};
  base::WaitableEvent done{base::WaitableEvent::ResetPolicy::MANUAL, base::WaitableEvent::InitialState::NOT_SIGNALED};
  gfx::ImageSkia image;  // Written before Signal(), read only after waiting.
};
VideoRequest::VideoRequest(const base::FilePath& source, int size, std::shared_ptr<std::atomic_bool> token)
    : path(source), edge(size), canceled(std::move(token)) {}
VideoRequest::~VideoRequest() = default;

// All Mojo/process/timer state lives on a dedicated IO sequence. Image workers
// wait here, never the UI. Keep the healthy child across requests, then retire
// it after 30 idle seconds. Timeout, cancellation or disconnect replaces it.
class VideoConnection {
 public:
  VideoConnection();
  ~VideoConnection();
  void Decode(std::shared_ptr<VideoRequest> request);
 private:
  bool Connect();
  void StopProcess();
  void StartDecode(std::shared_ptr<VideoRequest> request);
  void Poll();
  void Disconnected(uint64_t generation);
  void Received(std::shared_ptr<VideoRequest> request, mojom::VideoThumbnailPtr result);
  void Finish(gfx::ImageSkia image, bool retire);
  base::Process process_;
  mojo::Remote<mojom::VideoThumbnailService> service_;
  std::shared_ptr<VideoRequest> pending_;
  std::deque<std::shared_ptr<VideoRequest>> queue_;
  base::RepeatingTimer poll_;
  base::OneShotTimer idle_;
  base::TimeTicks deadline_;
  uint64_t generation_ = 0;
};
VideoConnection::VideoConnection() = default;
VideoConnection::~VideoConnection() { StopProcess(); }
bool VideoConnection::Connect() {
  if (service_.is_bound() && service_.is_connected()) return true;
  StopProcess();
  base::FilePath executable;
  if (!GetCrFilesHelperExecutable(&executable)) return false;
  base::CommandLine command(executable);
  command.AppendSwitch(kVideoThumbnailProcessSwitch);
  base::LaunchOptions options;
#if BUILDFLAG(IS_WIN)
  options.start_hidden = true;
#endif
  mojo::PlatformChannel channel;
  channel.PrepareToPassRemoteEndpoint(&options, &command);
  process_ = base::LaunchProcess(command, options);
  channel.RemoteProcessLaunchAttempted();
  if (!process_.IsValid()) return false;
  auto pipe = mojo::OutgoingInvitation::SendIsolated(channel.TakeLocalEndpoint(), "crfiles-video", process_.Handle());
  if (!pipe.is_valid()) { StopProcess(); return false; }
  service_.Bind(mojo::PendingRemote<mojom::VideoThumbnailService>(std::move(pipe), 0));
  service_.set_disconnect_handler(base::BindOnce(&VideoConnection::Disconnected, base::Unretained(this), generation_));
  return true;
}
void VideoConnection::StopProcess() {
  ++generation_;
  idle_.Stop();
  service_.reset();
  if (process_.IsValid()) {
    process_.Terminate(base::Process::kResultCodeKilledBadMessage, false);
    base::EnsureProcessTerminated(std::move(process_));
  }
}
void VideoConnection::Decode(std::shared_ptr<VideoRequest> request) {
  if (!request || request->abandoned.load() || (request->canceled && request->canceled->load())) {
    if (request) request->done.Signal();
    return;
  }
  if (pending_) {
    std::erase_if(queue_, [](const auto& item) {
      return !item || item->abandoned.load() || (item->canceled && item->canceled->load());
    });
    if (queue_.size() < 16) queue_.push_back(std::move(request));
    else request->done.Signal();
    return;
  }
  StartDecode(std::move(request));
}
void VideoConnection::StartDecode(std::shared_ptr<VideoRequest> request) {
  idle_.Stop();
  pending_ = std::move(request);
  if (pending_->abandoned.load() || (pending_->canceled && pending_->canceled->load()) || !Connect()) {
    Finish({}, true); return;
  }
  deadline_ = base::TimeTicks::Now() + base::Seconds(8);
  poll_.Start(FROM_HERE, base::Milliseconds(40), this, &VideoConnection::Poll);
  service_->Decode(pending_->path, pending_->edge, CreateFileAccessBookmark(pending_->path),
      base::BindOnce(&VideoConnection::Received, base::Unretained(this), pending_));
}
void VideoConnection::Poll() {
  if (pending_ && (pending_->abandoned.load() || (pending_->canceled && pending_->canceled->load()) ||
      base::TimeTicks::Now() >= deadline_)) Finish({}, true);
}
void VideoConnection::Disconnected(uint64_t generation) {
  if (generation == generation_) Finish({}, true);
}
void VideoConnection::Received(std::shared_ptr<VideoRequest> request, mojom::VideoThumbnailPtr result) {
  if (pending_ != request) return;
  if (request->abandoned.load() || (request->canceled && request->canceled->load())) { Finish({}, true); return; }
  if (!result) { Finish({}, false); return; }
  const uint32_t edge = static_cast<uint32_t>(request->edge);
  if (!result->width || !result->height || result->width > edge || result->height > edge ||
      result->pixels.size() != 128u * 128u * 4u) { Finish({}, true); return; }
  SkBitmap bitmap;
  const auto info = SkImageInfo::MakeN32Premul(result->width, result->height);
  if (!bitmap.tryAllocPixels(info) || !bitmap.writePixels(SkPixmap(info, result->pixels.data(), result->width * 4))) {
    Finish({}, false); return;
  }
  bitmap.setImmutable();
  auto image = gfx::ImageSkia::CreateFromBitmap(bitmap, 1.f);
  image.MakeThreadSafe();
  Finish(std::move(image), false);
}
void VideoConnection::Finish(gfx::ImageSkia image, bool retire) {
  poll_.Stop();
  if (retire) StopProcess();
  else idle_.Start(FROM_HERE, base::Seconds(30), this, &VideoConnection::StopProcess);
  auto request = std::move(pending_);
  if (request) { request->image = std::move(image); request->done.Signal(); }
  while (!queue_.empty()) {
    auto next = std::move(queue_.front());
    queue_.pop_front();
    if (next && !next->abandoned.load() && (!next->canceled || !next->canceled->load())) {
      StartDecode(std::move(next));
      return;
    }
    if (next) next->done.Signal();
  }
}
struct VideoHost {
  VideoHost();
  ~VideoHost();
  base::Thread thread{"crFilesVideoClient"};
  bool started;
  scoped_refptr<base::SingleThreadTaskRunner> runner;
  base::Lock slot;
};
VideoHost::VideoHost() : started(thread.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0))) {
  if (started) runner = thread.task_runner();
}
VideoHost::~VideoHost() = default;
}
gfx::ImageSkia LoadVideoThumbnail(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled) {
  if (maximum_edge <= 0 || maximum_edge > 128 || (canceled && canceled->load())) return {};
  static base::NoDestructor<VideoHost> host;
  if (!host->started) return {};
  base::AutoLock guard(host->slot);
  if (canceled && canceled->load()) return {};
  auto request = std::make_shared<VideoRequest>(path, maximum_edge, canceled);
  if (!host->runner->PostTask(FROM_HERE, base::BindOnce([](std::shared_ptr<VideoRequest> item) {
        static base::NoDestructor<VideoConnection> connection;
        connection->Decode(std::move(item));
      }, request))) return {};
  const auto deadline = base::TimeTicks::Now() + base::Seconds(9);
  while (!request->done.IsSignaled()) {
    if (canceled && canceled->load()) {
      request->abandoned = true;
      return {};
    }
    if (base::TimeTicks::Now() >= deadline) {
      request->abandoned = true;
      return {};
    }
    request->done.TimedWait(base::Milliseconds(50));
  }
  if (canceled && canceled->load()) return {};
  return request->image;
}
}
