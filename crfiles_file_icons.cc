// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_icons.h"
#include "crFiles/crfiles_cache_state.h"
#include <algorithm>
#include <map>
#include <string_view>
#include <utility>
#include "build/build_config.h"
#if BUILDFLAG(IS_LINUX)
#include <gio/gio.h>
#elif BUILDFLAG(IS_MAC)
#include <sys/mount.h>
#include "base/files/drive_info.h"
#endif
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"
#include "libcr/svg_icon.h"
#include "net/base/mime_util.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/modules/svg/include/SkSVGDOM.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <iomanip>
#include <tuple>
#include "base/containers/lru_cache.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/win/scoped_gdi_object.h"
#include "ui/gfx/image/image_skia_rep.h"
#include "ui/gfx/win/icon_util.h"
#include "third_party/skia/include/core/SkBitmap.h"
#endif
namespace crfiles {
namespace {
using IconKey = std::pair<std::string, int>;
base::Lock& IconLock() { static base::NoDestructor<base::Lock> lock; return *lock; }
std::map<IconKey, gfx::ImageSkia>& IconCache() {
  // Bounded by the fixed asset set times the six supported sizes.
  static base::NoDestructor<std::map<IconKey, gfx::ImageSkia>> cache;
  return *cache;
}
std::map<std::string, std::string>& SvgSourceCache() {
  static base::NoDestructor<std::map<std::string, std::string>> cache;
  return *cache;
}
std::string GetSvgSource(std::string_view asset) {
  {
    base::AutoLock guard(IconLock());
    auto found = SvgSourceCache().find(std::string(asset));
    if (found != SvgSourceCache().end()) return found->second;
  }
  base::FilePath assets;
  if (!base::PathService::Get(base::DIR_ASSETS, &assets)) return {};
  const auto path = assets.AppendASCII("crfiles_resources").AppendASCII("file_icons").AppendASCII(std::string(asset) + ".svg");
  std::string svg;
  if (!base::ReadFileToStringWithMaxSize(path, &svg, 1024 * 1024)) return {};
  base::AutoLock guard(IconLock());
  return SvgSourceCache().try_emplace(std::string(asset), std::move(svg)).first->second;
}
gfx::ImageSkia ReadIcon(std::string_view asset, int edge) {
  std::string svg = GetSvgSource(asset);
  if (svg.empty()) return {};
  SkMemoryStream stream(svg.data(), svg.size(), true);
  auto dom = SkSVGDOM::MakeFromStream(stream);
  if (!dom) return {};
  auto size = dom->containerSize();
  if (size.isEmpty()) size = SkSize::Make(48, 48);
  libcr::SvgIconCache renderer(std::move(svg), gfx::SizeF(size.width(), size.height()));
  auto model = renderer.GetImageModel(gfx::Size(edge, edge));
  if (!model.IsImage()) return {};
  auto image = model.GetImage().AsImageSkia();
  image.MakeThreadSafe();
  return image;
}
gfx::ImageSkia LoadNamedIcon(std::string_view asset, int edge) {
  const IconKey key{std::string(asset), edge};
  {
    base::AutoLock guard(IconLock());
    const auto found = IconCache().find(key);
    if (found != IconCache().end()) return found->second;
  }
  auto image = ReadIcon(key.first, edge);
  base::AutoLock guard(IconLock());
  return IconCache().try_emplace(key, std::move(image)).first->second;
}
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
// Only probe potential volume roots on the icon worker. Ordinary folders and
// all virtual archive/SMB rows use the memory-only bundled type lookup.
bool MayBeVolume(const base::FilePath& path) {
#if BUILDFLAG(IS_LINUX)
  if (!path.value().starts_with("/media/") && !path.value().starts_with("/mnt/") &&
      !path.value().starts_with("/run/media/") && !path.value().starts_with("/run/user/")) return false;
#endif
  const auto parent = path.DirName();
#if BUILDFLAG(IS_MAC)
  return parent.value() == "/Volumes";
#else
  const auto grandparent = parent.DirName();
  return parent.value() == "/mnt" || parent.value() == "/media" ||
      grandparent.value() == "/media" || grandparent.value() == "/run/media" ||
      (parent.BaseName().value() == "gvfs" && grandparent.DirName().value() == "/run/user");
#endif
}
std::string_view VolumeAsset(const base::FilePath& path) {
#if BUILDFLAG(IS_LINUX)
  g_autoptr(GFile) file = g_file_new_for_path(path.value().c_str());
  g_autoptr(GMount) mount = g_file_find_enclosing_mount(file, nullptr, nullptr);
  if (!mount) return {};
  g_autoptr(GFile) root = g_mount_get_root(mount);
  if (!g_file_equal(root, file)) {
    // GVfs can expose the root as a device URI and the row as its FUSE path.
    g_autofree gchar* mount_path = g_file_get_path(root);
    if (!mount_path || path.value() != mount_path) return {};
  }
  if (g_file_has_uri_scheme(root, "gphoto2")) return "camera-photo";
  g_autoptr(GIcon) icon = g_mount_get_icon(mount);
  if (!G_IS_THEMED_ICON(icon)) return {};
  const gchar* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
  auto has = [names](const char* name) { return g_strv_contains(names, name); };
  if (has("camera-photo") || has("camera-photo-symbolic")) return "camera-photo";
  if (has("media-flash") || has("media-flash-sd") || has("media-flash-sd-mmc") ||
      has("drive-removable-media-flash") || has("drive-removable-media-flash-sd") ||
      has("drive-removable-media-flash-sd-mmc")) return "media-flash";
  if (has("drive-removable-media") || has("drive-removable-media-symbolic") ||
      has("drive-removable-media-usb") || has("drive-harddisk-usb")) return "drive-removable-media";
  if (has("drive-harddisk") || has("drive-harddisk-symbolic")) return "drive-harddisk";
#else
  struct statfs volume;
  if (statfs(path.value().c_str(), &volume) == 0 && path.value() == volume.f_mntonname) {
    auto info = base::GetFileDriveInfo(path);
    if (info && info->is_removable.has_value())
      return *info->is_removable ? "drive-removable-media" : "drive-harddisk";
  }
#endif
  return {};
}
#endif
}  // namespace

// Fixed, licensed project assets. No theme discovery or directory enumeration.
std::string_view BundledFileIconAsset(const base::FilePath& path, std::string_view mime, bool directory) {
  if (directory) return "inode-directory";
  const auto basename = path.BaseName().AsUTF8Unsafe();
  const auto name = base::ToLowerASCII(basename);
  const std::string_view filename(name);
  const auto dot = filename.rfind('.');
  const auto ext = dot == filename.npos ? std::string_view() : filename.substr(dot);
  const bool doc_suffix = ext.empty() || ext == ".md" || ext == ".txt" || ext == ".rst" ||
                          ext == ".markdown" || ext == ".adoc";
  if (doc_suffix) {
    const auto stem = filename.substr(0, dot);
    if (stem == "readme") return "text-x-readme";
    if (stem == "license" || stem == "licence" || stem == "copying") return "text-x-copying";
    if (stem == "changelog") return "text-x-changelog";
    if (stem == "authors" || stem == "credits") return "text-x-authors";
    // The extensionless lowercase name is also used by the Unix install command.
    if (stem == "install" && (!ext.empty() || basename == "INSTALL")) return "text-x-install";
  }
  if (ext == ".java" || mime == "text/x-java" || mime == "text/x-java-source") return "text-x-java-source";
  if (ext == ".php" || ext == ".phtml" || mime == "application/x-php" || mime == "text/x-php" ||
      mime == "application/x-httpd-php") return "application-x-php";
  if (ext == ".rb" || ext == ".rake" || ext == ".gemspec" || name == "gemfile" || name == "rakefile" ||
      mime == "application/x-ruby" || mime == "text/x-ruby") return "application-x-ruby";
  if (ext == ".torrent" || mime == "application/x-bittorrent") return "application-x-bittorrent";
  if (name == "makefile" || name == "gnumakefile" || ext == ".mk" || mime == "text/x-makefile")
    return "text-x-makefile";
  if (ext == ".diff" || ext == ".patch" || mime == "text/x-patch" || mime == "text/x-diff")
    return "text-x-patch";
  if (ext == ".py" || ext == ".pyi" || ext == ".pyw" || mime == "text/x-python" || mime == "application/x-python")
    return "text-x-python";
  if (ext == ".css" || mime == "text/css") return "gnome-mime-text-css";
  if (ext == ".c" || ext == ".h" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
      ext == ".hh" || ext == ".hpp" || ext == ".hxx" || ext == ".m" || ext == ".mm" ||
      mime == "text/x-c" || mime == "text/x-c++src" || mime == "text/x-csrc" ||
      mime == "text/x-chdr" || mime == "text/x-c++hdr") return "text-x-source";
  if (mime.starts_with("image/")) return "image-x-generic";
  if (mime.starts_with("audio/")) return "audio-x-generic";
  if (mime.starts_with("video/")) return "video-x-generic";
  if (mime.starts_with("font/") || mime.find("font") != std::string::npos) return "font-x-generic";
  if (mime.find("word") != std::string::npos || mime.find("opendocument.text") != std::string::npos || mime == "application/pdf") return "x-office-document";
  if (mime.find("spreadsheet") != std::string::npos || mime.find("excel") != std::string::npos) return "x-office-spreadsheet";
  if (mime.find("presentation") != std::string::npos || mime.find("powerpoint") != std::string::npos) return "x-office-presentation";
  if (mime.find("zip") != std::string::npos || mime.find("tar") != std::string::npos ||
      mime.find("compressed") != std::string::npos || mime.find("archive") != std::string::npos) return "package-x-generic";
  if (mime.find("html") != std::string::npos) return "text-html";
  if (mime.find("script") != std::string::npos || mime.find("python") != std::string::npos || mime.find("shell") != std::string::npos) return "text-x-script";
  if (mime.starts_with("text/")) return "text-x-generic";
  if (mime.find("executable") != std::string::npos || mime.find("sharedlib") != std::string::npos) return "application-x-executable";
  // Use Adwaita's lined document for files without a dedicated type icon.
  return "text-x-generic";
}
std::optional<gfx::ImageSkia> FindCachedBundledIcon(std::string_view asset, int edge) {
  base::AutoLock guard(IconLock());
  auto found = IconCache().find({std::string(asset), edge});
  if (found == IconCache().end()) return std::nullopt;
  return found->second;
}
gfx::ImageSkia LoadBundledIcon(std::string_view asset, int edge) {
  if (std::ranges::find(kFileIconSizes, edge) == kFileIconSizes.end()) return {};
  return LoadNamedIcon(asset, edge);
}

gfx::ImageSkia LoadNetworkServerIcon(int edge) {
  if (std::ranges::find(kFileIconSizes, edge) == kFileIconSizes.end()) return {};
  return LoadNamedIcon("network-server", edge);
}

gfx::ImageSkia LoadNetworkWorkgroupIcon(int edge) { return LoadNamedIcon("network-workgroup", edge); }
gfx::ImageSkia LoadTrashIcon(bool full, int edge) { return LoadNamedIcon(full ? "user-trash-full" : "user-trash", edge); }
gfx::ImageSkia LoadArchiveLockIcon() { return LoadNamedIcon("locked", 20); }
gfx::ImageSkia LoadRemoteFileIcon(const std::string& name, bool directory, int edge) {
  std::string mime;
  const auto path = base::FilePath::FromUTF8Unsafe(name);
  auto extension = path.Extension();
  if (!extension.empty()) extension.erase(0, 1);
  if (!directory) net::GetWellKnownMimeTypeFromExtension(extension, &mime);
  return LoadBundledIcon(BundledFileIconAsset(path, mime, directory), edge);
}

#if BUILDFLAG(IS_WIN)
namespace {
Microsoft::WRL::ComPtr<IImageList> GetSystemImageList(int size) {
  // Keep COM references within the caller's initialized apartment. Worker
  // threads can outlive a scoped COM initialization; cache decoded icons instead.
  Microsoft::WRL::ComPtr<IImageList> images;
  if (SUCCEEDED(SHGetImageList(size, IID_PPV_ARGS(&images)))) {
    return images;
  }
  return nullptr;
}
bool IsDriveIconPath(const base::FilePath& path) {
  const auto& value = path.value();
  return value.size() == 3 && value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}
using SystemIconKey = std::tuple<base::FilePath, bool, std::string, int>;
struct CachedSystemIcon {
  gfx::ImageSkia image;
  base::TimeTicks loaded_at;
  std::shared_ptr<FileCacheEpoch> epoch;
  uint64_t revision = 0;
  size_t bytes = 0;
};
// Both levels store only thread-safe pixels, never apartment-bound COM objects.
// Count shared pixels conservatively in each entry to keep simple hard budgets.
template <typename Key>
class BoundedIconCache {
 public:
  BoundedIconCache(size_t entries, size_t bytes) : max_entries_(entries), max_bytes_(bytes) {}
  std::optional<gfx::ImageSkia> Get(const Key& key) {
    auto found = items_.Get(key);
    if (found == items_.end()) return std::nullopt;
    if (base::TimeTicks::Now() - found->second.loaded_at >= base::Seconds(30) ||
        (found->second.epoch && !found->second.epoch->Matches(found->second.revision))) {
      bytes_ -= found->second.bytes;
      items_.Erase(found);
      return std::nullopt;
    }
    return found->second.image;
  }
  void Put(const Key& key, gfx::ImageSkia image, std::shared_ptr<FileCacheEpoch> epoch = {}, uint64_t revision = 0) {
    size_t bytes = 0;
    for (const auto& rep : image.image_reps())
      if (!rep.is_null()) bytes += rep.GetBitmap().computeByteSize();
    if (bytes > max_bytes_ || (epoch && !epoch->Matches(revision))) return;
    if (auto found = items_.Get(key); found != items_.end()) {
      bytes_ -= found->second.bytes;
      items_.Erase(found);
    }
    while (!items_.empty() && (items_.size() >= max_entries_ || bytes_ + bytes > max_bytes_)) {
      bytes_ -= items_.rbegin()->second.bytes;
      items_.Erase(items_.rbegin());
    }
    items_.Put(key, CachedSystemIcon{std::move(image), base::TimeTicks::Now(), std::move(epoch), revision, bytes});
    bytes_ += bytes;
  }
 private:
  base::LRUCache<Key, CachedSystemIcon> items_{base::LRUCache<Key, CachedSystemIcon>::NO_AUTO_EVICT};
  const size_t max_entries_, max_bytes_;
  size_t bytes_ = 0;
};
BoundedIconCache<SystemIconKey>& SystemIconCache() {
  static base::NoDestructor<BoundedIconCache<SystemIconKey>> cache(512, 32 * 1024 * 1024);
  return *cache;
}
using SharedIconKey = std::tuple<int, unsigned, int>;
BoundedIconCache<SharedIconKey>& SharedIconCache() {
  static base::NoDestructor<BoundedIconCache<SharedIconKey>> cache(128, 8 * 1024 * 1024);
  return *cache;
}

int UnknownSystemIconIndex() {
  thread_local int index = -1;
  if (index < 0) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(L"crFiles-unknown-type", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                      SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) index = info.iIcon;
  }
  return index;
}

SkBitmap ReadSystemIconBitmap(const base::FilePath& path, int index, unsigned overlay, HICON fallback, int pixels) {
  const bool trace = IsDriveIconPath(path);
  // Choose an image list by its actual pixel size, including Windows DPI scaling.
  base::win::ScopedGDIObject<HICON> smaller;
  for (int size : {SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO}) {
    Microsoft::WRL::ComPtr<IImageList> images = GetSystemImageList(size);
    if (!images) {
      if (trace) LOG(INFO) << "[crFiles /] image_list path=" << path.AsUTF8Unsafe() << " pixels=" << pixels
          << " list=" << size << " failed";
      continue;
    }
    int width = 0, height = 0;
    const HRESULT size_result = images->GetIconSize(&width, &height);
    if (FAILED(size_result)) {
      if (trace) LOG(INFO) << "[crFiles /] image_list_size path=" << path.AsUTF8Unsafe() << " pixels=" << pixels
          << " list=" << size << " hr=0x" << std::hex << static_cast<unsigned long>(size_result);
      continue;
    }
    if (size == SHIL_JUMBO) {
      // A legacy icon can occupy only a small corner of a 256px canvas.
      // Prefer a smaller complete icon rather than shrinking that padding.
      Microsoft::WRL::ComPtr<IImageList2> extended;
      int original_width = 0, original_height = 0;
      if (SUCCEEDED(images.As(&extended)) &&
          SUCCEEDED(extended->GetOriginalSize(index, ILGOS_ALWAYS, &original_width, &original_height)) &&
          (original_width < width || original_height < height)) {
        if (trace) LOG(INFO) << "[crFiles /] skip_padded_icon path=" << path.AsUTF8Unsafe() << " pixels=" << pixels
            << " list=" << size << " canvas=" << width << "x" << height
            << " original=" << original_width << "x" << original_height;
        continue;
      }
    }
    HICON handle = nullptr;
    const UINT flags = ILD_TRANSPARENT | INDEXTOOVERLAYMASK(overlay);
    const HRESULT result = images->GetIcon(index, flags, &handle);
    if (trace) LOG(INFO) << "[crFiles /] get_icon path=" << path.AsUTF8Unsafe() << " pixels=" << pixels
        << " list=" << size << " index=" << index << " overlay=" << overlay
        << " native=" << width << "x" << height << " handle=" << static_cast<const void*>(handle)
        << " hr=0x" << std::hex << static_cast<unsigned long>(result);
    base::win::ScopedGDIObject<HICON> icon(handle);
    if (FAILED(result) || !icon.is_valid()) continue;
    if (std::min(width, height) < pixels && size != SHIL_JUMBO) {
      smaller = std::move(icon);
      continue;
    }
    auto bitmap = IconUtil::CreateSkBitmapFromHICON(icon.get(), gfx::Size(pixels, pixels));
    if (trace) LOG(INFO) << "[crFiles /] convert_icon path=" << path.AsUTF8Unsafe() << " pixels=" << pixels
        << " list=" << size << " bitmap=" << bitmap.width() << "x" << bitmap.height()
        << " empty=" << bitmap.drawsNothing();
    if (!bitmap.drawsNothing()) return bitmap;
  }
  const HICON handle = smaller.is_valid() ? smaller.get() : fallback;
  auto bitmap = handle ? IconUtil::CreateSkBitmapFromHICON(handle, gfx::Size(pixels, pixels)) : SkBitmap();
  if (trace) LOG(INFO) << "[crFiles /] convert_fallback_handle path=" << path.AsUTF8Unsafe() << " pixels=" << pixels
      << " source=" << (smaller.is_valid() ? "smaller-image-list" : "shell-hicon")
      << " handle=" << static_cast<const void*>(handle) << " bitmap=" << bitmap.width() << "x" << bitmap.height()
      << " empty=" << bitmap.drawsNothing();
  return bitmap;
}

FileIconResult ReadSystemIcon(const base::FilePath& path, bool directory, int edge,
    const std::shared_ptr<FileCacheEpoch>& epoch, uint64_t revision) {
  const bool trace = IsDriveIconPath(path);
  SHFILEINFOW info{};
  // SHGetFileInfo does not promise to set last-error; log it only as a hint.
  if (trace) SetLastError(ERROR_SUCCESS);
  const auto result = SHGetFileInfoW(path.value().c_str(), 0, &info, sizeof(info),
      SHGFI_ICON | SHGFI_SMALLICON | SHGFI_SYSICONINDEX | SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX);
  const DWORD last_error_hint = GetLastError();
  if (trace) LOG(INFO) << "[crFiles /] shell_query path=" << path.AsUTF8Unsafe() << " edge=" << edge
      << " directory=" << directory << " thread=" << GetCurrentThreadId() << " result=" << result
      << " handle=" << static_cast<const void*>(info.hIcon) << " packed_index=" << info.iIcon
      << " last_error_hint=" << last_error_hint;
  base::win::ScopedGDIObject<HICON> fallback(info.hIcon);
  if (!result) return {{}, true};
  const unsigned icon_index = static_cast<unsigned>(info.iIcon);
  const int index = static_cast<int>(icon_index & 0x00ffffff);
  const unsigned overlay = icon_index >> 24;
  // Windows also succeeds for unregistered types with a generic blank file.
  // Such files should retain the project's MIME-specific fallback instead.
  if (!directory && !overlay && index == UnknownSystemIconIndex()) return {};
  const SharedIconKey shared_key{index, overlay, edge};
  {
    base::AutoLock guard(IconLock());
    if (auto cached = SharedIconCache().Get(shared_key)) return {*cached};
  }
  gfx::ImageSkia image;
  for (int scale : {1, 2}) {
    auto bitmap = ReadSystemIconBitmap(path, index, overlay, fallback.get(), edge * scale);
    if (bitmap.drawsNothing()) continue;
    bitmap.setImmutable();
    image.AddRepresentation(gfx::ImageSkiaRep(bitmap, static_cast<float>(scale)));
  }
  if (!image.isNull()) image.MakeThreadSafe();
  const bool retry = image.isNull();
  if (!retry) {
    base::AutoLock guard(IconLock());
    SharedIconCache().Put(shared_key, image, epoch, revision);
  }
  if (trace) LOG(INFO) << "[crFiles /] system_icon_result path=" << path.AsUTF8Unsafe() << " edge=" << edge
      << " index=" << index << " overlay=" << overlay << " image_empty=" << retry;
  return {std::move(image), retry};
}
}  // namespace
#endif

