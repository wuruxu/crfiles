// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_access.h"
#if BUILDFLAG(IS_MAC)
#include "crFiles/crfiles_mac.h"
#endif
#include "crFiles/crfiles_settings_dialog.h"
#include "crFiles/crfiles_dialog.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include "base/base64.h"
#include "base/i18n/rtl.h"
#include "base/memory/weak_ptr.h"
#include "crFiles/crfiles_image_preview.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/events/event_observer.h"
#include "ui/events/event.h"
#include "ui/views/event_monitor.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "ui/gfx/codec/png_codec.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "base/strings/string_number_conversions.h"
#include "skia/ext/image_operations.h"
#include <memory>
#include <string_view>
#include <vector>
#include <utility>
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/process/kill.h"
#include "base/task/single_thread_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/process/launch.h"
#include "base/command_line.h"
#include "base/timer/timer.h"
#include "base/version_info/version_info.h"
#include "build/build_config.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/animation/ink_drop.h"
#include "ui/views/animation/ink_drop_host.h"
#include "ui/views/controls/link.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/info_bubble.h"
#include "ui/views/view_tracker.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <shellapi.h>
#include "base/win/shell_util.h"
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include "base/nix/xdg_util.h"
#if BUILDFLAG(IS_LINUX)
#include "crQuickShare/wifi_direct_go.h"
#endif
#endif
#include "crFiles/crfiles_app_icon.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_version.h"
#include "crFiles/crfiles_window.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/sharing/nearby_sharing.h"
#include "libcr/options_menu.h"
#include "libcr/viz_context_factory.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom.h"
#include "ui/color/color_id.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/radio_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "ui/views/view_observer.h"
#include "crFiles/crfiles_buildtime.h"
namespace crfiles {
namespace {
constexpr std::array<const char*, 15> kLanguages = {
    "", "en-US", "ja", "ko", "zh-CN", "zh-HK", "zh-TW", "ar", "fr", "fr-CA", "ru", "es", "es-419", "pt-BR", "pt-PT"};
constexpr std::array kHistoryLabels = {IDS_CRFILES_HISTORY_MOVE, IDS_CRFILES_MOVE_TRASH,
    IDS_CRFILES_HISTORY_ACCESS, IDS_CRFILES_HISTORY_OPEN_FILE, IDS_CRFILES_HISTORY_SEND, IDS_CRFILES_HISTORY_RECEIVE};
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
views::Label* Label(views::View* parent, std::u16string text) {
  auto* label = parent->AddChildView(std::make_unique<views::Label>(text));
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true);
  return label;
}
std::unique_ptr<views::View> Page(int title, std::unique_ptr<views::View> action = nullptr) {
  auto page = std::make_unique<views::View>();
  page->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets::TLBR(26, 30, 30, 30), 10));
  // Reserve the same title row for every page, including pages with actions.
  auto* header = page->AddChildView(std::make_unique<views::View>());
  header->SetPreferredSize(gfx::Size(0, 40));
  auto* header_layout = header->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
  header_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* heading = Label(header, Text(title));
  heading->SetMultiLine(false);
  heading->SetFontList(heading->font_list().DeriveWithSizeDelta(4));
  header_layout->SetFlexForView(heading, 1);
  if (action) header->AddChildView(std::move(action));
  auto* divider = page->AddChildView(std::make_unique<views::Separator>());
  divider->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(8, 0, 16, 0)));
  return page;
}
constexpr char kProjectUrl[] = "https://github.com/libcr/crFiles";
constexpr char16_t kAuthorEmail[] = u"wrxzzj@gmail.com";

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
void LaunchCrFilesProjectPageWithActivation(base::LaunchOptions options) {
  options.allow_new_privs = true;
  options.environment["MM_NOTTTY"] = "1";
  std::vector<std::string> argv = {"xdg-open", kProjectUrl};
  base::Process process = base::LaunchProcess(argv, options);
  if (!process.IsValid()) {
    LOG(ERROR) << "crFiles failed to open project URL";
    return;
  }
  base::EnsureProcessGetsReaped(std::move(process));
}
#endif

#if BUILDFLAG(IS_WIN)
void OpenCrFilesProjectPageViaExplorer() {
  base::win::ShellExecuteOptions options;
  options.verb = L"open";
  const HRESULT result = base::win::RunShellExecuteViaExplorer(
      base::UTF8ToWide(kProjectUrl), std::wstring(), options);
  if (SUCCEEDED(result)) {
    return;
  }
  const std::wstring project_url = base::UTF8ToWide(kProjectUrl);
  const HINSTANCE fallback_result = ::ShellExecuteW(
      nullptr, L"open", project_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(fallback_result) <= 32) {
    LOG(ERROR) << "crFiles failed to open project URL";
  }
}
#endif

void OpenProject() {
#if BUILDFLAG(IS_WIN)
  base::ThreadPool::CreateCOMSTATaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING})
      ->PostTask(FROM_HERE,
                 base::BindOnce(&OpenCrFilesProjectPageViaExplorer));
#elif BUILDFLAG(IS_MAC)
  if (!OpenProjectURLMac(kProjectUrl)) LOG(ERROR) << "crFiles failed to open project URL";
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  base::nix::CreateLaunchOptionsWithXdgActivation(
      base::BindOnce(&LaunchCrFilesProjectPageWithActivation));
#else
  LOG(ERROR) << "crFiles cannot open the project URL on this platform";
#endif
}

class AuthorEmailLink : public views::Link {
  METADATA_HEADER(AuthorEmailLink, views::Link)
 public:
  AuthorEmailLink() : views::Link(kAuthorEmail) {
    SetFocusBehavior(views::View::FocusBehavior::NEVER);
    SetForceUnderline(false);
    SetCallback(base::BindRepeating(&AuthorEmailLink::CopyEmail, weak_.GetWeakPtr()));
  }
  ~AuthorEmailLink() override {
    timer_.Stop();
    HideCopiedBubble();
  }
 protected:
  void VisibilityChanged(views::View* starting_from, bool is_visible) override {
    views::Link::VisibilityChanged(starting_from, is_visible);
    if (!is_visible) {
      timer_.Stop();
      HideCopiedBubble();
    }
  }
 private:
  void CopyEmail() {
    ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste).WriteText(kAuthorEmail);
    HideCopiedBubble();
    auto* bubble = new views::InfoBubble(this, views::BubbleBorder::Arrow::TOP_CENTER,
        Text(IDS_CRFILES_COPIED));
    bubble_.SetView(bubble);
    bubble->Show();
    timer_.Start(FROM_HERE, base::Seconds(2),
        base::BindOnce(&AuthorEmailLink::HideCopiedBubble, weak_.GetWeakPtr()));
  }
  void HideCopiedBubble() {
    views::View* bubble = bubble_.view();
    bubble_.SetView(nullptr);
    if (bubble) static_cast<views::InfoBubble*>(bubble)->Hide();
  }
  views::ViewTracker bubble_;
  base::OneShotTimer timer_;
  base::WeakPtrFactory<AuthorEmailLink> weak_{this};
};
BEGIN_METADATA(AuthorEmailLink)
END_METADATA
enum class SystemInfoKind { kGraphics, kDisplay };
class SystemInfoDetails : public views::View {
  METADATA_HEADER(SystemInfoDetails, views::View)
 public:
  SystemInfoDetails(views::View* source, SystemInfoKind kind) : kind_(kind) {
    source_.SetView(source);
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(8, 12), 6));
    if (kind_ == SystemInfoKind::kDisplay) {
      for (int title : {IDS_CRFILES_DISPLAY_NAME, IDS_CRFILES_DISPLAY_RESOLUTION,
                        IDS_CRFILES_DISPLAY_LOGICAL_SIZE, IDS_CRFILES_DISPLAY_WORK_AREA,
                        IDS_CRFILES_DISPLAY_SCALE, IDS_CRFILES_DISPLAY_WINDOW_SIZE,
                        IDS_CRFILES_DISPLAY_REFRESH_RATE, IDS_CRFILES_GRAPHICS_REFRESH_RATE}) AddRow(title);
      return;
    }
    for (int title : {IDS_CRFILES_GRAPHICS_DEVICE, IDS_CRFILES_GRAPHICS_DRIVER,
                      IDS_CRFILES_GRAPHICS_RENDERER, IDS_CRFILES_GRAPHICS_GL,
                      IDS_CRFILES_GRAPHICS_SKIA, IDS_CRFILES_GRAPHICS_COMPOSITING,
                      IDS_CRFILES_GRAPHICS_RASTER}) AddRow(title);
#if BUILDFLAG(IS_WIN)
    AddRow(IDS_CRFILES_GRAPHICS_DCOMP);
