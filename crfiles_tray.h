// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_CRFILES_TRAY_H_
#define CRFILES_CRFILES_TRAY_H_

#include <memory>
#include <optional>

#include "base/functional/callback.h"
#include "ui/gfx/geometry/rect.h"

namespace crfiles {
enum class TrayCommand { kToggle };
// UI-thread native status item. Destruction removes the icon and callbacks.
class CrFilesTray {
 public:
  virtual ~CrFilesTray() = default;
  virtual bool IsAvailable() const = 0;
  // Current visible icon bounds in Chromium screen DIPs, not native pixels.
  // Empty when the icon is hidden in overflow or its location is unavailable.
  virtual std::optional<gfx::Rect> GetIconBounds() const = 0;
};
// Returns nullptr if the native icon could not be installed.
std::unique_ptr<CrFilesTray> CreateCrFilesTray(
    base::RepeatingCallback<void(TrayCommand)> callback);
}  // namespace crfiles
#endif
