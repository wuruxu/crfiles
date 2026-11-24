// Copyright 2026 The Chromium Authors
#ifndef CRFILES_ARCHIVE_VIEW_H_
#define CRFILES_ARCHIVE_VIEW_H_

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "crFiles/crfiles_archive.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/view.h"

namespace views {
class Label;
}
namespace crfiles {
// Owns the archive index and the transient status/password controls. FilesView
// presents the indexed rows using its regular folder layout.
class ArchiveView : public views::View, public views::TextfieldController {
  METADATA_HEADER(ArchiveView, views::View)
 public:
  ArchiveView(base::FilePath archive, base::RepeatingClosure changed,
              base::RepeatingCallback<void(std::shared_ptr<ArchivePreview>)> open);
  ~ArchiveView() override;
  void Refresh();
  void Suspend();
  void Resume();
  bool Navigate(const base::FilePath& folder);
  bool Go(int delta);
  bool Up();
  const std::vector<ArchiveRow>& Rows() const;
  using Files = std::vector<std::shared_ptr<ArchivePreview>>;
  void OpenRow(size_t index);
  void ExportFiles(std::vector<size_t> indices, base::OnceCallback<void(Files)> ready,
                   bool keep_after_navigation = true);
  void ReportOpenError();
  bool HasForward() const { return position_ + 1 < history_.size(); }
  const base::FilePath& archive() const { return archive_; }
  const base::FilePath& folder() const { return history_[position_]; }

 private:
  bool HandleKeyEvent(views::Textfield*, const ui::KeyEvent& event) override;
  struct ExportJob;
  static void RunExport(base::WeakPtr<ArchiveView> view, std::shared_ptr<ExportJob> job, std::string password);
  void ShowFolder();
  void ShowStatus(int resource);
  void CancelWork(bool allow_background = true);
  base::FilePath archive_;
  std::vector<base::FilePath> history_{base::FilePath()};
  size_t position_ = 0;
  std::unique_ptr<ArchiveListing> listing_;
  std::shared_ptr<ArchiveProgress> progress_;
  std::shared_ptr<ExportJob> export_job_;
  base::OnceCallback<void(std::string)> retry_export_;
  raw_ptr<views::Label> status_;
  raw_ptr<views::Textfield> password_;
  base::RepeatingClosure changed_;
  base::RepeatingCallback<void(std::shared_ptr<ArchivePreview>)> open_;
  base::WeakPtrFactory<ArchiveView> weak_{this};
};
}
#endif