#endif
    AddRow(IDS_CRFILES_GRAPHICS_RENDERING_MODE);
    auto* note = Label(this, Text(IDS_CRFILES_GRAPHICS_NOTE));
    note->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
  }
  void AddedToWidget() override {
    views::View::AddedToWidget();
    UpdateVisibility();
  }
  void RemovedFromWidget() override {
    timer_.Stop();
    views::View::RemovedFromWidget();
  }
  void VisibilityChanged(views::View* starting_from, bool is_visible) override {
    views::View::VisibilityChanged(starting_from, is_visible);
    UpdateVisibility();
  }
 private:
  void AddRow(int title) {
    titles_.push_back(title);
    auto* label = Label(this, Text(title) + u": " + Text(IDS_CRFILES_GRAPHICS_UNAVAILABLE));
    label->SetSelectable(true);
    label->SetAllowCharacterBreak(true);
    labels_.push_back(label);
  }
  void UpdateVisibility() {
    timer_.Stop();
    if (!IsDrawn() || !GetWidget()) return;
    Refresh();
    timer_.Start(FROM_HERE, base::Seconds(1), this, &SystemInfoDetails::Refresh);
  }
  void Refresh() {
    auto* factory = libcr::VizContextFactory::Get();
    auto* source = source_.view();
    auto* widget = source ? source->GetWidget() : nullptr;
    if (kind_ == SystemInfoKind::kDisplay) {
      RefreshDisplay(widget);
      return;
    }
    auto info = factory && widget
        ? factory->GetGraphicsInfo(widget->GetCompositor()) : std::nullopt;
    std::vector<std::u16string> values(labels_.size(), Text(IDS_CRFILES_GRAPHICS_UNAVAILABLE));
    values.back() = Text(IDS_CRFILES_GRAPHICS_UNKNOWN_MODE);
    if (info) {
      auto value = [](const std::string& text) {
        return text.empty() ? Text(IDS_CRFILES_GRAPHICS_UNAVAILABLE) : base::UTF8ToUTF16(text);
      };
      auto status = [](bool enabled) {
        return Text(enabled ? IDS_CRFILES_GRAPHICS_ENABLED : IDS_CRFILES_GRAPHICS_DISABLED);
      };
      values[0] = value(info->device);
      values[1] = value(info->driver);
      values[2] = value(info->renderer);
      values[3] = value(info->gl_backend);
      values[4] = info->gpu_compositing ? value(info->skia_backend) : Text(IDS_CRFILES_GRAPHICS_SOFTWARE);
      values[5] = info->gpu_compositing ? status(true) : Text(IDS_CRFILES_GRAPHICS_SOFTWARE);
      values[6] = status(info->gpu_rasterization);
      int mode = IDS_CRFILES_GRAPHICS_UNKNOWN_MODE;
      switch (info->rendering_mode) {
        case libcr::VizContextFactory::RenderingMode::kHardware:
          mode = IDS_CRFILES_GRAPHICS_HARDWARE; break;
        case libcr::VizContextFactory::RenderingMode::kSoftware:
          mode = IDS_CRFILES_GRAPHICS_SOFTWARE_RENDERING; break;
        case libcr::VizContextFactory::RenderingMode::kUnknown:
          break;
      }
      values.back() = Text(mode);
#if BUILDFLAG(IS_WIN)
      values[7] = status(info->direct_composition);
#endif
    }
    SetValues(values);
  }
  void RefreshDisplay(views::Widget* widget) {
    std::vector<std::u16string> values(labels_.size(), Text(IDS_CRFILES_GRAPHICS_UNAVAILABLE));
    auto size_text = [](const gfx::Size& size) {
      return base::NumberToString16(size.width()) + u" × " + base::NumberToString16(size.height());
    };
    auto rate_text = [](double rate) {
      return std::isfinite(rate) && rate > 0
          ? base::NumberToString16WithFixedPrecision(rate, 2) + u" Hz"
          : Text(IDS_CRFILES_GRAPHICS_UNAVAILABLE);
    };
    if (widget) {
      values[5] = size_text(widget->GetWindowBoundsInScreen().size()) + u" DIP";
      if (widget->GetCompositor()) values[7] = rate_text(widget->GetCompositor()->refresh_rate());
      if (auto* screen = display::Screen::Get()) {
        const auto display = screen->GetDisplayNearestWindow(widget->GetNativeWindow());
        if (display.is_valid()) {
          if (!display.label().empty()) values[0] = base::UTF8ToUTF16(display.label());
          const auto pixels = display.GetSizeInPixel();
          if (!pixels.IsEmpty()) values[1] = size_text(pixels) + u" px";
          values[2] = size_text(display.bounds().size()) + u" DIP";
          values[3] = size_text(display.work_area().size()) + u" DIP";
          const double scale = display.device_scale_factor();
          if (std::isfinite(scale) && scale > 0)
            values[4] = base::NumberToString16WithFixedPrecision(scale * 100, 1) + u"%";
          values[6] = rate_text(display.display_frequency());
        }
      }
    }
    SetValues(values);
  }
  void SetValues(const std::vector<std::u16string>& values) {
    for (size_t i = 0; i < labels_.size(); ++i) {
      const auto text = Text(titles_[i]) + u": " + values[i];
      if (labels_[i]->GetText() != text) labels_[i]->SetText(text);
    }
  }
  const SystemInfoKind kind_;
  std::vector<int> titles_;
  std::vector<raw_ptr<views::Label>> labels_;
  views::ViewTracker source_;
  base::RepeatingTimer timer_;
};
BEGIN_METADATA(SystemInfoDetails)
END_METADATA
class SystemInfoLink : public views::Link {
  METADATA_HEADER(SystemInfoLink, views::Link)
 public:
  explicit SystemInfoLink(SystemInfoKind kind)
      : views::Link(Text(kind == SystemInfoKind::kGraphics ? IDS_CRFILES_GRAPHICS : IDS_CRFILES_DISPLAY)), kind_(kind) {
    SetFocusBehavior(views::View::FocusBehavior::NEVER);
    SetForceUnderline(false);
    SetHorizontalAlignment(gfx::ALIGN_LEFT);
    SetCallback(base::BindRepeating(&SystemInfoLink::ShowDetails, weak_.GetWeakPtr()));
  }
  ~SystemInfoLink() override { CloseDetails(); }
  void VisibilityChanged(views::View* starting_from, bool is_visible) override {
    views::Link::VisibilityChanged(starting_from, is_visible);
    if (!is_visible) CloseDetails();
  }
  void RemovedFromWidget() override {
    CloseDetails();
    views::Link::RemovedFromWidget();
  }
 private:
  void CloseDetails() {
    if (dialog_) dialog_->SetAnchorView(nullptr);
    if (widget_) { widget_->CloseNow(); widget_.reset(); }
    dialog_.reset();
  }
  void ShowDetails() {
    if (!GetWidget()) return;
    if (widget_ && !widget_->IsClosed() && widget_->IsVisible()) {
      widget_->Activate();
      return;
    }
    CloseDetails();
    // Keep the bubble physically to the right, including in RTL locales where
    // BubbleDialogDelegate mirrors the requested arrow.
    dialog_ = std::make_unique<views::BubbleDialogDelegate>(this,
        base::i18n::IsRTL() ? views::BubbleBorder::RIGHT_CENTER : views::BubbleBorder::LEFT_CENTER);
    dialog_->set_parent_window(GetWidget()->GetNativeView());
    dialog_->SetTitle(std::u16string(GetText()));
    dialog_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    dialog_->SetShowCloseButton(true);
    dialog_->set_close_on_deactivate(true);
    auto contents = std::make_unique<views::View>();
    contents->SetPreferredSize(gfx::Size(480, 320));
    contents->SetLayoutManager(std::make_unique<views::FillLayout>());
    auto* scroll = contents->AddChildView(std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled));
    scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    scroll->SetUseContentsPreferredSize(true);
    scroll->SetContents(std::make_unique<SystemInfoDetails>(this, kind_));
    dialog_->SetContentsView(std::move(contents));
    widget_ = CreateCrFilesBubble(dialog_.get(), base::BindOnce(
        &SystemInfoLink::DetailsClosed, weak_.GetWeakPtr()));
    dialog_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    dialog_->SizeToContents();
    widget_->Show();
  }
  void DetailsClosed(views::Widget::ClosedReason) { CloseDetails(); }
  const SystemInfoKind kind_;
  std::unique_ptr<views::BubbleDialogDelegate> dialog_;
  std::unique_ptr<views::Widget> widget_;
  base::WeakPtrFactory<SystemInfoLink> weak_{this};
};
BEGIN_METADATA(SystemInfoLink)
END_METADATA
std::unique_ptr<views::View> AboutBody(bool large) {
  auto body = std::make_unique<views::View>();
  auto* layout = body->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(0, 12), 24));
  layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* column = body->AddChildView(std::make_unique<views::View>());
  column->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
  layout->SetFlexForView(column, 1);
  auto* description = Label(column, Text(IDS_CRFILES_DESCRIPTION));
  auto* version = Label(column, u"crFiles " CRFILES_VERSION_STRING u" · Chromium " +
      base::ASCIIToUTF16(version_info::GetMajorVersionNumber()));
  auto* build = Label(column, Text(IDS_CRFILES_BUILD_TIME) + u": " + base::UTF8ToUTF16(std::string_view(CRFILES_BUILD_TIME)));
  version->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
  build->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
  if (large) for (auto* label : {description, version, build})
    label->SetFontList(label->font_list().DeriveWithSizeDelta(2));
  auto* project = column->AddChildView(std::make_unique<views::View>());
  project->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4))
      ->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* project_title = Label(project, Text(IDS_CRFILES_PROJECT));
  auto* project_link = project->AddChildView(std::make_unique<views::Link>(u"https://github.com/libcr/crFiles"));
  project_link->SetCallback(base::BindRepeating(&OpenProject)); project_link->SetForceUnderline(false);
  project_link->SetFocusBehavior(views::View::FocusBehavior::NEVER);
  auto* author = column->AddChildView(std::make_unique<views::View>());
  author->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal))
      ->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* author_title = Label(author, Text(IDS_CRFILES_AUTHOR) + u": wuruxu (");
  views::Link* email = author->AddChildView(std::make_unique<AuthorEmailLink>());
  auto* suffix = Label(author, u")");
  for (auto* label : {project_title, author_title, suffix}) {
    label->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
    if (large) label->SetFontList(label->font_list().DeriveWithSizeDelta(2));
  }
  if (large) for (auto* link : {project_link, email}) link->SetFontList(link->font_list().DeriveWithSizeDelta(2));
  auto* system = column->AddChildView(std::make_unique<views::View>());
  system->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4))
      ->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* system_title = Label(system, Text(IDS_CRFILES_SYSTEM_INFO) + u":");
  system_title->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
  auto* links = system->AddChildView(std::make_unique<views::View>());
  links->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8))
      ->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* graphics = links->AddChildView(std::make_unique<SystemInfoLink>(SystemInfoKind::kGraphics));
  auto* display = links->AddChildView(std::make_unique<SystemInfoLink>(SystemInfoKind::kDisplay));
  if (large) {
    system_title->SetFontList(system_title->font_list().DeriveWithSizeDelta(2));
    graphics->SetFontList(graphics->font_list().DeriveWithSizeDelta(2));
    display->SetFontList(display->font_list().DeriveWithSizeDelta(2));
  }
  auto* icon = body->AddChildView(std::make_unique<views::ImageView>(CreateCrFilesIconModel(gfx::Size(115, 115))));
  icon->SetPreferredSize(gfx::Size(156, 115)); icon->SetImageSize(gfx::Size(115, 115));
  icon->SetAccessibleName(u"crFiles");
  return body;
}
class NavigationButton : public views::LabelButton {
  METADATA_HEADER(NavigationButton, views::LabelButton)
 public:
  NavigationButton(PressedCallback callback, std::u16string text) : views::LabelButton(std::move(callback), text) {
    SetHorizontalAlignment(gfx::ALIGN_LEFT); SetPreferredSize(gfx::Size(168, 42));
    SetFocusBehavior(views::View::FocusBehavior::NEVER);
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(9, 14)));
    views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(), 9);
  }
  void Select(bool selected) { selected_ = selected; Update(); }
  void OnThemeChanged() override { views::LabelButton::OnThemeChanged(); Update(); }
 protected:
  void StateChanged(ButtonState old) override { views::LabelButton::StateChanged(old); Update(); }
 private:
  void Update() {
    SetEnabledTextColors(selected_ ? ui::kColorSysOnSecondaryContainer : ui::kColorSysOnSurface);
    SetBackground(selected_ ? views::CreateRoundedRectBackground(ui::kColorSysSecondaryContainer, 9) :
        GetState() == STATE_HOVERED ? views::CreateRoundedRectBackground(ui::kColorSysStateHoverOnSubtle, 9) : nullptr);
  }
  bool selected_ = false;
};
BEGIN_METADATA(NavigationButton)
END_METADATA
class Settings : public views::DialogDelegate, public ui::SelectFileDialog::Listener, public views::ViewObserver, public ui::EventObserver, public views::TextfieldController {
 public:
  Settings() {
    SetTitle(Text(IDS_CRFILES_SETTINGS));
#if BUILDFLAG(IS_MAC)
    // A window-modal dialog becomes a titlebar-less AppKit sheet. Settings
    // uses a regular native window so its close button is always available.
    SetModalType(ui::mojom::ModalType::kNone);
#else
    SetModalType(ui::mojom::ModalType::kWindow);
#endif
    SetShowCloseButton(true);
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone)); set_use_custom_frame(false);
    SetAppIcon(CreateCrFilesAppIconModel()); SetShowIcon(false);
    auto contents = std::make_unique<views::View>();
    contents->SetPreferredSize(gfx::Size(820, 560));
    auto* layout = contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
    auto* navigation = contents->AddChildView(std::make_unique<views::View>());
    navigation->SetPreferredSize(gfx::Size(196, 0));
    navigation->SetBackground(views::CreateSolidBackground(ui::kColorSysSurface2));
    navigation->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(18, 14), 6));
    constexpr std::array titles = {IDS_CRFILES_GENERAL, IDS_CRFILES_INTERFACE, IDS_CRFILES_NEARBY_SETTINGS,
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
        IDS_CRFILES_QUICK_SHARE,
#endif
        IDS_CRFILES_HISTORY, IDS_CRFILES_ABOUT};
    for (size_t i = 0; i < titles.size(); ++i)
      navigation_[i] = navigation->AddChildView(std::make_unique<NavigationButton>(
          base::BindRepeating(&Settings::SelectPage, base::Unretained(this), i), Text(titles[i])));
    auto* separator = contents->AddChildView(std::make_unique<views::Separator>());
    separator->SetOrientation(views::Separator::Orientation::kVertical);
    auto* right = contents->AddChildView(std::make_unique<views::View>());
    layout->SetFlexForView(right, 1);
    auto* right_layout = right->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical));
    auto* scroll = right->AddChildView(std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled));
    scroll->SetBackgroundColor(ui::kColorPrimaryBackground);
    scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    scroll->SetUseContentsPreferredSize(true); scroll->ClipHeightTo(0, 508);
    right_layout->SetFlexForView(scroll, 1);
    auto host = std::make_unique<views::View>(); host->SetLayoutManager(std::make_unique<views::FillLayout>());
    pages_[0] = host->AddChildView(GeneralPage());
    pages_[1] = host->AddChildView(InterfacePage());
    pages_[2] = host->AddChildView(NearbyPage());
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    pages_[3] = host->AddChildView(QuickSharePage());
#endif
    pages_[pages_.size() - 2] = host->AddChildView(HistoryPage());
    auto about = Page(IDS_CRFILES_ABOUT); about->AddChildView(AboutBody(false));
    pages_[pages_.size() - 1] = host->AddChildView(std::move(about));
    scroll->SetContents(std::move(host));
    auto* footer = right->AddChildView(std::make_unique<views::View>());
    footer->SetPreferredSize(gfx::Size(0, 52));
    auto* footer_layout = footer->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(8, 16), 8));
    error_ = Label(footer, Text(IDS_CRFILES_CONFIG_ERROR)); error_->SetVisible(false);
    auto* spacer = footer->AddChildView(std::make_unique<views::View>()); footer_layout->SetFlexForView(spacer, 1);
    footer->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&Settings::Close, base::Unretained(this)), Text(IDS_CRFILES_CLOSE)));
    SelectPage(0);
    contents_observation_.Observe(contents.get());
    SetContentsView(std::move(contents));
    SetInitiallyFocusedView(nullptr);
    sharing_timer_.Start(FROM_HERE, base::Milliseconds(250), this, &Settings::RefreshSharing);
  }
  views::View* GetInitiallyFocusedView() override { return nullptr; }
  bool OnCloseRequested(views::Widget::ClosedReason reason) override {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    SaveQuickText();
    if (quick_text_pending_) { quick_close_pending_ = true; return false; }
    if (quick_text_failed_) return false;
#endif
    return views::DialogDelegate::OnCloseRequested(reason);
  }
  void WindowClosing() override {
    StopSharingUpdates();
    CloseAvatars();
    // The delegate outlives the widget's content tree. Drop every reference
    // before the controls are destroyed, including references in containers.
    pages_.fill(nullptr);
    navigation_.fill(nullptr);
    navigation_styles_.fill(nullptr);
    checks_.fill(nullptr);
    themes_.clear();
    language_ = nullptr;
    retention_ = nullptr;
    name_ = nullptr;
    directory_ = nullptr;
    error_ = nullptr;
    auto_hide_toolbar_ = nullptr;
    windows_titlebar_style_ = nullptr;
    system_tray_ = nullptr;
    animate_folders_ = nullptr;
    hover_image_preview_ = nullptr;
    single_click_folders_ = nullptr;
    discovery_ = nullptr;
    auto_accept_ = nullptr;
    apply_ = nullptr;
    avatar_button_ = nullptr;
    folder_ = nullptr;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    quick_name_ = nullptr; quick_directory_ = nullptr; quick_enabled_ = nullptr;
    quick_folder_ = nullptr;
#if BUILDFLAG(IS_LINUX)
    quick_interface_ = nullptr; quick_direct_ = nullptr; quick_interface_refresh_ = nullptr;
#endif
#endif
    views::DialogDelegate::WindowClosing();
  }
  void OnViewIsDeleting(views::View*) override {
    // The content tree can die before its delegate. Never let the polling
    // timer retain access to children during or after that destruction.
    StopSharingUpdates();
    contents_observation_.Reset();
  }
  ~Settings() override {
    StopSharingUpdates(); CloseAvatars();
  }
 private:
  void StopSharingUpdates() {
    closing_ = true;
    avatar_decode_cancel_->store(true);
    weak_.InvalidateWeakPtrs();
    avatar_monitor_.reset();
    sharing_timer_.Stop();
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    quick_save_timer_.Stop();
#endif
    if (folder_dialog_) { folder_dialog_->ListenerDestroyed(); folder_dialog_.reset(); }
  }
  std::unique_ptr<views::View> GeneralPage() {
    auto page = Page(IDS_CRFILES_GENERAL);
    auto* language_row = page->AddChildView(std::make_unique<views::View>());
    language_row->SetPreferredSize(gfx::Size(0, 52));
    auto* language_layout = language_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal));
    language_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    language_layout->SetFlexForView(Label(language_row, Text(IDS_CRFILES_LANGUAGE)), 1);
    std::vector<ui::SimpleComboboxModel::Item> languages;
    languages.emplace_back(Text(IDS_CRFILES_SYSTEM_LANGUAGE));
    constexpr std::array<std::u16string_view, 14> language_names = {
        u"English (United States)", u"日本語", u"한국어", u"简体中文", u"繁體中文（香港）", u"繁體中文（台灣）",
        u"العربية", u"Français", u"Français (Canada)", u"Русский", u"Español",
        u"Español (Latinoamérica)", u"Português (Brasil)", u"Português (Portugal)"};
    for (const auto name : language_names) languages.emplace_back(std::u16string(name));
    language_ = language_row->AddChildView(std::make_unique<views::Combobox>(std::make_unique<ui::SimpleComboboxModel>(std::move(languages))));
    language_->SetPreferredSize(gfx::Size(260, 36));
    language_->SetSelectedIndex(0);
    const auto language = GetCrFilesLanguageSetting();
    for (size_t i = 0; i < kLanguages.size(); ++i) if (language == kLanguages[i]) language_->SetSelectedIndex(i);
    language_->SetAccessibleName(Text(IDS_CRFILES_LANGUAGE));
    language_->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    language_->SetCallback(base::BindRepeating(&Settings::LanguageChanged, base::Unretained(this)));
    auto* hint = Label(page.get(), Text(IDS_CRFILES_LANGUAGE_RESTART));
    hint->SetMultiLine(true); hint->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
    auto* theme_row = page->AddChildView(std::make_unique<views::View>());
    theme_row->SetPreferredSize(gfx::Size(0, 66));
    theme_row->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(14, 0, 0, 0)));
    auto* theme_layout = theme_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal));
    theme_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    theme_layout->SetFlexForView(Label(theme_row, Text(IDS_CRFILES_STYLE)), 1);
    auto* themes = theme_row->AddChildView(std::make_unique<views::View>());
    auto* toggle_layout = themes->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(2, 10), 4));
    toggle_layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    constexpr std::array modes = {CrFilesThemeMode::kSystem, CrFilesThemeMode::kLight, CrFilesThemeMode::kDark};
    constexpr std::array labels = {IDS_CRFILES_THEME_SETTING_SYSTEM, IDS_CRFILES_THEME_SETTING_LIGHT, IDS_CRFILES_THEME_SETTING_DARK};
    for (size_t i = 0; i < modes.size(); ++i) {
      auto mode = modes[i];
      auto* button = themes->AddChildView(std::make_unique<libcr::ThemeToggleButton>(base::BindRepeating(&Settings::ChangeTheme, base::Unretained(this), mode), mode, Text(labels[i]), GetCrFilesThemeMode() == mode, libcr::ThemeToggleStyle::kSettings));
      button->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      themes_.push_back(button);
    }
    auto* spacing = page->AddChildView(std::make_unique<views::View>());
    spacing->SetPreferredSize(gfx::Size(0, 2));
    const auto preferences = GetFileViewPreferences();
    auto_hide_toolbar_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_AUTO_HIDE_TOOLBAR)));
    animate_folders_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_ANIMATE_FOLDERS)));
    hover_image_preview_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_HOVER_IMAGE_PREVIEW)));
    single_click_folders_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_SINGLE_CLICK_FOLDERS)));
    auto_hide_toolbar_->SetChecked(preferences.auto_hide_toolbar);
    animate_folders_->SetChecked(preferences.animate_folders);
    hover_image_preview_->SetChecked(preferences.hover_image_preview);
    single_click_folders_->SetChecked(preferences.single_click_folders);
    for (auto* checkbox : {auto_hide_toolbar_.get(), animate_folders_.get(), hover_image_preview_.get(), single_click_folders_.get()}) {
      checkbox->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      checkbox->SetCallback(base::BindRepeating(&Settings::FileViewChanged, base::Unretained(this)));
    }
    return page;
  }
  std::unique_ptr<views::View> InterfacePage() {
    auto page = Page(IDS_CRFILES_INTERFACE);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    system_tray_ = page->AddChildView(std::make_unique<views::Checkbox>(
        Text(IDS_CRFILES_ENABLE_SYSTEM_TRAY)));
    system_tray_->SetChecked(GetSystemTrayEnabled());
    system_tray_->SetCallback(base::BindRepeating(
        &Settings::SystemTrayChanged, weak_.GetWeakPtr()));
#endif
#if BUILDFLAG(IS_MAC)
    windows_titlebar_style_ = page->AddChildView(std::make_unique<views::Checkbox>(
        Text(IDS_CRFILES_WINDOWS_TITLEBAR_STYLE)));
    windows_titlebar_style_->SetChecked(GetWindowsTitlebarStyleEnabled());
    windows_titlebar_style_->SetCallback(base::BindRepeating(
        &Settings::WindowsTitlebarStyleChanged, weak_.GetWeakPtr()));
    Label(page.get(), Text(IDS_CRFILES_TITLEBAR_RESTART));
#endif
    Label(page.get(), Text(IDS_CRFILES_NAVIGATION_STYLE));
    constexpr std::array labels = {IDS_CRFILES_NAVIGATION_MINIMAL, IDS_CRFILES_NAVIGATION_UNDERLINE, IDS_CRFILES_NAVIGATION_RAISED};
    const auto selected = GetNavigationStyle();
    for (size_t i = 0; i < labels.size(); ++i) {
      auto* radio = page->AddChildView(std::make_unique<views::RadioButton>(Text(labels[i]), 1));
      navigation_styles_[i] = radio;
      radio->SetPreferredSize(gfx::Size(0, 44));
      radio->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      radio->SetChecked(static_cast<NavigationStyle>(i) == selected);
      radio->SetCallback(base::BindRepeating(&Settings::NavigationStyleChanged, base::Unretained(this), static_cast<NavigationStyle>(i)));
    }
    return page;
  }
  void NavigationStyleChanged(NavigationStyle style) {
    const bool saved = SetNavigationStyle(style);
    error_->SetVisible(!saved);
    const auto selected = GetNavigationStyle();
    for (size_t i = 0; i < navigation_styles_.size(); ++i)
      navigation_styles_[i]->SetChecked(static_cast<NavigationStyle>(i) == selected);
    if (saved) ApplyCrFilesViewSettings();
  }
  void WindowsTitlebarStyleChanged() {
    const bool saved = SetWindowsTitlebarStyleEnabled(
        windows_titlebar_style_->GetChecked());
    error_->SetVisible(!saved);
    if (!saved)
      windows_titlebar_style_->SetChecked(GetWindowsTitlebarStyleEnabled());
  }
  void SystemTrayChanged() {
    const bool saved = SetSystemTrayEnabled(system_tray_->GetChecked());
    error_->SetVisible(!saved);
    if (saved) ApplyCrFilesTraySettings();
    else system_tray_->SetChecked(GetSystemTrayEnabled());
  }
  void FileViewChanged() {
    const bool saved = SetFileViewPreferences({auto_hide_toolbar_->GetChecked(), animate_folders_->GetChecked(), hover_image_preview_->GetChecked(), single_click_folders_->GetChecked()});
    error_->SetVisible(!saved);
    if (!saved) {
      const auto preferences = GetFileViewPreferences();
      auto_hide_toolbar_->SetChecked(preferences.auto_hide_toolbar);
      animate_folders_->SetChecked(preferences.animate_folders);
      hover_image_preview_->SetChecked(preferences.hover_image_preview);
      single_click_folders_->SetChecked(preferences.single_click_folders);
    }
    ApplyCrFilesViewSettings();
  }
  std::unique_ptr<views::View> NearbyPage() {
    auto apply = std::make_unique<views::MdTextButton>(
        base::BindRepeating(&Settings::ApplyNearby, base::Unretained(this)), Text(IDS_CRFILES_APPLY));
    apply_ = apply.get();
    apply_->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    auto page = Page(IDS_CRFILES_NEARBY_SETTINGS, std::move(apply));
    const auto prefs = GetNearbySharingPreferences(); avatar_ = prefs.avatar;
    custom_avatar_ = prefs.custom_avatar; custom_avatar_image_ = DecodeCustomAvatar(custom_avatar_);
    auto* avatar_row = page->AddChildView(std::make_unique<views::View>());
    auto* avatar_layout = avatar_row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
    avatar_layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    avatar_button_ = avatar_row->AddChildView(std::make_unique<views::ImageButton>(base::BindRepeating(&Settings::ShowAvatars, base::Unretained(this))));
    avatar_button_->SetPreferredSize(gfx::Size(92, 92));
    avatar_button_->SetImageHorizontalAlignment(views::ImageButton::ALIGN_CENTER);
    avatar_button_->SetImageVerticalAlignment(views::ImageButton::ALIGN_MIDDLE);
    avatar_button_->SetFocusBehavior(views::View::FocusBehavior::NEVER); avatar_button_->SetTooltipText(Text(IDS_CRFILES_AVATAR));
    avatar_button_->SetAccessibleName(Text(IDS_CRFILES_AVATAR)); UpdateAvatar();
    name_ = SharingField(page.get(), IDS_CRFILES_DEVICE_NAME, base::UTF8ToUTF16(prefs.name));
    directory_ = SharingField(page.get(), IDS_CRFILES_RECEIVE_DIRECTORY, prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory().AsUTF16Unsafe() : base::UTF8ToUTF16(prefs.directory));
    discovery_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_DISCOVERY)));
    discovery_->SetChecked(prefs.discovery_enabled);
    auto_accept_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_AUTO_ACCEPT)));
    auto_accept_->SetChecked(prefs.auto_accept_trusted);
    auto_accept_->SetMultiLine(true);
    return page;
  }
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  std::unique_ptr<views::View> QuickSharePage() {
    auto page = Page(IDS_CRFILES_QUICK_SHARE);
    const auto prefs = GetQuickSharePreferences();
    quick_enabled_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_QUICK_SHARE_ENABLE)));
    quick_enabled_->SetChecked(prefs.enabled);
    quick_enabled_->SetCallback(base::BindRepeating(&Settings::SaveQuickEnabled, base::Unretained(this)));
    Label(page.get(), Text(IDS_CRFILES_QUICK_SHARE_RECEIVE_HINT));
    quick_name_ = SharingField(page.get(), IDS_CRFILES_DEVICE_NAME, base::UTF8ToUTF16(prefs.name));
    quick_directory_ = SharingField(page.get(), IDS_CRFILES_RECEIVE_DIRECTORY,
        prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory().AsUTF16Unsafe() : base::UTF8ToUTF16(prefs.directory), true);
    quick_name_->set_controller(this);
    quick_directory_->set_controller(this);
