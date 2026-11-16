// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_network.h"
#include "crFiles/crfiles_file_access.h"
#include <algorithm>
#include "base/command_line.h"
#include "base/process/kill.h"
#include "base/process/launch.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
namespace crfiles {
std::string SmbLocation::Url() const {
  return "smb://" + server + "/" + base::EscapePath(share) +
      (path.empty() ? std::string() : "/" + base::EscapePath(path));
}
std::optional<SmbLocation> ParseSmbLocation(std::string address) {
  base::TrimWhitespaceASCII(address, base::TRIM_ALL, &address);
  if (address.size() > 8192 || !base::StartsWith(address, "smb://", base::CompareCase::INSENSITIVE_ASCII)) return std::nullopt;
  address.erase(0, 6);
  const auto slash = address.find('/');
  SmbLocation result;
  result.server = base::ToLowerASCII(address.substr(0, slash));
  if (result.server.empty() || result.server.size() > 300 || std::ranges::any_of(result.server, [](unsigned char c) {
        return !base::IsAsciiAlphaNumeric(c) && c != '.' && c != '-' && c != '_' && c != ':' && c != '[' && c != ']' && c != '%';
      })) return std::nullopt;
  if (slash == std::string::npos) return result;
  for (const auto& encoded : base::SplitString(address.substr(slash + 1), "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
    if (encoded.empty()) continue;
    if (encoded.find_first_of("?#") != std::string::npos) return std::nullopt;
    const auto name = base::UnescapeBinaryURLComponent(encoded);
    if (name.empty() || name.size() > 1024 || !base::IsStringUTF8(name) || name == "." || name == ".." ||
        std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':'; })) return std::nullopt;
    if (result.share.empty()) result.share = name;
    else { if (!result.path.empty()) result.path += '/'; result.path += name; }
  }
  if (result.path.size() > 4096) return std::nullopt;
  return result;
}
SmbConnection::SmbConnection() = default;
SmbConnection::~SmbConnection() { Stop(); }
void SmbConnection::TakeOver(SmbConnection& other, base::OnceClosure disconnected) {
  Stop();
  process_ = std::move(other.process_);
  service_ = std::move(other.service_);
  service_.set_disconnect_handler(std::move(disconnected));
}
void SmbConnection::Stop() {
  service_.reset();
  if (process_.IsValid()) {
    // Cancel also interrupts a blocked name lookup or a stuck library call.
    process_.Terminate(0, false);
    base::EnsureProcessTerminated(std::move(process_));
  }
}
bool SmbConnection::Start(base::OnceClosure disconnected) {
  Stop();
  base::FilePath executable;
  if (!GetCrFilesHelperExecutable(&executable)) return false;
  base::CommandLine command(executable);
  command.AppendSwitch(kSmbProcessSwitch);
  base::LaunchOptions options;
  mojo::PlatformChannel channel;
  channel.PrepareToPassRemoteEndpoint(&options, &command);
  process_ = base::LaunchProcess(command, options);
  channel.RemoteProcessLaunchAttempted();
  if (!process_.IsValid()) return false;
  auto pipe = mojo::OutgoingInvitation::SendIsolated(channel.TakeLocalEndpoint(), "crfiles-smb", process_.Handle());
  if (!pipe.is_valid()) { Stop(); return false; }
  service_.Bind(mojo::PendingRemote<mojom::SmbService>(std::move(pipe), 0));
  service_.set_disconnect_handler(std::move(disconnected));
  return true;
}
}
