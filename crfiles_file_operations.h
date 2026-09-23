// Copyright 2026 The Chromium Authors
#ifndef CRFILES_FILE_OPERATIONS_H_
#define CRFILES_FILE_OPERATIONS_H_
#include <vector>
#include <string>
#include <cstddef>
#include "base/files/file_path.h"
namespace crfiles {
struct FileOperationResult {
  std::vector<base::FilePath> changed;
  // Only sources actually removed by a successful move.
  std::vector<base::FilePath> removed;
  size_t failed = 0;
};
// Worker-only. Never merge or overwrite an existing destination. Symlinks and
// special files are rejected; moves publish a complete copy before removing input.
FileOperationResult TransferLocalFiles(std::vector<base::FilePath> sources,
                                      base::FilePath destination, bool move, std::string trigger = "drag");
FileOperationResult RenameLocalFile(base::FilePath source, base::FilePath target);
FileOperationResult CompressLocalFiles(std::vector<base::FilePath> sources, base::FilePath target);
}
#endif
