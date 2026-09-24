// Copyright 2026 The Chromium Authors
#ifndef CRFILES_SHARING_FILE_SHARE_DIALOG_H_
#define CRFILES_SHARING_FILE_SHARE_DIALOG_H_
#include <memory>
#include <vector>
#include <string>
#include "base/functional/callback.h"
#include "ui/gfx/geometry/rect.h"
#include "base/files/file_path.h"
namespace views { class Widget; class View; }
namespace crfiles {
class NearbySharing;
class IncomingShareToast {
 public:
  virtual ~IncomingShareToast() = default;
};
std::unique_ptr<IncomingShareToast> CreateIncomingShareToast(views::Widget* parent,
    std::shared_ptr<NearbySharing> service, base::RepeatingCallback<gfx::Rect(const std::string&)> bounds);
views::View* ShowTrustedDevicesDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service);
void SetFileShareSelection(views::View* view, std::vector<base::FilePath> paths);
views::View* ShowFileShareDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service,
                         std::vector<base::FilePath> paths);
}
#endif
