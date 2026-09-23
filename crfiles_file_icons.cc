// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_icons.h"
#include <algorithm>
#include <map>
#include <string_view>
#include <utility>
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"
#include "libcr/svg_icon.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/modules/svg/include/SkSVGDOM.h"
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
}
std::optional<gfx::ImageSkia> FindCachedFileIcon(const std::string& mime, bool directory, int edge) {
  const IconKey key{std::string(IconAsset(mime, directory)), edge};
  base::AutoLock guard(IconLock());
  auto found = IconCache().find(key);
  if (found == IconCache().end()) return std::nullopt;
  return found->second;
}
gfx::ImageSkia LoadFileIcon(const std::string& mime, bool directory, int edge) {
  if (std::ranges::find(kFileIconSizes, edge) == kFileIconSizes.end()) return {};
  if (auto cached = FindCachedFileIcon(mime, directory, edge)) return *cached;
  const IconKey key{std::string(IconAsset(mime, directory)), edge};
  // SVG IO and rasterization never hold the shared cache lock.
  auto image = ReadIcon(key.first, edge);
  base::AutoLock guard(IconLock());
  return IconCache().try_emplace(key, std::move(image)).first->second;
}
}
