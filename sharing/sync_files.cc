// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crFiles/sharing/sync_files.h"

#include <array>
#include <algorithm>
#include <vector>
#include <limits>
#include <set>
#include <utility>
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/i18n/case_conversion.h"
#include "base/i18n/icu_string_conversions.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#if BUILDFLAG(IS_WIN)
#include "crFiles/sharing/nearby_files_win.h"
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "base/files/scoped_file.h"
#endif

namespace crfiles {
namespace {
constexpr uint64_t kMaxSize = 16ULL * 1024 * 1024 * 1024;
constexpr size_t kMaxEntries = 100000;
constexpr char kPrivate[] = ".crfiles-sync";
std::vector<std::string> Parts(const std::string& name) {
  return base::SplitString(name, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
}
bool Component(const std::string& name) {
  if (name.empty() || name.size() > 240 || name == "." || name == ".." ||
      name.back() == '.' || name.back() == ' ' || !base::IsStringUTF8(name)) return false;
  for (char16_t c : base::UTF8ToUTF16(name)) {
    if (c < 32 || (c >= 0x7f && c <= 0x9f) || c == 0x061c || c == 0x200e || c == 0x200f ||
        (c >= 0x2028 && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069)) return false;
  }
  if (name.find_first_of("/\\:*?\"<>|") != std::string::npos) return false;
  const auto stem = base::ToUpperASCII(name.substr(0, name.find('.')));
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
      stem == "CONIN$" || stem == "CONOUT$" || stem == "CLOCK$") return false;
  if (stem.starts_with("COM") || stem.starts_with("LPT")) {
    const auto suffix = stem.substr(3);
    if ((suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
        suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3") return false;
  }
  return true;
}
bool HashFile(base::File& file, SyncContent* content, const std::function<bool()>& canceled) {
  base::File::Info before, after;
  if (!file.IsValid() || !file.GetInfo(&before) || before.is_directory || before.is_symbolic_link ||
      before.size < 0 || static_cast<uint64_t>(before.size) > kMaxSize) return false;
  crypto::hash::Hasher hash(crypto::hash::kSha256);
  std::vector<uint8_t> buffer(256 * 1024);
  uint64_t remaining = before.size;
  while (remaining) {
    if (canceled()) return false;
    auto bytes = base::span(buffer).first(static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size())));
    if (!file.ReadAtCurrentPosAndCheck(bytes)) return false;
    hash.Update(bytes); remaining -= bytes.size();
  }
  if (!file.GetInfo(&after) || before.size != after.size || before.last_modified != after.last_modified) return false;
  std::array<uint8_t, 32> digest;
  hash.Finish(digest);
  *content = {1, static_cast<uint64_t>(before.size), base::HexEncode(digest)};
  return true;
}
}  // namespace

std::string CanonicalSyncPath(const std::string& path) {
  if (!base::IsStringUTF8(path)) return {};
  if (base::IsStringASCII(path)) return path;
  std::string normalized;
  return base::ConvertToUtf8AndNormalize(path, "UTF-8", &normalized) ? normalized : std::string();
}
bool SafeSyncPath(const std::string& path) {
  if (path.empty() || path.size() > 4096 || CanonicalSyncPath(path) != path) return false;
  const auto parts = Parts(path);
  if (parts.size() > 64 || base::EqualsCaseInsensitiveASCII(parts.front(), kPrivate) ||
      base::EqualsCaseInsensitiveASCII(parts.front(), ".crphotos-lock")) return false;
  for (const auto& part : parts) if (!Component(part)) return false;
  return true;
}
std::string SyncHash(const std::string& bytes) {
  return base::HexEncode(crypto::hash::Sha256(base::as_byte_span(bytes)));
}

class SyncFiles::Impl {
 public:
  Impl(base::FilePath root, std::string id, std::function<bool()> canceled)
      : root(std::move(root)), id(std::move(id)), canceled(std::move(canceled)) {}
  base::FilePath root;
  std::string id;
  std::function<bool()> canceled;
  std::string identity;
#if BUILDFLAG(IS_WIN)
  nearby::DirectoryLock lock;
  struct Parent { nearby::PinnedDirectory pin; base::FilePath path; std::string leaf; };
#else
  base::ScopedFD root_fd;
  dev_t device = 0;
  struct Parent { base::ScopedFD fd; std::string leaf; };
#endif
  // Wire paths use NFC. On a filesystem preserving decomposed names, resolve
  // the existing spelling without renaming it. Scans reject canonical aliases.
#if BUILDFLAG(IS_WIN)
  bool Pin(const base::FilePath& directory, nearby::PinnedDirectory* pin) {
    const auto attributes = GetFileAttributesW(directory.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || !pin->Open(directory)) {
      SetLastError(ERROR_ACCESS_DENIED); return false;
    }
    return true;
  }
  bool Spelling(const base::FilePath& directory, const std::string& name, std::string* result) {
    *result = name;
    const auto exact = directory.Append(base::FilePath::FromUTF8Unsafe(name));
    if (GetFileAttributesW(exact.value().c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(directory.Append(L"*").value().c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool found = false, ok = true;
    do {
      const auto raw = base::WideToUTF8(data.cFileName);
      if (CanonicalSyncPath(raw) == name) {
        if (found) { ok = false; break; }
        found = true; *result = raw;
      }
    } while (FindNextFileW(search, &data));
    ok = ok && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search); return ok;
  }
#else
  bool Spelling(int directory, const std::string& name, std::string* result) {
    *result = name;
    struct stat info;
    if (fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) return true;
    if (errno != ENOENT) return false;
    base::ScopedFD scan(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!scan.is_valid()) return false;
    DIR* stream = fdopendir(scan.release());
    if (!stream) return false;
    bool found = false, ok = true;
    for (;;) {
      errno = 0;
      const auto* entry = readdir(stream);
      if (!entry) { ok = errno == 0; break; }
      const std::string raw(entry->d_name);
      if (CanonicalSyncPath(raw) == name) {
        if (found) { ok = false; break; }
        found = true; *result = raw;
      }
    }
    closedir(stream); return ok;
  }
#endif
  bool ParentOf(const std::string& path, Parent* parent) {
    const auto parts = Parts(path);
    parent->leaf = parts.back();
    const bool internal = path == kPrivate || path.starts_with(std::string(kPrivate) + "/");
#if BUILDFLAG(IS_WIN)
    parent->path = root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      nearby::PinnedDirectory pin;
      std::string spelling = parts[i];
      if (!Pin(parent->path, &pin) || (!internal && !Spelling(parent->path, parts[i], &spelling))) return false;
      parent->path = parent->path.Append(base::FilePath::FromUTF8Unsafe(spelling));
    }
    return Pin(parent->path, &parent->pin) && (internal || Spelling(parent->path, parts.back(), &parent->leaf));
#else
    parent->fd.reset(dup(root_fd.get()));
    for (size_t i = 0; parent->fd.is_valid() && i + 1 < parts.size(); ++i) {
      std::string spelling = parts[i];
      if (!internal && !Spelling(parent->fd.get(), parts[i], &spelling)) return false;
      parent->fd.reset(openat(parent->fd.get(), spelling.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    return parent->fd.is_valid() && (internal || Spelling(parent->fd.get(), parts.back(), &parent->leaf));
#endif
  }
  bool Mkdir(const std::string& path) {
    Parent p;
    if (!ParentOf(path, &p)) {
      PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=directory_parent root=" << root << " path=" << path;
      return false;
    }
#if BUILDFLAG(IS_WIN)
    const auto full = p.path.Append(base::FilePath::FromUTF8Unsafe(p.leaf));
    if (!nearby::CreatePrivateDirectory(full) && GetLastError() != ERROR_ALREADY_EXISTS) {
      PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=directory_create path=" << full;
      return false;
    }
    nearby::PinnedDirectory pin;
    if (pin.Open(full)) return true;
    PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=directory_pin path=" << full;
    return false;
#else
    if (mkdirat(p.fd.get(), p.leaf.c_str(), 0700) != 0 && errno != EEXIST) {
      PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=directory_create root=" << root << " path=" << path;
      return false;
    }
    base::ScopedFD child(openat(p.fd.get(), p.leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!child.is_valid()) {
      PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=directory_open root=" << root << " path=" << path;
      return false;
    }
    if (fsync(p.fd.get()) == 0) return true;
    PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=directory_flush root=" << root << " path=" << path;
    return false;
#endif
  }
  bool PrivateDirectory(const std::string& path) {
    Parent parent;
    if (!ParentOf(path, &parent)) {
      PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=private_parent root=" << root << " path=" << path;
      return false;
    }
#if BUILDFLAG(IS_WIN)
    if (nearby::ProtectPrivateDirectory(parent.path.Append(base::FilePath::FromUTF8Unsafe(parent.leaf)))) return true;
    PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=private_permissions root=" << root << " path=" << path;
    return false;
#else
    base::ScopedFD fd(openat(parent.fd.get(), parent.leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info;
    if (!fd.is_valid() || fstat(fd.get(), &info) != 0) {
      PLOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=private_stat root=" << root << " path=" << path;
      return false;
    }
    if (info.st_uid == geteuid() && info.st_dev == device && (info.st_mode & 0077) == 0) return true;
    // Policy failures have no errno: report actual metadata instead of a stale OS error.
    LOG(ERROR) << "[crFiles sync] prepare_failed id=" << id << " stage=private_permissions root=" << root
               << " path=" << path << " owner=" << info.st_uid << " expected_owner=" << geteuid()
               << " mode=" << (info.st_mode & 07777) << " device=" << info.st_dev << " expected_device=" << device;
    return false;
#endif
  }
  std::string Private(const std::string& operation, const char* suffix) const {
    return std::string(kPrivate) + "/" + id + "/" + operation + suffix;
  }
  bool Read(const std::string& path, SyncContent* content) {
    *content = {};
    Parent p;
    if (!ParentOf(path, &p)) {
      // Only a missing ancestor is absence. Access errors and links are errors.
#if BUILDFLAG(IS_WIN)
      return GetLastError() == ERROR_PATH_NOT_FOUND || GetLastError() == ERROR_FILE_NOT_FOUND;
#else
      return errno == ENOENT;
#endif
    }
#if BUILDFLAG(IS_WIN)
    const auto full = p.path.Append(base::FilePath::FromUTF8Unsafe(p.leaf));
    const auto attributes = GetFileAttributesW(full.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
      return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) { content->kind = 2; return true; }
#else
    struct stat info;
    if (fstatat(p.fd.get(), p.leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
    if (info.st_dev != device) return false;
    if (S_ISDIR(info.st_mode)) { content->kind = 2; return true; }
    if (!S_ISREG(info.st_mode)) return false;
#endif
    auto file = OpenFile(path, false);
    return HashFile(file, content, canceled);
  }
  base::File OpenFile(const std::string& path, bool create) {
    Parent p;
    if (!ParentOf(path, &p)) return {};
#if BUILDFLAG(IS_WIN)
    return nearby::OpenRegularFile(p.path.Append(base::FilePath::FromUTF8Unsafe(p.leaf)), create);
#else
    base::File file(openat(p.fd.get(), p.leaf.c_str(), O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
        (create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY), 0600));
    struct stat info;
    if (!file.IsValid() || fstat(file.GetPlatformFile(), &info) != 0 || !S_ISREG(info.st_mode)) return {};
    return file;
#endif
  }
  bool List(const std::string& directory, std::vector<std::string>* names) {
#if BUILDFLAG(IS_WIN)
    auto path = root;
    Parent parent;
    if (!directory.empty()) {
      if (!ParentOf(directory, &parent)) return false;
      path = parent.path.Append(base::FilePath::FromUTF8Unsafe(parent.leaf));
    }
    nearby::PinnedDirectory pin;
    if (!Pin(path, &pin)) return false;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(path.Append(L"*").value().c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
      const auto name = base::WideToUTF8(data.cFileName);
      if (name != "." && name != "..") names->push_back(name);
      if (canceled()) { ok = false; break; }
    } while (FindNextFileW(search, &data));
    ok = ok && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    return ok;
#else
    base::ScopedFD fd;
    if (directory.empty()) fd.reset(openat(root_fd.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    else {
      Parent p;
      if (!ParentOf(directory, &p)) return false;
      fd.reset(openat(p.fd.get(), p.leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    if (!fd.is_valid()) return false;
    DIR* stream = fdopendir(fd.release());
    if (!stream) return false;
    bool ok = true;
    for (;;) {
      errno = 0;
      auto* entry = readdir(stream);
      if (!entry) { ok = errno == 0; break; }
      std::string name(entry->d_name);
      if (name != "." && name != "..") names->push_back(std::move(name));
      if (canceled()) { ok = false; break; }
    }
    closedir(stream);
    return ok;
#endif
  }
  bool Walk(const std::string& directory, std::map<std::string, SyncContent>* files) {
    std::vector<std::string> names;
    if (!List(directory, &names)) return false;
    std::set<std::u16string> unique;
    for (const auto& raw_name : names) {
      const auto name = CanonicalSyncPath(raw_name);
      if (directory.empty() && (name == kPrivate || name == ".crphotos-lock")) continue;
      const auto path = directory.empty() ? name : directory + "/" + name;
      if (!SafeSyncPath(path) || !unique.insert(base::i18n::FoldCase(base::UTF8ToUTF16(name))).second ||
          files->size() >= kMaxEntries || canceled()) return false;
      SyncContent content;
      if (!Read(path, &content) || !content.kind) return false;
      files->emplace(path, content);
      if (content.kind == 2 && !Walk(path, files)) return false;
    }
    return true;
  }
  bool Move(const std::string& from, const std::string& to) {
    Parent source, dest;
    if (!ParentOf(from, &source) || !ParentOf(to, &dest)) return false;
#if BUILDFLAG(IS_WIN)
    return MoveFileExW(source.path.Append(base::FilePath::FromUTF8Unsafe(source.leaf)).value().c_str(),
        dest.path.Append(base::FilePath::FromUTF8Unsafe(dest.leaf)).value().c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    // The recovery name is a fresh random operation ID, in a private directory.
    struct stat info;
    if (fstatat(dest.fd.get(), dest.leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) return false;
    return renameat(source.fd.get(), source.leaf.c_str(), dest.fd.get(), dest.leaf.c_str()) == 0 &&
        fsync(source.fd.get()) == 0 && fsync(dest.fd.get()) == 0;
#endif
  }
  bool Install(const std::string& stage, const std::string& path) {
    Parent source, dest;
    if (!ParentOf(stage, &source) || !ParentOf(path, &dest)) return false;
#if BUILDFLAG(IS_WIN)
    return Move(stage, path);
#else
    if (linkat(source.fd.get(), source.leaf.c_str(), dest.fd.get(), dest.leaf.c_str(), 0) != 0) return false;
    if (fsync(dest.fd.get()) != 0) return false;
    return unlinkat(source.fd.get(), source.leaf.c_str(), 0) == 0 && fsync(source.fd.get()) == 0;
#endif
  }
};

SyncFiles::SyncFiles(base::FilePath root, std::string id, std::function<bool()> canceled)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(id), std::move(canceled))) {}
SyncFiles::~SyncFiles() = default;
bool SyncFiles::Open() {
  auto& p = *impl_;
  const auto fail = [&](const char* stage) {
    PLOG(ERROR) << "[crFiles sync] open_failed id=" << p.id << " stage=" << stage << " root=" << p.root;
    return false;
  };
  if (!p.root.IsAbsolute() || p.root.ReferencesParent()) {
    LOG(ERROR) << "[crFiles sync] open_failed id=" << p.id << " stage=invalid_root root=" << p.root;
    return false;
  }
#if BUILDFLAG(IS_WIN)
  if (!p.lock.Open(p.root)) return fail("lock_open");
  if (!p.lock.TryLock()) return fail("root_lock");
  base::win::ScopedHandle handle(CreateFileW(p.root.value().c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!handle.is_valid()) return fail("root_open");
  if (!GetFileInformationByHandle(handle.get(), &info)) return fail("root_stat");
  p.identity = base::NumberToString(info.dwVolumeSerialNumber) + ":" +
      base::NumberToString(info.nFileIndexHigh) + ":" + base::NumberToString(info.nFileIndexLow);
#else
  p.root_fd.reset(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!p.root_fd.is_valid()) return fail("filesystem_root_open");
  for (const auto& part : p.root.GetComponents()) {
    if (part == "/") continue;
    const int child = openat(p.root_fd.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0) {
      PLOG(ERROR) << "[crFiles sync] open_failed id=" << p.id << " stage=root_component root=" << p.root << " component=" << part;
      return false;
    }
    p.root_fd.reset(child);
  }
  struct stat info;
  if (flock(p.root_fd.get(), LOCK_EX | LOCK_NB) != 0) return fail("root_lock");
  if (fstat(p.root_fd.get(), &info) != 0) return fail("root_stat");
  p.device = info.st_dev;
  p.identity = base::NumberToString(info.st_dev) + ":" + base::NumberToString(info.st_ino);
#endif
  const auto recovery = std::string(kPrivate) + "/" + p.id;
  return p.Mkdir(kPrivate) && p.PrivateDirectory(kPrivate) && p.Mkdir(recovery) && p.PrivateDirectory(recovery);
}
std::string SyncFiles::Identity() const { return impl_->identity; }
std::optional<bool> SyncFiles::ReceiveNameAvailable(const std::string& name, bool single_file) {
  if (!SafeSyncPath(name) || name.find('/') != std::string::npos) return std::nullopt;
  std::vector<std::string> names;
  if (!impl_->List({}, &names)) return std::nullopt;
  const auto fold = [](const std::string& value) {
    return base::i18n::FoldCase(base::UTF8ToUTF16(CanonicalSyncPath(value)));
  };
  const auto selected = fold(name);
  const auto conflicts = fold("sync-conflict-" + SyncHash(name).substr(0, 16) + "-");
  for (const auto& existing : names) {
    const auto folded = fold(existing);
    if (folded == selected || (single_file && folded.starts_with(conflicts))) return false;
  }
  return true;
}
bool SyncFiles::CreateReceiveDirectory(const std::string& name) {
  if (!SafeSyncPath(name) || name.find('/') != std::string::npos || impl_->canceled()) return false;
#if BUILDFLAG(IS_WIN)
  return nearby::CreatePrivateDirectory(impl_->root.Append(base::FilePath::FromUTF8Unsafe(name)));
#else
  return mkdirat(impl_->root_fd.get(), name.c_str(), 0700) == 0 && fsync(impl_->root_fd.get()) == 0;
#endif
}
base::FilePath SyncFiles::Recovery() const { return impl_->root.AppendASCII(kPrivate).AppendASCII(impl_->id); }
bool SyncFiles::Read(const std::string& path, SyncContent* content) {
  return SafeSyncPath(path) && impl_->Read(path, content);
}
bool SyncFiles::Scan(const std::string& single_file, std::map<std::string, SyncContent>* files) {
  if (single_file.empty()) return impl_->Walk({}, files);
  SyncContent value;
  if (!Read(single_file, &value) || value.kind == 2) return false;
  if (value.kind) files->emplace(single_file, value);
  // Conflict copies for a single-file relationship are part of that relationship.
  std::vector<std::string> names;
  if (!impl_->List({}, &names)) return false;
  const std::string prefix = "sync-conflict-" + SyncHash(single_file).substr(0, 16) + "-";
  for (const auto& name : names) if (name.starts_with(prefix)) {
    if (!Read(name, &value) || value.kind != 1) return false;
    files->emplace(name, value);
  }
  return true;
}
base::File SyncFiles::OpenFile(const std::string& path) {
  return SafeSyncPath(path) ? impl_->OpenFile(path, false) : base::File();
}
base::File SyncFiles::CreateStage(const std::string& operation) {
  return impl_->OpenFile(impl_->Private(operation, ".part"), true);
}
base::File SyncFiles::OpenStage(const std::string& operation) {
  return impl_->OpenFile(impl_->Private(operation, ".part"), false);
}
void SyncFiles::DiscardStage(const std::string& operation) {
  Impl::Parent p;
  if (!impl_->ParentOf(impl_->Private(operation, ".part"), &p)) return;
#if BUILDFLAG(IS_WIN)
  DeleteFileW(p.path.Append(base::FilePath::FromUTF8Unsafe(p.leaf)).value().c_str());
#else
  unlinkat(p.fd.get(), p.leaf.c_str(), 0);
#endif
}
bool SyncFiles::ArchiveAbsent(const std::string& operation) {
  SyncContent archived;
  return impl_->Read(impl_->Private(operation, ".saved"), &archived) && !archived.kind;
}
bool SyncFiles::Record(const std::string& operation, const std::string& json) {
  auto file = impl_->OpenFile(impl_->Private(operation, ".json"), true);
  if (!file.IsValid() || !file.WriteAtCurrentPosAndCheck(base::as_byte_span(json)) || !file.Flush()) return false;
#if !BUILDFLAG(IS_WIN)
  Impl::Parent parent;
  return impl_->ParentOf(impl_->Private(operation, ".json"), &parent) && fsync(parent.fd.get()) == 0;
#else
  return true;
#endif
}
bool SyncFiles::VerifyStage(const std::string& operation, const SyncContent& expected) {
  auto file = OpenStage(operation);
  SyncContent actual;
  return HashFile(file, &actual, impl_->canceled) && actual == expected;
}
bool SyncFiles::CopyStage(const std::string& path, const std::string& operation, const SyncContent& expected) {
  auto input = OpenFile(path);
  auto output = CreateStage(operation);
  if (!input.IsValid() || !output.IsValid()) return false;
  std::vector<uint8_t> buffer(256 * 1024);
  uint64_t remaining = expected.size;
  while (remaining) {
    auto bytes = base::span(buffer).first(static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size())));
    if (impl_->canceled() || !input.ReadAtCurrentPosAndCheck(bytes) || !output.WriteAtCurrentPosAndCheck(bytes)) return false;
    remaining -= bytes.size();
  }
  if (!output.Flush()) return false;
  output.Close(); input.Close();
  SyncContent current;
  return Read(path, &current) && current == expected && VerifyStage(operation, expected);
}
bool SyncFiles::Apply(const std::string& path, const std::string& operation,
                      const SyncContent& before, const SyncContent& after) {
  if (!SafeSyncPath(path) || impl_->canceled()) return false;
  SyncContent current, archived;
  if (!Read(path, &current)) return false;
  if (current == after) return true;
  const auto archive = impl_->Private(operation, ".saved");
  if (!impl_->Read(archive, &archived)) return false;
  if (current.kind) {
    if (current != before || archived.kind) return false;
    if (current.kind == 2) {
      std::vector<std::string> children;
      if (!impl_->List(path, &children) || !children.empty()) return false;
    }
    if (!impl_->Move(path, archive) || !impl_->Read(archive, &archived) || archived != before) return false;
  } else if (before.kind && archived != before) return false;
  if (archived.kind == 2) {
    std::vector<std::string> children;
    if (!impl_->List(archive, &children) || !children.empty()) return false;
  }
  if (!after.kind) return true;
  if (after.kind == 2) return impl_->Mkdir(path);
  if (!VerifyStage(operation, after)) return false;
  return impl_->Install(impl_->Private(operation, ".part"), path);
}
}  // namespace crfiles
