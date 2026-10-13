// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_WINDOW_H_
#define CRFILES_WINDOW_H_
#include <memory>
#include "base/functional/callback.h"
namespace base { class FilePath; }
namespace views { class View; class Widget; }
namespace crfiles {
void ShowCrFilesWindow(base::OnceClosure quit);
void DestroyCrFilesWindow();
void RestoreCrFilesWindow();
void HideCrFilesWindow();
void QuitCrFiles();
std::unique_ptr<views::View> CreateCrFilesTabStrip(views::Widget* widget);
void ApplyCrFilesSharingSettings();
void ApplyCrFilesViewSettings();
void ApplyCrFilesTraySettings();
bool IsCrFilesSharingBusy();
bool IsCrFilesQuickShareBusy();
void ShowCrFilesReceivedDirectory(const base::FilePath& directory);
void ShowCrFilesHistory();
void ShowCrFilesTrustedDevices();
void ShowCrFilesSharingFolders();
}
#endif
