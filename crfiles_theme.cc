// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/crfiles_pdf_reader.h"
#include <atomic>
#include <optional>
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "build/build_config.h"
#include "ui/native_theme/native_theme.h"
#include "ui/native_theme/os_settings_provider.h"
#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#endif
#if BUILDFLAG(IS_LINUX)
#include "ui/linux/linux_ui.h"
#include "ui/linux/linux_ui_factory.h"
#endif
namespace crfiles {
namespace {
base::FilePath SettingsPath() {
#if BUILDFLAG(IS_MAC)
  if (IsCrFilesSandboxed()) return MacApplicationSupportDirectory().AppendASCII("settings.json");
#endif
  base::FilePath path;
#if BUILDFLAG(IS_WIN)
  base::PathService::Get(base::DIR_LOCAL_APP_DATA, &path);
#else
  base::PathService::Get(base::DIR_HOME, &path);
  path = path.AppendASCII(".config");
#endif
  return path.AppendASCII("crFiles").AppendASCII("settings.json");
}
base::Lock& SettingsLock() { static base::NoDestructor<base::Lock> lock; return *lock; }
base::DictValue ReadSettings() {
  std::string json;
  base::ReadFileToStringWithMaxSize(SettingsPath(), &json, 1024 * 1024);
  return base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC).value_or(base::DictValue());
}
base::DictValue& Settings() {
  static base::NoDestructor<base::DictValue> settings(ReadSettings());
  return *settings;
}
// Serialize all writes, including explicit synchronous Settings changes, so
// an older workspace task can never overwrite a newer preferences snapshot.
base::Lock& SettingsWriteLock() { static base::NoDestructor<base::Lock> lock; return *lock; }
std::atomic<uint64_t> settings_version{0};
bool WriteSettings(const base::DictValue& settings) {
  auto json = base::WriteJson(settings);
  return json && base::CreateDirectory(SettingsPath().DirName()) &&
      base::ImportantFileWriter::WriteFileAtomically(SettingsPath(), *json);
}
bool Save() {
  base::AutoLock write_lock(SettingsWriteLock());
  if (!WriteSettings(Settings())) return false;
  ++settings_version;
  return true;
}
scoped_refptr<base::SequencedTaskRunner> WorkspaceWriter() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::USER_VISIBLE,
          base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
  return *runner;
}
bool WriteWorkspaceSnapshot(base::DictValue settings, uint64_t version) {
  base::AutoLock write_lock(SettingsWriteLock());
  if (settings_version.load() != version) return true;
  const bool saved = WriteSettings(settings);
  if (!saved) LOG(ERROR) << "Cannot save crFiles workspace";
  return saved || settings_version.load() != version;
}
void ApplyThemeMode(CrFilesThemeMode theme_mode, bool notify) {
  ui::NativeTheme* native_theme = ui::NativeTheme::GetInstanceForNativeUi();
  switch (theme_mode) {
    case CrFilesThemeMode::kLight:
      ui::NativeTheme::SetPreferredColorSchemeOverride(
          ui::NativeTheme::PreferredColorScheme::kLight);
      break;
    case CrFilesThemeMode::kDark:
      ui::NativeTheme::SetPreferredColorSchemeOverride(
          ui::NativeTheme::PreferredColorScheme::kDark);
      break;
    case CrFilesThemeMode::kSystem: {
      ui::NativeTheme::SetPreferredColorSchemeOverride(std::nullopt);
      ui::NativeTheme::PreferredColorScheme color_scheme =
          ui::OsSettingsProvider::Get().PreferredColorScheme();
#if BUILDFLAG(IS_LINUX)
      if (color_scheme ==
          ui::NativeTheme::PreferredColorScheme::kNoPreference) {
        ui::LinuxUiTheme* linux_theme = ui::GetDefaultLinuxUiTheme();
        if (linux_theme) {
          color_scheme =
              linux_theme->PreferDarkTheme()
                  ? ui::NativeTheme::PreferredColorScheme::kDark
                  : ui::NativeTheme::PreferredColorScheme::kLight;
        }
      }
#endif
      native_theme->set_preferred_color_scheme(color_scheme);
      break;
    }
  }
  if (notify) {
    native_theme->NotifyOnNativeThemeUpdated();
  }
}

}
CrFilesThemeMode GetCrFilesThemeMode() {
  base::AutoLock lock(SettingsLock());
  auto* theme = Settings().FindString("theme");
  return theme && *theme == "dark" ? CrFilesThemeMode::kDark :
      theme && *theme == "light" ? CrFilesThemeMode::kLight : CrFilesThemeMode::kSystem;
}
void InitializeCrFilesTheme() {
  ApplyThemeMode(GetCrFilesThemeMode(), true);
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT}, base::BindOnce(&PruneFileHistory));
}
void SetCrFilesThemeMode(CrFilesThemeMode mode) {
  {
    base::AutoLock lock(SettingsLock());
    Settings().Set("theme", mode == CrFilesThemeMode::kDark ? "dark" :
        mode == CrFilesThemeMode::kLight ? "light" : "system");
    Save();
  }
  ApplyThemeMode(mode, true);
  UpdateCrFilesPdfReaderTheme(mode);
}
std::string GetCrFilesLanguageSetting() {
  base::AutoLock lock(SettingsLock());
  auto* language = Settings().FindString("language");
  return language ? *language : std::string();
}
void SetCrFilesLanguageSetting(std::string language) {
  base::AutoLock lock(SettingsLock());
  Settings().Set("language", std::move(language));
  Save();
}
NearbySharingPreferences GetNearbySharingPreferences() {
  base::AutoLock lock(SettingsLock());
  NearbySharingPreferences p;
  if (auto* name = Settings().FindString("device_name")) p.name = *name;
  if (auto* path = Settings().FindString("receive_directory")) p.directory = *path;
  p.discovery_enabled = Settings().FindBool("nearby_enabled").value_or(true);
  p.avatar = Settings().FindInt("nearby_avatar").value_or(0);
  p.auto_accept_trusted = Settings().FindBool("nearby_auto_accept").value_or(false);
  if (const auto* avatar = Settings().FindString("nearby_custom_avatar")) p.custom_avatar = *avatar;
  return p;
}
bool SetNearbySharingPreferences(const NearbySharingPreferences& p) {
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("device_name", p.name);
  Settings().Set("receive_directory", p.directory);
  Settings().Set("nearby_enabled", p.discovery_enabled);
  Settings().Set("nearby_avatar", p.avatar);
  Settings().Set("nearby_auto_accept", p.auto_accept_trusted);
  Settings().Set("nearby_custom_avatar", p.custom_avatar);
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
QuickSharePreferences GetQuickSharePreferences() {
  base::AutoLock lock(SettingsLock());
  QuickSharePreferences p;
  if (const auto* adapter_name = Settings().FindString("quick_share_upgrade_interface")) p.upgrade_interface = *adapter_name;
  p.direct_enabled = Settings().FindBool("quick_share_direct_enabled").value_or(false);
  p.enabled = Settings().FindBool("quick_share_enabled").value_or(true);
  if (const auto* name = Settings().FindString("quick_share_name")) p.name = *name;
  if (const auto* directory = Settings().FindString("quick_share_directory")) p.directory = *directory;
  return p;
}
bool SetQuickSharePreferences(const QuickSharePreferences& p) {
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("quick_share_direct_enabled", p.direct_enabled);
  Settings().Set("quick_share_upgrade_interface", p.upgrade_interface);
  Settings().Set("quick_share_enabled", p.enabled);
  Settings().Set("quick_share_name", p.name);
  Settings().Set("quick_share_directory", p.directory);
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
NavigationStyle GetNavigationStyle() {
  base::AutoLock lock(SettingsLock());
  const int style = Settings().FindInt("navigation_style").value_or(0);
  return style >= 0 && style <= 3 ? static_cast<NavigationStyle>(style) : NavigationStyle::kMinimal;
}
bool SetNavigationStyle(NavigationStyle style) {
  const int value = static_cast<int>(style);
  if (value < 0 || value > 3) return false;
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("navigation_style", value);
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
SplitSelectionStyle GetSplitSelectionStyle() {
  base::AutoLock lock(SettingsLock());
  const int value = Settings().FindInt("split_selection_style").value_or(0);
  return value >= 0 && value <= 2 ? static_cast<SplitSelectionStyle>(value) :
      SplitSelectionStyle::kFlow;
}
bool SetSplitSelectionStyle(SplitSelectionStyle style) {
  const int value = static_cast<int>(style);
  if (value < 0 || value > 2) return false;
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("split_selection_style", value);
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
FileViewPreferences GetFileViewPreferences() {
  base::AutoLock lock(SettingsLock());
  return {Settings().FindBool("auto_hide_toolbar").value_or(true),
          Settings().FindBool("animate_folders").value_or(true),
          Settings().FindBool("hover_image_preview").value_or(false),
          Settings().FindBool("single_click_folders").value_or(false)};
}
bool GetWindowsTitlebarStyleEnabled() {
  base::AutoLock lock(SettingsLock());
  return Settings().FindBool("windows_titlebar_style").value_or(true);
}
bool GetSmbEnabledPreference() {
  base::AutoLock lock(SettingsLock());
  return Settings().FindBool("smb_enabled").value_or(false);
}
bool IsSmbEnabledForSession() {
  // The window reads this before opening Settings. Keep ongoing transfers on
  // the same policy for the lifetime of the process.
  static const bool enabled = GetSmbEnabledPreference();
  return enabled;
}
bool SetSmbEnabledPreference(bool enabled) {
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().FindBool("smb_enabled");
  Settings().Set("smb_enabled", enabled);
  if (Save()) return true;
  if (previous) Settings().Set("smb_enabled", *previous);
  else Settings().Remove("smb_enabled");
  return false;
}
bool GetSystemTrayEnabled() {
  base::AutoLock lock(SettingsLock());
  return Settings().FindBool("system_tray_enabled").value_or(true);
}
bool SetSystemTrayEnabled(bool enabled) {
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("system_tray_enabled", enabled);
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
bool SetWindowsTitlebarStyleEnabled(bool enabled) {
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("windows_titlebar_style", enabled);
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
bool SetFileViewPreferences(const FileViewPreferences& preferences) {
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("auto_hide_toolbar", preferences.auto_hide_toolbar);
  Settings().Set("animate_folders", preferences.animate_folders);
  Settings().Set("hover_image_preview", preferences.hover_image_preview);
  Settings().Set("single_click_folders", preferences.single_click_folders);
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
HistoryPreferences GetHistoryPreferences() {
  base::AutoLock lock(SettingsLock());
  HistoryPreferences preferences;
  if (const auto* history = Settings().FindDict("history")) {
    const int days = history->FindInt("days").value_or(30);
    for (int allowed : kHistoryRetentionDays) if (days == allowed) preferences.days = days;
    for (size_t i = 0; i < kHistoryActions.size(); ++i)
      preferences.record[i] = history->FindBool(kHistoryActions[i]).value_or(true);
  }
  return preferences;
}
bool SetHistoryPreferences(const HistoryPreferences& preferences) {
  bool valid = false;
  for (int days : kHistoryRetentionDays) valid |= preferences.days == days;
  if (!valid) return false;
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  base::DictValue history;
  history.Set("days", preferences.days);
  for (size_t i = 0; i < kHistoryActions.size(); ++i) history.Set(kHistoryActions[i], preferences.record[i]);
  Settings().Set("history", std::move(history));
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
base::DictValue GetCrFilesBookmarks() {
  base::AutoLock lock(SettingsLock());
  const auto* bookmarks = Settings().FindDict("bookmarks");
  return bookmarks ? bookmarks->Clone() : base::DictValue();
}
bool SetCrFilesBookmarks(base::DictValue bookmarks) {
  base::AutoLock lock(SettingsLock());
  auto previous = Settings().Clone();
  Settings().Set("bookmarks", std::move(bookmarks));
  if (Save()) return true;
  Settings() = std::move(previous); return false;
}
base::DictValue GetCrFilesWorkspace() {
  base::AutoLock lock(SettingsLock());
  const auto* workspace = Settings().FindDict("workspace");
  return workspace ? workspace->Clone() : base::DictValue();
}
bool SetCrFilesWorkspace(base::DictValue workspace, base::OnceCallback<void(bool)> reply) {
  base::AutoLock lock(SettingsLock());
  Settings().Set("workspace", std::move(workspace));
  const uint64_t version = ++settings_version;
  return WorkspaceWriter()->PostTaskAndReplyWithResult(FROM_HERE,
      base::BindOnce(&WriteWorkspaceSnapshot, Settings().Clone(), version), std::move(reply));
}

}
