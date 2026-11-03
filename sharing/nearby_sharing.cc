// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_cache_state.h"
#include "crFiles/sharing/nearby_sharing.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/crfiles_tags.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/sharing/phone_sender.h"
#include "crFiles/sharing/connect_client.h"
#include "crFiles/crfiles_theme.h"
#include "base/base64.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPixmap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <limits>

#include "base/base_paths.h"
#include "base/compiler_specific.h"
#include "base/path_service.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crFiles/sharing/nearby_socket.h"
#if BUILDFLAG(IS_WIN)
#include <shlobj.h>
#include "base/base_paths_win.h"
#include "base/files/file_enumerator.h"
#include "crFiles/sharing/nearby_files_win.h"
#endif

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
#include "crypto/hash.h"
#include "crypto/keypair.h"
#include "net/cert/x509_util.h"
#include "net/dns/dns_response.h"
#include "third_party/boringssl/src/include/openssl/err.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "third_party/boringssl/src/include/openssl/x509.h"
#endif

namespace crfiles {
struct NearbySharing::TrustStore {
  mutable base::Lock lock;
  base::Lock io_lock;
  std::vector<TrustedDevice> devices;
  std::set<std::string> revoked;
  bool error = false;
};

struct NearbySharing::State {
  struct Outgoing {
    std::string address;
    std::vector<base::FilePath> paths;
    uint64_t generation;
    std::map<std::string, std::string> previews;
    std::optional<ConnectPeer> phone;
    std::string target_id;
    std::string sync_id = {};
    std::string sync_fingerprint = {};
    bool sync_remove = false;
  };
  mutable base::Lock lock;
  Snapshot snapshot;
  std::string name;
  std::string id;
  int avatar = 0;
  std::string custom_avatar;
  uint16_t identity_port = 0;
  struct IdentityResult {
    std::string fingerprint;
    base::TimeTicks expires;
    base::TimeTicks retry_after;
    bool folder_transfer = false;
    std::vector<SharedTarget> targets = {};
    bool sync_v1 = false;
    bool sync_removal = false;
    bool sync_wake = false;
  };
  std::map<std::string, IdentityResult> identities;
  std::shared_ptr<NearbySharing::TrustStore> trust;
  std::vector<SharedTarget> targets;
  std::vector<SharedTarget> peer_targets;
  bool peer_folder_transfer = false;
  std::shared_ptr<SyncService> sync;
  std::string sync_id;
  std::string sync_fingerprint;
  bool sync_remove = false;
  base::FilePath receive_root;
  bool peer_sync = false;
  std::string outgoing_target;
  std::string peer_fingerprint;
  bool trust_requested = false;
  uint64_t session_generation = 0;
  std::optional<Outgoing> outgoing;
  std::map<std::string, std::string> outgoing_previews;
  int decision = 0;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> cancel{0};
};

namespace {
using Stage = NearbySharing::Stage;
using State = NearbySharing::State;
using nearby::Accept;
using nearby::CreateSocket;
using nearby::kInvalidSocket;
using nearby::kSendFlags;
using nearby::LocalAddresses;
using nearby::PendingError;
using nearby::Poll;
using nearby::PollFd;
using nearby::RetrySocket;
using nearby::ScopedSocket;
using nearby::SetOption;
using nearby::Socket;
using nearby::SocketError;
using nearby::SocketLength;
#if BUILDFLAG(IS_WIN)
using nearby::CreatePrivateDirectory;
using nearby::DirectoryLock;
using nearby::OpenRegularFile;
using nearby::PinnedDirectory;
using nearby::ProtectPrivateDirectory;
#endif
constexpr size_t kMaximumFiles = 128;
std::atomic<uint64_t> g_session{0};
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
constexpr size_t kMaximumTreeEntries = 4096;
constexpr uint64_t kMaximumFileSize = 16ULL * 1024 * 1024 * 1024;
constexpr uint64_t kMaximumBatchSize = 64ULL * 1024 * 1024 * 1024;
constexpr size_t kChunkSize = 256 * 1024;
// Up to five fixed 96x96 RGBA previews (base64), plus the bounded manifest.
constexpr size_t kMaximumJson = 384 * 1024;
constexpr std::array<uint8_t, 11> kAlpn = {
    10, 'c', 'r', 'p', 'h', 'o', 't', 'o', 's', '/', '1'};
constexpr std::array<uint8_t, 14> kIdentityAlpn = {
    13, 'c', 'r', 'p', 'h', 'o', 't', 'o', 's', '-', 'i', 'd', '/', '1'};

std::string IdentityKey(const NearbySharing::Peer& peer) {
  return peer.id + "|" + peer.address + "|" +
      base::NumberToString(peer.identity_port) + "|" + peer.name + "|" +
      base::NumberToString(peer.avatar);
}

// Worker-local diagnostics: no logging or shared-state synchronization until
// a session has failed. Reasons are fixed strings, never peer-supplied data.
struct FailureInfo {
  const char* phase = "file_validation";
  const char* reason = "open_or_validate_failed";
  int os_error = 0;
  int ssl_error = 0;
  int message_id = 0;
  bool notify_peer = false;
  bool peer_reported = false;
};

struct TransferError {
  const char* code;
  int message_id;
};
constexpr TransferError kSourceReadError{"source_read", IDS_CRFILES_TRANSFER_SOURCE_READ_ERROR};
constexpr TransferError kConnectionError{"connection", IDS_CRFILES_TRANSFER_CONNECTION_ERROR};
constexpr TransferError kProtocolError{"protocol", IDS_CRFILES_TRANSFER_PROTOCOL_ERROR};
constexpr TransferError kChecksumError{"checksum", IDS_CRFILES_TRANSFER_CHECKSUM_ERROR};
constexpr TransferError kWriteError{"write", IDS_CRFILES_TRANSFER_WRITE_ERROR};
constexpr TransferError kFlushError{"flush", IDS_CRFILES_TRANSFER_FLUSH_ERROR};
constexpr TransferError kCommitError{"commit", IDS_CRFILES_TRANSFER_COMMIT_ERROR};
constexpr TransferError kExistsError{"destination_exists", IDS_CRFILES_TRANSFER_EXISTS_ERROR};
constexpr TransferError kFolderError{"receive_folder", IDS_CRFILES_TRANSFER_FOLDER_ERROR};
constexpr TransferError kSpaceError{"no_space", IDS_CRFILES_TRANSFER_SPACE_ERROR};
constexpr TransferError kHistoryError{"history", IDS_CRFILES_TRANSFER_HISTORY_ERROR};
constexpr TransferError kStatisticsError{"statistics", IDS_CRFILES_TRANSFER_STATISTICS_ERROR};
constexpr TransferError kConfirmationError{"confirmation", IDS_CRFILES_TRANSFER_CONFIRMATION_ERROR};
constexpr TransferError kApprovalError{"approval", IDS_CRFILES_TRANSFER_APPROVAL_ERROR};
constexpr TransferError kTransferErrors[] = {
    kSourceReadError, kConnectionError, kProtocolError, kChecksumError,
    kWriteError, kFlushError, kCommitError, kExistsError, kFolderError,
    kSpaceError, kHistoryError, kStatisticsError, kConfirmationError, kApprovalError};

bool FailTransfer(FailureInfo& failure, TransferError error) {
  failure.reason = error.code;
  failure.message_id = error.message_id;
  return false;
}
bool FailFileOperation(FailureInfo& failure, TransferError error) {
#if BUILDFLAG(IS_WIN)
  const auto os_error = GetLastError();
#else
  const int os_error = errno;
#endif
  failure.os_error = os_error;
  const auto file_error = os_error ? base::File::OSErrorToFileError(os_error) :
                                   base::File::FILE_ERROR_FAILED;
  if (file_error == base::File::FILE_ERROR_NO_SPACE) error = kSpaceError;
  else if (file_error == base::File::FILE_ERROR_EXISTS &&
           error.message_id == kCommitError.message_id) error = kExistsError;
  return FailTransfer(failure, error);
}

void SetStage(const std::shared_ptr<State>& state, Stage stage) {
  base::AutoLock lock(state->lock);
  state->snapshot.stage = state->stop ? Stage::kOff : stage;
  state->snapshot.status_message_id = stage == Stage::kReceivingConflict ?
      IDS_CRFILES_TRANSFER_CONFLICT_PROMPT : 0;
  if (stage == Stage::kReceivingConflict || (stage == Stage::kReceivingOffer && state->snapshot.syncing))
    ++state->snapshot.conflict_id;
  state->decision = 0;
}

bool IsCanceled(const State& state, uint64_t generation) {
  return state.stop || state.cancel != generation;
}

bool WaitSocket(Socket fd, short events, const State& state,
                uint64_t generation, base::TimeTicks deadline) {
  while (!IsCanceled(state, generation) && base::TimeTicks::Now() < deadline) {
    PollFd pfd{fd, events, 0};
    const int result = Poll(&pfd, 1, 100);
    if (result > 0) {
      return (pfd.revents & events) != 0;
    }
    if (result < 0 && !RetrySocket()) {
      return false;
    }
  }
  return false;
}

// All sockets are nonblocking. A canceled operation is noticed within 100 ms
// during network waits; no network IO or TLS handshake runs on the UI thread.
class Channel {
 public:
  Channel(ScopedSocket fd, SSL_CTX* context, bool server,
          std::shared_ptr<State> state, uint64_t generation, bool identity = false)
      : fd_(std::move(fd)), ssl_(SSL_new(context)), state_(std::move(state)),
        generation_(generation), identity_(identity) {
    // The socket layer suppresses SIGPIPE with MSG_NOSIGNAL on Linux and
    // SO_NOSIGPIPE on macOS, without changing process-wide signal handlers.
    method_.reset(BIO_meth_new(0, "crPhotos socket"));
    if (!method_ || !ssl_) return;
    BIO_meth_set_write(method_.get(), [](BIO* bio, const char* data, int size) {
      BIO_clear_retry_flags(bio);
      const Socket fd = *static_cast<const Socket*>(BIO_get_data(bio));
      const int result = static_cast<int>(send(fd, data, size, kSendFlags));
      if (result < 0 && RetrySocket())
        BIO_set_retry_write(bio);
      return result;
    });
    BIO_meth_set_read(method_.get(), [](BIO* bio, char* data, int size) {
      BIO_clear_retry_flags(bio);
      const Socket fd = *static_cast<const Socket*>(BIO_get_data(bio));
      const int result = static_cast<int>(recv(fd, data, size, 0));
      if (result < 0 && RetrySocket())
        BIO_set_retry_read(bio);
      return result;
    });
    BIO_meth_set_ctrl(method_.get(), [](BIO*, int command, long, void*) -> long {
      return command == BIO_CTRL_FLUSH ? 1 : 0;
    });
    socket_number_ = fd_.get();
    BIO* bio = BIO_new(method_.get());
    if (bio) {
      BIO_set_data(bio, &socket_number_);
      BIO_set_init(bio, 1);
      SSL_set_bio(ssl_.get(), bio, bio);
      if (server) SSL_set_accept_state(ssl_.get());
      else {
        SSL_set_connect_state(ssl_.get());
        const auto protocols = identity_ ? base::span<const uint8_t>(kIdentityAlpn) :
                                           base::span<const uint8_t>(kAlpn);
        if (SSL_set_alpn_protos(ssl_.get(), protocols.data(), protocols.size()) != 0) return;
      }
      valid_ = true;
    }
  }
  bool Handshake() {
    if (!valid_) {
      return false;
    }
    const auto deadline = base::TimeTicks::Now() + base::Seconds(identity_ ? 2 : 10);
    while (!IsCanceled(*state_, generation_)) {
      ERR_clear_error();
      const int result = SSL_do_handshake(ssl_.get());
      if (result == 1) {
        const uint8_t* selected = nullptr;
        unsigned length = 0;
        SSL_get0_alpn_selected(ssl_.get(), &selected, &length);
        const auto protocol = identity_ ? base::span<const uint8_t>(kIdentityAlpn).subspan<1>() :
                                          base::span<const uint8_t>(kAlpn).subspan<1>();
        return length == protocol.size() &&
            CRYPTO_memcmp(selected, protocol.data(), protocol.size()) == 0;
      }
      if (!Wait(result, deadline)) return false;
    }
    return false;
  }
  int last_ssl_error() const { return last_ssl_error_; }
  bool Bytes(base::span<uint8_t> bytes, bool send,
             base::TimeDelta timeout = base::Seconds(30)) {
    if (identity_) timeout = std::min(timeout, base::Seconds(2));
    const auto deadline = base::TimeTicks::Now() + timeout;
    while (!bytes.empty() && !IsCanceled(*state_, generation_) &&
           base::TimeTicks::Now() < deadline) {
      const int length = static_cast<int>(std::min(bytes.size(), kChunkSize));
      ERR_clear_error();
      const int result = send ? SSL_write(ssl_.get(), bytes.data(), length)
                              : SSL_read(ssl_.get(), bytes.data(), length);
      if (result > 0) bytes = bytes.subspan(static_cast<size_t>(result));
      else if (!Wait(result, deadline)) return false;
    }
    return bytes.empty();
  }
  bool SendJson(base::DictValue message, bool log_offer = false,
                base::TimeDelta timeout = base::Seconds(30)) {
    std::string json;
    if (!base::JSONWriter::Write(message, &json)) {
      if (log_offer) LOG(ERROR) << "[crPhotos nearby offer] send: JSON serialization failed";
      return false;
    }
    if (log_offer) LOG(INFO) << "[crPhotos nearby offer] send: json_bytes="
                            << json.size() << " limit=" << kMaximumJson;
    if (json.size() > kMaximumJson) {
      if (log_offer) LOG(ERROR) << "[crPhotos nearby offer] send: message exceeds limit";
      return false;
    }
    std::array<uint8_t, 4> header;
    for (size_t i = 0; i < 4; ++i)
      header.at(i) = static_cast<uint8_t>(json.size() >> (24 - i * 8));
    if (!Bytes(header, true, timeout)) {
      if (log_offer) LOG(ERROR) << "[crPhotos nearby offer] send: header write failed; canceled="
                               << IsCanceled(*state_, generation_);
      return false;
    }
    if (!Bytes(base::as_writable_byte_span(json), true, timeout)) {
      if (log_offer) LOG(ERROR) << "[crPhotos nearby offer] send: body write failed; canceled="
                               << IsCanceled(*state_, generation_);
      return false;
    }
    if (log_offer) LOG(INFO) << "[crPhotos nearby offer] send: write completed";
    return true;
  }
  std::optional<base::DictValue> ReadJson(
      base::TimeDelta timeout = base::Seconds(30), bool log_offer = false) {
    std::array<uint8_t, 4> header;
    if (!Bytes(header, false, timeout)) {
      if (log_offer) LOG(ERROR) << "[crPhotos nearby offer] receive: header read failed; canceled="
                               << IsCanceled(*state_, generation_);
      return std::nullopt;
    }
    size_t length = 0;
    for (uint8_t byte : header) length = (length << 8) | byte;
    if (log_offer) LOG(INFO) << "[crPhotos nearby offer] receive: json_bytes="
                            << length << " limit=" << kMaximumJson;
    if (!length || length > (identity_ ? 32768 : kMaximumJson)) {
      if (log_offer) LOG(ERROR) << "[crPhotos nearby offer] receive: invalid message length";
      return std::nullopt;
    }
    std::string json(length, '\0');
    if (!Bytes(base::as_writable_byte_span(json), false)) {
      if (log_offer) LOG(ERROR) << "[crPhotos nearby offer] receive: body read failed; canceled="
                               << IsCanceled(*state_, generation_);
      return std::nullopt;
    }
    auto parsed = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    if (log_offer) {
      if (parsed) LOG(INFO) << "[crPhotos nearby offer] receive: JSON parsed";
      else LOG(ERROR) << "[crPhotos nearby offer] receive: invalid JSON dictionary";
    }
    return parsed;
  }
  std::string PairingCode() {
    std::array<uint8_t, 6> bytes;
    constexpr char label[] = "EXPORTER-crPhotos-pairing-v1";
    if (SSL_export_keying_material(ssl_.get(), bytes.data(), bytes.size(),
                                  label, sizeof(label) - 1, nullptr, 0, 0) != 1)
      return {};
    const std::string hex = base::HexEncode(bytes);
    return hex.substr(0, 4) + " " + hex.substr(4, 4) + " " + hex.substr(8, 4);
  }
  std::string PeerFingerprint() const {
    bssl::UniquePtr<X509> certificate(SSL_get_peer_certificate(ssl_.get()));
    bssl::UniquePtr<EVP_PKEY> key(
        certificate ? X509_get_pubkey(certificate.get()) : nullptr);
    bssl::ScopedCBB encoded;
    if (!key || !CBB_init(encoded.get(), 128) ||
        !EVP_marshal_public_key(encoded.get(), key.get())) return {};
    // SAFETY: CBB owns a contiguous buffer of exactly CBB_len bytes and remains
    // alive for the lifetime of this span.
    const auto bytes = UNSAFE_BUFFERS(
        base::span(CBB_data(encoded.get()), CBB_len(encoded.get())));
    auto public_key = crypto::keypair::PublicKey::FromSubjectPublicKeyInfo(bytes);
    if (!public_key || !public_key->IsEcP256()) return {};
    return base::HexEncode(crypto::hash::Sha256(bytes));
  }
  bool Disconnected() const {
#if BUILDFLAG(IS_LINUX)
    PollFd pfd{fd_.get(), POLLIN | POLLRDHUP, 0};
    return Poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLRDHUP));
#else
    PollFd pfd{fd_.get(), POLLIN, 0};
    if (Poll(&pfd, 1, 0) <= 0) return false;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return true;
    char byte = 0;
    return (pfd.revents & POLLIN) && recv(fd_.get(), &byte, 1, MSG_PEEK) == 0;
#endif
  }
 private:
  bool Wait(int result, base::TimeTicks deadline) {
    const int error = SSL_get_error(ssl_.get(), result);
    last_ssl_error_ = error;
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
      return false;
    }
    return WaitSocket(fd_.get(), error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT,
                      *state_, generation_, deadline);
  }
  ScopedSocket fd_;
  Socket socket_number_ = kInvalidSocket;
  bssl::UniquePtr<BIO_METHOD> method_;
  bssl::UniquePtr<SSL> ssl_;
  std::shared_ptr<State> state_;
  uint64_t generation_;
  bool identity_ = false;
  bool valid_ = false;
  int last_ssl_error_ = 0;
};

