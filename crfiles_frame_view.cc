// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_frame_view.h"
#include <algorithm>
#include <vector>
#include "base/memory/weak_ptr.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "components/vector_icons/vector_icons.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_window.h"
#include "crFiles/crfiles_settings_dialog.h"
#include "crFiles/crfiles_app_icon.h"
#include "libcr/options_menu.h"
#include "libcr/install_result_bubble.h"
#include "libcr/linux_desktop_application.h"
#include "ui/compositor/layer.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/separator.h"

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "cc/paint/paint_flags.h"
#include "crFiles/grit/crfiles_strings.h"
#include "libcr/titlebar_controls.h"
#include "ui/base/hit_test.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/shadow_value.h"
#include "ui/views/controls/label.h"
#include "ui/views/widget/widget.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/client_view.h"
#include "ui/views/window/frame_view.h"
#include "ui/views/window/hit_test_utils.h"
#if BUILDFLAG(IS_MAC)
#include "crFiles/crfiles_mac.h"
#endif
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <cmath>
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "crFiles/crfiles_dialog.h"
#include "ui/aura/window.h"
#include "ui/display/win/screen_win.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#endif

namespace crfiles {
namespace {
// Match crPhotos' custom frame dimensions and shared caption controls.
constexpr int kTitlebarHeight = kCrFilesTitlebarHeight;
constexpr int kFrameBorder = 1;
constexpr int kResizeBorder = 6;

enum MenuCommand {
  kMenuCommandThemeSystem = 1, kMenuCommandThemeLight, kMenuCommandThemeDark,
  kMenuCommandFullscreen, kMenuCommandHistory, kMenuCommandTrustedDevices, kMenuCommandSharingFolders, kMenuCommandSettings, kMenuCommandAbout, kMenuCommandInstallOnDevice,
};
class OptionsMenuView : public libcr::OptionsMenuView {
  METADATA_HEADER(OptionsMenuView, libcr::OptionsMenuView)

 public:
  explicit OptionsMenuView(base::RepeatingCallback<void(int)> callback, bool video_open, bool fullscreen)
      : callback_(std::move(callback)) {
    AddThemeSelector(
        GetCrFilesThemeMode(),
        {l10n_util::GetStringUTF16(IDS_CRFILES_THEME_SYSTEM),
         l10n_util::GetStringUTF16(IDS_CRFILES_THEME_LIGHT),
         l10n_util::GetStringUTF16(IDS_CRFILES_THEME_DARK)},
        base::BindRepeating(
            [](base::RepeatingCallback<void(int)> callback,
               libcr::ThemeMode mode) {
              callback.Run(mode == libcr::ThemeMode::kSystem
                               ? kMenuCommandThemeSystem
                               : mode == libcr::ThemeMode::kLight
                                     ? kMenuCommandThemeLight
                                     : kMenuCommandThemeDark);
            },
            callback_));
    AddMenuSeparator();
    if (video_open) {
      AddMenuItem(base::BindRepeating([](base::RepeatingCallback<void(int)> callback) {
            callback.Run(kMenuCommandFullscreen);
          }, callback_),
          l10n_util::GetStringUTF16(fullscreen ? IDS_CRFILES_EXIT_FULLSCREEN : IDS_CRFILES_FULLSCREEN),
          u"F11", &kFullscreenIcon);
    }
    AddMenuItem(base::BindRepeating([](base::RepeatingCallback<void(int)> callback) {
          callback.Run(kMenuCommandHistory);
        }, callback_), l10n_util::GetStringUTF16(IDS_CRFILES_HISTORY),
        std::u16string_view(), &kListAltIcon);
    AddMenuItem(base::BindRepeating([](base::RepeatingCallback<void(int)> callback) {
          callback.Run(kMenuCommandTrustedDevices);
        }, callback_), l10n_util::GetStringUTF16(IDS_CRFILES_NEARBY_TRUSTED_DEVICES),
        std::u16string_view(), &vector_icons::kDevicesIcon);
    AddMenuItem(base::BindRepeating([](base::RepeatingCallback<void(int)> callback) {
          callback.Run(kMenuCommandSharingFolders);
        }, callback_), l10n_util::GetStringUTF16(IDS_CRFILES_SHARING_FOLDERS),
        std::u16string_view(), &vector_icons::kFolderOpenIcon);
    AddMenuItem(base::BindRepeating(
                    [](base::RepeatingCallback<void(int)> callback) {
                      callback.Run(kMenuCommandSettings);
                    },
                    callback_),
                l10n_util::GetStringUTF16(IDS_CRFILES_SETTINGS),
                std::u16string_view(), &vector_icons::kSettingsIcon);
    AddMenuSeparator();
    AddMenuItem(base::BindRepeating(
                    [](base::RepeatingCallback<void(int)> callback) {
                      callback.Run(kMenuCommandAbout);
                    },
                    callback_),
                l10n_util::GetStringUTF16(IDS_CRFILES_ABOUT),
                std::u16string_view(), &kInfoIcon);
#if BUILDFLAG(IS_LINUX)
    AddMenuItem(base::BindRepeating(
                    [](base::RepeatingCallback<void(int)> callback) {
                      callback.Run(kMenuCommandInstallOnDevice);
                    },
                    callback_),
                l10n_util::GetStringUTF16(IDS_CRFILES_INSTALL_ON_DEVICE),
                std::u16string_view(), &vector_icons::kInstallDesktopIcon);
#endif
  }

