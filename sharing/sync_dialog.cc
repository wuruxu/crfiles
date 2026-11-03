// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crFiles/sharing/sync_dialog.h"
#include <map>
#include <optional>
#include <utility>
#include "crFiles/sharing/nearby_sharing.h"
#include "crFiles/sharing/file_share_dialog.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_file_access.h"
#include "crFiles/grit/crfiles_strings.h"
#include "base/functional/bind.h"
#include "base/files/file_util.h"
#include "base/i18n/time_formatting.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/color/color_id.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/separator.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/window/dialog_delegate.h"

namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
views::Label* Label(views::View* parent, const std::u16string& text, bool multiline = false) {
  auto* label = parent->AddChildView(std::make_unique<views::Label>(text));
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetMultiLine(multiline);
  if (multiline) label->SetMaximumWidth(600); else label->SetMaximumWidthSingleLine(600);
  return label;
}
views::ImageButton* IconButton(views::View* parent, base::RepeatingClosure callback,
    const gfx::VectorIcon& icon, int title, ui::ColorId color = ui::kColorSysOnSurface) {
  auto button = views::CreateVectorImageButtonWithNativeTheme(std::move(callback), icon, 20,
      color, ui::kColorIconDisabled, color);
  button->SetPreferredSize(gfx::Size(36, 36));
  button->SetTooltipText(Text(title)); button->GetViewAccessibility().SetName(Text(title));
  return parent->AddChildView(std::move(button));
}
void Poll(std::shared_ptr<SyncService> sync) {
  base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()}, base::BindOnce(
      [](std::shared_ptr<SyncService> sync) { sync->Poll(); }, std::move(sync)));
}
class SyncView : public views::View {
  METADATA_HEADER(SyncView, views::View)
 public:
  SyncView(std::shared_ptr<NearbySharing> service, std::vector<base::FilePath> paths)
      : service_(std::move(service)), paths_(std::move(paths)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 12));
    Label(this, Text(IDS_CRFILES_SYNC_HINT), true);
    auto* selection = Label(this, paths_.size() == 1 ? paths_[0].BaseName().AsUTF16Unsafe() : Text(IDS_CRFILES_SYNC_SELECT_ONE));
    if (paths_.size() == 1) selection->SetTooltipText(paths_[0].AsUTF16Unsafe());
    status_ = Label(this, {}, true);
    devices_ = AddChildView(std::make_unique<views::View>());
    devices_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
    Refresh(); timer_.Start(FROM_HERE, base::Milliseconds(250), this, &SyncView::Refresh);
  }
  ~SyncView() override { ClosePopup(); }
 private:
  void ClosePopup() {
    update_from_remote_ = nullptr;
    if (popup_delegate_) popup_delegate_->SetAnchorView(nullptr);
    if (popup_widget_) popup_widget_->MakeCloseSynchronous({});
    popup_widget_.reset(); popup_delegate_.reset();
  }
  void Select(NearbySharing::Peer peer) {
    if (queued_ || requesting_ || !anchors_.contains(peer.verified_fingerprint)) return;
    ClosePopup();
    auto contents = std::make_unique<views::View>();
    auto* layout = contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    update_from_remote_ = contents->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_SYNC_UPDATE_FROM_REMOTE)));
    update_from_remote_->SetChecked(false);
    contents->AddChildView(std::make_unique<views::MdTextButton>(
        base::BindRepeating(&SyncView::Confirm, weak_.GetWeakPtr(), peer), Text(IDS_CRFILES_SYNC_START)));
    popup_delegate_ = std::make_unique<views::BubbleDialogDelegate>(
        anchors_.at(peer.verified_fingerprint), views::BubbleBorder::TOP_CENTER);
    popup_delegate_->SetTitle(l10n_util::GetStringFUTF16(IDS_CRFILES_SYNC_TO_DEVICE, base::UTF8ToUTF16(peer.name)));
    popup_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    popup_delegate_->SetShowCloseButton(false); popup_delegate_->set_margins(gfx::Insets(12));
    popup_delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    popup_delegate_->SetContentsView(std::move(contents));
    popup_widget_ = CreateCrFilesBubble(popup_delegate_.get(), base::BindOnce(
        [](base::WeakPtr<SyncView> view, views::Widget::ClosedReason) { if (view) view->ClosePopup(); }, weak_.GetWeakPtr()));
    auto* frame = popup_delegate_->GetBubbleFrameView();
    frame->SetDisplayVisibleArrow(true); frame->bubble_border()->set_draw_border_stroke(true);
    frame->bubble_border()->set_md_shadow_elevation(6);
    popup_delegate_->SizeToContents(); popup_widget_->Show();
  }
  void Confirm(NearbySharing::Peer peer) {
    if (!update_from_remote_ || paths_.size() != 1 || queued_ || requesting_) return;
    const bool write = update_from_remote_->GetChecked();
    // Close asynchronously, after the confirmation button's event unwinds.
    if (popup_widget_) popup_widget_->Close();
    requesting_ = true; device_key_.clear();
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
        base::BindOnce([](base::FilePath path) { return base::DirectoryExists(path) ? path : path.DirName(); }, paths_[0]),
        base::BindOnce([](base::WeakPtr<SyncView> view, NearbySharing::Peer peer, bool write, base::FilePath directory) {
          if (!view) return;
          EnsureDirectoryAccess(view->GetWidget(), directory, true, base::BindOnce(
              [](base::WeakPtr<SyncView> view, NearbySharing::Peer peer, bool write, bool allowed) {
                if (!view) return;
                view->device_key_.clear();
                if (!allowed) {
                  LOG(ERROR) << "[crFiles sync] start_failed stage=directory_access_not_granted";
                  view->requesting_ = false; view->show_request_error_ = true;
                  view->status_->SetText(Text(IDS_CRFILES_SYNC_PREPARE_ERROR)); return;
                }
                std::string retry_id;
                for (const auto& job : view->service_->sync_service()->Snapshot()) {
                  if (job.id == view->request_id_ && job.creator && !job.removed &&
                      job.fingerprint == peer.verified_fingerprint) retry_id = job.id;
                }
                const auto result = view->service_->StartSync(peer, view->paths_[0], write, retry_id);
                view->queued_ = !result.id.empty();
                view->status_->SetText(Text(view->queued_ ? IDS_CRFILES_SYNC_WAITING : result.error));
                if (result.id.empty()) {
                  view->requesting_ = false;
                  // Do not replace this immediate error with a previous job's
                  // status on the next timer tick. Keep its ID for a later retry.
                  view->show_request_error_ = true; return;
                }
                view->show_request_error_ = false; view->request_id_ = result.id;
                view->selected_peer_ = peer; view->cancel_requested_ = false;
                // Wait for command processing before inspecting the previous
                // paused snapshot, otherwise a retry immediately unlocks again.
                base::ThreadPool::PostTaskAndReply(FROM_HERE, {base::MayBlock()}, base::BindOnce(
                    [](std::shared_ptr<SyncService> sync) { sync->Poll(); }, view->service_->sync_service()),
                    base::BindOnce([](base::WeakPtr<SyncView> view) {
                      if (!view) return;
                      view->requesting_ = false; view->device_key_.clear(); view->Refresh();
                    }, view));
              }, view, std::move(peer), write));
        }, weak_.GetWeakPtr(), std::move(peer), write));
  }
  void PauseRequest(std::string id) {
    if (id.empty() || id != request_id_) return;
    cancel_requested_ = true; queued_ = false;
    const auto sync = service_->sync_service();
    sync->Pause(id, true); service_->CancelSync(id); Poll(sync);
    status_->SetText(Text(IDS_CRFILES_SYNC_PAUSED));
    // Refresh on the timer, after the avatar's click callback has returned.
  }
  void Refresh() {
    const auto state = service_->GetSnapshot();
    int job_status = IDS_CRFILES_SYNC_WAITING;
    bool stopped = show_request_error_;
    if (!request_id_.empty() && !show_request_error_) {
      bool found = false;
      // Queue acceptance is not registration or delivery. Follow this exact
      // request so source/database/transport errors cannot look like waiting.
      for (const auto& job : service_->sync_service()->Snapshot()) {
        if (job.id == request_id_ || (job.id.empty() && job.status == IDS_CRFILES_SYNC_STORAGE_ERROR)) {
          found = true;
          job_status = job.status ? job.status : IDS_CRFILES_SYNC_WAITING;
          stopped = job.paused || job.removed || job.status == IDS_CRFILES_SYNC_RETRY || IsSyncErrorStatus(job.status);
          if (job.paused || job.removed) cancel_requested_ = false;
          status_->SetText(Text(job_status));
          if (!requesting_ && stopped) queued_ = false;
          break;
        }
      }
      if (!found && !requesting_) {
        queued_ = false; request_id_.clear(); cancel_requested_ = false; status_->SetText({});
      }
    }
    if (cancel_requested_) status_->SetText(Text(IDS_CRFILES_SYNC_PAUSED));
    if (!state.enabled) status_->SetText(Text(IDS_CRFILES_NEARBY_OFF));
    else if (!queued_ && status_->GetText() == Text(IDS_CRFILES_NEARBY_OFF)) status_->SetText({});
    std::vector<NearbySharing::Peer> peers;
    bool selected_visible = false;
    for (const auto& peer : state.peers) if (peer.sync_v1 && !peer.phone &&
        peer.identity_expires > base::TimeTicks::Now() && !peer.verified_fingerprint.empty()) {
      peers.push_back(peer);
      if (selected_peer_ && peer.verified_fingerprint == selected_peer_->verified_fingerprint) {
        selected_peer_ = peer; selected_visible = true;
      }
    }
    // Discovery can disappear during a transfer. Keep its selected avatar and
    // progress visible; StartSync still checks a fresh identity before retrying.
    if (selected_peer_ && !selected_visible) peers.push_back(*selected_peer_);
    std::string key;
    for (const auto& peer : peers)
      key += peer.verified_fingerprint + "|" + peer.address + "|" + peer.name + "|" + std::to_string(peer.avatar) + "\n";
    if (key != device_key_ || !devices_initialized_) {
      devices_initialized_ = true; device_key_ = key;
      ClosePopup(); anchors_.clear(); devices_->RemoveAllChildViews();
      if (peers.empty()) Label(devices_, Text(IDS_CRFILES_SYNC_NO_DEVICES), true);
      views::View* row = nullptr;
      for (size_t i = 0; i < peers.size(); ++i) {
        const auto& peer = peers[i];
        if (i % 4 == 0) {
          row = devices_->AddChildView(std::make_unique<views::View>());
          row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 16));
        }
        auto* button = row->AddChildView(CreateFileShareDeviceButton(
            base::BindRepeating(&SyncView::Select, weak_.GetWeakPtr(), peer), peer.avatar, base::UTF8ToUTF16(peer.name)));
        anchors_[peer.verified_fingerprint] = button;
      }
      InvalidateLayout();
    }
    const bool session = !request_id_.empty() && state.syncing && !state.receiving &&
        state.sync_id == request_id_ && state.busy;
    const bool active = state.enabled && !stopped && !cancel_requested_ && !request_id_.empty() &&
        (session || (queued_ && job_status == IDS_CRFILES_SYNC_WAITING));
    const bool indeterminate = !session || state.stage != NearbySharing::Stage::kTransferring || !state.total;
    const double progress = session && state.total ? static_cast<double>(state.transferred) / state.total : 0;
    std::u16string detail(status_->GetText());
    if (active && session && state.stage == NearbySharing::Stage::kTransferring) {
      if (!state.current_file.empty()) detail += u"\n" + base::UTF8ToUTF16(state.current_file);
      detail += u"\n" + ui::FormatBytes(base::ByteSize(state.transferred)) + u" / " + ui::FormatBytes(base::ByteSize(state.total));
    }
    for (const auto& [fingerprint, button] : anchors_) {
      const bool transfer = active && selected_peer_ && fingerprint == selected_peer_->verified_fingerprint;
      SetFileShareDeviceTransfer(button, transfer, indeterminate, progress,
          base::BindRepeating(&SyncView::PauseRequest, weak_.GetWeakPtr(), request_id_), detail);
      button->SetEnabled(state.enabled && paths_.size() == 1 && (transfer || (!queued_ && !requesting_)));
    }
  }
  std::shared_ptr<NearbySharing> service_;
  std::vector<base::FilePath> paths_;
  raw_ptr<views::View> devices_ = nullptr;
  raw_ptr<views::Label> status_ = nullptr;
  raw_ptr<views::Checkbox> update_from_remote_ = nullptr;
  std::map<std::string, raw_ptr<views::View>> anchors_;
  std::unique_ptr<views::BubbleDialogDelegate> popup_delegate_;
  std::unique_ptr<views::Widget> popup_widget_;
  bool queued_ = false, requesting_ = false;
  bool show_request_error_ = false;
  bool cancel_requested_ = false, devices_initialized_ = false;
  std::optional<NearbySharing::Peer> selected_peer_;
  std::string device_key_, request_id_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<SyncView> weak_{this};
};
BEGIN_METADATA(SyncView)
END_METADATA

