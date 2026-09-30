// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_archive_dialog.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_archive.h"
#include "crFiles/grit/crfiles_strings.h"
#include <algorithm>
#include "components/vector_icons/vector_icons.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/utf_string_conversions.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/base/ime/text_input_type.h"
#include "ui/color/color_id.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/progress_bar.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/border.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/client_view.h"
#include "ui/views/view_observer.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
namespace crfiles {
gfx::Rect GetArchiveBubbleBounds(views::BubbleDialogDelegate& bubble) {
  auto* frame = bubble.GetBubbleFrameView();
  const bool old_visible_arrow = frame->GetDisplayVisibleArrow();
  // Creation and anchor updates can leave the border with Arrow::NONE. Only
  // enable the visible arrow after selecting a concrete placement direction.
  frame->SetDisplayVisibleArrow(false);
  auto* anchor_view = bubble.GetAnchorView();
  if (!anchor_view || !anchor_view->GetWidget()) return bubble.views::BubbleDialogDelegate::GetBubbleBounds();
  auto window = anchor_view->GetWidget()->GetClientAreaBoundsInScreen();
  window.Inset(8);
  auto pane = anchor_view->GetVisibleBounds();
  views::View::ConvertRectToScreen(anchor_view, &pane);
  pane.Inset(8);
  pane.Intersect(window);
  auto* border = frame->bubble_border();
  border->set_md_shadow_elevation(12);
  const auto old_arrow = border->arrow();
  const int old_offset = border->arrow_offset();
  const auto anchor = bubble.GetAnchorRect();
  const auto client_size = bubble.GetWidget()->client_view()->GetPreferredSize({});
  auto finish = [&](gfx::Rect bounds) {
    if (old_arrow != border->arrow() || old_offset != border->arrow_offset() ||
        old_visible_arrow != frame->GetDisplayVisibleArrow()) {
      frame->InvalidateLayout(); frame->SchedulePaint();
    }
    return bounds;
  };
  // All coordinates belong to the same window. Unlike the default Linux
  // placement, this also works when Wayland has no global screen coordinates.
  if (views::BubbleBorder::has_arrow(bubble.arrow())) {
    for (const auto& area : {pane, window}) {
      if (area.IsEmpty()) continue;
      for (const auto arrow : {views::BubbleBorder::TOP_CENTER, views::BubbleBorder::BOTTOM_CENTER,
                              views::BubbleBorder::LEFT_CENTER, views::BubbleBorder::RIGHT_CENTER}) {
        border->set_arrow(arrow); border->set_arrow_offset(0);
        frame->SetDisplayVisibleArrow(true);
        auto bounds = frame->GetUpdatedWindowBounds(anchor, arrow, client_size, false);
        if (bounds.width() > area.width() || bounds.height() > area.height()) continue;
        const bool horizontal = views::BubbleBorder::is_arrow_on_horizontal(arrow);
        const int shift = horizontal ? std::clamp(bounds.x(), area.x(), area.right() - bounds.width()) - bounds.x() :
            std::clamp(bounds.y(), area.y(), area.bottom() - bounds.height()) - bounds.y();
        border->set_arrow_offset(-shift);
        bounds = frame->GetUpdatedWindowBounds(anchor, arrow, client_size, false);
        if (area.Contains(bounds)) return finish(bounds);
      }
    }
  }
  // If no side can accommodate the bubble, prioritize its contents over an
  // arrow pointing through the bubble. Do not shrink and clip its controls.
  frame->SetDisplayVisibleArrow(false);
  border->set_arrow(views::BubbleBorder::NONE); border->set_arrow_offset(0);
  auto bounds = frame->GetUpdatedWindowBounds(anchor, views::BubbleBorder::NONE, client_size, false);
  bounds.set_x(std::clamp(bounds.x(), window.x(), std::max(window.x(), window.right() - bounds.width())));
  bounds.set_y(std::clamp(bounds.y(), window.y(), std::max(window.y(), window.bottom() - bounds.height())));
  return finish(bounds);
}
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
class ArchiveDialog : public views::BubbleDialogDelegate, public views::ViewObserver {
 public:
  ArchiveDialog(views::View* anchor, base::FilePath archive, base::FilePath destination, bool same_name_folder,
      base::RepeatingCallback<gfx::Rect()> icon_bounds,
      base::OnceCallback<void(FileOperationResult)> completion, std::vector<base::FilePath> sources = {});
  ~ArchiveDialog() override;
  void Show(std::unique_ptr<ArchiveDialog> self) {
    // Keep the delegate owned until the widget is destroyed. Native widget
    // ownership alone does not transfer ownership of a BubbleDialogDelegate.
    widget_ = CreateCrFilesBubble(this, base::BindOnce(
        [](std::unique_ptr<ArchiveDialog> dialog, views::Widget::ClosedReason) {
          dialog->StopUpdates();
          dialog->SetAnchorView(nullptr);
          dialog->widget_.reset();
          // A button callback may still be running on the delegate's stack.
          base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, std::move(dialog));
        }, std::move(self)));
    widget_->ShowInactive();
    Begin();
  }
  void Begin() {
    UpdateAnchor();
    anchor_timer_.Start(FROM_HERE, base::Milliseconds(100),
        base::BindRepeating(&ArchiveDialog::UpdateAnchor, weak_.GetWeakPtr()));
    Start();
  }
  gfx::Rect GetAnchorRect() const override {
    return icon_rect_.IsEmpty() ? views::BubbleDialogDelegate::GetAnchorRect() : icon_rect_;
  }
  gfx::Rect GetBubbleBounds() override { return GetArchiveBubbleBounds(*this); }
  void OnViewHierarchyWillBeDeleted(views::View*) override {
    // OnViewIsDeleting runs after children have already been freed. Release
    // references while the observed contents still owns all of its controls.
    ReleaseContents();
  }

