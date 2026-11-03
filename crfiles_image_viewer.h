// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRFILES_CRFILES_IMAGE_VIEWER_H_
#define CRFILES_CRFILES_IMAGE_VIEWER_H_

#include <string_view>
#include <vector>
#include "base/files/file_path.h"

namespace views { class Widget; }
namespace crfiles {
// UI-safe classification using the same image MIME category as thumbnails.
bool IsImageViewerFile(const base::FilePath& path, std::string_view mime = {});
// The playlist is a snapshot of the current ordering, restricted to one folder.
void ShowCrFilesImageViewer(views::Widget* parent,
                           std::vector<base::FilePath> paths,
                           const base::FilePath& selected);
}  // namespace crfiles
#endif
