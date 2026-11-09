// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_SINGLE_BUTTON_ROW_LAYOUT_H_
#define CRFILES_SINGLE_BUTTON_ROW_LAYOUT_H_

#include "ui/views/layout/box_layout.h"

namespace crfiles {

// Keep multi-button rows unchanged; size a lone button to 18.2% of the
// available width, but never below its preferred width. Use the row's
// main-axis alignment.
class SingleButtonRowLayout : public views::BoxLayout {
 public:
  explicit SingleButtonRowLayout(const gfx::Insets& insets = {}, int spacing = 0);

 protected:
  views::ProposedLayout CalculateProposedLayout(
      const views::SizeBounds& size_bounds) const override;
};

}  // namespace crfiles

#endif  // CRFILES_SINGLE_BUTTON_ROW_LAYOUT_H_
