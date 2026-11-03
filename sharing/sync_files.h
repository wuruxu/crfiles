// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRFILES_SHARING_SYNC_FILES_H_
#define CRFILES_SHARING_SYNC_FILES_H_

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include "base/files/file.h"
#include "base/files/file_path.h"

namespace crfiles {
struct SyncContent {
  int kind = 0;  // 0: tombstone, 1: regular file, 2: directory.
  uint64_t size = 0;
  std::string hash;
  bool operator==(const SyncContent&) const = default;
};
std::string CanonicalSyncPath(const std::string& path);
bool SafeSyncPath(const std::string& path);
std::string SyncHash(const std::string& bytes);

// Pins the root and walks relative paths without following links. A missing
// root, unreadable subtree or special file fails the entire scan.
class SyncFiles {
 public:
  SyncFiles(base::FilePath root, std::string id, std::function<bool()> canceled);
  ~SyncFiles();
  bool Open();
  // Check the original name (and a single file's conflict-copy namespace).
  // nullopt means the parent could not be inspected.
  std::optional<bool> ReceiveNameAvailable(const std::string& name, bool single_file);
  // Create a new child directory; an existing item is always an error.
  bool CreateReceiveDirectory(const std::string& name);
  std::string Identity() const;
  bool Scan(const std::string& single_file, std::map<std::string, SyncContent>* files);
  bool Read(const std::string& path, SyncContent* content);
  base::File OpenFile(const std::string& path);
  base::File CreateStage(const std::string& operation);
  base::File OpenStage(const std::string& operation);
  bool CopyStage(const std::string& path, const std::string& operation,
                 const SyncContent& expected);
  void DiscardStage(const std::string& operation);
  bool ArchiveAbsent(const std::string& operation);
  bool Record(const std::string& operation, const std::string& json);
  bool VerifyStage(const std::string& operation, const SyncContent& expected);
  // Idempotent after a crash. Existing data is moved to recovery first. Commit
  // never overwrites a concurrently created destination. Journal before calling.
  bool Apply(const std::string& path, const std::string& operation,
             const SyncContent& before, const SyncContent& after);
  base::FilePath Recovery() const;
 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace crfiles
#endif
