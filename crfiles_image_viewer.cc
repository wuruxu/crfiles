// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crFiles/crfiles_image_viewer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "crFiles/crfiles_context_actions.h"
#include "crFiles/crfiles_decode_memory.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/crfiles_image_preview.h"
#include "crFiles/grit/crfiles_strings.h"
#include "libcr/svg/svg_decoder.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/compositor.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/vector2d_f.h"
#include "ui/views/background.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/table_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/dialog_delegate.h"

namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }

constexpr int kPreviewEdge = 2048;
struct PreviewFrame {
  gfx::ImageSkia image;
  bool has_more = false;
};

// A viewer owns one worker-side decoder for the selected file.
class ViewerDecoder {
 public:
  explicit ViewerDecoder(std::shared_ptr<std::atomic_bool> canceled)
      : canceled_(std::move(canceled)) {}

  PreviewFrame Open(const base::FilePath& path) {
    if (canceled_->load()) return {};
    FileOperationLog log("open_file", "preview", path.AsUTF8Unsafe(), {});
    if (!log.valid()) return {};
    auto access = ResolveFileAccessBookmark(CreateFileAccessBookmark(path));
    base::File::Info before, after;
    if (!base::GetFileInfo(path, &before) || before.is_directory) return {};
    auto image = LoadImagePreviewWithAnimation(path, kPreviewEdge, canceled_, &svg_);
    if (canceled_->load()) { log.Cancel(); return {}; }
    if (image.isNull() || !base::GetFileInfo(path, &after) ||
        before.size != after.size || before.last_modified != after.last_modified) return {};
    log.Succeed();
    return {std::move(image), svg_ != nullptr};
  }

  PreviewFrame FrameAt(base::TimeDelta elapsed, int edge) {
    if (canceled_->load() || !svg_) return {};
    const bool finished = svg_->FinishedAt(elapsed);
    if (finished) edge = kPreviewEdge;
    const size_t pixels = static_cast<size_t>(edge) * edge;
    DecodeMemory memory((pixels + libcr::kSvgEmbeddedPixelLimit) * 4, canceled_);
    if (!memory.acquired() || canceled_->load()) return {};
    auto bitmap = svg_->Render(elapsed, edge);
    if (bitmap.drawsNothing() || canceled_->load()) return {};
    auto image = gfx::ImageSkia::CreateFromBitmap(bitmap, 1.f);
    image.MakeThreadSafe();
    return {std::move(image), !finished};
  }

 private:
  std::shared_ptr<std::atomic_bool> canceled_;
  std::unique_ptr<libcr::SvgDecoder> svg_;
};

