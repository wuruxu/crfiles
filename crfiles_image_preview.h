// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_IMAGE_PREVIEW_H_
#define CRFILES_IMAGE_PREVIEW_H_
#include <atomic>
#include <cstdint>
#include <memory>
#include "base/files/file_path.h"
#include "base/time/time.h"
#include "ui/gfx/image/image_skia.h"
namespace libcr { class SvgDecoder; }

namespace crfiles {
// Images/SVG/HEIC/HEIF, PDF first pages, and static video frames (up to 128 px).
// Video decode runs in a time-limited child; decoding calls are worker-only.
// Blocking decode: use MayBlock + WithBaseSyncPrimitives worker traits.
// Encoded inputs share a 128 MiB admission budget. Raster/HEIC pixels and
// PDF/SVG outputs share 320 MiB; larger HEIC requests run exclusively (up to
// 800 MiB estimated buffers). Codec-private scratch and retained caches are
// separate. PDFium/HEIC access is serialized; HEIC primary images allow 64 MP.
gfx::ImageSkia LoadImagePreview(const base::FilePath& path, int maximum_edge);
// UI-safe memory-only lookup using the directory's metadata snapshot. Performs
// no filesystem IO or decoding; mutation epochs reject invalidated entries.
gfx::ImageSkia FindMemoryThumbnail(const base::FilePath& path, int64_t size,
    base::Time modified, int maximum_edge);
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
// A viewer may retain the parsed scene to render subsequent frames on the same
// worker sequence. Other preview consumers only request the initial image.
gfx::ImageSkia LoadImagePreviewWithAnimation(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled,
    std::unique_ptr<libcr::SvgDecoder>* animation);
}
#endif