#if BUILDFLAG(IS_LINUX)
    Label(page.get(), Text(IDS_CRFILES_QS_ADVANCED));
    quick_direct_ = page->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_QS_DIRECT_ENABLE)));
    quick_direct_->SetChecked(prefs.direct_enabled || base::CommandLine::ForCurrentProcess()->HasSwitch("quick-share-direct-go"));
    quick_direct_->SetCallback(base::BindRepeating(&Settings::SaveQuickDirect, base::Unretained(this)));
    auto* interface_header = page->AddChildView(std::make_unique<views::View>());
    auto* interface_layout = interface_header->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    interface_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    Label(interface_header, Text(IDS_CRFILES_QS_INTERFACE));
    quick_interface_refresh_ = interface_header->AddChildView(std::make_unique<views::ImageButton>(
        base::BindRepeating(&Settings::RefreshQuickInterfaces, base::Unretained(this))));
    quick_interface_refresh_->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kRefreshIcon, ui::kColorSysOnSurface, 20));
    quick_interface_refresh_->SetBorder(views::CreateEmptyBorder(gfx::Insets(8)));
    quick_interface_refresh_->SetTooltipText(Text(IDS_CRFILES_REFRESH));
    quick_interface_refresh_->SetAccessibleName(Text(IDS_CRFILES_REFRESH));
    quick_interface_refresh_->SetAnimateOnStateChange(true);
    quick_interface_refresh_->SetAnimationDuration(base::Milliseconds(160));
    quick_interface_refresh_->SetHasInkDropActionOnClick(true);
    views::InstallRoundRectHighlightPathGenerator(quick_interface_refresh_, gfx::Insets(), 8);
    auto* refresh_ink = views::InkDrop::Get(quick_interface_refresh_);
    refresh_ink->SetMode(views::InkDropHost::InkDropMode::ON);
    refresh_ink->SetBaseColor(ui::kColorSysPrimary);
    refresh_ink->SetVisibleOpacity(0.16f);
    refresh_ink->SetHighlightOpacity(0.09f);
    refresh_ink->GetInkDrop()->SetShowHighlightOnHover(true);
    refresh_ink->GetInkDrop()->SetShowHighlightOnFocus(true);
    const auto* command = base::CommandLine::ForCurrentProcess();
    quick_interface_override_ = command->HasSwitch("quick-share-upgrade-interface");
    quick_selected_interface_ = quick_interface_override_ ? command->GetSwitchValueASCII("quick-share-upgrade-interface") : prefs.upgrade_interface;
    quick_interface_names_ = {""};
    std::vector<ui::SimpleComboboxModel::Item> items;
    items.emplace_back(Text(IDS_CRFILES_QS_AUTO_INTERFACE));
    if (!quick_selected_interface_.empty()) {
      quick_interface_names_.push_back(quick_selected_interface_);
      items.emplace_back(base::UTF8ToUTF16(quick_selected_interface_));
    }
    quick_interface_ = page->AddChildView(std::make_unique<views::Combobox>(std::make_unique<ui::SimpleComboboxModel>(std::move(items))));
    quick_interface_->SetAccessibleName(Text(IDS_CRFILES_QS_INTERFACE));
    quick_interface_->SetSelectedIndex(quick_selected_interface_.empty() ? 0 : 1);
    quick_interface_->SetCallback(base::BindRepeating(&Settings::SaveQuickInterface, base::Unretained(this)));
    Label(page.get(), Text(IDS_CRFILES_QS_INTERFACE_HINT));
    if (quick_interface_override_ || command->HasSwitch("quick-share-direct-go"))
      Label(page.get(), Text(IDS_CRFILES_QS_INTERFACE_OVERRIDE));
    RefreshQuickInterfaces();