 private:
  void ReleaseContents() {
    StopUpdates();
    contents_observation_.Reset();
    password_ = nullptr;
    status_ = nullptr;
    current_file_ = nullptr;
    bar_ = nullptr;
    conflict_controls_ = nullptr;
  }

  void UpdateAnchor() {
    const auto bounds = icon_bounds_.Run();
    const bool visible = !bounds.IsEmpty();
    if (visible == icon_visible_ && (!visible || bounds == icon_rect_)) return;
    icon_visible_ = visible;
    if (visible) icon_rect_ = bounds;
    // Virtual rows can be destroyed and recreated during scrolling or refresh.
    // Keep the task alive, but never point an arrow at a different file.
    if (!visible && GetBubbleFrameView())
      GetBubbleFrameView()->SetDisplayVisibleArrow(false);
    SetArrowWithoutResizing(visible ? views::BubbleBorder::TOP_CENTER : views::BubbleBorder::NONE);
    OnAnchorBoundsChanged();
  }
  void StopUpdates() {
    progress_->canceled = true;
    timer_.Stop();
    anchor_timer_.Stop();
    weak_.InvalidateWeakPtrs();
  }
  bool Start() {
    if (started_) return false;
    started_ = true;
    progress_->needs_password = false;
    progress_->bytes = 0;
    auto password = base::UTF16ToUTF8(password_->GetText());
    password_->SetText(u""); password_->SetEnabled(false);
    password_->SetVisible(false);
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
    SetButtonEnabled(ui::mojom::DialogButton::kOk, false);
    bar_->SetVisible(true); bar_->SetValue(-1);
    UpdateProgress();
    SizeToContents();
    timer_.Start(FROM_HERE, base::Milliseconds(150),
        base::BindRepeating(&ArchiveDialog::UpdateProgress, weak_.GetWeakPtr()));
    auto task = sources_.empty() ?
        base::BindOnce(&ExtractLocalArchive, archive_, destination_, same_name_folder_, std::move(password), progress_) :
        base::BindOnce(&CompressLocalFiles, sources_, archive_, progress_);
    // Both archive operations wait for their IPC worker and join it before
    // cleaning staging files. Extraction also waits for conflict decisions.
    // MayBlock alone permits file I/O, not these base synchronization waits.
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::WithBaseSyncPrimitives(),
         base::TaskPriority::USER_VISIBLE, base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
        std::move(task),
        base::BindOnce([](base::WeakPtr<ArchiveDialog> dialog,
            base::OnceCallback<void(FileOperationResult)> completion, FileOperationResult result) {
          if (dialog && dialog->progress_->needs_password) {
            dialog->completion_ = std::move(completion);
            dialog->AskPassword();
            return;
          }
          if (dialog) dialog->Finished(result);
          std::move(completion).Run(std::move(result));
        }, weak_.GetWeakPtr(), std::move(completion_)));
    return false;
  }
  void AskPassword() {
    timer_.Stop();
    started_ = false;
    bar_->SetVisible(false);
    password_->SetVisible(true);
    password_->SetEnabled(true);
    status_->SetText(Text(IDS_CRFILES_ARCHIVE_PASSWORD_REQUIRED));
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) | static_cast<int>(ui::mojom::DialogButton::kCancel));
    SetButtonEnabled(ui::mojom::DialogButton::kOk, true);
    SizeToContents();
    GetWidget()->Activate();
    password_->RequestFocus();
  }
  void ResolveConflict(ArchiveConflictChoice choice) {
    if (displayed_conflict_.empty()) return;
    progress_->ResolveConflict(displayed_conflict_, choice);
    displayed_conflict_.clear();
    if (choice == ArchiveConflictChoice::kCancel) { GetWidget()->Close(); return; }
    conflict_controls_->SetVisible(false);
    bar_->SetVisible(true);
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
    SizeToContents();
  }
  void UpdateProgress() {
    const auto conflict = progress_->PendingConflict();
    if (!conflict.empty()) {
      if (displayed_conflict_ != conflict) {
        displayed_conflict_ = conflict;
        status_->SetText(l10n_util::GetStringFUTF16(IDS_CRFILES_EXTRACT_EXISTS, conflict.AsUTF16Unsafe()));
        bar_->SetVisible(false);
        conflict_controls_->SetVisible(true);
        set_fixed_width(std::max(360, conflict_controls_->GetPreferredSize({}).width() + 40 + margins().width()));
        SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
        SizeToContents();
      }
      return;
    }
    if (sources_.empty()) {
      status_->SetText(Text(IDS_CRFILES_EXTRACTING) + u" " +
          ui::FormatBytes(base::ByteSize(progress_->bytes.load())));
      return;
    }
    const auto path = progress_->CurrentPath();
    current_file_->SetText(path.BaseName().AsUTF16Unsafe());
    current_file_->SetTooltipText(path.AsUTF16Unsafe());
    if (!progress_->prepared.load()) {
      status_->SetText(Text(IDS_CRFILES_COMPRESS_PREPARING));
      bar_->SetValue(-1);
      return;
    }
    const auto total = progress_->total_bytes.load();
    const auto bytes = std::min(progress_->bytes.load(), total);
    const double fraction = total ? static_cast<double>(bytes) / total :
        static_cast<double>(progress_->entries_done.load()) / std::max(uint64_t{1}, progress_->total_entries.load());
    // ZIP finalization and publishing the archive must finish before 100%.
    const double value = std::clamp(fraction, 0.0, 0.99);
    bar_->SetValue(value);
    status_->SetText(Text(IDS_CRFILES_COMPRESSING) + u" " +
        base::NumberToString16(static_cast<int>(value * 100)) + u"%\n" +
        ui::FormatBytes(base::ByteSize(bytes)) + u" / " + ui::FormatBytes(base::ByteSize(total)));
    if (!progress_ready_) { progress_ready_ = true; SizeToContents(); }
  }
  void Finished(const FileOperationResult& result) {
    const bool failed = result.failed != 0;
    timer_.Stop();
    anchor_timer_.Stop();
    bar_->SetVisible(!sources_.empty() && !failed);
    if (!sources_.empty() && !failed) bar_->SetValue(1);
    current_file_->SetVisible(false);
    conflict_controls_->SetVisible(false);
    status_->SetText(!result.recovery_path.empty() ?
        l10n_util::GetStringFUTF16(IDS_CRFILES_EXTRACT_RECOVERY, result.recovery_path.AsUTF16Unsafe()) :
        !result.conflict_path.empty() ?
        l10n_util::GetStringFUTF16(IDS_CRFILES_EXTRACT_CONFLICT, result.conflict_path.AsUTF16Unsafe()) :
        Text(sources_.empty() ? (failed ? IDS_CRFILES_EXTRACT_ERROR : IDS_CRFILES_EXTRACT_SUCCESS) :
            (failed ? IDS_CRFILES_COMPRESS_ERROR : IDS_CRFILES_COMPRESS_SUCCESS)));
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
    SetButtonLabel(ui::mojom::DialogButton::kCancel, Text(IDS_CRFILES_CLOSE));
    set_close_on_deactivate(sources_.empty());
    SizeToContents();
    if (!failed && sources_.empty()) timer_.Start(FROM_HERE, base::Seconds(5),
        base::BindRepeating(&ArchiveDialog::Dismiss, weak_.GetWeakPtr()));
  }
  void Dismiss() { GetWidget()->Close(); }
  base::FilePath archive_, destination_;
  std::vector<base::FilePath> sources_;
  bool same_name_folder_;
  bool started_ = false;
  bool progress_ready_ = false;
  base::OnceCallback<void(FileOperationResult)> completion_;
  std::shared_ptr<ArchiveProgress> progress_;
  raw_ptr<views::Textfield> password_ = nullptr;
  raw_ptr<views::Label> status_ = nullptr;
  raw_ptr<views::Label> current_file_ = nullptr;
  raw_ptr<views::ProgressBar> bar_ = nullptr;
  raw_ptr<views::View> conflict_controls_ = nullptr;
  base::FilePath displayed_conflict_;
  base::RepeatingTimer timer_;
  base::RepeatingTimer anchor_timer_;
  base::RepeatingCallback<gfx::Rect()> icon_bounds_;
  gfx::Rect icon_rect_;
  bool icon_visible_ = false;
  std::unique_ptr<views::Widget> widget_;
  base::ScopedObservation<views::View, views::ViewObserver> contents_observation_{this};
  base::WeakPtrFactory<ArchiveDialog> weak_{this};
};
ArchiveDialog::ArchiveDialog(views::View* anchor, base::FilePath archive, base::FilePath destination, bool same_name_folder,
      base::RepeatingCallback<gfx::Rect()> icon_bounds,
      base::OnceCallback<void(FileOperationResult)> completion, std::vector<base::FilePath> sources)
      : views::BubbleDialogDelegate(anchor, views::BubbleBorder::NONE), archive_(std::move(archive)), destination_(std::move(destination)), sources_(std::move(sources)), same_name_folder_(same_name_folder),
        completion_(std::move(completion)), progress_(std::make_shared<ArchiveProgress>()), icon_bounds_(std::move(icon_bounds)) {
    SetTitle(Text(sources_.empty() ? IDS_CRFILES_EXTRACT : IDS_CRFILES_COMPRESS));
    set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    SetModalType(ui::mojom::ModalType::kNone);
    set_close_on_deactivate(false);
    set_adjust_if_offscreen(true);
    SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
    SetButtonLabel(ui::mojom::DialogButton::kOk, Text(IDS_CRFILES_EXTRACT));
    SetButtonLabel(ui::mojom::DialogButton::kCancel, Text(IDS_CRFILES_CANCEL));
    auto contents = std::make_unique<views::View>();
    set_fixed_width(360);
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(20), 12));
    auto add_label = [&](std::u16string text) {
      auto* label = contents->AddChildView(std::make_unique<views::Label>(text));
      label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true);
      return label;
    };
    add_label(archive_.BaseName().AsUTF16Unsafe());

    password_ = contents->AddChildView(std::make_unique<views::Textfield>());
    password_->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
    password_->SetPlaceholderText(Text(IDS_CRFILES_ARCHIVE_PASSWORD));
    password_->SetAccessibleName(Text(IDS_CRFILES_ARCHIVE_PASSWORD));
    password_->SetVisible(false);
    status_ = add_label(Text(sources_.empty() ? IDS_CRFILES_EXTRACTING : IDS_CRFILES_COMPRESSING));
    current_file_ = add_label(u"");
    current_file_->SetMultiLine(false);
    current_file_->SetElideBehavior(gfx::ELIDE_MIDDLE);
    current_file_->SetVisible(!sources_.empty());
    bar_ = contents->AddChildView(std::make_unique<views::ProgressBar>());
    bar_->SetVisible(false);
    conflict_controls_ = contents->AddChildView(std::make_unique<views::View>());
    conflict_controls_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 8));
    auto* choices = conflict_controls_->AddChildView(std::make_unique<views::View>());
    auto* choices_layout = choices->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    choices_layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    auto add_choice = [&](views::View* parent, int label, ArchiveConflictChoice choice) {
      auto* button = parent->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&ArchiveDialog::ResolveConflict, weak_.GetWeakPtr(), choice), Text(label)));
      button->SetStyle(ui::ButtonStyle::kTonal);
      button->SetCornerRadius(8);
      button->SetCustomPadding(gfx::Insets::VH(10, 16));
      button->SetMinSize(gfx::Size(88, 36));
      button->SetAnimationDuration(base::Milliseconds(160));
    };
    add_choice(choices, IDS_CRFILES_REPLACE_ALL, ArchiveConflictChoice::kReplaceAll);
    auto* single_choices = choices->AddChildView(std::make_unique<views::View>());
    single_choices->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
    add_choice(single_choices, IDS_CRFILES_REPLACE, ArchiveConflictChoice::kReplace);
    add_choice(single_choices, IDS_CRFILES_SKIP, ArchiveConflictChoice::kSkip);
    conflict_controls_->AddChildView(std::make_unique<views::Separator>());
    auto* cancel_row = conflict_controls_->AddChildView(std::make_unique<views::View>());
    cancel_row->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(12, 0, 0, 0)));
    auto* cancel_layout = cancel_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal));
    cancel_layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    auto* cancel_button = cancel_row->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&ArchiveDialog::ResolveConflict, weak_.GetWeakPtr(), ArchiveConflictChoice::kCancel),
        Text(IDS_CRFILES_CANCEL)));
    cancel_button->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kCloseIcon, ui::kColorSysOnSurface, 18));
    conflict_controls_->SetVisible(false);
    contents_observation_.Observe(contents.get());
    SetContentsView(std::move(contents));
    SetCancelCallback(base::BindOnce(&ArchiveDialog::StopUpdates, weak_.GetWeakPtr()));
    SetCloseCallback(base::BindOnce(&ArchiveDialog::StopUpdates, weak_.GetWeakPtr()));
    SetAcceptCallbackWithClose(base::BindRepeating([](base::WeakPtr<ArchiveDialog> dialog) {
      return !dialog || dialog->Start();
    }, weak_.GetWeakPtr()));
  }
ArchiveDialog::~ArchiveDialog() {
    // Also cover destruction of the delegate before its widget/contents.
    ReleaseContents();
    if (completion_) std::move(completion_).Run({});
  }
}
void ShowArchiveDialog(views::View* anchor, base::FilePath archive,
    base::FilePath destination, bool same_name_folder,
    base::RepeatingCallback<gfx::Rect()> icon_bounds,
    base::OnceCallback<void(FileOperationResult)> completion) {
  auto dialog = std::make_unique<ArchiveDialog>(anchor, std::move(archive), std::move(destination),
      same_name_folder, std::move(icon_bounds), std::move(completion));
  auto* delegate = dialog.get();
  delegate->Show(std::move(dialog));
}
void ShowCompressDialog(views::View* anchor, std::vector<base::FilePath> sources,
    base::FilePath target, base::RepeatingCallback<gfx::Rect()> filename_bounds,
    base::OnceCallback<void(FileOperationResult)> completion) {
  auto dialog = std::make_unique<ArchiveDialog>(anchor, target, target.DirName(), false,
      std::move(filename_bounds), std::move(completion), std::move(sources));
  auto* delegate = dialog.get();
  delegate->Show(std::move(dialog));
}
}
