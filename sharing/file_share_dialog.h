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
#include "ui/base/models/image_model.h"
namespace views { class Widget; class View; }
namespace crfiles {
ui::ImageModel FileShareDeviceAvatar(int avatar, int edge = 64);
std::unique_ptr<views::View> CreateFileShareDeviceButton(base::RepeatingClosure callback,
    int avatar, const std::u16string& name);
// Updates a button returned by CreateFileShareDeviceButton using the same ring
// and cancel interaction as Share to.
void SetFileShareDeviceTransfer(views::View* button, bool active, bool indeterminate,
    double progress, base::RepeatingClosure cancel = {}, std::u16string detail = {});
class ShareSessionToast {
 public:
  virtual ~ShareSessionToast() = default;
};
std::unique_ptr<ShareSessionToast> CreateShareSessionToast(views::Widget* owner,
    std::shared_ptr<ShareService> service,
    base::RepeatingCallback<ShareBubbleAnchor(const NearbySharing::Snapshot&)> anchor);
std::optional<gfx::Rect> FileShareSessionBounds(views::View* view);
views::View* ShowTrustedDevicesDialog(views::Widget* parent, std::shared_ptr<ShareService> service);
views::View* ShowSharingFoldersDialog(views::Widget* parent,
    base::RepeatingCallback<std::vector<NearbySharing::SharedTarget>()> folders,
    base::RepeatingCallback<void(NearbySharing::SharedTarget)> open,
    base::RepeatingCallback<void(NearbySharing::SharedTarget)> remove);
void SetFileShareSelection(views::View* view, std::vector<base::FilePath> paths);
views::View* ShowFileShareDialog(views::Widget* parent, std::shared_ptr<ShareService> service,
                         std::vector<base::FilePath> paths);
}
#endif