base::DictValue Message(std::string type) {
  base::DictValue result;
  result.Set("type", std::move(type));
  return result;
}
bool IsMessage(const std::optional<base::DictValue>& message,
               std::string_view type) {
  const auto* value = message ? message->FindString("type") : nullptr;
  return value && *value == type;
}
bool CheckTransferMessage(const std::optional<base::DictValue>& message,
                          std::string_view type, FailureInfo& failure,
                          TransferError missing) {
  if (IsMessage(message, type)) return true;
  if (IsMessage(message, "error")) {
    failure.peer_reported = true;
    const auto* reason = message->FindString("reason");
    // Only fixed protocol codes are translated; never display peer text.
    for (const auto& error : kTransferErrors) {
      if (reason && *reason == error.code) {
        return FailTransfer(failure, error);
      }
    }
  }
  return FailTransfer(failure, message ? kProtocolError : missing);
}
bool HasUnsafeDisplayCharacters(std::string_view text) {
  return std::ranges::any_of(base::UTF8ToUTF16(text), [](char16_t c) {
    return c < 32 || (c >= 0x7f && c <= 0x9f) || c == 0x061c ||
           c == 0x200e || c == 0x200f || (c >= 0x2028 && c <= 0x202e) ||
           (c >= 0x2066 && c <= 0x2069);
  });
}
bool SafeComponent(std::string_view name) {
  if (name.empty() || name.size() > 240 || !base::IsStringUTF8(name) ||
      HasUnsafeDisplayCharacters(name) || name == ".incoming.part" ||
      name.back() == '.' || name.back() == ' ')
    return false;
  for (unsigned char c : name) {
    if (c < 32 || c == 127 || std::string_view("/\\:*?\"<>|").find(c) !=
                                 std::string_view::npos)
      return false;
  }
  std::string stem = base::ToUpperASCII(name.substr(0, name.find('.')));
  while (!stem.empty() && stem.back() == ' ') stem.pop_back();
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
      stem == "CONIN$" || stem == "CONOUT$" || stem == "CLOCK$" ||
      (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
       stem.back() >= '1' && stem.back() <= '9')) return false;
  if ((stem.starts_with("COM") || stem.starts_with("LPT")) &&
      (stem.substr(3) == "\xc2\xb9" || stem.substr(3) == "\xc2\xb2" ||
       stem.substr(3) == "\xc2\xb3")) return false;
  return true;
}
bool SafeName(std::string_view name) {
  if (!SafeComponent(name) || name.front() == '.') return false;
  const auto dot = name.rfind('.');
  if (dot == std::string_view::npos) return false;
  const std::string extension = base::ToLowerASCII(name.substr(dot));
  constexpr std::array<std::string_view, 18> extensions = {
      ".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif", ".tif", ".tiff",
      ".heic", ".heif", ".mp4", ".m4v", ".webm", ".mkv", ".mov", ".avi",
      ".ogv", ".mpeg"};
  return std::ranges::find(extensions, extension) != extensions.end();
}
bool SafeDeviceName(std::string_view name) {
  return !name.empty() && name.size() <= 64 && base::IsStringUTF8(name) &&
      !HasUnsafeDisplayCharacters(name);
}

// Called only on blocking workers. A private directory and an interprocess
// lock protect the durable identity and trust records across app instances.
base::FilePath TrustDirectory() {
#if BUILDFLAG(IS_MAC)
  if (IsCrFilesSandboxed()) return MacApplicationSupportDirectory().AppendASCII("nearby");
#endif
  base::FilePath home;
#if BUILDFLAG(IS_WIN)
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &home)) return {};
  return home.AppendASCII("crFiles").AppendASCII("nearby");
#else
  if (!base::PathService::Get(base::DIR_HOME, &home)) return {};
  return home.AppendASCII(".config").AppendASCII("crFiles").AppendASCII("nearby");
