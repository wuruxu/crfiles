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
struct Dirty { std::atomic<bool> value{true}; };
class Watch {
 public:
  Watch(base::FilePath path, std::shared_ptr<Dirty> dirty) : dirty_(std::move(dirty)) {
    static_cast<void>(watcher_.Watch(path, base::FilePathWatcher::Type::kRecursive,
        base::BindRepeating(&Watch::Changed, base::Unretained(this))));
  }
 private:
  void Changed(const base::FilePath&, bool) { dirty_->value = true; }
  std::shared_ptr<Dirty> dirty_;
  base::FilePathWatcher watcher_;
};
// The holder's watcher is constructed and destroyed only on watcher_runner.
// Its shared owner can move between successive desktop transport workers.
struct WatchHolder { std::unique_ptr<Watch> watch; };
std::shared_ptr<WatchHolder> StartWatch(scoped_refptr<base::SequencedTaskRunner> runner,
                                      base::FilePath path, std::shared_ptr<Dirty> dirty) {
  auto holder = std::shared_ptr<WatchHolder>(new WatchHolder, [runner](WatchHolder* value) {
    runner->DeleteSoon(FROM_HERE, value);
  });
  runner->PostTask(FROM_HERE, base::BindOnce([](std::shared_ptr<WatchHolder> holder,
      base::FilePath path, std::shared_ptr<Dirty> dirty) {
    holder->watch = std::make_unique<Watch>(std::move(path), std::move(dirty));
  }, holder, std::move(path), std::move(dirty)));
  return holder;
}
struct Relationship {
  SyncJob view;
  std::string single;
  std::string identity;
  std::string bookmark;
  int role = 0;
  bool confirmed = false;
  uint64_t counter = 0;
  Index index;
  std::shared_ptr<Dirty> dirty = std::make_shared<Dirty>();
  base::TimeTicks scanned;
  base::TimeTicks next;
  int failures = 0;
  std::shared_ptr<WatchHolder> watcher;
};
}  // namespace

