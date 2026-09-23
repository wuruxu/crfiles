// Copyright 2026 The Chromium Authors
#ifndef CRFILES_CACHE_STATE_H_
#define CRFILES_CACHE_STATE_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "base/files/file_path.h"
namespace crfiles {
// Shared only by live cache entries/readers. Unused path registrations expire.
struct FileCacheEpoch {
  std::atomic<uint64_t> revision{0};
  std::atomic<unsigned> writers{0};
  bool Matches(uint64_t value) const {
    return writers.load() == 0 && revision.load() == value;
  }
};
std::shared_ptr<FileCacheEpoch> ObserveFileCachePath(const base::FilePath& path, bool directory);
// Covers the entire mutation, including failures and partial changes. Directory
// readers overlap descendants; image readers overlap only changed subtrees.
class ScopedFileCacheMutation {
 public:
  explicit ScopedFileCacheMutation(std::vector<base::FilePath> paths);
  ~ScopedFileCacheMutation();
  ScopedFileCacheMutation(const ScopedFileCacheMutation&) = delete;
  ScopedFileCacheMutation& operator=(const ScopedFileCacheMutation&) = delete;
 private:
  uint64_t id_;
};
}
#endif
