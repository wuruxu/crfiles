// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_thumbnail_cache.h"
#include <algorithm>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/environment.h"
#include "base/files/file_enumerator.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "crFiles/crfiles_cache_state.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/image/image_skia_rep.h"
#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#endif
namespace crfiles {
namespace {
constexpr size_t kMaximumBytes = 256 * 1024 * 1024;
constexpr size_t kMaximumItems = 4096;
constexpr size_t kMaximumFileBytes = 128 * 128 * 4 + 1024;
bool Stopped(const std::shared_ptr<std::atomic_bool>& canceled) {
  return canceled && canceled->load(std::memory_order_relaxed);
}
base::FilePath CacheDirectory() {
#if BUILDFLAG(IS_MAC)
  if (IsCrFilesSandboxed()) return MacCacheDirectory().AppendASCII("thumbnails-v1");
#endif
  base::FilePath root;
#if BUILDFLAG(IS_WIN)
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &root)) return {};
  root = root.AppendASCII("crFiles").AppendASCII("Cache");
#else
  const auto cache = base::Environment::Create()->GetVar("XDG_CACHE_HOME");
  if (cache) root = base::FilePath(*cache);
  if (!root.IsAbsolute()) {
    if (!base::PathService::Get(base::DIR_HOME, &root)) return {};
    root = root.AppendASCII(".cache");
  }
  root = root.AppendASCII("crFiles");
#endif
  // Bump the namespace if orientation, tone mapping or pixel encoding changes.
  return root.AppendASCII("thumbnails-v1");
}
std::string CacheKey(const base::FilePath& path, const base::File::Info& info, int edge) {
  base::Pickle key;
  key.WriteString(path.AsUTF8Unsafe()); key.WriteInt64(info.size);
  key.WriteInt64(info.last_modified.ToDeltaSinceWindowsEpoch().InMicroseconds());
  key.WriteInt64(info.creation_time.ToDeltaSinceWindowsEpoch().InMicroseconds());
  key.WriteInt(edge); key.WriteInt(static_cast<int>(kN32_SkColorType));
  if (path.MatchesFinalExtension(FILE_PATH_LITERAL(".svg"))) key.WriteString("svg-v2");
  return base::HexEncode(crypto::hash::Sha256(key.AsBytes())) + ".thumb";
}
struct DiskItem { std::string name; size_t bytes; base::Time touched; };
// Disk IO is serialized separately from the memory cache. No UI thread or
// decoder holds this lock. The index is built once, not on every thumbnail.
class DiskCache {
 public:
  DiskCache();
  ~DiskCache();
  bool Initialize();
  bool Erase(std::list<DiskItem>::iterator item);
  bool Trim(size_t incoming);
  base::Lock lock;
  base::FilePath directory;
  std::list<DiskItem> recent;
  std::map<std::string, std::list<DiskItem>::iterator> index;
  size_t bytes = 0;
  bool initialized = false;
};
DiskCache::DiskCache() = default;
DiskCache::~DiskCache() = default;
bool DiskCache::Initialize() {
  if (initialized) return !directory.empty();
  initialized = true; directory = CacheDirectory();
  if (directory.empty() || !base::CreateDirectory(directory)) { directory.clear(); return false; }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(directory, 0700)) { directory.clear(); return false; }
