// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_FILE_ICONS_H_
#define CRFILES_FILE_ICONS_H_
#include <array>
#include <string>
#include <optional>
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
inline constexpr std::array<int, 6> kFileIconSizes = {22, 32, 48, 64, 96, 128};
using FileIcons = std::array<gfx::ImageSkia, kFileIconSizes.size()>;
// Memory-only lookup, safe on the UI sequence. Nullopt means not loaded yet.
std::optional<gfx::ImageSkia> FindCachedFileIcon(const std::string& mime, bool directory, int edge);
// Blocking bundled SVG rasterization; one requested size, cached across panes.
// Call only on a file worker. Never scans system or project directories.
gfx::ImageSkia LoadFileIcon(const std::string& mime, bool directory, int edge);
}
#endif