class ImageCanvas : public views::View {
  METADATA_HEADER(ImageCanvas, views::View)
 public:
  ImageCanvas() {
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetAccessibleName(Text(IDS_CRFILES_IMAGE_VIEWER));
    SetBackground(views::CreateSolidBackground(ui::kColorSysSurfaceVariant));
  }
  bool empty() const { return image_.isNull(); }
  float image_opacity() const { return image_opacity_; }
  void SetImageOpacity(float opacity) { image_opacity_ = opacity; SchedulePaint(); }
  void SetInteractionEnabled(bool enabled) { dragging_ = false; interaction_enabled_ = enabled; }
  void SetImage(gfx::ImageSkia image) {
    logical_size_ = image.size();
    image_ = std::move(image);
    dragging_ = false;
    Fit();
  }
  void SetFrame(gfx::ImageSkia image) {
    image_ = std::move(image);
    SchedulePaint();
  }
  int FrameEdge() const {
    const auto* compositor = GetWidget()->GetCompositor();
    const float dpi = compositor ? compositor->device_scale_factor() : 1.f;
    return std::clamp(static_cast<int>(std::ceil(
        std::max(logical_size_.width(), logical_size_.height()) * Scale() * dpi)), 1, kPreviewEdge);
  }
  void Fit() { fit_to_window_ = true; pan_ = {}; SchedulePaint(); }
  void Zoom(float factor) { ZoomAt(factor, gfx::PointF(width() / 2.f, height() / 2.f)); }
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnMouseDragged(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseCaptureLost() override;
  ui::Cursor GetCursor(const ui::MouseEvent&) override;
  void OnBoundsChanged(const gfx::Rect& previous) override;
 private:
  float FitScale() const {
    if (empty()) return 1.f;
    return std::min({1.f, std::max(1, width()) / static_cast<float>(logical_size_.width()),
        std::max(1, height()) / static_cast<float>(logical_size_.height())});
  }
  float Scale() const { return fit_to_window_ ? FitScale() : manual_scale_; }
  void ClampPan() {
    if (empty()) { pan_ = {}; return; }
    const float x = std::max(0.f, (logical_size_.width() * Scale() - width()) / 2.f);
    const float y = std::max(0.f, (logical_size_.height() * Scale() - height()) / 2.f);
    pan_ = gfx::Vector2dF(std::clamp(pan_.x(), -x, x), std::clamp(pan_.y(), -y, y));
  }
  void ZoomAt(float factor, gfx::PointF anchor) {
    if (empty()) return;
    SetScaleAt(std::clamp(Scale() * factor, FitScale() * 0.1f,
        std::max(1.f, FitScale() * 16.f)), anchor);
  }
  void ToggleActualSize(gfx::PointF anchor) {
    if (!fit_to_window_ && std::abs(Scale() - 1.f) < 0.001f) {
      Fit();
      return;
    }
    // One preview-image pixel per DIP; the decoded preview may be downsampled.
    SetScaleAt(1.f, anchor);
  }
  void SetScaleAt(float scale, gfx::PointF anchor) {
    const float ratio = scale / Scale();
    fit_to_window_ = false;
    manual_scale_ = scale;
    const float x = anchor.x() - width() / 2.f, y = anchor.y() - height() / 2.f;
    pan_ = gfx::Vector2dF(x - (x - pan_.x()) * ratio, y - (y - pan_.y()) * ratio);
    ClampPan(); SchedulePaint();
  }
  gfx::ImageSkia image_;
  gfx::Size logical_size_;
  float image_opacity_ = 1.f;
  bool fit_to_window_ = true;
  float manual_scale_ = 1.f;
  gfx::Vector2dF pan_;
  gfx::PointF last_point_;
  bool dragging_ = false;
  bool interaction_enabled_ = true;
};
void ImageCanvas::OnPaint(gfx::Canvas* canvas) {
  views::View::OnPaint(canvas);
  if (empty() || image_opacity_ <= 0.f) return;
  const float scale = Scale();
  const int w = std::max(1, static_cast<int>(std::round(logical_size_.width() * scale)));
  const int h = std::max(1, static_cast<int>(std::round(logical_size_.height() * scale)));
  const int x = static_cast<int>(std::round((width() - w) / 2.f + pan_.x()));
  const int y = static_cast<int>(std::round((height() - h) / 2.f + pan_.y()));
  if (image_opacity_ < 1.f)
    canvas->SaveLayerAlpha(static_cast<uint8_t>(std::round(image_opacity_ * 255.f)), GetLocalBounds());
  else canvas->Save();
  canvas->ClipRect(gfx::Rect(x, y, w, h));
  // A checkerboard distinguishes transparent pixels from the window surface.
  for (int row = 0; row < height(); row += 24)
    for (int column = 0; column < width(); column += 24)
      if (((row / 24) + (column / 24)) % 2 == 0)
        canvas->FillRect(gfx::Rect(column, row, 24, 24), GetColorProvider()->GetColor(ui::kColorSysSurface));
  canvas->DrawImageInt(image_, 0, 0, image_.width(), image_.height(), x, y, w, h, true);
  canvas->Restore();
}
bool ImageCanvas::OnMouseWheel(const ui::MouseWheelEvent& event) {
  if (!interaction_enabled_ || empty() || !event.y_offset()) return false;
  ZoomAt(std::pow(1.2f, std::clamp(event.y_offset() / 120.f, -4.f, 4.f)), event.location_f());
  return true;
}
bool ImageCanvas::OnMousePressed(const ui::MouseEvent& event) {
  if (!interaction_enabled_ || empty() || !event.IsOnlyLeftMouseButton()) return false;
  RequestFocus();
  dragging_ = event.GetClickCount() != 2;
  if (dragging_) last_point_ = event.location_f();
  else ToggleActualSize(event.location_f());
  if (GetWidget()) GetWidget()->SetCursor(GetCursor(event));
  return true;
}
bool ImageCanvas::OnMouseDragged(const ui::MouseEvent& event) {
  if (!dragging_) return false;
  pan_ += event.location_f() - last_point_;
  last_point_ = event.location_f(); ClampPan(); SchedulePaint();
  return true;
}
void ImageCanvas::OnMouseReleased(const ui::MouseEvent& event) {
  dragging_ = false;
  if (GetWidget()) GetWidget()->SetCursor(GetCursor(event));
}
void ImageCanvas::OnMouseCaptureLost() { dragging_ = false; }
ui::Cursor ImageCanvas::GetCursor(const ui::MouseEvent&) {
  return ui::Cursor(!interaction_enabled_ || empty() ? ui::mojom::CursorType::kPointer :
      dragging_ ? ui::mojom::CursorType::kGrabbing : ui::mojom::CursorType::kGrab);
}
void ImageCanvas::OnBoundsChanged(const gfx::Rect& previous) {
  views::View::OnBoundsChanged(previous); ClampPan(); SchedulePaint();
}

BEGIN_METADATA(ImageCanvas)
END_METADATA

class ImageViewer : public views::View, public gfx::AnimationDelegate, public views::WidgetObserver {
  METADATA_HEADER(ImageViewer, views::View)
 public:
  ImageViewer(std::vector<base::FilePath> paths, size_t index)
      : paths_(std::move(paths)), index_(index) {
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(6), 4));
    auto* toolbar = AddChildView(std::make_unique<views::View>());
    auto* columns = toolbar->SetLayoutManager(std::make_unique<views::TableLayout>());
    columns->AddColumn(views::LayoutAlignment::kStretch, views::LayoutAlignment::kCenter,
        1.f, views::TableLayout::ColumnSize::kFixed, 0, 0);
    columns->AddPaddingColumn(0.f, 4);
    columns->AddColumn(views::LayoutAlignment::kStretch, views::LayoutAlignment::kCenter,
        0.f, views::TableLayout::ColumnSize::kUsePreferred, 0, 0);
    columns->AddPaddingColumn(0.f, 4);
    columns->AddColumn(views::LayoutAlignment::kEnd, views::LayoutAlignment::kCenter,
        1.f, views::TableLayout::ColumnSize::kFixed, 0, 0);
    // Equal side columns keep navigation centered regardless of filename length.
    columns->LinkColumnSizes({0, 4});
    columns->AddRows(1, 0.f);
    auto* file = toolbar->AddChildView(std::make_unique<views::View>());
    file->SetLayoutManager(std::make_unique<views::FlexLayout>())
        ->SetOrientation(views::LayoutOrientation::kHorizontal)
        .SetCrossAxisAlignment(views::LayoutAlignment::kCenter);
    title_ = file->AddChildView(std::make_unique<views::Label>());
    title_->SetHorizontalAlignment(gfx::ALIGN_LEFT); title_->SetElideBehavior(gfx::ELIDE_MIDDLE);
    title_->SetProperty(views::kFlexBehaviorKey, views::FlexSpecification(
        views::MinimumFlexSizeRule::kScaleToZero, views::MaximumFlexSizeRule::kPreferred));
    external_ = Button(file, kOpenInNewIcon, IDS_CRFILES_IMAGE_OPEN_SYSTEM,
        base::BindRepeating(&ImageViewer::OpenSystem, weak_.GetWeakPtr()));
    external_->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 8, 0, 0));
    auto* navigation = toolbar->AddChildView(std::make_unique<views::View>());
    auto* buttons = navigation->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
    buttons->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    previous_ = Button(navigation, kKeyboardArrowLeftIcon, IDS_CRFILES_IMAGE_PREVIOUS,
        base::BindRepeating(&ImageViewer::Step, weak_.GetWeakPtr(), -1));
    next_ = Button(navigation, kKeyboardArrowRightIcon, IDS_CRFILES_IMAGE_NEXT,
        base::BindRepeating(&ImageViewer::Step, weak_.GetWeakPtr(), 1));
    position_ = toolbar->AddChildView(std::make_unique<views::Label>());
    position_->SetHorizontalAlignment(gfx::ALIGN_RIGHT);
    auto* surface = AddChildView(std::make_unique<views::View>());
    surface->SetLayoutManager(std::make_unique<views::FillLayout>());
    canvas_ = surface->AddChildView(std::make_unique<ImageCanvas>());
    status_ = surface->AddChildView(std::make_unique<views::Label>());
    status_->SetMultiLine(true); status_->SetCanProcessEventsWithinSubtree(false);
    layout->SetFlexForView(surface, 1);
    SetBackground(views::CreateSolidBackground(ui::kColorSysSurface));
    for (auto key : {ui::VKEY_LEFT, ui::VKEY_RIGHT, ui::VKEY_ESCAPE, ui::VKEY_HOME,
                    ui::VKEY_OEM_PLUS, ui::VKEY_ADD, ui::VKEY_OEM_MINUS, ui::VKEY_SUBTRACT, ui::VKEY_0})
      AddAccelerator(ui::Accelerator(key, ui::EF_NONE));
    AddAccelerator(ui::Accelerator(ui::VKEY_OEM_PLUS, ui::EF_SHIFT_DOWN));
    runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE});
    Load();
  }
  ~ImageViewer() override;
  views::View* focus_view() const { return canvas_; }
  void HandleEscape() {
    if (!GetWidget() || GetWidget()->IsClosed()) return;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce([](base::WeakPtr<views::Widget> widget) {
          if (widget && !widget->IsClosed()) widget->Close();
        }, GetWidget()->GetWeakPtr()));
  }
  gfx::Size GetMinimumSize() const override;
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override;
  void AnimationProgressed(const gfx::Animation* animation) override;
  void AnimationEnded(const gfx::Animation* animation) override;
 private:
  views::ImageButton* Button(views::View* parent, const gfx::VectorIcon& icon,
                            int label, base::RepeatingClosure action) {
    return parent->AddChildView(views::ImageButton::CreateIconButton(std::move(action), icon, Text(label)));
  }
  void AddedToWidget() override {
    views::View::AddedToWidget();
    observation_.Observe(GetWidget());
    UpdatePlayback(GetWidget()->IsVisible());
  }

  void RemovedFromWidget() override {
    UpdatePlayback(false);
    observation_.Reset();
    views::View::RemovedFromWidget();
  }

  void OnWidgetVisibilityChanged(views::Widget*, bool visible) override {
    UpdatePlayback(visible);
  }
  void OnWidgetShowStateChanged(views::Widget* widget) override {
    UpdatePlayback(widget->IsVisible());
  }
  void OnWidgetClosing(views::Widget*) override {
    canceled_->store(true);
    UpdatePlayback(false);
  }
  void OnWidgetDestroying(views::Widget*) override {
    canceled_->store(true);
    UpdatePlayback(false);
    observation_.Reset();
  }

  void UpdatePlayback(bool visible) {
    if (!visible || !has_more_frames_ || loading_ || canceled_->load() ||
        !GetWidget() || GetWidget()->IsClosed() || GetWidget()->IsMinimized()) {
      if (frame_timer_.IsRunning()) {
        elapsed_ += base::TimeTicks::Now() - play_start_;
        frame_timer_.Stop();
      }
      return;
    }
    if (frame_timer_.IsRunning()) return;
    play_start_ = base::TimeTicks::Now();
    frame_timer_.Start(FROM_HERE, base::Hertz(30), this, &ImageViewer::RequestFrame);
  }

  void RequestFrame() {
    if (frame_pending_) return;
    frame_pending_ = true;
    const auto time = elapsed_ + (base::TimeTicks::Now() - play_start_);
    decoder_.AsyncCall(&ViewerDecoder::FrameAt).WithArgs(time, canvas_->FrameEdge())
        .Then(base::BindOnce(&ImageViewer::ShowFrame, weak_.GetWeakPtr(), generation_));
  }

  void ShowFrame(uint64_t generation, PreviewFrame frame) {
    if (generation != generation_ || canceled_->load()) return;
    frame_pending_ = false;
    if (!frame.image.isNull()) canvas_->SetFrame(std::move(frame.image));
    if (!frame.has_more) {
      has_more_frames_ = false;
      frame_timer_.Stop();
      decoder_.Reset();
    }
  }

  void Zoom(float factor) { if (!loading_) canvas_->Zoom(factor); }
  void Fit() { if (!loading_) canvas_->Fit(); }
  void StartFade(float target) {
    fade_.Stop();
    fade_from_ = canvas_->image_opacity();
    fade_target_ = target;
    if (!IsDrawn() || !gfx::Animation::ShouldRenderRichAnimation() ||
        gfx::Animation::PrefersReducedMotion() || fade_from_ == target) {
      canvas_->SetImageOpacity(target);
      if (fading_out_) FinishFadeOut();
      return;
    }
    fade_.SetDuration(base::Milliseconds(fading_out_ ? 120 : 160));
    fade_.Start();
  }
  void FinishFadeOut() {
    fading_out_ = false;
    canvas_->SetImage({});
    canvas_->SetImageOpacity(0.f);
    status_->SetText(Text(IDS_CRFILES_IMAGE_LOADING)); status_->SetVisible(true);
    if (preview_ready_) PresentPreview();
  }
  void PresentPreview() {
    preview_ready_ = false;
    loading_ = false;
    const bool ok = !pending_image_.isNull();
    canvas_->SetImage(std::move(pending_image_));
    pending_image_ = {};
    canvas_->SetInteractionEnabled(true);
    status_->SetText(ok ? std::u16string() : Text(IDS_CRFILES_OPEN_ERROR)); status_->SetVisible(!ok);
    if (ok) StartFade(1.f);
    UpdatePlayback(GetWidget() && GetWidget()->IsVisible());
  }
  void Step(int direction) {
    if ((direction < 0 && index_ == 0) || (direction > 0 && index_ + 1 == paths_.size())) return;
    index_ = direction < 0 ? index_ - 1 : index_ + 1;
    Load();
  }
  void Load() {
    frame_timer_.Stop();
    frame_pending_ = false;
    has_more_frames_ = false;
    elapsed_ = {};
    canceled_->store(true);
    canceled_ = std::make_shared<std::atomic_bool>(false);
    const auto generation = ++generation_;
    fade_.Stop();
    pending_image_ = {};
    preview_ready_ = false;
    loading_ = true;
    canvas_->SetInteractionEnabled(false);
    status_->SetVisible(false);
    fading_out_ = !canvas_->empty();
    if (fading_out_) StartFade(0.f);
    else FinishFadeOut();
    previous_->SetEnabled(index_ > 0); next_->SetEnabled(index_ + 1 < paths_.size());
    title_->SetText(paths_[index_].BaseName().AsUTF16Unsafe());
    position_->SetText(base::NumberToString16(index_ + 1) + u" / " + base::NumberToString16(paths_.size()));
    title_->SetTooltipText(paths_[index_].AsUTF16Unsafe());
    if (GetWidget()) {
      GetWidget()->widget_delegate()->SetTitle(paths_[index_].DirName().AsUTF16Unsafe());
      GetWidget()->UpdateWindowTitle();
    }
    decoder_.emplace(runner_, canceled_);
    decoder_.AsyncCall(&ViewerDecoder::Open).WithArgs(paths_[index_])
        .Then(base::BindOnce(&ImageViewer::Loaded, weak_.GetWeakPtr(), generation));
  }
  void Loaded(uint64_t generation, PreviewFrame frame) {
    if (generation != generation_ || canceled_->load()) return;
    has_more_frames_ = frame.has_more;
    if (!has_more_frames_) decoder_.Reset();
    pending_image_ = std::move(frame.image);
    preview_ready_ = true;
    if (!fading_out_) PresentPreview();
  }
  void OpenSystem() {
    external_->SetEnabled(false);
    const auto path = paths_[index_];
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&OpenFileWith, path, std::string()),
        base::BindOnce([](base::WeakPtr<ImageViewer> viewer, base::FilePath path, bool ok) {
          if (!viewer) return;
          viewer->external_->SetEnabled(true);
          if (viewer->paths_[viewer->index_] != path) return;
          if (!ok) { viewer->status_->SetText(Text(IDS_CRFILES_OPEN_ERROR)); viewer->status_->SetVisible(true); }
          else if (!viewer->loading_ && !viewer->canvas_->empty()) viewer->status_->SetVisible(false);
        }, weak_.GetWeakPtr(), path));
  }
  std::vector<base::FilePath> paths_;
  size_t index_ = 0;
  uint64_t generation_ = 0;
  raw_ptr<ImageCanvas> canvas_ = nullptr;
  raw_ptr<views::Label> title_ = nullptr, position_ = nullptr, status_ = nullptr;
  raw_ptr<views::ImageButton> previous_ = nullptr, next_ = nullptr;
  raw_ptr<views::ImageButton> external_ = nullptr;
  std::shared_ptr<std::atomic_bool> canceled_ = std::make_shared<std::atomic_bool>(false);
  scoped_refptr<base::SequencedTaskRunner> runner_;
  base::SequenceBound<ViewerDecoder> decoder_;
  base::ScopedObservation<views::Widget, views::WidgetObserver> observation_{this};
  base::RepeatingTimer frame_timer_;
  base::TimeTicks play_start_;
  base::TimeDelta elapsed_;
  bool frame_pending_ = false;
  bool has_more_frames_ = false;
  gfx::ImageSkia pending_image_;
  bool preview_ready_ = false;
  bool loading_ = false;
  bool fading_out_ = false;
  float fade_from_ = 0.f, fade_target_ = 1.f;
  gfx::LinearAnimation fade_{base::Milliseconds(160), gfx::LinearAnimation::kDefaultFrameRate, this};
  base::WeakPtrFactory<ImageViewer> weak_{this};
};
ImageViewer::~ImageViewer() { canceled_->store(true); frame_timer_.Stop(); fade_.Stop(); }
void ImageViewer::AnimationProgressed(const gfx::Animation* animation) {
  const float progress = static_cast<float>(animation->GetCurrentValue());
  const float eased = progress * progress * (3.f - 2.f * progress);
  canvas_->SetImageOpacity(fade_from_ + (fade_target_ - fade_from_) * eased);
}
void ImageViewer::AnimationEnded(const gfx::Animation*) {
  canvas_->SetImageOpacity(fade_target_);
  if (fading_out_) FinishFadeOut();
}
gfx::Size ImageViewer::GetMinimumSize() const { return gfx::Size(360, 240); }
bool ImageViewer::AcceleratorPressed(const ui::Accelerator& accelerator) {
  switch (accelerator.key_code()) {
    case ui::VKEY_LEFT: Step(-1); break;
    case ui::VKEY_RIGHT: Step(1); break;
    case ui::VKEY_HOME: case ui::VKEY_0: Fit(); break;
    case ui::VKEY_OEM_PLUS: case ui::VKEY_ADD: Zoom(1.2f); break;
    case ui::VKEY_OEM_MINUS: case ui::VKEY_SUBTRACT: Zoom(1.f / 1.2f); break;
    case ui::VKEY_ESCAPE:
      HandleEscape();
      break;
    default: return false;
  }
  return true;
}

