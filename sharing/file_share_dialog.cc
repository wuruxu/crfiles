// Copyright 2026 The Chromium Authors
#include "crFiles/sharing/file_share_dialog.h"
#include <algorithm>
#include <array>
#include <optional>
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "components/vector_icons/vector_icons.h"
#include "crFiles/crfiles_app_icon.h"
#include "crFiles/crfiles_context_actions.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_window.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/sharing/nearby_sharing.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/color/color_id.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/progress_bar.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"
namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
ui::ImageModel Avatar(int index) {
  static constexpr std::array ids = {IDR_CRFILES_AVATAR_0, IDR_CRFILES_AVATAR_1, IDR_CRFILES_AVATAR_2,
      IDR_CRFILES_AVATAR_3, IDR_CRFILES_AVATAR_4, IDR_CRFILES_AVATAR_5, IDR_CRFILES_AVATAR_6, IDR_CRFILES_AVATAR_7, IDR_CRFILES_AVATAR_8};
  static const base::NoDestructor<std::array<gfx::ImageSkia, 9>> images([] {
    std::array<gfx::ImageSkia, 9> result;
    for (size_t i = 0; i < ids.size(); ++i) result[i] = gfx::ImageSkia::CreateFrom1xBitmap(
        gfx::PNGCodec::Decode(base::as_byte_span(ui::ResourceBundle::GetSharedInstance().GetRawDataResource(ids[i]))));
    return result;
  }());
  const auto& image = (*images)[std::clamp(index, 0, 8)];
  return image.isNull() ? CreateCrFilesIconModel(gfx::Size(64, 64)) : ui::ImageModel::FromImageSkia(
      gfx::ImageSkiaOperations::CreateCroppedCenteredRoundRectImage(gfx::Size(64, 64), 32, image));
}
struct Summary { size_t files = 0, folders = 0; std::optional<uint64_t> bytes; };
Summary Summarize(const std::vector<base::FilePath>& paths) {
  Summary result;
  for (const auto& path : paths) {
    if (base::DirectoryExists(path)) ++result.folders; else ++result.files;
  }
  result.bytes = FileSelectionSize(paths); return result;
}
class FileShareView : public views::View {
  METADATA_HEADER(FileShareView, views::View)
 public:
  FileShareView(std::shared_ptr<NearbySharing> service, std::vector<base::FilePath> paths)
      : service_(std::move(service)), paths_(std::move(paths)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical,
        gfx::Insets::TLBR(12, 16, 16, 16), 10));
    auto* summary = AddChildView(std::make_unique<views::View>());
    summary->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
    summary->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 12));
    count_ = Label(summary, base::NumberToString16(paths_.size()) + u" " + Text(IDS_CRFILES_ITEMS));
    size_ = Label(summary, Text(IDS_CRFILES_TOTAL_SIZE) + u": —");
    for (size_t i = 0; i < std::min<size_t>(paths_.size(), 4); ++i) {
      auto* name = Label(summary, paths_[i].BaseName().AsUTF16Unsafe()); name->SetTooltipText(paths_[i].AsUTF16Unsafe());
    }
    if (paths_.size() > 4) Label(summary, u"…");
    Label(summary, paths_.front().DirName().AsUTF16Unsafe());
    status_ = Label(this, Text(IDS_CRFILES_NEARBY_READY));
    enable_ = AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Enable, weak_.GetWeakPtr()), Text(IDS_CRFILES_ENABLE)));
    auto* devices = AddChildView(std::make_unique<views::View>());
    auto* layout = devices->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kStart);
    auto* manual = devices->AddChildView(std::make_unique<views::View>());
    manual->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
    auto* manual_button = manual->AddChildView(std::make_unique<views::LabelButton>(base::BindRepeating(&FileShareView::Manual, weak_.GetWeakPtr()), std::u16string()));
    manual_button->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(vector_icons::kAdd2Icon, ui::kColorSysPrimary, 48));
    manual_button->SetPreferredSize(gfx::Size(80, 72)); manual_button->SetAccessibleName(Text(IDS_CRFILES_MANUAL_CONNECTION));
    auto* manual_label = Label(manual, Text(IDS_CRFILES_MANUAL_CONNECTION)); manual_label->SetMaximumWidth(100); manual_label->SetMultiLine(true);
    auto* separator = devices->AddChildView(std::make_unique<views::Separator>());
    separator->SetOrientation(views::Separator::Orientation::kVertical); separator->SetPreferredLength(96);
    peers_ = devices->AddChildView(std::make_unique<views::View>());
    peers_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
    layout->SetFlexForView(peers_, 1);
    AddChildView(std::make_unique<views::Separator>());
    destination_ = Label(this, Text(IDS_CRFILES_DEVICE_ADDRESS));
    address_ = AddChildView(std::make_unique<views::Textfield>());
    address_->SetAccessibleName(Text(IDS_CRFILES_DEVICE_ADDRESS)); address_->SetPlaceholderText(u"192.168.1.2:59487"); address_->RemoveHoverEffect();
    targets_ = AddChildView(std::make_unique<views::Combobox>(std::make_unique<ui::SimpleComboboxModel>(std::vector<ui::SimpleComboboxModel::Item>{ui::SimpleComboboxModel::Item(Text(IDS_CRFILES_RECEIVED))})));
    targets_->SetAccessibleName(Text(IDS_CRFILES_HISTORY_DESTINATION)); targets_->SetVisible(false);
    progress_ = AddChildView(std::make_unique<views::ProgressBar>());
    prompt_ = Label(this, {}); prompt_->SetMultiLine(true);
    auto* decisions = AddChildView(std::make_unique<views::View>());
    decisions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    accept_ = decisions->AddChildView(std::make_unique<views::MdTextButton>(base::RepeatingClosure(), Text(IDS_CRFILES_ACCEPT)));
    reject_ = decisions->AddChildView(std::make_unique<views::MdTextButton>(base::RepeatingClosure(), Text(IDS_CRFILES_REJECT)));
    auto* actions = AddChildView(std::make_unique<views::View>());
    actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    send_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Send, weak_.GetWeakPtr()), Text(IDS_CRFILES_HISTORY_SEND)));
    cancel_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::RepeatingClosure(), Text(IDS_CRFILES_CANCEL)));
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&Summarize, paths_), base::BindOnce(&FileShareView::SummaryReady, weak_.GetWeakPtr()));
    Refresh(); timer_.Start(FROM_HERE, base::Milliseconds(250), this, &FileShareView::Refresh);
  }
  ~FileShareView() override { timer_.Stop(); if (own_session_) service_->CancelSession(own_session_); }
 private:
  views::Label* Label(views::View* owner, std::u16string text) {
    auto* label = owner->AddChildView(std::make_unique<views::Label>(text));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetElideBehavior(gfx::ELIDE_MIDDLE); return label;
  }
  void SummaryReady(Summary summary) {
    count_->SetText(Text(IDS_CRFILES_FILES) + u": " + base::NumberToString16(summary.files) + u"   ·   " +
        Text(IDS_CRFILES_FOLDERS) + u": " + base::NumberToString16(summary.folders));
    size_->SetText(Text(IDS_CRFILES_TOTAL_SIZE) + u": " + (summary.bytes ? ui::FormatBytes(base::ByteSize(*summary.bytes)) : std::u16string(u"—")));
  }
  void Enable() {
    auto prefs = GetNearbySharingPreferences(); prefs.discovery_enabled = true;
    if (SetNearbySharingPreferences(prefs)) ApplyCrFilesSharingSettings();
    else status_->SetText(Text(IDS_CRFILES_CONFIG_ERROR));
  }
  void Manual() {
    if (service_->GetSnapshot().busy) return;
    selected_.reset(); targets_->SetVisible(false); address_->SetVisible(true);
    destination_->SetText(Text(IDS_CRFILES_DEVICE_ADDRESS)); address_->RequestFocus();
  }
  void SelectPeer(NearbySharing::Peer peer) {
    if (service_->GetSnapshot().busy) return;
    selected_ = std::move(peer); address_->SetVisible(false);
    destination_->SetText(base::UTF8ToUTF16(selected_->name));
    std::vector<ui::SimpleComboboxModel::Item> items; items.emplace_back(Text(IDS_CRFILES_RECEIVED));
    for (const auto& target : selected_->targets) items.emplace_back(base::UTF8ToUTF16(target.name));
    targets_->SetOwnedModel(std::make_unique<ui::SimpleComboboxModel>(std::move(items))); targets_->SetSelectedIndex(0); targets_->SetVisible(true);
  }
  void Send() {
    const auto state = service_->GetSnapshot(); if (state.busy) return;
    std::string target;
    const size_t index = targets_->GetSelectedIndex().value_or(0);
    if (selected_ && index > 0 && index <= selected_->targets.size()) target = selected_->targets[index - 1].id;
    const bool sent = service_->Send(selected_ ? selected_->address : base::UTF16ToUTF8(address_->GetText()), paths_, {},
        selected_ ? selected_->phone : std::nullopt, target);
    send_error_ = !sent;
    if (sent) own_session_ = service_->GetSnapshot().session;
    Refresh();
  }
  void Decide(uint64_t session, NearbySharing::Stage stage, bool accept) { service_->Decide(session, stage, accept); Refresh(); }
  void Cancel(uint64_t session) { service_->CancelSession(session); Refresh(); }
  void Refresh() {
    const auto state = service_->GetSnapshot();
    using Stage = NearbySharing::Stage;
    int message = state.enabled ? IDS_CRFILES_NEARBY_READY : IDS_CRFILES_NEARBY_OFF;
    if (state.status_message_id) message = state.status_message_id;
    else if (state.stage == Stage::kComplete) message = IDS_CRFILES_COMPLETE;
    else if (state.stage == Stage::kError) message = IDS_CRFILES_TRANSFER_ERROR;
    else if (state.busy) message = IDS_CRFILES_TRANSFERRING;
    status_->SetText(Text(send_error_ && !state.busy ? IDS_CRFILES_SEND_ERROR : message));
    enable_->SetVisible(!state.enabled);
    const bool can_send = state.enabled && state.port && !state.busy && !paths_.empty();
    send_->SetEnabled(can_send); address_->SetEnabled(!state.busy); targets_->SetEnabled(!state.busy);
    if (!state.busy && known_peers_ != state.peers) {
      known_peers_ = state.peers; peers_->RemoveAllChildViews();
      views::View* row = nullptr; size_t index = 0;
      for (const auto& peer : known_peers_) {
        if (index++ % 3 == 0) {
          row = peers_->AddChildView(std::make_unique<views::View>());
          row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
        }
        auto* tile = row->AddChildView(std::make_unique<views::View>());
        tile->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
        auto* button = tile->AddChildView(std::make_unique<views::LabelButton>(base::BindRepeating(&FileShareView::SelectPeer, weak_.GetWeakPtr(), peer), std::u16string()));
        button->SetImageModel(views::Button::STATE_NORMAL, Avatar(peer.avatar)); button->SetPreferredSize(gfx::Size(100, 72));
        button->SetAccessibleName(base::UTF8ToUTF16(peer.name));
        auto* name = Label(tile, base::UTF8ToUTF16(peer.name)); name->SetPreferredSize(gfx::Size(100, 24));
      }
    }
    peers_->SetEnabled(can_send);
    progress_->SetVisible(state.busy); progress_->SetValue(state.total ? std::clamp(static_cast<double>(state.transferred) / state.total, 0.0, 1.0) : -1.0);
    const bool prompt = state.stage == Stage::kPairing || state.stage == Stage::kReceivingOffer;
    prompt_->SetVisible(prompt); accept_->SetVisible(prompt); reject_->SetVisible(prompt);
    if (prompt) {
      prompt_->SetText(Text(state.stage == Stage::kPairing ? IDS_CRFILES_COMPARE_CODE : IDS_CRFILES_RECEIVE_PROMPT) + u"\n" + base::UTF8ToUTF16(state.code));
      accept_->SetCallback(base::BindRepeating(&FileShareView::Decide, weak_.GetWeakPtr(), state.session, state.stage, true));
      reject_->SetCallback(base::BindRepeating(&FileShareView::Decide, weak_.GetWeakPtr(), state.session, state.stage, false));
    }
    cancel_->SetVisible(state.busy); cancel_->SetCallback(base::BindRepeating(&FileShareView::Cancel, weak_.GetWeakPtr(), state.session));
  }
  std::shared_ptr<NearbySharing> service_;
  std::vector<base::FilePath> paths_;
  std::vector<NearbySharing::Peer> known_peers_;
  std::optional<NearbySharing::Peer> selected_;
  uint64_t own_session_ = 0;
  bool send_error_ = false;
  raw_ptr<views::View> peers_ = nullptr;
  raw_ptr<views::Label> count_ = nullptr, size_ = nullptr, status_ = nullptr, destination_ = nullptr, prompt_ = nullptr;
  raw_ptr<views::Textfield> address_ = nullptr;
  raw_ptr<views::Combobox> targets_ = nullptr;
  raw_ptr<views::ProgressBar> progress_ = nullptr;
  raw_ptr<views::MdTextButton> enable_ = nullptr, send_ = nullptr, cancel_ = nullptr, accept_ = nullptr, reject_ = nullptr;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<FileShareView> weak_{this};
};
BEGIN_METADATA(FileShareView)
END_METADATA
}
void ShowFileShareDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service, std::vector<base::FilePath> paths) {
  if (!parent || !service || paths.empty()) return;
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  scroll->SetUseContentsPreferredSize(true); scroll->ClipHeightTo(300, 600);
  scroll->SetContents(std::make_unique<FileShareView>(std::move(service), std::move(paths)));
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->set_fixed_width(580); dialog->SetTitle(Text(IDS_CRFILES_SHARE_TO));
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone)); dialog->SetShowCloseButton(true);
  dialog->SetContentsView(std::move(scroll));
  views::DialogDelegate::CreateDialogWidget(std::move(dialog), parent->GetNativeWindow(), parent->GetNativeView())->Show();
}
}
