// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_network_share.h"
#include <atomic>
#include "base/functional/bind.h"
#include "base/files/file_util.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "crFiles/sharing/nearby_files_win.h"
#endif
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_split.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/timer/timer.h"
namespace crfiles {
namespace {
using Error = mojom::SmbError;
using Target = ShareService::RemoteReceiveTarget;
struct DeliveryState {
  std::atomic<bool> canceled{false};
  std::atomic<uint64_t> bytes{0};
  base::WaitableEvent finished;
  Error error = Error::kFailed;  // Published before signaling finished.
};
class Delivery {
 public:
  Delivery(const SmbCredentials& credentials, std::shared_ptr<DeliveryState> state)
      : credentials_(credentials), state_(std::move(state)) {}
  Error Run(std::string relative, base::File file, bool directory, bool probe) {
    file_ = std::move(file); probe_ = probe;
    if (credentials_.location.share.empty()) return Error::kInvalidAddress;
    if (!probe) {
      if (relative.ends_with('/')) relative.pop_back();
      auto parts = base::SplitString(relative, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
      if (parts.empty() || relative.size() > 4096) return Error::kInvalidAddress;
      auto path = credentials_.location.path;
      for (size_t i = 0; i < parts.size(); ++i) {
        const auto& part = parts[i];
        if (part.empty() || part == "." || part == ".." || part.find_first_of("\\:") != std::string::npos ||
            part.find('\0') != std::string::npos) return Error::kInvalidAddress;
        if (!path.empty()) path += '/';
        path += part;
        if (directory || i + 1 < parts.size()) directories_.push_back(path);
      }
      target_ = std::move(path); directory_ = directory;
      if (!directory && !file_.IsValid()) return Error::kLocalFile;
    }
    if (!connection_.Start(base::BindOnce(&Delivery::Finish, weak_.GetWeakPtr(), Error::kFailed))) return Error::kFailed;
    activity_ = base::TimeTicks::Now();
    timer_.Start(FROM_HERE, base::Milliseconds(100), this, &Delivery::CheckActivity);
    const auto& c = credentials_;
    connection_.service()->Connect(c.location.server, c.user, c.password, c.domain, c.location.share, c.location.path,
        base::BindOnce(&Delivery::Connected, weak_.GetWeakPtr()));
    loop_.Run();
    timer_.Stop(); weak_.InvalidateWeakPtrs(); connection_.Stop();
    return error_;
  }
 private:
  void CheckActivity() {
    if (state_->canceled || base::TimeTicks::Now() - activity_ > base::Seconds(45)) Finish(Error::kFailed);
  }
  void Connected(mojom::SmbListingPtr listing) {
    // Authentication failure is not evidence that the destination is read-only.
    if (listing->error != Error::kOk) { Finish(Error::kFailed); return; }
    if (listing->write_access == mojom::SmbWriteAccess::kReadOnly) { Finish(Error::kAccessDenied); return; }
    activity_ = base::TimeTicks::Now();
    if (probe_) {
      connection_.service()->CheckWritable(credentials_.location.share, credentials_.location.path,
          base::BindOnce(&Delivery::Finish, weak_.GetWeakPtr()));
    } else NextDirectory();
  }
  void NextDirectory() {
    if (state_->canceled) { Finish(Error::kFailed); return; }
    activity_ = base::TimeTicks::Now();
    if (next_directory_ < directories_.size()) {
      connection_.service()->Mutate(credentials_.location.share, directories_[next_directory_++],
          mojom::SmbMutation::kEnsureDirectory, {}, base::BindOnce(&Delivery::DirectoryCreated, weak_.GetWeakPtr()));
    } else if (directory_) Finish(Error::kOk);
    else connection_.service()->Upload(credentials_.location.share, target_, std::move(file_),
        mojom::SmbTransferPurpose::kNearbyReceive, connection_.ObserveTransfer(
            base::BindRepeating(&Delivery::UploadProgress, weak_.GetWeakPtr())),
        base::BindOnce(&Delivery::Finish, weak_.GetWeakPtr()));
  }
  void DirectoryCreated(Error error) {
    if (error != Error::kOk) Finish(error);
    else NextDirectory();
  }
  void UploadProgress(uint64_t bytes, uint64_t) {
    state_->bytes = bytes;
    activity_ = base::TimeTicks::Now();
  }
  void Finish(Error error) { error_ = error; loop_.Quit(); }
  const SmbCredentials& credentials_;
  std::shared_ptr<DeliveryState> state_;
  SmbConnection connection_;
  base::RunLoop loop_{base::RunLoop::Type::kNestableTasksAllowed};
  base::RepeatingTimer timer_;
  base::TimeTicks activity_;
  base::File file_;
  std::string target_;
  std::vector<std::string> directories_;
  size_t next_directory_ = 0;
  bool directory_ = false, probe_ = false;
  Error error_ = Error::kFailed;
  base::WeakPtrFactory<Delivery> weak_{this};
};
Error Deliver(const SmbCredentials& credentials, std::string relative, base::File source, bool directory, bool probe,
              base::RepeatingCallback<bool()> canceled, base::RepeatingCallback<void(uint64_t)> progress) {
  if (canceled && canceled.Run()) return Error::kFailed;
  // The receive worker stays responsible for TLS and heartbeat messages. Mojo
  // has its own IO thread; callbacks never touch Views or the TLS channel.
  base::Thread io("SmbReceive");
  base::Thread::Options options; options.message_pump_type = base::MessagePumpType::IO;
  if (!io.StartWithOptions(std::move(options))) return Error::kFailed;
  auto state = std::make_shared<DeliveryState>();
  if (!io.task_runner()->PostTask(FROM_HERE, base::BindOnce(
      [](SmbCredentials c, std::string relative, base::File source, bool directory, bool probe,
         std::shared_ptr<DeliveryState> state) {
        Delivery delivery(c, state);
        state->error = delivery.Run(std::move(relative), std::move(source), directory, probe);
        state->finished.Signal();
      }, credentials, std::move(relative), std::move(source), directory, probe, state))) return Error::kFailed;
  while (!state->finished.TimedWait(base::Milliseconds(100))) {
    if (canceled && canceled.Run()) state->canceled = true;
    if (progress && !state->canceled) progress.Run(state->bytes);
  }
  io.Stop();
  return state->error;
}
class NetworkReceiveTarget : public Target {
 public:
  explicit NetworkReceiveTarget(SmbCredentials credentials) : credentials_(std::move(credentials)) {}
  uint64_t MaximumFileSize() const override { return kSmbNearbyFileLimit; }
  Result Store(const std::string& relative, base::File source, bool directory,
               base::RepeatingCallback<bool()> canceled, base::RepeatingCallback<void(uint64_t)> progress) override {
    const auto result = Deliver(credentials_, relative, std::move(source), directory, false, std::move(canceled), std::move(progress));
    return result == Error::kOk ? Result::kOk : result == Error::kAccessDenied ? Result::kReadOnly :
        result == Error::kAlreadyExists ? Result::kExists : Result::kFailed;
  }
 private:
  const SmbCredentials credentials_;
};
}
mojom::SmbError CheckNetworkShareWritable(SmbCredentials credentials, base::FilePath cache_directory,
    base::RepeatingCallback<bool()> canceled) {
  const auto result = Deliver(credentials, {}, {}, false, true, canceled, {});
  if (result != Error::kOk) return result;
  if (canceled && canceled.Run()) return Error::kFailed;
  if (!base::CreateDirectory(cache_directory)) return Error::kLocalFile;
#if BUILDFLAG(IS_WIN)
  if (!nearby::ProtectPrivateDirectory(cache_directory)) return Error::kLocalFile;
#else
  if (!base::SetPosixFilePermissions(cache_directory, 0700)) return Error::kLocalFile;
#endif
  return Error::kOk;
}
std::shared_ptr<ShareService::RemoteReceiveTarget> CreateNetworkReceiveTarget(SmbCredentials credentials) {
  return std::make_shared<NetworkReceiveTarget>(std::move(credentials));
}
}
