// Copyright 2026 The Chromium Authors
#include "crFiles/sharing/file_share_dialog.h"
#include "crFiles/crfiles_dialog.h"
#include <algorithm>
#include <array>
#include <cmath>
#include "cc/paint/paint_flags.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkPathMeasure.h"
#include "third_party/skia/include/core/SkRRect.h"
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
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "components/vector_icons/vector_icons.h"
#include "crFiles/crfiles_app_icon.h"
#include "crFiles/crfiles_context_actions.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_window.h"
#include "libcr/svg_icon.h"
#if BUILDFLAG(IS_MAC)
#include "crFiles/crfiles_mac.h"
#endif
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
#include "ui/views/controls/progress_bar.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_tracker.h"
#include "ui/views/event_monitor.h"
#include "ui/events/event.h"
#include "ui/events/event_observer.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/client_view.h"
#include "ui/views/window/dialog_delegate.h"
#if BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_OZONE)
#include "ui/ozone/public/ozone_platform.h"
#endif
namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
std::u16string TransferProgressText(const NearbySharing::Snapshot& state, bool quick_share) {
  using Stage = NearbySharing::Stage;
  int message = IDS_CRFILES_TRANSFERRING;
  if (state.status_message_id) message = state.status_message_id;
  else if (state.stage == Stage::kComplete) message = IDS_CRFILES_COMPLETE;
  else if (state.stage == Stage::kError) message = IDS_CRFILES_TRANSFER_ERROR;
  else if (state.stage == Stage::kCanceled || !state.busy) message = IDS_CRFILES_CANCELED;
  else if (state.stage != Stage::kTransferring) message = IDS_CRFILES_LOADING;
  auto status = Text(message) + u"\n" +
      ui::FormatBytes(base::ByteSize(state.transferred)) + u" / " + ui::FormatBytes(base::ByteSize(state.total));
  if (quick_share && state.current_file_index) {
    status += u"\n" + Text(IDS_CRFILES_QS_PROGRESS_FILE) + u": " +
        base::NumberToString16(state.current_file_index) + u" / " + base::NumberToString16(state.files.size()) +
        u" · " + ui::FormatBytes(base::ByteSize(state.current_file_transferred)) + u" / " +
        ui::FormatBytes(base::ByteSize(state.current_file_total));
    status += u"\n" + Text(IDS_CRFILES_QS_PROGRESS_FINISHED) + u": " +
        base::NumberToString16(state.completed_files) + u" / " + base::NumberToString16(state.files.size());
    if (state.busy && state.stage == Stage::kTransferring) {
      status += u"\n" + Text(IDS_CRFILES_QS_PROGRESS_SPEED) + u": " +
          ui::FormatSpeed(base::ByteSize(state.bytes_per_second));
      status += u"\n" + Text(IDS_CRFILES_QS_PROGRESS_REMAINING) + u": " +
          (state.remaining_seconds < 0 ? std::u16string(u"—") :
           base::NumberToString16(state.remaining_seconds / 60) + u":" +
           (state.remaining_seconds % 60 < 10 ? u"0" : u"") + base::NumberToString16(state.remaining_seconds % 60));
    }
  }
  if (!state.busy && !state.started_at.is_null() && !state.finished_at.is_null()) {
    const auto seconds = std::max<int64_t>(0, (state.finished_at - state.started_at).InSeconds());
    status += u"\n" + Text(IDS_CRFILES_TOTAL_TIME) + u": " +
        l10n_util::GetStringFUTF16(IDS_CRFILES_DURATION_MINUTES_SECONDS,
            base::NumberToString16(seconds / 60), base::NumberToString16(seconds % 60));
  }
  if (state.warning_message_id) status += u"\n" + Text(state.warning_message_id);
  return status;
}
ui::ImageModel QuickShareAvatar(int edge = 64) {
  static base::NoDestructor<libcr::SvgIconCache> icon(
      ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(IDR_CRFILES_QUICK_SHARE_ICON),
      gfx::SizeF(150, 150));
  return icon->GetImageModel(gfx::Size(edge, edge));
}
ui::ImageModel QuickShareDeviceAvatar(uint8_t type) {
  const gfx::VectorIcon* icon = &vector_icons::kDevicesIcon;
  if (type == 3) icon = &kComputerCustomIcon;
  else if (type == 1 || type == 2 || type == 5) icon = &kHardwareSmartphoneOldIcon;
  return ui::ImageModel::FromVectorIcon(*icon, ui::kColorSysOnSurface, 54);
}
std::u16string QuickShareDeviceType(uint8_t type) {
  static constexpr std::array ids = {IDS_CRFILES_QS_TYPE_UNKNOWN, IDS_CRFILES_QS_TYPE_PHONE,
      IDS_CRFILES_QS_TYPE_TABLET, IDS_CRFILES_QS_TYPE_LAPTOP, IDS_CRFILES_QS_TYPE_CAR,
      IDS_CRFILES_QS_TYPE_FOLDABLE, IDS_CRFILES_QS_TYPE_XR};
  return Text(ids[type < ids.size() ? type : 0]);
}
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
class ShareSessionBubbleDelegate : public views::BubbleDialogDelegate {
 public:
  ShareSessionBubbleDelegate(views::BubbleBorder::Arrow arrow,
                            base::WeakPtr<views::Widget> bounds_owner)
      : views::BubbleDialogDelegate(nullptr, arrow), bounds_owner_(bounds_owner) {
    if (bounds_owner_) {
      set_available_screen_bounds_callback(base::BindRepeating(
          [](base::WeakPtr<views::Widget> owner, const gfx::Rect&) {
            return owner ? owner->GetClientAreaBoundsInScreen() : gfx::Rect();
          }, bounds_owner_));
    }
  }
  gfx::Rect GetBubbleBounds() override {
    if (!bounds_owner_) return views::BubbleDialogDelegate::GetBubbleBounds();
    // Wayland disables screen-based adjustment. A parented bubble can instead
    // fit within its owner's coordinates without moving the arrow's anchor.
    auto* frame = GetBubbleFrameView();
    frame->set_use_anchor_window_bounds(false);
    frame->SetPreferredArrowAdjustment(views::BubbleFrameView::PreferredArrowAdjustment::kOffset);
    return frame->GetUpdatedWindowBounds(GetAnchorRect(), arrow(),
        GetWidget()->client_view()->GetPreferredSize({}), true);
  }
 private:
  base::WeakPtr<views::Widget> bounds_owner_;
};
constexpr base::TimeDelta kReceiveResultDuration = base::Seconds(8);
constexpr base::TimeDelta kSendResultDuration = base::Seconds(5);

class CountdownCloseButton : public views::MdTextButton {
  METADATA_HEADER(CountdownCloseButton, views::MdTextButton)
 public:
  CountdownCloseButton(PressedCallback callback, const std::u16string& text)
      : views::MdTextButton(std::move(callback), text) {}
  void SetDeadline(base::TimeTicks deadline, base::TimeDelta duration) {
    if (deadline_ == deadline && duration_ == duration) return;
    deadline_ = deadline;
    duration_ = duration;
    repaint_timer_.Stop();
    SetStrokeColorOverrideDeprecated(deadline.is_null() ? std::nullopt : std::optional<SkColor>(SK_ColorTRANSPARENT));
    if (!deadline.is_null()) {
      repaint_timer_.Start(FROM_HERE, base::Milliseconds(33), this, &CountdownCloseButton::Tick);
    }
    SchedulePaint();
  }
 protected:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    views::MdTextButton::OnBoundsChanged(previous_bounds);
    SetCornerRadius(std::min(width(), height()) / 2.0f);
  }
  void PaintButtonContents(gfx::Canvas* canvas) override {
    views::MdTextButton::PaintButtonContents(canvas);
    if (deadline_.is_null() || width() <= 2 || height() <= 2) return;
    const double remaining = std::clamp((deadline_ - base::TimeTicks::Now()) / duration_, 0.0, 1.0);
    SkPathBuilder outline;
    const float radius = std::min(width() - 2, height() - 2) / 2.0f;
    outline.addRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(1, 1, width() - 2, height() - 2), radius, radius), SkPathDirection::kCW);
    const SkPath path = outline.detach();
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(2);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorButtonBorder));
    canvas->DrawPath(path, flags);
    if (remaining <= 0) return;
    SkPathMeasure measure(path, true);
    SkPathBuilder segment;
    if (!measure.getSegment(measure.getLength() * (1 - remaining), measure.getLength(), &segment, true)) return;
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
    canvas->DrawPath(segment.detach(), flags);
  }
 private:
  void Tick() {
    SchedulePaint();
    if (base::TimeTicks::Now() >= deadline_) repaint_timer_.Stop();
  }
  base::TimeTicks deadline_;
  base::TimeDelta duration_ = kReceiveResultDuration;
  base::RepeatingTimer repaint_timer_;
};
BEGIN_METADATA(CountdownCloseButton)
END_METADATA

