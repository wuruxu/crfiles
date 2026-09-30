// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_search_view.h"
#include "crFiles/crfiles_search.h"
#include <algorithm>
#include <set>
#include <map>
#include <vector>
#include "base/functional/bind.h"
#include "base/callback_list.h"
#include "base/i18n/case_conversion.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "components/vector_icons/vector_icons.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "crFiles/grit/crfiles_strings.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/events/event.h"
#include "ui/gfx/range/range.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/render_text.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/animation/ink_drop.h"
#include "ui/views/animation/ink_drop_host.h"
#include "ui/views/background.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
views::LabelButton* Button(views::View* parent, std::u16string text, base::RepeatingClosure action) {
  auto* button = parent->AddChildView(std::make_unique<views::LabelButton>(std::move(action), text));
  button->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(6, 10)));
  button->SetAnimateOnStateChange(true);
  button->SetHasInkDropActionOnClick(true);
  auto* ink = views::InkDrop::Get(button);
  ink->SetMode(views::InkDropHost::InkDropMode::ON);
  ink->SetBaseColor(ui::kColorSysPrimary); ink->SetHighlightOpacity(0.18f); ink->SetVisibleOpacity(0.28f);
  return button;
}
class SearchField : public views::Textfield {
  METADATA_HEADER(SearchField, views::Textfield)
 public:
  explicit SearchField(base::RepeatingCallback<void(bool)> focus) : focus_(std::move(focus)) {
    RemoveHoverEffect(); views::FocusRing::Remove(this);
  }
  void OnFocus() override { views::Textfield::OnFocus(); focus_.Run(true); }
  void OnBlur() override { views::Textfield::OnBlur(); focus_.Run(false); }
 private:
  base::RepeatingCallback<void(bool)> focus_;
};
BEGIN_METADATA(SearchField)
END_METADATA
class SearchMatchLabel : public views::Label {
  METADATA_HEADER(SearchMatchLabel, views::Label)
 public:
  SearchMatchLabel(std::u16string text, const std::u16string& query) : views::Label(text) {
    SetFontList(font_list().DeriveWithWeight(gfx::Font::Weight::NORMAL));
    SetHorizontalAlignment(gfx::ALIGN_LEFT);
    SetMultiLine(false); SetElideBehavior(gfx::ELIDE_TAIL);
    SetSubpixelRenderingEnabled(false);
    SetTooltipText(text);
    const auto folded = base::i18n::ToLower(text);
    const auto needle = base::i18n::ToLower(query);
    if (!needle.empty()) for (size_t position = folded.find(needle); position != std::u16string::npos;
        position = folded.find(needle, position + needle.size())) {
      if (position + needle.size() <= text.size()) matches_.emplace_back(position, position + needle.size());
    }
  }
 protected:
  void OnPaint(gfx::Canvas* canvas) override {
    views::View::OnPaint(canvas);
    if (!cached_text_ || cached_bounds_ != GetContentsBounds()) {
      cached_bounds_ = GetContentsBounds();
      cached_text_ = CreateRenderText();
      cached_text_->SetDisplayRect(cached_bounds_);
      cached_text_->SetWeight(gfx::Font::Weight::NORMAL);
      cached_text_->set_subpixel_rendering_suppressed(true);
      cached_text_->SetColor(GetColorProvider()->GetColor(ui::kColorSysOnSurface));
      for (const auto& range : matches_)
        cached_text_->ApplyColor(GetColorProvider()->GetColor(ui::kColorSysPrimary), range);
    }
    cached_text_->Draw(canvas);
  }
  void OnThemeChanged() override { cached_text_.reset(); views::Label::OnThemeChanged(); }
  void OnDeviceScaleFactorChanged(float old_scale, float new_scale) override {
    cached_text_.reset(); views::Label::OnDeviceScaleFactorChanged(old_scale, new_scale);
  }
 public:
  void SetFontList(const gfx::FontList& fonts) override { cached_text_.reset(); views::Label::SetFontList(fonts); }
 private:
  std::unique_ptr<gfx::RenderText> cached_text_;
  gfx::Rect cached_bounds_;
  std::vector<gfx::Range> matches_;
};
BEGIN_METADATA(SearchMatchLabel)
END_METADATA
class SearchResultButton : public views::Button {
  METADATA_HEADER(SearchResultButton, views::Button)
 public:
  SearchResultButton(base::RepeatingClosure open, const std::u16string& title,
                     const std::u16string& detail, const std::u16string& query, bool content, bool directory)
      : views::Button(std::move(open)) {
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetAccessibleName(content ? title + u" " + detail : title);
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(8, 10), 10));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    auto* image = AddChildView(std::make_unique<views::ImageView>());
    image->SetImage(ui::ImageModel::FromVectorIcon(directory ? vector_icons::kFolderOpenIcon : vector_icons::kDescriptionIcon, ui::kColorSysOnSurface, 20));
    image->SetCanProcessEventsWithinSubtree(false);
    auto* lines = AddChildView(std::make_unique<views::View>());
    lines->SetCanProcessEventsWithinSubtree(false);
    lines->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
    layout->SetFlexForView(lines, 1);
    lines->AddChildView(std::make_unique<SearchMatchLabel>(title, content ? std::u16string() : query));
    if (content) lines->AddChildView(std::make_unique<SearchMatchLabel>(detail, query));
  }
 protected:
  void StateChanged(ButtonState old_state) override {
    views::Button::StateChanged(old_state);
    SetBackground(GetState() == STATE_PRESSED ? views::CreateRoundedRectBackground(ui::kColorSysPrimaryContainer, 6) :
        GetState() == STATE_HOVERED ? views::CreateRoundedRectBackground(ui::kColorSysBaseContainerElevated, 6) : nullptr);
  }
  void NotifyClick(const ui::Event& event) override {
    if (event.IsMouseEvent() && event.AsMouseEvent()->GetClickCount() != 2) return;
    views::Button::NotifyClick(event);
  }
};
BEGIN_METADATA(SearchResultButton)
END_METADATA
// Logical height covers every result; child Views exist only around the viewport.
class SearchResults : public views::View {
  METADATA_HEADER(SearchResults, views::View)
 public:
  void SetModel(size_t count, int row_height) {
    if (count_ == count && row_height_ == row_height) return;
    count_ = count; row_height_ = row_height; PreferredSizeChanged();
  }
  gfx::Size CalculatePreferredSize(const views::SizeBounds& available) const override {
    return gfx::Size(available.width().is_bounded() ? available.width().value() : 1,
                     static_cast<int>(count_) * row_height_);
  }
  void Layout(PassKey) override {
    for (const auto& [index, row] : rows_)
      row->SetBounds(0, static_cast<int>(index) * row_height_, width(), row_height_);
    LayoutSuperclass<views::View>(this);
  }
  void Clear() { rows_.clear(); RemoveAllChildViews(); }
  std::map<size_t, raw_ptr<views::View>>& rows() { return rows_; }
  int row_height() const { return row_height_; }
 private:
  size_t count_ = 0;
  int row_height_ = 40;
  std::map<size_t, raw_ptr<views::View>> rows_;
};
BEGIN_METADATA(SearchResults)
END_METADATA
class SearchPage : public views::View, public views::TextfieldController, public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(SearchPage, views::View)
 public:
  SearchPage(base::FilePath root, base::RepeatingCallback<void(base::FilePath, bool)> open,
             base::RepeatingCallback<void(base::FilePath)> reveal, base::RepeatingClosure state_changed)
      : root_(std::move(root)), open_(std::move(open)), reveal_(std::move(reveal)), state_changed_(std::move(state_changed)) {
    // The pane receives focus before OnFocus forwards it to the search field.
    // It must expose valid accessibility semantics even during that transition.
    GetViewAccessibility().SetRole(ax::mojom::Role::kPane);
    SetAccessibleName(Text(IDS_CRFILES_SEARCH));
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetBackground(views::CreateSolidBackground(ui::kColorSysBase));
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 10));
    header_ = AddChildView(std::make_unique<views::View>());
    header_->SetPreferredSize(gfx::Size(0, 48));
    title_ = header_->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_SEARCH)));
    capsule_ = header_->AddChildView(std::make_unique<views::View>());
    filter_ = Button(capsule_, {}, base::BindRepeating(&SearchPage::ShowFilter, weak_.GetWeakPtr()));
    filter_->SetAccessibleName(Text(IDS_CRFILES_HISTORY_FILTER));
    filter_->SetBorder(views::CreateEmptyBorder(8));
    filter_->SetInstallFocusRingOnFocus(false);
    views::InkDrop::Get(filter_)->SetMode(views::InkDropHost::InkDropMode::OFF);
    filter_->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(vector_icons::kFilterOldIcon, ui::kColorSysOnSurface, 20));
    for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
      filter_->SetImageModel(state, ui::ImageModel::FromVectorIcon(vector_icons::kFilterOldIcon, ui::kColorSysPrimary, 20));
    chip_scroll_ = capsule_->AddChildView(std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled));
    chip_scroll_->SetUseContentsPreferredSize(true); chip_scroll_->SetBackgroundColor(std::nullopt);
    chip_scroll_->SetVerticalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    chip_scroll_->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kHiddenButEnabled);
    chips_ = chip_scroll_->SetContents(std::make_unique<views::View>());
    chips_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
    separator_ = capsule_->AddChildView(std::make_unique<views::Separator>());
    separator_->SetOrientation(views::Separator::Orientation::kVertical);
    input_ = capsule_->AddChildView(std::make_unique<SearchField>(base::BindRepeating(&SearchPage::FocusChanged, weak_.GetWeakPtr())));
    input_->set_controller(this);
    input_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(9, 12)));
    input_->SetBackgroundColor(ui::kColorSysBaseContainer);
    input_->SetPlaceholderText(Text(IDS_CRFILES_SEARCH_PLACEHOLDER));
    input_->SetAccessibleName(Text(IDS_CRFILES_SEARCH_PLACEHOLDER));
    FocusChanged(false);
    path_row_ = AddChildView(std::make_unique<views::View>());
    path_row_->SetPreferredSize(gfx::Size(0, 40));
    path_group_ = path_row_->AddChildView(std::make_unique<views::View>());
    path_group_->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainer, 20));
    auto* path_layout = path_group_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 0));
    path_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    pause_ = Button(path_group_, {}, base::BindRepeating(&SearchPage::TogglePause, weak_.GetWeakPtr()));
    pause_->SetBorder(views::CreateEmptyBorder(2));
    pause_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    // Fill the full leading segment, flush with the surrounding path capsule.
    views::InstallRoundRectHighlightPathGenerator(pause_, gfx::Insets(), gfx::RoundedCornersF(20, 0, 0, 20));
    auto* pause_ink = views::InkDrop::Get(pause_);
    pause_ink->SetHighlightOpacity(0.24f); pause_ink->SetVisibleOpacity(0.36f);
    pause_ink->GetInkDrop()->SetShowHighlightOnHover(true);
    path_separator_ = path_group_->AddChildView(std::make_unique<views::Separator>());
    path_separator_->SetOrientation(views::Separator::Orientation::kVertical);
    path_separator_->SetPreferredSize(gfx::Size(1, 18));
    auto* path = path_group_->AddChildView(std::make_unique<views::Label>(root_.AsUTF16Unsafe()));
    // The 1px divider plus 7px label inset leaves 8px after the button.
    path->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(0, 7, 0, 12)));
    path->SetHorizontalAlignment(gfx::ALIGN_LEFT); path->SetElideBehavior(gfx::ELIDE_MIDDLE); path->SetTooltipText(root_.AsUTF16Unsafe());
    path_layout->SetFlexForView(path, 1);
    status_ = AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_SEARCH_READY)));
    status_->SetHorizontalAlignment(gfx::ALIGN_LEFT); status_->SetMultiLine(true);
    // Preferred-size contents are assigned bounds by the layered scrolling path.
    // The platform default is disabled on Linux/Windows, leaving plain Views at
    // their initial zero size even when they already contain matching rows.
    scroll_ = AddChildView(std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled));
    scroll_->SetUseContentsPreferredSize(true); scroll_->SetBackgroundColor(std::nullopt);
    scroll_->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    layout->SetFlexForView(scroll_, 1);
    results_ = scroll_->SetContents(std::make_unique<SearchResults>());
    scroll_subscription_ = scroll_->AddContentsScrolledCallback(base::BindRepeating(&SearchPage::ScheduleVisibleRows, weak_.GetWeakPtr()));
    scroll_->RegisterPostLayoutCallback(base::BindRepeating([](base::WeakPtr<SearchPage> page, views::ScrollView*) {
      if (page) page->ScheduleVisibleRows();
    }, weak_.GetWeakPtr()));
    UpdateMode(); RenderResults(); UpdateControls();
  }
  ~SearchPage() override { Stop(); }
  void Stop() {
    pump_.Stop(); results_timer_.Stop(); status_timer_.Stop();
    if (connection_ && job_) connection_->Cancel(job_);
    job_ = 0; pending_ = false; paused_ = false; ++generation_;
  }
  void Restart() { Start(); }
  SearchPageState state() const {
    switch (status_id_) {
      case IDS_CRFILES_SEARCH_RUNNING: return SearchPageState::kRunning;
      case IDS_CRFILES_SEARCH_PAUSED: return SearchPageState::kPaused;
      case IDS_CRFILES_SEARCH_COMPLETE: case IDS_CRFILES_SEARCH_LIMIT: return SearchPageState::kComplete;
      case IDS_CRFILES_SEARCH_CANCELED: return SearchPageState::kCanceled;
      case IDS_CRFILES_SEARCH_FAILED: return SearchPageState::kFailed;
      default: return SearchPageState::kReady;
    }
  }
  void OnFocus() override { views::View::OnFocus(); input_->RequestFocus(); }
  void Layout(PassKey) override {
    LayoutSuperclass<views::View>(this);
    const int title_width = title_->GetPreferredSize({}).width();
    title_->SetBounds(0, 4, title_width, 40);
    const int field_width = std::min(660, std::max(0, header_->width() - 2 * (title_width + 16)));
    capsule_->SetBounds((header_->width() - field_width) / 2, 4, field_width, 40);
    filter_->SetBounds(4, 2, 36, 36);
    const int chip_width = (extensions_.empty() && committed_query_.empty()) ? 0 : std::min(chips_->GetPreferredSize({}).width(), std::max(0, field_width / 2 - 48));
    chip_scroll_->SetBounds(44, 6, chip_width, 28);
    separator_->SetBounds(44 + chip_width, 10, 1, 20);
    input_->SetBounds(45 + chip_width, 1, std::max(0, field_width - chip_width - 57), 38);
    const int group_width = std::min({660, path_row_->width(), path_group_->GetPreferredSize({}).width()});
    path_group_->SetBounds(std::max(0, (path_row_->width() - group_width) / 2), 0, std::max(0, group_width), 40);
  }
  void ContentsChanged(views::Textfield*, const std::u16string&) override {
    if (updating_) return;
    ParseTypes(false);
    // A newly typed query replaces the previous query chip. File-type chips
    // remain available, including when typing an extension followed by Space.
    if (!committed_query_.empty() &&
        std::u16string(input_->GetText()).find_first_not_of(u" \t\n") != std::u16string::npos) {
      committed_query_.clear(); RebuildChips();
    }
    ConditionsChanged();
  }
  bool HandleKeyEvent(views::Textfield*, const ui::KeyEvent& event) override {
    if (event.type() != ui::EventType::kKeyPressed) return false;
    if (event.key_code() == ui::VKEY_RETURN) { Start(); return true; }
    if (event.key_code() == ui::VKEY_ESCAPE) { Cancel(); return true; }
    return false;
  }
  bool IsCommandIdChecked(int id) const override { return id == 3 ? hidden_ : id == (content_ ? 2 : 1); }
  void ExecuteCommand(int id, int) override {
    if (id == 3) { ToggleHidden(); return; }
    if (id != 1 && id != 2) return;
    if (content_ == (id == 2)) return;
    content_ = id == 2; UpdateMode(); ConditionsChanged();
    if (HasSearchString()) RunSearch();
  }
 private:
  bool HasSearchString() const {
    std::string text = base::UTF16ToUTF8(input_->GetText());
    base::TrimWhitespaceASCII(text, base::TRIM_ALL, &text);
    return !text.empty() || !committed_query_.empty();
  }
  void FocusChanged(bool focused) {
    input_->SetBackgroundColor(focused ? ui::kColorSysBaseContainerElevated : ui::kColorSysBaseContainer);
    capsule_->SetBackground(views::CreateRoundedRectBackground(focused ? ui::kColorSysBaseContainerElevated : ui::kColorSysBaseContainer, 20));
    capsule_->SetBorder(focused ? views::CreateRoundedRectBorder(1, 20, ui::kColorSysPrimary) : nullptr);
  }
  void UpdateMode() {
    filter_->SetTooltipText(Text(content_ ? IDS_CRFILES_SEARCH_CONTENT : IDS_CRFILES_SEARCH_PATH));
  }
  void ShowFilter() {
    if (menu_runner_ && menu_runner_->IsRunning()) return;
    menu_runner_.reset(); menu_ = std::make_unique<ui::SimpleMenuModel>(this);
    menu_->AddRadioItem(1, Text(IDS_CRFILES_SEARCH_PATH), 1);
    menu_->AddRadioItem(2, Text(IDS_CRFILES_SEARCH_CONTENT), 1);
    menu_->AddSeparator(ui::NORMAL_SEPARATOR);
    menu_->AddCheckItem(3, Text(IDS_CRFILES_SEARCH_HIDDEN));
    menu_runner_ = std::make_unique<views::MenuRunner>(menu_.get(), views::MenuRunner::CONTEXT_MENU);
    menu_runner_->RunMenuAt(GetWidget(), nullptr, filter_->GetBoundsInScreen(), views::MenuAnchorPosition::kTopLeft, ui::mojom::MenuSourceType::kMouse);
  }
  void ParseTypes(bool final) {
    std::u16string text(input_->GetText()), remaining;
    bool changed = false;
    for (size_t start = 0; start < text.size();) {
      const auto end = text.find(u' ', start);
      const auto finish = end == std::u16string::npos ? text.size() : end;
      const auto token = base::ToLowerASCII(base::UTF16ToUTF8(std::u16string_view(text).substr(start, finish - start)));
      const bool complete = end != std::u16string::npos || final;
      const bool extension = complete && token.size() >= 2 && token.size() <= 32 && token.front() == '.' &&
          token.find_first_not_of(".abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
      if (extension && (extensions_.contains(token) || extensions_.size() < 32)) { extensions_.insert(token); changed = true; }
      else { remaining.append(text, start, finish - start); if (end != std::u16string::npos) remaining += u' '; }
      if (end == std::u16string::npos) break;
      start = end + 1;
    }
    if (!changed) return;
    updating_ = true; input_->SetText(remaining); input_->SetSelectedRange(gfx::Range(remaining.size())); updating_ = false;
    RebuildChips();
  }
  void RebuildChips() {
    chips_->RemoveAllChildViews();
    if (!committed_query_.empty()) {
      auto* keyword = Button(chips_, base::UTF8ToUTF16(committed_query_) + u" ×",
          base::BindRepeating([](base::WeakPtr<SearchPage> page) {
            if (page) base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                base::BindOnce(&SearchPage::RemoveQuery, page));
          }, weak_.GetWeakPtr()));
      keyword->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 8)));
      keyword->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysPrimaryContainer, 14));
      keyword->SetEnabledTextColors(ui::kColorSysOnPrimaryContainer);
      keyword->SetTooltipText(base::UTF8ToUTF16(committed_query_));
      keyword->SetAccessibleName(Text(IDS_CRFILES_CLOSE) + u" " + base::UTF8ToUTF16(committed_query_));
      views::InstallRoundRectHighlightPathGenerator(keyword, gfx::Insets(), 14);
    }
    for (const auto& extension : extensions_) {
      auto* chip = Button(chips_, base::UTF8ToUTF16(extension) + u" ×",
          base::BindRepeating([](base::WeakPtr<SearchPage> page, std::string value) {
            if (!page) return;
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                base::BindOnce(&SearchPage::RemoveType, page, value));
          }, weak_.GetWeakPtr(), extension));
      chip->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 8)));
      chip->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysPrimaryContainer, 14));
      chip->SetEnabledTextColors(ui::kColorSysOnPrimaryContainer);
      chip->SetAccessibleName(Text(IDS_CRFILES_CLOSE) + u" " + base::UTF8ToUTF16(extension));
    }
    InvalidateLayout();
  }
  void RemoveQuery() {
    Cancel(); committed_query_.clear(); normal_complete_ = false;
    updating_ = true; input_->SetText(u""); updating_ = false;
    RebuildChips(); input_->RequestFocus();
  }
  void RemoveType(std::string type) { extensions_.erase(type); RebuildChips(); ConditionsChanged(); }
  void ConditionsChanged() {
    Stop();
    normal_complete_ = false; hidden_started_ = false;
    rows_timer_.Stop(); results_->Clear();
    hits_.clear(); visible_hits_.clear(); scanned_ = 0; skipped_ = 0; limited_ = false;
    SetStatus(IDS_CRFILES_SEARCH_READY); UpdateControls();
  }
  void Start() {
    ParseTypes(true);
    RunSearch();
    if (job_ && !query_.empty()) {
      committed_query_ = query_;
      updating_ = true; input_->SetText(u""); updating_ = false;
      RebuildChips();
    }
  }
  void RunSearch() {
    Stop();
    query_ = base::UTF16ToUTF8(input_->GetText()); base::TrimWhitespaceASCII(query_, base::TRIM_ALL, &query_);
    if (query_.empty()) query_ = committed_query_;
    normal_complete_ = false; hidden_started_ = false; display_content_ = content_;
    rows_timer_.Stop(); results_->Clear();
    hits_.clear(); visible_hits_.clear(); scanned_ = 0; skipped_ = 0; limited_ = false; RenderResults();
    if ((query_.empty() && extensions_.empty()) || (content_ && query_.empty())) { SetStatus(IDS_CRFILES_SEARCH_READY); UpdateControls(); return; }
    if (query_.size() > 4096) { SetStatus(IDS_CRFILES_SEARCH_FAILED); UpdateControls(); return; }
    StartPass(false);
  }
  void StartPass(bool hidden_only) {
    scanned_base_ = scanned_; skipped_base_ = skipped_;
    hidden_pass_ = hidden_only;
    if (hidden_only) hidden_started_ = true;
    connection_ = SearchConnection::Get();
    auto options = mojom::SearchOptions::New(); options->root = root_.AsUTF8Unsafe(); options->query = query_;
    options->show_hidden = hidden_only; options->hidden_only = hidden_only;
    options->extensions.assign(extensions_.begin(), extensions_.end());
    options->mode = content_ ? mojom::SearchMode::kContent : mojom::SearchMode::kPath;
    job_ = connection_->Start(std::move(options), base::BindRepeating(&SearchPage::Disconnected, weak_.GetWeakPtr()));
    if (!job_) { SetStatus(IDS_CRFILES_SEARCH_FAILED); UpdateControls(); return; }
    SetStatus(IDS_CRFILES_SEARCH_RUNNING); UpdateControls(); Pull();
  }
  void Pull() {
    if (!job_ || pending_ || paused_) return;
    pending_ = true;
    connection_->Next(job_, base::BindOnce(&SearchPage::Batch, weak_.GetWeakPtr(), generation_));
  }
  void Batch(uint64_t generation, mojom::SearchBatchPtr batch) {
    if (generation != generation_) return;
    pending_ = false; scanned_ = scanned_base_ + batch->scanned; skipped_ = skipped_base_ + batch->skipped; limited_ |= batch->limited;
    bool added_visible = false;
    for (auto& hit : batch->hits) {
      if (hits_.size() < 10000) {
        if (hidden_ || !hit->hidden) { visible_hits_.push_back(hits_.size()); added_visible = true; }
        hits_.push_back(std::move(hit));
      }
      else limited_ = true;
    }
    if (added_visible && !results_timer_.IsRunning())
      results_timer_.Start(FROM_HERE, base::Milliseconds(100), this, &SearchPage::RenderResults);
    if (batch->done || batch->failed) {
      Stop();
      if (!batch->failed) {
        if (!hidden_pass_) normal_complete_ = true;
        if (hidden_ && normal_complete_ && !hidden_started_ && hits_.size() < 10000) { StartPass(true); return; }
      }
      SetStatus(batch->failed ? IDS_CRFILES_SEARCH_FAILED : limited_ ? IDS_CRFILES_SEARCH_LIMIT : IDS_CRFILES_SEARCH_COMPLETE);
    } else if (paused_) SetStatus(IDS_CRFILES_SEARCH_PAUSED, false);
    else {
      SetStatus(IDS_CRFILES_SEARCH_RUNNING, false);
      pump_.Start(FROM_HERE, base::Milliseconds(25), this, &SearchPage::Pull);
    }
    UpdateControls();
  }
  void ToggleHidden() {
    hidden_ = !hidden_;
    if (!job_ && !normal_complete_ && HasSearchString()) { RunSearch(); return; }
    visible_hits_.clear();
    for (size_t index = 0; index < hits_.size(); ++index)
      if (hidden_ || !hits_[index]->hidden) visible_hits_.push_back(index);
    // Ordinary hits precede the supplemental hidden pass; their row IDs stay stable.
    RenderResults();
    if (hidden_ && normal_complete_ && !hidden_started_ && !job_ && hits_.size() < 10000) {
      StartPass(true); return;
    }
    SetStatus(status_id_);
  }
  void TogglePause() {
    if (!job_) return;
    paused_ = !paused_; connection_->Pause(job_, paused_);
    if (!paused_) Pull();
    SetStatus(paused_ ? IDS_CRFILES_SEARCH_PAUSED : IDS_CRFILES_SEARCH_RUNNING); UpdateControls();
  }
  void Cancel() { Stop(); SetStatus(IDS_CRFILES_SEARCH_CANCELED); UpdateControls(); }
  void Disconnected() { Stop(); SetStatus(IDS_CRFILES_SEARCH_FAILED); UpdateControls(); }
  void SetStatus(int text, bool immediate = true) {
    const bool changed = status_id_ != text;
    status_id_ = text;
    if (changed && state_changed_) state_changed_.Run();
    if (immediate || changed) {
      status_timer_.Stop(); RenderResults(); PaintStatus();
    } else if (!status_timer_.IsRunning()) {
      status_timer_.Start(FROM_HERE, base::Milliseconds(250), this, &SearchPage::PaintStatus);
    }
  }
  void PaintStatus() {
    status_->SetText(Text(status_id_) + u" · " + base::NumberToString16(visible_hits_.size()) + u" " + Text(IDS_CRFILES_SEARCH_MATCHES) +
        u" · " + base::NumberToString16(scanned_) + u" " + Text(IDS_CRFILES_SEARCH_SCANNED) +
        u" · " + base::NumberToString16(skipped_) + u" " + Text(IDS_CRFILES_SEARCH_SKIPPED));
  }
  void UpdateControls() {
    const bool has_task = job_ != 0;
    if (!has_task && pause_->HasFocus()) input_->RequestFocus();
    pause_->SetVisible(has_task); path_separator_->SetVisible(has_task);
    pause_->SetEnabled(has_task);
    const auto caption = Text(paused_ ? IDS_CRFILES_SEARCH_RESUME : IDS_CRFILES_SEARCH_PAUSE);
    pause_->SetTooltipText(caption); pause_->SetAccessibleName(caption);
    const auto& icon = paused_ ? vector_icons::kPlayArrowIcon : vector_icons::kPauseIcon;
    pause_->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(icon, ui::kColorSysOnSurface, 26));
    for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
      pause_->SetImageModel(state, ui::ImageModel::FromVectorIcon(icon, ui::kColorSysPrimary, 26));
    pause_->SetPreferredSize(gfx::Size(40, 40));
  }
  void RenderResults() {
    results_timer_.Stop();
    const int line_height = std::max(20, gfx::FontList().GetHeight());
    results_->SetModel(visible_hits_.size(), display_content_ ? line_height * 2 + 20 : line_height + 16);
    ScheduleVisibleRows();
  }
  void ScheduleVisibleRows() {
    if (!rows_timer_.IsRunning()) rows_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &SearchPage::UpdateVisibleRows);
  }
  void UpdateVisibleRows() {
    if (!IsDrawn() || !results_->width()) return;
    const auto viewport = scroll_->GetVisibleRect();
    if (viewport.IsEmpty()) return;
    const int row_height = results_->row_height();
    const size_t first = static_cast<size_t>(std::max(0, viewport.y() / row_height - 3));
    const size_t last = std::min(visible_hits_.size(), static_cast<size_t>(std::max(0, viewport.bottom() / row_height + 4)));
    auto* focused = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
    auto& materialized = results_->rows();
    for (auto it = materialized.begin(); it != materialized.end();) {
      const bool outside = it->first < first || it->first >= last;
      // Keep one focused row alive while scrolling so keyboard focus is stable.
      if (outside && (it->first >= visible_hits_.size() || !focused || !it->second->Contains(focused))) {
        auto* obsolete = it->second.get();
        if (focused && obsolete->Contains(focused)) { focused = nullptr; input_->RequestFocus(); }
        it = materialized.erase(it); results_->RemoveChildViewT(obsolete);
      } else ++it;
    }
    const auto query = base::UTF8ToUTF16(query_);
    bool added = false;
    for (size_t i = first; i < last; ++i) {
      if (materialized.contains(i)) continue;
      const auto& hit = hits_[visible_hits_[i]];
      const auto path = base::FilePath::FromUTF8Unsafe(hit->path);
      auto* row = results_->AddChildView(std::make_unique<views::View>());
      // Overlay scrollbars paint above the viewport without reducing its width.
      // Keep the reveal button clear even while the macOS thumb is expanded.
      auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal,
          gfx::Insets::TLBR(0, 0, 0, 24), 4));
      layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
      auto* result = row->AddChildView(std::make_unique<SearchResultButton>(base::BindRepeating(open_, path, hit->directory),
          display_content_ ? path.BaseName().AsUTF16Unsafe() : path.AsUTF16Unsafe(), base::UTF8ToUTF16(hit->detail), query, display_content_, hit->directory));
      result->GetViewAccessibility().SetPosInSet(static_cast<int>(i + 1));
      result->GetViewAccessibility().SetSetSize(static_cast<int>(visible_hits_.size()));
      result->SetTooltipText(display_content_ ? path.AsUTF16Unsafe() + u"\n" + base::UTF8ToUTF16(hit->detail) : path.AsUTF16Unsafe());
      layout->SetFlexForView(result, 1);
      auto* folder = Button(row, {}, base::BindRepeating(reveal_, path));
      folder->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainer, 10));
      views::InstallRoundRectHighlightPathGenerator(folder, gfx::Insets(), 10);
      auto* ink = views::InkDrop::Get(folder);
      ink->SetHighlightOpacity(0.24f); ink->SetVisibleOpacity(0.36f);
      ink->GetInkDrop()->SetShowHighlightOnHover(true);
      folder->SetTooltipText(Text(IDS_CRFILES_SEARCH_OPEN_FOLDER));
      folder->SetAccessibleName(Text(IDS_CRFILES_SEARCH_OPEN_FOLDER));
      folder->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(vector_icons::kFolderOpenIcon, ui::kColorSysOnSurface, 18));
      for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
        folder->SetImageModel(state, ui::ImageModel::FromVectorIcon(vector_icons::kFolderOpenIcon, ui::kColorSysPrimary, 18));
      materialized.emplace(i, row); added = true;
      row->SetBounds(0, static_cast<int>(i) * row_height, results_->width(), row_height);
    }
    if (added) {
      size_t order = 0;
      for (const auto& [index, row] : materialized) results_->ReorderChildView(row.get(), order++);
      results_->InvalidateLayout();
    }
  }
  base::FilePath root_;
  base::RepeatingCallback<void(base::FilePath, bool)> open_;
  base::RepeatingCallback<void(base::FilePath)> reveal_;
  base::RepeatingClosure state_changed_;
  std::shared_ptr<SearchConnection> connection_;
  uint64_t job_ = 0, generation_ = 0, scanned_ = 0, skipped_ = 0;
  uint64_t scanned_base_ = 0, skipped_base_ = 0;
  int status_id_ = IDS_CRFILES_SEARCH_READY;
  bool normal_complete_ = false, hidden_started_ = false, hidden_pass_ = false;
  std::vector<size_t> visible_hits_;
  base::CallbackListSubscription scroll_subscription_;
  raw_ptr<SearchResults> results_ = nullptr;
  bool hidden_ = false;
  bool display_content_ = false;
  bool content_ = false, paused_ = false, pending_ = false, updating_ = false, limited_ = false;
  std::string query_, committed_query_;
  std::set<std::string> extensions_;
  std::vector<mojom::SearchHitPtr> hits_;
  raw_ptr<views::View> header_, capsule_, chips_, path_row_, path_group_;
  raw_ptr<views::Label> title_, status_;
  raw_ptr<views::LabelButton> filter_, pause_;
  raw_ptr<views::ScrollView> chip_scroll_, scroll_;
  raw_ptr<views::Separator> separator_, path_separator_;
  raw_ptr<views::Textfield> input_;
  base::OneShotTimer pump_, results_timer_, status_timer_, rows_timer_;
  std::unique_ptr<ui::SimpleMenuModel> menu_;
  std::unique_ptr<views::MenuRunner> menu_runner_;
  base::WeakPtrFactory<SearchPage> weak_{this};
};
BEGIN_METADATA(SearchPage)
END_METADATA
}
std::unique_ptr<views::View> CreateSearchPage(const base::FilePath& root, base::RepeatingCallback<void(base::FilePath, bool)> open,
    base::RepeatingCallback<void(base::FilePath)> reveal, base::RepeatingClosure state_changed) {
  return std::make_unique<SearchPage>(root, std::move(open), std::move(reveal), std::move(state_changed));
}
SearchPageState GetSearchPageState(views::View* page) {
  if (auto* search = views::AsViewClass<SearchPage>(page)) return search->state();
  return SearchPageState::kReady;
}
void RefreshSearchPage(views::View* page) { if (auto* search = views::AsViewClass<SearchPage>(page)) search->Restart(); }
void StopSearchPage(views::View* page) { if (auto* search = views::AsViewClass<SearchPage>(page)) search->Stop(); }
}
