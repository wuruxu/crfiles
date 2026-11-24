// Copyright 2026 The Chromium Authors
#ifndef CRFILES_ARCHIVE_H_
#define CRFILES_ARCHIVE_H_
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "base/files/file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "crFiles/crfiles_file_operations.h"
namespace crfiles {
inline constexpr char kArchiveProcessSwitch[] = "crfiles-archive-process";
enum class ArchiveConflictChoice { kWaiting, kReplaceAll, kReplace, kSkip, kCancel };
struct ArchiveProgress {
  ArchiveProgress();
  ~ArchiveProgress();
  // Worker-only; requires base synchronization waits to be allowed.
  ArchiveConflictChoice WaitForConflict(const base::FilePath& path);
  base::FilePath PendingConflict();
  void ResolveConflict(const base::FilePath& path, ArchiveConflictChoice choice);
  void SetCurrentPath(const base::FilePath& path);
  base::FilePath CurrentPath();

  std::atomic_bool canceled{false};
  std::atomic_bool needs_password{false};
  std::atomic_uint64_t bytes{0};
  std::atomic_uint64_t total_bytes{0}, total_entries{0}, entries_done{0};
  std::atomic_bool prepared{false};
 private:
  base::Lock progress_lock_;
  base::FilePath current_path_ GUARDED_BY(progress_lock_);
  base::Lock conflict_lock_;
  base::FilePath conflict_path_ GUARDED_BY(conflict_lock_);
  ArchiveConflictChoice conflict_choice_ GUARDED_BY(conflict_lock_) = ArchiveConflictChoice::kWaiting;
  base::WaitableEvent conflict_resolved_;

};
struct ArchiveRow {
  base::FilePath name;
  int ordinal;
  bool directory;
  bool link;
  uint64_t size;
  std::string mime;
};
struct ArchiveListing {
  base::File::Info source;
  std::map<base::FilePath, std::vector<ArchiveRow>> folders;
};
struct ArchivePreview {
  base::ScopedTempDir directory;
  base::FilePath path;
  ~ArchivePreview();
};
bool IsBrowsableArchive(const base::FilePath& path);
// Worker only. Both operations run the decoder in the disposable archive helper.
std::unique_ptr<ArchiveListing> ListArchive(const base::FilePath& archive,
    std::shared_ptr<ArchiveProgress> progress);
std::shared_ptr<ArchivePreview> PreviewArchiveMember(const base::FilePath& archive,
    base::File::Info source, base::FilePath member, int ordinal, std::string password,
    std::shared_ptr<ArchiveProgress> progress);
bool IsExtractableArchive(const base::FilePath& path);
std::string ArchiveFormat(const base::FilePath& path);
base::FilePath ArchiveFolderName(const base::FilePath& path);
int RunArchiveProcess();
// Worker-only, with base synchronization waits allowed (ThreadPool callers
// must specify MayBlock and WithBaseSyncPrimitives). Decoder runs in a
// disposable child, writing only into staging.
bool DecodeArchive(const base::FilePath& archive, const base::FilePath& staging,
                   const std::string& password, std::shared_ptr<ArchiveProgress> progress);
// Same worker requirements as DecodeArchive. ZIP creation shares the
// disposable archive helper process.
bool EncodeArchive(const base::FilePath& source_directory,
    std::vector<base::FilePath> relative_sources, const base::FilePath& output,
    std::shared_ptr<ArchiveProgress> progress);
FileOperationResult ExtractLocalArchive(base::FilePath archive, base::FilePath destination,
    bool same_name_folder, std::string password, std::shared_ptr<ArchiveProgress> progress);
}
#endif
