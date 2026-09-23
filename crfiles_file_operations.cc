// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_cache_state.h"
#include "crFiles/crfiles_file_operations.h"
#include <set>
#include "third_party/zlib/google/zip.h"
#include "crFiles/crfiles_history.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
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
bool RegularEntry(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  const auto attributes = GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  struct stat info;
  return lstat(path.value().c_str(), &info) == 0 && (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode));
#endif
}
bool SafeTree(const base::FilePath& path) {
  if (!RegularEntry(path)) return false;
  if (!base::DirectoryExists(path)) return true;
  // Check each level before recursing; never descend through symbolic links.
  std::vector<base::FilePath> pending{path};
  size_t count = 0;
  while (!pending.empty()) {
    auto folder = pending.back(); pending.pop_back();
    int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
    types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
    base::FileEnumerator entries(folder, false, types,
        base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
        base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (auto child = entries.Next(); !child.empty(); child = entries.Next()) {
      if (++count > 100000 || !RegularEntry(child)) return false;
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
    if (!log.valid() || canonical_destination.empty() || !base::DirectoryExists(canonical_destination)) { ++result.failed; continue; }
    const auto canonical = base::MakeAbsoluteFilePath(source);
    if (canonical.empty() || !seen.insert(canonical).second) { ++result.failed; continue; }
    const auto target = canonical_destination.Append(source.BaseName());
    if (canonical == target || canonical == canonical_destination || canonical.IsParent(canonical_destination) ||
        source.BaseName().empty() || base::PathExists(target) || base::IsLink(target) || !SafeTree(source)) {
      ++result.failed; continue;
    }
    // A same-filesystem move is an atomic, no-replace rename. Cross-device
    // moves fall back to publishing a complete copy before removing the input.
    if (move && Publish(source, target)) { result.changed.push_back(target); result.removed.push_back(source); log.Succeed(); continue; }
    base::ScopedTempDir staging;
    if (!staging.CreateUniqueTempDirUnderPath(canonical_destination)) { ++result.failed; continue; }
    const auto staged = staging.GetPath().Append(source.BaseName());
    const bool copied = base::DirectoryExists(source) ? base::CopyDirectory(source, staged, true) : base::CopyFile(source, staged);
    if (!copied || !SafeTree(staged) || !Publish(staged, target)) { ++result.failed; continue; }
    result.changed.push_back(target);
    if (move && !base::DeletePathRecursively(source)) { ++result.failed; log.SourceRetained(); }
    else { if (move) result.removed.push_back(source); log.Succeed(); }
  }
  return result;
}
FileOperationResult RenameLocalFile(base::FilePath source, base::FilePath target) {
  FileOperationResult result;
  FileOperationLog log("rename", "menu", source.AsUTF8Unsafe(), target.AsUTF8Unsafe());
  ScopedFileCacheMutation mutation({source, target});
  if (!log.valid() || source.DirName() != target.DirName() || source == target || !RegularEntry(source) || !Publish(source, target)) { result.failed = 1; return result; }
  log.Succeed(); result.changed.push_back(target); result.removed.push_back(source); return result;
}
FileOperationResult CompressLocalFiles(std::vector<base::FilePath> sources, base::FilePath target) {
  FileOperationResult result;
  if (sources.empty()) { result.failed = 1; return result; }
  FileOperationLog log("compress", "menu", sources.front().DirName().AsUTF8Unsafe(), target.AsUTF8Unsafe());
  ScopedFileCacheMutation mutation({target});
  base::ScopedTempDir staging;
  if (!log.valid() || base::PathExists(target) || !staging.CreateUniqueTempDirUnderPath(target.DirName())) { result.failed = 1; return result; }
  zip::ZipParams params; params.src_dir = sources.front().DirName(); params.recursive = true;
  params.dest_file = staging.GetPath().AppendASCII("archive.zip");
  std::vector<base::FilePath> relative_paths;
  relative_paths.reserve(sources.size());
  for (const auto& source : sources) {
    if (source.DirName() != params.src_dir || !SafeTree(source)) { result.failed = 1; return result; }
    relative_paths.push_back(source.BaseName());
  }
  params.src_files = relative_paths;
  if (!zip::Zip(params) || !Publish(params.dest_file, target)) { result.failed = 1; return result; }
  log.Succeed(); result.changed.push_back(target); return result;
}

}
