// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_THEME_H_
#define CRFILES_THEME_H_
#include <string>
#include <array>
#include "base/values.h"
#include "base/functional/callback_forward.h"
#include "libcr/theme_mode.h"
namespace crfiles {
using CrFilesThemeMode = libcr::ThemeMode;
struct NearbySharingPreferences {
  std::string name = "crFiles";
  std::string directory;
  int avatar = 0;
  bool auto_accept_trusted = false;
  bool discovery_enabled = true;
  std::string custom_avatar;
};
struct QuickSharePreferences {
  bool direct_enabled = false;
  std::string upgrade_interface;  // Empty selects an available wireless interface.
  bool enabled = true;
  std::string name = "crFiles";
  std::string directory;
  bool operator==(const QuickSharePreferences&) const = default;
};
QuickSharePreferences GetQuickSharePreferences();
bool SetQuickSharePreferences(const QuickSharePreferences& preferences);
inline constexpr std::array<const char*, 6> kHistoryActions = {"move", "trash", "access", "open_file", "send", "receive"};
inline constexpr std::array<int, 4> kHistoryRetentionDays = {7, 30, 90, 365};
// Value 1 replaces the former capsule style, preserving saved selections.
enum class NavigationStyle { kMinimal = 0, kUnderline = 1, kRaised = 2, kConnected = 3 };
NavigationStyle GetNavigationStyle();
bool SetNavigationStyle(NavigationStyle style);
enum class SplitSelectionStyle { kFlow = 0, kTopBar = 1, kCorners = 2 };
SplitSelectionStyle GetSplitSelectionStyle();
bool SetSplitSelectionStyle(SplitSelectionStyle style);
// macOS titlebar preference, applied when the main window is created.
bool GetWindowsTitlebarStyleEnabled();
bool SetWindowsTitlebarStyleEnabled(bool enabled);
bool GetSmbEnabledPreference();
bool SetSmbEnabledPreference(bool enabled);
bool IsSmbEnabledForSession();
bool GetSystemTrayEnabled();
bool SetSystemTrayEnabled(bool enabled);
struct FileViewPreferences {
  bool auto_hide_toolbar = true;
  bool animate_folders = true;
  bool hover_image_preview = false;
  bool single_click_folders = false;
};
FileViewPreferences GetFileViewPreferences();
bool SetFileViewPreferences(const FileViewPreferences& preferences);
struct HistoryPreferences {
  int days = 30;
  std::array<bool, 6> record = {true, true, true, true, true, true};
};
HistoryPreferences GetHistoryPreferences();
bool SetHistoryPreferences(const HistoryPreferences& preferences);
base::DictValue GetCrFilesBookmarks();
bool SetCrFilesBookmarks(base::DictValue bookmarks);
base::DictValue GetCrFilesWorkspace();
// Returns whether the write was queued. Completion reports disk errors.
bool SetCrFilesWorkspace(base::DictValue workspace, base::OnceCallback<void(bool)> reply);
void InitializeCrFilesTheme();
CrFilesThemeMode GetCrFilesThemeMode();
void SetCrFilesThemeMode(CrFilesThemeMode mode);
std::string GetCrFilesLanguageSetting();
void SetCrFilesLanguageSetting(std::string language);
NearbySharingPreferences GetNearbySharingPreferences();
bool SetNearbySharingPreferences(const NearbySharingPreferences& preferences);
}
#endif