BEGIN_METADATA(ImageViewer)
END_METADATA
}  // namespace

bool IsImageViewerFile(const base::FilePath& path, std::string_view mime) {
  if (mime.starts_with("image/")) return true;
  for (const auto* extension : {FILE_PATH_LITERAL(".png"), FILE_PATH_LITERAL(".jpg"), FILE_PATH_LITERAL(".jpeg"),
      FILE_PATH_LITERAL(".jpe"), FILE_PATH_LITERAL(".jfif"), FILE_PATH_LITERAL(".webp"), FILE_PATH_LITERAL(".gif"),
      FILE_PATH_LITERAL(".bmp"), FILE_PATH_LITERAL(".ico"), FILE_PATH_LITERAL(".wbmp"), FILE_PATH_LITERAL(".avif"),
      FILE_PATH_LITERAL(".svg"), FILE_PATH_LITERAL(".heic"), FILE_PATH_LITERAL(".heif")})
    if (path.MatchesExtension(extension)) return true;
  return false;
}

void ShowCrFilesImageViewer(views::Widget* parent, std::vector<base::FilePath> paths,
                           const base::FilePath& selected) {
  if (!parent || parent->IsClosed()) return;
  std::erase_if(paths, [&](const auto& path) { return path.DirName() != selected.DirName(); });
  auto found = std::ranges::find(paths, selected);
  if (found == paths.end()) { paths.push_back(selected); found = paths.end() - 1; }
  const size_t index = static_cast<size_t>(found - paths.begin());
  auto dialog = std::make_unique<views::DialogDelegate>();
  dialog->SetTitle(selected.DirName().AsUTF16Unsafe());
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  dialog->SetModalType(ui::mojom::ModalType::kWindow);
  dialog->set_draggable(true);
  dialog->SetCanResize(true); dialog->SetCanMaximize(true); dialog->SetShowCloseButton(true);
  auto viewer = std::make_unique<ImageViewer>(std::move(paths), index);
  dialog->SetInitiallyFocusedView(viewer->focus_view());
  const auto parent_bounds = parent->GetWindowBoundsInScreen();
  const auto work_area = display::Screen::Get()->GetDisplayNearestWindow(parent->GetNativeWindow()).work_area();
  const gfx::Size size(std::min(work_area.width(), std::max(360,
      static_cast<int>(std::round(parent_bounds.width() * 0.828)))),
      std::min(work_area.height(), std::max(320,
      static_cast<int>(std::round(parent_bounds.height() * 0.828)))));
  const auto center = parent_bounds.CenterPoint();
  gfx::Rect bounds(center.x() - size.width() / 2, center.y() - size.height() / 2,
      size.width(), size.height());
  bounds.AdjustToFit(work_area);
  viewer->SetPreferredSize(size);
  dialog->SetContentsView(std::move(viewer));
  ShowCrFilesDialog(std::move(dialog), parent, bounds);
}
}  // namespace crfiles