 private:
  base::RepeatingCallback<void(int)> callback_;
};

BEGIN_METADATA(OptionsMenuView)
END_METADATA

class OptionsMenuOverlayView : public views::View {
  METADATA_HEADER(OptionsMenuOverlayView, views::View)

 public:
  explicit OptionsMenuOverlayView(base::RepeatingClosure close_callback)
      : close_callback_(std::move(close_callback)) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
  }

  bool OnMousePressed(const ui::MouseEvent&) override {
    close_callback_.Run();
    return true;
  }

 private:
  base::RepeatingClosure close_callback_;
};

BEGIN_METADATA(OptionsMenuOverlayView)
END_METADATA

#if BUILDFLAG(IS_WIN)
class TitlebarDoubleClickHandler : public ui::EventHandler {
 public:
  TitlebarDoubleClickHandler(views::FrameView* frame, views::Widget* widget)
      : frame_(frame), widget_(widget),
        event_window_(widget->GetNativeView()->GetRootWindow()) {
    // Caption input may target the host rather than the content window.
    event_window_->AddPreTargetHandler(this);
  }
  ~TitlebarDoubleClickHandler() override {
    event_window_->RemovePreTargetHandler(this);
  }
  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->type() != ui::EventType::kMousePressed) return;
    const auto previous_press = last_press_;
    last_press_ = base::TimeTicks();
    if (!event->IsOnlyLeftMouseButton() || !event->target() ||
        widget_->IsClosed() || widget_->IsFullscreen() ||
        !widget_->widget_delegate()->CanMaximize() ||
        IsCrFilesWindowBlockedByModal(widget_)) return;
    auto point = event->location();
    aura::Window::ConvertPointToTarget(
        static_cast<aura::Window*>(event->target()),
        widget_->GetNativeView(), &point);
    views::View::ConvertPointToTarget(widget_->GetRootView(), frame_, &point);
    if (frame_->NonClientHitTest(point) != HTCAPTION) return;
    views::View::ConvertPointToScreen(frame_, &point);
    const auto screen_point =
        display::win::GetScreenWin()->DIPToScreenPoint(point);
    const auto elapsed = event->time_stamp() - previous_press;
    const auto delta = screen_point - last_press_point_;
    // Client/non-client transitions can reset Chromium's global click count.
    const bool double_click = !previous_press.is_null() &&
        elapsed > base::TimeDelta() &&
        elapsed <= base::Milliseconds(GetDoubleClickTime()) &&
        std::abs(delta.x()) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
        std::abs(delta.y()) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    last_press_ = event->time_stamp();
    last_press_point_ = screen_point;
    if (!double_click) return;
    last_press_ = base::TimeTicks();
    // A frameless translucent HWND cannot use native double-click maximize.
    event->StopPropagation();
    // Complete mouse dispatch before changing the HWND's bounds/show state.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce([](base::WeakPtr<views::Widget> widget, bool maximize) {
          if (!widget || widget->IsClosed() || widget->IsFullscreen() ||
              IsCrFilesWindowBlockedByModal(widget.get())) return;
          if (maximize) widget->Maximize();
          else widget->Restore();
        }, widget_->GetWeakPtr(), !widget_->IsMaximized()));
  }
 private:
  raw_ptr<views::FrameView> frame_;
  raw_ptr<views::Widget> widget_;
  raw_ptr<aura::Window> event_window_;
  base::TimeTicks last_press_;
  gfx::Point last_press_point_;
};
#endif

