// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_icons.h"
#include <algorithm>
#include <map>
#include <string_view>
#include <utility>
#include "build/build_config.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"
#include "libcr/svg_icon.h"
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
// Fixed, licensed project assets. No theme discovery or directory enumeration.
std::string_view IconAsset(const std::string& mime, bool directory) {
  if (directory) return "inode-directory";
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
  return "application-x-generic";
}
gfx::ImageSkia ReadIcon(std::string_view asset, int edge) {
  base::FilePath assets;
  if (!base::PathService::Get(base::DIR_ASSETS, &assets)) return {};
  const auto path = assets.AppendASCII("crfiles_resources").AppendASCII("file_icons").AppendASCII(std::string(asset) + ".svg");
  std::string svg;
  if (!base::ReadFileToStringWithMaxSize(path, &svg, 1024 * 1024)) return {};
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
std::optional<gfx::ImageSkia> FindCachedBundledIcon(const std::string& mime, bool directory, int edge) {
  const IconKey key{std::string(IconAsset(mime, directory)), edge};
  base::AutoLock guard(IconLock());
  auto found = IconCache().find(key);
  if (found == IconCache().end()) return std::nullopt;
  return found->second;
}
gfx::ImageSkia LoadBundledIcon(const std::string& mime, bool directory, int edge) {
  if (std::ranges::find(kFileIconSizes, edge) == kFileIconSizes.end()) return {};
  if (auto cached = FindCachedBundledIcon(mime, directory, edge)) return *cached;
  const IconKey key{std::string(IconAsset(mime, directory)), edge};
  // SVG IO and rasterization never hold the shared cache lock.
  auto image = ReadIcon(key.first, edge);
  base::AutoLock guard(IconLock());
  return IconCache().try_emplace(key, std::move(image)).first->second;
}
}  // namespace

#if BUILDFLAG(IS_WIN)
namespace {
bool IsDriveIconPath(const base::FilePath& path) {
  const auto& value = path.value();
  return value.size() == 3 && value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}
using SystemIconKey = std::tuple<base::FilePath, bool, std::string, int>;
struct CachedSystemIcon {
  gfx::ImageSkia image;
  base::TimeTicks loaded_at;
};
base::LRUCache<SystemIconKey, CachedSystemIcon>& SystemIconCache() {
  // Path-specific icons cannot share the fixed MIME cache. Bound their memory
  // and expire results so refreshing can pick up changed file associations.
  static base::NoDestructor<base::LRUCache<SystemIconKey, CachedSystemIcon>> cache(128);
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
    Microsoft::WRL::ComPtr<IImageList> images;
    const HRESULT list_result = SHGetImageList(size, IID_PPV_ARGS(&images));
    if (FAILED(list_result)) {
      if (trace) LOG(INFO) << "[crFiles /] image_list path=" << path.AsUTF8Unsafe() << " pixels=" << pixels
          << " list=" << size << " hr=0x" << std::hex << static_cast<unsigned long>(list_result);
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

FileIconResult ReadSystemIcon(const base::FilePath& path, bool directory, int edge) {
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
  gfx::ImageSkia image;
  for (int scale : {1, 2}) {
    auto bitmap = ReadSystemIconBitmap(path, index, overlay, fallback.get(), edge * scale);
    if (bitmap.drawsNothing()) continue;
    bitmap.setImmutable();
    image.AddRepresentation(gfx::ImageSkiaRep(bitmap, static_cast<float>(scale)));
  }
  if (!image.isNull()) image.MakeThreadSafe();
  const bool retry = image.isNull();
  if (trace) LOG(INFO) << "[crFiles /] system_icon_result path=" << path.AsUTF8Unsafe() << " edge=" << edge
      << " index=" << index << " overlay=" << overlay << " image_empty=" << retry;
  return {std::move(image), retry};
}
}  // namespace
#endif

std::optional<gfx::ImageSkia> FindCachedFileIcon(const base::FilePath& path, const std::string& mime, bool directory, int edge) {
#if BUILDFLAG(IS_WIN)
  base::AutoLock guard(IconLock());
  auto& cache = SystemIconCache();
  auto found = cache.Get(SystemIconKey{path, directory, mime, edge});
  if (found == cache.end()) return std::nullopt;
  if (base::TimeTicks::Now() - found->second.loaded_at >= base::Seconds(30)) {
    cache.Erase(found);
    return std::nullopt;
  }
  return found->second.image;
#else
  (void)path;
  return FindCachedBundledIcon(mime, directory, edge);
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
  auto result = ReadSystemIcon(path, directory, edge);
  const bool bundled = result.image.isNull();
  if (bundled) result.image = LoadBundledIcon(mime, directory, edge);
  if (IsDriveIconPath(path)) LOG(INFO) << "[crFiles /] load_icon_result path=" << path.AsUTF8Unsafe() << " edge=" << edge
      << " source=" << (bundled ? "bundled-folder" : "system") << " retry=" << result.retry
      << " image_empty=" << result.image.isNull() << " cached=" << !result.retry;
  // A failed Shell query must not pin the fallback in the successful cache.
  // Unknown file types are a normal fallback and do not request retries.
  if (!result.retry) {
    base::AutoLock guard(IconLock());
    SystemIconCache().Put(SystemIconKey{path, directory, mime, edge},
                          CachedSystemIcon{result.image, base::TimeTicks::Now()});
  }
  return result;
#else
  (void)path;
  return {LoadBundledIcon(mime, directory, edge)};
#endif
}
}
