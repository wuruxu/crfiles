// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_settings_dialog.h"
#include <array>
#include <algorithm>
#include <atomic>
#include "base/base64.h"
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
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/process/launch.h"
#include "base/command_line.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/link.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <shellapi.h>
#endif
#include "crFiles/crfiles_app_icon.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_version.h"
#include "crFiles/crfiles_window.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/sharing/nearby_sharing.h"
#include "libcr/options_menu.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom.h"
#include "ui/color/color_id.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
#include "ui/views/view_observer.h"
#include "crFiles/crfiles_buildtime.h"
namespace crfiles {
namespace {
constexpr std::array<const char*, 13> kLanguages = {
    "", "en-US", "zh-CN", "zh-HK", "zh-TW", "ar", "fr", "fr-CA", "ru", "es", "es-419", "pt-BR", "pt-PT"};
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
void OpenProject() {
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()}, base::BindOnce([] {
#if BUILDFLAG(IS_WIN)
    ShellExecuteW(nullptr, L"open", L"https://github.com/libcr/crFiles", nullptr, nullptr, SW_SHOWNORMAL);
#else
#if BUILDFLAG(IS_MAC)
    const char* opener = "/usr/bin/open";
#else
    const char* opener = "xdg-open";
#endif
    base::LaunchProcess(std::vector<std::string>{opener, "https://github.com/libcr/crFiles"}, base::LaunchOptions());
#endif
  }));
}
void CopyAuthor() { ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste).WriteText(u"wrxzzj@gmail.com"); }
std::unique_ptr<views::View> AboutBody(bool large) {
  auto body = std::make_unique<views::View>();
  auto* layout = body->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(0, 12), 24));
  layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
  auto* column = body->AddChildView(std::make_unique<views::View>());
  column->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
  layout->SetFlexForView(column, 1);
  auto* description = Label(column, Text(IDS_CRFILES_DESCRIPTION));
  auto* version = Label(column, u"crFiles " CRFILES_VERSION_STRING u" · Chromium 150");
  auto* build = Label(column, Text(IDS_CRFILES_BUILD_TIME) + u": " + base::UTF8ToUTF16(std::string_view(CRFILES_BUILD_TIME)));
  version->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
  build->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
  if (large) for (auto* label : {description, version, build})
    label->SetFontList(label->font_list().DeriveWithSizeDelta(2));
  auto* project = column->AddChildView(std::make_unique<views::View>());
  project->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
  auto* project_title = Label(project, Text(IDS_CRFILES_PROJECT));
  auto* project_link = project->AddChildView(std::make_unique<views::Link>(u"https://github.com/libcr/crFiles"));
  project_link->SetCallback(base::BindRepeating(&OpenProject)); project_link->SetForceUnderline(false);
  auto* author = column->AddChildView(std::make_unique<views::View>());
  author->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
  auto* author_title = Label(author, Text(IDS_CRFILES_AUTHOR) + u": wuruxu (");
  auto* email = author->AddChildView(std::make_unique<views::Link>(u"wrxzzj@gmail.com"));
  email->SetCallback(base::BindRepeating(&CopyAuthor)); email->SetForceUnderline(false);
  auto* suffix = Label(author, u")");
  for (auto* label : {project_title, author_title, suffix}) {
    label->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
    if (large) label->SetFontList(label->font_list().DeriveWithSizeDelta(2));
  }
  if (large) for (auto* link : {project_link, email}) link->SetFontList(link->font_list().DeriveWithSizeDelta(2));
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
class Settings : public views::DialogDelegate, public ui::SelectFileDialog::Listener, public views::ViewObserver, public ui::EventObserver {
 public:
  Settings() {
    SetTitle(Text(IDS_CRFILES_SETTINGS)); SetModalType(ui::mojom::ModalType::kWindow);
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
    constexpr std::array titles = {IDS_CRFILES_GENERAL, IDS_CRFILES_NEARBY_SETTINGS, IDS_CRFILES_HISTORY, IDS_CRFILES_ABOUT};
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
    pages_[1] = host->AddChildView(NearbyPage());
    pages_[2] = host->AddChildView(HistoryPage());
    auto about = Page(IDS_CRFILES_ABOUT); about->AddChildView(AboutBody(false));
    pages_[3] = host->AddChildView(std::move(about));
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
  void WindowClosing() override {
    StopSharingUpdates();
    CloseAvatars();
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
    constexpr std::array<std::u16string_view, 12> language_names = {
        u"English (United States)", u"简体中文", u"繁體中文（香港）", u"繁體中文（台灣）",
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
    auto_hide_toolbar_->SetChecked(preferences.auto_hide_toolbar);
    animate_folders_->SetChecked(preferences.animate_folders);
    for (auto* checkbox : {auto_hide_toolbar_.get(), animate_folders_.get()}) {
      checkbox->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      checkbox->SetCallback(base::BindRepeating(&Settings::FileViewChanged, base::Unretained(this)));
    }
    return page;
  }
  void FileViewChanged() {
    const bool saved = SetFileViewPreferences({auto_hide_toolbar_->GetChecked(), animate_folders_->GetChecked()});
    error_->SetVisible(!saved);
    if (!saved) {
      const auto preferences = GetFileViewPreferences();
      auto_hide_toolbar_->SetChecked(preferences.auto_hide_toolbar);
      animate_folders_->SetChecked(preferences.animate_folders);
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
  views::Textfield* SharingField(views::View* page, int id, std::u16string value) {
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
      folder_ = input->AddChildView(views::ImageButton::CreateIconButton(base::BindRepeating(&Settings::BrowseFolder, base::Unretained(this)), vector_icons::kFolderFilledIcon, Text(IDS_CRFILES_RECEIVE_DIRECTORY)));
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
    avatar_widget_ = views::BubbleDialogDelegate::CreateBubble(avatar_dialog_.get(),
        base::BindOnce(&Settings::AvatarPickerClosed, weak_.GetWeakPtr()));
    avatar_dialog_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    auto* border = avatar_dialog_->GetBubbleFrameView()->bubble_border();
    border->set_draw_border_stroke(true); border->set_md_shadow_elevation(6);
    avatar_dialog_->SizeToContents(); avatar_widget_->Show();
    avatar_monitor_ = views::EventMonitor::CreateApplicationMonitor(this, GetWidget()->GetNativeWindow(), {ui::EventType::kMousePressed});
  }
  void AvatarPickerClosed(views::Widget::ClosedReason) {
    avatar_monitor_.reset();
    if (avatar_dialog_) avatar_dialog_->SetAnchorView(nullptr);
  }
  void OnEvent(const ui::Event& event) override {
    if (event.type() != ui::EventType::kMousePressed || !avatar_monitor_ || !avatar_widget_ ||
        avatar_widget_->IsClosed() || !avatar_widget_->IsVisible()) return;
    const auto point = avatar_monitor_->GetLastMouseLocation();
    if (!avatar_widget_->GetWindowBoundsInScreen().Contains(point) && !avatar_button_->GetBoundsInScreen().Contains(point))
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
    folder_dialog_.reset();
    if (closing_ || IsCrFilesSharingBusy()) return;
    if (choosing_avatar_) {
      choosing_avatar_ = false; CancelAvatarDecode();
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&LoadImagePreviewCancelable, file.path(), 256, avatar_decode_cancel_),
          base::BindOnce(&Settings::AvatarDecoded, weak_.GetWeakPtr(), avatar_decode_generation_));
      return;
    }
    directory_->SetText(file.path().AsUTF16Unsafe()); error_->SetVisible(false); RefreshSharing();
  }
  void FileSelectionCanceled() override { folder_dialog_.reset(); choosing_avatar_ = false; }
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
    const bool valid = NearbySharing::IsValidConfiguration(p.name, base::FilePath::FromUTF8Unsafe(p.directory)) && SetNearbySharingPreferences(p);
    error_->SetVisible(!valid); if (!valid) return;
    ApplyCrFilesSharingSettings(); RefreshSharing();
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
    if (page != 1) CloseAvatars();
    for (size_t i = 0; i < pages_.size(); ++i) { pages_[i]->SetVisible(i == page); navigation_[i]->Select(i == page); }
  }
  void Close() { StopSharingUpdates(); CloseAvatars(); GetWidget()->Close(); }
  std::array<raw_ptr<views::View>, 4> pages_ = {};
  std::array<raw_ptr<NavigationButton>, 4> navigation_ = {};
  std::array<raw_ptr<views::Checkbox>, 6> checks_ = {};
  std::vector<raw_ptr<libcr::ThemeToggleButton>> themes_;
  raw_ptr<views::Combobox> language_ = nullptr, retention_ = nullptr;
  raw_ptr<views::Textfield> name_ = nullptr, directory_ = nullptr;
  raw_ptr<views::Label> error_ = nullptr;
  raw_ptr<views::Checkbox> auto_hide_toolbar_ = nullptr, animate_folders_ = nullptr;
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
    auto contents = std::make_unique<views::View>(); contents->SetPreferredSize(gfx::Size(640, 280));
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
  views::DialogDelegate::CreateDialogWidget(std::make_unique<Settings>(), parent->GetNativeWindow(), parent->GetNativeView())->Show();
}
void ShowCrFilesAboutDialog(views::Widget* parent) {
  views::DialogDelegate::CreateDialogWidget(std::make_unique<About>(), parent->GetNativeWindow(), parent->GetNativeView())->Show();
}
}
