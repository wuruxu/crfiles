// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_FILE_ICONS_H_
#define CRFILES_FILE_ICONS_H_
#include <array>
#include <string>
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
}
#endif
