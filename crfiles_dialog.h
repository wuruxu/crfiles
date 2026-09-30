// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_CRFILES_DIALOG_H_
#define CRFILES_CRFILES_DIALOG_H_

#include <memory>
#include "ui/views/widget/widget.h"

namespace views {
class DialogDelegate;
class BubbleDialogDelegate;
class Widget;
}

namespace crfiles {

// UI-thread-only hit test for visible bubbles, including nested popups.
bool IsInsideCrFilesBubble(const gfx::Point& screen_point);

// Defers client destruction until native activation/event callbacks unwind.
// A queued close is discarded if the original widget has been destroyed.
std::unique_ptr<views::Widget> CreateCrFilesBubble(
    views::BubbleDialogDelegate* delegate,
    views::Widget::ClosedCallback close);

// Owns the delegate and its widget until the dialog closes, including when
// the native window is closed by its parent.
void ShowCrFilesDialog(std::unique_ptr<views::DialogDelegate> delegate,
                       views::Widget* parent);

}  // namespace crfiles

#endif  // CRFILES_CRFILES_DIALOG_H_
