// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_frame_view.h"
#include <algorithm>
#include <vector>
#include "base/memory/weak_ptr.h"
#include "base/task/thread_pool.h"
#include "base/task/single_thread_task_runner.h"
#include "crFiles/crfiles_dialog.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/controls/menu/menu_runner.h"
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
#include "ui/views/controls/button/image_button.h"
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
#include "ui/gfx/animation/slide_animation.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/gfx/geometry/rrect_f.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRRect.h"
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
#include "base/time/time.h"
#include "ui/display/win/screen_win.h"
#endif
#if BUILDFLAG(IS_WIN)
#include "ui/aura/window.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#endif

namespace crfiles {
namespace {
// Match crPhotos' custom frame dimensions and shared caption controls.
constexpr int kTitlebarHeight = kCrFilesTitlebarHeight;
constexpr int kFrameBorder = 1;
constexpr int kResizeBorder = 6;

class SidebarButton : public views::ImageButton {
  METADATA_HEADER(SidebarButton, views::ImageButton)

 public:
  SidebarButton() : views::ImageButton(base::BindRepeating(&ToggleCrFilesSidebar)) {
    SetAnimateOnStateChange(true);
    SetAnimationDuration(base::Milliseconds(120));
    selection_.SetSlideDuration(base::Milliseconds(120));
    GetViewAccessibility().SetRole(ax::mojom::Role::kToggleButton);
    UpdateState(IsCrFilesSidebarVisible(), false);
    subscription_ = ObserveCrFilesSidebar(base::BindRepeating(
        &SidebarButton::OnSidebarChanged, base::Unretained(this)));
  }
 private:
  void OnSidebarChanged(bool visible) { UpdateState(visible, true); }
  void UpdateState(bool visible, bool animate) {
    if (initialized_ && visible_ == visible) return;
    initialized_ = true;
    visible_ = visible;
    const auto text = l10n_util::GetStringUTF16(visible
        ? IDS_CRFILES_HIDE_SIDEBAR : IDS_CRFILES_SHOW_SIDEBAR);
    SetTooltipText(text);
    SetAccessibleName(text);
    GetViewAccessibility().SetCheckedState(visible
        ? ax::mojom::CheckedState::kTrue : ax::mojom::CheckedState::kFalse);
    if (!animate) selection_.Reset(visible ? 1.0 : 0.0);
    else if (visible) selection_.Show();
    else selection_.Hide();
    SchedulePaint();
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    const auto* colors = GetColorProvider();
    if (!colors) return;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(colors->GetColor(ui::kColorSysPrimary));
    const float hover = static_cast<float>(hover_animation().GetCurrentValue());
    flags.setAlphaf(0.12f * static_cast<float>(selection_.GetCurrentValue()) +
                    0.08f * hover + (GetState() == STATE_PRESSED ? 0.04f : 0.0f));
    canvas->DrawRoundRect(GetLocalBounds(), 8, flags);
  }
  bool initialized_ = false;
  bool visible_ = false;
  gfx::SlideAnimation selection_{this};
  base::CallbackListSubscription subscription_;
};
BEGIN_METADATA(SidebarButton)
END_METADATA

enum MenuCommand {
  kMenuCommandThemeSystem = 1, kMenuCommandThemeLight, kMenuCommandThemeDark,
  kMenuCommandHistory, kMenuCommandTrustedDevices, kMenuCommandSharingFolders, kMenuCommandSyncing, kMenuCommandSettings, kMenuCommandAbout, kMenuCommandInstallOnDevice,
};
class OptionsMenuView : public libcr::OptionsMenuView {
  METADATA_HEADER(OptionsMenuView, libcr::OptionsMenuView)

