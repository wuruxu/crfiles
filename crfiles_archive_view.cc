// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_archive_view.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_file_icons.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/single_button_row_layout.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/events/event.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/textfield/textfield.h"

namespace crfiles {
struct ArchiveView::ExportJob {
  base::FilePath archive, folder;
  base::File::Info source;
  std::vector<ArchiveRow> rows;
  std::optional<size_t> anchor_index;
  Files files;
  gfx::ImageSkia lock_icon;
  std::shared_ptr<ArchiveProgress> progress = std::make_shared<ArchiveProgress>();
  base::OnceCallback<void(Files)> ready;
  bool background = true;

  void Extract(std::string password) {
    progress->needs_password = false;
    while (!progress->canceled && files.size() < rows.size()) {
      const auto& row = rows[files.size()];
      auto file = PreviewArchiveMember(archive, source, folder.Append(row.name), row.ordinal, password, progress);
      if (!file) {
        if (progress->needs_password && lock_icon.isNull()) lock_icon = LoadArchiveLockIcon();
        return;
      }
      files.push_back(std::move(file));
    }
  }
};
ArchiveView::ArchiveView(base::FilePath archive, base::RepeatingClosure changed,
    base::RepeatingClosure status_changed, PasswordAnchor password_anchor,
    base::RepeatingCallback<void(std::shared_ptr<ArchivePreview>)> open)
    : archive_(std::move(archive)), changed_(std::move(changed)),
      status_changed_(std::move(status_changed)), password_anchor_(std::move(password_anchor)),
      open_(std::move(open)) {}
ArchiveView::~ArchiveView() { CancelWork(); }
void ArchiveView::ReportOpenError() { ShowStatus(IDS_CRFILES_OPEN_ERROR); }
void ArchiveView::ShowStatus(int resource) {
  status_resource_ = resource;
  status_changed_.Run();
}
void ArchiveView::CancelWork(bool allow_background) {
  if (progress_) progress_->canceled = true;
  weak_.InvalidateWeakPtrs();
  progress_.reset();
  // A clipboard or share request owns its files after navigation. A pending
  // password prompt and a preview both belong to this particular directory.
  if (export_job_ && (!allow_background || !export_job_->background || retry_export_ || password_widget_))
    export_job_->progress->canceled = true;
  retry_export_.Reset();
  export_job_.reset();
  ClosePassword();
}
void ArchiveView::Suspend() { CancelWork(); }
void ArchiveView::Resume() {
  if (listing_) ShowFolder();
  else Refresh();
}
void ArchiveView::Refresh() {
  CancelWork();
  listing_.reset();
  ShowStatus(IDS_CRFILES_LOADING);
  changed_.Run();
  progress_ = std::make_shared<ArchiveProgress>();
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ListArchive, archive_, progress_),
      base::BindOnce([](base::WeakPtr<ArchiveView> view, std::unique_ptr<ArchiveListing> listing) {
        if (!view) return;
        view->progress_.reset();
        if (!listing) {
          view->history_ = {base::FilePath()};
          view->position_ = 0;
          view->ShowStatus(IDS_CRFILES_EXTRACT_ERROR);
          view->changed_.Run();
          return;
        }
        view->listing_ = std::move(listing);
        if (!view->listing_->folders.contains(view->folder())) {
          view->history_ = {base::FilePath()};
          view->position_ = 0;
        }
        view->ShowFolder();
      }, weak_.GetWeakPtr()));
}
const std::vector<ArchiveRow>& ArchiveView::Rows() const {
  static const base::NoDestructor<std::vector<ArchiveRow>> empty;
  if (listing_) {
    auto it = listing_->folders.find(folder());
    if (it != listing_->folders.end()) return it->second;
  }
  return *empty;
}
void ArchiveView::ShowFolder() {
  CancelWork();
  ShowStatus(Rows().empty() ? IDS_CRFILES_EMPTY : 0);
  changed_.Run();
}
bool ArchiveView::Navigate(const base::FilePath& path) {
  if (folder() == path) return true;
  if (!listing_ || !listing_->folders.contains(path)) return false;
  history_.resize(position_ + 1);
  history_.push_back(path);
  ++position_;
  ShowFolder();
  return true;
}
bool ArchiveView::Go(int delta) {
  if ((delta < 0 && !position_) || (delta > 0 && !HasForward())) return false;
  if (!listing_) return false;
  position_ = delta < 0 ? position_ - 1 : position_ + 1;
  ShowFolder();
  return true;
}
bool ArchiveView::Up() {
  if (!listing_ || folder().empty()) return false;
  auto parent = folder().DirName();
  Navigate(parent == base::FilePath(FILE_PATH_LITERAL(".")) ? base::FilePath() : parent);
  return true;
}
void ArchiveView::OpenRow(size_t index) {
  if (index >= Rows().size()) return;
  const auto item = Rows()[index];
  if (item.directory) Navigate(folder().Append(item.name));
  else if (!item.link) {
    ExportFiles({index}, base::BindOnce([](decltype(open_) open, Files files) {
      open.Run(std::move(files.front()));
    }, open_), false);
  }
}
void ArchiveView::ExportFiles(std::vector<size_t> indices, base::OnceCallback<void(Files)> ready,
                              bool keep_after_navigation) {
  const auto& rows = Rows();
  if (indices.empty() || std::ranges::any_of(indices, [&](size_t index) {
        return index >= rows.size() || rows[index].directory || rows[index].link;
      })) return;
  CancelWork(false);
  export_job_ = std::make_shared<ExportJob>();
  export_job_->background = keep_after_navigation;
  export_job_->archive = archive_;
  export_job_->folder = folder();
  export_job_->source = listing_->source;
  if (indices.size() == 1) export_job_->anchor_index = indices.front();
  export_job_->rows.reserve(indices.size());
  for (auto index : indices) export_job_->rows.push_back(rows[index]);
  export_job_->ready = std::move(ready);
  RunExport(weak_.GetWeakPtr(), export_job_, {});
}
void ArchiveView::ShowPassword() {
  if (!password_widget_) {
    auto* anchor = password_anchor_.Run(export_job_->anchor_index);
    if (!anchor) {
      CancelPassword();
      return;
    }
    password_delegate_ = std::make_unique<views::BubbleDialogDelegate>(anchor,
        anchor == this ? views::BubbleBorder::BOTTOM_CENTER : views::BubbleBorder::TOP_CENTER);
    password_delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    password_delegate_->set_adjust_if_offscreen(true);
    password_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    password_delegate_->SetShowTitle(true);
    password_delegate_->SetShowCloseButton(true);
    const auto prompt = l10n_util::GetStringUTF16(IDS_CRFILES_ARCHIVE_PASSWORD_REQUIRED);
    password_delegate_->SetTitle(prompt);
    if (!export_job_->lock_icon.isNull()) {
      password_delegate_->SetIcon(ui::ImageModel::FromImageSkia(export_job_->lock_icon));
      password_delegate_->SetShowIcon(true);
    }
    password_delegate_->set_frame_margins({.contents = gfx::Insets::TLBR(6, 12, 10, 12),
                                           .title = gfx::Insets::TLBR(10, 12, 0, 12)});
    password_delegate_->set_fixed_width(std::clamp(parent()->GetContentsBounds().width() - 32, 192, 360));
    auto contents = std::make_unique<views::View>();
    auto* row = contents->SetLayoutManager(std::make_unique<SingleButtonRowLayout>(gfx::Insets(), 6));
    row->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kEnd);
    password_ = contents->AddChildView(std::make_unique<views::Textfield>());
    row->SetFlexForView(password_, 1);
    password_->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
    password_->SetAccessibleName(prompt);
    submit_password_button_ = contents->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&ArchiveView::SubmitPassword, weak_.GetWeakPtr()),
        password_delegate_->GetDialogButtonLabel(ui::mojom::DialogButton::kOk)));
    submit_password_button_->SetEnabled(false);
    password_->set_controller(this);
    password_delegate_->SetInitiallyFocusedView(password_);
    password_delegate_->SetContentsView(std::move(contents));
    password_widget_ = CreateCrFilesBubble(password_delegate_.get(),
        base::IgnoreArgs<views::Widget::ClosedReason>(
            base::BindOnce(&ArchiveView::CancelPassword, weak_.GetWeakPtr())));
    password_delegate_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    password_delegate_->SizeToContents();
    password_widget_->Show();
  } else {
    password_->SetReadOnly(false);
    password_->SetInvalid(true);
  }
  password_->RequestFocus();
  ShowStatus(0);
}
void ArchiveView::UpdatePasswordAnchor() {
  if (!password_widget_ || !export_job_) return;
  auto* anchor = password_anchor_.Run(export_job_->anchor_index);
  if (anchor && anchor != password_delegate_->GetAnchorView()) {
    password_delegate_->SetAnchorView(anchor);
    password_delegate_->SetArrow(anchor == this ? views::BubbleBorder::BOTTOM_CENTER : views::BubbleBorder::TOP_CENTER);
  }
}
void ArchiveView::CancelPassword() {
  CancelWork(false);
  ShowStatus(0);
}
void ArchiveView::ClosePassword() {
  if (password_) password_->set_controller(nullptr);
  submit_password_button_ = nullptr;
  password_ = nullptr;
  if (password_delegate_) password_delegate_->SetAnchorView(nullptr);
  if (password_widget_) {
    password_widget_->MakeCloseSynchronous({});
    password_widget_.reset();
  }
  password_delegate_.reset();
}
void ArchiveView::VisibilityChanged(views::View* from, bool visible) {
  views::View::VisibilityChanged(from, visible);
  if (!visible && password_widget_) CancelPassword();
}
void ArchiveView::ContentsChanged(views::Textfield*, const std::u16string& text) {
  submit_password_button_->SetEnabled(retry_export_ && !text.empty());
}
void ArchiveView::SubmitPassword() {
  if (!retry_export_ || password_->IsIMEComposing() || password_->GetText().empty()) return;
  auto password = base::UTF16ToUTF8(password_->GetText());
  submit_password_button_->SetEnabled(false);
  password_->SetText({});
  password_->SetInvalid(false);
  password_->SetReadOnly(true);
  std::move(retry_export_).Run(std::move(password));
}
bool ArchiveView::HandleKeyEvent(views::Textfield*, const ui::KeyEvent& event) {
  if (event.type() != ui::EventType::kKeyPressed || password_->IsIMEComposing()) return false;
  if (event.key_code() == ui::VKEY_ESCAPE) {
    export_job_->progress->canceled = true;
    password_widget_->CloseWithReason(views::Widget::ClosedReason::kEscKeyPressed);
    return true;
  }
  if (event.key_code() != ui::VKEY_RETURN || !retry_export_) return false;
  SubmitPassword();
  return true;
}
void ArchiveView::RunExport(base::WeakPtr<ArchiveView> view, std::shared_ptr<ExportJob> job,
                            std::string password) {
  view->ShowStatus(IDS_CRFILES_EXTRACTING);
  auto extract = base::BindOnce(&ExportJob::Extract, job, std::move(password));
  base::ThreadPool::PostTaskAndReply(FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
      std::move(extract), base::BindOnce([](base::WeakPtr<ArchiveView> view, std::shared_ptr<ExportJob> job) {
        if (job->progress->canceled || (view && view->password_widget_ && view->password_widget_->IsClosed())) return;
        if (view && job->progress->needs_password) {
          view->retry_export_ = base::BindOnce(&ArchiveView::RunExport, view, job);
          view->ShowPassword();
          return;
        }
        const bool complete = job->files.size() == job->rows.size();
        if (view) {
          view->ClosePassword();
          view->export_job_.reset();
          view->ShowStatus(complete ? 0 : IDS_CRFILES_EXTRACT_ERROR);
        }
        if (complete) std::move(job->ready).Run(std::move(job->files));
        else if (!view && job->background) std::move(job->ready).Run({});
      }, view, std::move(job)));
}
BEGIN_METADATA(ArchiveView)
END_METADATA
}
