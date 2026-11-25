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
#include <set>
#include "base/containers/span.h"
#include "base/strings/string_split.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/gfx/geometry/size.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "base/time/time.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include "crFiles/crfiles_mac.h"
#endif
#if BUILDFLAG(IS_LINUX)
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include "libcr/svg_icon.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/modules/svg/include/SkSVGDOM.h"
#include "ui/gfx/codec/png_codec.h"
#elif BUILDFLAG(IS_WIN)
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include "base/win/scoped_com_initializer.h"
#include "crFiles/grit/crfiles_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/win/icon_util.h"
#endif
namespace crfiles {
namespace {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
gfx::ImageSkia MenuIcon(gfx::ImageSkia image) {
  if (image.isNull()) return {};
  auto resized = gfx::ImageSkiaOperations::CreateResizedImage(image, skia::ImageOperations::RESIZE_BEST, gfx::Size(18, 18));
  resized.MakeThreadSafe();
  return resized;
}
#endif
#if BUILDFLAG(IS_LINUX)
std::string KeyString(GKeyFile* file, const char* section, const char* key) {
  gchar* value = g_key_file_get_string(file, section, key, nullptr);
  std::string result = value ? value : "";
  g_free(value); return result;
}
std::vector<base::FilePath> ApplicationIconDirectories() {
  std::vector<base::FilePath> roots = {
      base::FilePath(g_get_home_dir()).AppendASCII(".icons"),
      base::FilePath(g_get_user_data_dir()).AppendASCII("icons")};
  const char* data_dirs = g_getenv("XDG_DATA_DIRS");
  for (const auto& directory : base::SplitString(data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share",
      ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) roots.push_back(base::FilePath(directory).AppendASCII("icons"));
  std::vector<std::string> themes;
  if (auto* source = g_settings_schema_source_get_default()) {
    if (auto* schema = g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", true)) {
      if (g_settings_schema_has_key(schema, "icon-theme")) {
        auto* settings = g_settings_new_full(schema, nullptr, nullptr);
        gchar* theme = g_settings_get_string(settings, "icon-theme");
        if (theme && *theme) themes.emplace_back(theme);
        g_free(theme); g_object_unref(settings);
      }
      g_settings_schema_unref(schema);
    }
  }
  themes.emplace_back("hicolor");
  std::set<std::string> visited;
  std::vector<base::FilePath> directories;
  for (size_t i = 0; i < themes.size() && visited.size() < 16; ++i) {
    const auto theme = themes[i];
    if (!visited.insert(theme).second || !IsValidEntryName(base::UTF8ToUTF16(theme))) continue;
    std::set<base::FilePath> relative_directories;
    for (const auto& root : roots) {
      const auto theme_root = root.Append(theme);
      GKeyFile* index = g_key_file_new();
      if (g_key_file_load_from_file(index, theme_root.AppendASCII("index.theme").value().c_str(), G_KEY_FILE_NONE, nullptr)) {
        for (const char* key : {"Directories", "ScaledDirectories"}) {
          for (const auto& directory : base::SplitString(KeyString(index, "Icon Theme", key), ";,",
              base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
            base::FilePath relative(directory);
            if (!relative.IsAbsolute() && !relative.ReferencesParent()) relative_directories.insert(relative);
          }
        }
        for (auto& parent : base::SplitString(KeyString(index, "Icon Theme", "Inherits"), ";,",
            base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
          if (themes.size() < 64) themes.push_back(std::move(parent));
        }
      }
      g_key_file_unref(index);
    }
    // A user icon directory can inherit index.theme from a system directory.
    for (const auto& root : roots)
      for (const auto& relative : relative_directories) directories.push_back(root.Append(theme).Append(relative));
  }
  directories.insert(directories.end(), roots.begin(), roots.end());
  directories.emplace_back("/usr/local/share/pixmaps");
  directories.emplace_back("/usr/share/pixmaps");
  return directories;
}
gfx::ImageSkia ReadApplicationIcon(const base::FilePath& path) {
  std::string data;
  if (!base::ReadFileToStringWithMaxSize(path, &data, 4 * 1024 * 1024)) return {};
  if (path.MatchesExtension(FILE_PATH_LITERAL(".svg"))) {
    SkMemoryStream stream(data.data(), data.size(), true);
    auto dom = SkSVGDOM::MakeFromStream(stream);
    if (!dom) return {};
    auto size = dom->containerSize(); if (size.isEmpty()) size = SkSize::Make(48, 48);
    libcr::SvgIconCache renderer(std::move(data), gfx::SizeF(size.width(), size.height()));
    auto image = renderer.GetImageModel(gfx::Size(18, 18));
    return image.IsImage() ? MenuIcon(image.GetImage().AsImageSkia()) : gfx::ImageSkia();
  }
  auto bitmap = gfx::PNGCodec::Decode(base::as_byte_span(data));
  return bitmap.drawsNothing() ? gfx::ImageSkia() : MenuIcon(gfx::ImageSkia::CreateFrom1xBitmap(bitmap));
}
gfx::ImageSkia ApplicationIcon(GAppInfo* app, const std::vector<base::FilePath>& directories) {
  if (!G_IS_DESKTOP_APP_INFO(app)) return {};
  gchar* value = g_desktop_app_info_get_string(G_DESKTOP_APP_INFO(app), "Icon");
  const std::string name = value ? value : "";
  g_free(value);
  if (name.empty()) return {};
  const base::FilePath path(name);
  if (path.IsAbsolute()) return ReadApplicationIcon(path);
  if (path.BaseName() != path || path.ReferencesParent()) return {};
  for (const auto& directory : directories) {
    for (const char* extension : {"", ".svg", ".png"}) {
      const auto candidate = directory.Append(name + extension);
      if (base::PathExists(candidate)) {
        auto icon = ReadApplicationIcon(candidate);
        if (!icon.isNull()) return icon;
      }
    }
  }
  return {};
}
#elif BUILDFLAG(IS_WIN)
gfx::ImageSkia ApplicationIcon(IAssocHandler* handler) {
  PWSTR location = nullptr;
  int index = 0;
  const auto result = handler->GetIconLocation(&location, &index);
  HICON icon = nullptr;
  if (SUCCEEDED(result) && location) SHDefExtractIconW(location, index, 0, nullptr, &icon, MAKELONG(32, 32));
  CoTaskMemFree(location);
  if (!icon) return {};
  auto bitmap = IconUtil::CreateSkBitmapFromHICON(icon);
  DestroyIcon(icon);
  return bitmap.drawsNothing() ? gfx::ImageSkia() : MenuIcon(gfx::ImageSkia::CreateFrom1xBitmap(bitmap));
}
#endif
}
std::vector<FileApplication> FileApplications(const base::FilePath& path,
    const std::optional<std::string>& member_mime) {
  std::vector<FileApplication> apps;
#if BUILDFLAG(IS_MAC)
  apps = FileApplicationsMac(path, member_mime);
#elif BUILDFLAG(IS_LINUX)
  gchar* type = nullptr;
  if (member_mime) {
    if (*member_mime == "application/octet-stream")
      type = g_content_type_guess(path.value().c_str(), nullptr, 0, nullptr);
    else
      type = g_content_type_from_mime_type(member_mime->c_str());
  } else {
    GFile* file = g_file_new_for_path(path.value().c_str());
    GFileInfo* info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    if (info) {
      type = g_strdup(g_file_info_get_content_type(info));
      g_object_unref(info);
    }
    g_object_unref(file);
  }
  if (type) {
    const auto icon_directories = ApplicationIconDirectories();
    GAppInfo* preferred = g_app_info_get_default_for_type(type, false);
    auto add = [&](GAppInfo* app, bool is_default) {
      const char* id = g_app_info_get_id(app);
      const char* name = g_app_info_get_display_name(app);
      if (id && name) apps.push_back({id, name, is_default, ApplicationIcon(app, icon_directories)});
    };
    if (preferred) add(preferred, true);
    GList* list = g_app_info_get_all_for_type(type);
    for (GList* node = list; node; node = node->next) {
      auto* app = G_APP_INFO(node->data);
      if (!preferred || !g_app_info_equal(preferred, app)) add(app, false);
    }
    g_list_free_full(list, g_object_unref);
    if (preferred) g_object_unref(preferred);
    g_free(type);
  }
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
      SHFILEINFOW info{};
      gfx::ImageSkia icon;
      UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
      if (member_mime) flags |= SHGFI_USEFILEATTRIBUTES;
      if (SHGetFileInfoW(path.value().c_str(), member_mime ? FILE_ATTRIBUTE_NORMAL : 0, &info, sizeof(info), flags) && info.hIcon) {
        auto bitmap = IconUtil::CreateSkBitmapFromHICON(info.hIcon);
        DestroyIcon(info.hIcon);
        if (!bitmap.drawsNothing()) icon = MenuIcon(gfx::ImageSkia::CreateFrom1xBitmap(bitmap));
      }
      apps.push_back({{}, base::WideToUTF8(name), true, std::move(icon)});
    }
  }
  Microsoft::WRL::ComPtr<IEnumAssocHandlers> handlers;
  if (SUCCEEDED(SHAssocEnumHandlers(extension.c_str(), ASSOC_FILTER_NONE, &handlers))) {
    Microsoft::WRL::ComPtr<IAssocHandler> handler;
    while (handlers->Next(1, &handler, nullptr) == S_OK) {
      PWSTR id = nullptr, name = nullptr;
      if (SUCCEEDED(handler->GetName(&id)) && SUCCEEDED(handler->GetUIName(&name)) && id && name) {
        auto icon = ApplicationIcon(handler.Get());
        const auto display_name = base::WideToUTF8(name);
        if (!apps.empty() && apps.front().is_default && apps.front().name == display_name && !icon.isNull())
          apps.front().icon = icon;
        apps.push_back({base::WideToUTF8(id), display_name, false, std::move(icon)});
      }
      CoTaskMemFree(id); CoTaskMemFree(name); handler.Reset();
    }
  }
  if (!member_mime || !apps.empty())
    apps.push_back({"windows:choose", base::UTF16ToUTF8(l10n_util::GetStringUTF16(IDS_CRFILES_OPEN_WITH)) + "…", false, {}});
#endif
  return apps;
}
bool OpenFileWith(const base::FilePath& path, const std::string& application) {
  FileOperationLog log("open_file", "menu", path.AsUTF8Unsafe(), application);
  bool ok = false;
#if BUILDFLAG(IS_LINUX)
  if (application.empty()) {
    GFile* file = g_file_new_for_path(path.value().c_str());
    char* uri = g_file_get_uri(file);
    ok = uri && g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
    g_free(uri); g_object_unref(file);
  } else if (auto* app = g_desktop_app_info_new(application.c_str())) {
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
  ok = OpenFileWithMac(path, application);
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
void CalculatePropertiesSize(std::vector<base::FilePath> paths,
                             std::shared_ptr<PropertiesSizeProgress> progress) {
  uint64_t bytes = 0;
  // Avoid counting nested selections twice. Do not resolve links here.
  std::ranges::sort(paths);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  std::vector<base::FilePath> roots;
  for (const auto& path : paths) {
    if (progress->canceled.load(std::memory_order_relaxed)) return;
    if (!std::ranges::any_of(roots, [&](const auto& root) { return root.IsParent(path); }))
      roots.push_back(path);
  }
  std::vector<std::unique_ptr<base::FileEnumerator>> stack;
  const auto inspect = [&](const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
    const DWORD attributes = GetFileAttributesW(path.value().c_str());
    const bool link = attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    const bool link = base::IsLink(path);
#endif
    base::File::Info info;
    if (link || !base::GetFileInfo(path, &info)) {
      progress->incomplete.store(true, std::memory_order_relaxed);
      return;
    }
    if (!info.is_directory) {
      if (info.size < 0 || static_cast<uint64_t>(info.size) > std::numeric_limits<uint64_t>::max() - bytes) {
        progress->incomplete.store(true, std::memory_order_relaxed);
        return;
      }
      bytes += static_cast<uint64_t>(info.size);
      progress->bytes.store(bytes, std::memory_order_relaxed);
      return;
    }
    int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
    types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
    stack.push_back(std::make_unique<base::FileEnumerator>(path, false, types,
        base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
        base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION));
  };
  for (const auto& root : roots) {
    if (progress->canceled.load(std::memory_order_relaxed)) return;
    inspect(root);
    while (!stack.empty()) {
      if (progress->canceled.load(std::memory_order_relaxed)) return;
      const auto child = stack.back()->Next();
      if (child.empty()) {
        if (stack.back()->GetError() != base::File::FILE_OK)
          progress->incomplete.store(true, std::memory_order_relaxed);
        stack.pop_back();
      } else {
        inspect(child);
      }
    }
  }
  progress->done.store(true, std::memory_order_release);
}
bool OpenFolderTerminal(const base::FilePath& path) {
  base::LaunchOptions options; options.current_directory = path;
#if BUILDFLAG(IS_WIN)
  return base::LaunchProcess(std::wstring(L"cmd.exe"), options).IsValid();
#elif BUILDFLAG(IS_MAC)
  return OpenFileWithMac(path, "Terminal");
#else
  for (const char* command : {"xdg-terminal-exec", "x-terminal-emulator"})
    if (base::LaunchProcess(std::vector<std::string>{command}, options).IsValid()) return true;
  return false;
#endif
}
}
