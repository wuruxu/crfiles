// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_tags_view.h"
#include <algorithm>
#include <cmath>
#include <map>
#include "ui/base/mojom/dialog_button.mojom.h"
#include <utility>
#include "base/functional/bind.h"
#include "base/files/file_util.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "ui/gfx/vector_icon_types.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/task/sequenced_task_runner.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/grit/crfiles_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/color/color_id.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/window/dialog_delegate.h"
namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
views::Label* Label(views::View* parent, const std::u16string& text) {
  auto* label = parent->AddChildView(std::make_unique<views::Label>(text));
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true); return label;
}
class TagToggleButton : public views::MdTextButton {
  METADATA_HEADER(TagToggleButton, views::MdTextButton)
 public:
  TagToggleButton(std::u16string name, bool active, bool mixed,
                  base::RepeatingCallback<void(bool)> changed)
      : views::MdTextButton({}, name), name_(std::move(name)), active_(active),
        mixed_(mixed), changed_(std::move(changed)) {
    SetCornerRadius(14);
    SetMinSize(gfx::Size());
    SetCustomPadding(gfx::Insets::VH(4, 10));
    GetViewAccessibility().SetRole(ax::mojom::Role::kToggleButton);
    SetCallback(base::BindRepeating(&TagToggleButton::Toggle, base::Unretained(this)));
    SetText(name_ + (mixed_ ? u" · " + Text(IDS_CRFILES_TAG_MIXED) : std::u16string()));
    base_width_ = std::clamp(GetPreferredSize({}).width(), 28, 144);
    UpdateAppearance();
  }
  void FinishSave(bool ok) {
    pending_ = false;
    if (!ok) { active_ = saved_active_; mixed_ = saved_mixed_; UpdateAppearance(); }
  }
  int MaximumWidth() const { return static_cast<int>(std::ceil(base_width_ * 1.25)); }
 private:
  void Toggle() {
    if (pending_) return;
    pending_ = true; saved_active_ = active_; saved_mixed_ = mixed_;
    active_ = !active_; mixed_ = false;
    UpdateAppearance();
    changed_.Run(active_);
  }
  void UpdateAppearance() {
    SetText(name_ + (mixed_ ? u" · " + Text(IDS_CRFILES_TAG_MIXED) : std::u16string()));
    SetTooltipText(std::u16string(GetText()));
    SetCornerRadius(active_ ? 17.5f : 14.f);
    SetPreferredSize(gfx::Size(active_ ? MaximumWidth() : base_width_, active_ ? 35 : 28));
    SetBgColorIdOverride(active_ ? ui::kColorSysPrimaryContainer : ui::kColorSysSurfaceVariant);
    SetEnabledTextColors(active_ ? ui::kColorSysOnPrimaryContainer : ui::kColorSysOnSurfaceVariant);
    SetStrokeColorIdOverride(active_ ? ui::kColorSysPrimaryContainer : ui::kColorSysSurfaceVariant);
    GetViewAccessibility().SetCheckedState(mixed_ ? ax::mojom::CheckedState::kMixed :
        active_ ? ax::mojom::CheckedState::kTrue : ax::mojom::CheckedState::kFalse);
  }
  std::u16string name_;
  int base_width_ = 28;
  bool active_;
  bool mixed_;
  bool pending_ = false, saved_active_ = false, saved_mixed_ = false;
  base::RepeatingCallback<void(bool)> changed_;
};
BEGIN_METADATA(TagToggleButton)
END_METADATA
constexpr gfx::PathElement kAddTagPath[] = {
    gfx::CANVAS_DIMENSIONS, 24, gfx::STROKE, 2,
    gfx::MOVE_TO, 5, 12, gfx::LINE_TO, 19, 12,
    gfx::MOVE_TO, 12, 5, gfx::LINE_TO, 12, 19,
};
const gfx::VectorIconRep kAddTagReps[] = {{kAddTagPath}};
const gfx::VectorIcon kAddTagIcon(kAddTagReps, 1, "add_tag");
class TagInputOutsideClick : public ui::EventHandler {
 public:
  TagInputOutsideClick(views::View* root, views::View* input, base::RepeatingClosure collapse)
      : root_(root), input_(input), collapse_(std::move(collapse)) {
    root->AddPreTargetHandler(this);
  }
  ~TagInputOutsideClick() override {
    if (root_.view()) root_.view()->RemovePreTargetHandler(this);
  }
  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->type() != ui::EventType::kMousePressed || !input_->GetVisible()) return;
    auto* target = static_cast<views::View*>(event->target());
    if (!target) return;
    auto point = event->location();
    views::View::ConvertPointToScreen(target, &point);
    if (!input_->GetBoundsInScreen().Contains(point)) collapse_.Run();
  }
 private:
  views::ViewTracker root_;
  raw_ptr<views::View> input_;
  base::RepeatingClosure collapse_;
};
class TagEditor : public views::View, public views::TextfieldController {
  METADATA_HEADER(TagEditor, views::View)
 public:
  TagEditor(std::vector<base::FilePath> paths, base::RepeatingClosure changed)
      : paths_(std::move(paths)), changed_(std::move(changed)) {
    auto* editor_layout = SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(20), 10));
    SetPreferredSize(gfx::Size(440, 450));
    Label(this, Text(paths_.empty() ? IDS_CRFILES_TAGS_MANAGE : IDS_CRFILES_TAGS_HINT));
    auto* row = AddChildView(std::make_unique<views::View>());
    auto* create_layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
    create_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    row->SetPreferredSize(gfx::Size(380, 36));
    create_ = row->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&TagEditor::BeginCreate, weak_.GetWeakPtr()), Text(IDS_CRFILES_TAG_CREATE)));
    create_->SetCornerRadius(16); create_->SetCustomPadding(gfx::Insets::VH(6, 12));
    input_ = row->AddChildView(std::make_unique<views::View>());
    input_->SetPreferredSize(gfx::Size(300, 32));
    input_->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 16));
    auto* layout = input_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(0, 8), 4));
    name_ = input_->AddChildView(std::make_unique<views::Textfield>());
    name_->SetPlaceholderText(Text(IDS_CRFILES_TAG_NAME)); name_->SetAccessibleName(Text(IDS_CRFILES_TAG_NAME));
    name_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(4, 4)));
    name_->SetBackgroundColor(ui::kColorSysSurfaceVariant);
    name_->set_controller(this);
    layout->SetFlexForView(name_, 1);
    save_ = input_->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
        base::BindRepeating(&TagEditor::Create, weak_.GetWeakPtr()), kAddTagIcon, 18,
        ui::kColorSysPrimary, ui::kColorIconDisabled, ui::kColorSysPrimary));
    save_->SetPreferredSize(gfx::Size(28, 28)); save_->SetTooltipText(Text(IDS_CRFILES_TAG_CREATE));
    save_->SetAccessibleName(Text(IDS_CRFILES_TAG_CREATE)); save_->SetEnabled(false);
    input_->SetVisible(false);
    auto* scroll = AddChildView(std::make_unique<views::ScrollView>());
    editor_layout->SetFlexForView(scroll, 1);
    scroll->ClipHeightTo(0, 300); scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    list_ = scroll->SetContents(std::make_unique<views::View>());
    list_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
    error_ = Label(this, {});
    Reload();
  }
  void AddedToWidget() override {
    views::View::AddedToWidget();
    outside_click_ = std::make_unique<TagInputOutsideClick>(GetWidget()->GetRootView(), input_,
        base::BindRepeating(&TagEditor::CollapseCreate, weak_.GetWeakPtr()));
  }
  void RemovedFromWidget() override {
    outside_click_.reset();
    views::View::RemovedFromWidget();
  }
  void ContentsChanged(views::Textfield*, const std::u16string&) override { UpdateSave(); }
 private:
  bool HasName() const {
    std::u16string trimmed;
    base::TrimWhitespace(name_->GetText(), base::TRIM_ALL, &trimmed);
    return !trimmed.empty();
  }
  void UpdateSave() { save_->SetEnabled(!busy_ && HasName()); }
  void Busy(bool busy) {
    busy_ = busy; create_->SetEnabled(!busy); name_->SetEnabled(!busy);
    list_->SetEnabled(!busy); UpdateSave();
  }
  void CollapseCreate() {
    input_->SetVisible(false); create_->SetVisible(true);
  }
  void BeginCreate() {
    create_->SetVisible(false); input_->SetVisible(true);
    UpdateSave(); name_->RequestFocus();
  }
  void Reload() {
    Busy(true);
    task_runner_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&ReadFileTags),
        base::BindOnce(&TagEditor::Loaded, weak_.GetWeakPtr()));
  }
  void Loaded(TagsSnapshot snapshot) {
    Busy(false); chips_.clear(); list_->RemoveAllChildViews();
    if (!snapshot.ok) { error_->SetText(Text(IDS_CRFILES_READ_ERROR)); return; }
    views::View* row = nullptr;
    int row_width = 0;
    for (const auto& tag : snapshot.tags) {
      size_t count = 0;
      for (const auto& path : paths_)
        count += std::ranges::find(tag.paths, path.NormalizePathSeparators().StripTrailingSeparators()) != tag.paths.end();
      const bool active = !paths_.empty() && count == paths_.size();
      const bool mixed = count > 0 && count < paths_.size();
      auto chip = std::make_unique<TagToggleButton>(base::UTF8ToUTF16(tag.name), active, mixed,
          base::BindRepeating(&TagEditor::Apply, weak_.GetWeakPtr(), tag.id));
      const int chip_width = chip->MaximumWidth();
      chip->SetEnabled(!paths_.empty());
      if (!row || row_width + 4 + chip_width > 380) {
        row = list_->AddChildView(std::make_unique<views::View>());
        auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
        layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
        row->SetPreferredSize(gfx::Size(380, 35));
        row_width = 0;
      }
      row_width += (row_width ? 4 : 0) + chip_width;
      chips_[tag.id] = row->AddChildView(std::move(chip));
    }
    if (snapshot.tags.empty()) Label(list_, Text(IDS_CRFILES_TAGS_EMPTY));
    InvalidateLayout();
  }
  void Create() {
    if (busy_ || !HasName()) return;
    Busy(true); error_->SetText({});
    task_runner_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&CreateFileTag, base::UTF16ToUTF8(name_->GetText())),
        base::BindOnce([](base::WeakPtr<TagEditor> view, TagWriteResult result) {
          if (!view) return;
          if (result == TagWriteResult::kOk) {
            view->name_->SetText({}); view->input_->SetVisible(false); view->create_->SetVisible(true);
            if (view->changed_) view->changed_.Run();
            view->Reload();
          } else { view->Busy(false); view->error_->SetText(Text(result == TagWriteResult::kInvalidName ? IDS_CRFILES_TAG_NAME_INVALID : IDS_CRFILES_CONFIG_ERROR)); }
        }, weak_.GetWeakPtr()));
  }
  void Apply(int64_t id, bool enabled) {
    if (busy_ || paths_.empty()) {
      if (auto chip = chips_.find(id); chip != chips_.end()) chip->second->FinishSave(false);
      return;
    }
    error_->SetText({});
    std::vector<int64_t> add, remove;
    (enabled ? add : remove).push_back(id);
    task_runner_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&EditFileTags, paths_, std::move(add), std::move(remove)),
        base::BindOnce([](base::WeakPtr<TagEditor> view, int64_t tag_id, TagWriteResult result) {
          if (!view) return;
          if (auto chip = view->chips_.find(tag_id); chip != view->chips_.end())
            chip->second->FinishSave(result == TagWriteResult::kOk);
          if (result != TagWriteResult::kOk)
            view->error_->SetText(Text(result == TagWriteResult::kLimit ? IDS_CRFILES_TAG_LIMIT : IDS_CRFILES_CONFIG_ERROR));
          else if (view->changed_) view->changed_.Run();
        }, weak_.GetWeakPtr(), id));
  }
  std::vector<base::FilePath> paths_;
  base::RepeatingClosure changed_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_ = base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()});
  std::map<int64_t, raw_ptr<TagToggleButton>> chips_;
  std::unique_ptr<TagInputOutsideClick> outside_click_;
  bool busy_ = false;
  raw_ptr<views::Textfield> name_ = nullptr;
  raw_ptr<views::View> list_ = nullptr, input_ = nullptr;
  raw_ptr<views::Label> error_ = nullptr;
  raw_ptr<views::MdTextButton> create_ = nullptr;
  raw_ptr<views::ImageButton> save_ = nullptr;
  base::WeakPtrFactory<TagEditor> weak_{this};
};
BEGIN_METADATA(TagEditor)
END_METADATA
// Resolve inherited tags for display only; keep stored associations path-local.
TagsSnapshot ReadPropertiesTags(const std::vector<base::FilePath>& paths) {
  auto snapshot = ReadFileTags();
  if (!snapshot.ok) return snapshot;
  for (auto& tag : snapshot.tags) {
    std::vector<base::FilePath> matched;
    for (const auto& tagged_path : tag.paths) {
      const auto ancestor = tagged_path.NormalizePathSeparators().StripTrailingSeparators();
      const bool directory = base::DirectoryExists(ancestor);
      for (const auto& original : paths) {
        const auto path = original.NormalizePathSeparators().StripTrailingSeparators();
        if (ancestor == path || (directory && ancestor.IsParent(path))) matched.push_back(path);
      }
    }
    tag.paths = std::move(matched);
  }
  return snapshot;
}
class TagsProperty : public views::View {
  METADATA_HEADER(TagsProperty, views::View)
 public:
  TagsProperty(std::vector<base::FilePath> paths, OpenTagCallback open_tag)
      : paths_(std::move(paths)), open_tag_(std::move(open_tag)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 6));
    Label(this, Text(IDS_CRFILES_TAGS) + u":");
    auto* scroll = AddChildView(std::make_unique<views::ScrollView>());
    scroll->SetUseContentsPreferredSize(true);
    scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    scroll->ClipHeightTo(0, 112);
    scroll->SetBackgroundColor(std::nullopt);
    auto content = std::make_unique<views::View>();
    content->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
    tags_ = scroll->SetContents(std::move(content));
    Label(tags_, Text(IDS_CRFILES_LOADING));
    static_cast<views::BoxLayout*>(GetLayoutManager())->SetFlexForView(scroll, 1);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()}, base::BindOnce(&ReadPropertiesTags, paths_),
        base::BindOnce(&TagsProperty::Loaded, weak_.GetWeakPtr()));
  }
 private:
  void Loaded(TagsSnapshot snapshot) {
    tags_->RemoveAllChildViews();
    views::View* row = nullptr;
    size_t matched = 0;
    int row_width = 0;
    if (snapshot.ok) for (const auto& tag : snapshot.tags) {
      size_t count = 0;
      for (const auto& path : paths_)
        count += std::ranges::find(tag.paths, path.NormalizePathSeparators().StripTrailingSeparators()) != tag.paths.end();
      if (!count) continue;
      ++matched;
      auto name = base::UTF8ToUTF16(tag.name);
      if (count < paths_.size()) name += u" (" + base::NumberToString16(count) + u")";
      auto chip = std::make_unique<views::MdTextButton>(base::BindRepeating(
          [](base::WeakPtr<TagsProperty> view, int64_t id, std::string tag_name) {
            if (!view || !view->open_tag_) return;
            auto open = view->open_tag_;
            if (view->GetWidget()) view->GetWidget()->Close();
            open.Run(id, std::move(tag_name));
          }, weak_.GetWeakPtr(), tag.id, tag.name), name);
      chip->SetCornerRadius(13);
      chip->SetMinSize(gfx::Size());
      chip->SetCustomPadding(gfx::Insets::VH(3, 8));
      const int chip_width = std::clamp(chip->GetPreferredSize({}).width(), 26, 180);
      chip->SetPreferredSize(gfx::Size(chip_width, 26));
      chip->SetTooltipText(name);
      chip->SetBgColorIdOverride(ui::kColorSysSecondaryContainer);
      chip->SetEnabledTextColors(ui::kColorSysOnSecondaryContainer);
      if (!row || row_width + 4 + chip_width > 348) {
        row = tags_->AddChildView(std::make_unique<views::View>());
        row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
        row_width = 0;
      }
      row_width += (row_width ? 4 : 0) + chip_width;
      row->AddChildView(std::move(chip));
    }
    if (!matched) Label(tags_, snapshot.ok ? std::u16string(u"—") : Text(IDS_CRFILES_READ_ERROR));
    PreferredSizeChanged();
  }
  std::vector<base::FilePath> paths_;
  OpenTagCallback open_tag_;
  raw_ptr<views::View> tags_ = nullptr;
  base::WeakPtrFactory<TagsProperty> weak_{this};
};
BEGIN_METADATA(TagsProperty)
END_METADATA

}
void ShowFileTagEditor(views::Widget* parent, std::vector<base::FilePath> paths, base::RepeatingClosure changed) {
  if (!parent || parent->IsClosed()) return;
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->SetTitle(Text(paths.empty() ? IDS_CRFILES_TAG_CREATE : IDS_CRFILES_TAGS));
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
  dialog->SetButtonLabel(ui::mojom::DialogButton::kCancel, Text(IDS_CRFILES_CLOSE));
  dialog->SetContentsView(std::make_unique<TagEditor>(std::move(paths), std::move(changed)));
  ShowCrFilesDialog(std::move(dialog), parent);
}
std::unique_ptr<views::View> CreateTagsPropertyView(std::vector<base::FilePath> paths, OpenTagCallback open_tag) { return std::make_unique<TagsProperty>(std::move(paths), std::move(open_tag)); }
}