std::optional<gfx::ImageSkia> FindCachedFileIcon(const base::FilePath& path, const std::string& mime, bool directory, int edge) {
#if BUILDFLAG(IS_WIN)
  base::AutoLock guard(IconLock());
  return SystemIconCache().Get(SystemIconKey{path, directory, mime, edge});
#else
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
  if (directory && MayBeVolume(path)) return std::nullopt;
#endif
  return FindCachedBundledIcon(BundledFileIconAsset(path, mime, directory), edge);
#endif
}

FileIconResult LoadFileIcon(const base::FilePath& path, const std::string& mime, bool directory, int edge) {
#if BUILDFLAG(IS_WIN)
  if (std::ranges::find(kFileIconSizes, edge) == kFileIconSizes.end()) return {};
  if (auto cached = FindCachedFileIcon(path, mime, directory, edge)) {
    if (IsDriveIconPath(path)) LOG(INFO) << "[crFiles /] worker_cache_hit path=" << path.AsUTF8Unsafe()
        << " edge=" << edge << " image_empty=" << cached->isNull();
    return {*cached};
  }
  if (IsDriveIconPath(path)) LOG(INFO) << "[crFiles /] load_icon path=" << path.AsUTF8Unsafe()
      << " edge=" << edge << " directory=" << directory << " mime=" << mime << " cache=miss";
  // Shell queries and SVG rendering must never run under the shared cache lock.
  auto epoch = ObserveFileCachePath(path, false);
  const uint64_t revision = epoch->revision.load();
  auto result = ReadSystemIcon(path, directory, edge, epoch, revision);
  const bool bundled = result.image.isNull();
  if (bundled) {
    auto asset = BundledFileIconAsset(path, mime, directory);
    if (directory && IsDriveIconPath(path)) {
      const auto type = GetDriveTypeW(path.value().c_str());
      if (type == DRIVE_FIXED) asset = "drive-harddisk";
      if (type == DRIVE_REMOVABLE) asset = "drive-removable-media";
    }
    result.image = LoadBundledIcon(asset, edge);
  }
  if (IsDriveIconPath(path)) LOG(INFO) << "[crFiles /] load_icon_result path=" << path.AsUTF8Unsafe() << " edge=" << edge
      << " source=" << (bundled ? "bundled" : "system") << " retry=" << result.retry
      << " image_empty=" << result.image.isNull() << " cached=" << !result.retry;
  // A failed Shell query must not pin the fallback in the successful cache.
  // Unknown file types are a normal fallback and do not request retries.
  if (!result.retry) {
    base::AutoLock guard(IconLock());
    SystemIconCache().Put(SystemIconKey{path, directory, mime, edge},
                          result.image, std::move(epoch), revision);
  }
  return result;
#else
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_MAC)
  if (directory && MayBeVolume(path)) {
    auto asset = VolumeAsset(path);
    if (!asset.empty()) return {LoadBundledIcon(asset, edge)};
  }
#endif
  return {LoadBundledIcon(BundledFileIconAsset(path, mime, directory), edge)};
#endif
}
}
