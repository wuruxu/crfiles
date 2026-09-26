// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_IMAGE_PREVIEW_H_
#define CRFILES_IMAGE_PREVIEW_H_
#include <atomic>
#include <memory>
#include "base/files/file_path.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
// Images/SVG and PDF first pages, bounded by file size and pixel count.
// Blocking decode: run on a file worker. PDFium access is serialized.
gfx::ImageSkia LoadImagePreview(const base::FilePath& path, int maximum_edge);
// Worker-only shared LRU. Validates file metadata on every lookup. Retained
// thumbnails are bounded by 64 MiB and 1024 entries; large previews bypass it.
gfx::ImageSkia LoadCachedThumbnail(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
// Cooperative cancellation around IO and decode stages; an executing codec call
// finishes before cancellation can be observed. PDF progressive rendering also
// checks cancellation and a time budget; PDF loading/parsing cannot be interrupted.
gfx::ImageSkia LoadImagePreviewCancelable(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
}
#endif