class FilesFrameView : public views::FrameView, public views::WidgetObserver {
  METADATA_HEADER(FilesFrameView, views::FrameView)
 public:
  explicit FilesFrameView(views::Widget* widget) : widget_(widget) {
    icon_ = AddChildView(std::make_unique<views::ImageView>(
        CreateCrFilesIconModel(gfx::Size(20, 20))));
    icon_->SetCanProcessEventsWithinSubtree(false);
    options_button_ = AddChildView(std::make_unique<libcr::TitlebarIconButton>(
        base::BindRepeating(&FilesFrameView::ShowOptionsMenu, weak_factory_.GetWeakPtr()),
        kBrowserToolsTouchOldIcon));
    options_button_->SetTooltipText(l10n_util::GetStringUTF16(IDS_CRFILES_MENU));
    views::SetHitTestComponent(options_button_, HTCLIENT);
    separator_ = AddChildView(std::make_unique<views::Separator>());
    separator_->SetOrientation(views::Separator::Orientation::kVertical);
    separator_->SetColorId(ui::kColorSysOnHeaderDivider);
    tabs_ = AddChildView(CreateCrFilesTabStrip(widget_));
    title_ = AddChildView(std::make_unique<views::Label>());
    // The frame paints rounded corners and shadows onto a translucent layer.
    title_->SetSubpixelRenderingEnabled(false);
    title_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title_->SetElideBehavior(gfx::ELIDE_MIDDLE);
    title_->SetCanProcessEventsWithinSubtree(false);
    minimize_ = AddCaption(libcr::CaptionButtonType::kMinimize, IDS_CRFILES_MINIMIZE,
        base::BindRepeating([](views::Widget* w) { w->Minimize(); }, widget_));
    maximize_ = AddCaption(libcr::CaptionButtonType::kMaximize, IDS_CRFILES_MAXIMIZE,
        base::BindRepeating([](views::Widget* w) { w->Maximize(); }, widget_));
    restore_ = AddCaption(libcr::CaptionButtonType::kRestore, IDS_CRFILES_RESTORE,
        base::BindRepeating([](views::Widget* w) { w->Restore(); }, widget_));
    close_ = AddCaption(libcr::CaptionButtonType::kClose, IDS_CRFILES_CLOSE,
        base::BindRepeating([](views::Widget* w) { w->Close(); }, widget_));
    AddAccelerator(ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE));
    observation_.Observe(widget_);
    UpdateWindowTitle();
    UpdateColors();
  }
#if BUILDFLAG(IS_WIN)
  void AddedToWidget() override {
    views::FrameView::AddedToWidget();
    titlebar_click_handler_ =
        std::make_unique<TitlebarDoubleClickHandler>(this, widget_);
  }
  void RemovedFromWidget() override {
    titlebar_click_handler_.reset();
    views::FrameView::RemovedFromWidget();
  }
  void OnWidgetDestroying(views::Widget*) override {
    titlebar_click_handler_.reset();
  }
