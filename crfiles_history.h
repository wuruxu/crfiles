// Copyright 2026 The Chromium Authors
#ifndef CRFILES_HISTORY_H_
#define CRFILES_HISTORY_H_
#include <cstdint>
#include <string>
#include <vector>
#include "base/time/time.h"
namespace crfiles {
// Worker-only journal. A pending row is committed before touching user files.
// Failure to create an enabled record prevents the operation from starting.
// A user-disabled action is valid without creating a journal row.
class FileOperationLog {
 public:
  FileOperationLog(std::string action, std::string trigger, std::string source, std::string destination);
  ~FileOperationLog();
  FileOperationLog(const FileOperationLog&) = delete;
  FileOperationLog& operator=(const FileOperationLog&) = delete;
  bool valid() const { return suppressed_ || id_ != 0; }
  void SetDestination(std::string destination);
  void Succeed() { result_ = "success"; }
  void Cancel() { result_ = "canceled"; }
  void SourceRetained() { result_ = "source_retained"; }
 private:
  bool suppressed_ = false;
  int64_t id_ = 0;
  std::string destination_;
  std::string result_ = "failed";
};
struct FileHistoryEntry {
  int64_t id;
  base::Time time;
  std::string action, trigger, source, destination, result;
};
struct FileHistoryPage {
  bool ok = false;
  std::vector<FileHistoryEntry> entries;
};
FileHistoryPage ReadFileHistory(int limit, std::string path_query, std::string action);
uint64_t FileHistoryVersion();
void PruneFileHistory();
}
#endif