#endif
    return page;
  }
#if BUILDFLAG(IS_LINUX)
  void SaveQuickInterface() {
    if (closing_ || quick_interface_override_ || quick_interfaces_pending_) return;
    auto prefs = GetQuickSharePreferences();
    const auto previous = prefs.upgrade_interface;
    prefs.upgrade_interface = quick_interface_names_[quick_interface_->GetSelectedIndex().value_or(0)];
    const bool saved = !IsCrFilesQuickShareBusy() && SetQuickSharePreferences(prefs);
    if (!saved) {
      const auto found = std::ranges::find(quick_interface_names_, previous);
      if (found != quick_interface_names_.end()) quick_interface_->SetSelectedIndex(found - quick_interface_names_.begin());
    }
    error_->SetVisible(!saved);
    RefreshSharing();
  }
  void SaveQuickDirect() {
    if (closing_) return;
    auto prefs = GetQuickSharePreferences();
    if (IsCrFilesQuickShareBusy() || base::CommandLine::ForCurrentProcess()->HasSwitch("quick-share-direct-go")) {
      quick_direct_->SetChecked(prefs.direct_enabled || base::CommandLine::ForCurrentProcess()->HasSwitch("quick-share-direct-go"));
      RefreshSharing();
      return;
    }
    prefs.direct_enabled = quick_direct_->GetChecked();
    const bool saved = SetQuickSharePreferences(prefs);
    if (!saved) quick_direct_->SetChecked(GetQuickSharePreferences().direct_enabled);
    error_->SetVisible(!saved);
    RefreshSharing();
  }
  void RefreshQuickInterfaces() {
    if (closing_ || quick_interfaces_pending_ || IsCrFilesQuickShareBusy()) return;
    quick_interfaces_pending_ = true;
    quick_interface_refresh_->SetEnabled(false);
    quick_interface_->SetEnabled(false);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&crquickshare::GetLinuxWifiDirectInterfaces),
        base::BindOnce(&Settings::QuickInterfacesReady, weak_.GetWeakPtr()));
  }
  void QuickInterfacesReady(std::vector<crquickshare::WifiDirectInterface> interfaces) {
    if (closing_ || !quick_interface_) return;
    const auto selected = quick_interface_names_[quick_interface_->GetSelectedIndex().value_or(0)];
    quick_interface_names_ = {""};
    std::vector<ui::SimpleComboboxModel::Item> items;
    const bool available = std::ranges::any_of(interfaces, [](const auto& item) { return item.available; });
    items.emplace_back(Text(IDS_CRFILES_QS_AUTO_INTERFACE) +
        (available ? std::u16string() : u" — " + Text(IDS_CRFILES_QS_INTERFACE_UNAVAILABLE)));
    size_t selection = 0;
    for (const auto& item : interfaces) {
      quick_interface_names_.push_back(item.name);
      const int status = item.mode == 3 ? IDS_CRFILES_QS_INTERFACE_AP :
          !item.available ? IDS_CRFILES_QS_INTERFACE_UNAVAILABLE :
          item.connected ? IDS_CRFILES_QS_INTERFACE_CONNECTED : IDS_CRFILES_QS_INTERFACE_AVAILABLE;
      items.emplace_back(base::UTF8ToUTF16(item.name) + u" — " + Text(status));
      if (item.name == selected) selection = items.size() - 1;
    }
    if (!selected.empty() && !selection) {
      selection = items.size();
      quick_interface_names_.push_back(selected);
      items.emplace_back(base::UTF8ToUTF16(selected) + u" — " + Text(IDS_CRFILES_QS_INTERFACE_UNAVAILABLE));
    }
    quick_interface_->SetOwnedModel(std::make_unique<ui::SimpleComboboxModel>(std::move(items)));
    quick_interface_->SetSelectedIndex(selection);
    quick_interfaces_loaded_ = true;
    quick_interfaces_pending_ = false;
    RefreshSharing();
  }
