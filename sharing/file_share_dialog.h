// Copyright 2026 The Chromium Authors
#ifndef CRFILES_SHARING_FILE_SHARE_DIALOG_H_
#define CRFILES_SHARING_FILE_SHARE_DIALOG_H_
#include <memory>
#include <vector>
#include <string>
#include "base/functional/callback.h"
#include "ui/gfx/geometry/rect.h"
#include "base/files/file_path.h"
#include "crFiles/sharing/nearby_sharing.h"
#include "crFiles/sharing/share_bubble_anchor.h"
namespace views { class Widget; class View; }
namespace crfiles {
class ShareSessionToast {
 public:
  virtual ~ShareSessionToast() = default;
};
std::unique_ptr<ShareSessionToast> CreateShareSessionToast(views::Widget* owner,
    std::shared_ptr<NearbySharing> service,
    base::RepeatingCallback<ShareBubbleAnchor(const NearbySharing::Snapshot&)> anchor);
std::optional<gfx::Rect> FileShareSessionBounds(views::View* view);
views::View* ShowTrustedDevicesDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service);
views::View* ShowSharingFoldersDialog(views::Widget* parent,
    base::RepeatingCallback<std::vector<NearbySharing::SharedTarget>()> folders,
    base::RepeatingCallback<void(base::FilePath)> open,
    base::RepeatingCallback<void(base::FilePath)> remove);
void SetFileShareSelection(views::View* view, std::vector<base::FilePath> paths);
views::View* ShowFileShareDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service,
                         std::vector<base::FilePath> paths);
}
#endif
