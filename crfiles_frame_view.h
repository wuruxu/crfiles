// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_CRFILES_FRAME_VIEW_H_
#define CRFILES_CRFILES_FRAME_VIEW_H_
#include <memory>
namespace views { class Widget; class FrameView; }
namespace crfiles {
inline constexpr int kCrFilesTitlebarHeight = 36;
std::unique_ptr<views::FrameView> CreateFilesFrameView(views::Widget* widget);
}
#endif
