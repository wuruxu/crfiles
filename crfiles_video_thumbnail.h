// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_VIDEO_THUMBNAIL_H_
#define CRFILES_VIDEO_THUMBNAIL_H_
#include <atomic>
#include <memory>
#include "base/files/file_path.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
inline constexpr char kVideoThumbnailProcessSwitch[] = "crfiles-video-thumbnail";
bool IsVideoThumbnailPath(const base::FilePath& path);
// Worker only (MayBlock + WithBaseSyncPrimitives). One reusable Mojo child;
// cancellation or an eight-second timeout terminates it. Idle lifetime: 30 s.
gfx::ImageSkia LoadVideoThumbnail(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
int RunVideoThumbnailProcess();
}
#endif