 public:
  explicit OptionsMenuView(base::RepeatingCallback<void(int)> callback)
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
    AddMenuItem(base::BindRepeating([](base::RepeatingCallback<void(int)> callback) {
          callback.Run(kMenuCommandSyncing);
        }, callback_), l10n_util::GetStringUTF16(IDS_CRFILES_SYNCING),
        std::u16string_view(), &vector_icons::kSyncIcon);
    AddMenuSeparator();
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
class TitlebarMouseHandler : public ui::EventHandler {
 public:
  TitlebarMouseHandler(views::FrameView* frame, views::Widget* widget)
      : frame_(frame), widget_(widget),
        event_window_(widget->GetNativeView()->GetRootWindow()) {
    // Caption input may target the host rather than the content window.
    event_window_->AddPreTargetHandler(this, ui::EventTarget::Priority::kSystem);
  }
  ~TitlebarMouseHandler() override {
    event_window_->RemovePreTargetHandler(this);
  }
  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->type() != ui::EventType::kMousePressed) return;
#if BUILDFLAG(IS_WIN)
    const auto previous_press = last_press_;
    last_press_ = base::TimeTicks();
#endif
    if ((!event->IsOnlyLeftMouseButton() && !event->IsOnlyRightMouseButton()) || !event->target() ||
        widget_->IsClosed() || widget_->IsFullscreen() ||
        (event->IsOnlyLeftMouseButton() && !widget_->widget_delegate()->CanMaximize()) ||
        IsCrFilesWindowBlockedByModal(widget_)) return;
    auto point = event->location();
    aura::Window::ConvertPointToTarget(
        static_cast<aura::Window*>(event->target()),
        widget_->GetNativeView(), &point);
    views::View::ConvertPointToTarget(widget_->GetRootView(), frame_, &point);
    if (frame_->NonClientHitTest(point) != HTCAPTION) return;
    views::View::ConvertPointToScreen(frame_, &point);
    if (event->IsOnlyRightMouseButton()) {
      event->StopPropagation();
      frame_->ShowContextMenu(point, ui::mojom::MenuSourceType::kMouse);
      return;
    }
#if BUILDFLAG(IS_WIN)
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
#endif
  }
 private:
  raw_ptr<views::FrameView> frame_;
  raw_ptr<views::Widget> widget_;
  raw_ptr<aura::Window> event_window_;
#if BUILDFLAG(IS_WIN)
  base::TimeTicks last_press_;
  gfx::Point last_press_point_;
#endif
};
#endif

class FilesFrameView : public views::FrameView, public views::WidgetObserver,
                       public views::ContextMenuController, public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(FilesFrameView, views::FrameView)
 public:
  explicit FilesFrameView(views::Widget* widget) : widget_(widget) {
    set_context_menu_controller(this);
    icon_ = AddChildView(std::make_unique<SidebarButton>());
    icon_->SetImageModel(views::Button::STATE_NORMAL, CreateCrFilesIconModel(gfx::Size(20, 20)));
    icon_->SetImageHorizontalAlignment(views::ImageButton::ALIGN_CENTER);
    icon_->SetImageVerticalAlignment(views::ImageButton::ALIGN_MIDDLE);
    icon_->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    views::SetHitTestComponent(icon_, HTCLIENT);
    options_button_ = AddChildView(std::make_unique<libcr::TitlebarIconButton>(
        base::BindRepeating(&FilesFrameView::ShowOptionsMenu, weak_factory_.GetWeakPtr()),
        kBrowserToolsTouchOldIcon));
    options_button_->SetTooltipText(l10n_util::GetStringUTF16(IDS_CRFILES_MENU));
    views::SetHitTestComponent(options_button_, HTCLIENT);
    separator_ = AddChildView(std::make_unique<views::Separator>());
    separator_->SetOrientation(views::Separator::Orientation::kVertical);
    separator_->SetColorId(ui::kColorSysOnHeaderDivider);
    tabs_ = AddChildView(CreateCrFilesTabStrip(widget_));
    tabs_->set_context_menu_controller(this);
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
  void ShowContextMenuForViewImpl(views::View*, const gfx::Point& point,
                                 ui::mojom::MenuSourceType type) override {
    gfx::Point local = point;
    views::View::ConvertPointFromScreen(this, &local);
    if (NonClientHitTest(local) != HTCAPTION || IsCrFilesWindowBlockedByModal(widget_)) return;
    title_menu_runner_.reset();
    title_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
    auto add = [&](int id) { title_menu_->AddItem(id, l10n_util::GetStringUTF16(id)); };
    add(IDS_CRFILES_MINIMIZE);
    add(widget_->IsMaximized() ? IDS_CRFILES_RESTORE : IDS_CRFILES_MAXIMIZE);
    title_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
    add(IDS_CRFILES_NEW_TAB);
    add(IDS_CRFILES_REOPEN_CLOSED_TAB);
    add(IDS_CRFILES_BOOKMARK_ALL_TABS);
    title_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
    add(IDS_CRFILES_CLOSE);
    title_menu_runner_ = std::make_unique<views::MenuRunner>(title_menu_.get(), views::MenuRunner::CONTEXT_MENU);
    title_menu_runner_->RunMenuAt(widget_, nullptr, gfx::Rect(point, gfx::Size()),
        views::MenuAnchorPosition::kTopLeft, type);
  }
  bool IsCommandIdEnabled(int command) const override {
    if (command == IDS_CRFILES_REOPEN_CLOSED_TAB) return CanReopenCrFilesTab();
    if (command == IDS_CRFILES_MINIMIZE) return widget_->widget_delegate()->CanMinimize();
    if (command == IDS_CRFILES_MAXIMIZE) return widget_->widget_delegate()->CanMaximize();
    return true;
  }
  void ExecuteCommand(int command, int) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&FilesFrameView::RunTitlebarCommand, weak_factory_.GetWeakPtr(), command));
  }
  void RunTitlebarCommand(int command) {
    if (widget_->IsClosed() || IsCrFilesWindowBlockedByModal(widget_)) return;
    switch (command) {
      case IDS_CRFILES_MINIMIZE: widget_->Minimize(); break;
      case IDS_CRFILES_RESTORE: widget_->Restore(); break;
      case IDS_CRFILES_MAXIMIZE: widget_->Maximize(); break;
      case IDS_CRFILES_NEW_TAB: NewCrFilesTab(); break;
      case IDS_CRFILES_REOPEN_CLOSED_TAB: ReopenCrFilesTab(); break;
      case IDS_CRFILES_BOOKMARK_ALL_TABS: BookmarkCrFilesTabs(); break;
      case IDS_CRFILES_CLOSE: widget_->Close(); break;
    }
  }
