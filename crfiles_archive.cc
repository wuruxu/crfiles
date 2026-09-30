// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_archive.h"
#include "crFiles/crfiles_archive.mojom.h"
#include <functional>
#include <string_view>
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/memory/raw_ref.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
namespace crfiles {
ArchiveProgress::ArchiveProgress() = default;
ArchiveProgress::~ArchiveProgress() = default;
void ArchiveProgress::SetCurrentPath(const base::FilePath& path) {
  base::AutoLock guard(progress_lock_); current_path_ = path;
}
base::FilePath ArchiveProgress::CurrentPath() {
  base::AutoLock guard(progress_lock_); return current_path_;
}
ArchiveConflictChoice ArchiveProgress::WaitForConflict(const base::FilePath& path) {
  conflict_resolved_.Reset();
  {
    base::AutoLock lock(conflict_lock_);
    conflict_path_ = path;
    conflict_choice_ = ArchiveConflictChoice::kWaiting;
  }
  // Closing the bubble/window sets canceled even if no button was chosen.
  while (!canceled && !conflict_resolved_.TimedWait(base::Milliseconds(100))) {}
  base::AutoLock lock(conflict_lock_);
  conflict_path_.clear();
  return canceled ? ArchiveConflictChoice::kCancel : conflict_choice_;
}
base::FilePath ArchiveProgress::PendingConflict() {
  base::AutoLock lock(conflict_lock_);
  return conflict_path_;
}
void ArchiveProgress::ResolveConflict(const base::FilePath& path, ArchiveConflictChoice choice) {
  base::AutoLock lock(conflict_lock_);
  if (conflict_path_ != path || conflict_choice_ != ArchiveConflictChoice::kWaiting) return;
  conflict_choice_ = choice;
  conflict_path_.clear();
  if (choice == ArchiveConflictChoice::kCancel) canceled = true;
  conflict_resolved_.Signal();
}
namespace {
class ProgressListener : public mojom::ArchiveProgressListener {
 public:
  explicit ProgressListener(ArchiveProgress& progress) : progress_(progress) {}
  void OnProgress(uint64_t bytes) override { progress_->bytes = bytes; }
  void OnCompressionProgress(uint64_t bytes, uint64_t entries, const base::FilePath& path) override {
    progress_->bytes = bytes; progress_->entries_done = entries; progress_->SetCurrentPath(path);
  }
 private:
  raw_ref<ArchiveProgress> progress_;
};
using ArchiveRequest = base::OnceCallback<void(mojom::ArchiveDecoder*,
    mojo::PendingRemote<mojom::ArchiveProgressListener>, base::OnceCallback<void(bool, bool)>)>;
bool RunArchiveOnIO(ArchiveProgress& progress, ArchiveRequest request) {
  base::FilePath executable;
  if (progress.canceled || !base::PathService::Get(base::FILE_EXE, &executable)) return false;
  base::CommandLine command(executable);
  command.AppendSwitch(kArchiveProcessSwitch);
  base::LaunchOptions options;
#if BUILDFLAG(IS_WIN)
  options.start_hidden = true;
#endif
  mojo::PlatformChannel channel;
  channel.PrepareToPassRemoteEndpoint(&options, &command);
  auto process = base::LaunchProcess(command, options);
  channel.RemoteProcessLaunchAttempted();
  if (!process.IsValid()) return false;
  bool success = false;
  auto pipe = mojo::OutgoingInvitation::SendIsolated(channel.TakeLocalEndpoint(), "crfiles-archive", process.Handle());
  if (pipe.is_valid()) {
    // RunArchiveOnIO runs inside a posted task on an already running IO thread.
    // Its nested loop must dispatch Mojo replies and the cancellation timer.
    base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
    mojo::Remote<mojom::ArchiveDecoder> decoder(mojo::PendingRemote<mojom::ArchiveDecoder>(std::move(pipe), 0));
    ProgressListener listener(progress);
    mojo::Receiver<mojom::ArchiveProgressListener> receiver(&listener);
    decoder.set_disconnect_handler(loop.QuitClosure());
    base::RepeatingTimer timer;
    const auto deadline = base::TimeTicks::Now() + base::Minutes(30);
    // The timer and decoder are destroyed before loop and progress go away.
    timer.Start(FROM_HERE, base::Milliseconds(100), base::BindRepeating(
        [](ArchiveProgress& state, base::TimeTicks expires, base::RepeatingClosure quit) {
          if (state.canceled || base::TimeTicks::Now() >= expires) quit.Run();
        }, std::ref(progress), deadline, loop.QuitClosure()));
    std::move(request).Run(decoder.get(), receiver.BindNewPipeAndPassRemote(),
        base::BindOnce([](bool& result, ArchiveProgress& state, base::RepeatingClosure quit, bool ok, bool needs_password) {
          result = ok; state.needs_password = needs_password; quit.Run();
        }, std::ref(success), std::ref(progress), loop.QuitClosure()));
    loop.Run();
  }
  // Wait until the child has stopped writing before staging can be deleted.
  process.Terminate(base::Process::kResultCodeKilledBadMessage, true);
  base::EnsureProcessTerminated(std::move(process));
  return success && !progress.canceled;
}
bool DecodeOnIO(const base::FilePath& archive, const base::FilePath& staging,
    const std::string& password, ArchiveProgress& progress) {
  base::File input(archive, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!input.IsValid()) return false;
  return RunArchiveOnIO(progress, base::BindOnce(
      [](base::File file, std::string format, base::FilePath directory, std::string secret,
          mojom::ArchiveDecoder* decoder, mojo::PendingRemote<mojom::ArchiveProgressListener> listener,
          base::OnceCallback<void(bool, bool)> completed) {
        decoder->Extract(std::move(file), format, directory, secret, std::move(listener), std::move(completed));
      }, std::move(input), ArchiveFormat(archive), staging, password));
}
}
bool IsExtractableArchive(const base::FilePath& path) {
  return !ArchiveFormat(path).empty();
}
std::string ArchiveFormat(const base::FilePath& path) {
  const auto name = base::ToLowerASCII(path.BaseName().AsUTF8Unsafe());
  for (const std::string_view suffix : {".tar.gz", ".tar.xz", ".tgz", ".tar", ".zip", ".7z", ".rar"})
    if (name.ends_with(suffix)) return std::string(suffix);
  return {};
}
base::FilePath ArchiveFolderName(const base::FilePath& path) {
  const auto name = path.BaseName();
  const auto format = ArchiveFormat(path);
  return format.empty() ? name.RemoveExtension() :
      base::FilePath(name.value().substr(0, name.value().size() - format.size()));
}
bool DecodeArchive(const base::FilePath& archive, const base::FilePath& staging,
    const std::string& password, std::shared_ptr<ArchiveProgress> progress) {
  base::Thread thread("crFilesArchiveClient");
  if (!thread.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0))) return false;
  base::WaitableEvent done;
  bool success = false;
  // Own the inputs in the posted task. The result and event stay alive until
  // the task signals completion and the IO thread has stopped.
  if (!thread.task_runner()->PostTask(FROM_HERE, base::BindOnce(
      [](base::FilePath source, base::FilePath output, std::string secret,
          std::shared_ptr<ArchiveProgress> state, bool& result, base::WaitableEvent& completed) {
        result = DecodeOnIO(source, output, secret, *state);
        completed.Signal();
      }, archive, staging, password, std::move(progress), std::ref(success), std::ref(done)))) return false;
  done.Wait();
  thread.Stop();
  return success;
}
bool EncodeArchive(const base::FilePath& source_directory,
    std::vector<base::FilePath> relative_sources, const base::FilePath& output,
    std::shared_ptr<ArchiveProgress> progress) {
  base::Thread thread("crFilesCompressClient");
  if (!thread.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0))) return false;
  base::WaitableEvent done;
  bool success = false;
  if (!thread.task_runner()->PostTask(FROM_HERE, base::BindOnce(
      [](base::FilePath directory, std::vector<base::FilePath> sources, base::FilePath target,
          std::shared_ptr<ArchiveProgress> state, bool& result, base::WaitableEvent& completed) {
        result = RunArchiveOnIO(*state, base::BindOnce(
            [](base::FilePath root, std::vector<base::FilePath> paths, base::FilePath destination,
                mojom::ArchiveDecoder* decoder, mojo::PendingRemote<mojom::ArchiveProgressListener> listener,
                base::OnceCallback<void(bool, bool)> callback) {
              decoder->Compress(root, paths, destination, std::move(listener), base::BindOnce(
                  [](base::OnceCallback<void(bool, bool)> reply, bool ok) { std::move(reply).Run(ok, false); },
                  std::move(callback)));
            }, std::move(directory), std::move(sources), std::move(target)));
        completed.Signal();
      }, source_directory, std::move(relative_sources), output, std::move(progress), std::ref(success), std::ref(done)))) return false;
  done.Wait();
  thread.Stop();
  return success;
}
}