#endif
  int ShadowMargin() const {
#if BUILDFLAG(IS_MAC)
    // AppKit constrains the outer window frame to the screen's usable top.
    // Draw the shadow outside that frame so the visible titlebar can reach it.
    return 0;
#else
    return widget_->IsMaximized() || widget_->IsFullscreen() ? 0 : 12;
#endif
  }
  int CornerRadius() const { return !windows_titlebar_style_ || widget_->IsMaximized() || widget_->IsFullscreen() ? 0 : 12; }
  gfx::Rect GetBoundsForClientView() const override {
    if (widget_->IsFullscreen()) return GetLocalBounds();
    const int border = widget_->IsMaximized() ? 0 : kFrameBorder;
    const int margin = ShadowMargin();
    return gfx::Rect(margin + border, margin + kTitlebarHeight, std::max(0, width() - (margin + border) * 2),
                     std::max(0, height() - kTitlebarHeight - border - margin * 2));
  }
  gfx::Rect GetWindowBoundsForClientBounds(const gfx::Rect& bounds) const override {
    if (widget_->IsFullscreen()) return bounds;
    const int border = widget_->IsMaximized() ? 0 : kFrameBorder;
    const int margin = ShadowMargin();
    gfx::Rect window_bounds(bounds.x() - border - margin, bounds.y() - kTitlebarHeight - margin,
                     bounds.width() + (border + margin) * 2,
                     bounds.height() + kTitlebarHeight + border + margin * 2);
#if BUILDFLAG(IS_MAC)
    if (!windows_titlebar_style_)
      return WindowBoundsForContentMac(widget_, window_bounds);
#endif
    return window_bounds;
  }
  gfx::Size GetMinimumSize() const override {
    if (widget_->IsFullscreen()) return gfx::Size(900, 500);
    const int border = widget_->IsMaximized() ? 0 : kFrameBorder;
    return gfx::Size(900 + 2 * (ShadowMargin() + border),
                     500 + kTitlebarHeight + border + 2 * ShadowMargin());
  }
  bool HasWindowTitle() const override { return !widget_->IsFullscreen(); }
  bool IsWindowTitleVisible() const override { return !widget_->IsFullscreen(); }
  int NonClientHitTest(const gfx::Point& point) override {
    auto surface = GetLocalBounds(); surface.Inset(ShadowMargin());
    if (!surface.Contains(point)) return HTNOWHERE;
    if (widget_->IsFullscreen()) return HTCLIENT;
    if (options_menu_overlay_ || options_button_->bounds().Contains(point)) return HTCLIENT;
    for (auto* button : {minimize_.get(), maximize_.get(), restore_.get(), close_.get()})
      if (button->GetVisible() && button->bounds().Contains(point)) return HTCLIENT;
    if (windows_titlebar_style_ && !widget_->IsMaximized()) {
      const int hit = GetHTComponentForFrame(point, gfx::Insets(ShadowMargin() + kResizeBorder),
          ShadowMargin() + kResizeBorder, ShadowMargin() + kResizeBorder, widget_->widget_delegate()->CanResize());
      if (hit != HTNOWHERE) return hit;
    }
    if (tabs_->bounds().Contains(point)) {
      gfx::Point local = point;
      views::View::ConvertPointToTarget(this, tabs_, &local);
      for (auto* target = tabs_->GetEventHandlerForPoint(local);
           target && target != tabs_; target = target->parent()) {
        if (views::AsViewClass<views::Button>(target)) return HTCLIENT;
      }
      return HTCAPTION;
    }
    const int hit = widget_->client_view()->NonClientHitTest(point);
    if (hit != HTNOWHERE) return hit;
    return point.y() < kTitlebarHeight + ShadowMargin() ? HTCAPTION : HTNOWHERE;
  }
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override {
    if (accelerator.key_code() == ui::VKEY_ESCAPE && options_menu_overlay_) {
      HideOptionsMenu();
      options_button_->RequestFocus();
      return true;
    }
    return false;
  }
  void ResetWindowControls() override {
    for (auto* button : {minimize_.get(), maximize_.get(), restore_.get(), close_.get()})
      button->SetState(views::Button::STATE_NORMAL);
    InvalidateLayout();
  }
  void UpdateWindowTitle() override { title_->SetText(widget_->widget_delegate()->GetWindowTitle()); }
  void Layout(PassKey) override {
    widget_->client_view()->SetBoundsRect(GetBoundsForClientView());
    const int margin = ShadowMargin();
    const int frame_width = width() - margin * 2;
    auto* client = widget_->client_view();
    client->SetPaintToLayer(); client->layer()->SetFillsBoundsOpaquely(false);
    client->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(0, 0, margin ? 12 : 0, margin ? 12 : 0));
    const bool show = !widget_->IsFullscreen();
    if (!show) HideOptionsMenu();
    options_button_->SetVisible(show);
    icon_->SetVisible(show && windows_titlebar_style_);
    const bool show_caption = show && windows_titlebar_style_;
    separator_->SetVisible(show_caption);
    icon_->SetBounds(14, 8, 20, 20);
    options_button_->SetBounds(frame_width - (windows_titlebar_style_ ? 161 : 44), 0, 36, kTitlebarHeight);
    separator_->SetBounds(frame_width - 117, 8, 1, 20);
    title_->SetVisible(false);
    tabs_->SetVisible(show);
    int tabs_left = 42;
#if BUILDFLAG(IS_MAC)
    if (!windows_titlebar_style_ && show)
      tabs_left = WindowControlsRightEdgeMac(widget_);