#endif
  void BrowseQuickFolder() {
    if (closing_ || folder_dialog_ || IsCrFilesQuickShareBusy()) return;
    choosing_quick_folder_ = true; choosing_avatar_ = false;
    folder_dialog_ = ui::SelectFileDialog::Create(this, nullptr);
    folder_dialog_->SelectFile(ui::SelectFileDialog::SELECT_FOLDER, Text(IDS_CRFILES_RECEIVE_DIRECTORY),
        base::FilePath::FromUTF16Unsafe(quick_directory_->GetText()), nullptr, 0, {}, GetWidget()->GetNativeWindow(), nullptr);
  }
  void SaveQuickEnabled() {
    if (closing_) return;
    auto prefs = GetQuickSharePreferences();
    prefs.enabled = quick_enabled_->GetChecked();
    const bool saved = !IsCrFilesQuickShareBusy() && SetQuickSharePreferences(prefs);
    if (!saved) quick_enabled_->SetChecked(GetQuickSharePreferences().enabled);
    error_->SetVisible(!saved);
    RefreshSharing();
  }
  void ContentsChanged(views::Textfield* sender, const std::u16string&) override {
    if (closing_ || (sender != quick_name_ && sender != quick_directory_)) return;
    ++quick_edit_generation_;
    quick_save_timer_.Start(FROM_HERE, base::Milliseconds(600), this, &Settings::SaveQuickText);
  }
  void SaveQuickText() {
    quick_save_timer_.Stop();
    if (closing_ || quick_text_pending_) return;
    quick_text_failed_ = false;
    const auto prefs = GetQuickSharePreferences();
    const auto name = base::UTF16ToUTF8(quick_name_->GetText());
    const auto directory_text = base::UTF16ToUTF8(quick_directory_->GetText());
    const auto previous_directory = prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory().AsUTF8Unsafe() : prefs.directory;
    if (name == prefs.name && directory_text == previous_directory) return;
    if (IsCrFilesQuickShareBusy()) {
      quick_text_failed_ = true;
      error_->SetVisible(true);
      quick_save_timer_.Start(FROM_HERE, base::Milliseconds(600), this, &Settings::SaveQuickText);
      return;
    }
    const auto directory = base::FilePath::FromUTF8Unsafe(directory_text);
    if (!NearbySharing::IsValidConfiguration(name, directory)) { quick_text_failed_ = true; error_->SetVisible(true); return; }
    quick_text_pending_ = true;
    auto save = base::BindOnce(
        [](base::WeakPtr<Settings> view, uint64_t generation, std::string name, std::string directory, bool allowed) {
          if (!view || view->closing_) return;
          view->quick_text_pending_ = false;
          if (generation != view->quick_edit_generation_) {
            view->SaveQuickText();
            return;
          }
          // Merge with current preferences so an asynchronous directory check
          // cannot overwrite switches or the adapter selected in the meantime.
          auto current = GetQuickSharePreferences();
          current.name = std::move(name);
          current.directory = std::move(directory);
          const bool valid = allowed && !IsCrFilesQuickShareBusy() && SetQuickSharePreferences(current);
          view->quick_text_failed_ = !valid;
          view->error_->SetVisible(!valid);
          view->RefreshSharing();
          if (std::exchange(view->quick_close_pending_, false) && valid)
            view->GetWidget()->Close();
        }, weak_.GetWeakPtr(), quick_edit_generation_, name, directory_text);
    if (directory_text == previous_directory) std::move(save).Run(true);
    else EnsureDirectoryAccess(GetWidget(), directory, true, std::move(save));
  }
