// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crFiles/sharing/sync_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <tuple>
#include "crFiles/sharing/sync_files.h"
#include "crFiles/crfiles_file_access.h"
#include "crFiles/grit/crfiles_strings.h"
#include "base/base64.h"
#include "base/files/file_path_watcher.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/case_conversion.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "crypto/hash.h"
#include "third_party/sqlite/sqlite3.h"

namespace crfiles {
namespace {
constexpr size_t kMaxEntries = 100000;
constexpr size_t kMaxJobs = 128;
constexpr size_t kMaxRegistrations = 4096;
constexpr uint64_t kMaxCounter = (1ULL << 62);
struct Entry : SyncContent {
  std::array<uint64_t, 2> version{};
};
using Index = std::map<std::string, Entry>;
std::string RandomId() { return base::HexEncode(base::RandBytesAsString(16)); }
std::string Json(const base::DictValue& value) {
  std::string text;
  base::JSONWriter::Write(value, &text);
  return text;
}
base::DictValue Message(const char* type) {
  base::DictValue value; value.Set("type", type); return value;
}
bool Type(const std::optional<base::DictValue>& value, const char* type) {
  const auto* name = value ? value->FindString("type") : nullptr;
  return name && *name == type;
}
bool Number(const base::DictValue& value, const char* key, uint64_t* output) {
  const auto* text = value.FindString(key);
  return text && !text->empty() && text->size() <= 19 &&
      std::ranges::all_of(*text, [](char c) { return c >= '0' && c <= '9'; }) &&
      base::StringToUint64(*text, output) && *output < kMaxCounter;
}
base::DictValue Encode(const Entry& e) {
  base::DictValue value;
  value.Set("kind", e.kind); value.Set("size", base::NumberToString(e.size));
  value.Set("hash", e.hash);
  value.Set("a", base::NumberToString(e.version[0]));
  value.Set("b", base::NumberToString(e.version[1]));
  return value;
}
bool Decode(const base::DictValue& value, Entry* e) {
  e->kind = value.FindInt("kind").value_or(-1);
  const auto* hash = value.FindString("hash");
  if (!hash || e->kind < 0 || e->kind > 2 || !Number(value, "size", &e->size) ||
      !Number(value, "a", &e->version[0]) || !Number(value, "b", &e->version[1])) return false;
  e->hash = *hash;
  if (e->kind != 1) return e->size == 0 && hash->empty();
  return e->size <= 16ULL * 1024 * 1024 * 1024 && hash->size() == 64 &&
      std::ranges::all_of(*hash, [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); });
}
Entry At(const Index& index, const std::string& path) {
  auto it = index.find(path); return it == index.end() ? Entry() : it->second;
}
bool Same(const Entry& a, const Entry& b) {
  return static_cast<const SyncContent&>(a) == static_cast<const SyncContent&>(b);
}
bool Dominates(const Entry& a, const Entry& b) {
  return a.version[0] >= b.version[0] && a.version[1] >= b.version[1] && a.version != b.version;
}
std::array<uint64_t, 2> Merge(const Entry& a, const Entry& b) {
  return {std::max(a.version[0], b.version[0]), std::max(a.version[1], b.version[1])};
}
std::string Parent(const std::string& path) {
  const auto slash = path.rfind('/'); return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
struct Planned {
  Entry result;
  int donor = 0;
  std::string source;
  std::string operation;
};
using Plan = std::map<std::string, Planned>;
std::string ConflictName(const std::string& path, const Entry& loser) {
  const auto parent = Parent(path);
  return (parent.empty() ? "" : parent + "/") + "sync-conflict-" + SyncHash(path).substr(0, 16) +
      "-" + SyncHash(Json(Encode(loser))).substr(0, 24);
}
bool Conflict(Plan* plan, const std::string& path, const Entry& value, int donor) {
  if (value.kind != 1) return false;
  auto name = ConflictName(path, value);
  Entry result = value;
  for (size_t attempt = 0; attempt < 8; ++attempt) {
    auto it = plan->find(name);
    if (it == plan->end() || Same(it->second.result, value)) {
      if (it != plan->end()) result.version = Merge(result, it->second.result);
      (*plan)[name] = {result, donor, path, {}};
      return true;
    }
    name = ConflictName(path, value) + "-" + SyncHash(name + Json(Encode(it->second.result))).substr(0, 16);
  }
  return false;
}
bool MakePlan(const Index& a, const Index& b, Plan* plan) {
  std::set<std::string> paths;
  for (const auto& [path, value] : a) paths.insert(path);
  for (const auto& [path, value] : b) paths.insert(path);
  std::vector<std::tuple<std::string, Entry, int>> conflicts;
  for (const auto& path : paths) {
    const auto left = At(a, path), right = At(b, path);
    int donor = 0;
    if (Same(left, right) || Dominates(left, right)) donor = 0;
    else if (Dominates(right, left)) donor = 1;
    else {
      // Concurrent delete/edit preserves the edit. A directory wins a concurrent
      // type conflict; a regular-file loser becomes a separately synced copy.
      if (!left.kind || right.kind == 2) donor = 1;
      const auto& loser = donor ? left : right;
      if (left.kind && right.kind && loser.kind == 1)
        conflicts.emplace_back(path, loser, 1 - donor);
    }
    auto result = donor ? right : left;
    result.version = Merge(left, right);
    plan->emplace(path, Planned{result, donor, path, {}});
  }
  for (const auto& [path, value, donor] : conflicts)
    if (!Conflict(plan, path, value, donor)) return false;
  // A new/modified descendant must survive a concurrent ancestor deletion or
  // replacement. Recreate its ancestors and retain a displaced regular file.
  std::vector<std::string> live;
  for (const auto& [path, item] : *plan) if (item.result.kind) live.push_back(path);
  for (const auto& path : live) {
    for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent)) {
      auto& ancestor = (*plan)[parent];
      if (ancestor.result.kind == 1 && !Conflict(plan, parent, ancestor.result, ancestor.donor)) return false;
      ancestor.result.kind = 2; ancestor.result.size = 0; ancestor.result.hash.clear();
      ancestor.result.version = Merge(ancestor.result, plan->at(path).result);
    }
  }
  std::set<std::u16string> folded;
  for (const auto& [path, value] : *plan) {
    if (!SafeSyncPath(path) || !folded.insert(base::i18n::FoldCase(base::UTF8ToUTF16(path))).second) return false;
  }
  return plan->size() <= kMaxEntries;
}
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (db) sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr);
  }
  ~Statement() { if (statement_) sqlite3_finalize(statement_); }
  bool Text(int i, const std::string& text) {
    return statement_ && text.size() <= static_cast<size_t>(std::numeric_limits<int>::max()) &&
        sqlite3_bind_text(statement_, i, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT) == SQLITE_OK;
  }
  int Step() { return statement_ ? sqlite3_step(statement_) : SQLITE_ERROR; }
  std::string Column(int i) {
    const auto* value = sqlite3_column_text(statement_, i);
    return value ? reinterpret_cast<const char*>(value) : "";
  }
 private:
  sqlite3_stmt* statement_ = nullptr;
};
struct Dirty {
  std::atomic<bool> value{true};
  std::atomic<int64_t> changed{0};
};
class Watch {
 public:
  Watch(base::FilePath path, bool recursive, std::shared_ptr<Dirty> dirty) : dirty_(std::move(dirty)) {
    if (!watcher_.Watch(path, recursive ? base::FilePathWatcher::Type::kRecursive :
        base::FilePathWatcher::Type::kNonRecursive,
        base::BindRepeating(&Watch::Changed, base::Unretained(this))))
      LOG(WARNING) << "[crFiles sync] watch_failed path=" << path << " fallback_seconds=10";
  }
 private:
  void Changed(const base::FilePath& path, bool error) {
    if (error) LOG(WARNING) << "[crFiles sync] watch_error path=" << path << " fallback_seconds=10";
    dirty_->changed = base::TimeTicks::Now().ToInternalValue();
    dirty_->value = true;
  }
  std::shared_ptr<Dirty> dirty_;
  base::FilePathWatcher watcher_;
};
// The holder's watcher is constructed and destroyed only on watcher_runner.
// Its shared owner can move between successive desktop transport workers.
struct WatchHolder { std::unique_ptr<Watch> watch; };
std::shared_ptr<WatchHolder> StartWatch(scoped_refptr<base::SequencedTaskRunner> runner,
                                      base::FilePath path, bool recursive, std::shared_ptr<Dirty> dirty) {
  auto holder = std::shared_ptr<WatchHolder>(new WatchHolder, [runner](WatchHolder* value) {
    runner->DeleteSoon(FROM_HERE, value);
  });
  runner->PostTask(FROM_HERE, base::BindOnce([](std::shared_ptr<WatchHolder> holder,
      base::FilePath path, bool recursive, std::shared_ptr<Dirty> dirty) {
    holder->watch = std::make_unique<Watch>(std::move(path), recursive, std::move(dirty));
  }, holder, std::move(path), recursive, std::move(dirty)));
  return holder;
}
struct Relationship {
  SyncJob view;
  std::string single;
  std::string identity;
  std::string bookmark;
  int role = 0;
  bool confirmed = false;
  bool removal_pending = false;
  uint64_t counter = 0;
  uint64_t acknowledged_counter = 0;
  bool connection_pending = true;
  bool receive_pending = false;
  Index index;
  std::shared_ptr<Dirty> dirty = std::make_shared<Dirty>();
  base::TimeTicks scanned;
  base::TimeTicks next;
  base::TimeTicks observed_change;
  base::TimeTicks burst_started;
  base::TimeTicks change_due;
  base::TimeTicks retry_not_before;
  bool wake_pending = false;
  bool remote_wake = false;
  int failures = 0;
  std::shared_ptr<WatchHolder> watcher;
};
bool DisjointSingleFiles(const std::string& first, const std::string& second) {
  if (first.empty() || second.empty() || first.find('/') != std::string::npos ||
      second.find('/') != std::string::npos) return false;
  const auto fold = [](const std::string& name) {
    return base::i18n::FoldCase(base::UTF8ToUTF16(name));
  };
  const auto a = fold(first), b = fold(second);
  const auto a_conflicts = fold("sync-conflict-" + SyncHash(first).substr(0, 16) + "-");
  const auto b_conflicts = fold("sync-conflict-" + SyncHash(second).substr(0, 16) + "-");
  // Each single-file scope includes its generated conflict copies. Keep those
  // namespaces disjoint too, including on case-insensitive filesystems.
  return a != b && a_conflicts != b_conflicts &&
      !a.starts_with(b_conflicts) && !b.starts_with(a_conflicts);
}
}  // namespace

