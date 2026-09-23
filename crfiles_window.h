// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_WINDOW_H_
#define CRFILES_WINDOW_H_
#include <memory>
#include "base/functional/callback.h"
namespace views { class View; class Widget; }
namespace crfiles {
void ShowCrFilesWindow(base::OnceClosure quit);
void DestroyCrFilesWindow();
std::unique_ptr<views::View> CreateCrFilesTabStrip(views::Widget* widget);
void ApplyCrFilesSharingSettings();
void ApplyCrFilesViewSettings();
bool IsCrFilesSharingBusy();
void ShowCrFilesHistory();
}
#endif
