// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_context_actions.h"
#include "crFiles/crfiles_history.h"
#include "base/process/launch.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include <limits>
#include "build/build_config.h"
#if BUILDFLAG(IS_LINUX)
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#elif BUILDFLAG(IS_WIN)
#include <windows.h>
#include <shellapi.h>
#endif
namespace crfiles {
std::vector<FileApplication> FileApplications(const base::FilePath& path) {
  std::vector<FileApplication> apps;
#if BUILDFLAG(IS_LINUX)
  GFile* file = g_file_new_for_path(path.value().c_str());
  GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
  if (info) {
    const char* type = g_file_info_get_content_type(info);
    if (type) {
      GAppInfo* preferred = g_app_info_get_default_for_type(type, false);
      auto add = [&](GAppInfo* app, bool is_default) {
        const char* id = g_app_info_get_id(app);
        const char* name = g_app_info_get_display_name(app);
        if (id && name) apps.push_back({id, name, is_default});
      };
      if (preferred) add(preferred, true);
      GList* list = g_app_info_get_all_for_type(type);
      for (GList* node = list; node; node = node->next) {
        auto* app = G_APP_INFO(node->data);
        if (!preferred || !g_app_info_equal(preferred, app)) add(app, false);
      }
      g_list_free_full(list, g_object_unref);
      if (preferred) g_object_unref(preferred);
    }
    g_object_unref(info);
  }
  g_object_unref(file);
#endif
  return apps;
}
bool OpenFileWith(const base::FilePath& path, const std::string& application) {
  FileOperationLog log("open_file", "menu", path.AsUTF8Unsafe(), application);
  bool ok = false;
#if BUILDFLAG(IS_LINUX)
  if (auto* app = g_desktop_app_info_new(application.c_str())) {
    GFile* file = g_file_new_for_path(path.value().c_str());
    GList* files = g_list_append(nullptr, file);
    ok = g_app_info_launch(G_APP_INFO(app), files, nullptr, nullptr);
    g_list_free(files); g_object_unref(file); g_object_unref(app);
  }
#elif BUILDFLAG(IS_WIN)
  ok = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", path.value().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif BUILDFLAG(IS_MAC)
  ok = base::LaunchProcess(std::vector<std::string>{"/usr/bin/open", path.value()}, base::LaunchOptions()).IsValid();
#endif
  if (ok) log.Succeed();
  return ok;
}
std::optional<uint64_t> FileSelectionSize(std::vector<base::FilePath> paths) {
  uint64_t bytes = 0; size_t count = paths.size();
  // Bound expensive folder inspection and never follow symbolic links.
  if (count > 100000) return std::nullopt;
  while (!paths.empty()) {
    const auto path = paths.back(); paths.pop_back();
    base::File::Info info;
    if (base::IsLink(path) || !base::GetFileInfo(path, &info)) return std::nullopt;
    if (!info.is_directory) {
      if (info.size < 0 || static_cast<uint64_t>(info.size) > std::numeric_limits<uint64_t>::max() - bytes) return std::nullopt;
      bytes += static_cast<uint64_t>(info.size); continue;
    }
    int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
    types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
    base::FileEnumerator entries(path, false, types, base::FilePath::StringType(),
        base::FileEnumerator::FolderSearchPolicy::ALL, base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (auto child = entries.Next(); !child.empty(); child = entries.Next()) {
      if (++count > 100000) return std::nullopt;
      paths.push_back(child);
    }
    if (entries.GetError() != base::File::FILE_OK) return std::nullopt;
  }
  return bytes;
}
bool OpenFolderTerminal(const base::FilePath& path) {
  base::LaunchOptions options; options.current_directory = path;
#if BUILDFLAG(IS_WIN)
  return base::LaunchProcess(std::wstring(L"cmd.exe"), options).IsValid();
#elif BUILDFLAG(IS_MAC)
  return base::LaunchProcess(std::vector<std::string>{"/usr/bin/open", "-a", "Terminal", path.value()}, options).IsValid();
#else
  for (const char* command : {"xdg-terminal-exec", "x-terminal-emulator"})
    if (base::LaunchProcess(std::vector<std::string>{command}, options).IsValid()) return true;
  return false;
#endif
}
}
