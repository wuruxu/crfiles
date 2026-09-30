// Copyright 2026 The Chromium Authors
#include "crFiles/sharing/file_share_dialog.h"
#include "crFiles/crfiles_dialog.h"
#include <algorithm>
#include <array>
#include <cmath>
#include "cc/paint/paint_flags.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/color/color_provider.h"
#include <optional>
#include "base/containers/span.h"
#include "base/i18n/time_formatting.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/checkbox.h"
#include "base/base64.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
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
#include "ui/gfx/color_palette.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
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
ui::ImageModel Avatar(int index, int edge = 64) {
  static constexpr std::array ids = {IDR_CRFILES_AVATAR_0, IDR_CRFILES_AVATAR_1, IDR_CRFILES_AVATAR_2,
      IDR_CRFILES_AVATAR_3, IDR_CRFILES_AVATAR_4, IDR_CRFILES_AVATAR_5, IDR_CRFILES_AVATAR_6, IDR_CRFILES_AVATAR_7, IDR_CRFILES_AVATAR_8};
  static const base::NoDestructor<std::array<gfx::ImageSkia, 9>> images([] {
    std::array<gfx::ImageSkia, 9> result;
    for (size_t i = 0; i < ids.size(); ++i) result[i] = gfx::ImageSkia::CreateFrom1xBitmap(
        gfx::PNGCodec::Decode(base::as_byte_span(ui::ResourceBundle::GetSharedInstance().GetRawDataResource(ids[i]))));
    return result;
  }());
  const auto& image = (*images)[std::clamp(index, 0, 8)];
  return image.isNull() ? CreateCrFilesIconModel(gfx::Size(edge, edge)) : ui::ImageModel::FromImageSkia(
      gfx::ImageSkiaOperations::CreateCroppedCenteredRoundRectImage(gfx::Size(edge, edge), edge / 2, image));
}
ui::ImageModel PeerAvatar(const NearbySharing::Snapshot& state) {
  std::string pixels;
  if (state.peer_custom_avatar.size() == 49152 && base::Base64Decode(state.peer_custom_avatar, &pixels) && pixels.size() == 96 * 96 * 4) {
    SkBitmap bitmap;
    const auto info = SkImageInfo::Make(96, 96, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (bitmap.tryAllocN32Pixels(96, 96) && bitmap.writePixels(SkPixmap(info, pixels.data(), 96 * 4))) {
      bitmap.setImmutable();
      return ui::ImageModel::FromImageSkia(gfx::ImageSkiaOperations::CreateCroppedCenteredRoundRectImage(
          gfx::Size(64, 64), 32, gfx::ImageSkia::CreateFrom1xBitmap(bitmap)));
    }
  }
  return Avatar(state.peer_avatar);
}
class IncomingShareToastImpl : public IncomingShareToast {
 public:
  IncomingShareToastImpl(views::Widget* parent, std::shared_ptr<NearbySharing> service,
                        base::RepeatingCallback<gfx::Rect(const std::string&)> bounds)
      : parent_(parent), service_(std::move(service)), bounds_(std::move(bounds)) {
    Refresh(); timer_.Start(FROM_HERE, base::Milliseconds(250), this, &IncomingShareToastImpl::Refresh);
  }
  ~IncomingShareToastImpl() override { timer_.Stop(); Close(); }
 private:
  void Close() {
    accept_ = nullptr; reject_ = nullptr; trust_ = nullptr;
    widget_.reset(); delegate_.reset();
  }
  views::Label* Label(views::View* parent, std::u16string text) {
    auto* label = parent->AddChildView(std::make_unique<views::Label>(text));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true);
    label->SetMaximumWidth(336); return label;
  }
  void Decide(bool accept) {
    if (decided_ || base::TimeTicks::Now() < ready_) return;
    decided_ = true;
    service_->Decide(session_, stage_, accept, trust_ && trust_->GetChecked());
    if (accept_) accept_->SetEnabled(false);
    if (reject_) reject_->SetEnabled(false);
  }
  void Show(const NearbySharing::Snapshot& state) {
    Close(); session_ = state.session; stage_ = state.stage; decided_ = false;
    ready_ = base::TimeTicks::Now() + base::Milliseconds(500);
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
    auto* identity = contents->AddChildView(std::make_unique<views::View>());
    auto* identity_layout = identity->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    identity_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    auto* avatar = identity->AddChildView(std::make_unique<views::ImageView>(PeerAvatar(state)));
    avatar->SetImageSize(gfx::Size(64, 64));
    auto* name = Label(identity, base::UTF8ToUTF16(state.peer_name)); name->SetMaximumWidth(248);
    if (stage_ == NearbySharing::Stage::kPairing) {
      auto* code = Label(contents.get(), Text(IDS_CRFILES_COMPARE_CODE) + u"\n" + base::UTF8ToUTF16(state.code));
      code->SetElideBehavior(gfx::NO_ELIDE);
      trust_ = contents->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_NEARBY_TRUST_DEVICE)));
    } else {
      auto* summary = contents->AddChildView(std::make_unique<views::View>());
      summary->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(12), 6));
      summary->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 12));
      const size_t folders = std::ranges::count_if(state.files, [](const std::string& path) { return path.ends_with('/'); });
      Label(summary, Text(IDS_CRFILES_FILES) + u": " + base::NumberToString16(state.files.size() - folders) +
          u"   ·   " + Text(IDS_CRFILES_FOLDERS) + u": " + base::NumberToString16(folders));
      Label(summary, Text(IDS_CRFILES_TOTAL_SIZE) + u": " + ui::FormatBytes(base::ByteSize(state.total)));
      Label(contents.get(), Text(IDS_CRFILES_RECEIVE_PROMPT));
    }
    auto* actions = contents->AddChildView(std::make_unique<views::View>());
    auto* layout = actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    accept_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&IncomingShareToastImpl::Decide, weak_.GetWeakPtr(), true), Text(IDS_CRFILES_ACCEPT)));
    reject_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&IncomingShareToastImpl::Decide, weak_.GetWeakPtr(), false), Text(IDS_CRFILES_REJECT)));
    accept_->SetEnabled(false); reject_->SetEnabled(false);
    delegate_ = std::make_unique<views::BubbleDialogDelegate>(nullptr, views::BubbleBorder::BOTTOM_RIGHT);
    delegate_->set_parent_window(parent_->GetNativeView());
    delegate_->SetAnchorRect(Anchor(state.receive_target));
    delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    delegate_->SetShowCloseButton(false); delegate_->set_close_on_deactivate(false);
    delegate_->SetCancelCallbackWithClose(base::BindRepeating([] { return false; }));
    delegate_->set_margins(gfx::Insets(12)); delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    delegate_->SetContentsView(std::move(contents));
    widget_ = CreateCrFilesBubble(delegate_.get(), base::BindOnce(
        [](base::WeakPtr<IncomingShareToastImpl> owner, views::Widget::ClosedReason) {
          if (!owner) return;
          owner->accept_ = nullptr; owner->reject_ = nullptr; owner->trust_ = nullptr;
          owner->widget_.reset();
        }, weak_.GetWeakPtr()));
    auto* frame = delegate_->GetBubbleFrameView(); frame->SetDisplayVisibleArrow(true);
    frame->bubble_border()->set_draw_border_stroke(true); frame->bubble_border()->set_md_shadow_elevation(6);
    delegate_->SizeToContents(); widget_->Show();
  }
  gfx::Rect Anchor(const std::string& target) {
    const auto area = bounds_.Run(target);
    return gfx::Rect(std::max(area.x(), area.right() - 12), std::max(area.y(), area.bottom() - 12), 1, 1);
  }
  void Refresh() {
    if (parent_->IsClosed()) { timer_.Stop(); Close(); return; }
    const auto state = service_->GetSnapshot();
    const bool prompt = state.receiving && state.busy &&
        (state.stage == NearbySharing::Stage::kPairing || state.stage == NearbySharing::Stage::kReceivingOffer);
    if (!prompt) { Close(); return; }
    if (!widget_ || session_ != state.session || stage_ != state.stage) Show(state);
    const auto anchor = Anchor(state.receive_target);
    if (delegate_->GetAnchorRect() != anchor) delegate_->SetAnchorRect(anchor);
    if (!decided_ && base::TimeTicks::Now() >= ready_) {
      if (accept_) accept_->SetEnabled(true);
      if (reject_) reject_->SetEnabled(true);
    }
  }
  raw_ptr<views::Widget> parent_;
  std::shared_ptr<NearbySharing> service_;
  base::RepeatingCallback<gfx::Rect(const std::string&)> bounds_;
  std::unique_ptr<views::BubbleDialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<views::Checkbox> trust_ = nullptr;
  raw_ptr<views::MdTextButton> accept_ = nullptr, reject_ = nullptr;
  uint64_t session_ = 0;
  NearbySharing::Stage stage_ = NearbySharing::Stage::kOff;
  bool decided_ = false;
  base::TimeTicks ready_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<IncomingShareToastImpl> weak_{this};
};