#endif
}
#if BUILDFLAG(IS_WIN)
DirectoryLock LockTrustDirectory(const base::FilePath& directory) {
  if (directory.empty() || !base::CreateDirectory(directory.DirName())) return {};
  if (!base::DirectoryExists(directory) && !CreatePrivateDirectory(directory) &&
      GetLastError() != ERROR_ALREADY_EXISTS) return {};
  DirectoryLock lock;
  if (!lock.Open(directory)) return {};
  if (!ProtectPrivateDirectory(directory)) return {};
  const auto deadline = base::TimeTicks::Now() + base::Seconds(10);
  while (!lock.TryLock()) {
    if (GetLastError() != ERROR_LOCK_VIOLATION || base::TimeTicks::Now() >= deadline) return {};
    Poll(nullptr, 0, 100);
  }
  return lock;
}
#else
base::ScopedFD LockTrustDirectory(const base::FilePath& directory) {
  if (directory.empty() || !base::CreateDirectory(directory.DirName())) return {};
  if (mkdir(directory.value().c_str(), 0700) != 0 && errno != EEXIST) return {};
  struct stat info;
  if (lstat(directory.value().c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != geteuid() || (info.st_mode & 0077)) return {};
  base::ScopedFD fd(open(directory.AppendASCII("lock").value().c_str(),
      O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (!fd.is_valid() || flock(fd.get(), LOCK_EX) != 0) return {};
  return fd;
}
#endif
bool ReadPrivateFile(const base::FilePath& path, std::string* bytes, size_t maximum) {
#if BUILDFLAG(IS_WIN)
  auto file = OpenRegularFile(path, false);
  if (!file.IsValid()) return false;
#else
  base::File file(open(path.value().c_str(),
                      O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  if (!file.IsValid()) return false;
  struct stat info;
  if (fstat(file.GetPlatformFile(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_uid != geteuid()) return false;
#endif
  const int64_t length = file.GetLength();
  if (length < 0 || static_cast<uint64_t>(length) > maximum) return false;
  bytes->resize(static_cast<size_t>(length));
  return file.ReadAtCurrentPosAndCheck(base::as_writable_byte_span(*bytes));
}
bool ReadTrustRecords(const base::FilePath& directory,
                      std::vector<NearbySharing::TrustedDevice>* devices) {
  const auto path = directory.AppendASCII("trusted.json");
  if (!base::PathExists(path)) return true;
  std::string json;
  if (!ReadPrivateFile(path, &json, 1024 * 1024)) return false;
  auto root = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  const auto* list = root ? root->FindList("devices") : nullptr;
  if (!list || list->size() > 128) return false;
  std::set<std::string> fingerprints;
  for (const auto& value : *list) {
    const auto* item = value.GetIfDict();
    const auto* fingerprint = item ? item->FindString("fingerprint") : nullptr;
    const auto* name = item ? item->FindString("name") : nullptr;
    const auto* since = item ? item->FindString("trusted_at") : nullptr;
    const auto* active = item ? item->FindString("last_active") : nullptr;
    const auto* sent = item ? item->FindString("sent") : nullptr;
    const auto* received = item ? item->FindString("received") : nullptr;
    int64_t since_us = 0, active_us = 0;
    NearbySharing::TrustedDevice device;
    if (!fingerprint || fingerprint->size() != 64 ||
        !std::ranges::all_of(*fingerprint, [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        }) || !fingerprints.insert(*fingerprint).second ||
        !name || !SafeDeviceName(*name) || !since || !active || !sent || !received ||
        !base::StringToInt64(*since, &since_us) || since_us <= 0 ||
        !base::StringToInt64(*active, &active_us) || active_us <= 0 ||
        !base::StringToUint64(*sent, &device.sent_bytes) ||
        !base::StringToUint64(*received, &device.received_bytes)) return false;
    device.fingerprint = *fingerprint;
    device.name = *name;
    const int avatar = item->FindInt("avatar").value_or(0);
    device.avatar = avatar >= 0 && avatar < NearbySharing::kAvatarCount ? avatar : 0;
    device.trusted_at = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(since_us));
    device.last_active = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(active_us));
    devices->push_back(std::move(device));
  }
  return true;
}
bool WriteTrustRecords(const base::FilePath& directory,
                       const std::vector<NearbySharing::TrustedDevice>& devices) {
  base::ListValue list;
  for (const auto& device : devices) {
    base::DictValue entry;
    entry.Set("fingerprint", device.fingerprint);
    entry.Set("name", device.name);
    entry.Set("avatar", device.avatar);
    entry.Set("trusted_at", base::NumberToString(
        device.trusted_at.ToDeltaSinceWindowsEpoch().InMicroseconds()));
    entry.Set("last_active", base::NumberToString(
        device.last_active.ToDeltaSinceWindowsEpoch().InMicroseconds()));
    entry.Set("sent", base::NumberToString(device.sent_bytes));
    entry.Set("received", base::NumberToString(device.received_bytes));
    list.Append(std::move(entry));
  }
  base::DictValue root;
  root.Set("devices", std::move(list));
  std::string json;
  return base::JSONWriter::Write(root, &json) &&
      base::ImportantFileWriter::WriteFileAtomically(
          directory.AppendASCII("trusted.json"), json);
}
void PublishTrust(const std::shared_ptr<NearbySharing::TrustStore>& store,
                  std::vector<NearbySharing::TrustedDevice> devices) {
  base::AutoLock lock(store->lock);
  store->devices = std::move(devices);
}
void LoadTrust(const std::shared_ptr<NearbySharing::TrustStore>& store) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  base::AutoLock io_lock(store->io_lock);
  const auto directory = TrustDirectory();
  auto fd = LockTrustDirectory(directory);
  std::vector<NearbySharing::TrustedDevice> devices;
  if (fd.is_valid() && ReadTrustRecords(directory, &devices))
    PublishTrust(store, std::move(devices));
}
std::optional<crypto::keypair::PrivateKey> LoadIdentity(
    const std::shared_ptr<NearbySharing::TrustStore>& store) {
  base::AutoLock io_lock(store->io_lock);
  const auto directory = TrustDirectory();
  auto fd = LockTrustDirectory(directory);
  if (!fd.is_valid()) return std::nullopt;
  const auto path = directory.AppendASCII("identity.pk8");
  if (base::PathExists(path)) {
    std::string bytes;
    if (!ReadPrivateFile(path, &bytes, 4096)) return std::nullopt;
    auto key = crypto::keypair::PrivateKey::FromPrivateKeyInfo(base::as_byte_span(bytes));
    if (!key || !key->IsEcP256()) return std::nullopt;
    return key;
  }
  auto key = crypto::keypair::PrivateKey::GenerateEcP256();
  const auto bytes = key.ToPrivateKeyInfo();
  if (!base::ImportantFileWriter::WriteFileAtomically(path, base::as_string_view(bytes)))
    return std::nullopt;
  return key;
}
bool IsTrusted(const std::shared_ptr<NearbySharing::TrustStore>& store,
               const std::string& fingerprint) {
  // Reload under the disk lock so revocation in another instance takes effect.
  base::AutoLock io_lock(store->io_lock);
  const auto directory = TrustDirectory();
  auto fd = LockTrustDirectory(directory);
  std::vector<NearbySharing::TrustedDevice> devices;
  if (!fd.is_valid() || !ReadTrustRecords(directory, &devices)) return false;
  const bool found = std::ranges::any_of(devices, [&](const auto& device) {
    return device.fingerprint == fingerprint;
  });
  PublishTrust(store, std::move(devices));
  base::AutoLock lock(store->lock);
  return found && !store->revoked.contains(fingerprint);
}
bool UpdateTrust(const std::shared_ptr<State>& state, bool remember,
                 uint64_t sent = 0, uint64_t received = 0) {
  std::string fingerprint, name;
  int avatar;
  {
    base::AutoLock lock(state->lock);
    if (state->stop) return false;
    fingerprint = state->peer_fingerprint;
    name = state->snapshot.peer_name;
    avatar = state->snapshot.peer_avatar;
  }
  base::AutoLock io_lock(state->trust->io_lock);
  if (remember && IsCanceled(*state, state->session_generation)) return false;
  const auto directory = TrustDirectory();
  auto fd = LockTrustDirectory(directory);
  std::vector<NearbySharing::TrustedDevice> devices;
  if (!fd.is_valid() || !ReadTrustRecords(directory, &devices)) return false;
  auto it = std::ranges::find(devices, fingerprint, &NearbySharing::TrustedDevice::fingerprint);
  if (it == devices.end()) {
    if (!remember) return true;
    if (devices.size() >= 128) return false;
    devices.push_back({fingerprint, name, avatar, base::Time::Now(), base::Time::Now()});
    it = std::prev(devices.end());
  }
  it->name = name;
  it->avatar = avatar;
  it->last_active = base::Time::Now();
  it->sent_bytes += std::min(sent, std::numeric_limits<uint64_t>::max() - it->sent_bytes);
  it->received_bytes += std::min(received, std::numeric_limits<uint64_t>::max() - it->received_bytes);
  if (!WriteTrustRecords(directory, devices)) return false;
  PublishTrust(state->trust, std::move(devices));
  if (remember) {
    base::AutoLock lock(state->trust->lock);
    if (IsCanceled(*state, state->session_generation)) return false;
    state->trust->revoked.erase(fingerprint);
  }
  return true;
}

bool AwaitDecision(Channel& channel, const std::shared_ptr<State>& state, uint64_t generation,
                   Stage stage, bool sync_heartbeat = false) {
  SetStage(state, stage);
  const auto deadline = base::TimeTicks::Now() + base::Seconds(120);
  auto heartbeat = base::TimeTicks::Now();
  while (!IsCanceled(*state, generation) && base::TimeTicks::Now() < deadline) {
    if (channel.Disconnected()) return false;
    {
      base::AutoLock lock(state->lock);
      if (state->decision) return state->decision == 1;
    }
    if (sync_heartbeat && base::TimeTicks::Now() - heartbeat >= base::Seconds(5)) {
      if (!channel.SendJson(Message("sync-heartbeat"))) return false;
      heartbeat = base::TimeTicks::Now();
    }
    // No UI thread is blocked while the user compares or accepts the offer.
    Poll(nullptr, 0, 100);
  }
  return false;
}

bool ConfirmReceivedReplacement(Channel& channel, const std::shared_ptr<State>& state,
                                uint64_t generation, FailureInfo& failure) {
  // The new file is verified and flushed before asking to replace the old one.
  if (!AwaitDecision(channel, state, generation, Stage::kReceivingConflict))
    return FailTransfer(failure, kApprovalError);
  SetStage(state, Stage::kTransferring);
  return !IsCanceled(*state, generation);
}

base::ListValue SharedTargetsJson(const std::shared_ptr<State>& state) {
  base::ListValue list;
  base::AutoLock lock(state->lock);
  for (const auto& target : state->targets) {
    base::DictValue entry; entry.Set("id", target.id); entry.Set("name", target.name);
    list.Append(std::move(entry));
  }
  return list;
}
std::vector<NearbySharing::SharedTarget> ParseTargets(const base::DictValue& message) {
  std::vector<NearbySharing::SharedTarget> targets;
  const auto* list = message.FindList("targets");
  if (!list || list->size() > 128) return targets;
  std::set<std::string> ids;
  for (const auto& value : *list) {
    const auto* entry = value.GetIfDict();
    const auto* id = entry ? entry->FindString("id") : nullptr;
    const auto* name = entry ? entry->FindString("name") : nullptr;
    if (!id || !SafeComponent(*id) || !name || !SafeDeviceName(*name) || !ids.insert(*id).second) return {};
    targets.push_back({*id, *name, {}});
  }
  return targets;
}
bool Pair(Channel& channel, const std::shared_ptr<State>& state,
          uint64_t generation, FailureInfo& failure) {
  failure.reason = "hello_send_failed";
  auto hello = Message("hello");
  hello.Set("version", 1);
  hello.Set("folder_transfer", true);
  hello.Set("sync_v1", true);
  hello.Set("sync_permissions", 2);
  hello.Set("sync_source_updates", 1);
  hello.Set("sync_removal", 1);
  if (!state->sync_id.empty()) hello.Set("sync", state->sync_id);
  if (state->sync_remove) hello.Set("sync_remove", true);
  hello.Set("targets", SharedTargetsJson(state));
  hello.Set("name", state->name);
  hello.Set("avatar", state->avatar);
  {
    base::AutoLock lock(state->lock);
    if (!state->snapshot.receiving && !state->outgoing_target.empty()) hello.Set("target", state->outgoing_target);
  }
  if (!state->custom_avatar.empty()) hello.Set("avatar_rgba96", state->custom_avatar);
  if (!channel.SendJson(std::move(hello))) {
    return false;
  }
  auto peer = channel.ReadJson();
  failure.reason = "hello_read_or_validation_failed";
  const std::string* name = peer ? peer->FindString("name") : nullptr;
  if (!IsMessage(peer, "hello") || peer->FindInt("version") != 1 ||
      !name || !SafeDeviceName(*name)) {
    return false;
  }
  const std::string code = channel.PairingCode();
  failure.reason = "code_or_fingerprint_failed";
  const std::string fingerprint = channel.PeerFingerprint();
  if (code.empty() || fingerprint.empty()) {
    return false;
  }
  const int avatar = peer->FindInt("avatar").value_or(0);
  std::string custom_avatar;
  if (const auto* encoded = peer->FindString("avatar_rgba96")) {
    std::string pixels;
    if (encoded->size() == 49152 && base::Base64Decode(*encoded, &pixels) &&
        pixels.size() == 96 * 96 * 4) custom_avatar = *encoded;
  }
  const bool trusted = IsTrusted(state->trust, fingerprint);
  state->peer_sync = peer->FindBool("sync_v1").value_or(false) && peer->FindInt("sync_permissions") == 2 &&
      peer->FindInt("sync_source_updates") == 1;
  if (state->snapshot.receiving) {
    if (const auto* sync_id = peer->FindString("sync")) {
      if (!state->peer_sync || !IsSyncId(*sync_id)) return false;
      state->sync_id = *sync_id;
    }
  } else if (!state->sync_id.empty() && (!state->peer_sync || fingerprint != state->sync_fingerprint)) return false;
  const bool peer_removal = peer->FindInt("sync_removal") == 1;
  if (state->snapshot.receiving) {
    const auto remove = peer->FindBool("sync_remove");
    if (peer->contains("sync_remove") && (!remove || state->sync_id.empty() || !peer_removal)) return false;
    state->sync_remove = remove.value_or(false);
  }
  if (state->sync_remove) {
    failure.reason = "sync_removal_not_authorized";
    if (!peer_removal || !state->sync->CanRemove(state->sync_id, fingerprint, !state->snapshot.receiving)) return false;
  } else if (!state->sync_id.empty() && !state->sync->CanConnect(state->sync_id, fingerprint)) {
    if (state->snapshot.receiving && state->sync->Revoked(state->sync_id, fingerprint))
      channel.SendJson(Message("sync-revoked"));
    return false;
  }
  // This exception is scoped to the removal-only worker branch. It cannot
  // authorize file transfer, initial consent, or adding a trusted device.
  const bool sync_authorized = state->sync_remove ||
      (!state->sync_id.empty() && state->sync->Authorized(state->sync_id, fingerprint));
  {
    base::AutoLock lock(state->lock);
    if (state->snapshot.receiving) {
      if (const auto* target_id = peer->FindString("target")) {
        const auto target = std::ranges::find(state->targets, *target_id, &NearbySharing::SharedTarget::id);
        if (target == state->targets.end()) return false;
        state->snapshot.receive_target = target->id;
        state->snapshot.receive_target_name = target->name;
      }
    }
    state->snapshot.syncing = !state->sync_id.empty();
    state->snapshot.sync_id = state->sync_id;
    state->snapshot.sync_background = sync_authorized;
    state->snapshot.peer_trusted = trusted;
    state->snapshot.peer_name = *name;
    state->snapshot.peer_custom_avatar = std::move(custom_avatar);
    state->snapshot.peer_avatar =
        avatar >= 0 && avatar < NearbySharing::kAvatarCount ? avatar : 0;
    state->peer_folder_transfer = peer->FindBool("folder_transfer").value_or(false);
    state->peer_targets = ParseTargets(*peer);
    state->peer_fingerprint = fingerprint;
    state->snapshot.code = code;
  }
  failure.reason = "local_pairing_not_approved_or_disconnected";
  if (!trusted && !sync_authorized &&
      !AwaitDecision(channel, state, generation, Stage::kPairing)) {
    if (!state->sync_id.empty() && !state->sync_remove) {
      channel.SendJson(Message("sync-declined"), false, base::Seconds(2));
      state->sync->Pause(state->sync_id, true);
      state->sync->Poll();
    }
    return false;
  }
  failure.reason = "paired_send_failed";
  if (!channel.SendJson(Message("paired"))) return false;
  SetStage(state, Stage::kWaiting);
  failure.reason = "peer_pairing_not_confirmed";
  const auto paired = channel.ReadJson(state->sync_remove ? base::Seconds(15) : base::Seconds(130));
  if (!state->sync_id.empty() && IsMessage(paired, "sync-revoked")) {
    state->sync->RemoteRemoved(state->sync_id, fingerprint);
    return false;
  }
  if (!state->sync_id.empty() && !state->sync_remove && IsMessage(paired, "sync-declined")) {
    state->sync->Pause(state->sync_id, true);
    state->sync->Poll();
    return false;
  }
  if (!IsMessage(paired, "paired") || IsCanceled(*state, generation)) return false;
  if (state->sync_remove) return true;
  bool remember;
  {
    base::AutoLock lock(state->lock);
    remember = state->trust_requested;
  }
  failure.reason = "trust_record_update_failed";
  if (!UpdateTrust(state, remember)) {
    return false;
  }
  {
    base::AutoLock lock(state->lock);
    state->snapshot.peer_trusted = trusted || remember;
  }
  return true;
}

struct SendFile {
  base::File file;
  base::FilePath source;
  std::string name;
  uint64_t size;
};
std::optional<std::vector<SendFile>> OpenSelectedFiles(
    const std::vector<base::FilePath>& paths) {
  if (paths.empty() || paths.size() > kMaximumFiles) return std::nullopt;
  std::vector<SendFile> files;
  std::set<std::string> names;
  uint64_t total = 0;
  std::vector<std::pair<base::FilePath, std::string>> pending;
  for (const auto& path : paths) pending.emplace_back(path, path.BaseName().AsUTF8Unsafe());
  for (size_t index = 0; index < pending.size(); ++index) {
    const auto path = pending[index].first;
    std::string name = pending[index].second;
    if (!SafeComponent(path.BaseName().AsUTF8Unsafe()) || name.size() > 4096 ||
        path.GetComponents().size() > 64 || base::IsLink(path) ||
        !names.insert(base::ToLowerASCII(name)).second) return std::nullopt;
    if (base::DirectoryExists(path)) {
#if BUILDFLAG(IS_WIN)
      PinnedDirectory pin;
      if (!pin.Open(path)) return std::nullopt;
#endif
      files.push_back({base::File(), path, name + "/", 0});
      int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
      types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
      base::FileEnumerator children(path, false, types,
          base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
          base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
      for (auto child = children.Next(); !child.empty(); child = children.Next()) {
        if (pending.size() >= kMaximumTreeEntries) return std::nullopt;
        pending.emplace_back(child, name + "/" + child.BaseName().AsUTF8Unsafe());
      }
      if (children.GetError() != base::File::FILE_OK) return std::nullopt;
      continue;
    }
#if BUILDFLAG(IS_WIN)
    auto file = OpenRegularFile(path, false);
    const int64_t size = file.IsValid() ? file.GetLength() : -1;
    if (size < 0 || static_cast<uint64_t>(size) > kMaximumFileSize) return std::nullopt;
    total += size;
    if (total > kMaximumBatchSize) return std::nullopt;
    files.push_back({std::move(file), path, std::move(name), static_cast<uint64_t>(size)});
#else
    base::ScopedFD fd(open(path.value().c_str(),
                          O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    struct stat info;
    if (!fd.is_valid() || fstat(fd.get(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uint64_t>(info.st_size) > kMaximumFileSize) {
      return std::nullopt;
    }
    total += info.st_size;
    if (total > kMaximumBatchSize) return std::nullopt;
    files.push_back({base::File(fd.release()), path, std::move(name),
                     static_cast<uint64_t>(info.st_size)});
#endif
  }
  return files;
}

bool SendFiles(Channel& channel, std::vector<SendFile> files,
               const std::shared_ptr<State>& state, uint64_t generation,
               FailureInfo& failure) {
  base::ListValue list;
  uint64_t total = 0;
  size_t preview_count = 0;
  for (const auto& file : files) {
    base::DictValue entry;
    entry.Set("name", file.name);
    entry.Set("size", base::NumberToString(file.size));
    if (const auto preview = state->outgoing_previews.find(file.name);
        preview != state->outgoing_previews.end()) {
      entry.Set("preview_rgba96", preview->second);
      ++preview_count;
    }
    total += file.size;
    list.Append(std::move(entry));
  }
  if (!state->outgoing_target.empty() &&
      std::ranges::find(state->peer_targets, state->outgoing_target, &NearbySharing::SharedTarget::id) == state->peer_targets.end()) return FailTransfer(failure, kFolderError);
  const bool tree = std::ranges::any_of(files, [](const auto& file) { return file.name.find('/') != std::string::npos || !SafeName(file.name); });
  if (tree && !state->peer_folder_transfer) return FailTransfer(failure, kProtocolError);
  auto offer = Message("offer");
  if (tree) offer.Set("tree", true);
  if (!state->outgoing_target.empty()) offer.Set("target", state->outgoing_target);
  offer.Set("files", std::move(list));
  LOG(INFO) << "[crPhotos nearby offer] send: files=" << files.size()
            << " previews=" << preview_count << " total_bytes=" << total;
  if (!channel.SendJson(std::move(offer), true)) return FailTransfer(failure, kConnectionError);
  LOG(INFO) << "[crPhotos nearby offer] send: waiting for acceptance";
  {
    base::AutoLock lock(state->lock);
    state->snapshot.total = total;
    state->snapshot.files.clear();
    for (const auto& file : files) state->snapshot.files.push_back(file.name);
  }
  SetStage(state, Stage::kWaiting);
  // Allow receive consent, contention with another receiver, and name scanning.
  const auto acceptance = channel.ReadJson(base::Seconds(250));
  if (!CheckTransferMessage(acceptance, "accept", failure, kConfirmationError)) return false;
  const auto* requested = acceptance ? acceptance->FindList("files") : nullptr;
  if (!IsMessage(acceptance, "accept") || !requested ||
      requested->size() > files.size()) {
    LOG(ERROR) << "[crPhotos nearby offer] send: acceptance missing or invalid; canceled="
               << IsCanceled(*state, generation);
    return FailTransfer(failure, kProtocolError);
  }
  LOG(INFO) << "[crPhotos nearby offer] send: acceptance received; requested_files="
            << requested->size();
  std::set<std::string> wanted;
  uint64_t remaining_total = 0;
  for (const auto& value : *requested) {
    const auto* name = value.GetIfString();
    if (!name || !wanted.insert(*name).second) return FailTransfer(failure, kProtocolError);
    const auto file = std::ranges::find(files, *name, &SendFile::name);
    if (file == files.end()) return FailTransfer(failure, kProtocolError);
    remaining_total += file->size;
  }
  {
    base::AutoLock lock(state->lock);
    state->snapshot.skipped_files += files.size() - wanted.size();
    state->snapshot.total = remaining_total;
  }
  SetStage(state, Stage::kTransferring);
  std::vector<uint8_t> buffer(kChunkSize);
  for (auto& file : files) {
    if (!wanted.contains(file.name)) continue;
    {
      base::AutoLock lock(state->lock);
      state->snapshot.current_file = file.name;
    }
    std::string recipient;
    { base::AutoLock lock(state->lock); recipient = state->snapshot.peer_name; }
    FileOperationLog log("send", "menu", file.source.AsUTF8Unsafe(), recipient + "/" + state->outgoing_target + "/" + file.name);
    if (!log.valid()) return FailTransfer(failure, kHistoryError);
    crypto::hash::Hasher hasher(crypto::hash::kSha256);
    uint64_t remaining = file.size;
    while (remaining && !IsCanceled(*state, generation)) {
      auto part = base::span(buffer).first(
          static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size())));
      if (!file.file.ReadAtCurrentPosAndCheck(part)) return FailTransfer(failure, kSourceReadError);
      hasher.Update(part);
      if (!channel.Bytes(part, true)) return FailTransfer(failure, kConnectionError);
      remaining -= part.size();
      base::AutoLock lock(state->lock);
      state->snapshot.transferred += part.size();
    }
    if (remaining || IsCanceled(*state, generation)) return false;
    std::array<uint8_t, 32> hash;
    hasher.Finish(hash);
    auto digest = Message("digest");
    digest.Set("sha256", base::HexEncode(hash));
    failure.notify_peer = true;  // The receiver is now waiting for a JSON digest.
    if (!channel.SendJson(std::move(digest))) return FailTransfer(failure, kConnectionError);
    // A receiver may need explicit consent before replacing a same-name file.
    if (!CheckTransferMessage(channel.ReadJson(base::Seconds(250)), "stored", failure,
                              kConfirmationError)) return false;
    failure.notify_peer = false;  // The next entry may be an unframed byte stream.
    log.Succeed();
    if (!UpdateTrust(state, false, file.size, 0)) return FailTransfer(failure, kStatisticsError);
  }
  return CheckTransferMessage(channel.ReadJson(), "complete", failure,
                              kConfirmationError);
}

// Walk only the receive root and its Received-* subtrees. Never follow
// symlinks, and fail rather than silently overlooking duplicates on IO errors
// or unreasonably large/deep directory trees.
#if BUILDFLAG(IS_WIN)
bool CollectReceivedNames(const base::FilePath& directory, bool root, size_t depth,
                          size_t& entries, std::set<std::string>& names,
                          const State& state, uint64_t generation,
                          base::TimeTicks deadline) {
  if (depth > 64 || IsCanceled(state, generation)) return false;
  PinnedDirectory pin;
  if (!pin.Open(directory)) return false;
  base::FileEnumerator enumerator(directory, false,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES,
      base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (auto path = enumerator.Next(); !path.empty(); path = enumerator.Next()) {
    if (++entries > 1000000 || IsCanceled(state, generation) ||
        base::TimeTicks::Now() >= deadline) return false;
    const DWORD attributes = GetFileAttributesW(path.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
    const std::string name = path.BaseName().AsUTF8Unsafe();
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) names.insert(base::ToLowerASCII(name));
    else if ((!root || name.starts_with("Received-")) &&
        !CollectReceivedNames(path, false, depth + 1, entries, names,
                              state, generation, deadline)) return false;
  }
  return enumerator.GetError() == base::File::FILE_OK;
}
#else
bool CollectReceivedNames(int directory, bool root, size_t depth,
                          size_t& entries, std::set<std::string>& names,
                          const State& state, uint64_t generation,
                          base::TimeTicks deadline) {
  if (depth > 64 || IsCanceled(state, generation)) return false;
  base::ScopedFD copy(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!copy.is_valid()) return false;
  std::unique_ptr<DIR, decltype(&closedir)> stream(fdopendir(copy.get()), closedir);
  if (!stream) return false;
  // fdopendir() owns the descriptor now; closedir() will close it.
  static_cast<void>(copy.release());
  while (true) {
    if (IsCanceled(state, generation) || base::TimeTicks::Now() >= deadline)
      return false;
    errno = 0;
    const auto* entry = readdir(stream.get());
    if (!entry) return errno == 0;
    const auto field = base::span(entry->d_name);
    const auto end = std::ranges::find(field, '\0');
    if (end == field.end()) return false;
    const std::string name(field.begin(), end);
    if (name == "." || name == "..") continue;
    if (++entries > 1000000) return false;
    struct stat info;
    if (fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) continue;
      return false;
    }
    if (S_ISREG(info.st_mode)) names.insert(base::ToLowerASCII(name));
    if (S_ISDIR(info.st_mode) && (!root || name.starts_with("Received-"))) {
      base::ScopedFD child(openat(directory, name.c_str(),
          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (!child.is_valid() ||
          !CollectReceivedNames(child.get(), false, depth + 1, entries,
                                names, state, generation, deadline)) return false;
    }
  }
}

#endif

bool SafeTransferName(std::string_view name, bool tree) {
  if (!tree) return SafeName(name);
  if (name.empty() || name.size() > 4096 || name.front() == '/') return false;
  if (name.back() == '/') name.remove_suffix(1);
  const auto parts = base::SplitStringPiece(name, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  return !parts.empty() && parts.size() <= 64 && std::ranges::all_of(parts, [](auto part) { return SafeComponent(part); });
}
bool ReceiveFiles(Channel& channel, const std::shared_ptr<State>& state,
                  uint64_t generation, FailureInfo& failure) {
  LOG(INFO) << "[crPhotos nearby offer] receive: waiting for manifest";
  auto offer = channel.ReadJson(base::Seconds(30), true);
  if (!CheckTransferMessage(offer, "offer", failure, kConnectionError)) return false;
  const auto* list = offer ? offer->FindList("files") : nullptr;
  const bool tree = offer && offer->FindBool("tree").value_or(false);
  if (tree && !state->peer_folder_transfer) return FailTransfer(failure, kProtocolError);
  if (!IsMessage(offer, "offer") || !list || list->empty() ||
      list->size() > (tree ? kMaximumTreeEntries : kMaximumFiles)) {
    LOG(ERROR) << "[crPhotos nearby offer] receive: invalid offer or file list; files="
               << (list ? list->size() : 0);
    return FailTransfer(failure, kProtocolError);
  }
  std::vector<std::pair<std::string, uint64_t>> files;
  std::set<std::string> names;
  uint64_t total = 0;
  for (const auto& value : *list) {
    const auto* entry = value.GetIfDict();
    const auto* name = entry ? entry->FindString("name") : nullptr;
    const auto* size = entry ? entry->FindString("size") : nullptr;
    uint64_t bytes;
    if (!name || !SafeTransferName(*name, tree) || !size || size->empty() || size->size() > 20 ||
        !std::ranges::all_of(*size, [](char c) { return c >= '0' && c <= '9'; }) ||
        !base::StringToUint64(*size, &bytes) || bytes > kMaximumFileSize ||
        (name->ends_with("/") && bytes != 0) ||
        !names.insert(base::ToLowerASCII(name->ends_with("/") ? name->substr(0, name->size() - 1) : *name)).second) {
      LOG(ERROR) << "[crPhotos nearby offer] receive: invalid name, size or duplicate at index="
                 << files.size();
      return FailTransfer(failure, kProtocolError);
    }
    total += bytes;
    if (total > kMaximumBatchSize) {
      LOG(ERROR) << "[crPhotos nearby offer] receive: batch size exceeds limit";
      return FailTransfer(failure, kProtocolError);
    }
    files.emplace_back(*name, bytes);
  }
  base::FilePath root;
  bool direct_target = false;
  int64_t receive_tag = 0;
  {
    base::AutoLock lock(state->lock);
    state->snapshot.total = total;
    root = state->snapshot.receive_directory;
    const auto* offered_target = offer->FindString("target");
    // A target announced before pairing must agree with the later manifest.
    if (!state->snapshot.receive_target.empty() &&
        (!offered_target || *offered_target != state->snapshot.receive_target)) return FailTransfer(failure, kProtocolError);
    if (const auto* target = offered_target) {
      auto found = std::ranges::find(state->targets, *target, &NearbySharing::SharedTarget::id);
      if (found == state->targets.end()) return FailTransfer(failure, kFolderError);
      root = found->directory;
      receive_tag = found->tag_id;
      direct_target = receive_tag == 0;
      state->snapshot.receive_target = found->id;
      state->snapshot.receive_target_name = found->name;
    }
  }
  // Fixed dimensions and exact byte counts avoid invoking an image codec on
  // untrusted compressed input. Bad previews are ignored, not transfer errors.
  std::map<base::FilePath, gfx::ImageSkia> previews;
  LOG(INFO) << "[crPhotos nearby offer] receive: manifest validated; files="
            << files.size() << " total_bytes=" << total;
  for (size_t i = 0; i < std::min<size_t>(list->size(), 5); ++i) {
    if (IsCanceled(*state, generation)) return false;
    const auto* encoded = (*list)[i].GetDict().FindString("preview_rgba96");
    constexpr size_t kPixelBytes = 96 * 96 * 4;
    std::string pixels;
    if (!encoded) continue;
    if (encoded->size() != kPixelBytes / 3 * 4 ||
        !base::Base64Decode(*encoded, &pixels) || pixels.size() != kPixelBytes) {
      LOG(WARNING) << "[crPhotos nearby offer] receive: invalid preview ignored; index=" << i;
      continue;
    }
    SkBitmap bitmap;
    if (!bitmap.tryAllocN32Pixels(96, 96)) {
      LOG(WARNING) << "[crPhotos nearby offer] receive: preview allocation failed; index=" << i;
      continue;
    }
    const auto info = SkImageInfo::Make(96, 96, kRGBA_8888_SkColorType,
                                       kUnpremul_SkAlphaType);
    if (!bitmap.writePixels(SkPixmap(info, pixels.data(), 96 * 4))) {
      LOG(WARNING) << "[crPhotos nearby offer] receive: preview conversion failed; index=" << i;
      continue;
    }
    auto image = gfx::ImageSkia::CreateFrom1xBitmap(bitmap);
    image.MakeThreadSafe();
    previews.emplace(base::FilePath::FromUTF8Unsafe(files[i].first), std::move(image));
  }
  LOG(INFO) << "[crPhotos nearby offer] receive: previews ready; count=" << previews.size();
  {
    base::AutoLock lock(state->lock);
    // Publish the manifest and its previews together so a polling view cannot
    // latch an incomplete preview set for this session.
    state->snapshot.files.clear();
    for (const auto& [name, size] : files) state->snapshot.files.push_back(name);
    state->snapshot.previews = std::move(previews);
  }
  std::string fingerprint;
  {
    base::AutoLock lock(state->lock);
    fingerprint = state->peer_fingerprint;
  }
  // Consent is a local receiver preference, never a claim from discovery or
  // the sender. Recheck the authenticated key against current trust records.
  const bool auto_accept = GetNearbySharingPreferences().auto_accept_trusted &&
      !fingerprint.empty() && IsTrusted(state->trust, fingerprint);
  LOG(INFO) << "[crPhotos nearby offer] receive: manifest published; auto_accept="
            << auto_accept;
  if (!auto_accept &&
      !AwaitDecision(channel, state, generation, Stage::kReceivingOffer)) {
    LOG(ERROR) << "[crPhotos nearby offer] receive: approval failed or timed out; canceled="
               << IsCanceled(*state, generation);
    return FailTransfer(failure, kApprovalError);
  }
  if (IsCanceled(*state, generation)) return false;
  {
    base::AutoLock lock(state->lock);
    if (!state->snapshot.receive_target.empty()) {
      auto target = std::ranges::find(state->targets, state->snapshot.receive_target, &NearbySharing::SharedTarget::id);
      if (target == state->targets.end() || target->directory != root || !base::DirectoryExists(root)) return FailTransfer(failure, kFolderError);
    }
  }
  if (!root.IsAbsolute()) return FailTransfer(failure, kFolderError);
  if (!base::CreateDirectory(root)) return FailFileOperation(failure, kFolderError);
  // Pin the destination and use relative operations to prevent path/symlink
  // substitution. Explicit shared targets receive directly into the pane.
#if BUILDFLAG(IS_WIN)
  DirectoryLock root_lock;
  if (!root_lock.Open(root)) return FailFileOperation(failure, kFolderError);
  const auto lock_deadline = base::TimeTicks::Now() + base::Seconds(60);
  while (!root_lock.TryLock()) {
    if (GetLastError() != ERROR_LOCK_VIOLATION || IsCanceled(*state, generation) ||
        base::TimeTicks::Now() >= lock_deadline) return FailFileOperation(failure, kFolderError);
    Poll(nullptr, 0, 100);
  }
#else
  base::ScopedFD root_fd(open(root.value().c_str(), O_RDONLY | O_DIRECTORY |
                             O_CLOEXEC | O_NOFOLLOW));
  if (!root_fd.is_valid()) return FailFileOperation(failure, kFolderError);
  // Serialize crPhotos receivers sharing this directory across processes.
  const auto lock_deadline = base::TimeTicks::Now() + base::Seconds(60);
  while (flock(root_fd.get(), LOCK_EX | LOCK_NB) != 0) {
    if ((errno != EWOULDBLOCK && errno != EAGAIN) ||
        IsCanceled(*state, generation) || base::TimeTicks::Now() >= lock_deadline)
      return FailFileOperation(failure, kFolderError);
    Poll(nullptr, 0, 100);
  }
#endif
  std::set<std::string> existing;
  size_t entries = 0;
#if BUILDFLAG(IS_WIN)
  const auto& scan_directory = root;
#else
  const auto scan_directory = root_fd.get();
#endif
  if (!direct_target && !tree && receive_tag == 0 && !CollectReceivedNames(scan_directory, true, 0, entries, existing, *state,
                            generation, base::TimeTicks::Now() + base::Seconds(60)))
    return FailTransfer(failure, kFolderError);
  const size_t offered_count = files.size();
  std::erase_if(files, [&](const auto& file) {
    return existing.contains(base::ToLowerASCII(file.first));
  });
  total = 0;
  base::ListValue requested;
  for (const auto& [name, size] : files) {
    requested.Append(name);
    total += size;
  }
  auto acceptance = Message("accept");
  acceptance.Set("files", std::move(requested));
  {
    base::AutoLock lock(state->lock);
    state->snapshot.skipped_files = offered_count - files.size();
    state->snapshot.total = total;
  }
  if (files.empty()) {
    if (!channel.SendJson(std::move(acceptance)) ||
        !channel.SendJson(Message("complete"))) return FailTransfer(failure, kConnectionError);
    return true;
  }
  const std::string folder = direct_target ? std::string() : "Received-" + base::HexEncode(base::RandBytesAsString(8));
  const auto batch = direct_target ? root : root.AppendASCII(folder);
  std::vector<base::FilePath> changed_paths{batch};
  for (const auto& [name, size] : files) changed_paths.push_back(batch.Append(base::FilePath::FromUTF8Unsafe(name)));
  ScopedFileCacheMutation received_changes(std::move(changed_paths));
#if BUILDFLAG(IS_WIN)
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(root.value().c_str(), &available, nullptr, nullptr)) return FailFileOperation(failure, kFolderError);
  if (total > available.QuadPart) return FailTransfer(failure, kSpaceError);
  if (!direct_target && !CreatePrivateDirectory(batch)) return FailFileOperation(failure, kFolderError);
  PinnedDirectory directory;
  if (!directory.Open(batch)) return FailFileOperation(failure, kFolderError);
#else
  struct statvfs space;
  if (fstatvfs(root_fd.get(), &space) != 0) return FailFileOperation(failure, kFolderError);
  if (!space.f_frsize) return FailTransfer(failure, kFolderError);
  if (total / space.f_frsize + 1 > space.f_bavail) return FailTransfer(failure, kSpaceError);
  if (!direct_target && mkdirat(root_fd.get(), folder.c_str(), 0700) != 0) return FailFileOperation(failure, kFolderError);
  base::ScopedFD directory(openat(root_fd.get(), direct_target ? "." : folder.c_str(),
      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!directory.is_valid()) return FailFileOperation(failure, kFolderError);
  if (fsync(root_fd.get()) != 0) return FailFileOperation(failure, kFlushError);
#endif
  {
    base::AutoLock lock(state->lock);
    state->snapshot.batch_directory = batch;
  }
  if (!channel.SendJson(std::move(acceptance))) return FailTransfer(failure, kConnectionError);
  SetStage(state, Stage::kTransferring);
  std::vector<uint8_t> buffer(kChunkSize);
  std::set<base::FilePath> tag_paths;
  for (const auto& [name, size] : files) {
    std::string sender;
    {
      base::AutoLock lock(state->lock);
      sender = state->snapshot.peer_name;
      state->snapshot.current_file = name;
    }
    FileOperationLog log("receive", "remote", sender + "/" + name,
        batch.Append(base::FilePath::FromUTF8Unsafe(name)).AsUTF8Unsafe());
    if (!log.valid()) return FailTransfer(failure, kHistoryError);
    const bool is_directory = tree && name.ends_with("/");
    auto parts = base::SplitString(name, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    const auto& leaf = parts.back();
    const size_t parents = is_directory ? parts.size() : parts.size() - 1;
    const std::string temporary = ".incoming-" + base::HexEncode(base::RandBytesAsString(8)) + ".part";
    base::File output;
#if BUILDFLAG(IS_WIN)
    auto parent_path = batch;
    std::vector<PinnedDirectory> pins;
    for (size_t i = 0; i < parents; ++i) {
      parent_path = parent_path.Append(base::FilePath::FromUTF8Unsafe(parts[i]));
      if (!base::CreateDirectory(parent_path)) return FailFileOperation(failure, kFolderError);
      PinnedDirectory pin;
      if (!pin.Open(parent_path)) return FailFileOperation(failure, kFolderError);
      pins.push_back(std::move(pin));
    }
    const auto temporary_path = parent_path.AppendASCII(temporary);
    if (!is_directory) output = OpenRegularFile(temporary_path, true);
#else
    base::ScopedFD parent_fd(openat(directory.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!parent_fd.is_valid()) return FailFileOperation(failure, kFolderError);
    for (size_t i = 0; i < parents; ++i) {
      if (mkdirat(parent_fd.get(), parts[i].c_str(), 0700) != 0 && errno != EEXIST) return FailFileOperation(failure, kFolderError);
      base::ScopedFD child(openat(parent_fd.get(), parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
      if (!child.is_valid()) return FailFileOperation(failure, kFolderError);
      parent_fd = std::move(child);
    }
    if (!is_directory) {
      base::ScopedFD fd(openat(parent_fd.get(), temporary.c_str(),
          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
      if (!fd.is_valid()) return FailFileOperation(failure, kWriteError);
      output = base::File(fd.release());
    }
#endif
    if (!is_directory && !output.IsValid()) return FailFileOperation(failure, kWriteError);
    crypto::hash::Hasher hasher(crypto::hash::kSha256);
    uint64_t remaining = size;
    bool ok = true;
    while (remaining && !IsCanceled(*state, generation)) {
      auto part = base::span(buffer).first(
          static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size())));
      if (!channel.Bytes(part, false)) {
        FailTransfer(failure, kConnectionError);
        ok = false;
        break;
      }
      if (!output.WriteAtCurrentPosAndCheck(part)) {
        FailFileOperation(failure, kWriteError);
        ok = false;
        break;
      }
      hasher.Update(part);
      remaining -= part.size();
      base::AutoLock lock(state->lock);
      state->snapshot.transferred += part.size();
    }
    ok = ok && !remaining && !IsCanceled(*state, generation);
    std::array<uint8_t, 32> hash;
    hasher.Finish(hash);
    auto digest = ok ? channel.ReadJson() : std::nullopt;
    const auto* expected = digest ? digest->FindString("sha256") : nullptr;
    if (ok) ok = CheckTransferMessage(digest, "digest", failure, kConnectionError);
    if (ok && !expected) ok = FailTransfer(failure, kProtocolError);
    if (ok && *expected != base::HexEncode(hash)) ok = FailTransfer(failure, kChecksumError);
    if (ok && !is_directory && !output.Flush()) ok = FailFileOperation(failure, kFlushError);
    output.Close();
#if BUILDFLAG(IS_WIN)
    const auto destination = parent_path.Append(base::FilePath::FromUTF8Unsafe(leaf));
    if (ok && !is_directory && !MoveFileExW(temporary_path.value().c_str(),
        destination.value().c_str(), MOVEFILE_WRITE_THROUGH)) {
      const auto error = GetLastError();
      if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        ok = ConfirmReceivedReplacement(channel, state, generation, failure);
        if (ok && !MoveFileExW(temporary_path.value().c_str(), destination.value().c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
          ok = FailFileOperation(failure, kCommitError);
      } else {
        ok = FailFileOperation(failure, kCommitError);
      }
    }
    if (!is_directory) base::DeleteFile(temporary_path);
#else
    // Create without overwriting first; rename only after explicit consent.
    if (ok && !is_directory && linkat(parent_fd.get(), temporary.c_str(), parent_fd.get(),
                        leaf.c_str(), 0) != 0) {
      if (errno == EEXIST) {
        ok = ConfirmReceivedReplacement(channel, state, generation, failure);
        if (ok && renameat(parent_fd.get(), temporary.c_str(), parent_fd.get(), leaf.c_str()) != 0)
          ok = FailFileOperation(failure, kCommitError);
      } else {
        ok = FailFileOperation(failure, kCommitError);
      }
    }
    if (!is_directory) unlinkat(parent_fd.get(), temporary.c_str(), 0);
    if (ok && fsync(parent_fd.get()) != 0) ok = FailFileOperation(failure, kFlushError);
#endif
    if (!ok) return false;
    if (!UpdateTrust(state, false, 0, size)) return FailTransfer(failure, kStatisticsError);
    if (!channel.SendJson(Message("stored"))) return FailTransfer(failure, kConnectionError);
    log.Succeed();
    if (receive_tag > 0) {
      auto path = batch;
      for (const auto& part : parts) {
        path = path.Append(base::FilePath::FromUTF8Unsafe(part));
        tag_paths.insert(path);
      }
    }
  }
  if (receive_tag > 0 && !tag_paths.empty() &&
      EditFileTags(std::vector<base::FilePath>(tag_paths.begin(), tag_paths.end()), {receive_tag}, {}) != TagWriteResult::kOk) {
    LOG(ERROR) << "crFiles: received files saved, but tagging failed; tag=" << receive_tag;
    base::AutoLock lock(state->lock);
    state->snapshot.warning_message_id = IDS_CRFILES_TAG_SAVE_ERROR;
  }
  if (!channel.SendJson(Message("complete"))) return FailTransfer(failure, kConnectionError);
  return true;
}

// DNS-SD wire helpers. Names are generated locally; incoming DNS names and
// compressed pointers are parsed by Chromium's bounded DNS parser.
void Number(std::string& out, uint64_t value, size_t bytes) {
  for (size_t i = bytes; i > 0; --i)
    out.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}
std::string DnsName(std::string_view name) {
  std::string out;
  for (auto label : base::SplitStringPiece(name, ".", base::KEEP_WHITESPACE,
                                          base::SPLIT_WANT_NONEMPTY)) {
    out.push_back(static_cast<char>(label.size()));
    out.append(label);
  }
  out.push_back(0);
  return out;
}
constexpr char kService[] = "_crphotos._tcp.local";
void Record(std::string& out, const std::string& name, uint16_t type,
            uint32_t ttl, const std::string& data) {
  out += DnsName(name);
  Number(out, type, 2);
  Number(out, type == 12 ? 1 : 0x8001, 2);
  Number(out, ttl, 4);
  Number(out, data.size(), 2);
  out += data;
}
std::string Advertisement(const State& state, uint16_t port,
                          in_addr address, uint32_t ttl) {
  const std::string instance = state.id + "." + kService;
  const std::string host = "crphotos-" + state.id + ".local";
  std::string out;
  for (uint16_t value : {0, 0x8400, 0, 4, 0, 0}) Number(out, value, 2);
  Record(out, kService, 12, ttl, DnsName(instance));
  std::string srv(4, '\0');
  Number(srv, port, 2);
  srv += DnsName(host);
  Record(out, instance, 33, ttl, srv);
  std::string txt;
  for (const auto& field : std::array<std::string, 5>{
           "v=1", "caps=targets,tree,sync-v1", "name=" + state.name,
           "avatar=" + base::NumberToString(state.avatar),
           "identity=" + base::NumberToString(state.identity_port)}) {
    txt.push_back(static_cast<char>(field.size()));
    txt += field;
  }
  Record(out, instance, 16, ttl, txt);
  const auto bytes = base::byte_span_from_ref(address.s_addr);
  Record(out, host, 1, ttl, std::string(base::as_string_view(bytes)));
  return out;
}

ScopedSocket CreateDiscoverySocket() {
  auto socket_fd = CreateSocket(SOCK_DGRAM);
  if (!socket_fd.is_valid()) return {};
  const int one = 1;
#if BUILDFLAG(IS_MAC)
  const unsigned char ttl = 255;
  // Bonjour's mDNSResponder already owns UDP 5353 on macOS. SO_REUSEPORT
  // allows each multicast listener to receive its own copy of the packets.
  if (SetOption(socket_fd.get(), SOL_SOCKET, SO_REUSEPORT, one) != 0) {
    PLOG(ERROR) << "Could not share the nearby discovery port";
    return {};
  }
#else
  const int ttl = 255;
#endif
  if (SetOption(socket_fd.get(), SOL_SOCKET, SO_REUSEADDR, one) != 0 ||
      SetOption(socket_fd.get(), IPPROTO_IP, IP_MULTICAST_TTL, ttl) != 0) {
    LOG(ERROR) << "Could not configure nearby discovery: " << SocketError();
    return {};
  }
  sockaddr_in bind_address{};
  bind_address.sin_family = AF_INET;
  bind_address.sin_port = htons(5353);
  if (bind(socket_fd.get(), reinterpret_cast<sockaddr*>(&bind_address),
           sizeof(bind_address)) != 0) {
    LOG(ERROR) << "Could not bind nearby discovery port: " << SocketError();
    return {};
  }
  return socket_fd;
}

void DiscoveryAttempt(std::shared_ptr<State> state, uint16_t port) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::WILL_BLOCK);
  auto socket_fd = CreateDiscoverySocket();
#if !BUILDFLAG(IS_MAC)
  if (!socket_fd.is_valid()) return;
#endif
  sockaddr_in multicast{};
  multicast.sin_family = AF_INET;
  multicast.sin_port = htons(5353);
  inet_pton(AF_INET, "224.0.0.251", &multicast.sin_addr);
  std::map<uint32_t, in_addr> interfaces;
  std::set<uint32_t> join_failures;
  std::set<uint32_t> send_failures;
  auto address_text = [](in_addr address) {
    std::array<char, INET_ADDRSTRLEN> text{};
    return inet_ntop(AF_INET, &address, text.data(), text.size()) ?
        std::string(text.begin(), std::ranges::find(text, '\0')) : std::string();
  };
  auto refresh_interfaces = [&] {
    std::map<uint32_t, in_addr> current;
    for (const in_addr local : LocalAddresses()) current.emplace(local.s_addr, local);
    std::erase_if(interfaces, [&](const auto& entry) {
      if (current.contains(entry.first)) return false;
      ip_mreq membership{multicast.sin_addr, entry.second};
      // A removed interface may already have lost its membership in the OS.
      SetOption(socket_fd.get(), IPPROTO_IP, IP_DROP_MEMBERSHIP, membership);
      LOG(INFO) << "crFiles nearby discovery interface removed; address="
                << address_text(entry.second);
      return true;
    });
    std::erase_if(join_failures, [&](uint32_t address) { return !current.contains(address); });
    std::erase_if(send_failures, [&](uint32_t address) { return !current.contains(address); });
    for (const auto& [address, local] : current) {
      if (interfaces.contains(address)) continue;
      ip_mreq membership{multicast.sin_addr, local};
      if (SetOption(socket_fd.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, membership) != 0) {
        const int error = SocketError();
        if (join_failures.insert(address).second)
          LOG(WARNING) << "crFiles nearby discovery multicast join failed; address="
                       << address_text(local) << "; error=" << error;
        continue;
      }
      interfaces.emplace(address, local);
      join_failures.erase(address);
      LOG(INFO) << "crFiles nearby discovery multicast joined; address="
                << address_text(local);
    }
  };
  auto publish = [&](uint32_t lifetime) {
    bool sent = false;
    for (const auto& [address, local] : interfaces) {
      int error = 0;
      bool published = false;
      if (SetOption(socket_fd.get(), IPPROTO_IP, IP_MULTICAST_IF, local) != 0) {
        error = SocketError();
      } else {
        const std::string bytes = Advertisement(*state, port, local, lifetime);
        const auto written = sendto(socket_fd.get(), bytes.data(), static_cast<int>(bytes.size()), kSendFlags,
            reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast));
        published = written == static_cast<int>(bytes.size());
        if (written < 0) error = SocketError();
      }
      if (published) {
        sent = true;
        if (send_failures.erase(address))
          LOG(INFO) << "crFiles nearby discovery publishing recovered; address="
                    << address_text(local);
      } else if (send_failures.insert(address).second) {
        LOG(WARNING) << "crFiles nearby discovery publishing failed; address="
                     << address_text(local) << "; error=" << error;
      }
    }
    return sent;
  };
  struct Seen { NearbySharing::Peer peer; base::TimeTicks expires; };
  std::map<std::string, Seen> seen;
  base::TimeTicks last_sent;
  base::TimeTicks last_response;
#if BUILDFLAG(IS_MAC)
  int failed_cycles = 0;
  auto retry_delay = base::Seconds(5);
  bool recreate_socket = false;
  bool probe_required = true;
  bool awaiting_peer_response = false;
  bool receive_failed = false;
  auto receive_failure = [&](int error) {
    if (!receive_failed)
      LOG(WARNING) << "crFiles macOS nearby discovery receive failed; error=" << error;
    receive_failed = true;
  };
  // A successful send validates the local path, not the presence of a peer.
  // Do not keep an error visible merely because no other device is advertising.
  auto probe = [&] {
    std::string question;
    for (uint16_t value : {0, 0, 1, 0, 0, 0}) Number(question, value, 2);
    question += DnsName(kService);
    Number(question, 12, 2);
    Number(question, 1, 2);
    bool sent = false;
    for (const auto& [address, local] : interfaces) {
      if (SetOption(socket_fd.get(), IPPROTO_IP, IP_MULTICAST_IF, local) != 0) continue;
      if (sendto(socket_fd.get(), question.data(), static_cast<int>(question.size()), kSendFlags,
          reinterpret_cast<sockaddr*>(&multicast), sizeof(multicast)) ==
          static_cast<int>(question.size())) sent = true;
    }
    return sent;
  };
#endif
  std::array<uint8_t, 4096> buffer;
  while (!state->stop) {
    const auto now = base::TimeTicks::Now();
#if BUILDFLAG(IS_MAC)
    if (now - last_sent >= retry_delay) {
      if (recreate_socket) {
        // Retry failed setup or sustained failure of the entire discovery path.
        // Keep the peer cache and its original expiry across socket replacement.
        socket_fd.reset();
        socket_fd = CreateDiscoverySocket();
        interfaces.clear();
        join_failures.clear();
        send_failures.clear();
        last_response = {};
        probe_required = true;
        awaiting_peer_response = false;
        receive_failed = false;
        recreate_socket = false;
        LOG(INFO) << "crFiles macOS nearby discovery socket retry; ready="
                  << socket_fd.is_valid() << "; delay_ms=" << retry_delay.InMilliseconds();
      }
#else
    if (now - last_sent >= base::Seconds(5)) {
#endif
      // Reconcile memberships independently. Failed joins and sends are retried
      // without restarting healthy interfaces or discarding unexpired peers.
      if (socket_fd.is_valid()) refresh_interfaces();
      bool published = socket_fd.is_valid() && publish(15);
#if BUILDFLAG(IS_MAC)
      published = published && !receive_failed;
      if (published && probe_required) {
        published = probe();
        if (published) {
          probe_required = false;
          awaiting_peer_response = true;
          LOG(INFO) << "crFiles macOS nearby discovery probe sent; peer_response_confirmed=0";
        }
      }
      if (published) {
        if (failed_cycles)
          LOG(INFO) << "crFiles macOS nearby discovery local path recovered; failed_cycles="
                    << failed_cycles << "; peer_response_confirmed=0";
        failed_cycles = 0;
        retry_delay = base::Seconds(5);
      } else {
        probe_required = true;
        failed_cycles = std::min(failed_cycles + 1, 3);
        recreate_socket = !socket_fd.is_valid();
        if (failed_cycles >= 3) {
          recreate_socket = true;
          retry_delay = std::min(retry_delay * 2, base::Seconds(30));
        }
      }
#endif
      {
        base::AutoLock lock(state->lock);
        state->snapshot.discovery_ready = !state->stop && published;
#if BUILDFLAG(IS_MAC)
        if (state->stop || published) state->snapshot.discovery_message_id = 0;
        else if (failed_cycles >= 3)
          state->snapshot.discovery_message_id = IDS_CRFILES_NEARBY_MAC_NETWORK_UNAVAILABLE;
#endif
      }
      last_sent = now;
    }
    PollFd pfd{socket_fd.get(), POLLIN, 0};
    const int poll_result = socket_fd.is_valid() ? Poll(&pfd, 1, 200) : Poll(nullptr, 0, 200);
#if BUILDFLAG(IS_MAC)
    if (poll_result < 0 && !RetrySocket()) {
      receive_failure(SocketError());
      Poll(nullptr, 0, 200);
    }
    if (poll_result > 0 && (pfd.revents & (POLLERR | POLLNVAL | POLLHUP))) {
      receive_failure(PendingError(socket_fd.get()));
      Poll(nullptr, 0, 200);
    }
#endif
    if (poll_result > 0) {
      sockaddr_in source{};
      SocketLength source_size = sizeof(source);
      const auto length = recvfrom(socket_fd.get(), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
          0, reinterpret_cast<sockaddr*>(&source), &source_size);
#if BUILDFLAG(IS_MAC)
      if (length < 0 && !RetrySocket()) receive_failure(SocketError());
#endif
      if (length >= 12 && length < static_cast<int>(buffer.size()) &&
          source.sin_port == htons(5353)) {
        auto packet = base::span(buffer).first(static_cast<size_t>(length));
        // Answer a bounded number of standard PTR questions. Replies remain
        // multicast and rate-limited, avoiding a unicast amplification service.
        if (packet[2] == 0 && packet[3] == 0 && packet[4] == 0 &&
            packet[5] > 0 && packet[5] <= 16 &&
            now - last_response >= base::Seconds(1)) {
          net::DnsRecordParser questions(packet, 12, 0);
          for (size_t i = 0; i < packet[5]; ++i) {
            std::string name;
            uint16_t type;
            if (!questions.ReadQuestion(name, type)) break;
            if (name == kService && (type == 12 || type == 255)) {
              publish(15);
              last_response = now;
              break;
            }
          }
        }
        // This v1 endpoint consumes the complete four-record announcements
        // emitted by its peers; no cache joins across untrusted packets.
        if (packet[0] == 0 && packet[1] == 0 && packet[2] == 0x84 &&
            packet[3] == 0 && packet[4] == 0 && packet[5] == 0 &&
            packet[6] == 0 && packet[7] == 4 && packet[8] == 0 &&
            packet[9] == 0 && packet[10] == 0 && packet[11] == 0) {
          net::DnsRecordParser parser(packet, 12, 4);
          net::DnsResourceRecord ptr, srv, txt, host;
          if (parser.ReadRecord(&ptr) && parser.ReadRecord(&srv) &&
              parser.ReadRecord(&txt) && parser.ReadRecord(&host) && parser.AtEnd() &&
              ptr.name == kService && ptr.type == 12 && srv.type == 33 &&
              srv.rdata.size() >= 7 && txt.type == 16 && txt.name == srv.name &&
              host.type == 1 && host.rdata.size() == 4 &&
              srv.name.ends_with(std::string(".") + kService)) {
            std::string instance_name, host_name;
            const unsigned instance_bytes = ptr.rdata.empty() ? 0 :
                parser.ReadName(ptr.rdata.data(), &instance_name);
            const unsigned host_bytes =
                parser.ReadName(srv.rdata.subspan<6>().data(), &host_name);
            const bool valid_names = instance_bytes && instance_bytes <= ptr.rdata.size() &&
                host_bytes && host_bytes <= srv.rdata.size() - 6 &&
                instance_name == srv.name && host_name == host.name;
            const std::string id = srv.name.substr(0, srv.name.find('.'));
            std::string name;
            bool version = false;
            int avatar = 0;
            unsigned identity_port = 0;
            auto fields = base::span<const uint8_t>(txt.rdata);
            while (!fields.empty()) {
              const size_t size = fields.front();
              fields = fields.subspan<1>();
              if (size > fields.size()) break;
              const std::string_view text = base::as_string_view(fields.first(size));
              if (text == "v=1") version = true;
              if (text.starts_with("identity=")) {
                unsigned parsed = 0;
                if (base::StringToUint(text.substr(9), &parsed) && parsed <= 65535)
                  identity_port = parsed;
              }
              if (text.starts_with("name=")) name = text.substr(5);
              if (text.starts_with("avatar=")) {
                int parsed = 0;
                if (base::StringToInt(text.substr(7), &parsed) &&
                    parsed >= 0 && parsed < NearbySharing::kAvatarCount)
                  avatar = parsed;
              }
              fields = fields.subspan(size);
            }
            const uint16_t peer_port = (srv.rdata[4] << 8) | srv.rdata[5];
            std::array<char, INET_ADDRSTRLEN> ip;
            if (valid_names && id != state->id && id.size() == 32 && version &&
                SafeDeviceName(name) && peer_port &&
                (seen.contains(id) || seen.size() < 128) &&
                inet_ntop(AF_INET, &source.sin_addr, ip.data(), ip.size())) {
              if (!ptr.ttl) seen.erase(id);
              else {
#if BUILDFLAG(IS_MAC)
                if (awaiting_peer_response) {
                  awaiting_peer_response = false;
                  LOG(INFO) << "crFiles macOS nearby discovery peer traffic verified; peer_traffic_confirmed=1";
                }
#endif
                const std::string address = std::string(ip.begin(), std::ranges::find(ip, '\0')) +
                    ":" + base::NumberToString(peer_port);
                seen[id] = {{id, name, address, avatar,
                    static_cast<uint16_t>(identity_port), {}, {}, false}, now + base::Seconds(15)};
              }
            }
          }
        }
      }
    }
    std::erase_if(seen, [now](const auto& entry) { return entry.second.expires <= now; });
    base::AutoLock lock(state->lock);
    state->snapshot.peers.clear();
    if (!state->stop) {
      std::set<std::string> live;
      for (const auto& [id, entry] : seen) {
        auto peer = entry.peer;
        const auto key = IdentityKey(peer);
        live.insert(key);
        const auto verified = state->identities.find(key);
        if (verified != state->identities.end() && verified->second.expires > now) {
          peer.verified_fingerprint = verified->second.fingerprint;
          peer.identity_expires = verified->second.expires;
          peer.folder_transfer = verified->second.folder_transfer;
          peer.sync_v1 = verified->second.sync_v1;
          peer.targets = verified->second.targets;
        }
        state->snapshot.peers.push_back(std::move(peer));
      }
      std::erase_if(state->identities, [&](const auto& entry) {
        return !live.contains(entry.first) && entry.second.expires <= now && entry.second.retry_after <= now;
      });
    }
  }
  if (socket_fd.is_valid()) publish(0);
}

void Discovery(std::shared_ptr<State> state, uint16_t port) {
  while (!state->stop) {
    {
      base::AutoLock lock(state->lock);
      state->snapshot.discovery_ready = false;
      state->snapshot.peers.clear();
      state->identities.clear();
    }
    DiscoveryAttempt(state, port);
    {
      base::AutoLock lock(state->lock);
      state->snapshot.discovery_ready = false;
      state->snapshot.peers.clear();
    }
    // Retry socket setup failures without spinning. Membership changes and
    // offline startup are handled in place by DiscoveryAttempt().
    for (int i = 0; i < 25 && !state->stop; ++i) Poll(nullptr, 0, 200);
  }
}

bool IsPrivateAddress(in_addr address) {
  const uint32_t ip = ntohl(address.s_addr);
  return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8 ||
         (ip >> 16) == 0xa9fe;
}

ScopedSocket Connect(std::string_view address, const State& state, uint64_t generation,
                       FailureInfo& failure, base::TimeDelta timeout = base::Seconds(10)) {
  failure.phase = "tcp_connect";
  failure.reason = "invalid_address_or_port";
  const auto parts = base::SplitStringPiece(address, ":", base::TRIM_WHITESPACE,
                                           base::SPLIT_WANT_ALL);
  unsigned port = 0;
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  if (parts.size() != 2 || !base::StringToUint(parts[1], &port) || !port || port > 65535 ||
      inet_pton(AF_INET, std::string(parts[0]).c_str(), &endpoint.sin_addr) != 1) {
    return {};
  }
  // No DNS rebinding, redirects, multicast, loopback, or public destinations.
  if (!IsPrivateAddress(endpoint.sin_addr)) {
    failure.reason = "non_private_address";
    return {};
  }
  endpoint.sin_port = htons(port);
  auto fd = CreateSocket(SOCK_STREAM);
  if (!fd.is_valid()) {
    failure.reason = "socket_creation_failed";
    failure.os_error = SocketError();
    return {};
  }
  const int result = connect(fd.get(), reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint));
  if (result != 0 && (!RetrySocket() ||
      !WaitSocket(fd.get(), POLLOUT, state, generation,
                  base::TimeTicks::Now() + timeout))) {
    failure.reason = "connect_or_wait_failed";
    failure.os_error = SocketError();
    return {};
  }
  const int error = PendingError(fd.get());
  if (error) {
    failure.reason = "socket_error";
    failure.os_error = error;
    return {};
  }
  return fd;
}

// This listener never enters Pair(), publishes a session, or reads a manifest.
// A separate worker and port keep even a stalled probe off the transfer path.
void ServeIdentities(std::shared_ptr<State> state, ScopedSocket listener,
                     bssl::UniquePtr<SSL_CTX> context, uint16_t transfer_port) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::WILL_BLOCK);
  base::TimeTicks next_allowed;
  while (!state->stop) {
    PollFd pfd{listener.get(), POLLIN, 0};
    if (Poll(&pfd, 1, 200) <= 0) continue;
    sockaddr_in remote{};
    SocketLength size = sizeof(remote);
    auto fd = Accept(listener.get(), reinterpret_cast<sockaddr*>(&remote), &size);
    if (!fd.is_valid() || !IsPrivateAddress(remote.sin_addr) ||
        base::TimeTicks::Now() < next_allowed) continue;
    next_allowed = base::TimeTicks::Now() + base::Milliseconds(250);
    Channel channel(std::move(fd), context.get(), true, state,
                    state->cancel.load(), true);
    const bool handshake_ok = channel.Handshake();
    if (!handshake_ok) continue;
    auto identity = Message("identity");
    identity.Set("id", state->id);
    identity.Set("port", transfer_port);
    identity.Set("folder_transfer", true);
    identity.Set("sync_v1", true);
    identity.Set("sync_permissions", 2);
    identity.Set("sync_source_updates", 1);
    identity.Set("sync_removal", 1);
    identity.Set("sync_wake", 1);
    if (!channel.SendJson(std::move(identity))) continue;
    // Legacy probes read only the small identity frame and close the channel.
    auto targets = Message("targets"); targets.Set("targets", SharedTargetsJson(state));
    if (!channel.SendJson(std::move(targets))) continue;
    // Optional control frame on the independent identity channel. Old probes
    // close after the catalog; no pairing, file access or transfer UI is opened.
    const auto wake = channel.ReadJson(base::Seconds(2));
    const auto* ids = wake ? wake->FindList("ids") : nullptr;
    if (!IsMessage(wake, "sync-wake") || !ids || ids->empty() || ids->size() > 128) continue;
    if (std::ranges::any_of(*ids, [](const auto& id) {
          return !id.is_string() || !IsSyncId(id.GetString());
        })) continue;
    const auto fingerprint = channel.PeerFingerprint();
    base::ListValue accepted;
    for (const auto& id : *ids) {
      if (state->sync->RequestWake(id.GetString(), fingerprint)) accepted.Append(id.GetString());
    }
    auto queued = Message("sync-wake-queued"); queued.Set("ids", std::move(accepted));
    channel.SendJson(std::move(queued));
  }
}

void ProbePhones(std::shared_ptr<State> state) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::WILL_BLOCK);
  std::map<std::string, base::TimeTicks> retries;
  while (!state->stop) {
    std::vector<ConnectPeer> peers;
    {
      base::AutoLock lock(state->lock);
      peers = state->snapshot.connect_peers;
    }
    const auto key = [](const ConnectPeer& peer) {
      return peer.id + "|" + peer.address + ":" + base::NumberToString(peer.port);
    };
    std::erase_if(retries, [&](const auto& entry) {
      return std::ranges::none_of(peers, [&](const auto& peer) { return key(peer) == entry.first; });
    });
    for (const auto& peer : peers) {
      if (state->stop) return;
      if (peer.legacy_service || !std::ranges::contains(peer.capabilities, "receive-v1") ||
          ConnectClient::IsVerifiedPeer(peer, base::Seconds(45))) continue;
      const auto endpoint = key(peer);
      if (retries[endpoint] > base::TimeTicks::Now()) continue;
      retries[endpoint] = base::TimeTicks::Now() + base::Seconds(30);
      VerifyDiscoveredPhone(peer, base::BindRepeating(
          [](std::shared_ptr<State> state, ConnectPeer target) {
            if (state->stop) return true;
            base::AutoLock lock(state->lock);
            return std::ranges::none_of(state->snapshot.connect_peers, [&](const auto& current) {
              return current.id == target.id && current.address == target.address && current.port == target.port;
            });
          }, state, peer));
      // Bound the aggregate probe rate, including advertisements with no local pairing.
      Poll(nullptr, 0, 250);
    }
    Poll(nullptr, 0, 200);
  }
}

void ProbeIdentities(std::shared_ptr<State> state, bssl::UniquePtr<SSL_CTX> context) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::WILL_BLOCK);
  while (!state->stop) {
    const auto wakeups = state->sync->PendingWakeups();
    std::optional<NearbySharing::Peer> target;
    std::string expected_fingerprint;
    {
      base::AutoLock lock(state->lock);
      const auto now = base::TimeTicks::Now();
      base::TimeTicks earliest = base::TimeTicks::Max();
      for (const auto& peer : state->snapshot.peers) {
        // Only probe explicitly advertised endpoints, never legacy transfer
        // ports. Each endpoint has one in-flight attempt and bounded retries.
        if (!peer.identity_port) continue;
        const auto found = state->identities.find(IdentityKey(peer));
        auto retry = found == state->identities.end() ? base::TimeTicks() : found->second.retry_after;
        if (found != state->identities.end() && found->second.sync_wake &&
            std::ranges::any_of(wakeups, [&](const auto& wake) {
              return wake.fingerprint == found->second.fingerprint;
            })) retry = {};
        if (retry <= now && retry < earliest) {
          target = peer;
          expected_fingerprint = found == state->identities.end() ? std::string() : found->second.fingerprint;
          earliest = retry;
        }
      }
      if (target) state->identities[IdentityKey(*target)].retry_after = now + base::Seconds(30);
    }
    if (!target) {
      Poll(nullptr, 0, 200);
      continue;
    }
    for (const auto& wake : wakeups) {
      if (wake.fingerprint == expected_fingerprint) state->sync->WakeAttempt(wake);
    }
    const auto separator = target->address.rfind(':');
    if (separator == std::string::npos) continue;
    const auto address = target->address.substr(0, separator + 1) +
        base::NumberToString(target->identity_port);
    const auto generation = state->cancel.load();
    FailureInfo failure;
    auto fd = Connect(address, *state, generation, failure, base::Seconds(2));
    std::string fingerprint;
    bool identity_mismatch = false;
    bool folder_transfer = false;
    bool sync_v1 = false;
    bool sync_removal = false;
    bool sync_wake = false;
    std::vector<NearbySharing::SharedTarget> targets;
    if (fd.is_valid()) {
      Channel channel(std::move(fd), context.get(), false, state, generation, true);
      const bool handshake_ok = channel.Handshake();
      if (handshake_ok) {
        const auto presented_fingerprint = channel.PeerFingerprint();
        {
          base::AutoLock lock(state->lock);
          const auto previous = state->identities.find(IdentityKey(*target));
          identity_mismatch = previous != state->identities.end() && !previous->second.fingerprint.empty() &&
              presented_fingerprint != previous->second.fingerprint;
        }
        const auto identity = channel.ReadJson(base::Seconds(2));
        const auto* id = identity ? identity->FindString("id") : nullptr;
        unsigned transfer_port = 0;
        const bool type_ok = IsMessage(identity, "identity");
        const bool id_ok = id && *id == target->id;
        const bool port_ok = identity &&
            base::StringToUint(target->address.substr(separator + 1), &transfer_port) &&
            identity->FindInt("port") == static_cast<int>(transfer_port);
        identity_mismatch |= identity && !(type_ok && id_ok && port_ok);
        if (type_ok && id_ok && port_ok) {
          fingerprint = channel.PeerFingerprint();
          folder_transfer = identity->FindBool("folder_transfer").value_or(false);
          sync_v1 = identity->FindBool("sync_v1").value_or(false) && identity->FindInt("sync_permissions") == 2 &&
              identity->FindInt("sync_source_updates") == 1;
          sync_removal = sync_v1 && identity->FindInt("sync_removal") == 1;
          sync_wake = sync_v1 && identity->FindInt("sync_wake") == 1;
          if (folder_transfer) {
            const auto catalog = channel.ReadJson(base::Seconds(2));
            if (IsMessage(catalog, "targets")) targets = ParseTargets(*catalog);
          }
          if (sync_wake && !identity_mismatch) {
            base::ListValue ids;
            for (const auto& wake : wakeups) {
              // Pin to the relationship certificate, not the advertised name.
              if (wake.fingerprint != fingerprint) continue;
              state->sync->WakeAttempt(wake);
              ids.Append(wake.id);
            }
            if (!ids.empty()) {
              auto request = Message("sync-wake"); request.Set("ids", std::move(ids));
              const bool sent = channel.SendJson(std::move(request));
              const auto reply = sent ? channel.ReadJson(base::Seconds(2)) : std::nullopt;
              if (!IsMessage(reply, "sync-wake-queued"))
                VLOG(1) << "[crFiles sync] wake_not_acknowledged peer=" << target->id;
              // A queue acknowledgement is not a file commit. Keep the outbox
              // pending until the data session durably acknowledges its counter.
            }
          }
        }
      }
    }
    {
      base::AutoLock lock(state->lock);
      const auto key = IdentityKey(*target);
      const bool present = std::ranges::any_of(state->snapshot.peers,
          [&](const auto& peer) { return IdentityKey(peer) == key; });
      if (!state->stop && present) {
        const auto now = base::TimeTicks::Now();
        auto& cached = state->identities[key];
        if (fingerprint.empty() && !identity_mismatch && cached.expires > now) {
          // A timeout is not a new identity. Keep the previous proof only for
          // its original lifetime and retry before it expires.
          cached.retry_after = now + base::Seconds(5);
        } else {
          const auto expires = fingerprint.empty() ? now : now + base::Seconds(60);
          cached = {std::move(fingerprint), expires,
                    now + base::Seconds(20), folder_transfer, std::move(targets), sync_v1, sync_removal, sync_wake};
        }
      }
    }
    // Globally bound outgoing connection rate, including forged discovery IDs.
    Poll(nullptr, 0, 250);
  }
}

class DesktopSyncWire : public SyncWire {
 public:
  DesktopSyncWire(Channel& channel, std::shared_ptr<State> state, uint64_t generation)
      : channel_(channel), state_(std::move(state)), generation_(generation) {}
  bool Write(base::DictValue message) override { return channel_.SendJson(std::move(message)); }
  std::optional<base::DictValue> Read() override {
    for (;;) {
      auto message = channel_.ReadJson(base::Seconds(250));
      if (!IsMessage(message, "sync-heartbeat")) return message;
      if (Canceled()) return std::nullopt;
    }
  }
  bool KeepAlive() override {
    if (base::TimeTicks::Now() - heartbeat_ < base::Seconds(5)) return !Canceled();
    heartbeat_ = base::TimeTicks::Now();
    return channel_.SendJson(Message("sync-heartbeat"));
  }
  bool Bytes(base::span<uint8_t> bytes, bool sending) override { return channel_.Bytes(bytes, sending); }
  bool Canceled() override { return IsCanceled(*state_, generation_); }
  std::optional<base::FilePath> Consent(const base::FilePath& destination,
      const std::string& name, bool single_file, int error) override {
    {
      base::AutoLock lock(state_->lock);
      state_->snapshot.sync_background = false;
      state_->snapshot.files = {single_file ? name : name + "/"};
      state_->snapshot.sync_receive_name = name;
      state_->snapshot.sync_receive_file = single_file;
      state_->snapshot.warning_message_id = error;
      state_->snapshot.batch_directory = single_file ? destination.DirName() : destination;
      state_->snapshot.receive_directory = destination;
    }
    if (!AwaitDecision(channel_, state_, generation_, Stage::kReceivingOffer, true) || Canceled()) return std::nullopt;
    base::FilePath approved;
    { base::AutoLock lock(state_->lock); approved = state_->snapshot.receive_directory;
      state_->snapshot.warning_message_id = 0; }
    SetStage(state_, Stage::kTransferring);
    return Canceled() ? std::nullopt : std::make_optional(approved);
  }
  void Begin(uint64_t total) override {
    SetStage(state_, Stage::kTransferring);
    base::AutoLock lock(state_->lock);
    state_->snapshot.total = total;
    state_->snapshot.transferred = 0;
  }
  void Progress(const std::string& path, uint64_t bytes) override {
    base::AutoLock lock(state_->lock);
    state_->snapshot.current_file = path;
    state_->snapshot.transferred += bytes;
  }
 private:
  Channel& channel_;
  std::shared_ptr<State> state_;
  uint64_t generation_;
  base::TimeTicks heartbeat_ = base::TimeTicks::Now();
};

void Run(std::shared_ptr<State> state) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::WILL_BLOCK);
  auto key = LoadIdentity(state->trust);
  if (!key) {
    SetStage(state, Stage::kError);
    return;
  }
  std::string der;
  if (!net::x509_util::CreateSelfSignedCert(key->key(), net::x509_util::DIGEST_SHA256,
      "CN=crPhotos", 1, base::Time::Now() - base::Days(1),
      base::Time::Now() + base::Days(7), {}, &der)) {
    SetStage(state, Stage::kError);
    return;
  }
  const uint8_t* encoded = reinterpret_cast<const uint8_t*>(der.data());
  bssl::UniquePtr<X509> certificate(d2i_X509(nullptr, &encoded, der.size()));
  bssl::UniquePtr<SSL_CTX> context(SSL_CTX_new(TLS_method()));
  if (!certificate || !context ||
      !SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION) ||
      !SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) ||
      !SSL_CTX_use_certificate(context.get(), certificate.get()) ||
      !SSL_CTX_use_PrivateKey(context.get(), key->key())) {
    SetStage(state, Stage::kError);
    return;
  }
  // TLS proves possession of both private keys. Public CA validation is not
  // applicable to local self-signed identities; Pair() requires either an
  // exact stored SPKI pin or explicit SAS confirmation before any manifest.
  SSL_CTX_set_custom_verify(context.get(),
      SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
      [](SSL*, uint8_t*) { return ssl_verify_ok; });
  SSL_CTX_set_options(context.get(), SSL_OP_NO_TICKET);
  SSL_CTX_set_num_tickets(context.get(), 0);
  SSL_CTX_set_alpn_select_cb(context.get(),
      [](SSL*, const uint8_t** out, uint8_t* out_length,
         const uint8_t* in, unsigned in_length, void*) -> int {
        uint8_t* selected = nullptr;
        if (SSL_select_next_proto(&selected, out_length, kAlpn.data(), kAlpn.size(),
                                  in, in_length) != OPENSSL_NPN_NEGOTIATED)
          return SSL_TLSEXT_ERR_ALERT_FATAL;
        *out = selected;
        return SSL_TLSEXT_ERR_OK;
      }, nullptr);
  auto listener = CreateSocket(SOCK_STREAM);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  if (!listener.is_valid() || bind(listener.get(), reinterpret_cast<sockaddr*>(&local),
      sizeof(local)) != 0 || listen(listener.get(), 1) != 0) {
    SetStage(state, Stage::kError);
    return;
  }
  SocketLength length = sizeof(local);
  if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&local), &length) != 0) {
    SetStage(state, Stage::kError);
    return;
  }
  const uint16_t port = ntohs(local.sin_port);
  bssl::UniquePtr<SSL_CTX> identity_context(SSL_CTX_new(TLS_method()));
  const bool identity_ready = identity_context &&
      SSL_CTX_set_min_proto_version(identity_context.get(), TLS1_3_VERSION) &&
      SSL_CTX_set_max_proto_version(identity_context.get(), TLS1_3_VERSION) &&
      SSL_CTX_use_certificate(identity_context.get(), certificate.get()) &&
      SSL_CTX_use_PrivateKey(identity_context.get(), key->key());
  if (identity_ready) {
    SSL_CTX_set_custom_verify(identity_context.get(),
        SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
        [](SSL*, uint8_t*) { return ssl_verify_ok; });
    SSL_CTX_set_options(identity_context.get(), SSL_OP_NO_TICKET);
    SSL_CTX_set_num_tickets(identity_context.get(), 0);
    SSL_CTX_set_alpn_select_cb(identity_context.get(),
        [](SSL*, const uint8_t** out, uint8_t* out_length,
           const uint8_t* in, unsigned in_length, void*) -> int {
          uint8_t* selected = nullptr;
          if (SSL_select_next_proto(&selected, out_length, kIdentityAlpn.data(),
                                    kIdentityAlpn.size(), in, in_length) != OPENSSL_NPN_NEGOTIATED)
            return SSL_TLSEXT_ERR_ALERT_FATAL;
          *out = selected;
          return SSL_TLSEXT_ERR_OK;
        }, nullptr);
  }
  auto identity_listener = CreateSocket(SOCK_STREAM);
  sockaddr_in identity_address{};
  identity_address.sin_family = AF_INET;
  SocketLength identity_size = sizeof(identity_address);
  if (identity_ready && identity_listener.is_valid() &&
      bind(identity_listener.get(), reinterpret_cast<sockaddr*>(&identity_address),
           sizeof(identity_address)) == 0 && listen(identity_listener.get(), 8) == 0 &&
      getsockname(identity_listener.get(), reinterpret_cast<sockaddr*>(&identity_address),
                  &identity_size) == 0) {
    state->identity_port = ntohs(identity_address.sin_port);
    base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::BindOnce(&ServeIdentities, state, std::move(identity_listener),
                       bssl::UpRef(identity_context.get()), port));
  } else {
    LOG(WARNING) << "crPhotos nearby identity listener unavailable";
  }
  if (identity_ready) {
    base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::BindOnce(&ProbeIdentities, state, std::move(identity_context)));
  }
  LOG(INFO) << "crPhotos nearby sharing listening TCP port=" << port;
  {
    base::AutoLock lock(state->lock);
    if (state->stop) return;
    state->snapshot.port = port;
    state->snapshot.stage = Stage::kReady;
  }
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
                           base::BindOnce(&Discovery, state, port));
  base::TimeTicks next_incoming;
  while (!state->stop) {
    std::optional<State::Outgoing> outgoing;
    {
      base::AutoLock lock(state->lock);
      outgoing = std::move(state->outgoing);
      state->outgoing.reset();
    }
    if (!outgoing) {
      std::vector<SyncEndpoint> endpoints;
      {
        base::AutoLock lock(state->lock);
        for (const auto& peer : state->snapshot.peers) {
          auto proof = state->identities.find(IdentityKey(peer));
          if (proof != state->identities.end() && proof->second.sync_v1 &&
              proof->second.expires > base::TimeTicks::Now())
            endpoints.push_back({peer.address, proof->second.fingerprint, proof->second.sync_removal});
        }
      }
      auto request = state->sync->Next(endpoints);
      if (request) {
        base::AutoLock lock(state->lock);
        // Manual sends queued during a database scan take priority.
        if (!state->outgoing && !state->stop) {
          outgoing = State::Outgoing{request->address, {}, state->cancel.load(), {}, std::nullopt, {},
                                     request->id, request->fingerprint, request->removal};
          state->snapshot.session = ++g_session;
          state->snapshot.started_at = base::TimeTicks::Now();
          state->snapshot.finished_at = {};
          state->snapshot.busy = true;
          state->snapshot.receiving = false;
          state->snapshot.syncing = true; state->snapshot.sync_background = true;
          state->snapshot.sync_id = request->id;
          state->snapshot.stage = Stage::kConnecting;
          state->snapshot.status_message_id = 0;
          state->snapshot.total = state->snapshot.transferred = 0;
          state->snapshot.current_file.clear();
        }
      }
    }
    const uint64_t generation = outgoing ? outgoing->generation : state->cancel.load();
    if (outgoing && outgoing->phone) {
      LOG(INFO) << "[crPhotos phone send] worker_dequeued; files=" << outgoing->paths.size();
      SendFilesToPhone(*outgoing->phone, outgoing->paths,
          base::BindRepeating([](std::shared_ptr<State> state, uint64_t generation) {
            return IsCanceled(*state, generation);
          }, state, generation),
          base::BindRepeating([](std::shared_ptr<State> state, uint64_t generation,
                                PhoneSendProgress progress) {
            base::AutoLock lock(state->lock);
            if (state->stop) return;
            const bool finished = progress.stage == Stage::kComplete ||
                progress.stage == Stage::kCanceled || progress.stage == Stage::kError;
            state->snapshot.stage = finished && IsCanceled(*state, generation) ?
                Stage::kCanceled : progress.stage;
            state->snapshot.status_message_id = state->snapshot.stage == Stage::kCanceled ?
                0 : progress.message_id;
            state->snapshot.transferred = progress.transferred;
            state->snapshot.total = progress.total;
            state->snapshot.peer_trusted = progress.verified;
            state->snapshot.busy = !finished;
            if (finished && state->snapshot.finished_at.is_null()) state->snapshot.finished_at = base::TimeTicks::Now();
          }, state, generation));
      continue;
    }
    ScopedSocket fd;
    std::optional<std::vector<SendFile>> files;
    FailureInfo failure;
    if (outgoing) {
      if (!IsCanceled(*state, generation)) files = outgoing->sync_id.empty() ?
          OpenSelectedFiles(outgoing->paths) : std::optional<std::vector<SendFile>>(std::in_place);
      if (!files) FailTransfer(failure, kSourceReadError);
      if (files && !IsCanceled(*state, generation)) {
        failure.message_id = kConnectionError.message_id;
        fd = Connect(outgoing->address, *state, generation, failure);
      }
    } else {
      PollFd pfd{listener.get(), POLLIN, 0};
      if (Poll(&pfd, 1, 200) <= 0) continue;
      sockaddr_in remote{};
      SocketLength remote_size = sizeof(remote);
      fd = Accept(listener.get(), reinterpret_cast<sockaddr*>(&remote), &remote_size);
      if (!fd.is_valid() || !IsPrivateAddress(remote.sin_addr)) {
        LOG(WARNING) << "[crPhotos nearby failure] phase=tcp_accept reason="
                     << (fd.is_valid() ? "non_private_address" : "accept_failed")
                     << " os_error=" << (fd.is_valid() ? 0 : SocketError());
        continue;
      }
      if (base::TimeTicks::Now() < next_incoming) {
        LOG(WARNING) << "[crPhotos nearby failure] phase=tcp_accept reason=rate_limited"
                     << " retry_after_ms="
                     << (next_incoming - base::TimeTicks::Now()).InMilliseconds();
        continue;
      }
      next_incoming = base::TimeTicks::Now() + base::Seconds(5);
    }
    {
      base::AutoLock lock(state->lock);
      // Send() may have queued an outgoing request while accept was waiting.
      if (!outgoing && state->outgoing) {
        LOG(WARNING) << "[crPhotos nearby failure] phase=tcp_accept reason=outgoing_pending";
        continue;
      }
      if (state->stop) break;
      state->snapshot.busy = true;
      if (!outgoing) {
        state->snapshot.session = ++g_session;
        state->snapshot.started_at = base::TimeTicks::Now();
        state->snapshot.finished_at = base::TimeTicks();
      }
      state->snapshot.receive_directory = state->receive_root;
      state->snapshot.receiving = !outgoing;
      state->sync_id = outgoing ? outgoing->sync_id : std::string();
      state->sync_remove = outgoing && outgoing->sync_remove;
      state->sync_fingerprint = outgoing ? outgoing->sync_fingerprint : std::string();
      state->snapshot.syncing = !state->sync_id.empty();
      state->snapshot.sync_id = state->sync_id;
      state->snapshot.sync_background = state->snapshot.syncing;
      state->snapshot.warning_message_id = 0;
      state->snapshot.files.clear();
      state->snapshot.current_file.clear();
      state->snapshot.receive_target.clear(); state->snapshot.receive_target_name.clear();
      state->outgoing_target = outgoing ? outgoing->target_id : std::string();
      state->snapshot.previews.clear();
      state->outgoing_previews = outgoing ? std::move(outgoing->previews) :
          std::map<std::string, std::string>();
      state->snapshot.peer_name.clear();
      state->snapshot.peer_avatar = 0;
      state->snapshot.peer_custom_avatar.clear();
      state->snapshot.peer_trusted = false;
      state->snapshot.skipped_files =
          outgoing && files && outgoing->paths.size() > files->size() ? outgoing->paths.size() - files->size() : 0;
      state->peer_fingerprint.clear();
      state->trust_requested = false;
      state->session_generation = generation;
      state->snapshot.code.clear();
      state->snapshot.batch_directory.clear();
      state->snapshot.total = state->snapshot.transferred = 0;
      state->snapshot.stage = Stage::kConnecting;
      state->snapshot.status_message_id = 0;
      state->snapshot.conflict_id = 0;
    }
    bool ok = false;
    if (fd.is_valid() && !IsCanceled(*state, generation)) {
      Channel channel(std::move(fd), context.get(), !outgoing, state, generation);
      failure.phase = "tls_handshake";
      failure.reason = "handshake_or_alpn_failed";
      failure.message_id = kConnectionError.message_id;
      const bool handshake_ok = channel.Handshake();
      if (handshake_ok) {
        failure.phase = "pairing";
        failure.message_id = 0;
        const bool pair_ok = Pair(channel, state, generation, failure);
        if (pair_ok) {
          failure.phase = "transfer";
          failure.reason = "offer_or_transfer_failed";
          failure.message_id = kProtocolError.message_id;
          failure.notify_peer = !outgoing;  // Sender reads framed replies throughout.
          if (!state->sync_id.empty()) {
            DesktopSyncWire wire(channel, state, generation);
            base::FilePath receive_directory;
            std::string peer_name; int peer_avatar;
            { base::AutoLock lock(state->lock); receive_directory = state->snapshot.receive_directory;
              peer_name = state->snapshot.peer_name; peer_avatar = state->snapshot.peer_avatar; }
            if (state->sync_remove) {
              failure.phase = "sync_removal";
              failure.reason = "sync_removal_not_acknowledged";
              ok = state->sync->RunRemoval(wire, outgoing.has_value(), state->sync_id, state->peer_fingerprint);
            } else {
              ok = state->sync->Run(wire, outgoing.has_value(), state->sync_id,
                  state->peer_fingerprint, peer_name, peer_avatar, receive_directory);
            }
            failure.message_id = IDS_CRFILES_SYNC_RETRY;
          } else {
            ok = outgoing ? SendFiles(channel, std::move(*files), state, generation, failure)
                          : ReceiveFiles(channel, state, generation, failure);
          }
          bool declined;
          { base::AutoLock lock(state->lock); declined = state->snapshot.syncing && state->decision < 0; }
          if (!ok && !declined && failure.notify_peer && !failure.peer_reported &&
              !IsCanceled(*state, generation)) {
            auto error = Message("error");
            error.Set("reason", failure.reason);
            channel.SendJson(std::move(error), false, base::Seconds(2));
          }
        }
      }
      if (!ok) failure.ssl_error = channel.last_ssl_error();
    }
    if (!ok && !state->sync_id.empty()) state->sync->Failed(state->sync_id);
    base::AutoLock lock(state->lock);
    state->snapshot.stage = state->stop ? Stage::kOff : ok ? Stage::kComplete :
        (IsCanceled(*state, generation) || (state->snapshot.syncing && state->decision < 0)) ? Stage::kCanceled : Stage::kError;
    state->snapshot.status_message_id = state->snapshot.stage == Stage::kError ?
        failure.message_id : 0;
    state->snapshot.busy = false;
    state->snapshot.finished_at = base::TimeTicks::Now();
    if (!ok) {
      LOG(ERROR) << "[crPhotos nearby failure] session=" << state->snapshot.session
                 << " receiving=" << state->snapshot.receiving
                 << " phase=" << failure.phase << " reason=" << failure.reason
                 << " canceled=" << IsCanceled(*state, generation)
                 << " peer_reported=" << failure.peer_reported
                 << " os_error=" << failure.os_error
                 << " last_ssl_error=" << failure.ssl_error;
    }
  }
}
#endif  // BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
}  // namespace

