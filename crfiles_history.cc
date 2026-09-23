// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_history.h"
#include <atomic>
#include <memory>
#include <utility>
#include "crFiles/crfiles_trash.h"
#include "crFiles/crfiles_theme.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "third_party/sqlite/sqlite3.h"
namespace crfiles {
namespace {
std::atomic<uint64_t> history_version{0};
base::Lock& HistoryLock() { static base::NoDestructor<base::Lock> lock; return *lock; }
class Database {
 public:
  Database() {
    // Chromium disables SQLite automatic initialization. All journal connections
    // are opened under HistoryLock, including this one-time initialization.
    static const int initialized = sqlite3_initialize();
    if (initialized != SQLITE_OK) return;
    const auto trash = TrashRoot();
    if (trash.empty() || !base::CreateDirectory(trash.DirName())) return;
    const auto path = trash.DirName().AppendASCII("history.sqlite3").AsUTF8Unsafe();
    sqlite3* opened = nullptr;
    const int result = sqlite3_open(path.c_str(), &opened);
    db_.reset(opened);
    if (result != SQLITE_OK) return;
    sqlite3_busy_timeout(db_.get(), 5000);
    ok_ = sqlite3_exec(db_.get(), "CREATE TABLE IF NOT EXISTS operations (id INTEGER PRIMARY KEY, time INTEGER NOT NULL, action TEXT NOT NULL, trigger TEXT NOT NULL, source TEXT NOT NULL, destination TEXT NOT NULL, result TEXT NOT NULL)", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (ok_) ok_ = sqlite3_exec(db_.get(), "CREATE INDEX IF NOT EXISTS operations_time ON operations(time DESC,id DESC)", nullptr, nullptr, nullptr) == SQLITE_OK;
  }
  void Prune() {
    if (!get()) return;
    const int days = GetHistoryPreferences().days;
    static base::Time last;
    static int last_days = 0;
    const auto now = base::Time::Now();
    if (days == last_days && now >= last && now - last < base::Hours(1)) return;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(get(), "DELETE FROM operations WHERE time < ?", -1, &statement, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(statement, 1, (now - base::Days(days)).ToDeltaSinceWindowsEpoch().InMicroseconds());
    if (sqlite3_step(statement) == SQLITE_DONE) {
      last = now; last_days = days;
      if (sqlite3_changes(get())) ++history_version;
    }
    sqlite3_finalize(statement);
  }
  sqlite3* get() { return ok_ ? db_.get() : nullptr; }
 private:
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
  bool ok_ = false;
};
class Statement {
 public:
  Statement(sqlite3* db, const char* query) {
    sqlite3_stmt* prepared = nullptr;
    if (db) sqlite3_prepare_v2(db, query, -1, &prepared, nullptr);
    statement_.reset(prepared);
  }
  sqlite3_stmt* get() { return statement_.get(); }
  void Text(int index, const std::string& value) { sqlite3_bind_text(statement_.get(), index, value.c_str(), -1, SQLITE_TRANSIENT); }
 private:
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement_{nullptr, sqlite3_finalize};
};
std::string Column(sqlite3_stmt* statement, int column) {
  const auto* value = sqlite3_column_text(statement, column);
  return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
}
}
FileOperationLog::FileOperationLog(std::string action, std::string trigger, std::string source, std::string destination)
    : destination_(std::move(destination)) {
  const auto preferences = GetHistoryPreferences();
  for (size_t i = 0; i < kHistoryActions.size(); ++i)
    if (action == kHistoryActions[i] && !preferences.record[i]) { suppressed_ = true; return; }
  base::AutoLock lock(HistoryLock());
  Database db;
  db.Prune();
  Statement query(db.get(), "INSERT INTO operations(time,action,trigger,source,destination,result) VALUES(?,?,?,?,?,'pending')");
  if (!query.get()) { LOG(ERROR) << "Cannot open file operation history"; return; }
  sqlite3_bind_int64(query.get(), 1, base::Time::Now().ToDeltaSinceWindowsEpoch().InMicroseconds());
  query.Text(2, action); query.Text(3, trigger); query.Text(4, source); query.Text(5, destination_);
  if (sqlite3_step(query.get()) == SQLITE_DONE) { id_ = sqlite3_last_insert_rowid(db.get()); ++history_version; }
}
FileOperationLog::~FileOperationLog() {
  if (!id_) return;
  base::AutoLock lock(HistoryLock());
  Database db;
  Statement query(db.get(), "UPDATE operations SET destination=?,result=? WHERE id=?");
  if (!query.get()) { LOG(ERROR) << "Cannot finish file operation history"; return; }
  query.Text(1, destination_); query.Text(2, result_); sqlite3_bind_int64(query.get(), 3, id_);
  if (sqlite3_step(query.get()) != SQLITE_DONE) LOG(ERROR) << "Cannot finish file operation history";
  ++history_version;
}
void FileOperationLog::SetDestination(std::string destination) { destination_ = std::move(destination); }
void PruneFileHistory() {
  base::AutoLock lock(HistoryLock());
  Database db; db.Prune();
}
uint64_t FileHistoryVersion() { return history_version.load(); }
FileHistoryPage ReadFileHistory(int limit, std::string path_query, std::string action) {
  base::AutoLock lock(HistoryLock());
  Database db;
  db.Prune();
  Statement query(db.get(), "SELECT id,time,action,trigger,source,destination,result FROM operations WHERE (? = '' OR instr(source, ?) > 0 OR instr(destination, ?) > 0) AND (? = '' OR action = ?) ORDER BY time DESC,id DESC LIMIT ?");
  FileHistoryPage page;
  if (!query.get()) return page;
  query.Text(1, path_query); query.Text(2, path_query); query.Text(3, path_query);
  query.Text(4, action); query.Text(5, action);
  sqlite3_bind_int(query.get(), 6, limit);
  int step;
  while ((step = sqlite3_step(query.get())) == SQLITE_ROW) {
    page.entries.push_back({sqlite3_column_int64(query.get(), 0),
        base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(sqlite3_column_int64(query.get(), 1))),
        Column(query.get(), 2), Column(query.get(), 3), Column(query.get(), 4), Column(query.get(), 5), Column(query.get(), 6)});
  }
  page.ok = step == SQLITE_DONE;
  return page;
}
}
