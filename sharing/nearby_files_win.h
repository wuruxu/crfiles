// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_SHARING_NEARBY_FILES_WIN_H_
#define CRFILES_SHARING_NEARBY_FILES_WIN_H_

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <optional>
#include <vector>
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/win/access_token.h"
#include "base/win/scoped_handle.h"
#include "base/win/sid.h"

namespace crfiles::nearby {

inline bool IsRegularHandle(HANDLE handle, bool directory) {
  BY_HANDLE_FILE_INFORMATION info{};
  return GetFileType(handle) == FILE_TYPE_DISK &&
      GetFileInformationByHandle(handle, &info) &&
      !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

// Pin every directory without FILE_SHARE_DELETE. Reject junctions, symlinks,
// device paths and UNC paths. Path-based creation below these handles cannot
// be redirected by renaming/replacing an ancestor during a transfer.
class PinnedDirectory {
 public:
  PinnedDirectory();
  ~PinnedDirectory();
  PinnedDirectory(PinnedDirectory&&);
  PinnedDirectory& operator=(PinnedDirectory&&);
  PinnedDirectory(const PinnedDirectory&) = delete;
  PinnedDirectory& operator=(const PinnedDirectory&) = delete;

  bool Open(const base::FilePath& path) {
    handles_.clear();
    const auto components = path.GetComponents();
    if (!path.IsAbsolute() || path.ReferencesParent() || components.size() < 2 ||
        components.front().size() != 2 || components.front().back() != L':') return false;
    base::FilePath current(components.front() + L"\\");
    for (size_t index = 1; index < components.size(); ++index) {
      if (components[index] != L"\\" && components[index] != L"/")
        current = current.Append(components[index]);
      base::win::ScopedHandle handle(CreateFileW(current.value().c_str(), FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
      if (!handle.is_valid() || !IsRegularHandle(handle.get(), true)) {
        handles_.clear();
        return false;
      }
      handles_.push_back(std::move(handle));
    }
    return !handles_.empty();
  }
 private:
  std::vector<base::win::ScopedHandle> handles_;
};

inline base::File OpenRegularFile(const base::FilePath& path, bool create,
                                  bool read_back = false) {
  base::win::ScopedHandle handle(CreateFileW(path.value().c_str(),
      create ? GENERIC_WRITE | (read_back ? GENERIC_READ : 0) : GENERIC_READ, FILE_SHARE_READ, nullptr,
      create ? CREATE_NEW : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!handle.is_valid() || !IsRegularHandle(handle.get(), false)) return {};
  return base::File(handle.release());
}

// New identity/receive directories grant access only to this user and SYSTEM.
inline bool CreatePrivateDirectory(const base::FilePath& path) {
  auto token = base::win::AccessToken::FromCurrentProcess();
  const auto sid = token ? token->User().ToSddlString() : std::nullopt;
  if (!sid) return false;
  const std::wstring sddl = L"O:" + *sid + L"D:P(A;OICI;FA;;;" + *sid + L")(A;OICI;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  const bool created = CreateDirectoryW(path.value().c_str(), &attributes) != FALSE;
  LocalFree(descriptor);
  return created;
}

inline bool ProtectPrivateDirectory(const base::FilePath& path) {
  auto token = base::win::AccessToken::FromCurrentProcess();
  if (!token) return false;
  const auto user = token->User();
  const auto sid = user.ToSddlString();
  if (!sid) return false;
  base::win::ScopedHandle handle(CreateFileW(path.value().c_str(), READ_CONTROL | WRITE_DAC,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!handle.is_valid() || !IsRegularHandle(handle.get(), true)) return false;
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR original = nullptr;
  if (GetSecurityInfo(handle.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
      &owner, nullptr, nullptr, nullptr, &original) != ERROR_SUCCESS) return false;
  const bool owned = EqualSid(owner, user.GetPSID()) != FALSE;
  LocalFree(original);
  if (!owned) return false;
  const std::wstring sddl = L"D:P(A;OICI;FA;;;" + *sid + L")(A;OICI;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
  PACL dacl = nullptr;
  BOOL present = FALSE, defaulted = FALSE;
  const bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) &&
      present && SetSecurityInfo(handle.get(), SE_FILE_OBJECT,
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
          nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
  LocalFree(descriptor);
  return ok;
}

class DirectoryLock {
 public:
  DirectoryLock();
  ~DirectoryLock();
  DirectoryLock(DirectoryLock&&);
  DirectoryLock& operator=(DirectoryLock&&);
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;

  bool Open(const base::FilePath& directory) {
    if (!directory_.Open(directory)) return false;
    handle_.Set(CreateFileW(directory.AppendASCII(".crphotos-lock").value().c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    return handle_.is_valid() && IsRegularHandle(handle_.get(), false);
  }
  bool TryLock() {
    OVERLAPPED position{};
    locked_ = LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                         0, 1, 0, &position) != FALSE;
    return locked_;
  }
  bool is_valid() const { return locked_; }
 private:
  PinnedDirectory directory_;
  base::win::ScopedHandle handle_;
  bool locked_ = false;
};
}  // namespace crfiles::nearby

#endif  // CRFILES_SHARING_NEARBY_FILES_WIN_H_
