// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_CRFILES_MAC_H_
#define CRFILES_CRFILES_MAC_H_

#include "crFiles/crfiles_context_actions.h"
#include "base/functional/callback_helpers.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/rect.h"

namespace views {
class NativeWidget;
class Widget;
namespace internal {
class NativeWidgetDelegate;
}
}  // namespace views

namespace crfiles {

// Allows the standalone executable to show and activate native windows.
void InitializeCrFilesApplicationMac();
void InstallCrFilesApplicationMenuMac(views::Widget* window);
void RemoveCrFilesApplicationMenuMac();
// Observe native titlebar presses without consuming AppKit's drag/zoom events.
base::ScopedClosureRunner ObserveTitlebarClicksMac(
    views::Widget* window, base::RepeatingClosure callback);

// Includes the native titlebar when converting content to outer window bounds.
gfx::Rect WindowBoundsForContentMac(views::Widget* widget,
                                    const gfx::Rect& content_bounds);
// Leading space for native window controls, including padding, in content DIPs.
int WindowControlsRightEdgeMac(views::Widget* widget);

// Clipboard helpers must be called on the UI thread.
std::vector<base::FilePath> ReadFileClipboardMac();
bool HasFileClipboardMac();
base::FilePath MediaDirectoryMac(bool pictures);
// Call on a worker: Launch Services may consult filesystem metadata.
bool IsFilePackageMac(const base::FilePath& path);
// Worker-only native icon lookup; returns an image safe to pass to the UI.
gfx::ImageSkia LoadApplicationIconMac(const base::FilePath& path, int edge);

std::vector<FileApplication> FileApplicationsMac(const base::FilePath& path);
// Called only inside the disposable video thumbnail helper process.
SkBitmap DecodeVideoThumbnailMac(const base::FilePath& path, int edge);

views::NativeWidget* CreateCrFilesNativeWidgetMac(
    views::internal::NativeWidgetDelegate* delegate);

}  // namespace crfiles

#endif  // CRFILES_CRFILES_MAC_H_