class ShareSessionToastImpl : public ShareSessionToast, public views::WidgetObserver {
 public:
  ShareSessionToastImpl(views::Widget* owner, std::shared_ptr<ShareService> service,
                        base::RepeatingCallback<ShareBubbleAnchor(const NearbySharing::Snapshot&)> anchor)
      : owner_(owner->GetWeakPtr()), service_(std::move(service)), anchor_(std::move(anchor)) {
    owner_observation_.Observe(owner);
    Refresh(); timer_.Start(FROM_HERE, base::Milliseconds(250), this, &ShareSessionToastImpl::Refresh);
  }
  ~ShareSessionToastImpl() override { result_close_timer_.Stop(); owner_observation_.Reset(); timer_.Stop(); Close(); }
 private:
  bool UseParentWindow(const ShareBubbleAnchor& anchor) const {
    if (!anchor.background && anchor.widget) return true;
#if BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_OZONE)
    return owner_ && !anchor.background &&
        !ui::OzonePlatform::GetInstance()->GetPlatformProperties()
             .supports_global_screen_coordinates;
#else
    return false;
#endif
  }
  void OnWidgetActivationChanged(views::Widget*, bool active) override {
    if (!active) return;
    stacking_dirty_ = true;
    // Let native activation finish before ordering the independent bubble.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&ShareSessionToastImpl::Refresh, weak_.GetWeakPtr()));
  }
  void OnWidgetVisibilityChanged(views::Widget*, bool) override {
    stacking_dirty_ = true;
  }
  void OnWidgetBoundsChanged(views::Widget*, const gfx::Rect&) override {
    if (parented_ && delegate_ && widget_) delegate_->SizeToContents();
  }
  void OnWidgetDestroying(views::Widget* widget) override {
    if (widget != owner_.get()) {
      Close();
      return;
    }
    owner_observation_.Reset();
    owner_.reset();
    result_close_timer_.Stop();
    timer_.Stop();
    Close();
  }
  void Close() {
    anchor_observation_.Reset();
    parent_widget_.reset();
    accept_ = nullptr; reject_ = nullptr; trust_ = nullptr;
    filename_ = nullptr; progress_status_ = nullptr; progress_ = nullptr; cancel_ = nullptr; open_received_ = nullptr;
    if (widget_) widget_->MakeCloseSynchronous({});
    widget_.reset(); delegate_.reset();
    background_mode_.reset();
    stacking_dirty_ = true;
  }
  views::Label* Label(views::View* parent, std::u16string text) {
    auto* label = parent->AddChildView(std::make_unique<views::Label>(text));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true);
    label->SetMaximumWidth(336); return label;
  }
  void Decide(bool accept) {
    if (decided_ || base::TimeTicks::Now() < ready_) return;
    decided_ = true;
    if (accept && (stage_ == NearbySharing::Stage::kReceivingOffer ||
        (service_->IsQuickShare() && stage_ == NearbySharing::Stage::kPairing && displayed_state_ && displayed_state_->receiving)))
      accepted_session_ = session_;
    service_->Decide(session_, stage_, accept, trust_ && trust_->GetChecked(), conflict_id_);
    if (accept_) accept_->SetEnabled(false);
    if (reject_) reject_->SetEnabled(false);
  }
  void CancelOrClose() {
    const auto state = service_->GetSnapshot();
    if (state.session == session_ && state.busy) {
      service_->CancelSession(session_);
      cancel_->SetEnabled(false);
    } else {
      result_close_timer_.Stop();
      // Destroy the button after its event dispatch has returned.
      dismissed_session_ = session_;
      dismiss_ = true;
    }
  }
  void UpdateProgress(const NearbySharing::Snapshot& state) {
    displayed_state_ = state;
    using Stage = NearbySharing::Stage;
    const auto result_duration = state.receiving ? kReceiveResultDuration : kSendResultDuration;
    const bool succeeded = state.stage == Stage::kComplete ||
        (!state.receiving && state.stage == Stage::kSent);
    if (!state.busy && succeeded) {
      if (result_session_ != state.session) {
        result_session_ = state.session;
        result_deadline_ = !state.receiving && !state.discovery_resume_at.is_null() ?
            state.discovery_resume_at : base::TimeTicks::Now() + result_duration;
        result_close_timer_.Start(FROM_HERE, result_deadline_ - base::TimeTicks::Now(),
            base::BindOnce([](base::WeakPtr<ShareSessionToastImpl> self, uint64_t session) {
              if (!self || self->session_ != session) return;
              self->dismissed_session_ = session;
              self->Close();
            }, weak_.GetWeakPtr(), state.session));
      }
    } else {
      result_close_timer_.Stop();
      result_session_ = 0;
      result_deadline_ = base::TimeTicks();
    }
    cancel_->SetDeadline(result_deadline_, result_duration);
    const auto filename = base::UTF8ToUTF16(state.current_file.empty() ?
        (state.files.empty() ? std::string() : state.files.front()) : state.current_file);
    bool resize = filename_->GetText() != filename;
    filename_->SetText(filename);
    filename_->SetTooltipText(filename);
    const auto status = TransferProgressText(state, service_->IsQuickShare());
    resize |= progress_status_->GetText() != status;
    progress_status_->SetText(status);
    const bool counted_files = service_->IsQuickShare() && !state.files.empty();
    const double value = state.total ? std::clamp(static_cast<double>(state.transferred) / state.total, 0.0, 1.0) :
        counted_files ? static_cast<double>(state.completed_files) / state.files.size() : 0.0;
    progress_->SetValue(state.stage == Stage::kComplete ? 1.0 :
        state.busy && (state.stage != Stage::kTransferring || (!state.total && !counted_files)) ? -1.0 : value);
    progress_->SetPaused(!state.busy && state.stage != Stage::kComplete);
    const auto action = Text(state.busy ? IDS_CRFILES_CANCEL : IDS_CRFILES_CLOSE);
    resize |= cancel_->GetText() != action;
    cancel_->SetText(action);
    if (!state.busy) cancel_->SetEnabled(true);
    else if (state.status_message_id == IDS_CRFILES_QS_RESTORING_NETWORK) cancel_->SetEnabled(false);
    if (open_received_) {
      const bool show = state.stage == Stage::kComplete && !state.batch_directory.empty();
      resize |= open_received_->GetVisible() != show;
      open_received_->SetVisible(show);
    }
    if (resize && delegate_ && widget_) delegate_->SizeToContents();
  }
  void Show(const NearbySharing::Snapshot& state, bool transfer = false,
            std::optional<ShareBubbleAnchor> position = std::nullopt) {
    // A Wayland surface must be recreated when changing its parent role.
    const bool same_prompt = session_ == state.session && stage_ == state.stage &&
        transfer_view_ == transfer && conflict_id_ == state.conflict_id;
    const bool decided = same_prompt && decided_;
    const bool trusted = same_prompt && trust_ && trust_->GetChecked();
    const auto ready = same_prompt ? ready_ : base::TimeTicks::Now() + base::Milliseconds(500);
    // Reparenting the same result must not restart its countdown.
    if (session_ != state.session) {
      result_close_timer_.Stop();
      result_session_ = 0;
      result_deadline_ = base::TimeTicks();
    }
    Close(); session_ = state.session; stage_ = state.stage; decided_ = decided;
    conflict_id_ = state.conflict_id;
    displayed_state_ = state;
    transfer_view_ = transfer;
    ready_ = ready;
    const bool code_notice = service_->IsQuickShare() && !state.receiving && !transfer;
    auto contents = std::make_unique<views::View>();
    auto* contents_layout = contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 12));
    if (!code_notice) contents_layout->set_minimum_cross_axis_size(336);
    auto* identity = contents->AddChildView(std::make_unique<views::View>());
    identity->SetVisible(!code_notice);
    auto* identity_layout = identity->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    identity_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    auto* avatar = identity->AddChildView(std::make_unique<views::ImageView>(service_->IsQuickShare() ? QuickShareDeviceAvatar(state.peer_device_type) : PeerAvatar(state)));
    avatar->SetImageSize(service_->IsQuickShare() ? gfx::Size(54, 54) : gfx::Size(64, 64));
    auto* identity_text = identity->AddChildView(std::make_unique<views::View>());
    identity_text->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
    auto* name_row = identity_text->AddChildView(std::make_unique<views::View>());
    auto* name_layout = name_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
    name_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    if (service_->IsQuickShare())
      name_row->AddChildView(std::make_unique<views::ImageView>(QuickShareAvatar(16)));
    auto* name = Label(name_row, base::UTF8ToUTF16(state.peer_name)); name->SetMaximumWidth(248);
    if (!code_notice) Label(contents.get(), Text(state.receiving ? IDS_CRFILES_HISTORY_RECEIVE : IDS_CRFILES_HISTORY_SEND));
    if (transfer) {
      filename_ = Label(contents.get(), std::u16string());
      filename_->SetMultiLine(false); filename_->SetElideBehavior(gfx::ELIDE_MIDDLE);
      filename_->SetMaximumWidthSingleLine(336);
      progress_ = contents->AddChildView(std::make_unique<views::ProgressBar>());
      progress_->SetPreferredSize(gfx::Size(336, 6));
      progress_->GetViewAccessibility().SetName(Text(IDS_CRFILES_TRANSFERRING));
      progress_status_ = Label(contents.get(), std::u16string());
    } else if (stage_ == NearbySharing::Stage::kReceivingConflict) {
      auto* filename = Label(contents.get(), base::UTF8ToUTF16(state.current_file));
      filename->SetTooltipText(base::UTF8ToUTF16(state.current_file));
      Label(contents.get(), Text(IDS_CRFILES_TRANSFER_CONFLICT_PROMPT));
    } else if (code_notice || stage_ == NearbySharing::Stage::kPairing) {
      if (service_->IsQuickShare()) {
        auto* code_row = contents->AddChildView(std::make_unique<views::View>());
        auto* code_layout = code_row->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
        code_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
        auto* prompt = Label(code_row, Text(IDS_CRFILES_QUICK_SHARE_COMPARE_CODE));
        prompt->SetMultiLine(false);
        auto* code = Label(code_row, base::UTF8ToUTF16(state.code));
        code->SetMultiLine(false);
        code->SetFontList(code->font_list().DeriveWithWeight(gfx::Font::Weight::BOLD));
        code->SetElideBehavior(gfx::NO_ELIDE);
      } else {
        auto* code = Label(contents.get(), Text(IDS_CRFILES_COMPARE_CODE) + u"\n" + base::UTF8ToUTF16(state.code));
        code->SetElideBehavior(gfx::NO_ELIDE);
      }
      if (service_->IsQuickShare() && state.receiving) {
        Label(contents.get(), Text(state.text_count ? IDS_CRFILES_QS_TEXT_CONTENT : IDS_CRFILES_FILES) + u": " +
            base::NumberToString16(state.text_count ? state.text_count : state.files.size()));
        Label(contents.get(), Text(IDS_CRFILES_TOTAL_SIZE) + u": " + ui::FormatBytes(base::ByteSize(state.total)));
        if (!state.text_count) Label(contents.get(), Text(IDS_CRFILES_RECEIVE_DIRECTORY) + u": " + state.receive_directory.AsUTF16Unsafe());
        Label(contents.get(), Text(state.text_count ? IDS_CRFILES_QS_TEXT_RECEIVE_PROMPT : IDS_CRFILES_RECEIVE_PROMPT));
      }
      if (!service_->IsQuickShare()) {
        trust_ = contents->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_NEARBY_TRUST_DEVICE)));
        trust_->SetChecked(trusted);
      }
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
    actions->SetVisible(!code_notice);
    auto* layout = actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    if (transfer) {
      cancel_ = actions->AddChildView(std::make_unique<CountdownCloseButton>(
          base::BindRepeating(&ShareSessionToastImpl::CancelOrClose, weak_.GetWeakPtr()), Text(IDS_CRFILES_CANCEL)));
      if (service_->IsQuickShare() && state.receiving) {
        open_received_ = actions->AddChildView(std::make_unique<views::MdTextButton>(
            base::BindRepeating([](base::WeakPtr<ShareSessionToastImpl> view) {
              if (!view || !view->displayed_state_ || view->displayed_state_->stage != NearbySharing::Stage::kComplete ||
                  view->displayed_state_->batch_directory.empty()) return;
              const auto directory = view->displayed_state_->batch_directory;
              const uint64_t session = view->session_;
              view->result_close_timer_.Stop();
              view->dismissed_session_ = session;
              // Hide immediately, but destroy the clicked button only after
              // event dispatch. The dismissed session must not reopen on refresh.
              if (view->widget_) view->widget_->Hide();
              base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                  base::BindOnce([](base::WeakPtr<ShareSessionToastImpl> toast,
                                    uint64_t completed_session, base::FilePath path) {
                    if (!toast) return;
                    if (toast->session_ == completed_session) toast->Close();
                    ShowCrFilesReceivedDirectory(path);
                  }, view, session, directory));
            }, weak_.GetWeakPtr()), Text(IDS_CRFILES_OPEN)));
        open_received_->SetTooltipText(Text(IDS_CRFILES_RECEIVED));
      }
      UpdateProgress(state);
    } else if (!code_notice) {
      const bool conflict = stage_ == NearbySharing::Stage::kReceivingConflict;
      accept_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&ShareSessionToastImpl::Decide, weak_.GetWeakPtr(), true), Text(conflict ? IDS_CRFILES_REPLACE : IDS_CRFILES_ACCEPT)));
      reject_ = actions->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&ShareSessionToastImpl::Decide, weak_.GetWeakPtr(), false), Text(conflict ? IDS_CRFILES_CANCEL : IDS_CRFILES_REJECT)));
      const bool enabled = !decided_ && base::TimeTicks::Now() >= ready_;
      accept_->SetEnabled(enabled); reject_->SetEnabled(enabled);
    }
    const auto anchor = position ? *position : anchor_.Run(state);
    // Wayland cannot position an independent toplevel at the requested screen
    // coordinates. Foreground bubbles need the owner's subsurface coordinates.
    // Background consent UI remains independent of the owner's visibility.
    parented_ = UseParentWindow(anchor);
    parent_widget_ = parented_ ? (anchor.widget ? anchor.widget : owner_) : base::WeakPtr<views::Widget>();
    delegate_ = std::make_unique<ShareSessionBubbleDelegate>(anchor.arrow,
        parented_ ? owner_ : base::WeakPtr<views::Widget>());
    delegate_->set_has_parent(parented_);
    if (parented_) {
      delegate_->set_parent_window(parent_widget_->GetNativeView());
      delegate_->SetAnchorWidget(parent_widget_.get());
      if (parent_widget_.get() != owner_.get()) anchor_observation_.Observe(parent_widget_.get());
    }
    delegate_->SetAnchorRect(anchor.bounds);
    delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    delegate_->SetShowCloseButton(false); delegate_->set_close_on_deactivate(false);
    delegate_->SetCancelCallbackWithClose(base::BindRepeating([] { return false; }));
    delegate_->set_margins(gfx::Insets(12)); delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    delegate_->SetContentsView(std::move(contents));
    widget_ = CreateCrFilesBubble(delegate_.get(), base::BindOnce(
        [](base::WeakPtr<ShareSessionToastImpl> owner, views::Widget::ClosedReason) {
          if (!owner) return;
          owner->accept_ = nullptr; owner->reject_ = nullptr; owner->trust_ = nullptr;
          owner->filename_ = nullptr; owner->progress_status_ = nullptr; owner->progress_ = nullptr; owner->cancel_ = nullptr; owner->open_received_ = nullptr;
          owner->widget_.reset();
        }, weak_.GetWeakPtr()));
    auto* frame = delegate_->GetBubbleFrameView(); frame->SetDisplayVisibleArrow(anchor.show_arrow);
    frame->bubble_border()->set_draw_border_stroke(true); frame->bubble_border()->set_md_shadow_elevation(6);
    Position(anchor);
    delegate_->SizeToContents();
    if (transfer || code_notice) widget_->ShowInactive();
    else widget_->Show();
    Position(anchor);
  }
  void Position(const ShareBubbleAnchor& anchor) {
    if (!widget_ || !delegate_) return;
    const auto desired_parent = UseParentWindow(anchor) ? (anchor.widget ? anchor.widget : owner_) :
        base::WeakPtr<views::Widget>();
    if ((parented_ != UseParentWindow(anchor) || parent_widget_.get() != desired_parent.get()) && displayed_state_) {
      const auto state = *displayed_state_;
      Show(state, transfer_view_, anchor);
      return;
    }
    const bool mode_changed = !background_mode_ || *background_mode_ != anchor.background;
    const bool anchor_changed = delegate_->arrow() != anchor.arrow ||
        delegate_->GetAnchorRect() != anchor.bounds;
    background_mode_ = anchor.background;
    delegate_->GetBubbleFrameView()->SetDisplayVisibleArrow(anchor.show_arrow);
    if (delegate_->arrow() != anchor.arrow) delegate_->SetArrow(anchor.arrow);
    if (delegate_->GetAnchorRect() != anchor.bounds) delegate_->SetAnchorRect(anchor.bounds);
    const auto order = anchor.background ? ui::ZOrderLevel::kFloatingWindow : ui::ZOrderLevel::kNormal;
    if (widget_->GetZOrderLevel() != order) widget_->SetZOrderLevel(order);
#if BUILDFLAG(IS_MAC)
    if (mode_changed || (anchor.background && IsCrFilesApplicationHiddenMac()))
      PrepareCrFilesShareBubbleMac(widget_.get(), anchor.background);
#endif
    if (widget_->IsVisible() && (stacking_dirty_ || mode_changed || anchor_changed)) {
      if (!parented_ && !anchor.background && owner_ && owner_->IsVisible() &&
          !owner_->IsMinimized()) {
        // A parentless bubble does not follow its owner's native stacking.
        // Reapply after activation or anchor changes, rather than every tick.
        widget_->StackAboveWidget(owner_.get());
      } else if (anchor.background) {
        widget_->StackAtTop();
      }
      stacking_dirty_ = false;
    }
  }
  void Refresh() {
    if (!owner_ || owner_->IsClosed()) { result_close_timer_.Stop(); timer_.Stop(); Close(); return; }
    if (dismiss_) { dismiss_ = false; Close(); }
    const auto state = service_->GetSnapshot();
    using Stage = NearbySharing::Stage;
    if (!state.discovery_resume_at.is_null() &&
        base::TimeTicks::Now() >= state.discovery_resume_at) {
      dismissed_session_ = state.session;
      Close();
      return;
    }
    if (service_->IsQuickShare() && !state.receiving &&
        (state.stage == Stage::kCanceled || state.status_message_id == IDS_CRFILES_QUICK_SHARE_REJECTED)) {
      dismissed_session_ = state.session;
      Close();
      return;
    }
    const bool terminal = state.stage == Stage::kComplete || state.stage == Stage::kSent || state.stage == Stage::kCanceled || state.stage == Stage::kError;
    const bool conflict = state.busy && state.stage == Stage::kReceivingConflict;
    const bool transfer = !conflict && state.session && state.session != dismissed_session_ &&
        (state.receiving ? (accepted_session_ == state.session || state.stage == Stage::kTransferring ||
         (terminal && (!state.current_file.empty() || state.stage == Stage::kComplete ||
          (service_->IsQuickShare() && state.stage == Stage::kError)))) :
         ((state.busy && state.stage != Stage::kPairing) || terminal));
    const bool prompt = state.busy &&
        (state.stage == NearbySharing::Stage::kPairing || state.stage == NearbySharing::Stage::kReceivingOffer || conflict);
    if (service_->IsQuickShare() && state.busy && !state.receiving) {
      if (!state.code.empty() && state.stage != Stage::kTransferring) {
        // Local confirmation immediately advances to waiting for the peer.
        // Keep the same informational bubble across that stage transition.
        if (!widget_ || transfer_view_ || session_ != state.session ||
            !displayed_state_ || displayed_state_->code != state.code) Show(state);
        else { stage_ = state.stage; displayed_state_ = state; }
        Position(anchor_.Run(state));
        if (state.stage == Stage::kPairing && auto_confirmed_session_ != state.session) {
          auto_confirmed_session_ = state.session;
          service_->Decide(state.session, state.stage, true, false, state.conflict_id);
        }
      } else Close();
      return;
    }
    if (transfer) {
      if (!widget_ || !transfer_view_ || session_ != state.session) Show(state, true);
      UpdateProgress(state);
    } else if (prompt) {
      if (!widget_ || transfer_view_ || session_ != state.session || stage_ != state.stage ||
          conflict_id_ != state.conflict_id) Show(state);
    } else {
      // Preserve results across discovery snapshot changes. Successful
      // transfers retain their independent auto-close deadline.
      if (!transfer_view_) Close();
      else if (widget_ && state.session != session_) {
        cancel_->SetText(Text(IDS_CRFILES_CLOSE)); cancel_->SetEnabled(true);
        if (displayed_state_) displayed_state_->busy = false;
      }
      else if (widget_ && !state.enabled && state.session == session_) UpdateProgress(state);
      if (widget_) Position(anchor_.Run(state));
      return;
    }
    Position(anchor_.Run(state));
    if (!decided_ && base::TimeTicks::Now() >= ready_) {
      if (accept_) accept_->SetEnabled(true);
      if (reject_) reject_->SetEnabled(true);
    }
  }
  raw_ptr<views::MdTextButton> open_received_ = nullptr;
  base::WeakPtr<views::Widget> owner_;
  base::ScopedObservation<views::Widget, views::WidgetObserver> owner_observation_{this};
  base::ScopedObservation<views::Widget, views::WidgetObserver> anchor_observation_{this};
  base::WeakPtr<views::Widget> parent_widget_;
  std::shared_ptr<ShareService> service_;
  base::RepeatingCallback<ShareBubbleAnchor(const NearbySharing::Snapshot&)> anchor_;
  std::unique_ptr<views::BubbleDialogDelegate> delegate_;
  std::unique_ptr<views::Widget> widget_;
  std::optional<NearbySharing::Snapshot> displayed_state_;
  raw_ptr<views::Checkbox> trust_ = nullptr;
  raw_ptr<views::MdTextButton> accept_ = nullptr, reject_ = nullptr;
  raw_ptr<CountdownCloseButton> cancel_ = nullptr;
  raw_ptr<views::Label> filename_ = nullptr, progress_status_ = nullptr;
  raw_ptr<views::ProgressBar> progress_ = nullptr;
  uint64_t session_ = 0;
  uint64_t conflict_id_ = 0;
  uint64_t accepted_session_ = 0, dismissed_session_ = 0;
  uint64_t auto_confirmed_session_ = 0;
  NearbySharing::Stage stage_ = NearbySharing::Stage::kOff;
  bool decided_ = false;
  bool transfer_view_ = false, dismiss_ = false;
  bool parented_ = false;
  std::optional<bool> background_mode_;
  bool stacking_dirty_ = true;
  base::TimeTicks ready_;
  uint64_t result_session_ = 0;
  base::TimeTicks result_deadline_;
  base::OneShotTimer result_close_timer_;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<ShareSessionToastImpl> weak_{this};
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
    quick_share_icon_ = name_row->AddChildView(std::make_unique<views::ImageView>());
    quick_share_icon_->SetImage(QuickShareAvatar(16));
    quick_share_icon_->SetVisible(false);
    label_ = name_row->AddChildView(std::make_unique<views::Label>()); label_->SetMaximumWidthSingleLine(112);
    trust_icon_ = name_row->AddChildView(std::make_unique<views::ImageView>());
    trust_icon_->SetImage(ui::ImageModel::FromVectorIcon(kAndroidSecurityPrivacySafeFilledIcon,
        gfx::kGoogleGreen600, label_->font_list().GetHeight()));
    trust_icon_->SetVisible(false);
    label_->SetCanProcessEventsWithinSubtree(false); SetIdentity(std::move(avatar), std::move(name));
  }
  void SetDeviceType(const std::u16string& type) {
    quick_share_icon_->SetVisible(true);
    SetAccessibleName(std::u16string(label_->GetText()) + (type.empty() ? std::u16string() : u" · " + type));
  }
  void SetIdentity(ui::ImageModel avatar, std::u16string name) {
    avatar_ = std::move(avatar);
    image_->SetImageSize(avatar_.IsVectorIcon() ? gfx::Size(54, 54) : gfx::Size(64, 64));
    UpdateAvatar();
    label_->SetText(name); SetAccessibleName(name); SetDeviceTooltip(std::move(name));
  }
  void SetDeviceTooltip(std::u16string tooltip) {
    device_tooltip_ = std::move(tooltip);
    SetTooltipText(transfer_ ? transfer_tooltip_ : device_tooltip_);
  }
  void SetTransfer(bool active, bool indeterminate, double progress,
                   base::RepeatingClosure cancel = {}, std::u16string detail = {}) {
    transfer_ = active; indeterminate_ = indeterminate; UpdateAvatar();
    progress_ = std::clamp(progress, 0.0, 1.0); cancel_ = std::move(cancel);
    if (active && indeterminate && !spinner_.IsRunning()) {
      spin_start_ = base::TimeTicks::Now();
      spinner_.Start(FROM_HERE, base::Milliseconds(33),
          base::BindRepeating(&DeviceButton::SchedulePaint, base::Unretained(this)));
    } else if (!active || !indeterminate) spinner_.Stop();
    SetAccessibleName(active ? Text(IDS_CRFILES_CANCEL) : std::u16string(label_->GetText()));
    transfer_tooltip_ = detail.empty() ? Text(IDS_CRFILES_CANCEL) :
        device_tooltip_ + u"\n" + detail + u"\n" + Text(IDS_CRFILES_CANCEL);
    SetTooltipText(active ? transfer_tooltip_ : device_tooltip_);
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
      if (!avatar_.IsVectorIcon()) canvas->DrawCircle(center, image_->width() / 2.0f, flags);
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
    image_->SetBackground(!avatar_.IsVectorIcon() && !transfer_ && (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) ?
        views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 32) : nullptr);
  }
 private:
  void UpdateAvatar() {
    if (transfer_) image_->SetBackground(nullptr);
    auto avatar = avatar_;
    if (avatar.IsVectorIcon() && (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED))
      avatar = ui::ImageModel::FromVectorIcon(*avatar.GetVectorIcon().vector_icon(), ui::kColorSysPrimary, 54);
    image_->SetImage(transfer_ && (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) ?
        ui::ImageModel() : avatar);
  }
  std::u16string device_tooltip_;
  std::u16string transfer_tooltip_;
  ui::ImageModel avatar_;
  raw_ptr<views::ImageView> image_ = nullptr, trust_icon_ = nullptr, quick_share_icon_ = nullptr;
  raw_ptr<views::Label> label_ = nullptr;
  bool transfer_ = false, indeterminate_ = false;
  double progress_ = 0;
  base::RepeatingClosure cancel_;
  base::TimeTicks spin_start_;
  base::RepeatingTimer spinner_;
};
BEGIN_METADATA(DeviceButton)
END_METADATA
class SharingFoldersView : public views::View {
  METADATA_HEADER(SharingFoldersView, views::View)
 public:
  SharingFoldersView(
      base::RepeatingCallback<std::vector<NearbySharing::SharedTarget>()> folders,
      base::RepeatingCallback<void(NearbySharing::SharedTarget)> open,
      base::RepeatingCallback<void(NearbySharing::SharedTarget)> remove)
      : folders_(std::move(folders)), open_(std::move(open)), remove_(std::move(remove)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 12));
    empty_ = AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_NO_SHARING_FOLDERS)));
    list_ = AddChildView(std::make_unique<views::View>());
    list_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(), 8));
    Refresh();
    timer_.Start(FROM_HERE, base::Milliseconds(250), this, &SharingFoldersView::Refresh);
  }
 private:
  void Refresh() {
    auto folders = folders_.Run();
    empty_->SetVisible(folders.empty());
    if (folders == displayed_) return;
    displayed_ = std::move(folders);
    // Refresh on the timer, after any row button's event dispatch has returned.
    list_->RemoveAllChildViews();
    for (const auto& folder : displayed_) {
      auto* row = list_->AddChildView(std::make_unique<views::View>());
      auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(8), 12));
      layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
      row->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8));
      auto* button = row->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating([](base::RepeatingCallback<void(NearbySharing::SharedTarget)> open,
                                 NearbySharing::SharedTarget target) {
            // Opening a tab closes this dialog; let the click finish first.
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE, base::BindOnce(open, std::move(target)));
          }, open_, folder), std::u16string()));
      button->SetImageModel(views::Button::STATE_NORMAL,
          ui::ImageModel::FromVectorIcon(vector_icons::kFolderOpenIcon, ui::kColorSysOnSurface, 24));
      button->SetImageCentered(true);
      button->SetMinSize(gfx::Size(40, 40));
      button->SetPreferredSize(gfx::Size(40, 40));
      button->SetTooltipText(Text(IDS_CRFILES_OPEN_NEW_TAB) + u"\n" + folder.directory.AsUTF16Unsafe());
      button->SetAccessibleName(Text(IDS_CRFILES_OPEN_NEW_TAB) + u": " + folder.directory.AsUTF16Unsafe());
      auto* labels = row->AddChildView(std::make_unique<views::View>());
      labels->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
      layout->SetFlexForView(labels, 1);
      auto* name = labels->AddChildView(std::make_unique<views::Label>(base::UTF8ToUTF16(folder.name)));
      name->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      name->SetMaximumWidthSingleLine(400);
      name->SetTooltipText(base::UTF8ToUTF16(folder.name));
      auto* path = labels->AddChildView(std::make_unique<views::Label>(folder.directory.AsUTF16Unsafe()));
      path->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      path->SetElideBehavior(gfx::ELIDE_MIDDLE);
      path->SetMaximumWidthSingleLine(400);
      path->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
      path->SetTooltipText(folder.directory.AsUTF16Unsafe());
      auto remove = views::CreateVectorImageButtonWithNativeTheme(
          base::BindRepeating(remove_, folder),
          kDeleteIcon, 20, ui::kColorSysError, ui::kColorIconDisabled,
          ui::kColorSysError);
      remove->SetPreferredSize(gfx::Size(36, 36));
      remove->SetTooltipText(Text(IDS_CRFILES_REMOVE_SHARE));
      remove->GetViewAccessibility().SetName(
          Text(IDS_CRFILES_REMOVE_SHARE) + u": " + folder.directory.AsUTF16Unsafe());
      row->AddChildView(std::move(remove));
    }
    InvalidateLayout();
  }
  base::RepeatingCallback<std::vector<NearbySharing::SharedTarget>()> folders_;
  base::RepeatingCallback<void(NearbySharing::SharedTarget)> open_, remove_;
  std::vector<NearbySharing::SharedTarget> displayed_;
  raw_ptr<views::View> list_ = nullptr;
  raw_ptr<views::Label> empty_ = nullptr;
  base::RepeatingTimer timer_;
};
BEGIN_METADATA(SharingFoldersView)
END_METADATA

