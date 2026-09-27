// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_THUMBNAIL_CACHE_H_
#define CRFILES_THUMBNAIL_CACHE_H_
#include <atomic>
#include <memory>
#include "base/files/file.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
// Worker-only persistent cache for folder thumbnails (up to 128 pixels).
// Hover previews remain in the bounded memory cache.
gfx::ImageSkia ReadDiskThumbnail(const base::FilePath& path, const base::File::Info& info,
    int edge, const std::shared_ptr<std::atomic_bool>& canceled);
void WriteDiskThumbnail(const base::FilePath& path, const base::File::Info& info,
    int edge, const gfx::ImageSkia& image, const std::shared_ptr<std::atomic_bool>& canceled);
}
#endif