NearbySharing::NearbySharing()
    : state_(std::make_shared<State>()), trust_(std::make_shared<TrustStore>()) {
  state_->trust = trust_;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  sync_ = std::make_shared<SyncService>(TrustDirectory().AppendASCII("sync"));
  state_->sync = sync_;
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()}, base::BindOnce(
      [](std::shared_ptr<SyncService> service) { service->Poll(); }, sync_));
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                            base::BindOnce(&LoadTrust, trust_));
  const auto preferences = GetNearbySharingPreferences();
  if (preferences.discovery_enabled) {
    const auto directory = preferences.directory.empty() ? DefaultReceiveDirectory() :
        base::FilePath::FromUTF8Unsafe(preferences.directory);
    Enable(preferences.name, directory, preferences.avatar);
  }
#endif
}
NearbySharing::~NearbySharing() { Disable(); }
std::vector<NearbySharing::LocalAddress> NearbySharing::GetLocalIPv4Addresses() {
  std::vector<LocalAddress> result;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  if (!nearby::InitializeSockets()) return result;
  for (const auto& entry : nearby::LocalInterfaces(false)) {
    const auto address = entry.address;
    if (!IsPrivateAddress(address)) continue;
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address, text.data(), text.size())) {
      InterfaceType type = InterfaceType::kOther;
      if (entry.type == nearby::InterfaceType::kWifi) type = InterfaceType::kWifi;
      if (entry.type == nearby::InterfaceType::kEthernet) type = InterfaceType::kEthernet;
      result.push_back({text.data(), type});
    }
  }
  std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.address < b.address;
  });
  result.erase(std::unique(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.address == b.address;
  }), result.end());
