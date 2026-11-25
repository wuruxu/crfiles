// Copyright 2026 The Chromium Authors
#ifndef CRFILES_ARCHIVE_VIEW_H_
#define CRFILES_ARCHIVE_VIEW_H_

#include <optional>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "crFiles/crfiles_archive.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/controls/textfield/textfield_controller.h"

namespace views {
class BubbleDialogDelegate;
class MdTextButton;
class Widget;
}
namespace crfiles {
// Owns the archive index and the transient status/password controls. FilesView
// presents the indexed rows using its regular folder layout.
class ArchiveView : public views::View, public views::TextfieldController {
  METADATA_HEADER(ArchiveView, views::View)
 public:
  using PasswordAnchor = base::RepeatingCallback<views::View*(std::optional<size_t>)>;
  ArchiveView(base::FilePath archive, base::RepeatingClosure changed,
              base::RepeatingClosure status_changed, PasswordAnchor password_anchor,
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
  void UpdatePasswordAnchor();
  int status_resource() const { return status_resource_; }
  bool NeedsFooter() const { return status_resource_ || password_widget_; }
  bool HasForward() const { return position_ + 1 < history_.size(); }
  const base::FilePath& archive() const { return archive_; }
  const base::FilePath& folder() const { return history_[position_]; }

 private:
  void VisibilityChanged(views::View* from, bool visible) override;
  void ContentsChanged(views::Textfield* sender, const std::u16string& text) override;
  bool HandleKeyEvent(views::Textfield*, const ui::KeyEvent& event) override;
  void ShowPassword();
  void SubmitPassword();
  void CancelPassword();
  void ClosePassword();
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
  int status_resource_ = 0;
  std::unique_ptr<views::BubbleDialogDelegate> password_delegate_;
  std::unique_ptr<views::Widget> password_widget_;
  raw_ptr<views::Textfield> password_ = nullptr;
  raw_ptr<views::MdTextButton> submit_password_button_ = nullptr;
  base::RepeatingClosure changed_, status_changed_;
  PasswordAnchor password_anchor_;
  base::RepeatingCallback<void(std::shared_ptr<ArchivePreview>)> open_;
  base::WeakPtrFactory<ArchiveView> weak_{this};
};
}
#endif
