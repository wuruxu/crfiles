// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_tray.h"

#import <Cocoa/Cocoa.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "crFiles/crfiles_app_icon.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/image/image_skia_util_mac.h"
#include "ui/gfx/mac/coordinate_conversion.h"

@interface CrFilesTrayTarget : NSObject
- (instancetype)initWithCallback:
    (base::RepeatingCallback<void(crfiles::TrayCommand)>)callback;
- (void)click:(NSStatusBarButton*)button;
@end

@implementation CrFilesTrayTarget {
  base::RepeatingCallback<void(crfiles::TrayCommand)> _callback;
}
- (instancetype)initWithCallback:
    (base::RepeatingCallback<void(crfiles::TrayCommand)>)callback {
  if ((self = [super init])) {
    _callback = std::move(callback);
  }
  return self;
}
- (void)click:(NSStatusBarButton*)button {
  NSEvent* event = NSApp.currentEvent;
  if (event.type == NSEventTypeRightMouseUp) {
    return;
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(_callback, crfiles::TrayCommand::kToggle));
}
@end

namespace crfiles {
namespace {
class CrFilesTrayMac : public CrFilesTray {
 public:
  explicit CrFilesTrayMac(base::RepeatingCallback<void(TrayCommand)> callback) {
    target_ = [[CrFilesTrayTarget alloc] initWithCallback:std::move(callback)];
    item_ = [NSStatusBar.systemStatusBar
        statusItemWithLength:NSSquareStatusItemLength];
    if (!item_) {
      return;
    }
    const auto image =
        CreateCrFilesIconModel(gfx::Size(36, 36)).Rasterize(nullptr);
    NSImage* icon = image.isNull() ? nil : gfx::NSImageFromImageSkia(image);
    if (!icon) {
      [NSStatusBar.systemStatusBar removeStatusItem:item_];
      item_ = nil;
      return;
    }
    icon.size = NSMakeSize(18, 18);
    item_.button.image = icon;
    item_.button.toolTip = @"crFiles";
    item_.button.target = target_;
    item_.button.action = @selector(click:);
    [item_.button sendActionOn:NSEventMaskLeftMouseUp];
  }
  ~CrFilesTrayMac() override {
    if (item_) {
      item_.button.target = nil;
      item_.button.action = nil;
      [NSStatusBar.systemStatusBar removeStatusItem:item_];
    }
  }
  bool IsAvailable() const override { return item_ != nil; }
  std::optional<gfx::Rect> GetIconBounds() const override {
    NSStatusBarButton* button = item_.button;
    if (!button || !button.window || !button.window.isVisible ||
        button.isHidden) {
      return std::nullopt;
    }
    const NSRect in_window = [button convertRect:button.bounds toView:nil];
    const auto bounds = gfx::ScreenRectFromNSRect(
        [button.window convertRectToScreen:in_window]);
    if (bounds.IsEmpty()) {
      return std::nullopt;
    }
    return bounds;
  }

 private:
  NSStatusItem* __strong item_ = nil;
  CrFilesTrayTarget* __strong target_ = nil;
};
}  // namespace
std::unique_ptr<CrFilesTray> CreateCrFilesTray(
    base::RepeatingCallback<void(TrayCommand)> callback) {
  auto tray = std::make_unique<CrFilesTrayMac>(std::move(callback));
  if (!tray->IsAvailable()) {
    return nullptr;
  }
  return tray;
}
}  // namespace crfiles
