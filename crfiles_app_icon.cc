// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_app_icon.h"

#include <string>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "libcr/svg_icon.h"

namespace crfiles {
namespace {

std::string LoadCrFilesIconSvg() {
  base::FilePath assets_dir;
  std::string svg;
  if (base::PathService::Get(base::DIR_ASSETS, &assets_dir)) {
    base::ReadFileToString(
        assets_dir.AppendASCII("crfiles_resources")
            .AppendASCII("crFiles.svg"),
        &svg);
  }
  return svg;
}

}  // namespace

ui::ImageModel CreateCrFilesIconModel(const gfx::Size& target_size) {
  static base::NoDestructor<libcr::SvgIconCache> cache(
      LoadCrFilesIconSvg(), gfx::SizeF(512.0f, 512.0f));
  return cache->GetImageModel(target_size);
}

ui::ImageModel CreateCrFilesAppIconModel() {
  return CreateCrFilesIconModel(gfx::Size(256, 256));
}

}  // namespace crfiles
