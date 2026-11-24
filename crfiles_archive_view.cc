// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_archive_view.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "crFiles/grit/crfiles_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/events/event.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"

namespace crfiles {
struct ArchiveView::ExportJob {
  base::FilePath archive, folder;
  base::File::Info source;
  std::vector<ArchiveRow> rows;
  Files files;
  std::shared_ptr<ArchiveProgress> progress = std::make_shared<ArchiveProgress>();
  base::OnceCallback<void(Files)> ready;
  bool background = true;

  void Extract(std::string password) {
    progress->needs_password = false;
    while (!progress->canceled && files.size() < rows.size()) {
      const auto& row = rows[files.size()];
      auto file = PreviewArchiveMember(archive, source, folder.Append(row.name), row.ordinal, password, progress);
      if (!file) return;
      files.push_back(std::move(file));
    }
  }
};
ArchiveView::ArchiveView(base::FilePath archive, base::RepeatingClosure changed,
    base::RepeatingCallback<void(std::shared_ptr<ArchivePreview>)> open)
    : archive_(std::move(archive)), changed_(std::move(changed)), open_(std::move(open)) {
  SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical));
  status_ = AddChildView(std::make_unique<views::Label>());
  password_ = AddChildView(std::make_unique<views::Textfield>());
  password_->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
  password_->SetAccessibleName(l10n_util::GetStringUTF16(IDS_CRFILES_ARCHIVE_PASSWORD));
  password_->set_controller(this);
  password_->SetVisible(false);
  status_->SetVisible(false);
}
ArchiveView::~ArchiveView() { CancelWork(); }
void ArchiveView::ReportOpenError() { ShowStatus(IDS_CRFILES_OPEN_ERROR); }
void ArchiveView::ShowStatus(int resource) {
  status_->SetText(resource ? l10n_util::GetStringUTF16(resource) : std::u16string());
  status_->SetVisible(resource != 0);
}
void ArchiveView::CancelWork(bool allow_background) {
  if (progress_) progress_->canceled = true;
  weak_.InvalidateWeakPtrs();
  progress_.reset();
  // A clipboard or share request owns its files after navigation. A pending
  // password prompt and a preview both belong to this particular directory.
  if (export_job_ && (!allow_background || !export_job_->background || retry_export_))
    export_job_->progress->canceled = true;
  retry_export_.Reset();
  export_job_.reset();
  password_->SetText({});
  password_->SetVisible(false);
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
  export_job_->rows.reserve(indices.size());
  for (auto index : indices) export_job_->rows.push_back(rows[index]);
  export_job_->ready = std::move(ready);
  RunExport(weak_.GetWeakPtr(), export_job_, {});
}
bool ArchiveView::HandleKeyEvent(views::Textfield*, const ui::KeyEvent& event) {
  if (!retry_export_ || event.type() != ui::EventType::kKeyPressed ||
      event.key_code() != ui::VKEY_RETURN || password_->IsIMEComposing()) return false;
  std::move(retry_export_).Run(base::UTF16ToUTF8(password_->GetText()));
  return true;
}
void ArchiveView::RunExport(base::WeakPtr<ArchiveView> view, std::shared_ptr<ExportJob> job,
                            std::string password) {
  view->password_->SetText({});
  view->password_->SetVisible(false);
  view->ShowStatus(IDS_CRFILES_EXTRACTING);
  auto extract = base::BindOnce(&ExportJob::Extract, job, std::move(password));
  base::ThreadPool::PostTaskAndReply(FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
      std::move(extract), base::BindOnce([](base::WeakPtr<ArchiveView> view, std::shared_ptr<ExportJob> job) {
        if (job->progress->canceled) return;
        if (view && job->progress->needs_password) {
          view->retry_export_ = base::BindOnce(&ArchiveView::RunExport, view, job);
          view->ShowStatus(IDS_CRFILES_ARCHIVE_PASSWORD_REQUIRED);
          view->password_->SetVisible(true);
          view->password_->RequestFocus();
          return;
        }
        const bool complete = job->files.size() == job->rows.size();
        if (view) {
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
