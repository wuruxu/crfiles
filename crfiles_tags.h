// Copyright 2026 The Chromium Authors
#ifndef CRFILES_TAGS_H_
#define CRFILES_TAGS_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "base/files/file_path.h"
namespace crfiles {
struct FileTag {
  int64_t id = 0;
  std::string name;
  std::vector<base::FilePath> paths;
};
struct TagsSnapshot {
  bool ok = false;
  std::vector<FileTag> tags;
};
enum class TagWriteResult { kOk, kError, kLimit, kInvalidName };
// Worker-only. Tags are attached to lexical absolute paths, never file IDs.
TagsSnapshot ReadFileTags();
TagWriteResult CreateFileTag(std::string name);
// Deletes tags and their associations, never the referenced files.
TagWriteResult DeleteFileTags(std::vector<int64_t> ids);
// Apply only explicit changes, preserving mixed selections and unrelated tags.
// The six-tag limit is checked for every path inside a single transaction.
TagWriteResult EditFileTags(std::vector<base::FilePath> paths,
                           std::vector<int64_t> add, std::vector<int64_t> remove);
void RemoveFileTagsUnderPath(const base::FilePath& path);
uint64_t FileTagsVersion();
struct TagSize {
  uint64_t bytes = 0;
  size_t available = 0;
  bool incomplete = false;
};
TagSize CalculateTagSize(std::vector<base::FilePath> paths,
                         const std::shared_ptr<std::atomic_bool>& canceled);
}
#endif
