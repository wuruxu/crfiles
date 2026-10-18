// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_tags.h"
#include <algorithm>
#include <limits>
#include <set>
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/i18n/case_conversion.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "build/build_config.h"
#include "crFiles/crfiles_trash.h"
#include "third_party/sqlite/sqlite3.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif
namespace crfiles {
namespace {
std::atomic<uint64_t> version{0};
base::Lock& TagsLock() { static base::NoDestructor<base::Lock> lock; return *lock; }
base::FilePath Key(const base::FilePath& path) {
  return path.NormalizePathSeparators().StripTrailingSeparators();
}
class Query {
 public:
  Query(sqlite3* db, const char* sql) {
    if (db && sqlite3_prepare_v2(db, sql, -1, &value_, nullptr) != SQLITE_OK) value_ = nullptr;
  }
  ~Query() { if (value_) sqlite3_finalize(value_); }
  bool Text(int index, const std::string& value) {
    return value_ && sqlite3_bind_text(value_, index, value.data(), value.size(), SQLITE_TRANSIENT) == SQLITE_OK;
  }
  bool Id(int index, int64_t value) { return value_ && sqlite3_bind_int64(value_, index, value) == SQLITE_OK; }
  int Step() { return value_ ? sqlite3_step(value_) : SQLITE_ERROR; }
  int64_t Number(int column) const { return sqlite3_column_int64(value_, column); }
  std::string String(int column) const {
    const auto* text = sqlite3_column_text(value_, column);
    return text ? reinterpret_cast<const char*>(text) : std::string();
  }
 private:
  sqlite3_stmt* value_ = nullptr;
};
class Database {
 public:
  sqlite3* Open() {
    if (db_) return db_;
    if (sqlite3_initialize() != SQLITE_OK) return nullptr;
    const auto trash = TrashRoot();
    if (trash.empty() || !base::CreateDirectory(trash.DirName())) return nullptr;
    const auto path = trash.DirName().AppendASCII("tags.sqlite3").AsUTF8Unsafe();
    sqlite3* opened = nullptr;
    if (sqlite3_open(path.c_str(), &opened) != SQLITE_OK) {
      if (opened) sqlite3_close(opened);
      return nullptr;
    }
    sqlite3_busy_timeout(opened, 5000);
    const char* schema = "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS tags(id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,name_key TEXT NOT NULL UNIQUE);"
        "CREATE TABLE IF NOT EXISTS paths(tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,path TEXT NOT NULL,PRIMARY KEY(tag_id,path));"
        "CREATE INDEX IF NOT EXISTS paths_by_path ON paths(path);";
    if (sqlite3_exec(opened, schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
      sqlite3_close(opened); return nullptr;
    }
    db_ = opened; return db_;
  }
 private:
  sqlite3* db_ = nullptr;
};
sqlite3* DB() { static Database db; return db.Open(); }
bool Exec(sqlite3* db, const char* sql) { return db && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
bool Link(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  const DWORD attributes = GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return base::IsLink(path);
#endif
}
}
uint64_t FileTagsVersion() { return version.load(); }
TagsSnapshot ReadFileTags() {
  base::AutoLock lock(TagsLock());
  TagsSnapshot result;
  auto* db = DB();
  if (!db) return result;
  Query tags(db, "SELECT id,name FROM tags ORDER BY name_key,id");
  int state;
  while ((state = tags.Step()) == SQLITE_ROW) {
    FileTag tag{.id = tags.Number(0), .name = tags.String(1)};
    Query paths(db, "SELECT path FROM paths WHERE tag_id=? ORDER BY path");
    paths.Id(1, tag.id);
    int path_state;
    while ((path_state = paths.Step()) == SQLITE_ROW)
      tag.paths.push_back(base::FilePath::FromUTF8Unsafe(paths.String(0)));
    if (path_state != SQLITE_DONE) return {};
    result.tags.push_back(std::move(tag));
  }
  result.ok = state == SQLITE_DONE;
  return result;
}
TagWriteResult CreateFileTag(std::string name) {
  if (!base::IsStringUTF8(name)) return TagWriteResult::kInvalidName;
  auto text = base::UTF8ToUTF16(name);
  base::TrimWhitespace(text, base::TRIM_ALL, &text);
  if (text.empty() || text.size() > 40 || std::ranges::any_of(text, [](char16_t c) { return c < 32 || c == 127; }))
    return TagWriteResult::kInvalidName;
  base::AutoLock lock(TagsLock());
  Query query(DB(), "INSERT OR IGNORE INTO tags(name,name_key) VALUES(?,?)");
  if (!query.Text(1, base::UTF16ToUTF8(text)) ||
      !query.Text(2, base::UTF16ToUTF8(base::i18n::ToLower(text))) || query.Step() != SQLITE_DONE)
    return TagWriteResult::kError;
  ++version; return TagWriteResult::kOk;
}
TagWriteResult DeleteFileTags(std::vector<int64_t> ids) {
  base::AutoLock lock(TagsLock());
  auto* db = DB();
  if (!Exec(db, "BEGIN IMMEDIATE")) return TagWriteResult::kError;
  for (int64_t id : ids) {
    Query query(db, "DELETE FROM tags WHERE id=?");
    if (id <= 0 || !query.Id(1, id) || query.Step() != SQLITE_DONE) {
      Exec(db, "ROLLBACK"); return TagWriteResult::kError;
    }
  }
  if (Exec(db, "COMMIT")) { ++version; return TagWriteResult::kOk; }
  Exec(db, "ROLLBACK"); return TagWriteResult::kError;
}
TagWriteResult EditFileTags(std::vector<base::FilePath> paths,
                           std::vector<int64_t> add, std::vector<int64_t> remove) {
  base::AutoLock lock(TagsLock());
  auto* db = DB();
  if (!Exec(db, "BEGIN IMMEDIATE")) return TagWriteResult::kError;
  auto result = TagWriteResult::kOk;
  for (const auto& original : paths) {
    const auto path = Key(original);
    if (!path.IsAbsolute() || path.ReferencesParent() || !base::PathExists(path)) { result = TagWriteResult::kError; break; }
    const auto key = path.AsUTF8Unsafe();
    for (int64_t id : remove) {
      Query query(db, "DELETE FROM paths WHERE path=? AND tag_id=?");
      if (!query.Text(1, key) || !query.Id(2, id) || query.Step() != SQLITE_DONE) result = TagWriteResult::kError;
    }
    for (int64_t id : add) {
      Query query(db, "INSERT OR IGNORE INTO paths(tag_id,path) VALUES(?,?)");
      if (!query.Id(1, id) || !query.Text(2, key) || query.Step() != SQLITE_DONE) result = TagWriteResult::kError;
    }
    Query count(db, "SELECT COUNT(*) FROM paths WHERE path=?");
    if (!count.Text(1, key) || count.Step() != SQLITE_ROW) result = TagWriteResult::kError;
    else if (count.Number(0) > 6) result = TagWriteResult::kLimit;
    if (result != TagWriteResult::kOk) break;
  }
  if (result == TagWriteResult::kOk && Exec(db, "COMMIT")) { ++version; return result; }
  Exec(db, "ROLLBACK");
  return result == TagWriteResult::kOk ? TagWriteResult::kError : result;
}
void RemoveFileTagsUnderPath(const base::FilePath& original) {
  const auto root = Key(original);
  if (!root.IsAbsolute() || root.ReferencesParent()) return;
  base::AutoLock lock(TagsLock());
  auto* db = DB();
  if (!Exec(db, "BEGIN IMMEDIATE")) { LOG(ERROR) << "Cannot clean file tags"; return; }
  std::vector<std::string> remove;
  bool ok = true;
  {
    Query paths(db, "SELECT DISTINCT path FROM paths");
    int state;
    while ((state = paths.Step()) == SQLITE_ROW) {
      const auto key = paths.String(0);
      const auto path = base::FilePath::FromUTF8Unsafe(key);
      if (root == path || root.IsParent(path)) remove.push_back(key);
    }
    ok = state == SQLITE_DONE;
  }
  for (const auto& path : remove) {
    Query query(db, "DELETE FROM paths WHERE path=?");
    if (!query.Text(1, path) || query.Step() != SQLITE_DONE) ok = false;
  }
  if (ok && Exec(db, "COMMIT")) { if (!remove.empty()) ++version; return; }
  Exec(db, "ROLLBACK"); LOG(ERROR) << "Cannot clean file tags";
}
TagSize CalculateTagSize(std::vector<base::FilePath> paths,
                         const std::shared_ptr<std::atomic_bool>& canceled) {
  TagSize result;
  std::ranges::sort(paths);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  std::vector<base::FilePath> roots;
  for (const auto& path : paths) {
    if (canceled->load()) return result;
    base::File::Info info;
    if (!base::GetFileInfo(path, &info)) { result.incomplete = true; continue; }
    ++result.available;
    if (!std::ranges::any_of(roots, [&](const auto& root) { return root == path || root.IsParent(path); })) roots.push_back(path);
  }
  const auto accumulate = [&](int64_t size) {
    if (size > 0) result.bytes += std::min(static_cast<uint64_t>(size), std::numeric_limits<uint64_t>::max() - result.bytes);
  };
  for (const auto& root : roots) {
    if (canceled->load()) return result;
    base::File::Info info;
    if (Link(root) || !base::GetFileInfo(root, &info)) { result.incomplete = true; continue; }
    if (!info.is_directory) { accumulate(info.size); continue; }
    std::vector<std::unique_ptr<base::FileEnumerator>> stack;
    const auto push = [&](const base::FilePath& path) {
      int flags = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
      flags |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
      stack.push_back(std::make_unique<base::FileEnumerator>(path, false, flags,
          base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
          base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION));
    };
    push(root);
    while (!stack.empty()) {
      if (canceled->load()) return result;
      const auto path = stack.back()->Next();
      if (path.empty()) {
        if (stack.back()->GetError() != base::File::FILE_OK) result.incomplete = true;
        stack.pop_back(); continue;
      }
      if (Link(path)) { result.incomplete = true; continue; }
      const auto metadata = stack.back()->GetInfo();
      if (metadata.IsDirectory()) push(path);
      else accumulate(metadata.GetSize());
    }
  }
  return result;
}
}
