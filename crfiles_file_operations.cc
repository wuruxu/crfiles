// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_cache_state.h"
#include "crFiles/crfiles_file_operations.h"
#include "crFiles/crfiles_archive.h"
#include "crFiles/crfiles_context_actions.h"
#include <set>
#include <limits>
#include "crFiles/crfiles_history.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include <copyfile.h>
#endif
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include "base/win/scoped_handle.h"
#else
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#if BUILDFLAG(IS_LINUX)
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif
namespace crfiles {
namespace {
#if BUILDFLAG(IS_WIN)
base::FilePath ResolvedWindowsPath(const base::FilePath& path) {
  base::win::ScopedHandle handle(CreateFileW(path.value().c_str(), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!handle.is_valid()) return {};
  const DWORD size = GetFinalPathNameByHandleW(handle.get(), nullptr, 0, FILE_NAME_NORMALIZED);
  if (!size || size > 65536) return {};
  std::wstring name(size + 1, L'\0');
  const DWORD written = GetFinalPathNameByHandleW(handle.get(), name.data(), static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
  if (!written || written >= name.size()) return {};
  name.resize(written);
  return base::FilePath(name).NormalizePathSeparators().StripTrailingSeparators();
}
bool WindowsContainsPath(const base::FilePath& source, const base::FilePath& destination) {
  const auto left = source.GetComponents(), right = destination.GetComponents();
  if (left.size() > right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i)
    if (!base::FilePath::CompareEqualIgnoreCase(left[i], right[i])) return false;
  return true;
}
#endif
bool RegularEntry(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  const auto attributes = GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  struct stat info;
  return lstat(path.value().c_str(), &info) == 0 && (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode));
#endif
}
bool ArchiveSymbolicLink(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  WIN32_FIND_DATAW data;
  const HANDLE handle = FindFirstFileW(path.value().c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) return false;
  FindClose(handle);
  return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && data.dwReserved0 == IO_REPARSE_TAG_SYMLINK;
#else
  return base::IsLink(path);
#endif
}
bool SafeTree(const base::FilePath& path, const std::atomic_bool* canceled = nullptr, ArchiveProgress* progress = nullptr,
    bool allow_archive_links = false) {
  if (canceled && canceled->load()) return false;
  if (!RegularEntry(path)) return false;
  auto count_entry = [&](const base::FilePath& entry, bool directory, int64_t size) {
    if (!progress) return true;
    progress->SetCurrentPath(entry);
    if (++progress->total_entries > 100000) return false;
    if (!directory && size > 0) {
      const auto bytes = static_cast<uint64_t>(size);
      if (bytes > std::numeric_limits<uint64_t>::max() - progress->total_bytes.load()) return false;
      progress->total_bytes += bytes;
    }
    return true;
  };
  if (progress) {
    base::File::Info info;
    if (!base::GetFileInfo(path, &info) || !count_entry(path, info.is_directory, info.size)) return false;
  }
  if (!base::DirectoryExists(path)) return true;
  // Check each level before recursing; never descend through symbolic links.
  std::vector<base::FilePath> pending{path};
  size_t count = 0;
  while (!pending.empty()) {
    if (canceled && canceled->load()) return false;
    auto folder = pending.back(); pending.pop_back();
    int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
    types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
    base::FileEnumerator entries(folder, false, types,
        base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
        base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (auto child = entries.Next(); !child.empty(); child = entries.Next()) {
      if (canceled && canceled->load()) return false;
      if (++count > 100000) return false;
      if (allow_archive_links && ArchiveSymbolicLink(child)) continue;
      if (!RegularEntry(child)) return false;
      const auto& info = entries.GetInfo();
      if (!count_entry(child, info.IsDirectory(), info.GetSize())) return false;
      if (base::DirectoryExists(child)) pending.push_back(child);
    }
    if (entries.GetError() != base::File::FILE_OK) return false;
  }
  return true;
}
bool Publish(const base::FilePath& from, const base::FilePath& to) {
#if BUILDFLAG(IS_WIN)
  return MoveFileExW(from.value().c_str(), to.value().c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
#elif BUILDFLAG(IS_LINUX)
  return syscall(SYS_renameat2, AT_FDCWD, from.value().c_str(), AT_FDCWD,
                 to.value().c_str(), RENAME_NOREPLACE) == 0;
#elif BUILDFLAG(IS_MAC)
  return renamex_np(from.value().c_str(), to.value().c_str(), RENAME_EXCL) == 0;
#else
  return false;
#endif
}
// Ordinary transfers preserve links. Archive validation deliberately retains
// its own policy, including rejecting link inputs for compression.
bool TransferTreeIsSafe(const base::FilePath& path) {
#if BUILDFLAG(IS_MAC)
  return base::IsLink(path) || SafeTree(path, nullptr, nullptr, true);
#else
  return SafeTree(path);
#endif
}
bool CopyTransferTree(const base::FilePath& source, const base::FilePath& target) {
#if BUILDFLAG(IS_MAC)
  // Preserve package permissions and metadata as well as link text. Never
  // dereference links, including dangling or absolute links, during copying.
  return copyfile(source.value().c_str(), target.value().c_str(), nullptr,
                  COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_NOFOLLOW |
                      COPYFILE_EXCL) == 0;
#else
  return base::DirectoryExists(source) ? base::CopyDirectory(source, target, true)
                                       : base::CopyFile(source, target);
#endif
}
}
FileOperationResult TransferLocalFiles(std::vector<base::FilePath> sources,
                                      base::FilePath destination, bool move, std::string trigger) {
  std::vector<base::FilePath> affected;
  for (const auto& source : sources) {
    if (move) affected.push_back(source);
    affected.push_back(destination.Append(source.BaseName()));
  }
  ScopedFileCacheMutation mutation(std::move(affected));
  FileOperationResult result;
  const auto canonical_destination = base::MakeAbsoluteFilePath(destination);

  std::set<base::FilePath> seen;
  for (const auto& source : sources) {
    FileOperationLog log(move ? "move" : "copy", trigger, source.AsUTF8Unsafe(), destination.Append(source.BaseName()).AsUTF8Unsafe());
    if (!log.valid() || !IsValidEntryName(source.BaseName().AsUTF16Unsafe()) || canonical_destination.empty() || !base::DirectoryExists(canonical_destination)) { ++result.failed; continue; }
    auto canonical = base::MakeAbsoluteFilePath(source);
#if BUILDFLAG(IS_MAC)
    // Resolve the parent, not the link target: a dangling link is still a valid
    // source and links to the same target are distinct directory entries.
    if (base::IsLink(source)) {
      const auto parent = base::MakeAbsoluteFilePath(source.DirName());
      canonical = parent.empty() ? base::FilePath() : parent.Append(source.BaseName());
    }
#endif
    if (canonical.empty() || !seen.insert(canonical).second) { ++result.failed; continue; }
#if BUILDFLAG(IS_WIN)
    const auto resolved_source = ResolvedWindowsPath(source);
    const auto resolved_destination = ResolvedWindowsPath(canonical_destination);
    if (resolved_source.empty() || resolved_destination.empty() ||
        WindowsContainsPath(resolved_source, resolved_destination)) { ++result.failed; continue; }
#endif
    const auto target = canonical_destination.Append(source.BaseName());
    if (canonical == target || canonical == canonical_destination || canonical.IsParent(canonical_destination) ||
        source.BaseName().empty() || base::PathExists(target) || base::IsLink(target) || !TransferTreeIsSafe(source)) {
      ++result.failed; continue;
    }
    // A same-filesystem move is an atomic, no-replace rename. If that fails,
    // copying cannot establish exclusive ownership of the source: another
    // process can still modify files or add children while we copy it.
    if (move && Publish(source, target)) { result.changed.push_back(target); result.removed.push_back(source); log.Succeed(); continue; }
    base::ScopedTempDir staging;
    if (!staging.CreateUniqueTempDirUnderPath(canonical_destination)) { ++result.failed; continue; }
    const auto staged = staging.GetPath().Append(source.BaseName());
    const bool copied = CopyTransferTree(source, staged);
    if (!copied || !TransferTreeIsSafe(staged) || !Publish(staged, target)) { ++result.failed; continue; }
    result.changed.push_back(target);
    // Never recursively delete a live source tree after a non-atomic copy.
    // Report partial completion so the UI keeps the source selection and the
    // history records that the source was retained. This also protects files
    // held open for writing by another application.
    if (move) { ++result.failed; ++result.sources_retained; log.SourceRetained(); }
    else { log.Succeed(); }
  }
  return result;
}
FileOperationResult RenameLocalFile(base::FilePath source, base::FilePath target) {
  FileOperationResult result;
  FileOperationLog log("rename", "menu", source.AsUTF8Unsafe(), target.AsUTF8Unsafe());
  ScopedFileCacheMutation mutation({source, target});
  if (!log.valid() || !IsValidEntryName(target.BaseName().AsUTF16Unsafe()) || source.DirName() != target.DirName() || source == target || !RegularEntry(source) || !Publish(source, target)) { result.failed = 1; return result; }
  log.Succeed(); result.changed.push_back(target); result.removed.push_back(source); return result;
}
FileOperationResult CompressLocalFiles(std::vector<base::FilePath> sources, base::FilePath target,
    std::shared_ptr<ArchiveProgress> progress) {
  FileOperationResult result;
  if (sources.empty()) { result.failed = 1; return result; }
  FileOperationLog log("compress", "menu", sources.front().DirName().AsUTF8Unsafe(), target.AsUTF8Unsafe());
  ScopedFileCacheMutation mutation({target});
  base::ScopedTempDir staging;
  if (!log.valid() || !IsValidEntryName(target.BaseName().AsUTF16Unsafe()) || base::PathExists(target) || !staging.CreateUniqueTempDirUnderPath(target.DirName())) { result.failed = 1; return result; }
  const auto source_directory = sources.front().DirName();
  const auto staged_archive = staging.GetPath().AppendASCII("archive.zip");
  std::vector<base::FilePath> relative_paths;
  relative_paths.reserve(sources.size());
  for (const auto& source : sources) {
    if (source.DirName() != source_directory || !SafeTree(source, &progress->canceled, progress.get())) {
      if (progress->canceled) log.Cancel(); else result.failed = 1;
      return result;
    }
    relative_paths.push_back(source.BaseName());
  }
  progress->SetCurrentPath({});
  progress->prepared = true;
  if (!EncodeArchive(source_directory, std::move(relative_paths), staged_archive, progress) || progress->canceled) {
    if (progress->canceled) log.Cancel(); else result.failed = 1;
    return result;
  }
  if (!Publish(staged_archive, target)) { result.failed = 1; return result; }
  log.Succeed(); result.changed.push_back(target); return result;
}

FileOperationResult ExtractLocalArchive(base::FilePath archive, base::FilePath destination,
    bool same_name_folder, std::string password, std::shared_ptr<ArchiveProgress> progress) {
  FileOperationResult result;
  FileOperationLog log("extract", "menu", archive.AsUTF8Unsafe(),
      (same_name_folder ? destination.Append(ArchiveFolderName(archive)) : destination).AsUTF8Unsafe());
  // Extract to accepts a new directory, including missing parent directories.
  // Create it on this worker before resolving its absolute filesystem path.
  if (!log.valid() || !destination.IsAbsolute() || !base::CreateDirectory(destination)) { result.failed = 1; return result; }
  const auto parent = base::MakeAbsoluteFilePath(destination);
  const auto name = ArchiveFolderName(archive);
  const auto target = same_name_folder ? parent.Append(name) : parent;
  log.SetDestination(target.AsUTF8Unsafe());
  if (!log.valid() || parent.empty() || !base::DirectoryExists(parent) ||
      !IsExtractableArchive(archive) || !RegularEntry(archive) ||
      (same_name_folder && !IsValidEntryName(name.AsUTF16Unsafe()))) {
    result.failed = 1; return result;
  }
  base::ScopedTempDir staging;
  if (!staging.CreateUniqueTempDirUnderPath(parent)) { result.failed = 1; return result; }
  const auto payload = staging.GetPath().AppendASCII("payload");
  if (!base::CreateDirectory(payload) || !DecodeArchive(archive, payload, password, progress) || !SafeTree(payload, &progress->canceled, nullptr, true)) {
    if (progress->canceled) log.Cancel();
    result.failed = progress->canceled ? 0 : 1; return result;
  }
  std::vector<std::pair<base::FilePath, base::FilePath>> pending;
  auto children = [&](const base::FilePath& from, const base::FilePath& to) {
    int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
    types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
    base::FileEnumerator entries(from, false, types, base::FilePath::StringType(),
        base::FileEnumerator::FolderSearchPolicy::ALL, base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (auto entry = entries.Next(); !entry.empty(); entry = entries.Next())
      pending.emplace_back(entry, to.Append(entry.BaseName()));
    return entries.GetError() == base::File::FILE_OK;
  };
  if (same_name_folder) pending.emplace_back(payload, target);
  else if (!children(payload, parent)) { result.failed = 1; return result; }
  ScopedFileCacheMutation mutation({target});
  bool replace_all = false;
  size_t backup_id = 0;
  std::set<base::FilePath> changed;
  auto mark_changed = [&](base::FilePath path) {
    changed.insert(path);
    // Refresh the visible top-level directory as well as the changed file.
    while (parent.IsParent(path.DirName())) { path = path.DirName(); changed.insert(path); }
  };
  while (!pending.empty()) {
    if (progress->canceled) break;
    const auto [source, output] = pending.back(); pending.pop_back();
    if (output == staging.GetPath() || output.IsParent(staging.GetPath())) { result.failed = 1; break; }
    bool exists = base::PathExists(output) || base::IsLink(output);
    // Merge real directories only; never traverse an existing link/junction.
    if (exists && RegularEntry(output) && RegularEntry(source) && base::DirectoryExists(source) && base::DirectoryExists(output)) {
      if (!children(source, output)) { result.failed = 1; break; }
      continue;
    }
    if (!exists && Publish(source, output)) { mark_changed(output); continue; }
    exists = base::PathExists(output) || base::IsLink(output);
    if (!exists) { result.failed = 1; break; }
    auto choice = replace_all ? ArchiveConflictChoice::kReplace : progress->WaitForConflict(output);
    if (choice == ArchiveConflictChoice::kCancel || progress->canceled) { progress->canceled = true; break; }
    if (choice == ArchiveConflictChoice::kSkip) continue;
    if (choice == ArchiveConflictChoice::kReplaceAll) replace_all = true;
    // Keep the old entry until the replacement has been successfully published.
    const auto backup = staging.GetPath().AppendASCII("replaced-" + std::to_string(backup_id++));
    if (!Publish(output, backup)) { result.failed = 1; break; }
    if (progress->canceled || !Publish(source, output)) {
      if (!Publish(backup, output)) {
        // Never let temporary-directory cleanup delete the only remaining copy.
        const auto preserved = staging.Take();
        result.recovery_path = preserved;
        log.SetDestination(preserved.AsUTF8Unsafe());
        mark_changed(output);
        result.failed = 1;
      } else if (!progress->canceled) result.failed = 1;
      break;
    }
    mark_changed(output);
  }
  result.changed.assign(changed.begin(), changed.end());
  if (progress->canceled) { if (!result.failed) log.Cancel(); return result; }
  if (result.failed) return result;
  log.Succeed();
  return result;
}

}
