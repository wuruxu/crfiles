// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_SHARING_SHARE_BUBBLE_ANCHOR_H_
#define CRFILES_SHARING_SHARE_BUBBLE_ANCHOR_H_

#include <optional>

#include "base/memory/weak_ptr.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/bubble/bubble_border.h"

namespace views { class Widget; }
namespace crfiles {
struct ShareBubbleAnchor {
  gfx::Rect bounds;
  views::BubbleBorder::Arrow arrow = views::BubbleBorder::BOTTOM_RIGHT;
  bool background = false;
  bool show_arrow = true;
  base::WeakPtr<views::Widget> widget = {};
};
// All rectangles use Chromium screen DIPs, including negative display origins.
// When the icon is unavailable, use the work-area edge without a false arrow.
ShareBubbleAnchor TrayShareBubbleAnchor(std::optional<gfx::Rect> icon,
                                        const gfx::Rect& screen,
                                        const gfx::Rect& work_area,
                                        bool mac_menu_bar);
}  // namespace crfiles
#endif