bool IsSyncId(const std::string& value) {
  return value.size() == 32 && std::ranges::all_of(value, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
  });
}
bool IsSyncErrorStatus(int status) {
  return status == IDS_CRFILES_TRANSFER_EXISTS_ERROR || status == IDS_CRFILES_SYNC_SOURCE_ERROR || status == IDS_CRFILES_SYNC_STORAGE_ERROR ||
      status == IDS_CRFILES_SYNC_RECOVERY_REQUIRED || status == IDS_CRFILES_SYNC_OVERLAP_ERROR ||
      status == IDS_CRFILES_SYNC_PREPARE_ERROR || status == IDS_CRFILES_SYNC_LIMIT_ERROR ||
      status == IDS_CRFILES_SYNC_DEVICE_ERROR || status == IDS_CRFILES_SYNC_REQUEST_ERROR;
}
class SyncService::Impl {
 public:
  explicit Impl(base::FilePath directory) : directory(std::move(directory)) {}
  ~Impl() { if (db) sqlite3_close(db); }
  struct Command {
    int kind; std::string id; base::FilePath source; std::string fingerprint; std::string peer; bool paused = false;
    int avatar = 0; bool read = true; bool write = false;
  };
  mutable base::Lock ui_lock;
  base::Lock io_lock;
  std::vector<Command> commands;
  std::vector<SyncJob> snapshot;
  std::vector<SyncWakeup> wakeups;
  std::map<std::string, std::string> wake_authorized;
  std::map<std::string, std::string> remote_wakes;
  std::map<std::string, base::TimeTicks> wake_attempts;
  base::FilePath directory;
  sqlite3* db = nullptr;
  bool loaded = false;
  std::unique_ptr<SyncFiles> database_lock;
  std::map<std::string, std::unique_ptr<Relationship>> jobs;
  scoped_refptr<base::SequencedTaskRunner> watcher_runner;
  bool DatabaseFailure(const char* stage) {
    LOG(ERROR) << "[crFiles sync] database_failed stage=" << stage << " directory=" << directory
               << " sqlite_code=" << (db ? sqlite3_extended_errcode(db) : SQLITE_ERROR)
               << " sqlite_message=" << (db ? sqlite3_errmsg(db) : "database unavailable");
    return false;
  }
  bool Exec(const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK) return true;
    return DatabaseFailure("execute");
  }
  bool Open() {
    if (loaded) return true;
    if (db) { sqlite3_close(db); db = nullptr; }
    base::File::Error directory_error;
    if (!base::CreateDirectoryAndGetError(directory, &directory_error)) {
      LOG(ERROR) << "[crFiles sync] database_failed stage=create_directory directory=" << directory
                 << " file_error=" << directory_error;
      return false;
    }
    const auto canonical_directory = base::MakeAbsoluteFilePath(directory);
    if (canonical_directory.empty()) {
      PLOG(ERROR) << "[crFiles sync] database_failed stage=canonicalize_directory directory=" << directory;
      return false;
    }
    directory = canonical_directory;
    database_lock = std::make_unique<SyncFiles>(directory, "database", [] { return false; });
    if (!database_lock->Open()) return DatabaseFailure("prepare_directory");
    const int initialized = sqlite3_initialize();
    if (initialized != SQLITE_OK) {
      LOG(ERROR) << "[crFiles sync] database_failed stage=initialize sqlite_code=" << initialized;
      return false;
    }
    if (sqlite3_open(directory.AppendASCII("sync.sqlite3").AsUTF8Unsafe().c_str(), &db) != SQLITE_OK)
      return DatabaseFailure("open");
    sqlite3_busy_timeout(db, 5000);
    {
      Statement version(db, "PRAGMA user_version");
      if (version.Step() != SQLITE_ROW) return DatabaseFailure("read_version");
      if (version.Column(0) != "0" && version.Column(0) != "1" && version.Column(0) != "2" && version.Column(0) != "3" && version.Column(0) != "4")
        return DatabaseFailure("unsupported_version");
    }
    if (!Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS relations(id TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS entries(id TEXT,path TEXT,value TEXT NOT NULL,PRIMARY KEY(id,path));"
        "CREATE TABLE IF NOT EXISTS pending(id TEXT,path TEXT,value TEXT NOT NULL,PRIMARY KEY(id,path)); PRAGMA user_version=4;")) return false;
    jobs.clear();
    Statement rows(db, "SELECT id,value FROM relations");
    int step;
    while ((step = rows.Step()) == SQLITE_ROW) {
      auto value = base::JSONReader::ReadDict(rows.Column(1), base::JSON_PARSE_RFC);
      auto r = std::make_unique<Relationship>();
      r->view.id = rows.Column(0);
      const auto invalid = [&](const char* stage) {
        LOG(ERROR) << "[crFiles sync] database_record_failed id=" << r->view.id << " stage=" << stage;
        return DatabaseFailure(stage);
      };
      if (!value || !IsSyncId(r->view.id)) return invalid("relation_json_or_id");
      const auto* root = value->FindString("root"); const auto* pin = value->FindString("peer");
      const auto* name = value->FindString("name"); const auto* identity = value->FindString("identity");
      const auto* single = value->FindString("single"); const auto* bookmark = value->FindString("bookmark");
      r->role = value->FindInt("role").value_or(-1);
      if (!root || !pin || pin->size() != 64 || !name || !identity || !single || !bookmark ||
          (r->role != 0 && r->role != 1) || !Number(*value, "counter", &r->counter)) return invalid("relation_fields");
      const bool has_acknowledgement = value->contains("acknowledged_counter");
      if ((has_acknowledgement && (!Number(*value, "acknowledged_counter", &r->acknowledged_counter) ||
              r->acknowledged_counter > r->counter)) ||
          (value->contains("connection_pending") && !value->FindBool("connection_pending")))
        return invalid("relation_acknowledgement");
      // Old records have no durable delivery baseline. Reconcile once before
      // allowing an unchanged one-way relationship to stop connecting.
      r->connection_pending = !has_acknowledgement || value->FindBool("connection_pending").value_or(true);
      r->view.root = base::FilePath::FromUTF8Unsafe(*root);
      if (!r->view.root.IsAbsolute() || r->view.root.ReferencesParent() || (!single->empty() && !SafeSyncPath(*single))) return invalid("relation_path");
      r->view.fingerprint = *pin; r->view.peer = *name; r->identity = *identity; r->single = *single; r->bookmark = *bookmark;
      r->view.paused = value->FindBool("paused").value_or(true);
      r->view.removed = value->FindBool("removed").value_or(true);
      if (value->contains("removal_pending") && !value->FindBool("removal_pending")) return invalid("relation_removal_state");
      // Upgrade old tombstones too: their remote list entries may still exist.
      r->removal_pending = r->view.removed && value->FindBool("removal_pending").value_or(true);
      r->confirmed = value->FindBool("confirmed").value_or(false);
      if (value->contains("receive_pending") && !value->FindBool("receive_pending")) return invalid("relation_receive_pending");
      r->receive_pending = value->FindBool("receive_pending").value_or(false);
      if (r->receive_pending && r->role != 1) return invalid("relation_receive_role");
      r->view.name = value->FindString("item_name") ? *value->FindString("item_name") :
          (r->single.empty() ? r->view.root.BaseName().AsUTF8Unsafe() : r->single);
      r->view.avatar = std::clamp(value->FindInt("avatar").value_or(0), 0, 8);
      // A retains its grants. B's legacy local grants are not A's policy:
      // wait for the authenticated creator instead of displaying invented values.
      const auto permissions = value->FindInt("permissions");
      if ((value->contains("permissions") && (!permissions || (*permissions != 0 && *permissions != 2))) ||
          (permissions == 2 && (!value->FindBool("read") || !value->FindBool("write")))) return invalid("relation_permissions");
      r->view.permissions_known = r->role == 0 || permissions == 2;
      r->view.read = r->view.permissions_known && value->FindBool("read").value_or(true);
      r->view.write = r->view.permissions_known && value->FindBool("write").value_or(true);
      uint64_t started = 0, synced = 0;
      if ((value->contains("started") && !Number(*value, "started", &started)) ||
          (value->contains("last_synced") && !Number(*value, "last_synced", &synced))) return invalid("relation_times");
      r->view.started = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(started));
      r->view.last_synced = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(synced));
      r->view.status = IDS_CRFILES_SYNC_WAITING;
      r->view.recovery = r->view.root.AppendASCII(".crfiles-sync").AppendASCII(r->view.id);
      if (jobs.size() >= kMaxRegistrations) return invalid("relation_limit");
      if (r->view.removed) { jobs.emplace(r->view.id, std::move(r)); continue; }
      Statement entries(db, "SELECT path,value FROM entries WHERE id=?");
      if (!entries.Text(1, r->view.id)) return invalid("entries_bind");
      int entry_step;
      while ((entry_step = entries.Step()) == SQLITE_ROW) {
        auto entry = base::JSONReader::ReadDict(entries.Column(1), base::JSON_PARSE_RFC);
        Entry parsed; const auto path = entries.Column(0);
        if (!SafeSyncPath(path) || !entry || !Decode(*entry, &parsed) || r->index.size() >= kMaxEntries) return invalid("entry_decode");
        r->index.emplace(path, parsed);
      }
      if (entry_step != SQLITE_DONE || jobs.size() >= kMaxRegistrations) return invalid("entries_read_or_limit");
      jobs.emplace(r->view.id, std::move(r));
    }
    if (step != SQLITE_DONE) return DatabaseFailure("read_relations");
    loaded = true;
    return true;
  }
  bool Save(const Relationship& r) {
    base::DictValue value;
    value.Set("root", r.view.root.AsUTF8Unsafe()); value.Set("peer", r.view.fingerprint);
    value.Set("name", r.view.peer); value.Set("single", r.single); value.Set("identity", r.identity);
    value.Set("bookmark", r.bookmark); value.Set("role", r.role); value.Set("confirmed", r.confirmed);
    value.Set("receive_pending", r.receive_pending);
    value.Set("paused", r.view.paused); value.Set("removed", r.view.removed);
    value.Set("removal_pending", r.removal_pending);
    value.Set("counter", base::NumberToString(r.counter));
    value.Set("acknowledged_counter", base::NumberToString(r.acknowledged_counter));
    value.Set("connection_pending", r.connection_pending);
    value.Set("item_name", r.view.name); value.Set("avatar", r.view.avatar);
    value.Set("read", r.view.read); value.Set("write", r.view.write);
    value.Set("permissions", r.view.permissions_known ? 2 : 0);
    value.Set("started", base::NumberToString(r.view.started.ToDeltaSinceWindowsEpoch().InMicroseconds()));
    value.Set("last_synced", base::NumberToString(r.view.last_synced.ToDeltaSinceWindowsEpoch().InMicroseconds()));
    Statement row(db, "INSERT OR REPLACE INTO relations VALUES(?,?)");
    if (row.Text(1, r.view.id) && row.Text(2, Json(value)) && row.Step() == SQLITE_DONE) return true;
    LOG(ERROR) << "[crFiles sync] relationship_save_failed id=" << r.view.id;
    return DatabaseFailure("save_relationship");
  }
  bool SaveEntry(const Relationship& r, const std::string& path, const Entry& value) {
    Statement row(db, "INSERT OR REPLACE INTO entries VALUES(?,?,?)");
    return row.Text(1, r.view.id) && row.Text(2, path) && row.Text(3, Json(Encode(value))) && row.Step() == SQLITE_DONE;
  }
  bool SaveIndex(const Relationship& r) {
    if (!Exec("BEGIN IMMEDIATE")) return false;
    bool ok = Save(r);
    for (const auto& [path, entry] : r.index) if (ok) ok = SaveEntry(r, path, entry);
    if (ok && Exec("COMMIT")) return true;
    Exec("ROLLBACK"); return false;
  }
  void Publish() {
    std::vector<SyncJob> views;
    std::vector<SyncWakeup> pending_wakes;
    std::map<std::string, std::string> authorized;
    for (auto& [id, r] : jobs) {
      if (loaded && r->confirmed && !r->view.paused && !r->view.removed && r->view.write) {
        if (r->role == 0) authorized.emplace(id, r->view.fingerprint);
        else if (r->wake_pending && r->counter > r->acknowledged_counter &&
            r->retry_not_before <= base::TimeTicks::Now())
          pending_wakes.push_back({id, r->view.fingerprint, r->counter});
      }
      if (r->view.removed && r->view.status != IDS_CRFILES_SYNC_STORAGE_ERROR) continue;
      auto view = r->view; view.creator = r->role == 0;
      if (view.paused && !IsSyncErrorStatus(view.status)) view.status = IDS_CRFILES_SYNC_PAUSED;
      views.push_back(std::move(view));
      if (!r->watcher && !r->view.paused) {
        if (!watcher_runner) watcher_runner = base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()});
        r->watcher = StartWatch(watcher_runner,
            r->single.empty() ? r->view.root : r->view.root.Append(base::FilePath::FromUTF8Unsafe(r->single)),
            r->single.empty(), r->dirty);
      }
      if (r->view.paused) r->watcher.reset();
    }
    base::AutoLock lock(ui_lock); snapshot = std::move(views);
    wakeups = std::move(pending_wakes); wake_authorized = std::move(authorized);
    std::erase_if(wake_attempts, [&](const auto& attempt) {
      return std::ranges::none_of(wakeups, [&](const auto& wake) { return wake.id == attempt.first; });
    });
  }
  bool Overlaps(const base::FilePath& root, const std::string& ignore_id = {}, const std::string& single = {}) {
    // Different single-file scopes may share a parent. Root locks are held only
    // during serialized I/O, and recovery directories are keyed by relationship.
    // Keep directory and ancestor/descendant root exclusions conservative.
    for (const auto& [id, r] : jobs) if (id != ignore_id && !r->view.removed && !r->view.fingerprint.empty() &&
        (r->view.root == root || r->view.root.IsParent(root) || root.IsParent(r->view.root))) {
      if (r->view.root == root && DisjointSingleFiles(single, r->single)) continue;
      LOG(ERROR) << "[crFiles sync] overlap root=" << root << " single=" << single << " existing_id=" << id
                 << " existing_root=" << r->view.root << " existing_single=" << r->single << " paused=" << r->view.paused;
      return true;
    }
    // A single file uses its parent for pinning/recovery, but only indexes that
    // file and its conflict copies. In particular, ~/file.txt does not include the
    // database under ~/.config or ~/Library in its synchronization scope.
    // Files inside the database directory remain forbidden.
    if (root == directory || directory.IsParent(root) || (single.empty() && root.IsParent(directory))) {
      LOG(ERROR) << "[crFiles sync] overlap_database root=" << root << " database_root=" << directory
                 << " single=" << single;
      return true;
    }
    return false;
  }
  bool Full() const {
    return jobs.size() >= kMaxRegistrations || static_cast<size_t>(std::ranges::count_if(jobs, [](const auto& pair) {
      return !pair.second->view.removed && !pair.second->view.fingerprint.empty();
    })) >= kMaxJobs;
  }
  int Create(Command command) {
    const auto fail = [&](const char* stage, int error) {
      LOG(ERROR) << "[crFiles sync] create_failed id=" << command.id << " stage=" << stage
                 << " source=" << command.source;
      return error;
    };
    if (Full()) return fail("relationship_limit", IDS_CRFILES_SYNC_LIMIT_ERROR);
    auto r = std::make_unique<Relationship>();
    r->view.id = command.id; r->view.fingerprint = command.fingerprint; r->view.peer = command.peer;
    base::File::Info info;
    if (base::IsLink(command.source)) return fail("source_is_link", IDS_CRFILES_SYNC_SOURCE_ERROR);
    if (!base::GetFileInfo(command.source, &info)) {
      PLOG(ERROR) << "[crFiles sync] source_stat_failed id=" << command.id << " source=" << command.source;
      return fail("source_stat", IDS_CRFILES_SYNC_SOURCE_ERROR);
    }
    if (info.is_symbolic_link) return fail("source_is_link", IDS_CRFILES_SYNC_SOURCE_ERROR);
    r->view.root = base::MakeAbsoluteFilePath(info.is_directory ? command.source : command.source.DirName());
    if (r->view.root.empty()) {
      PLOG(ERROR) << "[crFiles sync] source_canonicalize_failed id=" << command.id;
      return fail("source_canonicalize", IDS_CRFILES_SYNC_SOURCE_ERROR);
    }
    r->single = info.is_directory ? "" : CanonicalSyncPath(command.source.BaseName().AsUTF8Unsafe());
    r->view.name = CanonicalSyncPath(command.source.BaseName().AsUTF8Unsafe());
    r->view.avatar = std::clamp(command.avatar, 0, 8); r->view.started = base::Time::Now();
    r->view.read = command.read; r->view.write = command.write; r->view.permissions_known = true;
    if (!SafeSyncPath(r->view.name) || (!r->single.empty() && !SafeSyncPath(r->single)))
      return fail("source_name", IDS_CRFILES_SYNC_SOURCE_ERROR);
    if (Overlaps(r->view.root, {}, r->single))
      return fail("source_overlap", IDS_CRFILES_SYNC_OVERLAP_ERROR);
    SyncFiles files(r->view.root, r->view.id, [] { return false; });
    if (!files.Open()) return fail("source_prepare", IDS_CRFILES_SYNC_PREPARE_ERROR);
    r->identity = files.Identity(); r->view.recovery = files.Recovery();
    for (const auto& [id, existing] : jobs)
      if (!existing->view.removed && existing->identity == r->identity &&
          !DisjointSingleFiles(r->single, existing->single)) {
        LOG(ERROR) << "[crFiles sync] overlap_identity id=" << command.id << " single=" << r->single
                   << " existing_id=" << id << " existing_single=" << existing->single;
        return fail("source_identity_overlap", IDS_CRFILES_SYNC_OVERLAP_ERROR);
      }
    r->bookmark = base::Base64Encode(CreateFileAccessBookmark(r->view.root));
    r->view.status = IDS_CRFILES_SYNC_WAITING;
    if (!Save(*r)) return fail("relationship_save", IDS_CRFILES_SYNC_STORAGE_ERROR);
    jobs.emplace(r->view.id, std::move(r)); return 0;
  }
  void Poll() {
    if (!Open()) {
      base::AutoLock lock(ui_lock);
      wakeups.clear(); wake_authorized.clear(); remote_wakes.clear();
      // Include a service-wide error for newly queued requests that could not
      // be registered, even when older relationships already have snapshots.
      if (std::ranges::none_of(snapshot, [](const auto& job) { return job.id.empty(); })) snapshot.emplace_back();
      for (auto& job : snapshot) job.status = IDS_CRFILES_SYNC_STORAGE_ERROR;
      return;
    }
    std::vector<Command> pending;
    { base::AutoLock lock(ui_lock); pending.swap(commands); }
    for (auto& command : pending) {
      if (command.kind == 0) {
        const int error = Create(command);
        if (error) {
          // Keep a visible, dismissible failed request instead of silently losing
          // a rejected overlap, invalid name, or unavailable source.
          auto r = std::make_unique<Relationship>(); r->view.id = command.id;
          r->view.root = command.source; r->view.peer = command.peer;
          r->view.name = command.source.BaseName().AsUTF8Unsafe();
          r->view.avatar = std::clamp(command.avatar, 0, 8);
          r->view.read = command.read; r->view.write = command.write; r->view.permissions_known = true;
          r->view.status = error; r->view.paused = true;
          jobs.emplace(command.id, std::move(r));
        }
        continue;
      }
      bool retry = false;
      for (auto& [id, r] : jobs) {
        if (command.kind == 3 ? r->view.fingerprint != command.fingerprint : id != command.id) continue;
        if (command.kind == 5 && r->view.fingerprint != command.fingerprint) continue;
        const bool old_removed = r->view.removed;
        if (command.kind == 6) {
          if (r->role != 0 || r->view.removed || r->view.fingerprint != command.fingerprint) continue;
          r->view.paused = false; r->view.read = true; r->view.write = command.write;
          r->view.permissions_known = true;
        } else if (command.kind == 4) {
          // UI disabling is not authorization: only the creator can edit policy.
          if (r->role != 0 || r->view.removed) continue;
          r->view.read = command.read; r->view.write = command.write; r->view.permissions_known = true;
        } else r->view.paused = command.kind == 1 ? command.paused : true;
        r->view.removed = old_removed || command.kind == 2 || command.kind == 3 || command.kind == 5;
        if (!old_removed && (command.kind == 2 || command.kind == 3)) r->removal_pending = true;
        if (command.kind == 5) r->removal_pending = false;
        if (!r->view.removed && (command.kind == 4 || command.kind == 6 ||
            (command.kind == 1 && !command.paused))) r->connection_pending = true;
        if (r->view.removed) r->watcher.reset();
        if (!r->view.fingerprint.empty() && !Save(*r)) {
          // A failed durable write must never leave a requested pause/revoke
          // active in this process. Keep the command for a later storage retry.
          r->view.paused = true;
          r->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR;
          retry = true;
        } else {
          r->dirty->value = true; r->next = {}; r->failures = 0;
          r->retry_not_before = {}; r->burst_started = {}; r->change_due = {};
          r->wake_pending = false; r->remote_wake = false;
          r->view.status = IDS_CRFILES_SYNC_WAITING;
          if (r->view.removed) r->watcher.reset();
        }
      }
      if (retry) { base::AutoLock lock(ui_lock); commands.push_back(command); }
    }
    std::erase_if(jobs, [](const auto& pair) {
      return pair.second->view.removed && pair.second->view.fingerprint.empty();
    });
    std::map<std::string, std::string> wakes;
    { base::AutoLock lock(ui_lock); wakes.swap(remote_wakes); }
    for (const auto& [id, fingerprint] : wakes) {
      const auto it = jobs.find(id);
      if (it == jobs.end()) continue;
      auto& r = *it->second;
      if (r.role == 0 && r.confirmed && r.view.write && !r.view.paused && !r.view.removed &&
          r.view.fingerprint == fingerprint) r.remote_wake = true;
    }
    Publish();
  }
  bool ReceiveRemoval(const std::string& id, const std::string& fingerprint) {
    const auto it = jobs.find(id);
    // An unknown ID has no files/relationship to remove. Acknowledge without
    // creating records; unaffiliated peers cannot fill the database with IDs.
    if (it == jobs.end()) return true;
    auto& r = *it->second;
    if (r.view.fingerprint != fingerprint) return false;
    r.view.paused = true; r.view.removed = true; r.removal_pending = false;
    r.watcher.reset(); r.next = {}; r.failures = 0;
    const bool saved = Save(r);
    r.view.status = saved ? IDS_CRFILES_SYNC_WAITING : IDS_CRFILES_SYNC_STORAGE_ERROR;
    if (!saved) {
      // Stop locally even on disk errors, retry the durable tombstone, and do
      // not acknowledge the peer until it can be saved. No synced files are opened.
      base::AutoLock lock(ui_lock);
      if (std::ranges::none_of(commands, [&](const auto& command) { return command.kind == 5 && command.id == id; }))
        commands.push_back({5, id, {}, fingerprint, {}});
    }
    Publish(); return saved;
  }
  bool RemovalAllowed(const std::string& id, const std::string& fingerprint, bool outgoing) {
    if (!loaded || !IsSyncId(id) || fingerprint.size() != 64) return false;
    const auto it = jobs.find(id);
    if (it == jobs.end()) return !outgoing;
    return it->second->view.fingerprint == fingerprint &&
        (!outgoing || (it->second->view.removed && it->second->removal_pending));
  }
  bool RemoveOverWire(SyncWire& wire, bool outgoing, const std::string& id, const std::string& fingerprint) {
    Poll();
    if (!RemovalAllowed(id, fingerprint, outgoing) || wire.Canceled()) return false;
    if (outgoing) {
      auto& r = *jobs.at(id);
      // A local removal must be durable before notifying another device.
      if (!Save(r)) { r.view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; Publish(); return false; }
      auto request = Message("sync-remove"); request.Set("id", id);
      if (!wire.Write(std::move(request))) return false;
      const auto reply = wire.Read();
      const auto* received_id = reply ? reply->FindString("id") : nullptr;
      if (!Type(reply, "sync-removed") || !received_id || *received_id != id || wire.Canceled()) return false;
      r.removal_pending = false;
      if (!Save(r)) {
        // Lost local acknowledgement state is safe: the peer handles retries.
        r.removal_pending = true; r.view.status = IDS_CRFILES_SYNC_STORAGE_ERROR;
        Publish(); return false;
      }
      r.view.status = IDS_CRFILES_SYNC_WAITING; r.failures = 0; Publish(); return true;
    }
    const auto request = wire.Read();
    const auto* received_id = request ? request->FindString("id") : nullptr;
    if (!Type(request, "sync-remove") || !received_id || *received_id != id || wire.Canceled() ||
        !ReceiveRemoval(id, fingerprint)) return false;
    auto reply = Message("sync-removed"); reply.Set("id", id);
    return wire.Write(std::move(reply));
  }
  bool Interrupted(const std::string& id, SyncWire& wire) {
    return wire.Canceled() || HasPendingCommand(id);
  }
  bool HasPendingCommand(const std::string& id) {
    base::AutoLock lock(ui_lock);
    return std::ranges::any_of(commands, [&](const auto& command) {
      return (command.id == id && command.kind != 0) ||
          (command.kind == 3 && jobs.at(id)->view.fingerprint == command.fingerprint);
    });
  }
  bool Recover(Relationship& r, SyncFiles& files) {
    Statement rows(db, "SELECT path,value FROM pending WHERE id=? ORDER BY path");
    if (!rows.Text(1, r.view.id)) return false;
    std::vector<std::pair<std::string, base::DictValue>> pending;
    int step;
    while ((step = rows.Step()) == SQLITE_ROW) {
      auto value = base::JSONReader::ReadDict(rows.Column(1), base::JSON_PARSE_RFC);
      if (!value || !value->FindDict("after") || pending.size() >= kMaxEntries) return false;
      pending.emplace_back(rows.Column(0), std::move(*value));
    }
    if (step != SQLITE_DONE) return false;
    std::ranges::sort(pending, [](const auto& a, const auto& b) {
      const bool deleted_a = a.second.FindDict("after")->FindInt("kind") == 0;
      const bool deleted_b = b.second.FindDict("after")->FindInt("kind") == 0;
      if (deleted_a != deleted_b) return deleted_a;
      return deleted_a ? a.first > b.first : a.first < b.first;
    });
    bool rescan = false;
    for (const auto& [path, value] : pending) {
      const auto* operation = value.FindString("operation");
      const auto* before = value.FindDict("before"); const auto* after = value.FindDict("after");
      Entry old_entry, new_entry;
      if (!operation || !IsSyncId(*operation) || !before || !after || !Decode(*before, &old_entry) ||
          !Decode(*after, &new_entry)) return false;
      SyncContent current;
      if (!files.Read(path, &current)) return false;
      if (current != static_cast<const SyncContent&>(old_entry) &&
          current != static_cast<const SyncContent&>(new_entry) && files.ArchiveAbsent(*operation)) {
        if (new_entry.kind == 1) {
          // No original has been moved yet. Keep the newly edited original and
          // install the already verified incoming content as a conflict copy.
          if (!files.Apply(ConflictName(path, new_entry), *operation, {}, new_entry)) return false;
        } else if (new_entry.kind != 0) return false;
        Statement discard(db, "DELETE FROM pending WHERE id=? AND path=?");
        if (!discard.Text(1, r.view.id) || !discard.Text(2, path) || discard.Step() != SQLITE_DONE) return false;
        r.dirty->value = true; rescan = true; continue;
      }
      if (!files.Apply(path, *operation, old_entry, new_entry)) {
        // A child may have been created since the deletion plan was prepared.
        // Keep an untouched directory and rescan instead of retrying its deletion
        // forever or treating that child as permission to recursively remove it.
        SyncContent now;
        if (new_entry.kind != 0 || !files.ArchiveAbsent(*operation) ||
            !files.Read(path, &now) || now.kind != 2) return false;
        Statement discard(db, "DELETE FROM pending WHERE id=? AND path=?");
        if (!discard.Text(1, r.view.id) || !discard.Text(2, path) || discard.Step() != SQLITE_DONE) return false;
        r.dirty->value = true; rescan = true; continue;
      }
      if (!Finish(r, path, new_entry)) return false;
    }
    return !rescan;
  }
  bool Finish(Relationship& r, const std::string& path, const Entry& entry) {
    if (!Exec("BEGIN IMMEDIATE")) return false;
    Statement remove(db, "DELETE FROM pending WHERE id=? AND path=?");
    r.counter = std::max(r.counter, entry.version[r.role]);
    if (!Save(r) || !SaveEntry(r, path, entry) || !remove.Text(1, r.view.id) || !remove.Text(2, path) ||
        remove.Step() != SQLITE_DONE || !Exec("COMMIT")) { Exec("ROLLBACK"); return false; }
    r.index[path] = entry; return true;
  }
  bool Scan(Relationship& r, SyncFiles& files) {
    if (!(r.role == 0 ? r.view.write : r.view.read)) {
      Statement pending(db, "SELECT 1 FROM pending WHERE id=? LIMIT 1");
      if (!pending.Text(1, r.view.id) || pending.Step() != SQLITE_DONE) {
        // Do not replay a receive journal after this direction is disabled.
        r.view.status = IDS_CRFILES_SYNC_RECOVERY_REQUIRED; return false;
      }
    }
    if (!Recover(r, files)) { r.view.status = IDS_CRFILES_SYNC_RECOVERY_REQUIRED; return false; }
    // Events only invalidate a snapshot. Periodic full hashing also catches
    // missed/overflowed notifications and edits made while the app was stopped.
    if (!r.dirty->value.exchange(false) && base::TimeTicks::Now() - r.scanned < base::Seconds(10)) return true;
    std::map<std::string, SyncContent> observed;
    if (!files.Scan(r.single, &observed)) {
      LOG(ERROR) << "[crFiles sync] scan_failed id=" << r.view.id << " root=" << r.view.root << " single=" << r.single;
      r.dirty->value = true; r.view.status = IDS_CRFILES_SYNC_SOURCE_ERROR; return false;
    }
    if (r.receive_pending) {
      bool unexpected = std::ranges::any_of(observed, [&](const auto& item) {
        return item.second != static_cast<const SyncContent&>(At(r.index, item.first));
      });
      unexpected |= std::ranges::any_of(r.index, [&](const auto& item) {
        return item.second.kind && !observed.contains(item.first);
      });
      if (unexpected) {
        LOG(ERROR) << "[crFiles sync] receive_destination_failed id=" << r.view.id << " stage=initial_contents_changed";
        r.dirty->value = true; r.view.status = IDS_CRFILES_TRANSFER_EXISTS_ERROR; return false;
      }
    }
    Index next = r.index;
    uint64_t next_counter = r.counter;
    const auto invalid_scan = [&] {
      r.dirty->value = true; r.view.status = IDS_CRFILES_SYNC_SOURCE_ERROR; return false;
    };
    bool changed = false;
    for (const auto& [path, value] : observed) {
      auto& e = next[path];
      if (static_cast<const SyncContent&>(e) != value) {
        if (++next_counter >= kMaxCounter) return invalid_scan();
        static_cast<SyncContent&>(e) = value; e.version[r.role] = next_counter;
        changed = true;
      }
    }
    for (auto& [path, e] : next) if (e.kind && !observed.contains(path)) {
      if (++next_counter >= kMaxCounter) return invalid_scan();
      static_cast<SyncContent&>(e) = {}; e.version[r.role] = next_counter;
      changed = true;
    }
    size_t path_bytes = 0;
    for (const auto& [path, entry] : next) path_bytes += path.size();
    if (next.size() > kMaxEntries || path_bytes > 32 * 1024 * 1024) return invalid_scan();
    if (changed) {
      r.counter = next_counter;
      r.index = std::move(next);
      if (!SaveIndex(r)) { loaded = false; r.dirty->value = true; r.view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; return false; }
    }
    r.scanned = base::TimeTicks::Now(); return true;
  }
  bool ScanLocal(Relationship& r) {
    if (r.dirty->value || base::TimeTicks::Now() - r.scanned >= base::Seconds(10)) {
      std::string bookmark;
      if (!base::Base64Decode(r.bookmark, &bookmark)) {
        LOG(ERROR) << "[crFiles sync] local_check_failed id=" << r.view.id << " stage=bookmark_decode";
        r.view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; return false;
      }
      auto access = ResolveFileAccessBookmark(std::vector<uint8_t>(bookmark.begin(), bookmark.end()));
      SyncFiles files(r.view.root, r.view.id, [&] { return HasPendingCommand(r.view.id); });
      if (!files.Open()) { r.view.status = IDS_CRFILES_SYNC_PREPARE_ERROR; return false; }
      if (files.Identity() != r.identity) {
        LOG(ERROR) << "[crFiles sync] local_check_failed id=" << r.view.id << " stage=root_identity_changed";
        r.view.status = IDS_CRFILES_SYNC_SOURCE_ERROR; return false;
      }
      if (!Scan(r, files)) {
        if (!IsSyncErrorStatus(r.view.status)) r.view.status = IDS_CRFILES_SYNC_SOURCE_ERROR;
        return false;
      }
    }
    return true;
  }
  bool Due(Relationship& r, base::TimeTicks now) {
    const auto changed = base::TimeTicks::FromInternalValue(r.dirty->changed.load());
    if (changed != r.observed_change) {
      r.observed_change = changed;
      if (r.burst_started.is_null()) r.burst_started = changed;
      r.change_due = std::min(changed + base::Seconds(2), r.burst_started + base::Seconds(10));
    }
    if (now < r.retry_not_before) return false;
    if (r.role == 0 && (!r.confirmed || r.connection_pending || r.remote_wake)) return true;
    return now >= (r.change_due.is_null() ? r.next : r.change_due);
  }
  bool NeedsConnection(Relationship& r) {
    const bool periodic = r.next <= base::TimeTicks::Now();
    if (periodic) r.next = base::TimeTicks::Now() + base::Seconds(10) + base::Milliseconds(base::RandIntInclusive(0, 1500));
    r.burst_started = {}; r.change_due = {};
    if (r.role == 0 && (!r.confirmed || r.connection_pending || r.remote_wake)) return true;
    const bool sends = r.role == 0 ? r.view.read : r.view.write;
    if (sends && !ScanLocal(r)) {
      // Failed local reads must not spin at watcher speed or publish a wake
      // from an uncommitted index. A later successful scan retries delivery.
      r.wake_pending = false;
      r.retry_not_before = base::TimeTicks::Now() + base::Seconds(10);
      return false;
    }
    const bool pending = sends && r.counter > r.acknowledged_counter;
    if (r.role == 1) {
      r.wake_pending = pending;
      if (sends) r.view.status = pending ? IDS_CRFILES_SYNC_WAITING : IDS_CRFILES_SYNC_CURRENT;
      return false;
    }
    const bool connect = pending || (r.view.write && periodic);
    r.view.status = connect ? IDS_CRFILES_SYNC_WAITING :
        (r.view.read || r.view.write ? IDS_CRFILES_SYNC_CURRENT : IDS_CRFILES_SYNC_NO_PERMISSION);
    return connect;
  }
  bool SendIndex(SyncWire& wire, const Index& index, const std::set<std::string>* applied = nullptr) {
    auto it = index.begin();
    do {
      auto page = Message("sync-index"); base::ListValue entries;
      for (size_t n = 0; n < 32 && it != index.end(); ++n, ++it) {
        auto entry = Encode(it->second); entry.Set("path", it->first);
        if (applied) entry.Set("applied", applied->contains(it->first));
        entries.Append(std::move(entry));
      }
      page.Set("entries", std::move(entries)); page.Set("last", it == index.end());
      if (!wire.Write(std::move(page))) return false;
    } while (it != index.end());
    return true;
  }
  bool InScope(const Relationship& r, const std::string& path) {
    return r.single.empty() || path == r.single ||
        (path.find('/') == std::string::npos && path.starts_with("sync-conflict-" + SyncHash(r.single).substr(0, 16) + "-"));
  }
  bool ReadIndex(SyncWire& wire, const Relationship& r, Index* index, std::set<std::string>* applied = nullptr) {
    size_t path_bytes = 0;
    for (size_t pages = 0; pages <= kMaxEntries / 32; ++pages) {
      const auto page = wire.Read();
      const auto* entries = page ? page->FindList("entries") : nullptr;
      const auto last = page ? page->FindBool("last") : std::nullopt;
      if (!Type(page, "sync-index") || !entries || !last || entries->size() > 32 ||
          (entries->empty() && !*last)) return false;
      for (const auto& value : *entries) {
        const auto* item = value.GetIfDict(); const auto* path = item ? item->FindString("path") : nullptr;
        Entry entry;
        if (path) path_bytes += path->size();
        if (path_bytes > 32 * 1024 * 1024 || !path || !SafeSyncPath(*path) || !InScope(r, *path) || !Decode(*item, &entry) || (!r.single.empty() && entry.kind == 2) ||
            index->size() >= kMaxEntries || !index->emplace(*path, entry).second) return false;
        if (applied) {
          const auto received = item->FindBool("applied");
          if (!received) return false;
          if (*received) applied->insert(*path);
        }
      }
      if (*last) return true;
    }
    return false;
  }
  bool Transfer(SyncWire& wire, Relationship& r, SyncFiles& files, const Index& remote, Plan& plan) {
    // Stage every needed payload before applying any path. A source can also be
    // the loser of a conflict and must remain available throughout this phase.
    for (auto& [path, item] : plan) {
      if (Interrupted(r.view.id, wire)) return false;
      if (item.result.kind != 1) continue;
      const Entry local = At(r.index, path), other = At(remote, path);
      const bool local_need = !Same(local, item.result), remote_need = !Same(other, item.result);
      if (!local_need && !remote_need) continue;
      item.operation = RandomId();
      if (item.donor == r.role) {
        if (local_need || remote_need) {
          if (!files.CopyStage(item.source, item.operation, item.result)) return false;
        }
        auto header = Message("sync-content"); header.Set("path", path);
        header.Set("hash", item.result.hash); header.Set("sending", remote_need);
        if (!wire.Write(std::move(header))) return false;
        if (remote_need) {
          auto input = files.OpenStage(item.operation);
          if (!input.IsValid()) return false;
          std::vector<uint8_t> buffer(256 * 1024);
          uint64_t remaining = item.result.size;
          while (remaining) {
            auto bytes = base::span(buffer).first(static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size())));
            if (Interrupted(r.view.id, wire) || !input.ReadAtCurrentPosAndCheck(bytes) || !wire.Bytes(bytes, true)) return false;
            remaining -= bytes.size(); wire.Progress(path, bytes.size());
          }
        }
        if (!Type(wire.Read(), "sync-staged")) return false;
      } else {
        const auto header = wire.Read();
        const auto* name = header ? header->FindString("path") : nullptr;
        const auto* hash = header ? header->FindString("hash") : nullptr;
        if (!Type(header, "sync-content") || !name || *name != path || !hash || *hash != item.result.hash ||
            header->FindBool("sending") != local_need) return false;
        if (local_need) {
          auto output = files.CreateStage(item.operation);
          if (!output.IsValid()) return false;
          std::vector<uint8_t> buffer(256 * 1024);
          uint64_t remaining = item.result.size;
          while (remaining) {
            auto bytes = base::span(buffer).first(static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size())));
            if (Interrupted(r.view.id, wire) || !wire.Bytes(bytes, false) || !output.WriteAtCurrentPosAndCheck(bytes)) return false;
            remaining -= bytes.size(); wire.Progress(path, bytes.size());
          }
          if (!output.Flush()) return false;
          output.Close();
          if (!files.VerifyStage(item.operation, item.result)) return false;
        }
        if (!wire.Write(Message("sync-staged"))) return false;
      }
    }
    return true;
  }
  bool Apply(SyncWire& wire, Relationship& r, SyncFiles& files, Plan& plan) {
    // First remove obsolete descendants, deepest first; then create directories
    // and install files, parents first. Directories are archived only when empty.
    std::vector<std::string> order;
    for (auto it = plan.rbegin(); it != plan.rend(); ++it)
      if (!it->second.result.kind) order.push_back(it->first);
    for (const auto& [path, item] : plan) if (item.result.kind) order.push_back(path);
    bool changed = false;
    if (!Exec("BEGIN IMMEDIATE")) return false;
    base::ScopedClosureRunner rollback(base::BindOnce([](Impl* self) { self->Exec("ROLLBACK"); }, base::Unretained(this)));
    for (const auto& path : order) {
      auto& item = plan.at(path);
      if (Interrupted(r.view.id, wire)) return false;
      const Entry before = At(r.index, path);
      if (Same(before, item.result) && before.version == item.result.version) continue;
      changed = true;
      bool below_replaced_file = false;
      if (!before.kind) for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent)) {
        if (At(r.index, parent).kind == 1 && plan.contains(parent) && plan.at(parent).result.kind == 2)
          below_replaced_file = true;
      }
      SyncContent actual;
      if (!below_replaced_file && (!files.Read(path, &actual) || actual != static_cast<const SyncContent&>(before))) {
        r.dirty->value = true; return false;
      }
      if (item.operation.empty()) item.operation = RandomId();
      auto record = Message("sync-recovery");
      record.Set("operation", item.operation); record.Set("path", path);
      record.Set("before", Encode(before)); record.Set("after", Encode(item.result));
      const auto json = Json(record);
      Statement pending(db, "INSERT OR REPLACE INTO pending VALUES(?,?,?)");
      if (!files.Record(item.operation, json) || !pending.Text(1, r.view.id) ||
          !pending.Text(2, path) || !pending.Text(3, json) || pending.Step() != SQLITE_DONE) return false;
    }
    if (!Exec("COMMIT")) return false;
    static_cast<void>(rollback.Release());
    if (!Recover(r, files)) { r.view.status = IDS_CRFILES_SYNC_RECOVERY_REQUIRED; return false; }
    if (changed) r.dirty->value = true;
    return true;
  }
  bool Exchange(SyncWire& wire, Relationship& r, SyncFiles& files) {
    if (!r.view.permissions_known) return false;
    const bool send = r.role == 0 ? r.view.read : r.view.write;
    const bool receive = r.role == 0 ? r.view.write : r.view.read;
    // No file/index access when neither direction is permitted.
    if (!send && !receive) {
      r.view.status = IDS_CRFILES_SYNC_NO_PERMISSION;
      return wire.Write(Message("sync-idle")) && Type(wire.Read(), "sync-idle");
    }
    if (files.Identity() != r.identity || !Scan(r, files)) return false;
    Index remote;
    std::set<std::string> applied;
    if (send && receive) {
      if (r.role == 0) {
        if (!SendIndex(wire, r.index) || !ReadIndex(wire, r, &remote)) return false;
      } else if (!ReadIndex(wire, r, &remote) || !SendIndex(wire, r.index)) return false;
    } else if (send) {
      if (!SendIndex(wire, r.index) || !ReadIndex(wire, r, &remote, &applied)) return false;
      // Receipts may only echo the advertised source entry or an empty entry.
      // They never introduce receiver content, paths or versions to the source.
      if (remote.size() != r.index.size()) return false;
      for (const auto& [path, receipt] : remote) {
        const auto source = r.index.find(path);
        if (source == r.index.end() ||
            (!(Same(receipt, source->second) && receipt.version == source->second.version) &&
             !(Same(receipt, Entry()) && receipt.version == Entry().version))) return false;
      }
    } else {
      if (!ReadIndex(wire, r, &remote)) return false;
      Index receipts;
      // Local edits/deletions retain the source's version component in the
      // durable index. Do not reapply a source revision already received, even
      // when the receiver's current content differs. A newer source revision
      // can replace it through the usual journal and recovery archive.
      for (const auto& [path, entry] : remote) {
        const auto local = r.index.find(path);
        if (local != r.index.end() && local->second.version[1 - r.role] >= entry.version[1 - r.role])
          applied.insert(path);
        receipts.emplace(path, Same(At(r.index, path), entry) ? entry : Entry());
      }
      if (!SendIndex(wire, receipts, &applied)) return false;
    }
    Plan plan;
    base::ScopedClosureRunner cleanup(base::BindOnce([](Impl* self, Relationship* relation, SyncFiles* files, Plan* plan) {
      std::set<std::string> retained;
      Statement rows(self->db, "SELECT value FROM pending WHERE id=?");
      if (!rows.Text(1, relation->view.id)) return;
      int step;
      while ((step = rows.Step()) == SQLITE_ROW) {
        auto value = base::JSONReader::ReadDict(rows.Column(0), base::JSON_PARSE_RFC);
        const auto* id = value ? value->FindString("operation") : nullptr;
        if (!id) return;
        retained.insert(*id);
      }
      if (step != SQLITE_DONE) return;
      for (const auto& [path, item] : *plan)
        if (!item.operation.empty() && !retained.contains(item.operation)) files->DiscardStage(item.operation);
    }, base::Unretained(this), base::Unretained(&r), base::Unretained(&files), base::Unretained(&plan)));
    if (send && receive) {
      if (!MakePlan(r.role == 0 ? r.index : remote, r.role == 0 ? remote : r.index, &plan)) {
        r.view.status = IDS_CRFILES_SYNC_SOURCE_ERROR; return false;
      }
    } else {
      // A read-only peer never donates bytes or versions back to the source.
      // Apply only source revisions not previously received. Receiver edits and
      // receiver-only paths remain untouched until the source changes that path.
      // Changed source revisions still archive replaced copies. Ancestor
      // conflicts fail safely instead of rewriting an unchanged parent.
      const auto& source = send ? r.index : remote;
      std::set<std::u16string> folded;
      for (const auto& [path, entry] : source) {
        if (!SafeSyncPath(path) || !folded.insert(base::i18n::FoldCase(base::UTF8ToUTF16(path))).second) return false;
        if (entry.kind) for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent))
          if (At(source, parent).kind != 2) return false;
        if (applied.contains(path)) continue;
        if (receive && entry.kind) for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent)) {
          if (applied.contains(parent) && At(r.index, parent).kind != 2) {
            // An unchanged source parent must not undo a receiver-side deletion
            // or type change just to install a new child. Stop before journaling.
            LOG(ERROR) << "[crFiles sync] source_update_blocked id=" << r.view.id << " path=" << path
                       << " stage=receiver_parent_changed parent=" << parent;
            r.view.status = IDS_CRFILES_SYNC_SOURCE_ERROR; return false;
          }
        }
        plan.emplace(path, Planned{entry, send ? r.role : 1 - r.role, path, {}});
      }
    }
    uint64_t total = 0;
    for (const auto& [path, item] : plan) if (item.result.kind == 1 &&
        !Same(At(item.donor == r.role ? remote : r.index, path), item.result)) total += item.result.size;
    wire.Begin(total);
    crypto::hash::Hasher hasher(crypto::hash::kSha256);
    for (const auto& [path, item] : plan) {
      auto entry = Encode(item.result); entry.Set("path", path); entry.Set("donor", item.donor); entry.Set("source", item.source);
      const std::string serialized = Json(entry) + "\n";
      hasher.Update(base::as_byte_span(serialized));
    }
    std::array<uint8_t, 32> hash; hasher.Finish(hash);
    auto ready = Message("sync-plan"); ready.Set("hash", base::HexEncode(hash));
    if (!wire.Write(std::move(ready))) return false;
    const auto other = wire.Read();
    const auto* digest = other ? other->FindString("hash") : nullptr;
    if (!Type(other, "sync-plan") || !digest || *digest != base::HexEncode(hash) ||
        !Transfer(wire, r, files, remote, plan) || (receive && !Apply(wire, r, files, plan))) return false;
    if (!wire.Write(Message("sync-committed")) || !Type(wire.Read(), "sync-committed")) return false;
    return true;
  }
  bool Run(SyncWire& wire, bool outgoing, const std::string& id,
           const std::string& fingerprint, const std::string& peer, int avatar,
           const base::FilePath& receive_directory) {
    if (!Open() || !IsSyncId(id)) return false;
    Relationship* relation = nullptr;
    bool peer_read = false, peer_write = false;
    auto it = jobs.find(id);
    if (outgoing) {
      if (it == jobs.end() || it->second->role != 0 || it->second->view.paused || it->second->view.removed ||
          it->second->view.fingerprint != fingerprint) return false;
      relation = it->second.get();
      auto open = Message("sync-open"); open.Set("id", id); open.Set("create", !relation->confirmed);
      open.Set("name", relation->single.empty() ? CanonicalSyncPath(relation->view.root.BaseName().AsUTF8Unsafe()) : relation->single);
      open.Set("file", !relation->single.empty());
      open.Set("permissions", 2); open.Set("read", relation->view.read); open.Set("write", relation->view.write);
      open.Set("source_updates", 1);
      if (!wire.Write(std::move(open))) return false;
      const auto accepted = wire.Read();
      if (Type(accepted, "sync-declined")) {
        relation->view.paused = true;
        if (!Save(*relation)) {
          relation->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR;
          base::AutoLock lock(ui_lock); commands.push_back({1, id, {}, {}, {}, true});
        }
        Publish(); return false;
      }
      if (!Type(accepted, "sync-accepted") || accepted->FindInt("permissions") != 2 || accepted->FindInt("source_updates") != 1 ||
          accepted->FindBool("read") != relation->view.read ||
          accepted->FindBool("write") != relation->view.write) return false;
      relation->confirmed = true;
      if (!Save(*relation)) { relation->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; Publish(); return false; }
    } else {
      const auto open = wire.Read();
      const auto* remote_id = open ? open->FindString("id") : nullptr;
      if (!Type(open, "sync-open") || !remote_id || *remote_id != id || open->FindInt("permissions") != 2 || open->FindInt("source_updates") != 1 ||
          !open->FindBool("read") || !open->FindBool("write")) return false;
      peer_read = *open->FindBool("read"); peer_write = *open->FindBool("write");
      if (it == jobs.end()) {
        const auto* name = open->FindString("name"); const auto file = open->FindBool("file");
        if (!open->FindBool("create").value_or(false) || !name || !SafeSyncPath(*name) ||
            name->find('/') != std::string::npos || !file || Full()) return false;
        auto r = std::make_unique<Relationship>();
        r->view.id = id; r->view.fingerprint = fingerprint; r->view.peer = peer;
        r->single = *file ? *name : ""; r->role = 1; r->confirmed = true;
        r->view.name = *name; r->view.started = base::Time::Now();
        r->view.avatar = std::clamp(avatar, 0, 8);
        r->view.read = peer_read; r->view.write = peer_write; r->view.permissions_known = true;
        auto destination = receive_directory.Append(base::FilePath::FromUTF8Unsafe(*name));
        int destination_error = 0;
        for (;;) {
          const auto approved = wire.Consent(destination, *name, *file, destination_error);
          if (!approved) { wire.Write(Message("sync-declined")); return false; }
          if (wire.Canceled()) return false;
          if (!approved->IsAbsolute() || approved->ReferencesParent() ||
              CanonicalSyncPath(approved->BaseName().AsUTF8Unsafe()) != *name) return false;
          destination = *approved;
          const auto fail_destination = [&](const char* stage, int error) {
            LOG(ERROR) << "[crFiles sync] receive_destination_failed id=" << id
                       << " stage=" << stage << " path=" << destination;
            destination_error = error;
          };
          const auto bookmark = CreateFileAccessBookmark(approved->DirName());
          auto access = ResolveFileAccessBookmark(bookmark);
          const auto parent = base::MakeAbsoluteFilePath(approved->DirName());
          if (parent.empty() || !base::DirectoryExists(parent)) {
            fail_destination("parent_access", IDS_CRFILES_SYNC_PREPARE_ERROR); continue;
          }
          r->view.root = *file ? parent : parent.Append(base::FilePath::FromUTF8Unsafe(*name));
          if (Overlaps(r->view.root, {}, r->single)) {
            fail_destination("overlap", IDS_CRFILES_SYNC_OVERLAP_ERROR); continue;
          }
          SyncFiles parent_files(parent, id, [&] { return wire.Canceled() || !wire.KeepAlive(); });
          if (!parent_files.Open()) {
            fail_destination("parent_prepare", IDS_CRFILES_SYNC_PREPARE_ERROR); continue;
          }
          const auto available = parent_files.ReceiveNameAvailable(*name, *file);
          if (!available || !*available) {
            fail_destination(available ? "name_exists" : "parent_read",
                available ? IDS_CRFILES_TRANSFER_EXISTS_ERROR : IDS_CRFILES_SYNC_PREPARE_ERROR); continue;
          }
          if (*file) {
            const auto identity = parent_files.Identity();
            const bool overlap = std::ranges::any_of(jobs, [&](const auto& pair) {
              const auto& existing = *pair.second;
              return !existing.view.removed && existing.identity == identity &&
                  !DisjointSingleFiles(r->single, existing.single);
            });
            if (overlap) { fail_destination("identity_overlap", IDS_CRFILES_SYNC_OVERLAP_ERROR); continue; }
            r->identity = identity; r->view.recovery = parent_files.Recovery();
          } else {
            // Exclusive creation also rejects an item appearing after the check.
            if (!parent_files.CreateReceiveDirectory(*name)) {
              fail_destination("directory_create", IDS_CRFILES_SYNC_PREPARE_ERROR); continue;
            }
            SyncFiles initial(r->view.root, id, [&] { return wire.Canceled() || !wire.KeepAlive(); });
            if (!initial.Open()) {
              fail_destination("directory_prepare", IDS_CRFILES_SYNC_PREPARE_ERROR); continue;
            }
            r->identity = initial.Identity(); r->view.recovery = initial.Recovery();
          }
          // Persist the approved scope before transferring. Until the initial
          // exchange commits, unexpected local contents must never be adopted.
          r->receive_pending = true;
          r->bookmark = base::Base64Encode(bookmark);
          if (!Save(*r)) return false;
          LOG(INFO) << "[crFiles sync] receive_destination_approved id=" << id
                    << " root=" << r->view.root << " single=" << r->single;
          break;
        }
        relation = r.get(); jobs.emplace(id, std::move(r));
      } else {
        relation = it->second.get();
        if (relation->role != 1 || relation->view.fingerprint != fingerprint ||
            relation->view.paused || relation->view.removed) return false;
      }
      // These are the authenticated creator's grants to this receiver, not
      // local grants back to the creator. Persist and display the exact policy.
      relation->view.read = peer_read; relation->view.write = peer_write;
      relation->view.permissions_known = true;
      if (relation->identity.empty()) {
        std::string bookmark;
        if (!base::Base64Decode(relation->bookmark, &bookmark)) return false;
        auto access = ResolveFileAccessBookmark(std::vector<uint8_t>(bookmark.begin(), bookmark.end()));
        if (!base::CreateDirectory(relation->view.root)) return false;
        const auto canonical_root = base::MakeAbsoluteFilePath(relation->view.root);
        if (canonical_root.empty() || Overlaps(canonical_root, id, relation->single)) return false;
        relation->view.root = canonical_root;
        SyncFiles initial(relation->view.root, id, [&] { return Interrupted(id, wire); });
        if (!initial.Open()) { relation->view.status = IDS_CRFILES_SYNC_PREPARE_ERROR; Publish(); return false; }
        relation->identity = initial.Identity(); relation->view.recovery = initial.Recovery();
        if (!Save(*relation)) { relation->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; Publish(); return false; }
      }
      if (!Save(*relation)) { relation->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; Publish(); return false; }
      Publish();
      auto accepted = Message("sync-accepted"); accepted.Set("permissions", 2);
      accepted.Set("source_updates", 1);
      accepted.Set("read", relation->view.read); accepted.Set("write", relation->view.write);
      if (!wire.Write(std::move(accepted))) return false;
    }
    relation->view.peer = peer; relation->view.avatar = std::clamp(avatar, 0, 8);
    if (!Save(*relation)) { relation->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; Publish(); return false; }
    std::string bookmark;
    if (!base::Base64Decode(relation->bookmark, &bookmark)) {
      LOG(ERROR) << "[crFiles sync] access_failed id=" << id << " stage=bookmark_decode";
      relation->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; Publish(); return false;
    }
    auto access = ResolveFileAccessBookmark(std::vector<uint8_t>(bookmark.begin(), bookmark.end()));
    SyncFiles files(relation->view.root, id, [&] { return Interrupted(id, wire) || !wire.KeepAlive(); });
    relation->view.status = IDS_CRFILES_SYNC_ACTIVE; Publish();
    const bool opened = files.Open();
    if (!opened) relation->view.status = IDS_CRFILES_SYNC_PREPARE_ERROR;
    else if (files.Identity() != relation->identity) {
      LOG(ERROR) << "[crFiles sync] source_identity_changed id=" << id << " root=" << relation->view.root
                 << " expected=" << relation->identity << " actual=" << files.Identity();
      relation->view.status = IDS_CRFILES_SYNC_SOURCE_ERROR;
    }
    bool ok = opened && Exchange(wire, *relation, files);
    const bool idle = relation->view.status == IDS_CRFILES_SYNC_NO_PERMISSION;
    if (ok) {
      const auto previous = relation->view.last_synced;
      const auto previous_acknowledged = relation->acknowledged_counter;
      const bool previous_pending = relation->connection_pending;
      const bool previous_receive_pending = relation->receive_pending;
      if (!idle) relation->view.last_synced = base::Time::Now();
      // Exchange returns only after the peer's commit (or idle-policy reply).
      // Local scans alone must never mark a pending update as delivered.
      relation->acknowledged_counter = relation->counter;
      relation->connection_pending = false;
      if (!idle) relation->receive_pending = false;
      if (!Save(*relation)) {
        relation->view.last_synced = previous;
        relation->acknowledged_counter = previous_acknowledged;
        relation->connection_pending = previous_pending;
        relation->receive_pending = previous_receive_pending;
        relation->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; ok = false;
      }
    }
    if (!idle && !IsSyncErrorStatus(relation->view.status))
      relation->view.status = ok ? IDS_CRFILES_SYNC_CURRENT : IDS_CRFILES_SYNC_RETRY;
    relation->failures = ok ? 0 : std::min(relation->failures + 1, 5);
    relation->next = base::TimeTicks::Now() + base::Seconds(ok ? 10 : 10 * (1 << relation->failures)) +
        base::Milliseconds(base::RandIntInclusive(0, 1500));
    relation->retry_not_before = ok ? base::TimeTicks() : relation->next;
    if (!ok) relation->dirty->value = true;
    Publish(); return ok;
  }
};

