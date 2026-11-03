// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_dialog.h"

#include <utility>
#include <algorithm>
#include <vector>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"

namespace crfiles {
namespace {

std::vector<base::WeakPtr<views::Widget>>& Bubbles() {
  static base::NoDestructor<std::vector<base::WeakPtr<views::Widget>>> bubbles;
  return *bubbles;
}

struct DialogRecord {
  base::WeakPtr<views::Widget> widget;
  base::WeakPtr<views::Widget> parent;
};

std::vector<DialogRecord>& Dialogs() {
  static base::NoDestructor<std::vector<DialogRecord>> dialogs;
  return *dialogs;
}

void PruneBubbles() {
  std::erase_if(Bubbles(), [](const auto& widget) {
    // IsClosed() can become true before asynchronous native teardown ends.
    // Keep such widgets registered so shutdown can finish closing them.
    return !widget;
  });
}

class DialogOwner {
 public:
  DialogOwner(std::unique_ptr<views::DialogDelegate> delegate,
              views::Widget* parent)
      : delegate_(std::move(delegate)) {
    delegate_->SetOwnershipOfNewWidget(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET);
    widget_.reset(views::DialogDelegate::CreateDialogWidget(
        delegate_.get(), parent->GetNativeWindow(), parent->GetNativeView()));
  }

  ~DialogOwner() {
    // Callbacks can retain raw_ptrs to controls owned by the widget.
    delegate_->SetAcceptCallback({});
    delegate_->SetCancelCallback({});
    delegate_->SetCloseCallback({});
    widget_.reset();
    // The delegate must outlive the widget's teardown notifications.
  }

  views::Widget* widget() { return widget_.get(); }

 private:
  std::unique_ptr<views::DialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
};

}  // namespace

bool IsInsideCrFilesBubble(const gfx::Point& screen_point) {
  PruneBubbles();
  return std::ranges::any_of(Bubbles(), [&](const auto& widget) {
    return !widget->IsClosed() && widget->IsVisible() &&
           widget->GetWindowBoundsInScreen().Contains(screen_point);
  });
}

bool IsCrFilesWindowBlockedByModal(views::Widget* window) {
  if (!window || window->IsClosed()) return false;
  // AppKit sheets need not appear in the same native ownership traversal as
  // ordinary child windows. Keep the explicit parent passed at creation.
  for (const auto& dialog : Dialogs()) {
    if (dialog.parent.get() == window && dialog.widget &&
        !dialog.widget->IsClosed() && dialog.widget->IsVisible() &&
        dialog.widget->widget_delegate()->GetModalType() == ui::mojom::ModalType::kWindow)
      return true;
  }
  for (views::Widget* child : views::Widget::GetAllOwnedWidgets(window->GetNativeView())) {
    if (child != window && !child->IsClosed() && child->IsVisible() &&
        child->widget_delegate() &&
        child->widget_delegate()->GetModalType() == ui::mojom::ModalType::kWindow)
      return true;
  }
  return false;
}

void CloseCrFilesDialogs() {
  // Closing a window can destroy other windows through their owners. Use weak
  // pointers and a snapshot so callbacks cannot invalidate this traversal.
  auto windows = std::move(Bubbles());
  Bubbles().clear();
  auto dialogs = std::move(Dialogs());
  Dialogs().clear();
  for (const auto& dialog : dialogs) windows.push_back(dialog.widget);
  for (const auto& widget : windows) {
    // Close() may already have marked it closed while native teardown is still
    // queued. CloseNow() must finish that teardown as well.
    if (widget) widget->CloseNow();
  }
}

std::unique_ptr<views::Widget> CreateCrFilesBubble(
    views::BubbleDialogDelegate* delegate,
    views::Widget::ClosedCallback close) {
  auto lifetime = std::make_shared<base::WeakPtr<views::Widget>>();
  auto widget = views::BubbleDialogDelegate::CreateBubble(
      delegate, base::BindOnce(
          [](std::shared_ptr<base::WeakPtr<views::Widget>> lifetime,
             views::Widget::ClosedCallback close,
             views::Widget::ClosedReason reason) {
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(
                    [](base::WeakPtr<views::Widget> widget,
                       views::Widget::ClosedCallback close,
                       views::Widget::ClosedReason reason) {
                      if (widget) std::move(close).Run(reason);
                    }, *lifetime, std::move(close), reason));
          }, lifetime, std::move(close)));
  *lifetime = widget->GetWeakPtr();
  PruneBubbles();
  Bubbles().push_back(*lifetime);
  return widget;
}

void ShowCrFilesDialog(std::unique_ptr<views::DialogDelegate> delegate,
                       views::Widget* parent,
                       const gfx::Rect& initial_bounds) {
  auto owner = std::make_unique<DialogOwner>(std::move(delegate), parent);
  auto* widget = owner->widget();
  if (!initial_bounds.IsEmpty()) {
    const auto minimum = widget->GetMinimumSize();
    const gfx::Size size(std::max(initial_bounds.width(), minimum.width()),
                         std::max(initial_bounds.height(), minimum.height()));
    const auto center = initial_bounds.CenterPoint();
    widget->SetBoundsConstrained(gfx::Rect(center.x() - size.width() / 2,
        center.y() - size.height() / 2, size.width(), size.height()));
  }
  std::erase_if(Dialogs(), [](const auto& dialog) { return !dialog.widget; });
  Dialogs().push_back({widget->GetWeakPtr(), parent->GetWeakPtr()});
  // Close moves this callback out of the widget before invoking it. Releasing
  // its owner then destroys the widget first and the delegate second. Native
  // parent-window teardown also invokes this callback.
  widget->MakeCloseSynchronous(base::BindOnce(
      [](std::unique_ptr<DialogOwner> owner, views::Widget::ClosedReason) {
        owner.reset();
      },
      std::move(owner)));
  widget->Show();
}

}  // namespace crfiles