#endif
  return result;
}
base::FilePath NearbySharing::DefaultReceiveDirectory() {
#if BUILDFLAG(IS_MAC)
  return MacDownloadsDirectory().AppendASCII("crFiles Received");
#else
#if BUILDFLAG(IS_WIN)
  PWSTR pictures = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pictures))) {
    const auto directory = base::FilePath(pictures).AppendASCII("crFiles Received");
    CoTaskMemFree(pictures);
    return directory;
  }
#endif
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) return {};
  return home.AppendASCII("Downloads").AppendASCII("crFiles Received");
#endif
}
bool NearbySharing::IsValidConfiguration(std::string_view name,
                                         const base::FilePath& directory) {
#if BUILDFLAG(IS_WIN)
  const auto parts = directory.GetComponents();
  if (parts.size() < 2 || parts.front().size() != 2 ||
      parts.front().back() != L':' || directory.ReferencesParent()) return false;
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  return SafeDeviceName(name) && directory.IsAbsolute();
#else
  return false;
#endif
}

void NearbySharing::Enable(std::string name, base::FilePath directory, int avatar) {
  if (!IsValidConfiguration(name, directory) || GetSnapshot().busy) return;
  Disable();
  state_ = std::make_shared<State>();
  state_->trust = trust_;
  state_->sync = sync_;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  state_->name = std::move(name);
  state_->avatar = avatar >= 0 && avatar < kAvatarCount ? avatar : 0;
  const auto custom_avatar = GetNearbySharingPreferences().custom_avatar;
  std::string pixels;
  if (custom_avatar.size() == 49152 && base::Base64Decode(custom_avatar, &pixels) &&
      pixels.size() == 96 * 96 * 4) state_->custom_avatar = custom_avatar;
  state_->snapshot.avatar = state_->avatar;
  state_->snapshot.device_name = state_->name;
  state_->id = base::ToLowerASCII(base::HexEncode(base::RandBytesAsString(16)));
  state_->snapshot.enabled = true;
  state_->snapshot.stage = Stage::kStarting;
  state_->receive_root = directory;
  state_->snapshot.receive_directory = std::move(directory);
  connect_discovery_ = std::make_unique<ConnectDiscovery>(base::BindRepeating(
      [](std::shared_ptr<State> state, std::vector<ConnectPeer> peers) {
        base::AutoLock lock(state->lock);
        if (!state->stop) state->snapshot.connect_peers = std::move(peers);
      }, state_));
  // Phone sends and discovery verification both destroy a ConnectClient,
  // joining its IO thread. MayBlock permits IO but does not permit that join.
  base::ThreadPool::PostTask(FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&Run, state_));
  base::ThreadPool::PostTask(FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::BEST_EFFORT},
      base::BindOnce(&ProbePhones, state_));
