// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_IMAGE_PREVIEW_H_
#define CRFILES_IMAGE_PREVIEW_H_
#include <atomic>
#include <memory>
#include "base/files/file_path.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
// Images/SVG/HEIC/HEIF, PDF first pages, and static video frames (up to 128 px).
// Video decode runs in a time-limited child; all calls below are worker-only.
// Blocking decode: use MayBlock + WithBaseSyncPrimitives worker traits.
// Encoded inputs share a 128 MiB admission budget. Raster/HEIC pixels and
// PDF/SVG outputs share 320 MiB; larger HEIC requests run exclusively (up to
// 800 MiB estimated buffers). Codec-private scratch and retained caches are
// separate. PDFium/HEIC access is serialized; HEIC primary images allow 64 MP.
gfx::ImageSkia LoadImagePreview(const base::FilePath& path, int maximum_edge);
// Worker-only memory/disk lookup: validates metadata, never decodes the source
// file on a cache miss. Folder thumbnails also have a 256 MiB / 4096-item disk LRU.
gfx::ImageSkia FindCachedThumbnail(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
// Worker-only shared LRU. Validates file metadata on every lookup. Retained
// thumbnails/hover previews up to 720 px are bounded by 64 MiB and 1024 entries;
// larger previews bypass it.
gfx::ImageSkia LoadCachedThumbnail(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
// Cooperative cancellation around IO and decode stages; an executing codec call
// finishes before cancellation can be observed. PDF progressive rendering also
// checks cancellation and a time budget; PDF loading/parsing cannot be interrupted.
gfx::ImageSkia LoadImagePreviewCancelable(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
}
#endif
