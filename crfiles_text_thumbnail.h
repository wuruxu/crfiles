// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_TEXT_THUMBNAIL_H_
#define CRFILES_TEXT_THUMBNAIL_H_
#include <atomic>
#include <memory>
#include "base/files/file_path.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
bool IsTextThumbnailPath(const base::FilePath& path);
// Worker-only, plain text: at most 16 KiB read and the first five lines drawn.
// Callers must keep these images out of persistent thumbnail storage.
gfx::ImageSkia LoadTextThumbnail(const base::FilePath& path, int edge,
    const std::shared_ptr<std::atomic_bool>& canceled);
}
#endif
