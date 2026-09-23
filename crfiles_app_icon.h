// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_APP_ICON_H_
#define CRFILES_APP_ICON_H_

#include "ui/base/models/image_model.h"
#include "ui/gfx/geometry/size.h"

namespace crfiles {

ui::ImageModel CreateCrFilesIconModel(const gfx::Size& target_size);
ui::ImageModel CreateCrFilesAppIconModel();

}  // namespace crfiles

#endif  // CRFILES_APP_ICON_H_