#endif
  views::Textfield* SharingField(views::View* page, int id, std::u16string value, [[maybe_unused]] bool quick_share = false) {
    auto* row = page->AddChildView(std::make_unique<views::View>());
    auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    auto* label = Label(row, Text(id));
    const int label_width = std::max(views::Label(Text(IDS_CRFILES_DEVICE_NAME)).GetPreferredSize({}).width(),
        views::Label(Text(IDS_CRFILES_RECEIVE_DIRECTORY)).GetPreferredSize({}).width());
    label->SetPreferredSize(gfx::Size(label_width, label->GetPreferredSize({}).height()));
    auto* input = row->AddChildView(std::make_unique<views::View>());
    auto* input_layout = input->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
    input_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    layout->SetFlexForView(input, 1);
    auto* field = input->AddChildView(std::make_unique<views::Textfield>());
    input_layout->SetFlexForView(field, 1); field->SetText(value); field->SetAccessibleName(Text(id)); field->RemoveHoverEffect();
    if (id == IDS_CRFILES_RECEIVE_DIRECTORY) {
      input->SetBorder(views::CreateRoundedRectBorder(1, 8, ui::kColorSysNeutralOutline));
      input->SetBackground(views::CreateRoundedRectBackground(ui::kColorTextfieldBackground, 8));
      field->SetBorder(views::CreateEmptyBorder(gfx::Insets(8))); field->SetBackgroundEnabled(false);
      auto browse = base::BindRepeating(&Settings::BrowseFolder, base::Unretained(this));
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      if (quick_share) browse = base::BindRepeating(&Settings::BrowseQuickFolder, base::Unretained(this));
#endif
      auto* button = input->AddChildView(views::ImageButton::CreateIconButton(std::move(browse), vector_icons::kFolderFilledIcon, Text(IDS_CRFILES_RECEIVE_DIRECTORY)));
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      if (quick_share) quick_folder_ = button;
      else
#endif
        folder_ = button;
    }
    return field;
  }
  ui::ImageModel AvatarImage(int index, int edge) {
    static constexpr std::array ids = {IDR_CRFILES_AVATAR_0, IDR_CRFILES_AVATAR_1, IDR_CRFILES_AVATAR_2, IDR_CRFILES_AVATAR_3, IDR_CRFILES_AVATAR_4, IDR_CRFILES_AVATAR_5, IDR_CRFILES_AVATAR_6, IDR_CRFILES_AVATAR_7, IDR_CRFILES_AVATAR_8};
    // These PNGs are BINDATA, not scale-tagged image resources. Decode once,
    // matching crPhotos, then crop each requested avatar to a circle.
    static const base::NoDestructor<std::array<gfx::ImageSkia, 9>> images([] {
      std::array<gfx::ImageSkia, 9> result;
      for (size_t i = 0; i < ids.size(); ++i) {
        const auto bytes = ui::ResourceBundle::GetSharedInstance().GetRawDataResource(ids[i]);
        result[i] = gfx::ImageSkia::CreateFrom1xBitmap(gfx::PNGCodec::Decode(base::as_byte_span(bytes)));
      }
      return result;
    }());
    const auto& image = (*images)[std::clamp(index, 0, 8)];
    if (image.isNull()) return CreateCrFilesIconModel(gfx::Size(edge, edge));
    return ui::ImageModel::FromImageSkia(gfx::ImageSkiaOperations::CreateCroppedCenteredRoundRectImage(gfx::Size(edge, edge), edge / 2, image));
  }
  gfx::ImageSkia DecodeCustomAvatar(const std::string& encoded) {
    std::string pixels;
    if (encoded.size() != 49152 || !base::Base64Decode(encoded, &pixels) || pixels.size() != 96 * 96 * 4) return {};
    SkBitmap bitmap;
    const auto info = SkImageInfo::Make(96, 96, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!bitmap.tryAllocN32Pixels(96, 96) || !bitmap.writePixels(SkPixmap(info, pixels.data(), 96 * 4))) return {};
    bitmap.setImmutable(); return gfx::ImageSkia::CreateFrom1xBitmap(bitmap);
  }
  void UpdateAvatar() {
    avatar_button_->SetImageModel(views::Button::STATE_NORMAL, custom_avatar_image_.isNull() ? AvatarImage(avatar_, 80) :
        ui::ImageModel::FromImageSkia(gfx::ImageSkiaOperations::CreateCroppedCenteredRoundRectImage(gfx::Size(80, 80), 40, custom_avatar_image_)));
  }
  void CloseAvatars() {
    avatar_monitor_.reset();
    if (avatar_dialog_) avatar_dialog_->SetAnchorView(nullptr);
    if (avatar_widget_) { avatar_widget_->CloseNow(); avatar_widget_.reset(); }
    avatar_dialog_.reset();
  }
  void ShowAvatars() {
    if (closing_ || IsCrFilesSharingBusy() || !GetWidget()) return;
    if (avatar_widget_ && !avatar_widget_->IsClosed() && avatar_widget_->IsVisible()) { avatar_widget_->Close(); return; }
    CloseAvatars();
    avatar_dialog_ = std::make_unique<views::BubbleDialogDelegate>(avatar_button_, views::BubbleBorder::TOP_CENTER);
    avatar_dialog_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    avatar_dialog_->SetShowCloseButton(false);
    avatar_dialog_->set_close_on_deactivate(true);
    avatar_dialog_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    avatar_dialog_->set_margins(gfx::Insets(8));
    auto choices = std::make_unique<views::View>();
    choices->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
    views::View* row = nullptr;
    for (int index = 0; index <= NearbySharing::kAvatarCount; ++index) {
      if (index % 4 == 0) {
        row = choices->AddChildView(std::make_unique<views::View>());
        row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
      }
      auto* button = row->AddChildView(std::make_unique<views::ImageButton>(base::BindRepeating(&Settings::ChooseAvatar, base::Unretained(this), index)));
      button->SetPreferredSize(gfx::Size(60, 60));
      button->SetImageHorizontalAlignment(views::ImageButton::ALIGN_CENTER);
      button->SetImageVerticalAlignment(views::ImageButton::ALIGN_MIDDLE);
      button->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      const bool custom = index == NearbySharing::kAvatarCount;
      button->SetImageModel(views::Button::STATE_NORMAL, custom ?
          ui::ImageModel::FromVectorIcon(vector_icons::kAdd2Icon, ui::kColorSysOnSurface, 48) : AvatarImage(index, 48));
      if (custom) for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
        button->SetImageModel(state, ui::ImageModel::FromVectorIcon(vector_icons::kAdd2Icon, ui::kColorSysPrimary, 48));
      button->SetAccessibleName(custom ? Text(IDS_CRFILES_AVATAR) : Text(IDS_CRFILES_AVATAR) + u" " + base::NumberToString16(index + 1));
      button->SetTooltipText(Text(IDS_CRFILES_AVATAR));
    }
    avatar_dialog_->SetContentsView(std::move(choices));
    avatar_widget_ = CreateCrFilesBubble(avatar_dialog_.get(),
        base::BindOnce(&Settings::AvatarPickerClosed, weak_.GetWeakPtr()));
    avatar_dialog_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    auto* border = avatar_dialog_->GetBubbleFrameView()->bubble_border();
    border->set_draw_border_stroke(true); border->set_md_shadow_elevation(6);
    avatar_dialog_->SizeToContents(); avatar_widget_->Show();
    avatar_monitor_ = views::EventMonitor::CreateApplicationMonitor(this, GetWidget()->GetNativeWindow(), {ui::EventType::kMousePressed});
  }
  void AvatarPickerClosed(views::Widget::ClosedReason) {
    CloseAvatars();
  }
  void OnEvent(const ui::Event& event) override {
    if (event.type() != ui::EventType::kMousePressed || !avatar_monitor_ || !avatar_widget_ ||
        avatar_widget_->IsClosed() || !avatar_widget_->IsVisible()) return;
    const auto point = avatar_monitor_->GetLastMouseLocation();
    if (!IsInsideCrFilesBubble(point) && !avatar_button_->GetBoundsInScreen().Contains(point))
      avatar_widget_->Close();
  }
  void CancelAvatarDecode() {
    avatar_decode_cancel_->store(true);
    avatar_decode_cancel_ = std::make_shared<std::atomic_bool>(false);
    ++avatar_decode_generation_;
  }
  void ChooseAvatar(int index) {
    if (closing_ || IsCrFilesSharingBusy()) return;
    if (avatar_widget_) avatar_widget_->Close();
    CancelAvatarDecode();
    if (index == NearbySharing::kAvatarCount) {
      if (folder_dialog_ || !GetWidget()) return;
      choosing_avatar_ = true;
      folder_dialog_ = ui::SelectFileDialog::Create(this, nullptr);
      ui::SelectFileDialog::FileTypeInfo types;
      types.extensions = {{FILE_PATH_LITERAL("png"), FILE_PATH_LITERAL("jpg"), FILE_PATH_LITERAL("jpeg"), FILE_PATH_LITERAL("webp")}};
      types.include_all_files = false;
      folder_dialog_->SelectFile(ui::SelectFileDialog::SELECT_OPEN_FILE, Text(IDS_CRFILES_AVATAR), {}, &types, 1, {}, GetWidget()->GetNativeWindow(), nullptr);
      return;
    }
    custom_avatar_.clear(); custom_avatar_image_ = {};
    avatar_ = index; UpdateAvatar(); RefreshSharing();
  }
  void BrowseFolder() {
    if (closing_ || folder_dialog_ || IsCrFilesSharingBusy()) return;
    choosing_avatar_ = false;
    folder_dialog_ = ui::SelectFileDialog::Create(this, nullptr);
    folder_dialog_->SelectFile(ui::SelectFileDialog::SELECT_FOLDER, Text(IDS_CRFILES_RECEIVE_DIRECTORY),
        base::FilePath::FromUTF16Unsafe(directory_->GetText()), nullptr, 0, {}, GetWidget()->GetNativeWindow(), nullptr);
  }
  void FileSelected(const ui::SelectedFileInfo& file, int) override {
    RememberFileAccess(file.path());
    folder_dialog_.reset();
    if (closing_) return;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    if (std::exchange(choosing_quick_folder_, false)) {
      if (IsCrFilesQuickShareBusy()) return;
      quick_directory_->SetText(file.path().AsUTF16Unsafe()); ++quick_edit_generation_; SaveQuickText(); return;
    }
#endif
    if (IsCrFilesSharingBusy()) return;
    if (choosing_avatar_) {
      choosing_avatar_ = false; CancelAvatarDecode();
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&LoadImagePreviewCancelable, file.path(), 256, avatar_decode_cancel_),
          base::BindOnce(&Settings::AvatarDecoded, weak_.GetWeakPtr(), avatar_decode_generation_));
      return;
    }
    directory_->SetText(file.path().AsUTF16Unsafe()); error_->SetVisible(false); RefreshSharing();
  }
  void FileSelectionCanceled() override {
    folder_dialog_.reset(); choosing_avatar_ = false;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    choosing_quick_folder_ = false;
#endif
  }
  void AvatarDecoded(uint64_t generation, gfx::ImageSkia image) {
    if (closing_ || generation != avatar_decode_generation_ || IsCrFilesSharingBusy()) return;
    if (!image.isNull()) {
      image = gfx::ImageSkiaOperations::CreateCroppedCenteredRoundRectImage(gfx::Size(96, 96), 0, image);
      std::array<uint8_t, 96 * 96 * 4> pixels;
      const auto info = SkImageInfo::Make(96, 96, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
      if (image.bitmap()->readPixels(info, pixels.data(), 96 * 4, 0, 0)) {
        custom_avatar_ = base::Base64Encode(pixels); custom_avatar_image_ = image;
        UpdateAvatar(); error_->SetVisible(false); RefreshSharing(); return;
      }
    }
    error_->SetVisible(true);
  }
  void RefreshSharing() {
    if (closing_ || !contents_observation_.IsObserving()) return;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    const bool quick_idle = !IsCrFilesQuickShareBusy();
    quick_enabled_->SetEnabled(quick_idle);
    quick_name_->SetEnabled(quick_idle); quick_directory_->SetEnabled(quick_idle);
    quick_folder_->SetEnabled(quick_idle && !folder_dialog_);
#if BUILDFLAG(IS_LINUX)
    quick_direct_->SetEnabled(quick_idle && !base::CommandLine::ForCurrentProcess()->HasSwitch("quick-share-direct-go"));
    quick_interface_->SetEnabled(quick_idle && quick_interfaces_loaded_ && !quick_interfaces_pending_ && !quick_interface_override_);
    quick_interface_refresh_->SetEnabled(quick_idle && !quick_interfaces_pending_);
#endif
#endif
    const bool enabled = !IsCrFilesSharingBusy();
    if (!enabled && avatar_widget_ && avatar_widget_->IsVisible()) avatar_widget_->Close();
    for (auto* field : {name_.get(), directory_.get()}) field->SetEnabled(enabled);
    discovery_->SetEnabled(enabled); auto_accept_->SetEnabled(enabled); avatar_button_->SetEnabled(enabled); folder_->SetEnabled(enabled && !folder_dialog_);
    const auto p = GetNearbySharingPreferences();
    const auto directory = p.directory.empty() ? NearbySharing::DefaultReceiveDirectory().AsUTF8Unsafe() : p.directory;
    apply_->SetEnabled(enabled && (p.name != base::UTF16ToUTF8(name_->GetText()) || directory != base::UTF16ToUTF8(directory_->GetText()) || p.avatar != avatar_ || p.custom_avatar != custom_avatar_ || p.discovery_enabled != discovery_->GetChecked() || p.auto_accept_trusted != auto_accept_->GetChecked()));
  }
  void ApplyNearby() {
    if (closing_ || IsCrFilesSharingBusy()) return;
    auto p = GetNearbySharingPreferences(); p.name = base::UTF16ToUTF8(name_->GetText()); p.directory = base::UTF16ToUTF8(directory_->GetText());
    p.avatar = avatar_; p.discovery_enabled = discovery_->GetChecked(); p.auto_accept_trusted = auto_accept_->GetChecked();
    p.custom_avatar = custom_avatar_;
    if (!NearbySharing::IsValidConfiguration(p.name, base::FilePath::FromUTF8Unsafe(p.directory))) {
      error_->SetVisible(true); return;
    }
    const auto destination = base::FilePath::FromUTF8Unsafe(p.directory);
    EnsureDirectoryAccess(GetWidget(), destination, true, base::BindOnce(
        [](base::WeakPtr<Settings> view, NearbySharingPreferences preferences, bool allowed) {
          if (!view || view->closing_) return;
          const bool valid = allowed && !IsCrFilesSharingBusy() && SetNearbySharingPreferences(preferences);
          view->error_->SetVisible(!valid);
          if (valid) { ApplyCrFilesSharingSettings(); view->RefreshSharing(); }
        }, weak_.GetWeakPtr(), std::move(p)));
  }
  std::unique_ptr<views::View> HistoryPage() {
    auto page = Page(IDS_CRFILES_HISTORY);
    auto* row = page->AddChildView(std::make_unique<views::View>());
    auto* row_layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    row_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    row_layout->SetFlexForView(Label(row, Text(IDS_CRFILES_KEEP_HISTORY)), 1);
    constexpr std::array labels = {IDS_CRFILES_HISTORY_7_DAYS, IDS_CRFILES_HISTORY_30_DAYS, IDS_CRFILES_HISTORY_90_DAYS, IDS_CRFILES_HISTORY_1_YEAR};
    std::vector<ui::SimpleComboboxModel::Item> items;
    for (int id : labels) items.emplace_back(Text(id));
    retention_ = row->AddChildView(std::make_unique<views::Combobox>(std::make_unique<ui::SimpleComboboxModel>(items)));
    retention_->SetAccessibleName(Text(IDS_CRFILES_KEEP_HISTORY));
    retention_->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    const auto retention_size = retention_->GetPreferredSize({});
    retention_->SetPreferredSize(gfx::Size((retention_size.width() * 3 + 1) / 2, retention_size.height()));
    const auto preferences = GetHistoryPreferences();
    for (size_t i = 0; i < kHistoryRetentionDays.size(); ++i) if (preferences.days == kHistoryRetentionDays[i]) retention_->SetSelectedIndex(i);
    for (size_t i = 0; i < kHistoryLabels.size(); ++i) {
      auto* item = page->AddChildView(std::make_unique<views::View>());
      item->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainer, 8));
      item->SetLayoutManager(std::make_unique<views::FillLayout>());
      checks_[i] = item->AddChildView(std::make_unique<views::Checkbox>(Text(kHistoryLabels[i])));
      checks_[i]->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(10, 0)));
      checks_[i]->SetChecked(preferences.record[i]);
    }
    retention_->SetCallback(base::BindRepeating(&Settings::SaveHistory, base::Unretained(this)));
    for (auto& check : checks_) check->SetCallback(base::BindRepeating(&Settings::SaveHistory, base::Unretained(this)));
    return page;
  }
  void LanguageChanged() { SetCrFilesLanguageSetting(kLanguages[language_->GetSelectedIndex().value_or(0)]); }
  void ChangeTheme(CrFilesThemeMode mode) {
    SetCrFilesThemeMode(mode);
    for (const auto& button : themes_) button->SetSelected(button->theme_mode() == mode);
  }
  void SaveHistory() {
    HistoryPreferences preferences; preferences.days = kHistoryRetentionDays[retention_->GetSelectedIndex().value_or(1)];
    for (size_t i = 0; i < checks_.size(); ++i) preferences.record[i] = checks_[i]->GetChecked();
    const bool saved = SetHistoryPreferences(preferences); error_->SetVisible(!saved);
    if (!saved) {
      preferences = GetHistoryPreferences();
      for (size_t i = 0; i < checks_.size(); ++i) checks_[i]->SetChecked(preferences.record[i]);
      for (size_t i = 0; i < kHistoryRetentionDays.size(); ++i) if (preferences.days == kHistoryRetentionDays[i]) retention_->SetSelectedIndex(i);
      return;
    }
    base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE}, base::BindOnce(&PruneFileHistory));
  }
  void SelectPage(size_t page) {
    if (page != 2) CloseAvatars();
    for (size_t i = 0; i < pages_.size(); ++i) { pages_[i]->SetVisible(i == page); navigation_[i]->Select(i == page); }
  }
  void Close() { GetWidget()->Close(); }
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  static constexpr size_t kPageCount = 6;
#if BUILDFLAG(IS_LINUX)
  raw_ptr<views::Combobox> quick_interface_ = nullptr;
  raw_ptr<views::Checkbox> quick_direct_ = nullptr;
  std::vector<std::string> quick_interface_names_;
  std::string quick_selected_interface_;
  raw_ptr<views::ImageButton> quick_interface_refresh_ = nullptr;
  bool quick_interface_override_ = false, quick_interfaces_loaded_ = false, quick_interfaces_pending_ = false;