#else
  state_->snapshot.stage = Stage::kError;
#endif
}
void NearbySharing::SetSharedTargets(std::vector<SharedTarget> targets) {
  base::AutoLock lock(state_->lock);
  std::erase_if(targets, [](const auto& target) {
    return target.id.empty() || target.id.size() > 80 || target.name.empty() || target.name.size() > 80 ||
        !target.directory.IsAbsolute() || target.directory.ReferencesParent();
  });
  if (targets.size() > 128) targets.resize(128);
  if (state_->snapshot.busy && state_->snapshot.receiving && !state_->snapshot.receive_target.empty()) {
    auto previous = std::ranges::find(state_->targets, state_->snapshot.receive_target, &SharedTarget::id);
    auto next = std::ranges::find(targets, state_->snapshot.receive_target, &SharedTarget::id);
    if (previous == state_->targets.end() || next == targets.end() || (previous->directory != next->directory || previous->tag_id != next->tag_id)) ++state_->cancel;
  }
  state_->targets = std::move(targets);
}
void NearbySharing::Disable() {
  state_->stop = true;
  ++state_->cancel;
  // Join before locking: the IO callback may be publishing its final snapshot.
  connect_discovery_.reset();
  base::AutoLock lock(state_->lock);
  state_->snapshot.enabled = false;
  state_->snapshot.discovery_ready = false;
#if BUILDFLAG(IS_MAC)
  state_->snapshot.discovery_message_id = 0;
#endif
  state_->snapshot.busy = false;
  state_->snapshot.port = 0;
  state_->snapshot.stage = Stage::kOff;
  state_->snapshot.status_message_id = 0;
  state_->snapshot.peers.clear();
  state_->snapshot.connect_peers.clear();
}
NearbySharing::Snapshot NearbySharing::GetSnapshot() const {
  Snapshot snapshot;
  {
    base::AutoLock lock(state_->lock);
    snapshot = state_->snapshot;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    // Read current identity proofs instead of waiting for the next mDNS tick.
    const auto now = base::TimeTicks::Now();
    for (auto& peer : snapshot.peers) {
      const auto identity = state_->identities.find(IdentityKey(peer));
      if (identity != state_->identities.end() && identity->second.expires > now) {
        peer.verified_fingerprint = identity->second.fingerprint;
        peer.identity_expires = identity->second.expires;
        peer.folder_transfer = identity->second.folder_transfer;
        peer.sync_v1 = identity->second.sync_v1;
        peer.targets = identity->second.targets;
      } else {
        peer.verified_fingerprint.clear(); peer.identity_expires = {};
        peer.folder_transfer = false; peer.sync_v1 = false; peer.targets.clear();
      }
    }
#endif
  }
  base::AutoLock lock(trust_->lock);
  snapshot.trust_error = trust_->error;
  for (auto& peer : snapshot.peers) {
    peer.trusted = !peer.verified_fingerprint.empty() &&
        peer.identity_expires > base::TimeTicks::Now() && !trust_->error &&
        !trust_->revoked.contains(peer.verified_fingerprint) &&
        std::ranges::any_of(trust_->devices, [&](const auto& device) {
          return device.fingerprint == peer.verified_fingerprint;
        });
  }
  for (const auto& phone : snapshot.connect_peers) {
    if (phone.legacy_service || !std::ranges::contains(phone.capabilities, "receive-v1")) continue;
    Peer peer;
    peer.id = phone.id;
    peer.name = phone.name;
    peer.avatar = phone.avatar;
    peer.address = phone.address + ":" + base::NumberToString(phone.port);
    peer.phone = phone;
    peer.trusted = ConnectClient::IsVerifiedPeer(phone);
    snapshot.peers.push_back(std::move(peer));
  }
  // Collapse repeated advertisements without confusing HTTPS and desktop
  // transport endpoints, or merging different devices that share a name.
  std::vector<Peer> unique;
  for (auto& peer : snapshot.peers) {
    const auto duplicate = std::ranges::find_if(unique, [&](const Peer& item) {
      if (item.phone.has_value() != peer.phone.has_value()) return false;
      return item.address == peer.address || (!peer.id.empty() && item.id == peer.id) ||
          (!peer.verified_fingerprint.empty() && peer.identity_expires > base::TimeTicks::Now() &&
           item.identity_expires > base::TimeTicks::Now() && item.verified_fingerprint == peer.verified_fingerprint);
    });
    if (duplicate == unique.end()) unique.push_back(std::move(peer));
    else if (peer.identity_expires > duplicate->identity_expires) *duplicate = std::move(peer);
  }
  snapshot.peers = std::move(unique);
  return snapshot;
}
void NearbySharing::CancelSync(const std::string& id) {
  base::AutoLock lock(state_->lock);
  if (state_->snapshot.busy && state_->snapshot.syncing && state_->snapshot.sync_id == id) ++state_->cancel;
}
SyncStartResult NearbySharing::StartSync(const Peer& peer, const base::FilePath& source, bool write,
                                    const std::string& retry_id) {
  const auto fail = [&](const char* stage, int error) -> SyncStartResult {
    LOG(ERROR) << "[crFiles sync] start_failed stage=" << stage << " retry_id=" << retry_id;
    return {{}, error};
  };
  if (!sync_) return fail("service_unavailable", IDS_CRFILES_SYNC_REQUEST_ERROR);
  const auto snapshot = GetSnapshot();
  if (!snapshot.enabled) return fail("sharing_disabled", IDS_CRFILES_NEARBY_OFF);
  if (!peer.sync_v1 || peer.phone) return fail("peer_unsupported", IDS_CRFILES_SYNC_DEVICE_ERROR);
  const auto current = std::ranges::find_if(snapshot.peers, [&](const auto& value) {
    return value.address == peer.address && value.sync_v1 && value.identity_expires > base::TimeTicks::Now() &&
        value.verified_fingerprint == peer.verified_fingerprint;
  });
  if (current == snapshot.peers.end()) return fail("peer_identity_expired_or_changed", IDS_CRFILES_SYNC_DEVICE_ERROR);
  if (!retry_id.empty()) {
    const auto result = sync_->Retry(retry_id, current->verified_fingerprint, write);
    if (!result.id.empty()) CancelSync(retry_id);
    return result;
  }
  return sync_->Queue(source, current->verified_fingerprint, current->name, current->avatar, write);
}

