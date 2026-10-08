// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_tray.h"

#include <windows.h>

#include <shellapi.h>

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/rtl.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/string_util_win.h"
#include "base/task/single_thread_task_runner.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/resources/win/crfiles_resource.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/display/win/screen_win.h"

namespace crfiles {
namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kOpenCommand = 1;
constexpr UINT kSettingsCommand = 2;
constexpr UINT kQuitCommand = 3;
constexpr wchar_t kWindowClass[] = L"CrFilesTrayWindow";
class CrFilesTrayWin : public CrFilesTray {
 public:
  explicit CrFilesTrayWin(base::RepeatingCallback<void(TrayCommand)> callback)
      : callback_(std::move(callback)) {
    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return;
    }
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    // A hidden top-level window receives Explorer's broadcast after a restart.
    window_ =
        CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"", WS_POPUP, 0, 0, 0,
                        0, nullptr, nullptr, window_class.hInstance, this);
    if (!window_) {
      return;
    }
    icon_ = static_cast<HICON>(LoadImageW(
        window_class.hInstance, MAKEINTRESOURCEW(IDI_CRFILES_APP_ICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), 0));
    if (icon_) {
      AddIcon();
    }
  }
  ~CrFilesTrayWin() override {
    if (menu_open_) {
      EndMenu();
    }
    if (window_) {
      KillTimer(window_, 1);
      auto data = IconData();
      Shell_NotifyIconW(NIM_DELETE, &data);
      DestroyWindow(window_);
    }
    if (icon_) {
      DestroyIcon(icon_);
    }
  }
  bool IsAvailable() const override { return installed_; }
  std::optional<gfx::Rect> GetIconBounds() const override {
    if (!installed_) {
      return std::nullopt;
    }
    NOTIFYICONIDENTIFIER identifier = {};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = window_;
    identifier.uID = 1;
    RECT rect = {};
    if (FAILED(Shell_NotifyIconGetRect(&identifier, &rect)) ||
        rect.right <= rect.left || rect.bottom <= rect.top) {
      return std::nullopt;
    }
    return display::win::GetScreenWin()->ScreenToDIPRect(
        nullptr, gfx::Rect(rect.left, rect.top, rect.right - rect.left,
                           rect.bottom - rect.top));
  }

 private:
  NOTIFYICONDATAW IconData() const {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon_;
    base::wcslcpy(data.szTip, L"crFiles");
    return data;
  }
  void AddIcon() {
    auto data = IconData();
    installed_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (installed_) {
      data.uVersion = NOTIFYICON_VERSION_4;
      Shell_NotifyIconW(NIM_SETVERSION, &data);
      KillTimer(window_, 1);
    } else {
      SetTimer(window_, 1, 1000, nullptr);
    }
  }
  void Dispatch(TrayCommand command) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(callback_, command));
  }
  void ShowMenu() {
    POINT cursor = {};
    if (!window_ || menu_open_ || !GetCursorPos(&cursor)) {
      return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) {
      return;
    }
    const auto append_item = [menu](UINT command, int string_id) {
      const std::u16string label = l10n_util::GetStringUTF16(string_id);
      return AppendMenuW(menu, MF_STRING, command, base::as_wcstr(label)) !=
             FALSE;
    };
    if (!append_item(kOpenCommand, IDS_CRFILES_TRAY_OPEN) ||
        !append_item(kSettingsCommand, IDS_CRFILES_SETTINGS) ||
        !AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) ||
        !append_item(kQuitCommand, IDS_CRFILES_MAC_QUIT)) {
      DestroyMenu(menu);
      return;
    }
    SetMenuDefaultItem(menu, kOpenCommand, FALSE);
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON |
                 TPM_BOTTOMALIGN;
    flags |= base::i18n::IsRTL() ? TPM_RIGHTALIGN | TPM_LAYOUTRTL
                               : TPM_LEFTALIGN;
    // The native menu runs a nested loop; the tray can be removed meanwhile.
    const auto weak = weak_factory_.GetWeakPtr();
    menu_open_ = true;
    SetForegroundWindow(window_);
    if (!weak) {
      DestroyMenu(menu);
      return;
    }
    const UINT command =
        TrackPopupMenuEx(menu, flags, cursor.x, cursor.y, window_, nullptr);
    DestroyMenu(menu);
    if (!weak) {
      return;
    }
    menu_open_ = false;
    // Let clicks outside dismiss this menu, including on subsequent openings.
    PostMessageW(window_, WM_NULL, 0, 0);
    switch (command) {
      case kOpenCommand:
        Dispatch(TrayCommand::kOpen);
        break;
      case kSettingsCommand:
        Dispatch(TrayCommand::kSettings);
        break;
      case kQuitCommand:
        Dispatch(TrayCommand::kQuit);
        break;
    }
  }
  static LRESULT CALLBACK WindowProc(HWND window,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* params = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(params->lpCreateParams));
    }
    auto* self = reinterpret_cast<CrFilesTrayWin*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) {
      if (self->taskbar_created_ && message == self->taskbar_created_) {
        self->installed_ = false;
        self->AddIcon();
        return 0;
      }
      if (message == WM_TIMER && wparam == 1) {
        self->AddIcon();
        return 0;
      }
      if (message == kTrayMessage) {
        const UINT event = LOWORD(lparam);
        if (event == WM_CONTEXTMENU) {
          self->ShowMenu();
        } else if (!self->menu_open_ &&
                   (event == NIN_SELECT || event == NIN_KEYSELECT)) {
          self->Dispatch(TrayCommand::kToggle);
        }
        return 0;
      }
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
  HWND window_ = nullptr;
  HICON icon_ = nullptr;
  UINT taskbar_created_ = 0;
  bool installed_ = false;
  bool menu_open_ = false;
  base::RepeatingCallback<void(TrayCommand)> callback_;
  base::WeakPtrFactory<CrFilesTrayWin> weak_factory_{this};
};
}  // namespace
std::unique_ptr<CrFilesTray> CreateCrFilesTray(
    base::RepeatingCallback<void(TrayCommand)> callback) {
  auto tray = std::make_unique<CrFilesTrayWin>(std::move(callback));
  if (!tray->IsAvailable()) {
    return nullptr;
  }
  return tray;
}
}  // namespace crfiles
