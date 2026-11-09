// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/single_button_row_layout.h"

#include <algorithm>
#include <cmath>

#include "ui/views/controls/button/button.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace crfiles {

SingleButtonRowLayout::SingleButtonRowLayout(const gfx::Insets& insets, int spacing)
    : views::BoxLayout(Orientation::kHorizontal, insets, spacing) {}

views::ProposedLayout SingleButtonRowLayout::CalculateProposedLayout(
    const views::SizeBounds& size_bounds) const {
  auto layout = views::BoxLayout::CalculateProposedLayout(size_bounds);
  if (!size_bounds.width().is_bounded()) return layout;
  views::ChildLayout* button = nullptr;
  for (auto& child : layout.child_layouts) {
    if (!child.visible || !views::IsViewClass<views::Button>(child.child_view)) continue;
    if (button) return layout;
    button = &child;
  }
  if (!button) return layout;
  // Without flex children, BoxLayout's host_size only spans the content.
  // Use the allocated width so the ratio does not depend on spacer views.
  auto available = gfx::Rect(size_bounds.width().value(), layout.host_size.height());
  available.Inset(host_view()->GetInsets());
  available.Inset(inside_border_insets());
  const int width = std::max(static_cast<int>(std::lround(available.width() * 0.182)),
      button->child_view->GetPreferredSize({}).width());
  int x = available.x();
  if (main_axis_alignment() == MainAxisAlignment::kCenter) {
    x += (available.width() - width) / 2;
  } else if (main_axis_alignment() == MainAxisAlignment::kEnd) {
    x = available.right() - width;
  }
  button->bounds.set_x(x);
  button->bounds.set_width(width);
  button->available_size.set_width(width);
  // Keep a status label or a leading spacer clear of the resized button.
  for (auto& child : layout.child_layouts) {
    if (&child == button || !child.visible || child.bounds.x() >= button->bounds.right()) continue;
    child.bounds.set_width(std::min(child.bounds.width(),
        std::max(0, button->bounds.x() - between_child_spacing() - child.bounds.x())));
    child.available_size.set_width(child.bounds.width());
  }
  return layout;
}

}  // namespace crfiles