bool NearbySharing::Send(std::string address, std::vector<base::FilePath> paths,
                        std::map<base::FilePath, gfx::ImageSkia> previews,
                        std::optional<ConnectPeer> phone, std::string target_id) {
  base::AutoLock lock(state_->lock);
  if (!state_->snapshot.enabled || !state_->snapshot.port || state_->snapshot.busy ||
      paths.empty() || paths.size() > kMaximumFiles) {
    return false;
  }
  if (phone && (phone->legacy_service ||
      !std::ranges::contains(phone->capabilities, "receive-v1") ||
      address != phone->address + ":" + base::NumberToString(phone->port) ||
      !std::ranges::contains(state_->snapshot.connect_peers, *phone))) return false;
  state_->snapshot.busy = true;
  state_->snapshot.started_at = base::TimeTicks::Now();
  state_->snapshot.finished_at = base::TimeTicks();
  std::map<std::string, std::string> encoded_previews;
  std::set<std::string> preview_names;
  for (const auto& path : paths) {
    if (phone) break;
    const auto name = path.BaseName().AsUTF8Unsafe();
    if (!preview_names.insert(base::ToLowerASCII(name)).second) continue;
    if (preview_names.size() > 5) break;
    const auto found = previews.find(path);
    if (found == previews.end() || found->second.isNull() ||
        found->second.width() <= 0 || found->second.height() <= 0) continue;
    const auto image = gfx::ImageSkiaOperations::CreateCroppedCenteredRoundRectImage(
        gfx::Size(96, 96), 0, found->second);
    const auto* bitmap = image.bitmap();
    std::vector<uint8_t> pixels(96 * 96 * 4);
    const auto info = SkImageInfo::Make(96, 96, kRGBA_8888_SkColorType,
                                       kUnpremul_SkAlphaType);
    if (bitmap && bitmap->readPixels(info, pixels.data(), 96 * 4, 0, 0))
      encoded_previews.emplace(name, base::Base64Encode(pixels));
  }
  // Allocate before queueing so UI cancellation is scoped even during Connect().
  state_->snapshot.session = ++g_session;
  state_->snapshot.receiving = false;
  state_->snapshot.syncing = false; state_->snapshot.sync_background = false;
  state_->snapshot.sync_id.clear();
  state_->snapshot.current_file.clear();
  state_->snapshot.peer_name = phone ? phone->name : std::string();
  state_->snapshot.peer_avatar = phone ? phone->avatar : 0;
  state_->snapshot.peer_custom_avatar.clear();
  state_->snapshot.peer_trusted = false;
  state_->snapshot.total = state_->snapshot.transferred = 0;
  state_->snapshot.skipped_files = 0;
  state_->snapshot.batch_directory.clear();
  state_->snapshot.files.clear();
  state_->snapshot.previews.clear();
  state_->snapshot.stage = Stage::kConnecting;
  state_->snapshot.status_message_id = 0;
  LOG(INFO) << "[crPhotos nearby trace] send_queued; session=" << state_->snapshot.session
            << " phone=" << phone.has_value() << " files=" << paths.size();
  state_->outgoing = State::Outgoing{std::move(address), std::move(paths),
                                     state_->cancel.load(), std::move(encoded_previews), std::move(phone), std::move(target_id)};
  return true;
}
bool NearbySharing::SetSyncReceiveDirectory(uint64_t session, const base::FilePath& parent) {
  base::AutoLock lock(state_->lock);
  const auto& snapshot = state_->snapshot;
  if (!snapshot.busy || !snapshot.receiving || !snapshot.syncing || snapshot.session != session ||
      snapshot.stage != Stage::kReceivingOffer || state_->decision != 0 ||
      !IsSyncId(state_->sync_id) || parent.empty() || !parent.IsAbsolute() || parent.ReferencesParent()) return false;
  if (snapshot.sync_receive_name.empty()) return false;
  state_->snapshot.receive_directory = parent.Append(base::FilePath::FromUTF8Unsafe(snapshot.sync_receive_name));
  state_->snapshot.batch_directory = snapshot.sync_receive_file ? parent : state_->snapshot.receive_directory;
  state_->snapshot.warning_message_id = 0;
  return true;
}
void NearbySharing::Decide(uint64_t session, Stage prompt, bool accept, bool trust,
                          uint64_t conflict_id) {
  base::AutoLock lock(state_->lock);
  if (state_->snapshot.syncing && prompt == Stage::kReceivingOffer &&
      (conflict_id == 0 || conflict_id != state_->snapshot.conflict_id)) return;
  if (state_->snapshot.session == session && state_->snapshot.stage == prompt &&
      (prompt == Stage::kPairing || prompt == Stage::kReceivingOffer ||
       (prompt == Stage::kReceivingConflict && conflict_id != 0 &&
        conflict_id == state_->snapshot.conflict_id))) {
    state_->decision = accept ? 1 : -1;
    if (prompt == Stage::kPairing) state_->trust_requested = accept && trust;
    // Sync must deliver its negative acknowledgement before closing the channel.
    // AwaitDecision still returns false, so rejecting never authorizes file I/O.
    if (!accept && !(state_->snapshot.syncing &&
        (prompt == Stage::kPairing || prompt == Stage::kReceivingOffer))) ++state_->cancel;
  }
}
void NearbySharing::Cancel() { ++state_->cancel; }
void NearbySharing::CancelSession(uint64_t session) {
  std::string sync_id;
  {
    base::AutoLock lock(state_->lock);
    if (state_->snapshot.busy && state_->snapshot.session == session) {
      ++state_->cancel;
      if (state_->snapshot.syncing) sync_id = state_->snapshot.sync_id;
    }
  }
  if (sync_ && !sync_id.empty()) sync_->Pause(std::move(sync_id), true);
}
std::vector<NearbySharing::TrustedDevice> NearbySharing::GetTrustedDevices() const {
  base::AutoLock lock(trust_->lock);
  auto devices = trust_->devices;
  std::erase_if(devices, [&](const auto& device) {
    return trust_->revoked.contains(device.fingerprint);
  });
  return devices;
}
void NearbySharing::ForgetTrustedDevice(std::string fingerprint) {
  if (sync_) sync_->Revoke(fingerprint);
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  {
    base::AutoLock lock(trust_->lock);
    trust_->revoked.insert(fingerprint);
    trust_->error = false;
  }
  // Revocation also cancels an active session; no further sends are authorized
  // on an already authenticated connection after the user revokes trust.
  Cancel();
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
      base::BindOnce([](std::shared_ptr<TrustStore> store, std::shared_ptr<SyncService> sync, std::string fingerprint) {
        if (sync) sync->Poll();
        base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
        base::AutoLock io_lock(store->io_lock);
        const auto directory = TrustDirectory();
        auto fd = LockTrustDirectory(directory);
        std::vector<TrustedDevice> devices;
        if (!fd.is_valid() || !ReadTrustRecords(directory, &devices)) {
          base::AutoLock lock(store->lock);
          store->error = true;
          return;
        }
        std::erase_if(devices, [&](const auto& device) {
          return device.fingerprint == fingerprint;
        });
        if (WriteTrustRecords(directory, devices)) {
          PublishTrust(store, std::move(devices));
        } else {
          base::AutoLock lock(store->lock);
          store->error = true;
        }
      }, trust_, sync_, std::move(fingerprint)));
#endif
}
}  // namespace crfiles
