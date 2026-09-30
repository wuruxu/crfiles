// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/sharing/share_bubble_anchor.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace crfiles {
ShareBubbleAnchor TrayShareBubbleAnchor(std::optional<gfx::Rect> icon,
                                        const gfx::Rect& screen,
                                        const gfx::Rect& work_area,
                                        bool mac_menu_bar) {
  using Border = views::BubbleBorder;
  if (icon && !icon->IsEmpty() && screen.Intersects(*icon)) {
    const auto center = icon->CenterPoint();
    const std::array distances = {std::abs(center.y() - screen.y()),
                                  std::abs(center.y() - screen.bottom()),
                                  std::abs(center.x() - screen.x()),
                                  std::abs(center.x() - screen.right())};
    constexpr std::array arrows = {Border::TOP_CENTER, Border::BOTTOM_CENTER,
                                   Border::LEFT_CENTER, Border::RIGHT_CENTER};
    auto edge = std::min_element(distances.begin(), distances.end()) -
                distances.begin();
    // A corner menu-bar icon still points down. On Windows prefer the reserved
    // taskbar edge over the closest screen edge of a corner icon.
    if (mac_menu_bar || center.y() < work_area.y()) {
      edge = 0;
    } else if (center.y() >= work_area.bottom()) {
      edge = 1;
    } else if (center.x() < work_area.x()) {
      edge = 2;
    } else if (center.x() >= work_area.right()) {
      edge = 3;
    }
    return {*icon, arrows[edge], true, true};
  }
  // Prefer the edge occupied by the taskbar; autohide defaults to the bottom.
  const std::array insets = {
      work_area.y() - screen.y(), screen.bottom() - work_area.bottom(),
      work_area.x() - screen.x(), screen.right() - work_area.right()};
  size_t edge = std::max_element(insets.begin(), insets.end()) - insets.begin();
  if (mac_menu_bar) {
    edge = 0;
  } else if (insets[edge] <= 0) {
    edge = 1;
  }
  gfx::Point point(work_area.right() - 12, work_area.bottom() - 12);
  auto arrow = Border::BOTTOM_RIGHT;
  if (edge == 0) {
    point.set_y(work_area.y() + 12);
    arrow = Border::TOP_RIGHT;
  } else if (edge == 2) {
    point.set_x(work_area.x() + 12);
    arrow = Border::LEFT_BOTTOM;
  } else if (edge == 3) {
    arrow = Border::RIGHT_BOTTOM;
  }
  return {gfx::Rect(point, gfx::Size(1, 1)), arrow, true, false};
}
}  // namespace crfiles