class DeviceButton : public views::Button {
  METADATA_HEADER(DeviceButton, views::Button)
 public:
  DeviceButton(PressedCallback callback, ui::ImageModel avatar, std::u16string name) : views::Button(std::move(callback)) {
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(6), 8));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    image_ = AddChildView(std::make_unique<views::ImageView>());
    image_->SetPreferredSize(gfx::Size(64, 64)); image_->SetImageSize(gfx::Size(64, 64));
    image_->SetCanProcessEventsWithinSubtree(false);
    auto* name_row = AddChildView(std::make_unique<views::View>());
    auto* name_layout = name_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
    name_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    name_row->SetCanProcessEventsWithinSubtree(false);
    label_ = name_row->AddChildView(std::make_unique<views::Label>()); label_->SetMaximumWidthSingleLine(112);
    trust_icon_ = name_row->AddChildView(std::make_unique<views::ImageView>());
    trust_icon_->SetImage(ui::ImageModel::FromVectorIcon(kAndroidSecurityPrivacySafeFilledIcon,
        gfx::kGoogleGreen600, label_->font_list().GetHeight()));
    trust_icon_->SetVisible(false);
    label_->SetCanProcessEventsWithinSubtree(false); SetIdentity(std::move(avatar), std::move(name));
  }
  void SetIdentity(ui::ImageModel avatar, std::u16string name) {
    avatar_ = std::move(avatar); UpdateAvatar();
    label_->SetText(name); SetAccessibleName(name); SetTooltipText(name);
  }
  void SetTransfer(bool active, bool indeterminate, double progress,
                   base::RepeatingClosure cancel = {}) {
    transfer_ = active; indeterminate_ = indeterminate; UpdateAvatar();
    progress_ = std::clamp(progress, 0.0, 1.0); cancel_ = std::move(cancel);
    if (active && indeterminate && !spinner_.IsRunning()) {
      spin_start_ = base::TimeTicks::Now();
      spinner_.Start(FROM_HERE, base::Milliseconds(33),
          base::BindRepeating(&DeviceButton::SchedulePaint, base::Unretained(this)));
    } else if (!active || !indeterminate) spinner_.Stop();
    SetAccessibleName(active ? Text(IDS_CRFILES_CANCEL) : std::u16string(label_->GetText()));
    SetTooltipText(active ? Text(IDS_CRFILES_CANCEL) : std::u16string(label_->GetText()));
    SchedulePaint();
  }
  void NotifyClick(const ui::Event& event) override {
    if (transfer_) { if (cancel_) cancel_.Run(); return; }
    views::Button::NotifyClick(event);
  }
  void OnPaintBorder(gfx::Canvas* canvas) override {
    views::Button::OnPaintBorder(canvas);
    if (!transfer_) return;
    cc::PaintFlags flags;
    flags.setAntiAlias(true); flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(3);
    const auto center = gfx::PointF(image_->bounds().CenterPoint());
    const float radius = image_->width() / 2.0f + 2;
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimaryContainer));
    canvas->DrawCircle(center, radius, flags);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
    const float angle = indeterminate_ ? std::fmod(
        (base::TimeTicks::Now() - spin_start_).InMillisecondsF() * 0.3, 360.0) : -90;
    canvas->sk_canvas()->drawArc(SkRect::MakeLTRB(center.x() - radius, center.y() - radius,
        center.x() + radius, center.y() + radius), angle, indeterminate_ ? 90 : 360 * progress_, flags);
    if (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) {
      flags.setStyle(cc::PaintFlags::kFill_Style);
      flags.setColor(GetColorProvider()->GetColor(ui::kColorSysSurface));
      canvas->DrawCircle(center, image_->width() / 2.0f, flags);
      const auto icon = gfx::CreateVectorIcon(vector_icons::kCloseIcon, 48,
          GetColorProvider()->GetColor(ui::kColorSysError));
      canvas->DrawImageInt(icon, static_cast<int>(center.x()) - 24, static_cast<int>(center.y()) - 24);
    }
  }
  void SetTrusted(bool trusted) { trust_icon_->SetVisible(trusted); }
  views::View* IconAnchor() { return image_; }
 protected:
  void StateChanged(ButtonState old) override {
    views::Button::StateChanged(old);
    UpdateAvatar();
    image_->SetBackground(!transfer_ && (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) ?
        views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 32) : nullptr);
  }
 private:
  void UpdateAvatar() {
    if (transfer_) image_->SetBackground(nullptr);
    image_->SetImage(transfer_ && (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) ?
        ui::ImageModel() : avatar_);
  }
  ui::ImageModel avatar_;
  raw_ptr<views::ImageView> image_ = nullptr, trust_icon_ = nullptr;
  raw_ptr<views::Label> label_ = nullptr;
  bool transfer_ = false, indeterminate_ = false;
  double progress_ = 0;
  base::RepeatingClosure cancel_;
  base::TimeTicks spin_start_;
  base::RepeatingTimer spinner_;
};
BEGIN_METADATA(DeviceButton)
END_METADATA
class TrustedDevicesView : public views::View {
  METADATA_HEADER(TrustedDevicesView, views::View)

