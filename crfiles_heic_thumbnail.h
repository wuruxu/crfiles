// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_CRFILES_HEIC_THUMBNAIL_H_
#define CRFILES_CRFILES_HEIC_THUMBNAIL_H_

#include <atomic>
#include <memory>
#include <string>
#include "base/files/file_path.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace crfiles {
bool IsHeicThumbnailPath(const base::FilePath& path);
// Worker-only decode of bounded HEIC/HEIF data. Returns orientation-correct,
// sRGB N32 premultiplied pixels fitted within maximum_edge, or an empty bitmap.
// macOS uses ImageIO; other platforms dynamically load libheif.
SkBitmap DecodeHeicThumbnail(const std::string& encoded, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
}  // namespace crfiles
#endif
