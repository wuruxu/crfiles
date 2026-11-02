// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRFILES_SHARING_SYNC_DIALOG_H_
#define CRFILES_SHARING_SYNC_DIALOG_H_
#include <memory>
#include <vector>
#include "base/files/file_path.h"
#include "base/functional/callback.h"
namespace views { class View; class Widget; }
namespace crfiles {
class NearbySharing;
views::View* ShowSyncDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service,
    std::vector<base::FilePath> paths, base::RepeatingCallback<void(base::FilePath)> open);
}  // namespace crfiles
#endif
