// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_tags.h"
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_cache_state.h"
#include "crFiles/crfiles_trash.h"
#include <string>
#include "crFiles/crfiles_history.h"
#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/files/file_enumerator.h"
#include "base/files/important_file_writer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"
#include "base/uuid.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#endif
namespace crfiles {
namespace {
base::Lock& TrashLock() { static base::NoDestructor<base::Lock> lock; return *lock; }
}
base::FilePath TrashRoot() {
#if BUILDFLAG(IS_MAC)
  if (IsCrFilesSandboxed()) return MacApplicationSupportDirectory().AppendASCII("Trash");
#endif
  base::FilePath root;
#if BUILDFLAG(IS_WIN)
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &root)) return {};
#elif BUILDFLAG(IS_MAC)
  if (!base::PathService::Get(base::DIR_HOME, &root)) return {};
  root = root.AppendASCII("Library").AppendASCII("Application Support");
#else
  auto value = base::Environment::Create()->GetVar("XDG_DATA_HOME");
  if (value) root = base::FilePath::FromUTF8Unsafe(*value);
  if (!root.IsAbsolute()) {
    if (!base::PathService::Get(base::DIR_HOME, &root)) return {};
    root = root.AppendASCII(".local").AppendASCII("share");
  }
#endif
  return root.AppendASCII("crFiles").AppendASCII("Trash");
}
bool MoveToTrash(const base::FilePath& path) {
  ScopedFileCacheMutation mutation({path, TrashRoot()});
  FileOperationLog log("trash", "menu", path.AsUTF8Unsafe(), TrashRoot().AsUTF8Unsafe());
  if (!log.valid()) return false;
  base::AutoLock lock(TrashLock());
  const auto root = TrashRoot();
  if (root.empty() || !path.IsAbsolute() || path.ReferencesParent() ||
      path == path.DirName() || root == path || root.IsParent(path) || path.IsParent(root)) return false;
  const auto item = root.AppendASCII(base::Uuid::GenerateRandomV4().AsLowercaseString());
  const auto files = item.AppendASCII("files");
  if (!base::CreateDirectory(files)) return false;
  // Record the original path before moving data; failed moves leave no data lost.
  if (!base::ImportantFileWriter::WriteFileAtomically(item.AppendASCII("origin"), path.AsUTF8Unsafe())) return false;
  log.SetDestination(files.Append(path.BaseName()).AsUTF8Unsafe());
  if (!base::Move(path, files.Append(path.BaseName()))) return false;
  RemoveFileTagsUnderPath(path);
  log.Succeed(); return true;
}
bool EmptyTrash() {
  const auto root = TrashRoot();
  if (root.empty() || !root.IsAbsolute() || root.ReferencesParent()) return false;
  ScopedFileCacheMutation mutation({root});
  FileOperationLog log("empty_trash", "menu", root.AsUTF8Unsafe(), {});
  if (!log.valid()) return false;
  base::AutoLock lock(TrashLock());
  if (base::IsLink(root)) return false;
  if (!base::PathExists(root)) { log.Succeed(); return true; }
  base::FileEnumerator entries(root, false, base::FileEnumerator::DIRECTORIES,
      base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  bool ok = true;
  for (auto entry = entries.Next(); !entry.empty(); entry = entries.Next()) {
    // Never delete the trash root or unrelated files outside its UUID entries.
    if (!base::Uuid::ParseLowercase(entry.BaseName().AsUTF8Unsafe()).is_valid()) { ok = false; continue; }
    if (!base::DeletePathRecursively(entry)) ok = false;
    else RemoveFileTagsUnderPath(entry);
  }
  ok &= entries.GetError() == base::File::FILE_OK;
  if (ok) log.Succeed();
  return ok;
}
bool RestoreFromTrash(const base::FilePath& path) {
  FileOperationLog log("restore", "menu", path.AsUTF8Unsafe(), {});
  if (!log.valid()) return false;
  base::AutoLock lock(TrashLock());
  const auto root = TrashRoot();
  const auto item = path.DirName().DirName();
  if (root.empty() || item.DirName() != root || path.DirName().BaseName().value() != FILE_PATH_LITERAL("files")) return false;
  std::string origin;
  if (!base::ReadFileToStringWithMaxSize(item.AppendASCII("origin"), &origin, 65536)) return false;
  const auto destination = base::FilePath::FromUTF8Unsafe(origin);
  log.SetDestination(destination.AsUTF8Unsafe());
  ScopedFileCacheMutation mutation({path, destination, root});
  if (!destination.IsAbsolute() || destination.ReferencesParent() || destination.BaseName() != path.BaseName() ||
      root == destination || root.IsParent(destination) || destination.IsParent(root) ||
      base::PathExists(destination) || base::IsLink(destination)) return false;
  if (!base::CreateDirectory(destination.DirName()) || !base::Move(path, destination)) return false;
  // Remove only empty directories and metadata after the successful move.
  base::DeleteFile(item.AppendASCII("origin"));
  base::DeleteFile(path.DirName());
  base::DeleteFile(item);
  RemoveFileTagsUnderPath(path);
  log.Succeed(); return true;
}
}