bool IsSyncId(const std::string& value) {
  return value.size() == 32 && std::ranges::all_of(value, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
  });
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
  base::FilePath directory;
  sqlite3* db = nullptr;
  bool loaded = false;
  std::unique_ptr<SyncFiles> database_lock;
  std::map<std::string, std::unique_ptr<Relationship>> jobs;
  scoped_refptr<base::SequencedTaskRunner> watcher_runner;
  bool Exec(const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
  bool Open() {
    if (loaded) return true;
    if (db) { sqlite3_close(db); db = nullptr; }
    if (!base::CreateDirectory(directory)) return false;
    const auto canonical_directory = base::MakeAbsoluteFilePath(directory);
    if (canonical_directory.empty()) return false;
    directory = canonical_directory;
    database_lock = std::make_unique<SyncFiles>(directory, "database", [] { return false; });
    if (!database_lock->Open() || sqlite3_initialize() != SQLITE_OK ||
        sqlite3_open(directory.AppendASCII("sync.sqlite3").AsUTF8Unsafe().c_str(), &db) != SQLITE_OK) return false;
    sqlite3_busy_timeout(db, 5000);
    {
      Statement version(db, "PRAGMA user_version");
      if (version.Step() != SQLITE_ROW || (version.Column(0) != "0" && version.Column(0) != "1" && version.Column(0) != "2")) return false;
    }
    if (!Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS relations(id TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS entries(id TEXT,path TEXT,value TEXT NOT NULL,PRIMARY KEY(id,path));"
        "CREATE TABLE IF NOT EXISTS pending(id TEXT,path TEXT,value TEXT NOT NULL,PRIMARY KEY(id,path)); PRAGMA user_version=2;")) return false;
    jobs.clear();
    Statement rows(db, "SELECT id,value FROM relations");
    int step;
    while ((step = rows.Step()) == SQLITE_ROW) {
      auto value = base::JSONReader::ReadDict(rows.Column(1), base::JSON_PARSE_RFC);
      auto r = std::make_unique<Relationship>();
      r->view.id = rows.Column(0);
      if (!value || !IsSyncId(r->view.id)) return false;
      const auto* root = value->FindString("root"); const auto* pin = value->FindString("peer");
      const auto* name = value->FindString("name"); const auto* identity = value->FindString("identity");
      const auto* single = value->FindString("single"); const auto* bookmark = value->FindString("bookmark");
      r->role = value->FindInt("role").value_or(-1);
      if (!root || !pin || pin->size() != 64 || !name || !identity || !single || !bookmark ||
          (r->role != 0 && r->role != 1) || !Number(*value, "counter", &r->counter)) return false;
      r->view.root = base::FilePath::FromUTF8Unsafe(*root);
      if (!r->view.root.IsAbsolute() || r->view.root.ReferencesParent() || (!single->empty() && !SafeSyncPath(*single))) return false;
      r->view.fingerprint = *pin; r->view.peer = *name; r->identity = *identity; r->single = *single; r->bookmark = *bookmark;
      r->view.paused = value->FindBool("paused").value_or(true);
      r->view.removed = value->FindBool("removed").value_or(true);
      r->confirmed = value->FindBool("confirmed").value_or(false);
      r->view.name = value->FindString("item_name") ? *value->FindString("item_name") :
          (r->single.empty() ? r->view.root.BaseName().AsUTF8Unsafe() : r->single);
      r->view.avatar = std::clamp(value->FindInt("avatar").value_or(0), 0, 8);
      // Existing registrations retain their previously granted two-way access.
      r->view.read = value->FindBool("read").value_or(true);
      r->view.write = value->FindBool("write").value_or(true);
      uint64_t started = 0, synced = 0;
      if ((value->contains("started") && !Number(*value, "started", &started)) ||
          (value->contains("last_synced") && !Number(*value, "last_synced", &synced))) return false;
      r->view.started = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(started));
      r->view.last_synced = base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(synced));
      r->view.status = IDS_CRFILES_SYNC_WAITING;
      r->view.recovery = r->view.root.AppendASCII(".crfiles-sync").AppendASCII(r->view.id);
      if (jobs.size() >= kMaxRegistrations) return false;
      if (r->view.removed) { jobs.emplace(r->view.id, std::move(r)); continue; }
      Statement entries(db, "SELECT path,value FROM entries WHERE id=?");
      if (!entries.Text(1, r->view.id)) return false;
      int entry_step;
      while ((entry_step = entries.Step()) == SQLITE_ROW) {
        auto entry = base::JSONReader::ReadDict(entries.Column(1), base::JSON_PARSE_RFC);
        Entry parsed; const auto path = entries.Column(0);
        if (!SafeSyncPath(path) || !entry || !Decode(*entry, &parsed) || r->index.size() >= kMaxEntries) return false;
        r->index.emplace(path, parsed);
      }
      if (entry_step != SQLITE_DONE || jobs.size() >= kMaxRegistrations) return false;
      jobs.emplace(r->view.id, std::move(r));
    }
    if (step != SQLITE_DONE) return false;
    loaded = true;
    return true;
  }
  bool Save(const Relationship& r) {
    base::DictValue value;
    value.Set("root", r.view.root.AsUTF8Unsafe()); value.Set("peer", r.view.fingerprint);
    value.Set("name", r.view.peer); value.Set("single", r.single); value.Set("identity", r.identity);
    value.Set("bookmark", r.bookmark); value.Set("role", r.role); value.Set("confirmed", r.confirmed);
    value.Set("paused", r.view.paused); value.Set("removed", r.view.removed);
    value.Set("counter", base::NumberToString(r.counter));
    value.Set("item_name", r.view.name); value.Set("avatar", r.view.avatar);
    value.Set("read", r.view.read); value.Set("write", r.view.write);
    value.Set("started", base::NumberToString(r.view.started.ToDeltaSinceWindowsEpoch().InMicroseconds()));
    value.Set("last_synced", base::NumberToString(r.view.last_synced.ToDeltaSinceWindowsEpoch().InMicroseconds()));
    Statement row(db, "INSERT OR REPLACE INTO relations VALUES(?,?)");
    return row.Text(1, r.view.id) && row.Text(2, Json(value)) && row.Step() == SQLITE_DONE;
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
    for (auto& [id, r] : jobs) {
      if (r->view.removed && r->view.status != IDS_CRFILES_SYNC_STORAGE_ERROR) continue;
      auto view = r->view;
      if (view.paused && view.status != IDS_CRFILES_SYNC_SOURCE_ERROR && view.status != IDS_CRFILES_SYNC_STORAGE_ERROR) view.status = IDS_CRFILES_SYNC_PAUSED;
      views.push_back(std::move(view));
      if (!r->watcher && !r->view.paused) {
        if (!watcher_runner) watcher_runner = base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()});
        r->watcher = StartWatch(watcher_runner, r->view.root, r->dirty);
      }
      if (r->view.paused) r->watcher.reset();
    }
    base::AutoLock lock(ui_lock); snapshot = std::move(views);
  }
  bool Overlaps(const base::FilePath& root, const std::string& ignore_id = {}) {
    // Disallow overlapping roots, including single files sharing a parent. This
    // keeps restoration folders and filesystem locks unambiguous in v1.
    for (const auto& [id, r] : jobs) if (id != ignore_id && !r->view.removed && !r->view.fingerprint.empty() &&
        (r->view.root == root || r->view.root.IsParent(root) || root.IsParent(r->view.root))) return true;
    return root == directory || root.IsParent(directory) || directory.IsParent(root);
  }
  bool Full() const {
    return jobs.size() >= kMaxRegistrations || static_cast<size_t>(std::ranges::count_if(jobs, [](const auto& pair) {
      return !pair.second->view.removed && !pair.second->view.fingerprint.empty();
    })) >= kMaxJobs;
  }
  bool Create(Command command) {
    if (Full()) return false;
    auto r = std::make_unique<Relationship>();
    r->view.id = command.id; r->view.fingerprint = command.fingerprint; r->view.peer = command.peer;
    base::File::Info info;
    if (base::IsLink(command.source) || !base::GetFileInfo(command.source, &info) || info.is_symbolic_link) return false;
    r->view.root = base::MakeAbsoluteFilePath(info.is_directory ? command.source : command.source.DirName());
    if (r->view.root.empty()) return false;
    r->single = info.is_directory ? "" : CanonicalSyncPath(command.source.BaseName().AsUTF8Unsafe());
    r->view.name = CanonicalSyncPath(command.source.BaseName().AsUTF8Unsafe());
    r->view.avatar = std::clamp(command.avatar, 0, 8); r->view.started = base::Time::Now();
    r->view.read = command.read; r->view.write = command.write;
    if (!SafeSyncPath(CanonicalSyncPath(command.source.BaseName().AsUTF8Unsafe())) ||
        (!r->single.empty() && !SafeSyncPath(r->single)) || Overlaps(r->view.root)) return false;
    SyncFiles files(r->view.root, r->view.id, [] { return false; });
    if (!files.Open()) return false;
    r->identity = files.Identity(); r->view.recovery = files.Recovery();
    for (const auto& [id, existing] : jobs)
      if (!existing->view.removed && existing->identity == r->identity) return false;
    r->bookmark = base::Base64Encode(CreateFileAccessBookmark(r->view.root));
    r->view.status = IDS_CRFILES_SYNC_WAITING;
    if (!Save(*r)) return false;
    jobs.emplace(r->view.id, std::move(r)); return true;
  }
  void Poll() {
    if (!Open()) {
      base::AutoLock lock(ui_lock);
      if (snapshot.empty()) snapshot.emplace_back();
      for (auto& job : snapshot) job.status = IDS_CRFILES_SYNC_STORAGE_ERROR;
      return;
    }
    std::vector<Command> pending;
    { base::AutoLock lock(ui_lock); pending.swap(commands); }
    for (auto& command : pending) {
      if (command.kind == 0) {
        if (!Create(command)) {
          // Keep a visible, dismissible failed request instead of silently losing
          // a rejected overlap, invalid name, or unavailable source.
          auto r = std::make_unique<Relationship>(); r->view.id = command.id;
          r->view.root = command.source; r->view.peer = command.peer;
          r->view.name = command.source.BaseName().AsUTF8Unsafe();
          r->view.avatar = std::clamp(command.avatar, 0, 8);
          r->view.read = command.read; r->view.write = command.write;
          r->view.status = IDS_CRFILES_SYNC_SOURCE_ERROR; r->view.paused = true;
          jobs.emplace(command.id, std::move(r));
        }
        continue;
      }
      bool retry = false;
      for (auto& [id, r] : jobs) {
        if (command.kind == 3 ? r->view.fingerprint != command.fingerprint : id != command.id) continue;
        const bool old_removed = r->view.removed;
        if (command.kind == 4) {
          r->view.read = command.read; r->view.write = command.write;
        } else r->view.paused = command.kind == 1 ? command.paused : true;
        r->view.removed = old_removed || command.kind == 2 || command.kind == 3;
        if (!r->view.fingerprint.empty() && !Save(*r)) {
          // A failed durable write must never leave a requested pause/revoke
          // active in this process. Keep the command for a later storage retry.
          r->view.paused = true;
          r->view.status = IDS_CRFILES_SYNC_STORAGE_ERROR;
          retry = true;
        } else {
          r->dirty->value = true; r->next = {}; r->failures = 0;
          r->view.status = IDS_CRFILES_SYNC_WAITING;
          if (r->view.removed) r->watcher.reset();
        }
      }
      if (retry) { base::AutoLock lock(ui_lock); commands.push_back(command); }
    }
    std::erase_if(jobs, [](const auto& pair) {
      return pair.second->view.removed && pair.second->view.fingerprint.empty();
    });
    Publish();
  }
  bool Interrupted(const std::string& id, SyncWire& wire) {
    if (wire.Canceled()) return true;
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
    if (!r.view.write) {
      Statement pending(db, "SELECT 1 FROM pending WHERE id=? LIMIT 1");
      if (!pending.Text(1, r.view.id) || pending.Step() != SQLITE_DONE) {
        // Do not replay a previous receive journal after local Write is revoked.
        r.view.status = IDS_CRFILES_SYNC_RECOVERY_REQUIRED; return false;
      }
    }
    if (!Recover(r, files)) { r.view.status = IDS_CRFILES_SYNC_RECOVERY_REQUIRED; return false; }
    // Events only invalidate a snapshot. Periodic full hashing also catches
    // missed/overflowed notifications and edits made while the app was stopped.
    if (!r.dirty->value.exchange(false) && base::TimeTicks::Now() - r.scanned < base::Seconds(30)) return true;
    std::map<std::string, SyncContent> observed;
    if (!files.Scan(r.single, &observed)) { r.dirty->value = true; r.view.status = IDS_CRFILES_SYNC_SOURCE_ERROR; return false; }
    Index next = r.index;
    for (const auto& [path, value] : observed) {
      auto& e = next[path];
      if (static_cast<const SyncContent&>(e) != value) {
        if (++r.counter >= kMaxCounter) return false;
        static_cast<SyncContent&>(e) = value; e.version[r.role] = r.counter;
      }
    }
    for (auto& [path, e] : next) if (e.kind && !observed.contains(path)) {
      if (++r.counter >= kMaxCounter) return false;
      static_cast<SyncContent&>(e) = {}; e.version[r.role] = r.counter;
    }
    size_t path_bytes = 0;
    for (const auto& [path, entry] : next) path_bytes += path.size();
    if (next.size() > kMaxEntries || path_bytes > 32 * 1024 * 1024) return false;
    r.index = std::move(next);
    if (!SaveIndex(r)) { loaded = false; return false; }
    r.scanned = base::TimeTicks::Now(); return true;
  }
  bool SendIndex(SyncWire& wire, const Index& index) {
    auto it = index.begin();
    do {
      auto page = Message("sync-index"); base::ListValue entries;
      for (size_t n = 0; n < 32 && it != index.end(); ++n, ++it) {
        auto entry = Encode(it->second); entry.Set("path", it->first); entries.Append(std::move(entry));
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
  bool ReadIndex(SyncWire& wire, const Relationship& r, Index* index) {
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
  bool Exchange(SyncWire& wire, Relationship& r, SyncFiles& files, bool peer_read, bool peer_write) {
    const bool send = r.view.read && peer_write;
    const bool receive = r.view.write && peer_read;
    // No file/index access when neither direction is permitted.
    if (!send && !receive) {
      r.view.status = IDS_CRFILES_SYNC_NO_PERMISSION;
      return wire.Write(Message("sync-idle")) && Type(wire.Read(), "sync-idle");
    }
    if (files.Identity() != r.identity || !Scan(r, files)) return false;
    Index remote;
    if (send && receive) {
      if (r.role == 0) {
        if (!SendIndex(wire, r.index) || !ReadIndex(wire, r, &remote)) return false;
      } else if (!ReadIndex(wire, r, &remote) || !SendIndex(wire, r.index)) return false;
    } else if (send) {
      if (!SendIndex(wire, r.index) || !ReadIndex(wire, r, &remote)) return false;
    } else {
      if (!ReadIndex(wire, r, &remote)) return false;
      Index receipts;
      // Reveal only whether the advertised source content is already present;
      // never expose a receiver edit's bytes, hash, version or private names.
      for (const auto& [path, entry] : remote)
        receipts.emplace(path, Same(At(r.index, path), entry) ? entry : Entry());
      if (!SendIndex(wire, receipts)) return false;
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
      // Receiver edits replaced by the source remain in its recovery archive;
      // receiver-only paths are left alone. Ancestor conflicts fail safely.
      const auto& source = send ? r.index : remote;
      std::set<std::u16string> folded;
      for (const auto& [path, entry] : source) {
        if (!SafeSyncPath(path) || !folded.insert(base::i18n::FoldCase(base::UTF8ToUTF16(path))).second) return false;
        if (entry.kind) for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent))
          if (At(source, parent).kind != 2) return false;
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
      open.Set("permissions", 1); open.Set("read", relation->view.read); open.Set("write", relation->view.write);
      if (!wire.Write(std::move(open))) return false;
      const auto accepted = wire.Read();
      if (Type(accepted, "sync-declined")) {
        relation->view.paused = true; Save(*relation); Publish(); return false;
      }
      if (!Type(accepted, "sync-accepted") || accepted->FindInt("permissions") != 1 ||
          !accepted->FindBool("read") || !accepted->FindBool("write")) return false;
      peer_read = *accepted->FindBool("read"); peer_write = *accepted->FindBool("write");
      relation->confirmed = true;
      if (!Save(*relation)) return false;
    } else {
      const auto open = wire.Read();
      const auto* remote_id = open ? open->FindString("id") : nullptr;
      if (!Type(open, "sync-open") || !remote_id || *remote_id != id || open->FindInt("permissions") != 1 ||
          !open->FindBool("read") || !open->FindBool("write")) return false;
      peer_read = *open->FindBool("read"); peer_write = *open->FindBool("write");
      if (it == jobs.end()) {
        const auto* name = open->FindString("name"); const auto file = open->FindBool("file");
        if (!open->FindBool("create").value_or(false) || !name || !SafeSyncPath(*name) ||
            name->find('/') != std::string::npos || !file || Full()) return false;
        auto r = std::make_unique<Relationship>();
        r->view.id = id; r->view.fingerprint = fingerprint; r->view.peer = peer;
        r->view.root = receive_directory.AppendASCII("Sync-" + id);
        r->single = *file ? *name : ""; r->role = 1; r->confirmed = true;
        r->view.name = *name; r->view.started = base::Time::Now();
        r->view.avatar = std::clamp(avatar, 0, 8);
        if (Overlaps(r->view.root) || base::PathExists(r->view.root)) return false;
        if (!wire.Consent(r->view.root, *name)) {
          wire.Write(Message("sync-declined")); return false;
        }
        // Register consent durably before the first file. A withdrawn relation
        // remains a tombstone in this database and can never be silently recreated.
        r->bookmark = base::Base64Encode(CreateFileAccessBookmark(receive_directory));
        if (!Save(*r)) return false;
        relation = r.get(); jobs.emplace(id, std::move(r));
      } else {
        relation = it->second.get();
        if (relation->role != 1 || relation->view.fingerprint != fingerprint ||
            relation->view.paused || relation->view.removed) return false;
      }
      if (relation->identity.empty()) {
        if (!base::CreateDirectory(relation->view.root)) return false;
        const auto canonical_root = base::MakeAbsoluteFilePath(relation->view.root);
        if (canonical_root.empty() || Overlaps(canonical_root, id)) return false;
        relation->view.root = canonical_root;
        SyncFiles initial(relation->view.root, id, [&] { return Interrupted(id, wire); });
        if (!initial.Open()) return false;
        relation->identity = initial.Identity(); relation->view.recovery = initial.Recovery();
        if (!Save(*relation)) return false;
      }
      auto accepted = Message("sync-accepted"); accepted.Set("permissions", 1);
      accepted.Set("read", relation->view.read); accepted.Set("write", relation->view.write);
      if (!wire.Write(std::move(accepted))) return false;
    }
    relation->view.peer = peer; relation->view.avatar = std::clamp(avatar, 0, 8);
    if (!Save(*relation)) return false;
    std::string bookmark;
    if (!base::Base64Decode(relation->bookmark, &bookmark)) return false;
    auto access = ResolveFileAccessBookmark(std::vector<uint8_t>(bookmark.begin(), bookmark.end()));
    SyncFiles files(relation->view.root, id, [&] { return Interrupted(id, wire) || !wire.KeepAlive(); });
    relation->view.status = IDS_CRFILES_SYNC_ACTIVE; Publish();
    const bool opened = files.Open();
    if (!opened || files.Identity() != relation->identity) relation->view.status = IDS_CRFILES_SYNC_SOURCE_ERROR;
    bool ok = opened && Exchange(wire, *relation, files, peer_read, peer_write);
    const bool idle = relation->view.status == IDS_CRFILES_SYNC_NO_PERMISSION;
    if (ok && !idle) {
      const auto previous = relation->view.last_synced;
      relation->view.last_synced = base::Time::Now();
      if (!Save(*relation)) { relation->view.last_synced = previous; ok = false; }
    }
    if (!idle && relation->view.status != IDS_CRFILES_SYNC_RECOVERY_REQUIRED && relation->view.status != IDS_CRFILES_SYNC_SOURCE_ERROR)
      relation->view.status = ok ? IDS_CRFILES_SYNC_CURRENT : IDS_CRFILES_SYNC_RETRY;
    relation->failures = ok ? 0 : std::min(relation->failures + 1, 5);
    relation->next = base::TimeTicks::Now() + base::Seconds(ok ? 10 : 10 * (1 << relation->failures)) +
        base::Milliseconds(base::RandIntInclusive(0, 1500));
    if (!ok) relation->dirty->value = true;
    Publish(); return ok;
  }
};

SyncService::SyncService(base::FilePath directory) : impl_(std::make_unique<Impl>(std::move(directory))) {}
SyncService::~SyncService() = default;
bool SyncService::Queue(const base::FilePath& source, std::string fingerprint, std::string peer, int avatar, bool write) {
  if (!source.IsAbsolute() || source.ReferencesParent() || fingerprint.size() != 64) return false;
  base::AutoLock lock(impl_->ui_lock);
  if (impl_->commands.size() >= kMaxJobs) return false;
  impl_->commands.push_back({0, RandomId(), source, std::move(fingerprint), std::move(peer), false, avatar, true, write}); return true;
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
void SyncService::Poll() { base::AutoLock lock(impl_->io_lock); impl_->Poll(); }
std::optional<SyncRequest> SyncService::Next(const std::vector<SyncEndpoint>& peers) {
  base::AutoLock lock(impl_->io_lock);
  impl_->Poll();
  if (!impl_->loaded) return std::nullopt;
  const auto now = base::TimeTicks::Now();
  for (auto& [id, r] : impl_->jobs) {
    if (r->role != 0 || r->view.paused || r->view.removed || r->next > now || r->view.fingerprint.empty()) continue;
    const auto peer = std::ranges::find(peers, r->view.fingerprint, &SyncEndpoint::fingerprint);
    if (peer == peers.end()) { r->view.status = IDS_CRFILES_SYNC_WAITING; continue; }
    r->next = now + base::Seconds(30);
    return SyncRequest{id, peer->address, peer->fingerprint};
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
  return impl_->loaded && it != impl_->jobs.end() && it->second->view.removed &&
      it->second->view.fingerprint == fingerprint;
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
  if (it->second->view.status != IDS_CRFILES_SYNC_RECOVERY_REQUIRED && it->second->view.status != IDS_CRFILES_SYNC_SOURCE_ERROR)
    it->second->view.status = IDS_CRFILES_SYNC_RETRY;
  it->second->next = std::max(it->second->next, base::TimeTicks::Now() + base::Seconds(30) +
      base::Milliseconds(base::RandIntInclusive(0, 1500)));
  impl_->Publish();
}
}  // namespace crfiles