class TrustedDevicesView : public views::View {
  METADATA_HEADER(TrustedDevicesView, views::View)

 public:
  explicit TrustedDevicesView(std::shared_ptr<ShareService> service)
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
                [](std::shared_ptr<ShareService> service, std::string fingerprint) {
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
  std::shared_ptr<ShareService> service_;
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
  result.bytes = FileSelectionSize(paths);
  return result;
}
class FileShareView : public views::View, public ui::EventObserver {
  METADATA_HEADER(FileShareView, views::View)
 public:
  using views::View::OnEvent;
  FileShareView(std::shared_ptr<ShareService> service, std::vector<base::FilePath> paths)
      : service_(std::move(service)), paths_(std::move(paths)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical,
        gfx::Insets::TLBR(12, 16, 16, 16), 10));
    auto* summary = AddChildView(std::make_unique<views::View>());
    summary_container_ = summary;
    summary->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
    summary->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 12));
    count_ = Label(summary, base::NumberToString16(paths_.size()) + u" " + Text(IDS_CRFILES_ITEMS));
    size_ = Label(summary, Text(IDS_CRFILES_TOTAL_SIZE) + u": —");
    status_row_ = AddChildView(std::make_unique<views::View>());
    auto* status_layout = status_row_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    status_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    status_ = Label(status_row_, std::u16string());
    status_detail_ = Label(status_row_, std::u16string());
    status_row_->SetVisible(false);
    status_->SetVisible(false);
    discovery_status_ = Label(this, std::u16string());
    discovery_status_->SetMultiLine(true); discovery_status_->SetVisible(false);
    enable_ = AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Enable, weak_.GetWeakPtr()), Text(IDS_CRFILES_ENABLE)));
    auto* devices = AddChildView(std::make_unique<views::View>());
    auto* layout = devices->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kStart);
    manual_ = devices->AddChildView(std::make_unique<DeviceButton>(base::BindRepeating(&FileShareView::Manual, weak_.GetWeakPtr()),
        ui::ImageModel::FromVectorIcon(vector_icons::kAdd2Icon, ui::kColorSysPrimary, 48), Text(IDS_CRFILES_MANUAL_CONNECTION)));
    auto* separator = devices->AddChildView(std::make_unique<views::Separator>());
    separator->SetOrientation(views::Separator::Orientation::kVertical); separator->SetPreferredLength(96);
    manual_->SetVisible(!service_->IsQuickShare());
    separator->SetVisible(!service_->IsQuickShare());
    service_->SetDiscoveryActive(true);
    peers_ = devices->AddChildView(std::make_unique<views::View>());
    peers_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
    layout->SetFlexForView(peers_, 1);
    session_device_ = devices->AddChildView(std::make_unique<DeviceButton>(base::RepeatingClosure(), service_->IsQuickShare() ? QuickShareDeviceAvatar(0) : Avatar(0), Text(IDS_CRFILES_TRANSFERRING)));
    session_device_->SetVisible(false);
    if (service_->GetSnapshot().busy) {
      if (!paths_.empty()) base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&Summarize, paths_), base::BindOnce(&FileShareView::SummaryReady, weak_.GetWeakPtr(), ++summary_generation_));
    } else SetSelection(paths_);
    Refresh(); timer_.Start(FROM_HERE, base::Milliseconds(250), this, &FileShareView::Refresh);
  }
  // A transfer belongs to the service, so dismissing its UI must not cancel it.
  // The explicit Cancel action and application shutdown still cancel sessions.
  ~FileShareView() override { timer_.Stop(); CloseToast(); service_->SetDiscoveryActive(false); }
  void SetSelection(std::vector<base::FilePath> paths) {
    if (service_->GetSnapshot().busy) return;
    CloseToast(); active_device_ = nullptr; selected_.reset(); paths_ = std::move(paths);
    selection_ready_ = false; send_error_ = false;
    const auto state = service_->GetSnapshot(); shown_session_ = state.session; shown_stage_ = state.stage;
    selection_session_ = state.session;
    count_->SetText(base::NumberToString16(paths_.size()) + u" " + Text(IDS_CRFILES_ITEMS));
    size_->SetText(Text(IDS_CRFILES_TOTAL_SIZE) + u": —");
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&Summarize, paths_), base::BindOnce(&FileShareView::SummaryReady, weak_.GetWeakPtr(), ++summary_generation_));
  }
  std::optional<gfx::Rect> SessionBounds() {
    if (!GetWidget() || !GetWidget()->IsVisible()) return std::nullopt;
    const auto state = service_->GetSnapshot();
    if (anchor_session_ != state.session) {
      anchor_session_ = state.session;
      session_anchor_bounds_.reset();
    }
    auto* device = session_device_->GetVisible() ? session_device_.get() : active_device_.get();
    // Place session bubbles below the entire device tile so the name and type
    // remain visible beneath the avatar while the verification code is shown.
    if (device && device->IsDrawn()) {
      auto bounds = device->GetBoundsInScreen();
      auto origin = bounds.origin();
      views::View::ConvertPointFromScreen(this, &origin);
      session_anchor_bounds_ = gfx::Rect(origin, bounds.size());
      return bounds;
    }
    // Discovery can temporarily withdraw or replace the completed session's
    // device. Keep its last anchor in this view instead of reparenting the
    // result bubble to the main window. Local coordinates also follow moves.
    if (service_->IsQuickShare() && session_anchor_bounds_ &&
        state.session != selection_session_) {
      auto bounds = *session_anchor_bounds_;
      auto origin = bounds.origin();
      views::View::ConvertPointToScreen(this, &origin);
      bounds.set_origin(origin);
      return bounds;
    }
    return std::nullopt;
  }
 private:
  views::Label* Label(views::View* owner, std::u16string text) {
    auto* label = owner->AddChildView(std::make_unique<views::Label>(text));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetElideBehavior(gfx::ELIDE_MIDDLE); return label;
  }
  void SummaryReady(uint64_t generation, Summary summary) {
    if (generation != summary_generation_) return;
    selection_ready_ = true;
    const auto state = service_->GetSnapshot(); if (state.receiving && state.busy) return;
    count_->SetText(Text(IDS_CRFILES_FILES) + u": " + base::NumberToString16(summary.files) + u"   ·   " +
        Text(IDS_CRFILES_FOLDERS) + u": " + base::NumberToString16(summary.folders));
    size_->SetText(Text(IDS_CRFILES_TOTAL_SIZE) + u": " + (summary.bytes ? ui::FormatBytes(base::ByteSize(*summary.bytes)) : std::u16string(u"—")));
  }
  void Enable() {
    auto prefs = GetNearbySharingPreferences(); prefs.discovery_enabled = true;
    if (SetNearbySharingPreferences(prefs)) ApplyCrFilesSharingSettings();
    else { status_->SetText(Text(IDS_CRFILES_CONFIG_ERROR)); status_->SetVisible(true); status_row_->SetVisible(true); }
  }
  void CloseToast() {
    compose_monitor_.reset();
    address_ = nullptr; targets_ = nullptr;
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
    // Native activation and combobox menus can deactivate a composing bubble
    // without an outside click. Dismiss it using pointer events instead.
    toast_delegate_->set_close_on_deactivate(false);
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
    if (!prompt) {
      compose_monitor_ = views::EventMonitor::CreateApplicationMonitor(
          this, GetWidget()->GetNativeWindow(), {ui::EventType::kMousePressed});
    }
  }
  void OnEvent(const ui::Event& event) override {
    if (event.type() != ui::EventType::kMousePressed || !compose_monitor_ ||
        !toast_widget_ || toast_widget_->IsClosed() ||
        !toast_widget_->IsVisible() || (targets_ && targets_->IsMenuRunning())) return;
    const auto point = compose_monitor_->GetLastMouseLocation();
    if (IsInsideCrFilesBubble(point) ||
        (active_device_ && active_device_->GetBoundsInScreen().Contains(point))) return;
    // Native close callbacks defer client destruction until this event unwinds.
    toast_widget_->Close();
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
    if (latest == state.peers.end() || (latest->ble_only && !latest->bluetooth_connectable)) return;
    selected_ = *latest; Compose(anchor);
  }
  void Compose(DeviceButton* anchor) {
    const auto request = ++compose_request_;
    // Showing an activating bubble inside a mouse callback can race the
    // enclosing window's activation and immediately close it as lost-focus.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
            [](base::WeakPtr<FileShareView> owner,
               std::unique_ptr<views::ViewTracker> anchor, uint64_t request) {
              if (!owner || !owner->GetWidget() || owner->GetWidget()->IsClosed() ||
                  request != owner->compose_request_ || !anchor->view() ||
                  owner->service_->GetSnapshot().busy) return;
              owner->ShowCompose(static_cast<DeviceButton*>(anchor->view()));
            }, weak_.GetWeakPtr(), std::make_unique<views::ViewTracker>(anchor), request));
  }
  void ShowCompose(DeviceButton* anchor) {
    if (service_->IsQuickShare() && !SelectedPeerAvailable(service_->GetSnapshot())) return;
    CloseToast(); composing_ = true; active_device_ = anchor;
    auto contents = std::make_unique<views::View>();
    auto* layout = contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), service_->IsQuickShare() ? 8 : 12));
    layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    if (!selected_) {
      address_ = contents->AddChildView(std::make_unique<views::Textfield>());
      address_->SetPreferredSize(gfx::Size(240, 36));
      address_->SetAccessibleName(Text(IDS_CRFILES_DEVICE_ADDRESS)); address_->SetPlaceholderText(u"192.168.1.2:59487"); address_->RemoveHoverEffect();
    } else if (!service_->IsQuickShare()) {
      std::vector<ui::SimpleComboboxModel::Item> items; items.emplace_back(Text(IDS_CRFILES_RECEIVED));
      for (const auto& target : selected_->targets) items.emplace_back(base::UTF8ToUTF16(target.name));
      targets_ = contents->AddChildView(std::make_unique<views::Combobox>(std::make_unique<ui::SimpleComboboxModel>(std::move(items))));
      targets_->SetPreferredSize(gfx::Size(240, 36));
      targets_->SetAccessibleName(Text(IDS_CRFILES_HISTORY_DESTINATION)); targets_->SetSelectedIndex(0);
    } else {
      Label(contents.get(), Text(IDS_CRFILES_QUICK_SHARE_TO_DEVICE));
    }
    contents->AddChildView(std::make_unique<views::MdTextButton>(base::BindRepeating(&FileShareView::Send, weak_.GetWeakPtr()), Text(IDS_CRFILES_HISTORY_SEND)));
    PresentToast(anchor->IconAnchor(), std::move(contents), false);
  }
  bool SelectedPeerAvailable(const ShareService::Snapshot& state) const {
    return selected_ && std::ranges::any_of(state.peers, [&](const auto& peer) {
      return peer.id == selected_->id && peer.address == selected_->address &&
          peer.phone.has_value() == selected_->phone.has_value() &&
          !peer.name.empty() && (!peer.ble_only || peer.bluetooth_connectable);
    });
  }
  void Send() {
    const auto state = service_->GetSnapshot(); if (state.busy || !composing_ ||
        (service_->IsQuickShare() && !selection_ready_)) return;
    if (service_->IsQuickShare() && !SelectedPeerAvailable(state)) {
      // Defer bubble destruction until the Send button's event has unwound.
      dismiss_compose_ = true;
      selected_.reset(); active_device_ = nullptr; send_error_ = false;
      ++compose_request_;
      return;
    }
    std::string target;
    const size_t index = targets_ ? targets_->GetSelectedIndex().value_or(0) : 0;
    if (selected_ && index > 0 && index <= selected_->targets.size()) target = selected_->targets[index - 1].id;
    if (selected_ && selected_->ble_only && !selected_->bluetooth_connectable) return;
    const bool sent = service_->Send(selected_ ? selected_->address : base::UTF16ToUTF8(address_->GetText()), paths_, {},
        selected_ ? selected_->phone : std::nullopt, target);
    send_error_ = !sent;
    if (sent) dismiss_compose_ = true;
    // The timer replaces the toast after this button's event dispatch returns.
  }
  void Cancel(uint64_t session) { service_->CancelSession(session); }
  void Refresh() {
    auto state = service_->GetSnapshot();
    if (service_->IsQuickShare()) {
      // Discovery ranks routes by freshness, not presentation order. Retain
      // existing endpoint positions and append new endpoints in discovery order.
      const auto rank = [&](const auto& peer) {
        return std::ranges::find(known_peers_, peer.id, &NearbySharing::Peer::id) - known_peers_.begin();
      };
      std::ranges::stable_sort(state.peers, [&](const auto& left, const auto& right) {
        return rank(left) < rank(right);
      });
    }
    if (dismiss_compose_) { dismiss_compose_ = false; CloseToast(); }
    if (service_->IsQuickShare() && !state.busy && composing_ &&
        !SelectedPeerAvailable(state)) {
      CloseToast();
      selected_.reset(); active_device_ = nullptr; send_error_ = false;
      ++compose_request_;
    }
    using Stage = NearbySharing::Stage;
    // Keep the avatar anchor stationary while Quick Share auto-confirms.
    summary_container_->SetVisible(service_->IsQuickShare() || state.stage != Stage::kPairing);
    if (state.receiving && (state.busy || paths_.empty())) {
      count_->SetText(Text(state.text_count ? IDS_CRFILES_QS_TEXT_CONTENT : IDS_CRFILES_FILES) + u": " +
          (state.text_count ? base::NumberToString16(state.text_count) : state.files.empty() ? std::u16string(u"—") : base::NumberToString16(state.files.size())));
      size_->SetText(Text(IDS_CRFILES_TOTAL_SIZE) + u": " + (state.files.empty() && !state.text_count ? std::u16string(u"—") : ui::FormatBytes(base::ByteSize(state.total))));
    }
    const bool transport_ready = service_->IsQuickShare() ? state.discovery_ready : state.port != 0;
    int message = !state.enabled ? IDS_CRFILES_NEARBY_OFF :
        !transport_ready ? IDS_CRFILES_LOADING :
        !state.discovery_ready ? IDS_CRFILES_NEARBY_DISCOVERY_UNAVAILABLE :
        IDS_CRFILES_NEARBY_READY;
    if (state.status_message_id) message = state.status_message_id;
    else if (state.stage == Stage::kComplete) message = IDS_CRFILES_COMPLETE;
    else if (state.stage == Stage::kError) message = IDS_CRFILES_TRANSFER_ERROR;
    else if (state.stage == Stage::kCanceled) message = IDS_CRFILES_CANCELED;
    else if (state.busy) message = IDS_CRFILES_TRANSFERRING;
    status_->SetText(Text(send_error_ && !state.busy ? IDS_CRFILES_SEND_ERROR : message));
    status_->SetVisible(message != IDS_CRFILES_NEARBY_READY || (send_error_ && !state.busy));
    status_row_->SetVisible(status_->GetVisible());
    std::u16string status_detail;
    if (service_->IsQuickShare() && state.busy && state.status_message_id != IDS_CRFILES_QS_RESTORING_NETWORK) {
      status_detail = !state.receiving && state.stage != Stage::kTransferring ?
          Text(IDS_CRFILES_QUICK_SHARE_WAIT_FOR_RECEIVER) :
          ui::FormatBytes(base::ByteSize(state.transferred)) + u" / " + ui::FormatBytes(base::ByteSize(state.total));
    }
    if (state.warning_message_id) status_detail += (status_detail.empty() ? u"" : u"\n") + Text(state.warning_message_id);
    status_detail_->SetText(status_detail);
    status_detail_->SetTooltipText(status_detail);
    status_detail_->SetVisible(!status_detail.empty());
    const bool show_discovery_status = state.discovery_message_id != 0 &&
        (!service_->IsQuickShare() || state.discovery_message_id != IDS_CRFILES_QUICK_SHARE_BLE_HINT);
    discovery_status_->SetVisible(!service_->IsQuickShare() && show_discovery_status);
    if (service_->IsQuickShare()) {
      if (auto* widget = GetWidget()) {
        const auto title = Text(show_discovery_status ? state.discovery_message_id : IDS_CRFILES_QUICK_SHARE);
        auto* delegate = widget->widget_delegate();
        if (delegate->GetWindowTitle() != title) delegate->SetTitle(title);
      }
    } else if (state.discovery_message_id) {
      discovery_status_->SetText(Text(state.discovery_message_id));
    }
    enable_->SetVisible(!state.enabled && !service_->IsQuickShare());
    const bool discovery_paused = base::TimeTicks::Now() < state.discovery_resume_at;
    const bool can_send = state.enabled && transport_ready && !state.busy && !discovery_paused && !paths_.empty() &&
        (!service_->IsQuickShare() || selection_ready_);
    manual_->SetEnabled(can_send || (state.busy && active_device_ == manual_));
    const bool same_devices = std::ranges::equal(known_peers_, state.peers, [&](const auto& left, const auto& right) {
      if (service_->IsQuickShare())
        return left.id == right.id &&
            (left.ble_only && !left.bluetooth_connectable) == (right.ble_only && !right.bluetooth_connectable);
      return left.id == right.id && left.address == right.address && left.name == right.name &&
          left.avatar == right.avatar && left.device_type == right.device_type && left.bluetooth_address == right.bluetooth_address && left.ble_only == right.ble_only && left.bluetooth_connectable == right.bluetooth_connectable && left.phone.has_value() == right.phone.has_value();
    });
    if (!state.busy && !discovery_paused && !toast_widget_ && !same_devices) {
      active_device_ = nullptr;
      known_peers_ = state.peers; peers_->RemoveAllChildViews();
      views::View* row = nullptr; size_t index = 0;
      for (const auto& peer : known_peers_) {
        if (index++ % 3 == 0) {
          row = peers_->AddChildView(std::make_unique<views::View>());
          row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
        }
        auto* button = row->AddChildView(std::make_unique<DeviceButton>(base::RepeatingClosure(), service_->IsQuickShare() ? QuickShareDeviceAvatar(peer.device_type) : Avatar(peer.avatar), base::UTF8ToUTF16(peer.name)));
        if (service_->IsQuickShare()) button->SetDeviceType(QuickShareDeviceType(peer.device_type));
        button->SetTrusted(peer.trusted);
        if (peer.ble_only && !peer.bluetooth_connectable) {
          auto* hint = button->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_QUICK_SHARE_BLE_ONLY)));
          hint->SetMaximumWidthSingleLine(112);

          button->GetViewAccessibility().SetName(base::UTF8ToUTF16(peer.name) + u" · " +
              QuickShareDeviceType(peer.device_type) + u" · " + Text(IDS_CRFILES_QUICK_SHARE_BLE_HINT));
        }
        auto tooltip = base::UTF8ToUTF16(peer.name);
        if (service_->IsQuickShare()) tooltip += u"\n" + QuickShareDeviceType(peer.device_type);
        if (!peer.bluetooth_address.empty()) tooltip += u"\n" + base::UTF8ToUTF16(peer.bluetooth_address);
        if (peer.ble_only && !peer.bluetooth_connectable) tooltip += u"\n" + Text(IDS_CRFILES_QUICK_SHARE_BLE_HINT);
        button->SetDeviceTooltip(std::move(tooltip));
        button->SetCallback(base::BindRepeating(&FileShareView::SelectPeer, weak_.GetWeakPtr(), base::Unretained(button), peer));
        // Discovery is withdrawn during a transfer and returns with refreshed
        // metadata. Rebind the result anchor to the live, clickable avatar.
        if (service_->IsQuickShare() && selected_ && peer.id == selected_->id &&
            peer.address == selected_->address && (!peer.ble_only || peer.bluetooth_connectable)) active_device_ = button;
      }
    }
    // Refresh trust badges even while a toast pins the discovery buttons.
    size_t peer_index = 0;
    for (const auto& row : peers_->children()) for (const auto& child : row->children()) {
      if (peer_index >= known_peers_.size()) break;
      const auto& displayed = known_peers_[peer_index++];
      const auto latest = std::ranges::find_if(state.peers, [&](const auto& peer) {
        return peer.id == displayed.id && (service_->IsQuickShare() ||
            (peer.address == displayed.address && peer.phone.has_value() == displayed.phone.has_value()));
      });
      auto* button = static_cast<DeviceButton*>(child.get());
      if (service_->IsQuickShare() && !state.busy && !discovery_paused && !toast_widget_ &&
          latest != state.peers.end() &&
          (displayed.name != latest->name || displayed.device_type != latest->device_type ||
           displayed.address != latest->address || displayed.bluetooth_address != latest->bluetooth_address ||
           displayed.ble_only != latest->ble_only || displayed.bluetooth_connectable != latest->bluetooth_connectable)) {
        // Update the existing card and its send route without replacing its view.
        if (displayed.name != latest->name || displayed.device_type != latest->device_type)
          button->SetIdentity(QuickShareDeviceAvatar(latest->device_type), base::UTF8ToUTF16(latest->name));
        if (displayed.name != latest->name || displayed.device_type != latest->device_type)
          button->SetDeviceType(QuickShareDeviceType(latest->device_type));
        auto tooltip = base::UTF8ToUTF16(latest->name) + u"\n" + QuickShareDeviceType(latest->device_type);
        if (!latest->bluetooth_address.empty()) tooltip += u"\n" + base::UTF8ToUTF16(latest->bluetooth_address);
        if (latest->ble_only && !latest->bluetooth_connectable) tooltip += u"\n" + Text(IDS_CRFILES_QUICK_SHARE_BLE_HINT);
        button->SetDeviceTooltip(std::move(tooltip));
        button->SetCallback(base::BindRepeating(&FileShareView::SelectPeer, weak_.GetWeakPtr(), base::Unretained(button), *latest));
      }
      // A composing bubble pins the list, but must not keep lost peers visible.
      // Active transfers own their avatar independently of discovery broadcasts.
      if (service_->IsQuickShare() && !discovery_paused) button->SetVisible(state.busy || latest != state.peers.end());
      button->SetEnabled((can_send && latest != state.peers.end() && (!latest->ble_only || latest->bluetooth_connectable)) || (state.busy && button == active_device_));
      button->SetTrusted(state.busy && button == active_device_ && !state.code.empty() ? state.peer_trusted :
          latest != state.peers.end() && latest->trusted);
    }

    if (service_->IsQuickShare() && !state.busy && !discovery_paused && !toast_widget_)
      known_peers_ = state.peers;

    const bool session = state.busy || (!composing_ &&
        (!service_->IsQuickShare() || state.session != selection_session_) &&
        (state.stage == Stage::kComplete || state.stage == Stage::kSent || state.stage == Stage::kError || state.stage == Stage::kCanceled));
    // A fallback session avatar replaces the discovery list; otherwise the
    // connected device can appear once in each of the two views.
    const bool fallback = discovery_paused ? session_device_->GetVisible() : session && (state.receiving || !active_device_) &&
        (service_->IsQuickShare() ? (state.busy || known_peers_.empty()) :
         (state.busy || toast_widget_ || shown_session_ != state.session || shown_stage_ != state.stage));
    session_device_->SetVisible(fallback);
    session_device_->SetEnabled(state.busy);
    peers_->SetVisible(!fallback);
    session_device_->SetTrusted(state.peer_trusted);
    if (session) {
      session_device_->SetIdentity(service_->IsQuickShare() ? QuickShareDeviceAvatar(state.peer_device_type) : PeerAvatar(state), base::UTF8ToUTF16(state.peer_name));
      if (service_->IsQuickShare()) session_device_->SetDeviceType(QuickShareDeviceType(state.peer_device_type));
    }
    // The direction is retained with completed/canceled/failed sessions.
    // An old receive result must not dismiss a newly composed outgoing offer.
    if (state.receiving && state.busy && toast_widget_) CloseToast();
    shown_session_ = state.session; shown_stage_ = state.stage;
    auto* transfer_target = fallback ? session_device_.get() : active_device_.get();
    if (active_device_ && active_device_ != transfer_target) active_device_->SetTransfer(false, false, 0);
    if (session_device_ != transfer_target) session_device_->SetTransfer(false, false, 0);
    std::u16string transfer_detail;
    if (service_->IsQuickShare() && state.busy && !state.receiving) {
      const auto filename = base::UTF8ToUTF16(state.current_file.empty() ?
          (state.files.empty() ? std::string() : state.files.front()) : state.current_file);
      transfer_detail = filename.empty() ? std::u16string() : filename + u"\n";
      transfer_detail += TransferProgressText(state, true);
    }
    if (transfer_target) transfer_target->SetTransfer(state.busy,
        state.stage != Stage::kTransferring || state.total == 0,
        state.total ? static_cast<double>(state.transferred) / state.total : 0,
        base::BindRepeating(&FileShareView::Cancel, weak_.GetWeakPtr(), state.session), std::move(transfer_detail));
  }

  std::shared_ptr<ShareService> service_;
  std::vector<base::FilePath> paths_;
  std::vector<NearbySharing::Peer> known_peers_;
  uint64_t anchor_session_ = 0;
  std::optional<gfx::Rect> session_anchor_bounds_;
  std::optional<NearbySharing::Peer> selected_;
  bool selection_ready_ = false;
  bool send_error_ = false;
  bool dismiss_compose_ = false;
  raw_ptr<views::View> peers_ = nullptr, summary_container_ = nullptr;
  raw_ptr<views::Label> count_ = nullptr, size_ = nullptr, status_ = nullptr, discovery_status_ = nullptr;
  raw_ptr<views::View> status_row_ = nullptr;
  raw_ptr<views::Label> status_detail_ = nullptr;
  raw_ptr<views::Textfield> address_ = nullptr;
  raw_ptr<views::Combobox> targets_ = nullptr;
  raw_ptr<views::MdTextButton> enable_ = nullptr;
  raw_ptr<DeviceButton> manual_ = nullptr, session_device_ = nullptr, active_device_ = nullptr;
  std::unique_ptr<views::BubbleDialogDelegate> toast_delegate_;
  std::unique_ptr<views::Widget> toast_widget_;
  std::unique_ptr<views::EventMonitor> compose_monitor_;
  uint64_t summary_generation_ = 0;
  uint64_t shown_session_ = 0;
  uint64_t selection_session_ = 0;
  uint64_t compose_request_ = 0;
  NearbySharing::Stage shown_stage_ = NearbySharing::Stage::kOff;
  bool toast_prompt_ = false, composing_ = false;
  base::RepeatingTimer timer_;
  base::WeakPtrFactory<FileShareView> weak_{this};
};
BEGIN_METADATA(FileShareView)
END_METADATA
}
std::unique_ptr<ShareSessionToast> CreateShareSessionToast(views::Widget* owner,
    std::shared_ptr<ShareService> service,
    base::RepeatingCallback<ShareBubbleAnchor(const NearbySharing::Snapshot&)> anchor) {
  return std::make_unique<ShareSessionToastImpl>(owner, std::move(service), std::move(anchor));
}
std::optional<gfx::Rect> FileShareSessionBounds(views::View* view) {
  return view ? static_cast<FileShareView*>(view)->SessionBounds() : std::nullopt;
}
views::View* ShowTrustedDevicesDialog(views::Widget* parent,
                                      std::shared_ptr<ShareService> service) {
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

views::View* ShowSharingFoldersDialog(views::Widget* parent,
    base::RepeatingCallback<std::vector<NearbySharing::SharedTarget>()> folders,
    base::RepeatingCallback<void(NearbySharing::SharedTarget)> open,
    base::RepeatingCallback<void(NearbySharing::SharedTarget)> remove) {
  if (!parent) return nullptr;
  auto contents = std::make_unique<SharingFoldersView>(
      std::move(folders), std::move(open), std::move(remove));
  auto* result = contents.get();
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  scroll->SetUseContentsPreferredSize(true);
  scroll->ClipHeightTo(288, 480);
  scroll->SetContents(std::move(contents));
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->set_fixed_width(680);
  dialog->SetTitle(Text(IDS_CRFILES_SHARING_FOLDERS));
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
views::View* ShowFileShareDialog(views::Widget* parent, std::shared_ptr<ShareService> service, std::vector<base::FilePath> paths) {
  if (!parent || !service) return nullptr;
  auto scroll = std::make_unique<views::ScrollView>();
  scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
  scroll->SetUseContentsPreferredSize(true); scroll->ClipHeightTo(300, 600);
  const bool quick_share = service->IsQuickShare();
  auto* contents = scroll->SetContents(std::make_unique<FileShareView>(std::move(service), std::move(paths)));
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->set_fixed_width(580); dialog->SetTitle(Text(quick_share ? IDS_CRFILES_QUICK_SHARE : IDS_CRFILES_SHARE_TO));
  dialog->SetModalType(ui::mojom::ModalType::kWindow);
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone)); dialog->SetShowCloseButton(true);
  dialog->SetContentsView(std::move(scroll));
  ShowCrFilesDialog(std::move(dialog), parent);
  return contents;
}
}
