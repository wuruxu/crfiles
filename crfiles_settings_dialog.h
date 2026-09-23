// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_SETTINGS_DIALOG_H_
#define CRFILES_SETTINGS_DIALOG_H_
namespace views { class Widget; }
namespace crfiles {
void ShowCrFilesSettingsDialog(views::Widget* parent);
void ShowCrFilesAboutDialog(views::Widget* parent);
}
#endif
