// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_context_actions.h"
#include "crFiles/crfiles_history.h"
#include "base/process/launch.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/string_util.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include <limits>
#include <algorithm>
#include "build/build_config.h"
#if BUILDFLAG(IS_LINUX)
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#elif BUILDFLAG(IS_WIN)
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include "base/win/scoped_com_initializer.h"
#include "crFiles/grit/crfiles_strings.h"
#include "ui/base/l10n/l10n_util.h"
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
#elif BUILDFLAG(IS_WIN)
  base::win::ScopedCOMInitializer com;
  if (!com.Succeeded()) return apps;
  const auto extension = path.Extension();
  DWORD length = 0;
  AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, extension.c_str(), L"open", nullptr, &length);
  if (length && length < 32768) {
    std::wstring name(length, L'\0');
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME, extension.c_str(), L"open", name.data(), &length))) {
      name.resize(length ? length - 1 : 0);
      apps.push_back({{}, base::WideToUTF8(name), true});
    }
  }
  Microsoft::WRL::ComPtr<IEnumAssocHandlers> handlers;
  if (SUCCEEDED(SHAssocEnumHandlers(extension.c_str(), ASSOC_FILTER_NONE, &handlers))) {
    Microsoft::WRL::ComPtr<IAssocHandler> handler;
    while (handlers->Next(1, &handler, nullptr) == S_OK) {
      PWSTR id = nullptr, name = nullptr;
      if (SUCCEEDED(handler->GetName(&id)) && SUCCEEDED(handler->GetUIName(&name)) && id && name)
        apps.push_back({base::WideToUTF8(id), base::WideToUTF8(name), false});
      CoTaskMemFree(id); CoTaskMemFree(name); handler.Reset();
    }
  }
  apps.push_back({"windows:choose", base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_CRFILES_OPEN_WITH)) + "…", false});
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
  base::win::ScopedCOMInitializer com;
  if (com.Succeeded()) {
    if (application.empty()) {
      ok = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", path.value().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    } else if (application == "windows:choose") {
      OPENASINFO info{};
      info.pcszFile = path.value().c_str();
      info.oaifInFlags = OAIF_EXEC;
      ok = SUCCEEDED(SHOpenWithDialog(nullptr, &info));
    } else {
      Microsoft::WRL::ComPtr<IEnumAssocHandlers> handlers;
      Microsoft::WRL::ComPtr<IShellItem> item;
      Microsoft::WRL::ComPtr<IDataObject> data;
      if (SUCCEEDED(SHCreateItemFromParsingName(path.value().c_str(), nullptr, IID_PPV_ARGS(&item))) &&
          SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) &&
          SUCCEEDED(SHAssocEnumHandlers(path.Extension().c_str(), ASSOC_FILTER_NONE, &handlers))) {
        Microsoft::WRL::ComPtr<IAssocHandler> handler;
        while (handlers->Next(1, &handler, nullptr) == S_OK) {
          PWSTR id = nullptr;
          const bool match = SUCCEEDED(handler->GetName(&id)) && id && base::WideToUTF8(id) == application;
          CoTaskMemFree(id);
          if (match) { ok = SUCCEEDED(handler->Invoke(data.Get())); break; }
          handler.Reset();
        }
      }
    }
  }
#elif BUILDFLAG(IS_MAC)
  ok = base::LaunchProcess(std::vector<std::string>{"/usr/bin/open", path.value()}, base::LaunchOptions()).IsValid();
#endif
  if (ok) log.Succeed();
  return ok;
}
bool IsValidEntryName(std::u16string_view name) {
  if (name.empty() || name == u"." || name == u".." ||
      name.find_first_of(u"/\\") != std::u16string_view::npos || name.find(char16_t(0)) != std::u16string_view::npos) return false;
  const auto path = base::FilePath::FromUTF16Unsafe(name);
  if (path.IsAbsolute() || path.ReferencesParent() || path.BaseName() != path) return false;
#if BUILDFLAG(IS_WIN)
  if (name.size() > 255 || name.back() == u'.' || name.back() == u' ' || name.find_first_of(u"<>:\"|?*") != std::u16string_view::npos ||
      std::ranges::any_of(name, [](char16_t c) { return c < 32; })) return false;
  auto stem = base::ToLowerASCII(std::u16string(name.substr(0, name.find(u'.'))));
  while (!stem.empty() && stem.back() == u' ') stem.pop_back();
  if (stem == u"con" || stem == u"prn" || stem == u"aux" || stem == u"nul" || stem == u"clock$" ||
      stem == u"conin$" || stem == u"conout$") return false;
  if (stem.size() == 4 && (stem.starts_with(u"com") || stem.starts_with(u"lpt")) &&
      ((stem.back() >= u'1' && stem.back() <= u'9') || stem.back() == u'¹' || stem.back() == u'²' || stem.back() == u'³')) return false;
#endif
  return true;
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