#endif
  std::vector<DiskItem> entries;
  base::FileEnumerator files(directory, false, base::FileEnumerator::FILES);
  for (auto path = files.Next(); !path.empty(); path = files.Next()) {
    if (!path.MatchesExtension(FILE_PATH_LITERAL(".thumb"))) continue;
    const auto name = path.BaseName().AsUTF8Unsafe();
    if (name.size() != 70 || !std::ranges::all_of(std::string_view(name).substr(0, 64),
        [](char c) { return base::IsHexDigit(c); })) continue;
    const auto info = files.GetInfo();
    if (info.GetSize() <= 0 || info.GetSize() > static_cast<int64_t>(kMaximumFileBytes)) {
      base::DeleteFile(path); continue;
    }
    entries.push_back({name, static_cast<size_t>(info.GetSize()), info.GetLastModifiedTime()});
  }
  std::ranges::sort(entries, std::greater<>(), &DiskItem::touched);
  for (auto& item : entries) {
    bytes += item.bytes; recent.push_back(std::move(item));
    index.emplace(recent.back().name, std::prev(recent.end()));
  }
  Trim(0);
  return true;
}
bool DiskCache::Erase(std::list<DiskItem>::iterator item) {
  if (!base::DeleteFile(directory.AppendASCII(item->name))) return false;
  bytes -= item->bytes; index.erase(item->name); recent.erase(item); return true;
}
bool DiskCache::Trim(size_t incoming) {
  while (!recent.empty() && (bytes + incoming > kMaximumBytes || recent.size() + (incoming ? 1 : 0) > kMaximumItems))
    if (!Erase(std::prev(recent.end()))) return false;
  return true;
}
DiskCache& Cache() { static base::NoDestructor<DiskCache> cache; return *cache; }
gfx::ImageSkia DecodeCache(std::string_view encoded, int edge) {
  auto reader = base::PickleIterator::WithData(base::as_byte_span(encoded));
  int version = 0, width = 0, height = 0;
  std::string checksum;
  if (!reader.ReadInt(&version) || version != 1 || !reader.ReadInt(&width) || !reader.ReadInt(&height) ||
      width <= 0 || height <= 0 || width > edge || height > edge || !reader.ReadString(&checksum)) return {};
  const auto pixels = reader.ReadData();
  if (!pixels || pixels->size() != static_cast<size_t>(width) * height * 4 ||
      checksum != base::HexEncode(crypto::hash::Sha256(*pixels))) return {};
  SkBitmap bitmap;
  if (!bitmap.tryAllocN32Pixels(width, height) ||
      !bitmap.writePixels(SkPixmap(SkImageInfo::MakeN32Premul(width, height), pixels->data(), static_cast<size_t>(width) * 4))) return {};
  bitmap.setImmutable();
  auto image = gfx::ImageSkia::CreateFromBitmap(bitmap, 1.f);
  image.MakeThreadSafe(); return image;
}
}
gfx::ImageSkia ReadDiskThumbnail(const base::FilePath& path, const base::File::Info& info,
    int edge, const std::shared_ptr<std::atomic_bool>& canceled) {
  if (edge <= 0 || edge > 128 || Stopped(canceled)) return {};
  const auto name = CacheKey(path, info, edge);
  std::string encoded;
  auto& cache = Cache();
  base::FilePath file;
  bool should_touch = false;
  {
    base::AutoLock guard(cache.lock);
    if (Stopped(canceled) || !cache.Initialize()) return {};
    const auto found = cache.index.find(name);
    if (found == cache.index.end()) return {};
    file = cache.directory.AppendASCII(name);
    const auto now = base::Time::Now();
    if (now - found->second->touched >= base::Minutes(1)) {
      found->second->touched = now;
      should_touch = true;
    }
    cache.recent.splice(cache.recent.begin(), cache.recent, found->second);
  }
  if (Stopped(canceled)) return {};
  if (!base::ReadFileToStringWithMaxSize(file, &encoded, kMaximumFileBytes)) {
    base::AutoLock guard(cache.lock);
    if (auto found = cache.index.find(name); found != cache.index.end()) cache.Erase(found->second);
    return {};
  }
  if (should_touch) {
    const auto now = base::Time::Now();
    base::TouchFile(file, now, now);
  }
  if (Stopped(canceled)) return {};
  auto image = DecodeCache(encoded, edge);
  if (image.isNull()) {
    base::AutoLock guard(cache.lock);
    if (auto found = cache.index.find(name); found != cache.index.end()) cache.Erase(found->second);
  }
  return image;
}
namespace {
struct PendingWrites {
  base::Lock lock;
  std::set<std::string> keys;
};
PendingWrites& Writes() { static base::NoDestructor<PendingWrites> writes; return *writes; }
struct WritePermit {
  explicit WritePermit(std::string value);
  ~WritePermit();
  std::string key;
};
WritePermit::WritePermit(std::string value) : key(std::move(value)) {}
WritePermit::~WritePermit() {
  auto& writes = Writes(); base::AutoLock guard(writes.lock); writes.keys.erase(key);
}
scoped_refptr<base::SequencedTaskRunner> DiskWriter() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::BEST_EFFORT}));
  return *runner;
}
void StoreDiskThumbnail(const base::FilePath& path, const base::File::Info& info,
    int edge, const gfx::ImageSkia& image, const std::shared_ptr<std::atomic_bool>& canceled) {
  if (edge <= 0 || edge > 128 || image.isNull() || Stopped(canceled)) return;
  const auto& bitmap = image.GetRepresentation(1.f).GetBitmap();
  const int width = bitmap.width(), height = bitmap.height();
  if (width <= 0 || height <= 0 || width > edge || height > edge) return;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  if (!bitmap.readPixels(SkImageInfo::MakeN32Premul(width, height), pixels.data(), static_cast<size_t>(width) * 4, 0, 0)) return;
  base::Pickle encoded;
  encoded.WriteInt(1); encoded.WriteInt(width); encoded.WriteInt(height);
  encoded.WriteString(base::HexEncode(crypto::hash::Sha256(pixels))); encoded.WriteData(pixels);
  const auto name = CacheKey(path, info, edge);
  auto& cache = Cache();
  base::FilePath target_file;
  {
    base::AutoLock guard(cache.lock);
    if (Stopped(canceled) || !cache.Initialize()) return;
    if (cache.index.contains(name)) return;
    // All stores run on DiskWriter's sequence, so another writer cannot
    // consume this capacity while the lock is released for the file write.
    if (!cache.Trim(encoded.AsBytes().size())) return;
    target_file = cache.directory.AppendASCII(name);
  }
  if (!base::ImportantFileWriter::WriteFileAtomically(target_file, encoded.AsStringView())) return;
  base::AutoLock guard(cache.lock);
  if (cache.index.contains(name)) return;
  // A canceled write must not leave an unindexed file. If deletion fails,
  // register it so subsequent eviction can still account for and retry it.
  if (Stopped(canceled) && base::DeleteFile(target_file)) return;
  cache.recent.push_front({name, encoded.AsBytes().size(), base::Time::Now()});
  cache.index.emplace(name, cache.recent.begin()); cache.bytes += encoded.AsBytes().size();
}
}
void WriteDiskThumbnail(const base::FilePath& path, const base::File::Info& info,
    int edge, const gfx::ImageSkia& image, const std::shared_ptr<std::atomic_bool>& canceled) {
  if (edge <= 0 || edge > 128 || image.isNull() || Stopped(canceled)) return;
  const auto key = CacheKey(path, info, edge);
  auto& writes = Writes();
  {
    base::AutoLock guard(writes.lock);
    // At most 2 MiB of 128 px image payloads waiting for disk. Drop optional
    // cache writes under pressure instead of delaying a thumbnail result.
    if (writes.keys.size() >= 32 || !writes.keys.insert(key).second) return;
  }
  auto epoch = ObserveFileCachePath(path, false);
  const auto revision = epoch->revision.load();
  DiskWriter()->PostTask(FROM_HERE, base::BindOnce([](std::unique_ptr<WritePermit>,
      base::FilePath file, base::File::Info before, int size, gfx::ImageSkia thumbnail,
      std::shared_ptr<std::atomic_bool> token, std::shared_ptr<FileCacheEpoch> observed, uint64_t version) {
    if (Stopped(token) || !observed->Matches(version)) return;
    base::File::Info current;
    if (!base::GetFileInfo(file, &current) || current.is_directory || before.size != current.size ||
        before.last_modified != current.last_modified || before.creation_time != current.creation_time) return;
    if (!observed->Matches(version) || Stopped(token)) return;
    StoreDiskThumbnail(file, before, size, thumbnail, token);
  }, std::make_unique<WritePermit>(key), path, info, edge, image, canceled, std::move(epoch), revision));
}
}
