// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_search.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/kill.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
namespace crfiles {
SearchConnection::SearchConnection() = default;
SearchConnection::~SearchConnection() {
  service_.reset();
  if (process_.IsValid()) base::EnsureProcessTerminated(std::move(process_));
}
std::shared_ptr<SearchConnection> SearchConnection::Get() {
  static base::NoDestructor<std::weak_ptr<SearchConnection>> instance;
  auto connection = instance->lock();
  if (!connection) { connection = std::make_shared<SearchConnection>(); *instance = connection; }
  return connection;
}
bool SearchConnection::Connect() {
  if (service_.is_bound() && service_.is_connected()) return true;
  service_.reset();
  if (process_.IsValid()) base::EnsureProcessTerminated(std::move(process_));
  base::FilePath executable;
  if (!GetCrFilesHelperExecutable(&executable)) return false;
  base::CommandLine command(executable);
  command.AppendSwitch(kSearchProcessSwitch);
  base::LaunchOptions options;
  mojo::PlatformChannel channel;
  channel.PrepareToPassRemoteEndpoint(&options, &command);
  process_ = base::LaunchProcess(command, options);
  channel.RemoteProcessLaunchAttempted();
  if (!process_.IsValid()) return false;
  auto pipe = mojo::OutgoingInvitation::SendIsolated(channel.TakeLocalEndpoint(), "crfiles-search", process_.Handle());
  if (!pipe.is_valid()) { base::EnsureProcessTerminated(std::move(process_)); return false; }
  service_.Bind(mojo::PendingRemote<mojom::SearchService>(std::move(pipe), 0));
  service_.set_disconnect_handler(base::BindOnce(&SearchConnection::Disconnected, base::Unretained(this)));
  return true;
}
void SearchConnection::Disconnected() {
  service_.reset();
  auto jobs = std::move(jobs_);
  jobs_.clear();
  for (auto& [id, callback] : jobs) callback.Run();
}
uint64_t SearchConnection::Start(mojom::SearchOptionsPtr options, base::RepeatingClosure disconnected) {
  if (options->roots.size() > 10000 || !Connect()) return 0;
  if (options->scoped) {
    for (auto& root : options->roots)
      root->access_bookmark = CreateFileAccessBookmark(base::FilePath::FromUTF8Unsafe(root->path));
  } else options->access_bookmark = CreateFileAccessBookmark(base::FilePath::FromUTF8Unsafe(options->root));
  const uint64_t id = ++next_job_;
  jobs_.emplace(id, std::move(disconnected)); service_->Start(id, std::move(options));
  return id;
}
void SearchConnection::Next(uint64_t job, mojom::SearchService::NextCallback callback) {
  if (service_.is_bound()) service_->Next(job, std::move(callback));
}
void SearchConnection::Pause(uint64_t job, bool paused) { if (service_.is_bound()) service_->SetPaused(job, paused); }
void SearchConnection::Cancel(uint64_t job) { jobs_.erase(job); if (service_.is_bound()) service_->Cancel(job); }
}