 public:
  explicit TrustedDevicesView(std::shared_ptr<NearbySharing> service)
      : service_(std::move(service)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 12));
    trust_error_ = AddText(IDS_CRFILES_NEARBY_ERROR);
    trust_error_->SetVisible(false);
    trusted_list_ = AddChildView(std::make_unique<views::View>());
    trusted_list_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
    empty_trust_ = AddText(IDS_CRFILES_NEARBY_NO_TRUSTED_DEVICES);
    Refresh();
    timer_.Start(FROM_HERE, base::Milliseconds(250),
        base::BindRepeating(&TrustedDevicesView::Refresh, base::Unretained(this)));
  }

 private:
  views::Label* AddText(int id) {
    auto* label = AddChildView(std::make_unique<views::Label>(Text(id)));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetMultiLine(true);
    label->SetMaximumWidth(600);
    return label;
  }
  void Refresh() {
    const auto state = service_->GetSnapshot();
    const auto devices = service_->GetTrustedDevices();
    trust_error_->SetVisible(state.trust_error);
    empty_trust_->SetVisible(devices.empty());
    if (devices != trusted_devices_) {
      trusted_devices_ = devices;
      trusted_list_->RemoveAllChildViews();
      for (const auto& device : devices) {
        auto* row = trusted_list_->AddChildView(std::make_unique<views::View>());
        auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
        layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
        auto* avatar = row->AddChildView(std::make_unique<views::ImageView>());
        avatar->SetImage(Avatar(device.avatar, 48));
        auto* text = row->AddChildView(std::make_unique<views::View>());
        text->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
        layout->SetFlexForView(text, 1);
        const auto details = l10n_util::GetStringFUTF16(
            IDS_CRFILES_NEARBY_TRUST_DETAILS, base::UTF8ToUTF16(device.name),
            base::TimeFormatShortDateAndTime(device.trusted_at),
            base::TimeFormatShortDateAndTime(device.last_active));
        auto* first = text->AddChildView(std::make_unique<views::Label>(details));
        first->SetHorizontalAlignment(gfx::ALIGN_LEFT);
        first->SetEnabledColor(ui::kColorSysPrimary);
        first->SetTooltipText(details + u"\n" + base::UTF8ToUTF16(device.fingerprint));
        auto* second = text->AddChildView(std::make_unique<views::Label>(
            l10n_util::GetStringFUTF16(IDS_CRFILES_NEARBY_TRUST_STATS,
                ui::FormatBytes(base::ByteSize(device.sent_bytes)),
                ui::FormatBytes(base::ByteSize(device.received_bytes)))));
        second->SetHorizontalAlignment(gfx::ALIGN_LEFT);
        auto forget = views::CreateVectorImageButtonWithNativeTheme(
            base::BindRepeating(
                [](std::shared_ptr<NearbySharing> service, std::string fingerprint) {
                  service->ForgetTrustedDevice(std::move(fingerprint));
                }, service_, device.fingerprint),
            kDeleteIcon, 20, ui::kColorSysError, ui::kColorIconDisabled,
            ui::kColorSysError);
        forget->SetPreferredSize(gfx::Size(36, 36));
        forget->SetTooltipText(Text(IDS_CRFILES_NEARBY_FORGET));
        forget->GetViewAccessibility().SetName(Text(IDS_CRFILES_NEARBY_FORGET));
        row->AddChildView(std::move(forget));
        trusted_list_->AddChildView(std::make_unique<views::Separator>());
      }
    }
  }
  std::shared_ptr<NearbySharing> service_;
  raw_ptr<views::View> trusted_list_;
  raw_ptr<views::Label> empty_trust_;
  raw_ptr<views::Label> trust_error_;
  std::vector<NearbySharing::TrustedDevice> trusted_devices_;
  base::RepeatingTimer timer_;
};
BEGIN_METADATA(TrustedDevicesView)
END_METADATA

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
    summary_container_ = summary;
    summary->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
    summary->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 12));
    count_ = Label(summary, base::NumberToString16(paths_.size()) + u" " + Text(IDS_CRFILES_ITEMS));
    size_ = Label(summary, Text(IDS_CRFILES_TOTAL_SIZE) + u": —");
    status_ = Label(this, std::u16string());
    status_->SetVisible(false);
    enable_ = AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Enable, weak_.GetWeakPtr()), Text(IDS_CRFILES_ENABLE)));
    auto* devices = AddChildView(std::make_unique<views::View>());
    auto* layout = devices->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kStart);
    manual_ = devices->AddChildView(std::make_unique<DeviceButton>(base::BindRepeating(&FileShareView::Manual, weak_.GetWeakPtr()),
        ui::ImageModel::FromVectorIcon(vector_icons::kAdd2Icon, ui::kColorSysPrimary, 48), Text(IDS_CRFILES_MANUAL_CONNECTION)));
    auto* separator = devices->AddChildView(std::make_unique<views::Separator>());
    separator->SetOrientation(views::Separator::Orientation::kVertical); separator->SetPreferredLength(96);
    peers_ = devices->AddChildView(std::make_unique<views::View>());
    peers_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
    layout->SetFlexForView(peers_, 1);
    session_device_ = devices->AddChildView(std::make_unique<DeviceButton>(base::RepeatingClosure(), Avatar(0), Text(IDS_CRFILES_TRANSFERRING)));
    session_device_->SetVisible(false);
    if (service_->GetSnapshot().busy) {
      if (!paths_.empty()) base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&Summarize, paths_), base::BindOnce(&FileShareView::SummaryReady, weak_.GetWeakPtr(), ++summary_generation_));
    } else SetSelection(paths_);
    Refresh(); timer_.Start(FROM_HERE, base::Milliseconds(250), this, &FileShareView::Refresh);
  }
  ~FileShareView() override { timer_.Stop(); CloseToast(); if (own_session_) service_->CancelSession(own_session_); }
  void SetSelection(std::vector<base::FilePath> paths) {
    if (service_->GetSnapshot().busy) return;
    CloseToast(); active_device_ = nullptr; selected_.reset(); paths_ = std::move(paths);
    const auto state = service_->GetSnapshot(); shown_session_ = state.session; shown_stage_ = state.stage;
    count_->SetText(base::NumberToString16(paths_.size()) + u" " + Text(IDS_CRFILES_ITEMS));
    size_->SetText(Text(IDS_CRFILES_TOTAL_SIZE) + u": —");
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&Summarize, paths_), base::BindOnce(&FileShareView::SummaryReady, weak_.GetWeakPtr(), ++summary_generation_));
  }
 private:
  views::Label* Label(views::View* owner, std::u16string text) {
    auto* label = owner->AddChildView(std::make_unique<views::Label>(text));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetElideBehavior(gfx::ELIDE_MIDDLE); return label;
  }
  void SummaryReady(uint64_t generation, Summary summary) {
    if (generation != summary_generation_) return;
    const auto state = service_->GetSnapshot(); if (state.receiving && state.busy) return;
    count_->SetText(Text(IDS_CRFILES_FILES) + u": " + base::NumberToString16(summary.files) + u"   ·   " +
        Text(IDS_CRFILES_FOLDERS) + u": " + base::NumberToString16(summary.folders));
    size_->SetText(Text(IDS_CRFILES_TOTAL_SIZE) + u": " + (summary.bytes ? ui::FormatBytes(base::ByteSize(*summary.bytes)) : std::u16string(u"—")));
    if (toast_count_) toast_count_->SetText(count_->GetText());
    if (toast_size_) toast_size_->SetText(size_->GetText());
  }
  void Enable() {
    auto prefs = GetNearbySharingPreferences(); prefs.discovery_enabled = true;
    if (SetNearbySharingPreferences(prefs)) ApplyCrFilesSharingSettings();
    else { status_->SetText(Text(IDS_CRFILES_CONFIG_ERROR)); status_->SetVisible(true); }
  }
  void CloseToast() {
    address_ = nullptr; targets_ = nullptr; toast_status_ = nullptr; toast_count_ = nullptr; toast_size_ = nullptr;
    accept_ = nullptr; reject_ = nullptr; trust_device_ = nullptr;
    if (toast_delegate_) toast_delegate_->SetAnchorView(nullptr);
    // Explicit teardown must not schedule another close from native callbacks.
    if (toast_widget_) toast_widget_->MakeCloseSynchronous({});
    toast_widget_.reset(); toast_delegate_.reset();
    toast_prompt_ = false; composing_ = false;
  }
  void PresentToast(views::View* anchor, std::unique_ptr<views::View> contents, bool prompt) {
    toast_delegate_ = std::make_unique<views::BubbleDialogDelegate>(anchor, views::BubbleBorder::TOP_CENTER);
    toast_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    toast_delegate_->SetShowCloseButton(false);
    toast_delegate_->set_close_on_deactivate(!prompt);
    if (prompt) toast_delegate_->SetCancelCallbackWithClose(base::BindRepeating([] { return false; }));
    toast_delegate_->set_margins(gfx::Insets(12));
    toast_delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    toast_delegate_->SetContentsView(std::move(contents));
    toast_widget_ = CreateCrFilesBubble(toast_delegate_.get(),
        base::BindOnce([](base::WeakPtr<FileShareView> owner, views::Widget::ClosedReason) {
          if (!owner || !owner->toast_widget_) return;
          owner->CloseToast();
        }, weak_.GetWeakPtr()));
    auto* frame = toast_delegate_->GetBubbleFrameView();
    frame->SetDisplayVisibleArrow(true); frame->bubble_border()->set_draw_border_stroke(true);
    frame->bubble_border()->set_md_shadow_elevation(6);
    toast_delegate_->SizeToContents(); toast_widget_->Show();
  }
  std::unique_ptr<views::View> ToastContents(ui::ImageModel avatar, std::u16string name,
                                           const NearbySharing::Snapshot* state = nullptr) {
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
    auto* identity = contents->AddChildView(std::make_unique<views::View>());
    identity->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    auto* image = identity->AddChildView(std::make_unique<views::ImageView>(avatar)); image->SetImageSize(gfx::Size(64, 64));
    auto* title = Label(identity, std::move(name)); title->SetMaximumWidthSingleLine(248);
    if (state && state->stage == NearbySharing::Stage::kPairing) return contents;
    auto* information = contents->AddChildView(std::make_unique<views::View>());
    information->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(12), 6));
    information->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 12));
    if (state && state->receiving) {
      Label(information, Text(IDS_CRFILES_FILES) + u": " + (state->files.empty() ? std::u16string(u"—") : base::NumberToString16(state->files.size())));
      Label(information, Text(IDS_CRFILES_TOTAL_SIZE) + u": " + (state->files.empty() ? std::u16string(u"—") : ui::FormatBytes(base::ByteSize(state->total))));
    } else {
      toast_count_ = Label(information, std::u16string(count_->GetText())); toast_size_ = Label(information, std::u16string(size_->GetText()));
    }
    return contents;
  }
  void Manual() {
    const auto state = service_->GetSnapshot(); if (state.busy || !state.enabled || paths_.empty()) return;
    selected_.reset(); Compose(manual_);
  }
  void SelectPeer(DeviceButton* anchor, NearbySharing::Peer peer) {
    const auto state = service_->GetSnapshot(); if (state.busy) return;
    const auto latest = std::ranges::find_if(state.peers, [&](const auto& item) {
      return item.id == peer.id && item.address == peer.address && item.phone.has_value() == peer.phone.has_value();
    });
    if (latest == state.peers.end()) return;
    selected_ = *latest; Compose(anchor);
  }
  void Compose(DeviceButton* anchor) {
    CloseToast(); composing_ = true; active_device_ = anchor;
    auto contents = std::make_unique<views::View>();
    auto* layout = contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    if (!selected_) {
      address_ = contents->AddChildView(std::make_unique<views::Textfield>());
      address_->SetPreferredSize(gfx::Size(240, 36));
      address_->SetAccessibleName(Text(IDS_CRFILES_DEVICE_ADDRESS)); address_->SetPlaceholderText(u"192.168.1.2:59487"); address_->RemoveHoverEffect();
    } else {
      std::vector<ui::SimpleComboboxModel::Item> items; items.emplace_back(Text(IDS_CRFILES_RECEIVED));
      for (const auto& target : selected_->targets) items.emplace_back(base::UTF8ToUTF16(target.name));
      targets_ = contents->AddChildView(std::make_unique<views::Combobox>(std::make_unique<ui::SimpleComboboxModel>(std::move(items))));
      targets_->SetPreferredSize(gfx::Size(240, 36));
      targets_->SetAccessibleName(Text(IDS_CRFILES_HISTORY_DESTINATION)); targets_->SetSelectedIndex(0);
    }
    contents->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Send, weak_.GetWeakPtr()), Text(IDS_CRFILES_HISTORY_SEND)));
    PresentToast(anchor->IconAnchor(), std::move(contents), false);
  }
  void Send() {
    const auto state = service_->GetSnapshot(); if (state.busy || !composing_) return;
    std::string target;
    const size_t index = targets_ ? targets_->GetSelectedIndex().value_or(0) : 0;
    if (selected_ && index > 0 && index <= selected_->targets.size()) target = selected_->targets[index - 1].id;
    const bool sent = service_->Send(selected_ ? selected_->address : base::UTF16ToUTF8(address_->GetText()), paths_, {},
        selected_ ? selected_->phone : std::nullopt, target);
    send_error_ = !sent;
    if (sent) { own_session_ = service_->GetSnapshot().session; dismiss_compose_ = true; }
    // The timer replaces the toast after this button's event dispatch returns.
  }
  void Decide(uint64_t session, NearbySharing::Stage stage, bool accept) {
    if (decision_sent_ || base::TimeTicks::Now() < prompt_ready_) return;
    decision_sent_ = true; service_->Decide(session, stage, accept,
        stage == NearbySharing::Stage::kPairing && trust_device_ && trust_device_->GetChecked());
    if (accept_) accept_->SetEnabled(false);
    if (reject_) reject_->SetEnabled(false);
  }
  void Cancel(uint64_t session) { service_->CancelSession(session); }
  void ShowSessionToast(const NearbySharing::Snapshot& state, int message) {
    CloseToast();
    shown_session_ = state.session; shown_stage_ = state.stage;
    toast_prompt_ = state.stage == NearbySharing::Stage::kPairing || state.stage == NearbySharing::Stage::kReceivingOffer;
    decision_sent_ = false; prompt_ready_ = base::TimeTicks::Now() + base::Milliseconds(500);
    std::unique_ptr<views::View> contents;
    if (!state.busy) {
      contents = std::make_unique<views::View>();
      auto* layout = contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical, gfx::Insets(), 8));
      layout->set_minimum_cross_axis_size(336);
      auto result_label = [&](std::u16string text) {
        auto* label = Label(contents.get(), std::move(text));
        // Label() defaults to ELIDE_MIDDLE, which is invalid for multiline text.
        label->SetElideBehavior(gfx::NO_ELIDE);
        label->SetMultiLine(true);
        label->SetAllowCharacterBreak(true);
        label->SetMaximumWidth(336);
        return label;
      };
      toast_status_ = result_label(Text(message));
      result_label(Text(IDS_CRFILES_TOTAL_SIZE) + u": " + ui::FormatBytes(base::ByteSize(state.total)));
      const auto elapsed = state.started_at.is_null() || state.finished_at.is_null() ?
          base::TimeDelta() : state.finished_at - state.started_at;
      const int64_t seconds = std::max<int64_t>(0, elapsed.InSeconds());
      const auto duration = l10n_util::GetStringFUTF16(IDS_CRFILES_DURATION_MINUTES_SECONDS,
          base::NumberToString16(seconds / 60), base::NumberToString16(seconds % 60));
      result_label(Text(IDS_CRFILES_TOTAL_TIME) + u": " + duration);
    } else {
      contents = ToastContents(PeerAvatar(state), base::UTF8ToUTF16(state.peer_name.empty() && selected_ ? selected_->name : state.peer_name), &state);
    }
    if (state.busy) {
      toast_status_ = Label(contents.get(), Text(message));
      toast_status_->SetElideBehavior(gfx::NO_ELIDE);
      toast_status_->SetMultiLine(true); toast_status_->SetMaximumWidth(336);
    }
    if (toast_prompt_) {
      toast_status_->SetText(Text(state.stage == NearbySharing::Stage::kPairing ? IDS_CRFILES_COMPARE_CODE : IDS_CRFILES_RECEIVE_PROMPT) +
          (state.stage == NearbySharing::Stage::kPairing ? u"\n" + base::UTF8ToUTF16(state.code) : std::u16string()));
      if (state.stage == NearbySharing::Stage::kPairing)
        trust_device_ = contents->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_NEARBY_TRUST_DEVICE)));
      auto* actions = contents->AddChildView(std::make_unique<views::View>());
      actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
      actions->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 20));
      accept_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Decide, weak_.GetWeakPtr(), state.session, state.stage, true), Text(IDS_CRFILES_ACCEPT)));
      actions->AddChildView(std::make_unique<views::Separator>())->SetOrientation(views::Separator::Orientation::kVertical);
      reject_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Decide, weak_.GetWeakPtr(), state.session, state.stage, false), Text(IDS_CRFILES_REJECT)));
      accept_->SetEnabled(false); reject_->SetEnabled(false);
    } else if (state.busy) {
      contents->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Cancel, weak_.GetWeakPtr(), state.session), Text(IDS_CRFILES_CANCEL)));
    }
    auto* anchor = state.receiving || !active_device_ ? session_device_.get() : active_device_.get();
    PresentToast(anchor->IconAnchor(), std::move(contents), toast_prompt_);
  }
  void Refresh() {
    const auto state = service_->GetSnapshot();
    if (dismiss_compose_) { dismiss_compose_ = false; CloseToast(); }
    using Stage = NearbySharing::Stage;
    summary_container_->SetVisible(state.stage != Stage::kPairing);
    if (state.receiving && (state.busy || paths_.empty())) {
      count_->SetText(Text(IDS_CRFILES_FILES) + u": " + (state.files.empty() ? std::u16string(u"—") : base::NumberToString16(state.files.size())));
      size_->SetText(Text(IDS_CRFILES_TOTAL_SIZE) + u": " + (state.files.empty() ? std::u16string(u"—") : ui::FormatBytes(base::ByteSize(state.total))));
    }
    int message = !state.enabled ? IDS_CRFILES_NEARBY_OFF :
        !state.port ? IDS_CRFILES_LOADING :
        !state.discovery_ready ? IDS_CRFILES_NEARBY_DISCOVERY_UNAVAILABLE :
        IDS_CRFILES_NEARBY_READY;
    if (state.status_message_id) message = state.status_message_id;
    else if (state.stage == Stage::kComplete) message = IDS_CRFILES_COMPLETE;
    else if (state.stage == Stage::kError) message = IDS_CRFILES_TRANSFER_ERROR;
    else if (state.stage == Stage::kCanceled) message = IDS_CRFILES_CANCELED;
    else if (state.busy) message = IDS_CRFILES_TRANSFERRING;
    status_->SetText(Text(send_error_ && !state.busy ? IDS_CRFILES_SEND_ERROR : message));
    status_->SetVisible(message != IDS_CRFILES_NEARBY_READY || (send_error_ && !state.busy));
    enable_->SetVisible(!state.enabled);
    const bool can_send = state.enabled && state.port && !state.busy && !paths_.empty();
    manual_->SetEnabled(can_send || (state.busy && active_device_ == manual_));
    const bool same_devices = std::ranges::equal(known_peers_, state.peers, [](const auto& left, const auto& right) {
      return left.id == right.id && left.address == right.address && left.name == right.name &&
          left.avatar == right.avatar && left.phone.has_value() == right.phone.has_value();
    });
    if (!state.busy && !toast_widget_ && !same_devices) {
      active_device_ = nullptr;
      known_peers_ = state.peers; peers_->RemoveAllChildViews();
      views::View* row = nullptr; size_t index = 0;
      for (const auto& peer : known_peers_) {
        if (index++ % 3 == 0) {
          row = peers_->AddChildView(std::make_unique<views::View>());
          row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
        }
        auto* button = row->AddChildView(std::make_unique<DeviceButton>(base::RepeatingClosure(), Avatar(peer.avatar), base::UTF8ToUTF16(peer.name)));
        button->SetTrusted(peer.trusted);
        button->SetCallback(base::BindRepeating(&FileShareView::SelectPeer, weak_.GetWeakPtr(), base::Unretained(button), peer));
      }
    }
    // Refresh trust badges even while a toast pins the discovery buttons.
    size_t peer_index = 0;
    for (const auto& row : peers_->children()) for (const auto& child : row->children()) {
      if (peer_index >= known_peers_.size()) break;
      const auto& displayed = known_peers_[peer_index++];
      const auto latest = std::ranges::find_if(state.peers, [&](const auto& peer) {
        return peer.id == displayed.id && peer.address == displayed.address &&
            peer.phone.has_value() == displayed.phone.has_value();
      });
      auto* button = static_cast<DeviceButton*>(child.get());
      button->SetEnabled(can_send || (state.busy && button == active_device_));
      button->SetTrusted(state.busy && button == active_device_ && !state.code.empty() ? state.peer_trusted :
          latest != state.peers.end() && latest->trusted);
    }

    const bool session = state.busy || (!composing_ && (state.stage == Stage::kComplete || state.stage == Stage::kError || state.stage == Stage::kCanceled));
    // A fallback session avatar replaces the discovery list; otherwise the
    // connected device can appear once in each of the two views.
    const bool fallback = session && (state.receiving || !active_device_) &&
        (state.busy || toast_widget_ || shown_session_ != state.session || shown_stage_ != state.stage);
    session_device_->SetVisible(fallback);
    peers_->SetVisible(!fallback);
    session_device_->SetTrusted(state.peer_trusted);
    if (session) session_device_->SetIdentity(PeerAvatar(state), base::UTF8ToUTF16(state.peer_name));
    if (state.receiving && toast_widget_) CloseToast();
    const bool pairing = !state.receiving && state.busy && state.stage == Stage::kPairing;
    const bool finished = !state.receiving && !state.busy && !composing_ &&
        (state.stage == Stage::kComplete || state.stage == Stage::kError || state.stage == Stage::kCanceled);
    if ((pairing || finished) && GetWidget() && GetWidget()->IsVisible() &&
        (shown_session_ != state.session || shown_stage_ != state.stage)) ShowSessionToast(state, message);
    if (!pairing && toast_prompt_) CloseToast();
    if (!pairing) { shown_session_ = state.session; shown_stage_ = state.stage; }
    if (!session && !composing_ && toast_widget_) CloseToast();
    if (toast_prompt_ && !decision_sent_ && base::TimeTicks::Now() >= prompt_ready_) {
      if (accept_) accept_->SetEnabled(true);
      if (reject_) reject_->SetEnabled(true);
    }
    if (toast_status_ && !toast_prompt_) {
      const auto text = Text(message) + (state.busy ? u"\n" +
          ui::FormatBytes(base::ByteSize(state.transferred)) + u" / " + ui::FormatBytes(base::ByteSize(state.total)) : std::u16string());
      if (toast_status_->GetText() != text) {
        toast_status_->SetText(text);
        if (toast_delegate_ && toast_widget_) toast_delegate_->SizeToContents();
      }
    }
    auto* transfer_target = fallback ? session_device_.get() : active_device_.get();
    if (active_device_ && active_device_ != transfer_target) active_device_->SetTransfer(false, false, 0);
    if (session_device_ != transfer_target) session_device_->SetTransfer(false, false, 0);
    if (transfer_target) transfer_target->SetTransfer(state.busy,
        state.stage != Stage::kTransferring || state.total == 0,
        state.total ? static_cast<double>(state.transferred) / state.total : 0,
        base::BindRepeating(&FileShareView::Cancel, weak_.GetWeakPtr(), state.session));
  }

  std::shared_ptr<NearbySharing> service_;
  std::vector<base::FilePath> paths_;
  std::vector<NearbySharing::Peer> known_peers_;
  std::optional<NearbySharing::Peer> selected_;
  uint64_t own_session_ = 0;
  bool send_error_ = false;
  bool dismiss_compose_ = false;
  raw_ptr<views::View> peers_ = nullptr, summary_container_ = nullptr;
  raw_ptr<views::Label> count_ = nullptr, size_ = nullptr, status_ = nullptr, toast_status_ = nullptr, toast_count_ = nullptr, toast_size_ = nullptr;
  raw_ptr<views::Textfield> address_ = nullptr;
  raw_ptr<views::Combobox> targets_ = nullptr;
  raw_ptr<views::Checkbox> trust_device_ = nullptr;
  raw_ptr<views::MdTextButton> enable_ = nullptr, accept_ = nullptr, reject_ = nullptr;
  raw_ptr<DeviceButton> manual_ = nullptr, session_device_ = nullptr, active_device_ = nullptr;
  std::unique_ptr<views::BubbleDialogDelegate> toast_delegate_;
  std::unique_ptr<views::Widget> toast_widget_;
  uint64_t summary_generation_ = 0;
  uint64_t shown_session_ = 0;
  NearbySharing::Stage shown_stage_ = NearbySharing::Stage::kOff;
  bool toast_prompt_ = false, composing_ = false, decision_sent_ = false;
  base::TimeTicks prompt_ready_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<FileShareView> weak_{this};
};
BEGIN_METADATA(FileShareView)
END_METADATA
}
std::unique_ptr<IncomingShareToast> CreateIncomingShareToast(views::Widget* parent,
    std::shared_ptr<NearbySharing> service, base::RepeatingCallback<gfx::Rect(const std::string&)> bounds) {
  return std::make_unique<IncomingShareToastImpl>(parent, std::move(service), std::move(bounds));
}
views::View* ShowTrustedDevicesDialog(views::Widget* parent,
                                      std::shared_ptr<NearbySharing> service) {
  if (!parent) return nullptr;
  auto contents = std::make_unique<TrustedDevicesView>(std::move(service));
  auto* result = contents.get();
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  scroll->SetUseContentsPreferredSize(true);
  scroll->ClipHeightTo(288, 960);
  scroll->SetContents(std::move(contents));
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->set_fixed_width(680);
  dialog->SetTitle(Text(IDS_CRFILES_NEARBY_TRUSTED_DEVICES));
  dialog->SetModalType(ui::mojom::ModalType::kWindow);
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  dialog->SetShowCloseButton(true);
  dialog->SetContentsView(std::move(scroll));
  ShowCrFilesDialog(std::move(dialog), parent);
  return result;
}

void SetFileShareSelection(views::View* view, std::vector<base::FilePath> paths) {
  static_cast<FileShareView*>(view)->SetSelection(std::move(paths));
}
views::View* ShowFileShareDialog(views::Widget* parent, std::shared_ptr<NearbySharing> service, std::vector<base::FilePath> paths) {
  if (!parent || !service) return nullptr;
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  scroll->SetUseContentsPreferredSize(true); scroll->ClipHeightTo(300, 600);
  auto* contents = scroll->SetContents(std::make_unique<FileShareView>(std::move(service), std::move(paths)));
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->set_fixed_width(580); dialog->SetTitle(Text(IDS_CRFILES_SHARE_TO));
  dialog->SetModalType(ui::mojom::ModalType::kWindow);
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone)); dialog->SetShowCloseButton(true);
  dialog->SetContentsView(std::move(scroll));
  ShowCrFilesDialog(std::move(dialog), parent);
  return contents;
}
}
