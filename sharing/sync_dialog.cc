// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crFiles/sharing/sync_dialog.h"
#include <utility>
#include "crFiles/sharing/nearby_sharing.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_file_access.h"
#include "crFiles/grit/crfiles_strings.h"
#include "base/functional/bind.h"
#include "base/files/file_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/window/dialog_delegate.h"

namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
views::Label* Label(views::View* parent, const std::u16string& text) {
  auto* label = parent->AddChildView(std::make_unique<views::Label>(text));
  label->SetMultiLine(true); label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetMaximumWidth(600); return label;
}
class SyncView : public views::View {
  METADATA_HEADER(SyncView, views::View)
 public:
  SyncView(std::shared_ptr<NearbySharing> service, std::vector<base::FilePath> paths,
           base::RepeatingCallback<void(base::FilePath)> open)
      : service_(std::move(service)), paths_(std::move(paths)), open_(std::move(open)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(20), 12));
    Label(this, Text(IDS_CRFILES_SYNC_HINT));
    Label(this, paths_.size() == 1 ? paths_[0].AsUTF16Unsafe() : Text(IDS_CRFILES_SYNC_SELECT_ONE));
    status_ = Label(this, {});
    devices_ = AddChildView(std::make_unique<views::View>());
    devices_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 8));
    Label(this, Text(IDS_CRFILES_SYNC_RELATIONSHIPS));
    jobs_ = AddChildView(std::make_unique<views::View>());
    jobs_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
    Refresh(); timer_.Start(FROM_HERE, base::Seconds(1), this, &SyncView::Refresh);
  }
 private:
  void Start(NearbySharing::Peer peer) {
    if (paths_.size() != 1 || queued_ || requesting_) return;
    requesting_ = true; device_key_.clear();
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
        base::BindOnce([](base::FilePath path) { return base::DirectoryExists(path) ? path : path.DirName(); }, paths_[0]),
        base::BindOnce([](base::WeakPtr<SyncView> view, NearbySharing::Peer peer, base::FilePath directory) {
          if (!view) return;
          EnsureDirectoryAccess(view->GetWidget(), directory, true, base::BindOnce(
              [](base::WeakPtr<SyncView> view, NearbySharing::Peer peer, bool allowed) {
                if (!view) return;
                view->requesting_ = false; view->device_key_.clear();
                if (!allowed) return;
                view->queued_ = view->service_->StartSync(peer, view->paths_[0]);
                view->status_->SetText(Text(view->queued_ ? IDS_CRFILES_SYNC_WAITING : IDS_CRFILES_SYNC_SOURCE_ERROR));
                view->device_key_.clear();
              }, view, std::move(peer)));
        }, weak_.GetWeakPtr(), std::move(peer)));
  }
  void Control(std::string id, bool remove, bool paused) {
    const auto sync = service_->sync_service();
    if (remove) sync->Remove(id); else sync->Pause(id, paused);
    if (remove || paused) service_->CancelSync(id);
    base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()}, base::BindOnce(
        [](std::shared_ptr<SyncService> sync) { sync->Poll(); }, sync));
  }
  void Open(base::FilePath path) { if (open_) open_.Run(std::move(path)); }
  void Refresh() {
    const auto state = service_->GetSnapshot();
    if (!state.enabled) status_->SetText(Text(IDS_CRFILES_NEARBY_OFF));
    std::string key = state.enabled ? "on" : "off";
    key += queued_ ? "queued" : requesting_ ? "requesting" : "new";
    std::vector<NearbySharing::Peer> peers;
    for (const auto& peer : state.peers) if (peer.sync_v1 && !peer.phone &&
        peer.identity_expires > base::TimeTicks::Now() && !peer.verified_fingerprint.empty()) {
      peers.push_back(peer); key += peer.address + peer.verified_fingerprint + peer.name;
    }
    if (key != device_key_) {
      device_key_ = key; devices_->RemoveAllChildViews();
      if (peers.empty()) Label(devices_, Text(IDS_CRFILES_SYNC_NO_DEVICES));
      for (const auto& peer : peers) {
        auto* button = devices_->AddChildView(std::make_unique<views::MdTextButton>(
            base::BindRepeating(&SyncView::Start, weak_.GetWeakPtr(), peer), base::UTF8ToUTF16(peer.name)));
        button->SetEnabled(state.enabled && paths_.size() == 1 && !queued_ && !requesting_);
      }
    }
    const auto sync = service_->sync_service();
    if (!sync) return;
    const auto jobs = sync->Snapshot(); key.clear();
    for (const auto& job : jobs) key += job.id + std::to_string(job.status) + (job.paused ? "p" : "a") + job.root.AsUTF8Unsafe();
    if (key == job_key_ && initialized_) return;
    initialized_ = true; job_key_ = key; jobs_->RemoveAllChildViews();
    if (jobs.empty()) Label(jobs_, Text(IDS_CRFILES_SYNC_NONE));
    for (const auto& job : jobs) {
      auto* row = jobs_->AddChildView(std::make_unique<views::View>());
      row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
      Label(row, job.root.AsUTF16Unsafe() + u"\n" + base::UTF8ToUTF16(job.peer));
      Label(row, Text(job.status ? job.status : IDS_CRFILES_SYNC_WAITING));
      if (job.id.empty()) continue;
      auto* actions = row->AddChildView(std::make_unique<views::View>());
      actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
      actions->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&SyncView::Control, weak_.GetWeakPtr(), job.id, false, !job.paused),
          Text(job.paused ? IDS_CRFILES_SYNC_RESUME : IDS_CRFILES_SYNC_PAUSE)));
      actions->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&SyncView::Control, weak_.GetWeakPtr(), job.id, true, true), Text(IDS_CRFILES_SYNC_REMOVE)));
      actions->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&SyncView::Open, weak_.GetWeakPtr(), job.root), Text(IDS_CRFILES_OPEN)));
      if (!job.recovery.empty()) actions->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&SyncView::Open, weak_.GetWeakPtr(), job.recovery), Text(IDS_CRFILES_SYNC_RECOVERY)));
    }
    InvalidateLayout();
  }
  std::shared_ptr<NearbySharing> service_;
  std::vector<base::FilePath> paths_;
  base::RepeatingCallback<void(base::FilePath)> open_;
  raw_ptr<views::View> devices_ = nullptr, jobs_ = nullptr;
  raw_ptr<views::Label> status_ = nullptr;
  bool initialized_ = false, queued_ = false, requesting_ = false;
  std::string device_key_, job_key_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<SyncView> weak_{this};
};
BEGIN_METADATA(SyncView)
END_METADATA
}  // namespace
views::View* ShowSyncDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service,
    std::vector<base::FilePath> paths, base::RepeatingCallback<void(base::FilePath)> open) {
  if (!parent || !service || !service->sync_service()) return nullptr;
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  scroll->SetUseContentsPreferredSize(true); scroll->ClipHeightTo(300, 600);
  auto* contents = scroll->SetContents(std::make_unique<SyncView>(std::move(service), std::move(paths), std::move(open)));
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->set_fixed_width(680); dialog->SetTitle(Text(IDS_CRFILES_SYNC_TO));
  dialog->SetModalType(ui::mojom::ModalType::kWindow);
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone)); dialog->SetShowCloseButton(true);
  dialog->SetContentsView(std::move(scroll)); ShowCrFilesDialog(std::move(dialog), parent);
  return contents;
}
}  // namespace crfiles
