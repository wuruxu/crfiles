// Copyright 2026 The Chromium Authors
#ifndef CRFILES_SHARING_FILE_SHARE_DIALOG_H_
#define CRFILES_SHARING_FILE_SHARE_DIALOG_H_
#include <memory>
#include <vector>
#include "base/files/file_path.h"
namespace views { class Widget; }
namespace crfiles {
class NearbySharing;
void ShowFileShareDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service,
                         std::vector<base::FilePath> paths);
}
#endif
