// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_dialog.h"

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/test/views_test_base.h"

namespace crfiles {
namespace {
using CrFilesDialogTest = views::ViewsTestBase;

TEST_F(CrFilesDialogTest, BubbleHitTestTracksVisibilityAndLifetime) {
  auto parent = CreateTestWidget(views::Widget::InitParams::CLIENT_OWNS_WIDGET);
  parent->Show();
  auto delegate = std::make_unique<views::BubbleDialogDelegate>(
      parent->GetContentsView(), views::BubbleBorder::TOP_LEFT);
  delegate->set_close_on_deactivate(false);
  auto bubble = CreateCrFilesBubble(delegate.get(), base::BindOnce(
      [](views::Widget::ClosedReason) {}));
  bubble->SetBounds(gfx::Rect(100, 100, 200, 120));
  bubble->Show();
  const auto bounds = bubble->GetWindowBoundsInScreen();
  const auto inside = bounds.CenterPoint();
  EXPECT_TRUE(IsInsideCrFilesBubble(inside));
  EXPECT_FALSE(IsInsideCrFilesBubble(gfx::Point(bounds.right() + 10, bounds.y())));
  bubble->Hide();
  EXPECT_FALSE(IsInsideCrFilesBubble(inside));
  bubble->Show();
  EXPECT_TRUE(IsInsideCrFilesBubble(inside));
  bubble->MakeCloseSynchronous({});
  bubble.reset();
  EXPECT_FALSE(IsInsideCrFilesBubble(inside));
}

TEST_F(CrFilesDialogTest, CloseKeepsWidgetAliveUntilCallbackUnwinds) {
  auto parent = CreateTestWidget(views::Widget::InitParams::CLIENT_OWNS_WIDGET);
  auto delegate = std::make_unique<views::BubbleDialogDelegate>(
      parent->GetContentsView(), views::BubbleBorder::TOP_LEFT);
  std::unique_ptr<views::Widget> bubble;
  bool closed = false;
  bubble = CreateCrFilesBubble(delegate.get(), base::BindOnce(
      [&](views::Widget::ClosedReason) {
        closed = true;
        bubble.reset();
      }));
  auto lifetime = bubble->GetWeakPtr();
  bubble->Close();
  EXPECT_TRUE(lifetime);
  EXPECT_FALSE(closed);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(closed);
  EXPECT_FALSE(lifetime);
}

TEST_F(CrFilesDialogTest, StaleCloseDoesNotCloseReplacement) {
  auto parent = CreateTestWidget(views::Widget::InitParams::CLIENT_OWNS_WIDGET);
  auto delegate = std::make_unique<views::BubbleDialogDelegate>(
      parent->GetContentsView(), views::BubbleBorder::TOP_LEFT);
  bool stale_callback_ran = false;
  auto bubble = CreateCrFilesBubble(delegate.get(), base::BindOnce(
      [&](views::Widget::ClosedReason) { stale_callback_ran = true; }));
  bubble->Close();
  bubble.reset();
  delegate.reset();
  delegate = std::make_unique<views::BubbleDialogDelegate>(
      parent->GetContentsView(), views::BubbleBorder::TOP_LEFT);
  bubble = CreateCrFilesBubble(delegate.get(), base::BindOnce(
      [](views::Widget::ClosedReason) {}));
  auto replacement = bubble->GetWeakPtr();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(stale_callback_ran);
  EXPECT_TRUE(replacement);
  bubble.reset();
}
}  // namespace
}  // namespace crfiles
