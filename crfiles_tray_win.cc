// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_tray.h"

#include <windows.h>

#include <shellapi.h>

#include <cwchar>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "crFiles/resources/win/crfiles_resource.h"
#include "ui/display/win/screen_win.h"

namespace crfiles {
namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
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
    wcscpy_s(data.szTip, L"crFiles");
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
        if (event == NIN_SELECT || event == NIN_KEYSELECT) {
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
  base::RepeatingCallback<void(TrayCommand)> callback_;
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
