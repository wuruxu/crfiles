// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_pdf_reader.h"

#include <memory>
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
#include "crpdf/crpdf_reader.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
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

struct LaunchedReader {
  base::Process process{};
  mojo::PendingRemote<crpdf::mojom::ReaderController> controller;
};

crpdf::mojom::ThemeMode ReaderTheme(CrFilesThemeMode mode) {
  return mode == CrFilesThemeMode::kDark ? crpdf::mojom::ThemeMode::kDark :
      mode == CrFilesThemeMode::kLight ? crpdf::mojom::ThemeMode::kLight :
      crpdf::mojom::ThemeMode::kSystem;
}

LaunchedReader LaunchReader(base::CommandLine command, base::LaunchOptions options) {
  const auto executable = ReaderExecutable();
  FileOperationLog log("open_file", "menu", command.GetSwitchValuePath("pdf").AsUTF8Unsafe(), "crpdf");
  if (!log.valid() || executable.empty() || !base::PathExists(executable)) return {};
  command.SetProgram(executable);
#if BUILDFLAG(IS_WIN)
  options.grant_foreground_privilege = true;
#endif
#if BUILDFLAG(IS_MAC)
  base::File bookmark_file;
  if (IsCrFilesSandboxed()) {
    // Editing may replace the PDF atomically, so transfer the folder grant.
    const auto bookmark = CreateFileAccessBookmark(command.GetSwitchValuePath("pdf").DirName());
    if (bookmark.empty() || bookmark.size() > crpdf::kMaxFileAccessBookmarkBytes) return {};
    base::FilePath temporary_directory, temporary_path;
    if (!base::GetTempDir(&temporary_directory)) return {};
    bookmark_file = base::CreateAndOpenTemporaryFileInDir(temporary_directory, &temporary_path);
    if (!bookmark_file.IsValid() || !base::DeleteFile(temporary_path) ||
        !bookmark_file.WriteAtCurrentPosAndCheck(bookmark) ||
        bookmark_file.Seek(base::File::FROM_BEGIN, 0) != 0) return {};
    options.fds_to_remap.emplace_back(bookmark_file.GetPlatformFile(), crpdf::kFileAccessBookmarkFd);
    command.AppendSwitchASCII(crpdf::kFileAccessBookmarkFdSwitch,
        base::NumberToString(crpdf::kFileAccessBookmarkFd));
  }
#endif
  // Prepare after the bookmark remap so POSIX channel handles cannot reuse it.
  mojo::PlatformChannel channel;
  channel.PrepareToPassRemoteEndpoint(&options, &command);
  auto process = base::LaunchProcess(command, options);
  channel.RemoteProcessLaunchAttempted();
  if (!process.IsValid()) return {};
  log.Succeed();
  // An empty name keeps each reader's isolated connection independent.
  auto pipe = mojo::OutgoingInvitation::SendIsolated(
      channel.TakeLocalEndpoint(), {}, process.Handle());
  return {std::move(process),
      mojo::PendingRemote<crpdf::mojom::ReaderController>(std::move(pipe), 0)};
}

// Poll without occupying a worker for the lifetime of a document. On POSIX,
// GetTerminationStatus also reaps exited children. Live readers are independent
// of crFiles shutdown; closing our process handles does not terminate them.
class ReaderProcesses {
 public:
  void Add(base::OnceClosure on_error, LaunchedReader launched) {
    if (!launched.process.IsValid()) {
      LOG(ERROR) << "Unable to launch crFiles in PDF reader mode";
      if (on_error) std::move(on_error).Run();
      return;
    }
    auto reader = std::make_unique<Reader>();
    reader->process = std::move(launched.process);
    reader->on_error = std::move(on_error);
    if (launched.controller.is_valid()) {
      reader->controller.Bind(std::move(launched.controller));
      // Launching is asynchronous. Catch changes made since the command line
      // was captured, before registering the reader for subsequent updates.
      reader->controller->SetTheme(ReaderTheme(GetCrFilesThemeMode()));
      reader->controller.reset_on_disconnect();
    }
    readers_.push_back(std::move(reader));
    if (!timer_.IsRunning())
      timer_.Start(FROM_HERE, base::Seconds(1), this, &ReaderProcesses::Poll);
  }

  void SetTheme(CrFilesThemeMode mode) {
    for (auto& reader : readers_) {
      if (reader->controller.is_bound()) reader->controller->SetTheme(ReaderTheme(mode));
    }
  }

 private:
  void Poll() {
    std::vector<base::OnceClosure> errors;
    for (auto it = readers_.begin(); it != readers_.end();) {
      auto& reader = **it;
      int exit_code = 0;
      const auto status = base::GetTerminationStatus(reader.process.Handle(), &exit_code);
      if (status == base::TERMINATION_STATUS_STILL_RUNNING) { ++it; continue; }
      if (status != base::TERMINATION_STATUS_NORMAL_TERMINATION) {
        LOG(ERROR) << "PDF reader exited abnormally: " << exit_code;
        if (reader.on_error) errors.push_back(std::move(reader.on_error));
      }
      it = readers_.erase(it);
    }
    if (readers_.empty()) timer_.Stop();
    for (auto& error : errors) std::move(error).Run();
  }
  struct Reader {
    base::Process process;
    base::OnceClosure on_error;
    mojo::Remote<crpdf::mojom::ReaderController> controller;
  };
  std::vector<std::unique_ptr<Reader>> readers_;
  base::RepeatingTimer timer_;
};

ReaderProcesses& GetReaderProcesses() {
  static base::NoDestructor<ReaderProcesses> readers;
  return *readers;
}

void StartReader(base::CommandLine command, base::OnceClosure on_error,
                 base::LaunchOptions options) {
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
      base::BindOnce(&LaunchReader, std::move(command), std::move(options)),
      base::BindOnce(&ReaderProcesses::Add, base::Unretained(&GetReaderProcesses()), std::move(on_error)));
}
}  // namespace

bool IsPdfReaderFile(const base::FilePath& path, std::string_view mime) {
  return mime == "application/pdf" || path.MatchesExtension(FILE_PATH_LITERAL(".pdf"));
}

void UpdateCrFilesPdfReaderTheme(libcr::ThemeMode mode) {
  GetReaderProcesses().SetTheme(mode);
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
