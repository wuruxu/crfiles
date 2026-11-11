// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_pdf_reader.h"

#include <utility>
#include <vector>
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/crfiles_theme.h"
#include "crpdf/crpdf_launch_options.h"
#if BUILDFLAG(IS_LINUX)
#include "base/nix/xdg_util.h"
#endif

namespace crfiles {
namespace {
base::FilePath ReaderExecutable() {
  // The macOS App Store helper is the same binary with an inheritance-only
  // signature; other platforms relaunch the current executable directly.
  base::FilePath executable;
  if (!GetCrFilesHelperExecutable(&executable)) return {};
  return executable;
}

base::Process LaunchReader(base::CommandLine command, base::LaunchOptions options) {
  const auto executable = ReaderExecutable();
  FileOperationLog log("open_file", "menu", command.GetSwitchValuePath("pdf").AsUTF8Unsafe(), "crpdf");
  if (!log.valid() || executable.empty() || !base::PathExists(executable)) return base::Process();
  command.SetProgram(executable);
#if BUILDFLAG(IS_WIN)
  options.grant_foreground_privilege = true;
#endif
#if BUILDFLAG(IS_MAC)
  base::File bookmark_file;
  if (IsCrFilesSandboxed()) {
    // Editing may replace the PDF atomically, so transfer the folder grant.
    const auto bookmark = CreateFileAccessBookmark(command.GetSwitchValuePath("pdf").DirName());
    if (bookmark.empty() || bookmark.size() > crpdf::kMaxFileAccessBookmarkBytes) return base::Process();
    base::FilePath temporary_directory, temporary_path;
    if (!base::GetTempDir(&temporary_directory)) return base::Process();
    bookmark_file = base::CreateAndOpenTemporaryFileInDir(temporary_directory, &temporary_path);
    if (!bookmark_file.IsValid() || !base::DeleteFile(temporary_path) ||
        !bookmark_file.WriteAtCurrentPosAndCheck(bookmark) ||
        bookmark_file.Seek(base::File::FROM_BEGIN, 0) != 0) return base::Process();
    options.fds_to_remap.emplace_back(bookmark_file.GetPlatformFile(), crpdf::kFileAccessBookmarkFd);
    command.AppendSwitchASCII(crpdf::kFileAccessBookmarkFdSwitch,
        base::NumberToString(crpdf::kFileAccessBookmarkFd));
  }
#endif
  auto process = base::LaunchProcess(command, options);
  if (process.IsValid()) log.Succeed();
  return process;
}

// Poll without occupying a worker for the lifetime of a document. On POSIX,
// GetTerminationStatus also reaps exited children. Live readers are independent
// of crFiles shutdown; closing our process handles does not terminate them.
class ReaderProcesses {
 public:
  void Add(base::OnceClosure on_error, base::Process process) {
    if (!process.IsValid()) {
      LOG(ERROR) << "Unable to launch crFiles in PDF reader mode";
      if (on_error) std::move(on_error).Run();
      return;
    }
    readers_.push_back({std::move(process), std::move(on_error)});
    if (!timer_.IsRunning())
      timer_.Start(FROM_HERE, base::Seconds(1), this, &ReaderProcesses::Poll);
  }

 private:
  void Poll() {
    std::vector<base::OnceClosure> errors;
    for (auto it = readers_.begin(); it != readers_.end();) {
      int exit_code = 0;
      const auto status = base::GetTerminationStatus(it->process.Handle(), &exit_code);
      if (status == base::TERMINATION_STATUS_STILL_RUNNING) { ++it; continue; }
      if (status != base::TERMINATION_STATUS_NORMAL_TERMINATION) {
        LOG(ERROR) << "PDF reader exited abnormally: " << exit_code;
        if (it->on_error) errors.push_back(std::move(it->on_error));
      }
      it = readers_.erase(it);
    }
    if (readers_.empty()) timer_.Stop();
    for (auto& error : errors) std::move(error).Run();
  }
  struct Reader {
    base::Process process;
    base::OnceClosure on_error;
  };
  std::vector<Reader> readers_;
  base::RepeatingTimer timer_;
};

void StartReader(base::CommandLine command, base::OnceClosure on_error,
                 base::LaunchOptions options) {
  static base::NoDestructor<ReaderProcesses> readers;
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
      base::BindOnce(&LaunchReader, std::move(command), std::move(options)),
      base::BindOnce(&ReaderProcesses::Add, base::Unretained(readers.get()), std::move(on_error)));
}
}  // namespace

bool IsPdfReaderFile(const base::FilePath& path, std::string_view mime) {
  return mime == "application/pdf" || path.MatchesExtension(FILE_PATH_LITERAL(".pdf"));
}

void OpenCrFilesPdf(const base::FilePath& path, base::OnceClosure on_error) {
  base::CommandLine command(base::CommandLine::NO_PROGRAM);
  command.AppendSwitch(crpdf::kCrFilesReaderSwitch);
  command.AppendSwitchPath("pdf", path);
  command.AppendSwitchASCII("lang", base::i18n::GetConfiguredLocale());
  const auto theme = GetCrFilesThemeMode();
  command.AppendSwitchASCII(crpdf::kThemeSwitch, theme == CrFilesThemeMode::kDark ? "dark" :
      theme == CrFilesThemeMode::kLight ? "light" : "system");
  constexpr const char* kDisplaySwitches[] = {
      "ozone-platform", "use-gl", "use-angle", "force-device-scale-factor"};
  command.CopySwitchesFrom(*base::CommandLine::ForCurrentProcess(), kDisplaySwitches);
#if BUILDFLAG(IS_LINUX)
  base::nix::CreateLaunchOptionsWithXdgActivation(
      base::BindOnce(&StartReader, std::move(command), std::move(on_error)));
#else
  StartReader(std::move(command), std::move(on_error), base::LaunchOptions());
#endif
}
}  // namespace crfiles