#endif
    const int tabs_right = options_button_->x() - 8;
    tabs_->SetBounds(tabs_left, 0, std::max(0, tabs_right - tabs_left), kTitlebarHeight);
    minimize_->SetVisible(show_caption);
    close_->SetVisible(show_caption);
    close_->SetBounds(frame_width - 36, 0, 36, kTitlebarHeight);
    maximize_->SetBounds(frame_width - 72, 0, 36, kTitlebarHeight);
    restore_->SetBoundsRect(maximize_->bounds());
    maximize_->SetVisible(show_caption && !widget_->IsMaximized());
    restore_->SetVisible(show_caption && widget_->IsMaximized());
    minimize_->SetBounds(frame_width - 108, 0, 36, kTitlebarHeight);
    title_->SetBounds(42, 0, std::max(0, frame_width - 211), kTitlebarHeight);
    for (views::View* child : std::vector<views::View*>{static_cast<views::View*>(icon_.get()), options_button_.get(),
         separator_.get(), tabs_.get(), title_.get(), minimize_.get(), maximize_.get(), restore_.get(), close_.get()}) {
      auto bounds = child->bounds(); bounds.Offset(margin, margin); child->SetBoundsRect(bounds);
    }
    close_->SetPaintToLayer(); close_->layer()->SetFillsBoundsOpaquely(false);
    close_->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(0, CornerRadius(), 0, 0));
    LayoutOptionsMenu();
  }
  void OnPaint(gfx::Canvas* canvas) override {
    if (widget_->IsFullscreen()) { canvas->FillRect(GetLocalBounds(), SK_ColorBLACK); return; }
    const bool active = widget_->ShouldPaintAsActive();
    const int margin = ShadowMargin();
    auto surface = GetLocalBounds(); surface.Inset(margin);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetColorProvider()->GetColor(active ? ui::kColorSysHeader : ui::kColorSysHeaderInactive));
    if (margin) flags.setLooper(gfx::CreateShadowDrawLooper({gfx::ShadowValue(gfx::Vector2d(0, 3), 16,
        SkColorSetA(SK_ColorBLACK, active ? 72 : 40))}));
    canvas->DrawRoundRect(surface, CornerRadius(), flags);
  }
  void OnThemeChanged() override { views::FrameView::OnThemeChanged(); UpdateColors(); }
  void OnWidgetActivationChanged(views::Widget*, bool) override { UpdateColors(); }
  void OnWidgetShowStateChanged(views::Widget*) override { InvalidateLayout(); SchedulePaint(); }
  void OnWidgetDestroyed(views::Widget*) override { observation_.Reset(); }
 private:
void ShowOptionsMenu() {
  if (options_menu_overlay_) {
    HideOptionsMenu();
    return;
  }
  options_menu_overlay_ = AddChildView(
      std::make_unique<OptionsMenuOverlayView>(base::BindRepeating(
          &FilesFrameView::HideOptionsMenu,
          weak_factory_.GetWeakPtr())));
  views::SetHitTestComponent(options_menu_overlay_, HTCLIENT);
  options_menu_view_ = options_menu_overlay_->AddChildView(
      std::make_unique<OptionsMenuView>(base::BindRepeating(
          &FilesFrameView::HandleOptionsMenuCommand,
          weak_factory_.GetWeakPtr()), true, widget_->IsFullscreen()));
  views::SetHitTestComponent(options_menu_view_, HTCLIENT);
  options_button_->SetOverlayMenuOpen(true);
  LayoutOptionsMenu();
  SchedulePaint();
}

void HideOptionsMenu() {
  if (!options_menu_overlay_) {
    return;
  }
  views::View* overlay = options_menu_overlay_;
  options_menu_overlay_ = nullptr;
  options_menu_view_ = nullptr;
  RemoveChildViewT(overlay);
  options_button_->SetOverlayMenuOpen(false);
  SchedulePaint();
}

