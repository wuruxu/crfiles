// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_FILE_ICONS_H_
#define CRFILES_FILE_ICONS_H_
#include <array>
#include <string>
#include <string_view>
#include <optional>
#include "base/files/file_path.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
inline constexpr std::array<int, 6> kFileIconSizes = {22, 32, 48, 64, 96, 128};
using FileIcons = std::array<gfx::ImageSkia, kFileIconSizes.size()>;
struct FileIconResult {
  gfx::ImageSkia image;
  // A transient Shell failure; keep the fallback visible while retrying.
  bool retry = false;
};
// Memory-only lookup, safe on the UI sequence. Nullopt means not loaded yet.
// Transient failures are not cached here.
std::optional<gfx::ImageSkia> FindCachedFileIcon(const base::FilePath& path, const std::string& mime, bool directory, int edge);
// Windows prefers the path's Shell icon, with bundled SVGs as a fallback.
// Call only on a file worker, in a COM STA on Windows. Cached across panes.
// The caller should retry transient failures with a bounded delay.
FileIconResult LoadFileIcon(const base::FilePath& path, const std::string& mime, bool directory, int edge);
// Bundled network device icon on every platform; call on a file worker.
gfx::ImageSkia LoadNetworkServerIcon(int edge);
// Bundled location and password prompt icons. Call on a file worker.
gfx::ImageSkia LoadNetworkWorkgroupIcon(int edge);
gfx::ImageSkia LoadTrashIcon(bool full, int edge);
gfx::ImageSkia LoadArchiveLockIcon();
// Type icons for virtual entries. Lookup is memory-only; load on a file worker.
// The returned asset name is a string literal, shared across equivalent file types.
std::string_view BundledFileIconAsset(const base::FilePath& path, std::string_view mime, bool directory);
std::optional<gfx::ImageSkia> FindCachedBundledIcon(std::string_view asset, int edge);
gfx::ImageSkia LoadBundledIcon(std::string_view asset, int edge);
// Remote names select bundled type icons without querying local paths/Shell.
gfx::ImageSkia LoadRemoteFileIcon(const std::string& name, bool directory, int edge);
}
#endif