SyncService::SyncService(base::FilePath directory) : impl_(std::make_unique<Impl>(std::move(directory))) {}
SyncService::~SyncService() = default;
SyncStartResult SyncService::Queue(const base::FilePath& source, std::string fingerprint, std::string peer, int avatar, bool write) {
  if (!source.IsAbsolute() || source.ReferencesParent() || fingerprint.size() != 64) {
    LOG(ERROR) << "[crFiles sync] queue_failed stage=invalid_source_or_peer source=" << source;
    return {{}, IDS_CRFILES_SYNC_SOURCE_ERROR};
  }
  base::AutoLock lock(impl_->ui_lock);
  if (impl_->commands.size() >= kMaxJobs) {
    LOG(ERROR) << "[crFiles sync] queue_failed stage=command_limit";
    return {{}, IDS_CRFILES_SYNC_LIMIT_ERROR};
  }
  const auto id = RandomId();
  impl_->commands.push_back({0, id, source, std::move(fingerprint), std::move(peer), false, avatar, true, write});
  LOG(INFO) << "[crFiles sync] queued id=" << id;
  return {id, 0};
}
SyncStartResult SyncService::Retry(const std::string& id, const std::string& fingerprint, bool write) {
  base::AutoLock lock(impl_->ui_lock);
  if (impl_->commands.size() >= kMaxJobs) {
    LOG(ERROR) << "[crFiles sync] retry_failed id=" << id << " stage=command_limit";
    return {{}, IDS_CRFILES_SYNC_LIMIT_ERROR};
  }
  const auto job = std::ranges::find_if(impl_->snapshot, [&](const auto& value) {
    return value.id == id && value.creator && !value.removed && fingerprint.size() == 64 && value.fingerprint == fingerprint;
  });
  if (job == impl_->snapshot.end()) {
    LOG(ERROR) << "[crFiles sync] retry_failed id=" << id << " stage=relationship_not_eligible";
    return {{}, IDS_CRFILES_SYNC_REQUEST_ERROR};
  }
  impl_->commands.push_back({6, id, {}, fingerprint, {}, false, 0, true, write});
  return {id, 0};
}
void SyncService::Pause(std::string id, bool paused) {
  base::AutoLock lock(impl_->ui_lock); impl_->commands.push_back({1, std::move(id), {}, {}, {}, paused});
}
void SyncService::Permissions(std::string id, bool read, bool write) {
  base::AutoLock lock(impl_->ui_lock);
  impl_->commands.push_back({4, std::move(id), {}, {}, {}, false, 0, read, write});
}
void SyncService::Remove(std::string id) {
  base::AutoLock lock(impl_->ui_lock); impl_->commands.push_back({2, std::move(id), {}, {}, {}});
}
void SyncService::Revoke(std::string fingerprint) {
  base::AutoLock lock(impl_->ui_lock); impl_->commands.push_back({3, {}, {}, std::move(fingerprint), {}});
}
std::vector<SyncJob> SyncService::Snapshot() const {
  base::AutoLock lock(impl_->ui_lock); return impl_->snapshot;
}
std::vector<SyncWakeup> SyncService::PendingWakeups() const {
  base::AutoLock lock(impl_->ui_lock);
  std::vector<SyncWakeup> due;
  for (const auto& wake : impl_->wakeups) {
    const auto attempt = impl_->wake_attempts.find(wake.id);
    if (attempt == impl_->wake_attempts.end() || attempt->second <= base::TimeTicks::Now())
      due.push_back(wake);
  }
  return due;
}
void SyncService::WakeAttempt(const SyncWakeup& wake) {
  base::AutoLock lock(impl_->ui_lock);
  // Delivery is acknowledged only by the actual file exchange. A lost wake or
  // acknowledgement is harmless: retry at most once per ten seconds.
  if (std::ranges::any_of(impl_->wakeups, [&](const auto& current) {
        return current.id == wake.id && current.fingerprint == wake.fingerprint && current.counter == wake.counter;
      })) impl_->wake_attempts[wake.id] = base::TimeTicks::Now() + base::Seconds(10);
}
bool SyncService::RequestWake(const std::string& id, const std::string& fingerprint) {
  base::AutoLock lock(impl_->ui_lock);
  const auto allowed = impl_->wake_authorized.find(id);
  if (!IsSyncId(id) || fingerprint.empty() || allowed == impl_->wake_authorized.end() ||
      allowed->second != fingerprint) return false;
  if (impl_->remote_wakes.size() >= kMaxJobs && !impl_->remote_wakes.contains(id)) return false;
  // This bounded, deduplicated queue neither cancels an active exchange nor
  // bypasses the worker's current permission check and failure backoff.
  impl_->remote_wakes[id] = fingerprint;
  return true;
}
void SyncService::Poll() { base::AutoLock lock(impl_->io_lock); impl_->Poll(); }
std::optional<SyncRequest> SyncService::Next(const std::vector<SyncEndpoint>& peers) {
  base::AutoLock lock(impl_->io_lock);
  impl_->Poll();
  if (!impl_->loaded) return std::nullopt;
  const auto now = base::TimeTicks::Now();
  for (auto& [id, r] : impl_->jobs) {
    const bool removal = r->view.removed && r->removal_pending;
    if ((!removal && (r->view.paused || r->view.removed)) || r->view.fingerprint.empty()) continue;
    if (removal ? r->next > now : !impl_->Due(*r, now)) continue;
    if (!removal && !impl_->NeedsConnection(*r)) {
      // A failed index save requires reopening the DB before inspecting any
      // other relationship. No connection is scheduled from an unsaved scan.
      if (!impl_->loaded) break;
      continue;
    }
    const auto peer = std::ranges::find_if(peers, [&](const auto& endpoint) {
      return endpoint.fingerprint == r->view.fingerprint && (!removal || endpoint.removal);
    });
    if (peer == peers.end()) { if (!r->view.removed) r->view.status = IDS_CRFILES_SYNC_WAITING; continue; }
    r->next = now + base::Seconds(30);
    r->remote_wake = false;
    impl_->Publish();
    return SyncRequest{id, peer->address, peer->fingerprint, removal};
  }
  impl_->Publish(); return std::nullopt;
}
bool SyncService::CanConnect(const std::string& id, const std::string& fingerprint) {
  base::AutoLock lock(impl_->io_lock);
  impl_->Poll();
  if (!impl_->loaded) return false;
  auto it = impl_->jobs.find(id);
  return it == impl_->jobs.end() || (!it->second->view.paused && !it->second->view.removed &&
      it->second->view.fingerprint == fingerprint);
}
bool SyncService::Revoked(const std::string& id, const std::string& fingerprint) {
  base::AutoLock lock(impl_->io_lock);
  auto it = impl_->jobs.find(id);
  if (!impl_->loaded || it == impl_->jobs.end() || !it->second->view.removed ||
      it->second->view.fingerprint != fingerprint) return false;
  // A sync-revoked reply also tells the peer to remove its entry. Never send
  // that reply for an in-memory tombstone whose durable save has failed.
  if (!impl_->Save(*it->second)) {
    it->second->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR; impl_->Publish(); return false;
  }
  return true;
}
bool SyncService::CanRemove(const std::string& id, const std::string& fingerprint, bool outgoing) {
  base::AutoLock lock(impl_->io_lock);
  impl_->Poll();
  return impl_->RemovalAllowed(id, fingerprint, outgoing);
}
bool SyncService::RemoteRemoved(const std::string& id, const std::string& fingerprint) {
  base::AutoLock lock(impl_->io_lock);
  impl_->Poll();
  return impl_->RemovalAllowed(id, fingerprint, false) && impl_->ReceiveRemoval(id, fingerprint);
}
bool SyncService::RunRemoval(SyncWire& wire, bool outgoing, const std::string& id, const std::string& fingerprint) {
  base::AutoLock lock(impl_->io_lock);
  return impl_->RemoveOverWire(wire, outgoing, id, fingerprint);
}
bool SyncService::Authorized(const std::string& id, const std::string& fingerprint) {
  base::AutoLock lock(impl_->io_lock);
  impl_->Poll();
  auto it = impl_->jobs.find(id);
  return impl_->loaded && it != impl_->jobs.end() && it->second->confirmed && !it->second->view.paused &&
      !it->second->view.removed && it->second->view.fingerprint == fingerprint;
}
bool SyncService::Run(SyncWire& wire, bool outgoing, const std::string& id,
                      const std::string& fingerprint, const std::string& peer, int avatar,
                      const base::FilePath& receive_directory) {
  base::AutoLock lock(impl_->io_lock);
  return impl_->Run(wire, outgoing, id, fingerprint, peer, avatar, receive_directory);
}
void SyncService::Failed(const std::string& id) {
  base::AutoLock lock(impl_->io_lock);
  auto it = impl_->jobs.find(id);
  if (it == impl_->jobs.end()) return;
  if (!IsSyncErrorStatus(it->second->view.status)) it->second->view.status = IDS_CRFILES_SYNC_RETRY;
  int delay = 30;
  if (it->second->view.removed && it->second->removal_pending) {
    it->second->failures = std::min(it->second->failures + 1, 5);
    delay = 10 * (1 << it->second->failures);
  }
  it->second->next = std::max(it->second->next, base::TimeTicks::Now() + base::Seconds(delay) +
      base::Milliseconds(base::RandIntInclusive(0, 1500)));
  it->second->retry_not_before = it->second->next;
  impl_->Publish();
}
}  // namespace crfiles