#if BUILDFLAG(IS_WIN)
  void AddedToWidget() override {
    views::FrameView::AddedToWidget();
    titlebar_click_handler_ =
        std::make_unique<TitlebarMouseHandler>(this, widget_);
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
    if (icon_->GetVisible() && icon_->bounds().Contains(point)) return HTCLIENT;
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
    auto client_point = point;
    views::View::ConvertPointToWidget(this, &client_point);
    // Path-bar blank areas opt into native caption dragging and double-clicks.
    if (!IsCrFilesWindowBlockedByModal(widget_) &&
        views::GetHitTestComponent(widget_->client_view(), client_point) == HTCAPTION)
      return HTCAPTION;
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
    client->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF());
    const bool show = !widget_->IsFullscreen();
    if (!show) HideOptionsMenu();
    options_button_->SetVisible(show);
    icon_->SetVisible(show && windows_titlebar_style_);
    const bool show_caption = show && windows_titlebar_style_;
    separator_->SetVisible(show_caption);
    icon_->SetBounds(8, 2, 32, 32);
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
    const float radius = CornerRadius();
    const gfx::RRectF outline(gfx::RectF(surface), radius, radius, radius, radius, 0, 0, 0, 0);
    canvas->DrawPath(SkPath::RRect(SkRRect{outline}), flags);
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
          weak_factory_.GetWeakPtr())));
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
  if (command == kMenuCommandHistory) ShowCrFilesHistory();
  if (command == kMenuCommandTrustedDevices) ShowCrFilesTrustedDevices();
  if (command == kMenuCommandSharingFolders) ShowCrFilesSharingFolders();
  if (command == kMenuCommandSyncing) ShowCrFilesSyncing();
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
  std::unique_ptr<ui::SimpleMenuModel> title_menu_;
  std::unique_ptr<views::MenuRunner> title_menu_runner_;
  raw_ptr<views::View> options_menu_overlay_ = nullptr;
  raw_ptr<views::View> options_menu_view_ = nullptr;
  raw_ptr<views::ImageButton> icon_ = nullptr;
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
  std::unique_ptr<TitlebarMouseHandler> titlebar_click_handler_;
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