class SyncingView : public views::View {
  METADATA_HEADER(SyncingView, views::View)
 public:
  SyncingView(std::shared_ptr<NearbySharing> service, base::RepeatingCallback<void(base::FilePath)> open)
      : service_(std::move(service)), open_(std::move(open)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 12));
    jobs_ = AddChildView(std::make_unique<views::View>());
    jobs_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
    Refresh(); timer_.Start(FROM_HERE, base::Milliseconds(250), this, &SyncingView::Refresh);
  }
 private:
  void Control(std::string id, bool remove, bool paused) {
    const auto sync = service_->sync_service();
    if (remove) sync->Remove(id); else sync->Pause(id, paused);
    if (remove || paused) service_->CancelSync(id);
    Poll(sync);
  }
  void Permissions(std::string id, views::Checkbox* read, views::Checkbox* write) {
    const auto sync = service_->sync_service();
    sync->Permissions(id, read->GetChecked(), write->GetChecked());
    service_->CancelSync(id); Poll(sync);
  }
  void Open(base::FilePath path) { if (open_) open_.Run(std::move(path)); }
  std::u16string Date(base::Time value) {
    return value.is_null() ? Text(IDS_CRFILES_SYNC_NOT_YET) : base::TimeFormatShortDateAndTime(value);
  }
  void Refresh() {
    const auto jobs = service_->sync_service()->Snapshot();
    if (initialized_ && jobs == snapshot_) return;
    initialized_ = true; snapshot_ = jobs; jobs_->RemoveAllChildViews();
    if (jobs.empty()) Label(jobs_, Text(IDS_CRFILES_SYNC_NONE));
    for (const auto& job : jobs) {
      auto* row = jobs_->AddChildView(std::make_unique<views::View>());
      auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
      layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
      auto* avatar = row->AddChildView(std::make_unique<views::ImageButton>(
          base::BindRepeating(&SyncingView::Open, weak_.GetWeakPtr(), job.root)));
      avatar->SetImageModel(views::Button::STATE_NORMAL, FileShareDeviceAvatar(job.avatar, 48));
      avatar->SetPreferredSize(gfx::Size(48, 48));
      const auto status = Text(job.status ? job.status : IDS_CRFILES_SYNC_WAITING);
      avatar->SetTooltipText(base::UTF8ToUTF16(job.peer) + u"\n" + status);
      avatar->GetViewAccessibility().SetName(base::UTF8ToUTF16(job.peer) + u" · " + Text(IDS_CRFILES_OPEN));
      auto* text = row->AddChildView(std::make_unique<views::View>());
      text->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
      layout->SetFlexForView(text, 1);
      const auto details = l10n_util::GetStringFUTF16(IDS_CRFILES_SYNC_TIMES, job.started.is_null() ? u"—" : Date(job.started), Date(job.last_synced));
      auto* first = Label(text, details); first->SetEnabledColor(ui::kColorSysPrimary);
      first->SetTooltipText(base::UTF8ToUTF16(job.peer) + u"\n" + details + u"\n" + status);
      auto* second = text->AddChildView(std::make_unique<views::View>());
      auto* second_layout = second->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
      second_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
      auto* name = Label(second, job.name.empty() ? job.root.BaseName().AsUTF16Unsafe() : base::UTF8ToUTF16(job.name));
      name->SetTooltipText(job.root.AsUTF16Unsafe()); name->SetMaximumWidthSingleLine(200);
      auto* read = second->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_SYNC_READ)));
      auto* write = second->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_SYNC_WRITE)));
      read->SetChecked(job.read); write->SetChecked(job.write);
      const auto managed = job.creator ? std::u16string() : u"\n" + l10n_util::GetStringFUTF16(
          IDS_CRFILES_SYNC_MANAGED_BY, base::UTF8ToUTF16(job.peer));
      read->SetTooltipText(Text(IDS_CRFILES_SYNC_READ_HINT) + managed);
      write->SetTooltipText(Text(IDS_CRFILES_SYNC_WRITE_HINT) + managed);
      read->SetCallback(base::BindRepeating(&SyncingView::Permissions, weak_.GetWeakPtr(), job.id, base::Unretained(read), base::Unretained(write)));
      write->SetCallback(base::BindRepeating(&SyncingView::Permissions, weak_.GetWeakPtr(), job.id, base::Unretained(read), base::Unretained(write)));
      const bool valid = !job.id.empty() && !job.fingerprint.empty() && !job.removed;
      const bool editable = valid && job.creator && job.permissions_known;
      read->SetEnabled(editable); write->SetEnabled(editable);
      if (!job.permissions_known && valid) Label(text, Text(IDS_CRFILES_SYNC_NO_PERMISSION));
      if (IsSyncErrorStatus(job.status) || job.status == IDS_CRFILES_SYNC_RETRY) {
        auto* error = Label(text, status, true); error->SetEnabledColor(ui::kColorSysError);
      }
      if (job.id.empty()) continue;
      auto* actions = row->AddChildView(std::make_unique<views::View>());
      actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 0));
      IconButton(actions, base::BindRepeating(&SyncingView::Control, weak_.GetWeakPtr(), job.id, false, !job.paused),
          job.paused ? vector_icons::kPlayArrowIcon : vector_icons::kPauseIcon,
          job.paused ? IDS_CRFILES_SYNC_RESUME : IDS_CRFILES_SYNC_PAUSE)->SetEnabled(valid);
      if (!job.recovery.empty()) IconButton(actions, base::BindRepeating(&SyncingView::Open, weak_.GetWeakPtr(), job.recovery),
          vector_icons::kFolderOpenIcon, IDS_CRFILES_SYNC_RECOVERY);
      IconButton(actions, base::BindRepeating(&SyncingView::Control, weak_.GetWeakPtr(), job.id, true, true),
          kDeleteIcon, IDS_CRFILES_SYNC_REMOVE, ui::kColorSysError);
      jobs_->AddChildView(std::make_unique<views::Separator>());
    }
    InvalidateLayout();
  }
  std::shared_ptr<NearbySharing> service_;
  base::RepeatingCallback<void(base::FilePath)> open_;
  raw_ptr<views::View> jobs_ = nullptr;
  bool initialized_ = false;
  std::vector<SyncJob> snapshot_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<SyncingView> weak_{this};
};
BEGIN_METADATA(SyncingView)
END_METADATA
views::View* Show(views::Widget* parent, std::unique_ptr<views::View> view, int title) {
  auto* result = view.get();
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  scroll->SetUseContentsPreferredSize(true); scroll->ClipHeightTo(288, 960);
  scroll->SetContents(std::move(view));
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->set_fixed_width(680); dialog->SetTitle(Text(title));
  dialog->SetModalType(ui::mojom::ModalType::kWindow);
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone)); dialog->SetShowCloseButton(true);
  dialog->SetContentsView(std::move(scroll)); ShowCrFilesDialog(std::move(dialog), parent);
  return result;
}
}  // namespace
views::View* ShowSyncDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service, std::vector<base::FilePath> paths) {
  if (!parent || !service || !service->sync_service()) return nullptr;
  return Show(parent, std::make_unique<SyncView>(std::move(service), std::move(paths)), IDS_CRFILES_SYNC_TO);
}
views::View* ShowSyncingDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service,
    base::RepeatingCallback<void(base::FilePath)> open) {
  if (!parent || !service || !service->sync_service()) return nullptr;
  return Show(parent, std::make_unique<SyncingView>(std::move(service), std::move(open)), IDS_CRFILES_SYNCING);
}
}  // namespace crfiles
