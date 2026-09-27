// Copyright 2026 The Chromium Authors
#ifndef CRFILES_ARCHIVE_DIALOG_H_
#define CRFILES_ARCHIVE_DIALOG_H_
#include "base/functional/callback_forward.h"
#include "crFiles/crfiles_file_operations.h"
#include "ui/gfx/geometry/rect.h"
namespace views { class View; class BubbleDialogDelegate; }
namespace crfiles {
gfx::Rect GetArchiveBubbleBounds(views::BubbleDialogDelegate& bubble);
void ShowCompressDialog(views::View* anchor, std::vector<base::FilePath> sources,
    base::FilePath target, base::RepeatingCallback<gfx::Rect()> filename_bounds,
    base::OnceCallback<void(FileOperationResult)> completion);
void ShowArchiveDialog(views::View* anchor, base::FilePath archive,
    base::FilePath destination, bool same_name_folder,
    base::RepeatingCallback<gfx::Rect()> icon_bounds,
    base::OnceCallback<void(FileOperationResult)> completion);
}
#endif