#endif
  raw_ptr<views::Textfield> quick_name_ = nullptr, quick_directory_ = nullptr;
  raw_ptr<views::Checkbox> quick_enabled_ = nullptr;
  base::OneShotTimer quick_save_timer_;
  uint64_t quick_edit_generation_ = 0;
  bool quick_text_pending_ = false, quick_text_failed_ = false, quick_close_pending_ = false;
  raw_ptr<views::ImageButton> quick_folder_ = nullptr;
  bool choosing_quick_folder_ = false;
#else
  static constexpr size_t kPageCount = 5;
#endif
  std::array<raw_ptr<views::View>, kPageCount> pages_ = {};
  std::array<raw_ptr<NavigationButton>, kPageCount> navigation_ = {};
  std::array<raw_ptr<views::RadioButton>, 3> navigation_styles_ = {};
  std::array<raw_ptr<views::Checkbox>, 6> checks_ = {};
  std::vector<raw_ptr<libcr::ThemeToggleButton>> themes_;
  raw_ptr<views::Combobox> language_ = nullptr, retention_ = nullptr;
  raw_ptr<views::Textfield> name_ = nullptr, directory_ = nullptr;
  raw_ptr<views::Label> error_ = nullptr;
  raw_ptr<views::Checkbox> auto_hide_toolbar_ = nullptr, animate_folders_ = nullptr, hover_image_preview_ = nullptr;
  raw_ptr<views::Checkbox> single_click_folders_ = nullptr;
  raw_ptr<views::Checkbox> windows_titlebar_style_ = nullptr;
  raw_ptr<views::Checkbox> system_tray_ = nullptr;
  raw_ptr<views::Checkbox> discovery_ = nullptr, auto_accept_ = nullptr;
  raw_ptr<views::MdTextButton> apply_ = nullptr;
  raw_ptr<views::ImageButton> avatar_button_ = nullptr, folder_ = nullptr;
  int avatar_ = 0;
  bool choosing_avatar_ = false;
  std::string custom_avatar_;
  gfx::ImageSkia custom_avatar_image_;
  uint64_t avatar_decode_generation_ = 0;
  std::shared_ptr<std::atomic_bool> avatar_decode_cancel_ = std::make_shared<std::atomic_bool>(false);
  std::unique_ptr<views::EventMonitor> avatar_monitor_;
  base::RepeatingTimer sharing_timer_;
  scoped_refptr<ui::SelectFileDialog> folder_dialog_;
  std::unique_ptr<views::BubbleDialogDelegate> avatar_dialog_;
  std::unique_ptr<views::Widget> avatar_widget_;
  bool closing_ = false;
  base::ScopedObservation<views::View, views::ViewObserver> contents_observation_{this};
  base::WeakPtrFactory<Settings> weak_{this};
};
class About : public views::DialogDelegate {
 public:
  About() {
    SetTitle(Text(IDS_CRFILES_ABOUT)); SetModalType(ui::mojom::ModalType::kWindow);
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone)); set_use_custom_frame(true);
    SetAppIcon(CreateCrFilesIconModel(gfx::Size(32, 32))); SetShowIcon(false);
#if defined(USE_AURA)
    SetCenterTitle(true);
#endif
    auto contents = std::make_unique<views::View>(); contents->SetPreferredSize(gfx::Size(640, 320));
    contents->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(4, 4, 0, 4)));
    auto* layout = contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical));
    auto* top = contents->AddChildView(std::make_unique<views::View>()); layout->SetFlexForView(top, 1);
    contents->AddChildView(AboutBody(true));
    auto* bottom = contents->AddChildView(std::make_unique<views::View>()); layout->SetFlexForView(bottom, 1);
    contents->AddChildView(std::make_unique<views::Separator>());
    auto* footer = contents->AddChildView(std::make_unique<views::View>());
    auto* footer_layout = footer->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(12, 0)));
    footer_layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    footer->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&About::Close, base::Unretained(this)), Text(IDS_CRFILES_CLOSE)));
    SetContentsView(std::move(contents));
  }
 private:
  void Close() { GetWidget()->Close(); }
};
}
void ShowCrFilesSettingsDialog(views::Widget* parent) {
  ShowCrFilesDialog(std::make_unique<Settings>(), parent);
}
void ShowCrFilesAboutDialog(views::Widget* parent) {
  ShowCrFilesDialog(std::make_unique<About>(), parent);
}
}