void LayoutOptionsMenu() {
  if (!options_menu_overlay_) return;
  options_menu_overlay_->SetBoundsRect(GetLocalBounds());
  const gfx::Size size = options_menu_view_->GetPreferredSize();
  const gfx::Insets insets = options_menu_view_->GetInsets();
  const int x = std::clamp(options_button_->bounds().right() - size.width() + insets.right(),
                           0, std::max(0, width() - size.width()));
  options_menu_view_->SetBounds(x, options_button_->bounds().bottom() - insets.top(), size.width(), size.height());
  options_menu_view_->DeprecatedLayoutImmediately();
}
void HandleOptionsMenuCommand(int command) {
  if (command >= kMenuCommandThemeSystem && command <= kMenuCommandThemeDark) {
    const auto mode = command == kMenuCommandThemeLight ? CrFilesThemeMode::kLight :
        command == kMenuCommandThemeDark ? CrFilesThemeMode::kDark : CrFilesThemeMode::kSystem;
    SetCrFilesThemeMode(mode);
    if (options_menu_view_) static_cast<OptionsMenuView*>(options_menu_view_.get())->SetThemeMode(mode);
    return;
  }
  HideOptionsMenu();
  if (command == kMenuCommandFullscreen && true) {
    widget_->SetFullscreen(!widget_->IsFullscreen());
  }
  if (command == kMenuCommandHistory) ShowCrFilesHistory();
  if (command == kMenuCommandTrustedDevices) ShowCrFilesTrustedDevices();
  if (command == kMenuCommandSharingFolders) ShowCrFilesSharingFolders();
  if (command == kMenuCommandSettings) ShowCrFilesSettingsDialog(widget_);
  if (command == kMenuCommandAbout) ShowCrFilesAboutDialog(widget_);
#if BUILDFLAG(IS_LINUX)
  if (command == kMenuCommandInstallOnDevice) InstallOnDevice();
#endif
}
#if BUILDFLAG(IS_LINUX)
void InstallOnDevice() {
  libcr::LinuxDesktopApplication application;
  application.app_id = "libcr.crfiles";
  application.name = "crFiles";
  application.comment = "Tabbed file manager";
  application.categories = "System;FileTools;FileManager;";
  application.icon_asset_path =
      base::FilePath(FILE_PATH_LITERAL("crfiles_resources"))
          .Append(FILE_PATH_LITERAL("crFiles.svg"));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&libcr::InstallLinuxDesktopApplication,
                     std::move(application)),
      base::BindOnce(&FilesFrameView::ShowInstallResult,
                     weak_factory_.GetWeakPtr()));
}

void ShowInstallResult(bool success) {
  install_result_bubble_.Show(
      options_button_, l10n_util::GetStringUTF16(
          success ? IDS_CRFILES_INSTALL_ON_DEVICE_SUCCESS
                  : IDS_CRFILES_INSTALL_ON_DEVICE_FAILED));
}
#endif


  views::Button* AddCaption(libcr::CaptionButtonType type, int text,
                            base::RepeatingClosure callback) {
    auto* button = AddChildView(std::make_unique<libcr::CaptionButton>(std::move(callback), type));
    button->SetTooltipText(l10n_util::GetStringUTF16(text));
    views::SetHitTestComponent(button, HTCLIENT);
    return button;
  }
  void UpdateColors() {
    if (title_) title_->SetEnabledColor(widget_->ShouldPaintAsActive()
        ? ui::kColorSysOnSurface : ui::kColorSysOnSurfaceSecondary);
    SchedulePaint();
  }
  raw_ptr<libcr::TitlebarIconButton> options_button_ = nullptr;
  raw_ptr<views::View> options_menu_overlay_ = nullptr;
  raw_ptr<views::View> options_menu_view_ = nullptr;
  raw_ptr<views::ImageView> icon_ = nullptr;
  raw_ptr<views::Separator> separator_ = nullptr;
#if BUILDFLAG(IS_LINUX)
  libcr::InstallResultBubbleController install_result_bubble_;
#endif
  raw_ptr<views::Widget> widget_;
  // Snapshot the preference: changing settings must not partially restyle an
  // existing native window. AppKit frame changes take effect on next launch.
#if BUILDFLAG(IS_MAC)
  const bool windows_titlebar_style_ = GetWindowsTitlebarStyleEnabled();
#else
  const bool windows_titlebar_style_ = true;
#endif
  raw_ptr<views::View> tabs_ = nullptr;
  raw_ptr<views::Label> title_ = nullptr;
  raw_ptr<views::Button> minimize_ = nullptr, maximize_ = nullptr;
  raw_ptr<views::Button> restore_ = nullptr, close_ = nullptr;
#if BUILDFLAG(IS_WIN)
  std::unique_ptr<TitlebarDoubleClickHandler> titlebar_click_handler_;
#endif
  base::ScopedObservation<views::Widget, views::WidgetObserver> observation_{this};
  base::WeakPtrFactory<FilesFrameView> weak_factory_{this};
};
BEGIN_METADATA(FilesFrameView)
END_METADATA
}  // namespace
std::unique_ptr<views::FrameView> CreateFilesFrameView(views::Widget* widget) {
  return std::make_unique<FilesFrameView>(widget);
}
}  // namespace crfiles
