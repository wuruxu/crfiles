// Copyright 2026 The Chromium Authors
#ifndef CRFILES_FILE_OPERATIONS_H_
#define CRFILES_FILE_OPERATIONS_H_
#include <vector>
#include <string>
#include <cstddef>
#include <memory>
#include "base/files/file_path.h"
namespace crfiles {
struct ArchiveProgress;
struct FileOperationResult {
  std::vector<base::FilePath> changed;
  // Only sources actually removed by a successful move.
  std::vector<base::FilePath> removed;
  size_t failed = 0;
  size_t sources_retained = 0;
  // Populated for extraction conflicts; existing entries are never overwritten.
  base::FilePath conflict_path;
  // An original entry retained here if replacement rollback could not finish.
  base::FilePath recovery_path;
};
// Worker-only. Never merge or overwrite an existing destination. Symlinks and
// special files are rejected. Moves use an atomic no-replace rename; fallback
// copies retain the live source and report partial completion.
FileOperationResult TransferLocalFiles(std::vector<base::FilePath> sources,
                                      base::FilePath destination, bool move, std::string trigger = "drag");
FileOperationResult RenameLocalFile(base::FilePath source, base::FilePath target);
FileOperationResult CompressLocalFiles(std::vector<base::FilePath> sources, base::FilePath target,
    std::shared_ptr<ArchiveProgress> progress);
}
#endif
