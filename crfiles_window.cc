// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_cache_state.h"
#include "crFiles/crfiles_window.h"
#include "crFiles/crfiles_tray.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/crfiles_tags.h"
#include "crFiles/crfiles_tags_view.h"
#include "crFiles/crfiles_search_view.h"
#include "crFiles/crfiles_network.h"
#include "crFiles/crfiles_network_share.h"
#include "crFiles/crfiles_network_transfer.h"
#include "crFiles/crfiles_context_actions.h"
#include "ui/views/context_menu_controller.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/hit_test.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "net/base/filename_util.h"
#include "ui/display/screen.h"
#include <algorithm>
#include <deque>
#include <atomic>
#include <memory>
#include <list>
#include <iterator>
#include <numeric>
#include <limits>
#include "base/callback_list.h"
#include "base/scoped_observation.h"
#include "base/logging.h"
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <map>
#include <set>
#include <vector>
#include "base/base_paths.h"
#include "base/containers/flat_set.h"
#include "base/strings/string_util.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/file_path_watcher.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "base/i18n/case_conversion.h"
#include "base/i18n/rtl.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/gfx/image/image_skia_rep.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/uuid.h"
#include "base/values.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/task/bind_post_task.h"
#include "base/threading/sequence_bound.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_LINUX)
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/desktop_aura/desktop_window_tree_host_linux.h"
#include "ui/views/window/non_client_view.h"
#endif
#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#if BUILDFLAG(IS_LINUX)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/gfx/vector_icon_types.h"
#include "crFiles/crfiles_app_icon.h"
#include "crFiles/crfiles_file_icons.h"
#include "crFiles/crfiles_image_preview.h"
#include "crFiles/crfiles_image_viewer.h"
#include "crFiles/crfiles_pdf_reader.h"
#include "crFiles/crfiles_heic_thumbnail.h"
#include "crFiles/crfiles_text_thumbnail.h"
#include "crFiles/crfiles_video_thumbnail.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "net/base/mime_util.h"
#include "crFiles/crfiles_frame_view.h"
#include "crFiles/crfiles_settings_dialog.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_trash.h"
#include "crFiles/crfiles_file_operations.h"
#include "crFiles/crfiles_archive.h"
#include "crFiles/crfiles_archive_dialog.h"
#include "ui/views/drag_controller.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/clipboard/file_info.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/mojom/drag_drop_types.mojom.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/base/dragdrop/drop_target_event.h"
#include "ui/compositor/layer_tree_owner.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "ui/views/view_tracker.h"
#include "ui/views/view_targeter.h"
#include "ui/views/animation/animation_delegate_views.h"
#include "ui/views/animation/bounds_animator.h"
#include "ui/views/animation/bounds_animator_observer.h"
#include "ui/views/controls/button/button_controller.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/repeat_controller.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/rect_based_targeting_utils.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/views/layout/layout_manager.h"
#include "ui/views/layout/delegating_layout_manager.h"
#include "ui/views/layout/proposed_layout.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/base/text/bytes_formatting.h"
#include "ui/views/controls/focus_ring.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "crFiles/grit/crfiles_strings.h"
#include "crFiles/sharing/nearby_sharing.h"
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
#include "crFiles/sharing/quick_share_adapter.h"
#endif
#include "crFiles/sharing/file_share_dialog.h"
#include "crFiles/sharing/sync_dialog.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/native_theme/native_theme.h"
#include "ui/events/event_handler.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animator.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/image/image_skia_operations.h"
#include "skia/ext/image_operations.h"
#include "ui/gfx/geometry/vector2d.h"
#include "ui/gfx/geometry/rrect_f.h"
#include "ui/views/background.h"
#include "ui/views/animation/ink_drop.h"
#include "ui/views/animation/ink_drop_host.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/slider.h"
#include "ui/views/controls/styled_label.h"
#include "ui/gfx/range/range.h"
#include "ui/views/controls/separator.h"
#include "ui/views/controls/resize_area.h"
#include "ui/views/controls/resize_area_delegate.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/throbber.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/scrollbar/base_scroll_bar_thumb.h"
#include "libcr/overlay_scroll_bar.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/flex_layout.h"
#include "ui/views/widget/tooltip_manager.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/metrics.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/window/frame_view.h"
#if BUILDFLAG(IS_WIN)
#include "crFiles/crfiles_windows.h"
#include "base/win/scoped_com_initializer.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif
#if BUILDFLAG(IS_MAC)
#include "crFiles/crfiles_mac.h"
#endif
#if BUILDFLAG(IS_LINUX)
#include "base/nix/xdg_util.h"
#endif
namespace crfiles {
namespace {

// Keep navigation chrome (toolbar, sidebar and active tab) on one surface.
// A lightly tinted content surface separates files without hard dividers and
// gives pale document icons more definition in light mode.
constexpr ui::ColorId kFolderSurfaceColor = ui::kColorSysSurface1;

// Share the navigation surface with the active tab, including theme changes.
ui::ColorId NavigationColor(const views::View* view) {
  const auto* widget = view->GetWidget();
  const bool light = widget->GetColorMode() == ui::ColorProviderKey::ColorMode::kLight;
  const bool normal_contrast =
      widget->GetNativeTheme()->preferred_contrast() !=
          ui::NativeTheme::PreferredContrast::kMore &&
      widget->GetForcedColors() == ui::ColorProviderKey::ForcedColors::kNone;
  return light && normal_contrast ? ui::kColorSysSurface3 : ui::kColorSysBase;
}

class NavigationBackground : public views::Background {
 public:
  NavigationBackground() { set_color(ui::kColorSysBase); }

  void Paint(gfx::Canvas* canvas, views::View* view) const override {
    canvas->DrawColor(view->GetColorProvider()->GetColor(NavigationColor(view)));
  }

  void OnViewThemeChanged(views::View* view) override {
    set_color(NavigationColor(view));
    view->SchedulePaint();
  }
};

constexpr int kFolderEdgeFadeHeight = 12;

class FolderEdgeFade : public views::View {
  METADATA_HEADER(FolderEdgeFade, views::View)
 public:
  FolderEdgeFade(views::ScrollView* scroll, bool top) : scroll_(scroll), top_(top) {
    SetCanProcessEventsWithinSubtree(false);
    GetViewAccessibility().SetIsIgnored(true);
    scroll_subscription_ = scroll_->AddContentsScrolledCallback(base::BindRepeating(
        &FolderEdgeFade::SchedulePaint, base::Unretained(this)));
  }
  void OnThemeChanged() override {
    views::View::OnThemeChanged();
    SchedulePaint();
  }
 protected:
  void OnPaint(gfx::Canvas* canvas) override {
    if (!scroll_->contents() || width() <= 0 || height() <= 0) return;
    const float offset = scroll_->CurrentOffset().y();
    const float distance = top_ ? offset :
        scroll_->contents()->height() - scroll_->GetVisibleRect().height() - offset;
    // Ease down to zero at the boundary instead of popping a full-strength
    // edge on the first pixel of scrolling. The indicator takes no layout space.
    const float fraction = std::clamp(distance / kFolderEdgeFadeHeight, 0.0f, 1.0f);
    if (fraction <= 0) return;
    auto edge = SkColor4f::FromColor(GetColorProvider()->GetColor(kFolderSurfaceColor));
    edge.fA *= fraction * fraction * (3.0f - 2.0f * fraction);
    auto transparent = edge;
    transparent.fA = 0;
    const SkColor4f colors[] = {edge, transparent};
    const SkPoint points[] = {{0, top_ ? 0.0f : static_cast<float>(height())},
                              {0, top_ ? static_cast<float>(height()) : 0.0f}};
    cc::PaintFlags flags;
    flags.setShader(cc::PaintShader::MakeLinearGradient(points, colors, nullptr, 2, SkTileMode::kClamp));
    canvas->DrawRect(GetLocalBounds(), flags);
  }
 private:
  const raw_ptr<views::ScrollView> scroll_;
  const bool top_;
  base::CallbackListSubscription scroll_subscription_;
};
BEGIN_METADATA(FolderEdgeFade)
END_METADATA

// Match the pane surface so the toolbar reads as part of the folder view.
class FooterBackground : public views::Background {
 public:
  explicit FooterBackground(ui::ColorId surface) : surface_(surface) {}
  void Paint(gfx::Canvas* canvas, views::View* view) const override {
    canvas->FillRect(view->GetLocalBounds(), view->GetColorProvider()->GetColor(surface_));
  }
  void OnViewThemeChanged(views::View* view) override { view->SchedulePaint(); }
 private:
  const ui::ColorId surface_;
};

class FooterGlowBorder : public views::Border {
 public:
  explicit FooterGlowBorder(bool trash) : trash_(trash) {}
  gfx::Insets GetInsets() const override { return gfx::Insets(1); }
  gfx::Size GetMinimumSize() const override { return gfx::Size(2, 2); }
  void Paint(const views::View& view, gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1.f);
    flags.setColor(view.GetColorProvider()->GetColor(
        trash_ ? ui::kColorSysPrimary : ui::kColorSysOutline));
    for (int i = 0; i < (trash_ ? 3 : 1); ++i) {
      const float inset = i + 0.5f;
      if (view.width() <= 2.f * inset || view.height() <= 2.f * inset) break;
      if (trash_) flags.setAlphaf(0.18f * (3 - i));
      const gfx::RectF bounds(inset, inset, view.width() - 2.f * inset,
                              view.height() - 2.f * inset);
      if (trash_) {
        canvas->DrawRoundRect(bounds, 24.f - inset, flags);
      } else {
        const float radius = std::min(12.f - inset,
            std::min(bounds.width() / 2.f, bounds.height()));
        constexpr float kArcControl = 0.55228475f;
        const float control = radius * kArcControl;
        const float left = bounds.x(), right = bounds.right(), top = bounds.y();
        SkPathBuilder path;
        path.moveTo(left, view.height()).lineTo(left, top + radius)
            .cubicTo(left, top + radius - control, left + radius - control, top, left + radius, top)
            .lineTo(right - radius, top)
            .cubicTo(right - radius + control, top, right, top + radius - control, right, top + radius)
            .lineTo(right, view.height());
        canvas->DrawPath(path.detach(), flags);
      }
    }
  }
 private:
  const bool trash_;
};

constexpr int kFullscreenExitSize = 44;
constexpr int kFullscreenExitTop = 68;
constexpr int kFullscreenExitSlide = kFullscreenExitTop + kFullscreenExitSize + 10;
// Android head, drawn in the same theme color as other menu icons.
constexpr gfx::PathElement kQuickShareAndroidIconPath[] = {
    gfx::CANVAS_DIMENSIONS, 24,
    gfx::MOVE_TO, 2, 19,
    gfx::CUBIC_TO, 2, 13.48f, 6.48f, 9, 12, 9,
    gfx::CUBIC_TO, 17.52f, 9, 22, 13.48f, 22, 19,
    gfx::CLOSE,
    gfx::CIRCLE, 7.5f, 15.5f, 1,
    gfx::CIRCLE, 16.5f, 15.5f, 1,
    gfx::NEW_PATH,
    gfx::MOVE_TO, 7, 10.5f,
    gfx::LINE_TO, 4.5f, 6,
    gfx::LINE_TO, 5.8f, 5.3f,
    gfx::LINE_TO, 8.3f, 9.8f,
    gfx::CLOSE,
    gfx::MOVE_TO, 17, 10.5f,
    gfx::LINE_TO, 19.5f, 6,
    gfx::LINE_TO, 18.2f, 5.3f,
    gfx::LINE_TO, 15.7f, 9.8f,
    gfx::CLOSE,
};
const gfx::VectorIconRep kQuickShareAndroidIconReps[] = {{kQuickShareAndroidIconPath}};
const gfx::VectorIcon kQuickShareAndroidIcon = {kQuickShareAndroidIconReps, 1, "quick_share_android"};
// A path tag, drawn in the same theme color as other menu icons.
constexpr gfx::PathElement kTagsIconPath[] = {
    gfx::CANVAS_DIMENSIONS, 20, gfx::STROKE, 1.5f,
    gfx::MOVE_TO, 2, 3, gfx::LINE_TO, 10, 3, gfx::LINE_TO, 18, 11,
    gfx::LINE_TO, 11, 18, gfx::LINE_TO, 2, 9, gfx::CLOSE,
    gfx::CIRCLE, 6, 7, 1,
};
const gfx::VectorIconRep kTagsIconReps[] = {{kTagsIconPath}};
const gfx::VectorIcon kTagsIcon(kTagsIconReps, 1, "crfiles_tags");

// Geometry from tag-folder.svg. Both paths inherit the theme's icon color.
constexpr gfx::PathElement kTagFolderIconPath[] = {
    gfx::CANVAS_DIMENSIONS, 24, gfx::STROKE, 1.5f, gfx::CAP_SQUARE,
    gfx::MOVE_TO, 3, 10, gfx::LINE_TO, 3, 5.75f,
    gfx::LINE_TO, 4.25f, 4.5f, gfx::LINE_TO, 9, 4.5f,
    gfx::LINE_TO, 11.5f, 7, gfx::LINE_TO, 19.75f, 7,
    gfx::LINE_TO, 21, 8.25f, gfx::LINE_TO, 21, 10.5f,
    gfx::MOVE_TO, 3, 13.5f, gfx::LINE_TO, 3, 17.75f,
    gfx::LINE_TO, 4.25f, 19, gfx::LINE_TO, 7, 19,
    gfx::MOVE_TO, 10, 19, gfx::LINE_TO, 11, 19,
    gfx::MOVE_TO, 6.5f, 10, gfx::LINE_TO, 16.5f, 10,
    gfx::MOVE_TO, 14, 12.5f, gfx::LINE_TO, 18.5f, 12.5f,
    gfx::LINE_TO, 22.5f, 16.5f, gfx::LINE_TO, 17.5f, 21.5f,
    gfx::LINE_TO, 13.5f, 17.5f, gfx::LINE_TO, 13.5f, 13,
    gfx::CLOSE,
    gfx::NEW_PATH, gfx::CIRCLE, 16.5f, 15.5f, 0.9f,
};
const gfx::VectorIconRep kTagFolderIconReps[] = {{kTagFolderIconPath}};
const gfx::VectorIcon kTagFolderIcon(kTagFolderIconReps, 1, "crfiles_tag_folder");

constexpr gfx::PathElement kBreadcrumbDiskIconPath[] = {
    gfx::CANVAS_DIMENSIONS, 20,
    gfx::STROKE, 1.5f,
    gfx::MOVE_TO, 5, 4, gfx::LINE_TO, 15, 4,
    gfx::LINE_TO, 18, 11, gfx::LINE_TO, 18, 16,
    gfx::LINE_TO, 2, 16, gfx::LINE_TO, 2, 11, gfx::CLOSE,
    gfx::MOVE_TO, 2, 11, gfx::LINE_TO, 18, 11,
    gfx::MOVE_TO, 5, 13.5f, gfx::LINE_TO, 6, 13.5f,
    gfx::MOVE_TO, 9, 13.5f, gfx::LINE_TO, 10, 13.5f,
};
const gfx::VectorIconRep kBreadcrumbDiskIconReps[] = {{kBreadcrumbDiskIconPath}};
const gfx::VectorIcon kBreadcrumbDiskIcon(kBreadcrumbDiskIconReps, 1, "crfiles_breadcrumb_disk");

constexpr gfx::PathElement kCompressIconPath[] = {
    gfx::CANVAS_DIMENSIONS, 20,
    gfx::STROKE, 1.5f,
    gfx::MOVE_TO, 4, 2, gfx::LINE_TO, 12, 2,
    gfx::LINE_TO, 16, 6, gfx::LINE_TO, 16, 18,
    gfx::LINE_TO, 4, 18, gfx::CLOSE,
    gfx::MOVE_TO, 12, 2, gfx::LINE_TO, 12, 6,
    gfx::LINE_TO, 16, 6,
    gfx::NEW_PATH,
    gfx::MOVE_TO, 8, 4, gfx::LINE_TO, 10, 4,
    gfx::LINE_TO, 10, 6, gfx::LINE_TO, 8, 6, gfx::CLOSE,
    gfx::MOVE_TO, 10, 6, gfx::LINE_TO, 12, 6,
    gfx::LINE_TO, 12, 8, gfx::LINE_TO, 10, 8, gfx::CLOSE,
    gfx::MOVE_TO, 8, 8, gfx::LINE_TO, 10, 8,
    gfx::LINE_TO, 10, 10, gfx::LINE_TO, 8, 10, gfx::CLOSE,
    gfx::NEW_PATH, gfx::STROKE, 1.5f,
    gfx::ROUND_RECT, 8.5f, 11, 3, 4, 1,
};
const gfx::VectorIconRep kCompressIconReps[] = {{kCompressIconPath}};
const gfx::VectorIcon kCompressIcon(kCompressIconReps, 1, "crfiles_compress");

#if BUILDFLAG(IS_MAC)
constexpr int kPrimaryModifier = ui::EF_COMMAND_DOWN;
#else
constexpr int kPrimaryModifier = ui::EF_CONTROL_DOWN;
#endif
bool PrimaryModifierDown(const ui::Event& event) {
  return (event.flags() & kPrimaryModifier) != 0;
}
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
base::FilePath Home() {
#if BUILDFLAG(IS_MAC)
  if (IsCrFilesSandboxed()) return MacUserHomeDirectory();
#endif
  base::FilePath home;
  base::PathService::Get(base::DIR_HOME, &home);
  return home;
}
// A navigation-only location on Windows. Never pass it to filesystem operations.
bool IsDrivesLocation(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  return path.value() == L"/";
#else
  return false;
#endif
}
bool IsDriveRoot(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  const auto& value = path.value();
  return value.size() == 3 && ((value[0] >= L'A' && value[0] <= L'Z') ||
      (value[0] >= L'a' && value[0] <= L'z')) && value[1] == L':' &&
      (value[2] == L'\\' || value[2] == L'/');
#else
  return false;
#endif
}
std::u16string NavigationPath(const base::FilePath& path) {
  auto text = path.AsUTF16Unsafe();
#if BUILDFLAG(IS_WIN)
  if (text.size() >= 3 && text[1] == u':' && (text[2] == u'\\' || text[2] == u'/') &&
      ((text[0] >= u'A' && text[0] <= u'Z') || (text[0] >= u'a' && text[0] <= u'z'))) {
    if (text[0] >= u'A' && text[0] <= u'Z') text[0] += u'a' - u'A';
    text = u"/" + text.substr(0, 1) + text.substr(2);
    std::ranges::replace(text, u'\\', u'/');
  }
#endif
  return text;
}
std::optional<base::FilePath> ParseNavigationPath(std::u16string_view text) {
  if (text.empty()) return std::nullopt;
#if BUILDFLAG(IS_WIN)
  if (text == u"/") return base::FilePath(FILE_PATH_LITERAL("/"));
  // Preserve UNC paths; only a single leading slash uses the virtual namespace.
  if (text.front() == u'/' && !text.starts_with(u"//")) {
    if (text.size() < 2 || !((text[1] >= u'a' && text[1] <= u'z') ||
        (text[1] >= u'A' && text[1] <= u'Z')) || (text.size() > 2 && text[2] != u'/')) return std::nullopt;
    char16_t drive = text[1];
    if (drive >= u'a' && drive <= u'z') drive -= u'a' - u'A';
    std::u16string native(1, drive);
    native += u":\\";
    if (text.size() > 3) native += text.substr(3);
    return base::FilePath::FromUTF16Unsafe(native).NormalizePathSeparators();
  }
#endif
  return base::FilePath::FromUTF16Unsafe(text);
}
std::u16string EntryName(const base::FilePath& path) {
  if (IsDrivesLocation(path)) return u"/";
  if (IsDriveRoot(path)) return path.AsUTF16Unsafe().substr(0, 2);
  return path.BaseName().AsUTF16Unsafe();
}
std::u16string BreadcrumbName(const base::FilePath& path) {
  if (IsDriveRoot(path)) return NavigationPath(path).substr(1, 1);
  return EntryName(path);
}
enum class FileSort { kNameAscending, kNameDescending, kModifiedDescending, kModifiedAscending, kSize, kType, kSizeAscending, kTypeDescending, kCreatedAscending, kCreatedDescending, kPermissionsAscending, kPermissionsDescending };
constexpr std::array kSortMenuLabels = {
    IDS_CRFILES_SORT_A_Z, IDS_CRFILES_SORT_Z_A,
    IDS_CRFILES_LAST_MODIFIED, IDS_CRFILES_FIRST_MODIFIED,
    IDS_CRFILES_SIZE, IDS_CRFILES_TYPE,
    IDS_CRFILES_SORT_SIZE_ASCENDING, IDS_CRFILES_SORT_TYPE_DESCENDING,
    IDS_CRFILES_SORT_CREATED_ASCENDING, IDS_CRFILES_SORT_CREATED_DESCENDING,
    IDS_CRFILES_SORT_PERMISSIONS_ASCENDING, IDS_CRFILES_SORT_PERMISSIONS_DESCENDING};

struct FileViewOptions {
  bool icons = true;
  int icon_index = 1;
  FileSort sort = FileSort::kModifiedDescending;
  bool show_hidden = false;
  bool size_column = true;
  bool type_column = true;
  bool modified_column = true;
  bool created_column = false, permissions_column = false;
  std::array<int, 6> column_widths = {280, 90, 140, 160, 160, 140};
  bool operator==(const FileViewOptions&) const = default;
  NetworkColumns NetworkColumnOptions() const {
    return {size_column, type_column, modified_column, created_column};
  }
};
struct Entry {
  base::FilePath path;
  bool directory;
  int64_t size;
  base::Time modified;
  std::string mime;
  bool hidden = false;
  // Computed once with metadata, never inside the sort comparator.
  std::u16string name_key = base::i18n::ToLower(EntryName(path));
  bool package = false;
  base::Time created = {};
  std::optional<int> permissions = std::nullopt;
  int64_t tag = 0;  // Nonzero only for virtual folder entries in Tags/.
  std::string tag_name = {};
  size_t tag_count = 0;
};
// Read actual birth time, never POSIX ctime (metadata-change time).
// Called only by directory/metadata workers. Missing metadata displays as a dash.
void ReadExtraMetadata(Entry& entry, const base::FileEnumerator::FileInfo* enumerated = nullptr) {
#if BUILDFLAG(IS_POSIX)
  base::stat_wrapper_t metadata;
  if (enumerated && !S_ISLNK(enumerated->stat().st_mode)) {
    metadata = enumerated->stat();
  } else if (base::File::Stat(entry.path, &metadata) != 0) {
    return;
  }
  entry.permissions = metadata.st_mode & 07777;
#if BUILDFLAG(IS_MAC)
  if (metadata.st_birthtimespec.tv_sec > 0)
    entry.created = base::Time::FromTimeT(metadata.st_birthtimespec.tv_sec) +
        base::Microseconds(metadata.st_birthtimespec.tv_nsec / 1000);
#elif BUILDFLAG(IS_LINUX)
  struct statx birth = {};
  // Avoid requiring the glibc 2.28 statx wrapper in the compatibility sysroot.
  if (syscall(SYS_statx, AT_FDCWD, entry.path.value().c_str(), AT_STATX_SYNC_AS_STAT,
              STATX_BTIME, &birth) == 0 && (birth.stx_mask & STATX_BTIME))
    entry.created = base::Time::FromTimeT(birth.stx_btime.tv_sec) +
        base::Microseconds(birth.stx_btime.tv_nsec / 1000);
#endif
#elif BUILDFLAG(IS_WIN)
  if (enumerated) {
    entry.created = base::Time::FromFileTime(enumerated->find_data().ftCreationTime);
  } else {
    base::File::Info info;
    if (base::GetFileInfo(entry.path, &info)) entry.created = info.creation_time;
  }
#endif
}
std::u16string PermissionsText(const std::optional<int>& permissions) {
  if (!permissions) return u"—";
  const int mode = *permissions;
  std::u16string text = u"---------";
  constexpr std::array<char16_t, 9> symbols = {
      u'r', u'w', u'x', u'r', u'w', u'x', u'r', u'w', u'x'};
  for (size_t i = 0; i < symbols.size(); ++i)
    if (mode & (1 << (8 - i))) text[i] = symbols[i];
  if (mode & 04000) text[2] = mode & 0100 ? u's' : u'S';
  if (mode & 02000) text[5] = mode & 0010 ? u's' : u'S';
  if (mode & 01000) text[8] = mode & 0001 ? u't' : u'T';
  return text;
}
std::u16string CreatedText(base::Time created) {
  return created.is_null() ? u"—" : base::TimeFormatShortDateAndTime(created);
}
struct EntryVisuals {
  FileIcons images = {};
  gfx::ImageSkia thumbnail;
  gfx::ImageSkia thumbnail_display;
  int thumbnail_display_edge = 0;
  bool thumbnail_display_only = false;
  bool thumbnail_attempted = false;
  std::array<bool, kFileIconSizes.size()> icon_loaded = {};
  std::array<unsigned, kFileIconSizes.size()> icon_attempts = {};
  std::array<base::TimeTicks, kFileIconSizes.size()> icon_retry_at = {};
};
// Metadata snapshots are immutable across workers, cache entries and panes.
// Mutable selection, deletion and image state belongs to each pane instead.
template <typename T>
std::shared_ptr<const T> ShareDirectoryData(T value) {
  return std::shared_ptr<const T>(new T(std::move(value)), [](const T* data) {
    // Releasing a large snapshot or diff must not free thousands of strings
    // and map nodes on the UI sequence after a directory switch.
    base::ThreadPool::PostTask(FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](std::unique_ptr<const T>) {}, std::unique_ptr<const T>(data)));
  });
}
template <typename T>
class SnapshotVector {
 public:
  SnapshotVector();
  explicit SnapshotVector(std::vector<T> values);
  SnapshotVector& operator=(std::vector<T> values);
  const std::vector<T>& values() const {
    static const base::NoDestructor<std::vector<T>> empty;
    return values_ ? *values_ : *empty;
  }
  auto begin() const { return values().begin(); }
  auto end() const { return values().end(); }
  const T& operator[](size_t index) const { return values()[index]; }
  size_t size() const { return values().size(); }
  size_t capacity() const { return values().capacity(); }
  bool empty() const { return values().empty(); }
  void clear() { values_.reset(); }
 private:
  std::shared_ptr<const std::vector<T>> values_;
};
template <typename T>
SnapshotVector<T>::SnapshotVector() = default;
template <typename T>
SnapshotVector<T>::SnapshotVector(std::vector<T> values) : values_(ShareDirectoryData(std::move(values))) {}
template <typename T>
SnapshotVector<T>& SnapshotVector<T>::operator=(std::vector<T> values) {
  values_ = ShareDirectoryData(std::move(values)); return *this;
}
using DirectoryEntries = SnapshotVector<Entry>;
using CancelFlag = std::shared_ptr<std::atomic_bool>;
bool Stopped(const CancelFlag& canceled) { return canceled && canceled->load(std::memory_order_relaxed); }
struct SortRecord {
  size_t index;
  base::FilePath path;
  std::u16string name;
  std::string mime;
  base::Time modified;
  int64_t size;
  bool directory, hidden, removed;
  base::Time created;
  std::optional<int> permissions;
};
std::vector<SortRecord> SortRecords(const DirectoryEntries& entries, const CancelFlag& canceled = {},
                                    const std::set<base::FilePath>& removed = {}) {
  std::vector<SortRecord> records; records.reserve(entries.size());
  for (size_t index = 0; index < entries.size(); ++index) {
    if ((index % 256) == 0 && Stopped(canceled)) return {};
    const auto& e = entries[index];
    records.push_back({index, e.path, e.name_key, e.mime, e.modified, e.size, e.directory, e.hidden, removed.contains(e.path), e.created, e.permissions});
  }
  return records;
}
std::vector<size_t> SortDirectory(std::vector<SortRecord> records, FileSort mode,
                                  bool hidden, bool trash, const CancelFlag& canceled) {
  std::vector<size_t> order;
  if (Stopped(canceled)) return order;
  order.reserve(records.size());
  for (size_t index = 0; index < records.size(); ++index) {
    if ((index % 256) == 0 && Stopped(canceled)) return {};
    const auto& e = records[index];
    if (!e.removed && (hidden || trash || !e.hidden)) order.push_back(index);
  }
  auto names = [](const SortRecord& a, const SortRecord& b) {
    return a.name == b.name ? a.path.value() < b.path.value() : a.name < b.name;
  };
  auto less = [&](size_t left, size_t right) {
    const auto& a = records[left]; const auto& b = records[right];
    if (a.directory != b.directory) return a.directory;
    switch (mode) {
      case FileSort::kNameAscending: return names(a, b);
      case FileSort::kNameDescending: return names(b, a);
      case FileSort::kModifiedDescending: if (a.modified != b.modified) return a.modified > b.modified; break;
      case FileSort::kModifiedAscending: if (a.modified != b.modified) return a.modified < b.modified; break;
      case FileSort::kSize: if (a.size != b.size) return a.size > b.size; break;
      case FileSort::kType: if (a.mime != b.mime) return a.mime < b.mime; break;
      case FileSort::kSizeAscending: if (a.size != b.size) return a.size < b.size; break;
      case FileSort::kCreatedAscending: if (a.created != b.created) return a.created < b.created; break;
      case FileSort::kCreatedDescending: if (a.created != b.created) return a.created > b.created; break;
      case FileSort::kPermissionsAscending: if (a.permissions != b.permissions) return a.permissions < b.permissions; break;
      case FileSort::kPermissionsDescending: if (a.permissions != b.permissions) return a.permissions > b.permissions; break;
      case FileSort::kTypeDescending:
        if (a.mime != b.mime) return a.mime > b.mime;
        return names(b, a);
    }
    return names(a, b);
  };
  // Bottom-up merge passes permit cancellation without breaking comparator ordering.
  std::vector<size_t> scratch(order.size());
  for (size_t width = 1; width < order.size(); width *= 2) {
    for (size_t start = 0; start < order.size(); start += 2 * width) {
      size_t left = start, middle = std::min(start + width, order.size()), right = middle;
      const size_t end = std::min(start + 2 * width, order.size());
      for (size_t out = start; out < end; ++out) {
        if ((out % 256) == 0 && Stopped(canceled)) return {};
        if (left < middle && (right == end || !less(order[right], order[left]))) scratch[out] = order[left++];
        else scratch[out] = order[right++];
      }
    }
    order.swap(scratch);
  }
  for (auto& index : order) index = records[index].index;
  return order;
}
enum class FileVisualLane { kCache, kRaster, kPdf, kVideo };
constexpr std::array kFileIconRetryDelays = {
    base::Milliseconds(250), base::Seconds(1), base::Seconds(3)};
bool UsesApplicationIcon(const base::FilePath& path, bool directory) {
#if BUILDFLAG(IS_MAC)
  return directory && path.MatchesExtension(FILE_PATH_LITERAL(".app"));
#else
  return false;
#endif
}
struct FileVisualRequest {
  base::FilePath path;
  std::string mime;
  bool directory;
  int icon_index;
  bool icon;
  bool thumbnail;
  bool probe = false;
  bool large = false;
  std::shared_ptr<std::atomic_bool> canceled = nullptr;
};
struct FileVisualResult {
  FileVisualRequest request;
  gfx::ImageSkia icon;
  gfx::ImageSkia thumbnail;
  bool retry_icon = false;
};
struct FileRowRange {
  uint64_t model_generation;
  uint64_t row_generation;
  size_t first, last;
  int width, row_height;
  size_t keep_first = 0, keep_last = 0;
  bool operator==(const FileRowRange&) const = default;
};
struct FileVisualQueueState {
  FileRowRange rows;
  std::pair<size_t, size_t> visible;
  int icon_index;
  bool icons;
  uint64_t metadata_generation;
  bool operator==(const FileVisualQueueState&) const = default;
};
std::vector<FileVisualResult> LoadFileVisuals(std::vector<FileVisualRequest> batch,
                                            std::shared_ptr<std::atomic_bool> canceled) {
  std::vector<FileVisualResult> results;
  for (const auto& item : batch) {
    if (canceled->load(std::memory_order_relaxed)) break;
    FileVisualResult result{item, {}, {}};
    auto stopped = [&] { return canceled->load(std::memory_order_relaxed) ||
        item.canceled->load(std::memory_order_relaxed); };
    if (item.icon && !stopped()) {
#if BUILDFLAG(IS_MAC)
      if (UsesApplicationIcon(item.path, item.directory))
        result.icon = LoadApplicationIconMac(item.path, kFileIconSizes[item.icon_index]);
#endif
      if (result.icon.isNull() && !stopped()) {
        auto icon = LoadFileIcon(item.path, item.mime, item.directory, kFileIconSizes[item.icon_index]);
        result.icon = std::move(icon.image);
        result.retry_icon = icon.retry;
      }
    }
    if (item.thumbnail && !stopped())
      result.thumbnail = item.probe ? FindCachedThumbnail(item.path, kFileIconSizes.back(), item.canceled) :
          LoadCachedThumbnail(item.path, kFileIconSizes.back(), item.canceled);
    // Return canceled items too, so the UI releases their pending reservations.
    results.push_back(std::move(result));
  }
  return results;
}
struct DirectoryDelta {
  std::map<base::FilePath, size_t> positions;
  std::set<base::FilePath> unchanged;
  std::vector<size_t> display_positions;
  std::array<std::vector<size_t>, 26> initials;
  bool equal = false;
  std::optional<size_t> DisplayPosition(const base::FilePath& path) const {
    const auto found = positions.find(path);
    if (found == positions.end() || found->second >= display_positions.size()) return std::nullopt;
    const size_t position = display_positions[found->second];
    return position == std::numeric_limits<size_t>::max() ? std::nullopt : std::make_optional(position);
  }
};
struct DirectoryResult {
  base::FilePath path;
  DirectoryEntries entries;
  bool ok = false;
  std::shared_ptr<FileCacheEpoch> epoch;
  uint64_t revision = 0;
  FileSort sort = FileSort::kNameAscending;
  bool hidden = false;
  SnapshotVector<size_t> order;
  std::shared_ptr<const DirectoryDelta> delta;
  uint64_t base_generation = 0;
  size_t metadata_bytes = 0;
  bool Current() const { return epoch && epoch->Matches(revision); }
};
// Bounded UI-sequence snapshots. Each snapshot observes only its filesystem
// subtree, independently of History writes or unrelated file operations.
class DirectoryCache {
 public:
  std::optional<DirectoryResult> Get(const base::FilePath& path) {
    auto found = std::ranges::find(items_, path, [](const Item& item) { return item.result.path; });
    if (found == items_.end()) return std::nullopt;
    if (!found->result.Current() || base::TimeTicks::Now() - found->time > base::Seconds(30)) {
      Erase(found); return std::nullopt;
    }
    items_.splice(items_.begin(), items_, found);
    return items_.front().result;
  }
  void Remove(const base::FilePath& path) {
    auto found = std::ranges::find(items_, path, [](const Item& item) { return item.result.path; });
    if (found != items_.end()) Erase(found);
  }
  void Put(const DirectoryResult& result) {
    auto found = std::ranges::find(items_, result.path, [](const Item& item) { return item.result.path; });
    if (found != items_.end()) Erase(found);
    if (!result.ok || !result.Current() || IsDrivesLocation(result.path) || result.path == TrashRoot()) return;
    const size_t bytes = sizeof(Item) + result.metadata_bytes + result.order.capacity() * sizeof(size_t);
    if (bytes > kMaxBytes || result.entries.size() > kMaxEntries) return;
    while (!items_.empty() && (items_.size() >= 32 || bytes_ + bytes > kMaxBytes ||
           entries_ + result.entries.size() > kMaxEntries)) Erase(std::prev(items_.end()));
    items_.push_front({result, base::TimeTicks::Now(), bytes});
    items_.front().result.delta.reset();
    bytes_ += bytes; entries_ += result.entries.size();
  }
 private:
  struct Item { DirectoryResult result; base::TimeTicks time; size_t bytes; };
  static constexpr size_t kMaxBytes = 16 * 1024 * 1024;
  static constexpr size_t kMaxEntries = 20000;
  void Erase(std::list<Item>::iterator item) {
    bytes_ -= item->bytes; entries_ -= item->result.entries.size(); items_.erase(item);
  }
  std::list<Item> items_;
  size_t bytes_ = 0, entries_ = 0;
};
DirectoryCache& CachedDirectories() {
  static base::NoDestructor<DirectoryCache> cache;
  return *cache;
}
bool SameMetadata(const Entry& a, const Entry& b) {
  return a.path == b.path && a.directory == b.directory &&
      a.size == b.size && a.modified == b.modified && a.mime == b.mime && a.hidden == b.hidden &&
      a.created == b.created && a.permissions == b.permissions && a.package == b.package &&
      a.tag == b.tag && a.tag_name == b.tag_name && a.tag_count == b.tag_count;
}
DirectoryResult CompareDirectory(DirectoryEntries previous, std::set<base::FilePath> removed,
    uint64_t generation, DirectoryResult result, CancelFlag canceled) {
  if (!result.ok) { result.entries.clear(); result.order.clear(); }
  auto delta = std::make_unique<DirectoryDelta>();
  std::map<base::FilePath, const Entry*> live;
  for (const auto& entry : previous) {
    if (Stopped(canceled)) return result;
    if (!removed.contains(entry.path)) live.emplace(entry.path, &entry);
  }
  for (size_t index = 0; index < result.entries.size(); ++index) {
    if (Stopped(canceled)) return result;
    const auto& entry = result.entries[index];
    delta->positions.emplace(entry.path, index);
    const auto old = live.find(entry.path);
    if (old != live.end() && SameMetadata(*old->second, entry)) delta->unchanged.insert(entry.path);
  }
  delta->equal = result.ok && live.size() == result.entries.size() && delta->unchanged.size() == live.size();
  if (!delta->equal) {
    delta->display_positions.resize(result.entries.size(), std::numeric_limits<size_t>::max());
    for (size_t position = 0; position < result.order.size(); ++position) {
      if (Stopped(canceled)) return result;
      const auto index = result.order[position];
      delta->display_positions[index] = position;
      const auto& name = result.entries[index].name_key;
      if (!name.empty() && name.front() >= u'a' && name.front() <= u'z')
        delta->initials[name.front() - u'a'].push_back(position);
    }
  }
  result.delta = ShareDirectoryData(std::move(*delta)); result.base_generation = generation;
  return result;
}
DirectoryResult ReadDirectoryEntries(base::FilePath path, const CancelFlag& canceled) {
  DirectoryResult result;
  std::vector<Entry> entries;
  auto finish = [&]() {
    result.metadata_bytes = entries.capacity() * sizeof(Entry) + result.path.value().capacity() * sizeof(base::FilePath::CharType);
    for (const auto& entry : entries) result.metadata_bytes += entry.path.value().capacity() * sizeof(base::FilePath::CharType) +
        entry.mime.capacity() + entry.name_key.capacity() * sizeof(char16_t);
    result.entries = DirectoryEntries(std::move(entries));
    return std::move(result);
  };
  result.path = path;
  result.epoch = ObserveFileCachePath(path, true); result.revision = result.epoch->revision.load();
  if (Stopped(canceled)) return finish();
#if BUILDFLAG(IS_WIN)
  if (IsDrivesLocation(path)) {
    const DWORD drives = GetLogicalDrives();
    const DWORD last_error_hint = drives ? ERROR_SUCCESS : GetLastError();
    LOG(INFO) << "[crFiles /] enumerate mask=" << drives << " last_error_hint=" << last_error_hint;
    for (int i = 0; i < 26; ++i) {
      if (Stopped(canceled)) return finish();
      if (!(drives & (DWORD{1} << i))) continue;
      std::wstring name = L"A:\\";
      name.front() = static_cast<wchar_t>(L'A' + i);
      entries.push_back({base::FilePath(name), true, 0, {}, "inode/directory"});
      LOG(INFO) << "[crFiles /] entry path=" << entries.back().path.AsUTF8Unsafe()
          << " directory=" << entries.back().directory << " mime=" << entries.back().mime;
    }
    result.ok = drives != 0;
    LOG(INFO) << "[crFiles /] enumerate_result ok=" << result.ok << " count=" << entries.size();
    return finish();
  }
#endif
  if (path == TrashRoot() && !path.empty()) {
    if (!base::CreateDirectory(path)) return finish();
    result.path = path;
    base::FileEnumerator items(path, false, base::FileEnumerator::DIRECTORIES);
    for (auto item = items.Next(); !item.empty(); item = items.Next()) {
      if (Stopped(canceled)) return finish();
      auto contents = ReadDirectoryEntries(item.AppendASCII("files"), canceled);
      for (const auto& entry : contents.entries) entries.push_back(entry);
    }
    result.ok = items.GetError() == base::File::FILE_OK;
    return finish();
  }
  const auto absolute = base::MakeAbsoluteFilePath(path);
  if (absolute.empty() || !base::DirectoryExists(absolute)) return finish();
  result.path = absolute;
  result.epoch = ObserveFileCachePath(absolute, true); result.revision = result.epoch->revision.load();
  int file_types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES;
#if BUILDFLAG(IS_POSIX)
  // Enumerate the link itself so a missing target does not hide the entry.
  file_types |= base::FileEnumerator::SHOW_SYM_LINKS;
#endif
  base::FileEnumerator files(result.path, false, file_types,
      base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (auto next = files.Next(); !next.empty(); next = files.Next()) {
    if (Stopped(canceled)) return finish();
    const auto info = files.GetInfo();
    bool directory = info.IsDirectory();
    int64_t size = info.GetSize();
    base::Time modified = info.GetLastModifiedTime();
#if BUILDFLAG(IS_POSIX)
    if (S_ISLNK(info.stat().st_mode)) {
      // Preserve navigation and metadata for valid directory/file links, but
      // fall back to lstat metadata for dangling or inaccessible targets.
      base::File::Info target;
      if (base::GetFileInfo(next, &target)) {
        directory = target.is_directory;
        size = target.size;
        modified = target.last_modified;
      }
    }
#endif
    std::string mime = "inode/directory";
    if (!directory && !net::GetMimeTypeFromFile(next, &mime)) mime = "application/octet-stream";
    bool hidden = next.BaseName().AsUTF8Unsafe().starts_with(".");
#if BUILDFLAG(IS_WIN)
    const DWORD attributes = GetFileAttributesW(next.value().c_str());
    hidden |= attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN);
#endif
    entries.push_back({next, directory, size, modified,
                              mime, hidden});
    ReadExtraMetadata(entries.back(), &info);
#if BUILDFLAG(IS_MAC)
    entries.back().package = directory && IsFilePackageMac(next);
#endif
  }
  result.ok = files.GetError() == base::File::FILE_OK;
  return finish();
}
DirectoryResult ReadDirectory(base::FilePath path, FileSort sort, bool hidden, CancelFlag canceled) {
  auto result = ReadDirectoryEntries(path, canceled);
  result.sort = sort; result.hidden = hidden;
  if (result.ok && !Stopped(canceled))
    result.order = SortDirectory(SortRecords(result.entries, canceled), sort, hidden, result.path == TrashRoot(), canceled);
  return result;
}
// UI-owned queue. A job may have multiple panes waiting on the same path;
// cancellation of one pane never cancels the other subscribers' read.
class DirectoryReadQueue {
 public:
  using Reply = base::OnceCallback<void(DirectoryResult)>;
  void Request(base::FilePath path, FileSort sort, bool hidden, CancelFlag subscriber, Reply reply) {
    Prune(); path = path.StripTrailingSeparators();
    for (auto& job : jobs_) if (job->path == path && !Stopped(job->canceled)) {
      job->subscribers.push_back({std::move(subscriber), std::move(reply)}); Pump(); return;
    }
    auto job = std::make_shared<Job>(); job->path = std::move(path); job->sort = sort; job->hidden = hidden;
    job->subscribers.push_back({std::move(subscriber), std::move(reply)});
    jobs_.push_back(std::move(job)); Pump();
  }
  void Prune() {
    for (auto& job : jobs_) {
      std::erase_if(job->subscribers, [](const Subscriber& item) { return Stopped(item.canceled); });
      if (job->subscribers.empty()) job->canceled->store(true);
    }
    std::erase_if(jobs_, [](const auto& job) { return !job->running && Stopped(job->canceled); });
  }
 private:
  struct Subscriber { CancelFlag canceled; Reply reply; };
  struct Job {
    base::FilePath path;
    FileSort sort;
    bool hidden = false, running = false;
    CancelFlag canceled = std::make_shared<std::atomic_bool>(false);
    std::vector<Subscriber> subscribers;
  };
  void Pump() {
    Prune();
    for (auto& job : jobs_) {
      if (running_ >= 2) break;
      if (job->running || Stopped(job->canceled)) continue;
      job->running = true; ++running_;
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&ReadDirectory, job->path, job->sort, job->hidden, job->canceled),
          base::BindOnce(&DirectoryReadQueue::Finished, base::Unretained(this), job));
    }
  }
  void Finished(std::shared_ptr<Job> job, DirectoryResult result) {
    --running_; std::erase(jobs_, job);
    for (auto& subscriber : job->subscribers)
      if (!Stopped(subscriber.canceled)) std::move(subscriber.reply).Run(result);
    Pump();
  }
  size_t running_ = 0;
  std::vector<std::shared_ptr<Job>> jobs_;
};
DirectoryReadQueue& DirectoryReads() { static base::NoDestructor<DirectoryReadQueue> queue; return *queue; }
// Owned and destroyed on one blocking sequence; notifications are coalesced
// before crossing to the UI. Each visible directory has one shared watcher.
class LiveDirectoryWatch {
 public:
  LiveDirectoryWatch(base::FilePath path, base::RepeatingCallback<void(base::FilePath)> changed)
      : path_(std::move(path)), changed_(std::move(changed)) {
    const bool ok = watcher_.Watch(path_, base::FilePathWatcher::Type::kNonRecursive,
        base::BindRepeating(&LiveDirectoryWatch::Changed, base::Unretained(this)));
    if (!ok) StartFallback();
    if (path_ == TrashRoot()) StartFallback();
#if BUILDFLAG(IS_APPLE)
    // kqueue does not report content changes to files inside a directory.
    StartFallback();
#endif
  }
 private:
  void StartFallback() {
    if (!fallback_.IsRunning()) fallback_.Start(FROM_HERE, base::Seconds(5), this, &LiveDirectoryWatch::Flush);
  }
  void Changed(const base::FilePath&, bool error) {
    if (error) StartFallback();
    if (!coalesce_.IsRunning()) coalesce_.Start(FROM_HERE, base::Milliseconds(200), this, &LiveDirectoryWatch::Flush);
  }
  void Flush() {
    { ScopedFileCacheMutation invalidate({path_}); }
    changed_.Run(path_);
  }
  base::FilePath path_;
  base::RepeatingCallback<void(base::FilePath)> changed_;
  base::FilePathWatcher watcher_;
  base::OneShotTimer coalesce_;
  base::RepeatingTimer fallback_;
};
void AnimateButton(views::Button* button) {
  button->SetAnimateOnStateChange(true);
  button->SetAnimationDuration(base::Milliseconds(160));
  button->SetHasInkDropActionOnClick(true);
  // Checkboxes and radio buttons host their ink drop on an internal image view.
  auto* ink_view = button->ink_drop_view();
  if (ink_view == button)
    views::InstallRoundRectHighlightPathGenerator(button, gfx::Insets(), 8);
  auto* ink = views::InkDrop::Get(ink_view);
  ink->SetMode(views::InkDropHost::InkDropMode::ON);
  ink->SetBaseColor(ui::kColorSysPrimary);
  ink->SetVisibleOpacity(0.16f);
  ink->SetHighlightOpacity(0.09f);
  ink->GetInkDrop()->SetShowHighlightOnHover(true);
  ink->GetInkDrop()->SetShowHighlightOnFocus(true);
}
class SidebarHighlightPath : public views::HighlightPathGenerator {
 public:
  std::optional<gfx::RRectF> GetRoundRect(const gfx::RectF& rect) override {
    const float radius = rect.height() / 2.f;
    // Flat against the sidebar edge, rounded toward the file content.
    return gfx::RRectF(rect, 0, 0, radius, radius, radius, radius, 0, 0);
  }
};
class SplitButtonHighlightPath : public views::HighlightPathGenerator {
 public:
  explicit SplitButtonHighlightPath(bool leading) : leading_(leading) {}
  std::optional<gfx::RRectF> GetRoundRect(const gfx::RectF& rect) override {
    const float radius = rect.height() / 2.f;
    const bool round_left = leading_ != base::i18n::IsRTL();
    const float left = round_left ? radius : 0.f;
    const float right = round_left ? 0.f : radius;
    return gfx::RRectF(rect, left, left, right, right, right, right, left, left);
  }
 private:
  bool leading_;
};
class ViewControlsCapsule : public views::View {
  METADATA_HEADER(ViewControlsCapsule, views::View)
 public:
  ViewControlsCapsule() {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
    SetBackground(views::CreateRoundedRectBackground(ui::kColorSysSurface5, 18));
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    views::View::OnPaintBackground(canvas);
    if (children().size() != 2 || height() <= 0) return;
    const float divider_height = std::min(12.f, static_cast<float>(height()));
    const float x = GetMirroredXInView(children().front()->bounds().right());
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysNeutralOutline));
    canvas->DrawRoundRect(gfx::RectF(x - 0.5f, (height() - divider_height) / 2.f,
        1.f, divider_height), 0.5f, flags);
  }
};
BEGIN_METADATA(ViewControlsCapsule)
END_METADATA
class SidebarButton : public views::LabelButton {
  METADATA_HEADER(SidebarButton, views::LabelButton)
 public:
  SidebarButton(base::RepeatingClosure callback, std::u16string title)
      : views::LabelButton(std::move(callback), title) {
    AnimateButton(this);
    // Use regular text consistently, including translucent bookmark animations.
    label()->SetFontList(label()->font_list().DeriveWithWeight(gfx::Font::Weight::NORMAL));
    label()->SetSubpixelRenderingEnabled(false);
    SetTooltipText(title);
    SetFocusBehavior(views::View::FocusBehavior::NEVER);
    SetInstallFocusRingOnFocus(false);
    SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(8, 22, 8, 14)));
    views::HighlightPathGenerator::Install(this, std::make_unique<SidebarHighlightPath>());
  }
  void SetSelected(bool selected) {
    selected_ = selected;
    SetEnabledTextColors(selected ? ui::kColorSysOnSecondaryContainer : ui::kColorSysOnSurface);
    SchedulePaint();
  }
  void SetFeedback(float value) { feedback_ = value; SchedulePaint(); }
  void SetMenuHighlight(bool value) { menu_highlight_ = value; SchedulePaint(); }
  void UseStableHoverHighlight() {
    stable_hover_ = true;
    views::InkDrop::Get(this)->GetInkDrop()->SetShowHighlightOnHover(false);
  }
  void OnMouseEntered(const ui::MouseEvent& event) override {
    hovered_ = true;
    views::LabelButton::OnMouseEntered(event);
    if (stable_hover_) SchedulePaint();
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_ = false;
    views::LabelButton::OnMouseExited(event);
    if (stable_hover_) SchedulePaint();
  }
  void SetBreathing(bool enabled) {
    if (enabled == breathing_timer_.IsRunning()) return;
    if (enabled) {
      breathing_start_ = base::TimeTicks::Now();
      breathing_alpha_ = 0.35f;
      breathing_timer_.Start(FROM_HERE, base::Milliseconds(33), this,
                            &SidebarButton::AnimateBreathing);
    } else {
      breathing_timer_.Stop();
      breathing_alpha_ = 0.f;
    }
    SchedulePaint();
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    if ((menu_highlight_ || (stable_hover_ && hovered_)) && !selected_) {
      cc::PaintFlags highlight;
      highlight.setAntiAlias(true);
      highlight.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
      highlight.setAlphaf(0.09f);
      SidebarHighlightPath path;
      canvas->DrawPath(path.GetHighlightPath(this), highlight);
    }
    if (!selected_ && feedback_ <= 0.f) return;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetColorProvider()->GetColor(
        selected_ ? ui::kColorSysSecondaryContainer : ui::kColorSysPrimaryContainer));
    SidebarHighlightPath path;
    if (!selected_) flags.setAlphaf(feedback_);
    if (selected_ || feedback_ > 0.f)
      canvas->DrawPath(path.GetHighlightPath(this), flags);
    if (selected_ && feedback_ > 0.f) {
      flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
      flags.setAlphaf(feedback_ * 0.14f);
      canvas->DrawPath(path.GetHighlightPath(this), flags);
    }
  }
  void OnPaintBorder(gfx::Canvas* canvas) override {
    views::LabelButton::OnPaintBorder(canvas);
    if (breathing_alpha_ <= 0.f || width() <= 2 || height() <= 2) return;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(2.f);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
    flags.setAlphaf(breathing_alpha_);
    // Keep the entire stroke inside the view so its edges are not clipped.
    gfx::RectF bounds(GetLocalBounds());
    bounds.Inset(1.f);
    SidebarHighlightPath path;
    const auto outline = path.GetRoundRect(bounds);
    canvas->DrawPath(SkPath::RRect(SkRRect{*outline}), flags);
  }
 private:
  void AnimateBreathing() {
    const double phase = (base::TimeTicks::Now() - breathing_start_).InSecondsF() / 2.0;
    breathing_alpha_ = 0.35f + 0.60f * static_cast<float>(
        (1.0 - std::cos(phase * 6.283185307179586)) / 2.0);
    if (IsDrawn()) SchedulePaint();
  }
  bool selected_ = false;
  bool menu_highlight_ = false;
  bool stable_hover_ = false, hovered_ = false;
  float feedback_ = 0.f;
  float breathing_alpha_ = 0.f;
  base::TimeTicks breathing_start_;
  base::RepeatingTimer breathing_timer_;
};
BEGIN_METADATA(SidebarButton)
END_METADATA
class BookmarkButton : public SidebarButton {
  METADATA_HEADER(BookmarkButton, SidebarButton)
 public:
  using ReorderCallback = base::RepeatingCallback<void(gfx::Point, bool, bool)>;
  BookmarkButton(base::RepeatingClosure open, std::u16string title, ReorderCallback reorder)
      : SidebarButton(std::move(open), std::move(title)), reorder_(std::move(reorder)) {
    UseStableHoverHighlight();
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    pressed_ = event.IsOnlyLeftMouseButton(); dragging_ = false; origin_ = event.location();
    return SidebarButton::OnMousePressed(event);
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (pressed_ && (dragging_ || ExceededDragThreshold(event.location() - origin_))) {
      if (!dragging_) CancelDragPressFeedback();
      dragging_ = true;
      auto point = event.location(); views::View::ConvertPointToScreen(this, &point);
      reorder_.Run(point, false, false);
      return true;
    }
    return SidebarButton::OnMouseDragged(event);
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    pressed_ = false;
    if (dragging_) {
      dragging_ = false;
      CancelDragPressFeedback();
      auto point = event.location(); views::View::ConvertPointToScreen(this, &point);
      reorder_.Run(point, true, false);
      return;
    }
    SidebarButton::OnMouseReleased(event);
  }
  void OnMouseCaptureLost() override {
    pressed_ = false;
    if (dragging_) {
      dragging_ = false;
      CancelDragPressFeedback();
      reorder_.Run({}, true, true);
    }
    SidebarButton::OnMouseCaptureLost();
  }
 private:
  void CancelDragPressFeedback() {
    // A reorder consumes release without running the button's click/cancel path.
    // Button state and the pending ink drop must therefore both be reset.
    SetState(STATE_NORMAL);
    views::InkDrop::Get(ink_drop_view())->AnimateToState(
        views::InkDropState::HIDDEN, nullptr);
  }
  ReorderCallback reorder_;
  gfx::Point origin_;
  bool pressed_ = false, dragging_ = false;
};
BEGIN_METADATA(BookmarkButton)
END_METADATA
class BookmarkDragPreview : public views::LabelButton {
  METADATA_HEADER(BookmarkDragPreview, views::LabelButton)
 public:
  explicit BookmarkDragPreview(const std::u16string& title)
      : views::LabelButton(base::RepeatingClosure(), title) {
    SetProperty(views::kViewIgnoredByLayoutKey, true);
    SetImageModel(STATE_NORMAL, ui::ImageModel::FromVectorIcon(vector_icons::kFolderOpenIcon,
        ui::kColorSysPrimary, 24));
    SetImageLabelSpacing(8);
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(10, 14)));
    SetEnabledTextColors(ui::kColorSysOnSurface);
    label()->SetSubpixelRenderingEnabled(false);
    SetCanProcessEventsWithinSubtree(false);
    SetPaintToLayer(); layer()->SetFillsBoundsOpaquely(false);
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    cc::PaintFlags flags; flags.setAntiAlias(true);
    auto bounds = GetLocalBounds(); bounds.Inset(3);
    auto shadow = bounds; shadow.Offset(0, 2);
    flags.setColor(SkColorSetARGB(28, 0, 0, 0));
    const int radius = std::min(bounds.width(), bounds.height()) / 2;
    canvas->DrawRoundRect(shadow, radius, flags);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysSurface));
    canvas->DrawRoundRect(bounds, radius, flags);
  }
};
BEGIN_METADATA(BookmarkDragPreview)
END_METADATA
class PropertiesSizeLabel : public views::Label, public views::WidgetObserver {
  METADATA_HEADER(PropertiesSizeLabel, views::Label)
 public:
  explicit PropertiesSizeLabel(std::vector<base::FilePath> paths)
      : progress_(std::make_shared<PropertiesSizeProgress>()) {
    SetHorizontalAlignment(gfx::ALIGN_LEFT);
    SetSelectable(true);
    SetMultiLine(true);
    Refresh();
    timer_.Start(FROM_HERE, base::Milliseconds(200), this, &PropertiesSizeLabel::Refresh);
    base::ThreadPool::PostTask(FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE, base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
        base::BindOnce(&CalculatePropertiesSize, std::move(paths), progress_));
  }
  ~PropertiesSizeLabel() override { Cancel(); }
  void AddedToWidget() override {
    views::Label::AddedToWidget();
    observation_.Observe(GetWidget());
  }
  void RemovedFromWidget() override {
    Cancel();
    observation_.Reset();
    views::Label::RemovedFromWidget();
  }
  void OnWidgetClosing(views::Widget*) override { Cancel(); }
  void OnWidgetDestroying(views::Widget*) override {
    Cancel();
    observation_.Reset();
  }
 private:
  void Cancel() {
    progress_->canceled.store(true, std::memory_order_relaxed);
    timer_.Stop();
  }
  void Refresh() {
    const bool done = progress_->done.load(std::memory_order_acquire);
    const bool incomplete = progress_->incomplete.load(std::memory_order_relaxed);
    auto text = Text(incomplete ? IDS_CRFILES_SIZE_AT_LEAST : IDS_CRFILES_SIZE) + u": " +
        ui::FormatBytes(base::ByteSize(progress_->bytes.load(std::memory_order_relaxed)));
    if (!done) text += u" · " + Text(IDS_CRFILES_CALCULATING_SIZE);
    if (GetText() != text) SetText(text);
    if (incomplete && !incomplete_shown_) {
      SetTooltipText(Text(IDS_CRFILES_SIZE_INCOMPLETE_DETAIL));
      incomplete_shown_ = true;
    }
    if (done) timer_.Stop();
  }
  std::shared_ptr<PropertiesSizeProgress> progress_;
  bool incomplete_shown_ = false;
  base::RepeatingTimer timer_;
  base::ScopedObservation<views::Widget, views::WidgetObserver> observation_{this};
};
BEGIN_METADATA(PropertiesSizeLabel)
END_METADATA
void ShowFileProperties(views::Widget* parent,
                        const std::vector<base::FilePath>& paths,
                        const std::vector<Entry>& entries,
                        bool calculate_directory_size, OpenTagCallback open_tag) {
  auto dialog = std::make_unique<views::DialogDelegate>(); dialog->SetTitle(Text(IDS_CRFILES_PROPERTIES));
  dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
  auto contents = std::make_unique<views::View>();
  contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(20), 10));
  contents->SetPreferredSize(gfx::Size(520, 340));
  auto add = [&](std::u16string text) { auto* label = contents->AddChildView(std::make_unique<views::Label>(text)); label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true); label->SetSelectable(true); };
  add(base::NumberToString16(paths.size()) + u" " + Text(IDS_CRFILES_ITEMS));
  if (entries.empty()) {
    if (paths.size() == 1) add(paths.front().AsUTF16Unsafe());
    add(Text(IDS_CRFILES_READ_ERROR));
  }
  int64_t bytes = 0; bool has_directory = false;
  for (const auto& path : paths) {
    const auto entry = std::ranges::find(entries, path, &Entry::path);
    if (entry == entries.end()) continue;
    has_directory |= entry->directory;
    if (!entry->directory && entry->size > 0) bytes += std::min(entry->size, std::numeric_limits<int64_t>::max() - bytes);
    if (paths.size() == 1) {
      add(path.AsUTF16Unsafe()); add(Text(IDS_CRFILES_TYPE) + u": " + (entry->directory ? Text(IDS_CRFILES_FOLDER) : base::UTF8ToUTF16(entry->mime)));
      if (!entry->modified.is_null()) add(Text(IDS_CRFILES_LAST_MODIFIED) + u": " + base::TimeFormatShortDateAndTime(entry->modified));
      add(Text(IDS_CRFILES_CREATED) + u": " + CreatedText(entry->created));
      add(Text(IDS_CRFILES_PERMISSIONS) + u": " + PermissionsText(entry->permissions));
    }
  }
  if (has_directory && calculate_directory_size) {
    contents->AddChildView(std::make_unique<PropertiesSizeLabel>(paths));
  } else {
    auto* size_label = contents->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_SIZE) + u": " +
      ((has_directory || entries.empty()) ? std::u16string(u"—") : ui::FormatBytes(base::ByteSize(static_cast<uint64_t>(bytes))))));
    size_label->SetHorizontalAlignment(gfx::ALIGN_LEFT); size_label->SetSelectable(true);
  }
  if (!paths.empty() && std::ranges::all_of(paths, [](const auto& path) { return path.IsAbsolute() && !IsDrivesLocation(path); })) {
    contents->AddChildView(CreateTagsPropertyView(paths, std::move(open_tag)));
    contents->SetPreferredSize(gfx::Size(520, 440));
  }
  dialog->SetContentsView(std::move(contents));
  ShowCrFilesDialog(std::move(dialog), parent);
}

// Sidebar targets are not necessarily present in the current directory listing.
// Read their metadata off the UI thread, then use the same properties layout.
void ShowFolderProperties(views::Widget* parent, base::FilePath path, OpenTagCallback open_tag) {
  if (!parent || parent->IsClosed() || path.empty()) return;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce([](base::FilePath path) {
        std::vector<Entry> entries;
        base::File::Info info;
        if (base::GetFileInfo(path, &info)) {
          std::string mime;
          if (!info.is_directory && !net::GetMimeTypeFromFile(path, &mime))
            mime = "application/octet-stream";
          entries.push_back({path, info.is_directory, info.size, info.last_modified, mime});
          ReadExtraMetadata(entries.back());
        }
        return entries;
      }, path),
      base::BindOnce([](base::WeakPtr<views::Widget> parent, base::FilePath path, OpenTagCallback open_tag,
                        std::vector<Entry> entries) {
        if (parent && !parent->IsClosed()) ShowFileProperties(parent.get(), {path}, entries, true, std::move(open_tag));
      }, parent->GetWeakPtr(), path, std::move(open_tag)));
}
class BookmarksView : public views::View, public views::ContextMenuController, public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(BookmarksView, views::View)
 public:
  void BookmarkPaths(std::vector<base::FilePath> paths) { AddPaths(entries_.size(), std::move(paths)); }
  BookmarksView(base::RepeatingCallback<void(base::FilePath)> open,
                base::RepeatingCallback<void(base::FilePath)> new_tab, OpenTagCallback open_tag)
      : open_(std::move(open)), new_tab_(std::move(new_tab)), open_tag_(std::move(open_tag)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(8, 0), 6));
    auto stored = GetCrFilesBookmarks();
    if (const auto* items = stored.FindList("items")) {
      for (const auto& value : *items) {
        const auto* item = value.GetIfDict(); if (!item) continue;
        const auto* path = item->FindString("path"); if (!path) continue;
        auto folder = base::FilePath::FromUTF8Unsafe(*path);
#if BUILDFLAG(IS_MAC)
        const int media_kind = item->FindInt("kind").value_or(0);
        if (media_kind == 1 || media_kind == 2) {
          const auto system_folder = MediaFolder(media_kind == 1);
          if (!system_folder.empty()) folder = system_folder;
        }
#endif
        if (!folder.IsAbsolute() || folder.ReferencesParent()) continue;
        const auto* name = item->FindString("name");
        entries_.push_back({folder, name ? *name : std::string(), item->FindInt("kind").value_or(0)});
        if (entries_.size() >= 256) break;
      }
    } else {
      entries_.push_back({MediaFolder(true), {}, 1});
      entries_.push_back({MediaFolder(false), {}, 2});
    }
    Rebuild();
  }
  ~BookmarksView() override { motion_timer_.Stop(); CloseDragPreview(); }
  bool SetCurrentPath(const base::FilePath& path) {
    current_ = path;
    bool matched = false;
    for (size_t i = 0; i < buttons_.size(); ++i) {
      const bool selected = !matched && entries_[i].path == path;
      buttons_[i]->SetSelected(selected); matched |= selected;
    }
    return matched;
  }
  bool GetDropFormats(int* formats, std::set<ui::ClipboardFormatType>*) override {
    *formats = ui::OSExchangeData::FILE_NAME; return true;
  }
  bool CanDrop(const ui::OSExchangeData& data) override {
    const auto files = data.GetFilenames();
    return files && !files->empty() && files->size() <= 256;
  }
  int OnDragUpdated(const ui::DropTargetEvent& event) override {
    const int operation = DropOperation(event);
    external_drop_highlight_ = true;
    insertion_ = buttons_.size();
    for (size_t i = 0; i < buttons_.size(); ++i) {
      if (event.y() < buttons_[i]->bounds().CenterPoint().y()) { insertion_ = i; break; }
    }
    SetDropHighlight(operation != 0);
    SchedulePaint();
    return operation;
  }
  void OnDragExited() override { SetDropHighlight(false); }
  DropCallback GetDropCallback(const ui::DropTargetEvent&) override {
    return base::BindOnce(&BookmarksView::Drop, weak_.GetWeakPtr(), insertion_);
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    views::View::OnPaintBackground(canvas);
    if (drop_alpha_ <= 0.f) return;
    cc::PaintFlags flags; flags.setAntiAlias(true);
    if (external_drop_highlight_) {
      flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimaryContainer));
      flags.setAlphaf(drop_alpha_ * 0.35f);
      canvas->DrawRoundRect(GetLocalBounds(), 8, flags);
    }
    const int y = insertion_ < buttons_.size() ? buttons_[insertion_]->y() - 3 :
        buttons_.empty() ? 8 : buttons_.back()->bounds().bottom() + 3;
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary)); flags.setAlphaf(drop_alpha_);
    canvas->DrawRoundRect(gfx::Rect(12, y - 1, std::max(0, width() - 24), 2), 1, flags);
  }
  void Layout(PassKey) override {
    LayoutSuperclass<views::View>(this);
    if (!animate_layout_) return;
    animate_layout_ = false;
    for (size_t i = 0; i < buttons_.size(); ++i) {
      auto* button = buttons_[i].get();
      auto old = old_positions_.find(entries_[i].path);
      gfx::Transform offset;
      offset.Translate(0, old == old_positions_.end() ? 6 : old->second - button->y());
      auto* layer = button->layer();
      layer->GetAnimator()->StopAnimating(); layer->SetTransform(offset);
      layer->SetOpacity(old == old_positions_.end() ? 0.f : 1.f);
      ui::ScopedLayerAnimationSettings animation(layer->GetAnimator());
      animation.SetTransitionDuration(base::Milliseconds(160)); animation.SetTweenType(gfx::Tween::EASE_OUT);
      layer->SetTransform(gfx::Transform()); layer->SetOpacity(1.f);
    }
    old_positions_.clear();
    RevealFeedback();
  }
  void ShowContextMenuForViewImpl(views::View* source, const gfx::Point& point, ui::mojom::MenuSourceType type) override {
    const auto found = std::ranges::find_if(buttons_, [source](const auto& button) { return button.get() == source; });
    if (found == buttons_.end()) return;
    context_ = entries_[std::distance(buttons_.begin(), found)].path;
    runner_.reset(); ClearMenuHighlight();
    (*found)->SetMenuHighlight(true);
    menu_ = std::make_unique<ui::SimpleMenuModel>(this);
    menu_->AddItem(IDS_CRFILES_OPEN_NEW_TAB, Text(IDS_CRFILES_OPEN_NEW_TAB));
    menu_->AddSeparator(ui::NORMAL_SEPARATOR);
    menu_->AddItem(IDS_CRFILES_REMOVE_BOOKMARK, Text(IDS_CRFILES_REMOVE_BOOKMARK));
    menu_->AddItem(IDS_CRFILES_RENAME, Text(IDS_CRFILES_RENAME));
    menu_->AddSeparator(ui::NORMAL_SEPARATOR);
    menu_->AddItem(IDS_CRFILES_PROPERTIES, Text(IDS_CRFILES_PROPERTIES));
    runner_ = std::make_unique<views::MenuRunner>(menu_.get(), views::MenuRunner::CONTEXT_MENU,
        base::BindRepeating(&BookmarksView::ClearMenuHighlight, weak_.GetWeakPtr()));
    runner_->RunMenuAt(GetWidget(), nullptr, gfx::Rect(point, gfx::Size(1, 1)), views::MenuAnchorPosition::kTopLeft, type);
  }
  bool IsCommandIdEnabled(int) const override { return true; }
  void ExecuteCommand(int command, int) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&BookmarksView::Action, weak_.GetWeakPtr(), command, context_));
  }
 private:
  struct Bookmark { base::FilePath path; std::string name; int kind = 0; };
  void ClearMenuHighlight() {
    for (const auto& button : buttons_) button->SetMenuHighlight(false);
  }
  void CloseDragPreview() {
    if (auto* preview = drag_preview_.view()) {
      preview->parent()->RemoveChildViewT(preview);
      drag_preview_.SetView(nullptr);
    }
  }
  void AnimateReorder(const base::FilePath& path, size_t insertion, bool active) {
    auto found = std::ranges::find(entries_, path, &Bookmark::path);
    if (found == entries_.end()) return;
    const size_t from = std::distance(entries_.begin(), found);
    size_t to = std::min(insertion, entries_.size());
    if (to > from) --to;
    const int step = buttons_[from]->height() + 6;
    for (size_t i = 0; i < buttons_.size(); ++i) {
      gfx::Transform transform;
      if (active && i > from && i <= to) transform.Translate(0, -step);
      else if (active && i >= to && i < from) transform.Translate(0, step);
      auto* layer = buttons_[i]->layer();
      if (layer->GetTargetTransform() == transform &&
          layer->GetTargetOpacity() == (active && i == from ? 0.32f : 1.f)) continue;
      ui::ScopedLayerAnimationSettings animation(layer->GetAnimator());
      animation.SetTransitionDuration(base::Milliseconds(160));
      animation.SetTweenType(gfx::Tween::EASE_OUT);
      animation.SetPreemptionStrategy(ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
      layer->SetTransform(transform);
      layer->SetOpacity(active && i == from ? 0.32f : 1.f);
    }
  }
  void ReorderBookmark(base::FilePath path, gfx::Point point, bool finished, bool cancelled) {
    external_drop_highlight_ = false;
    if (cancelled) { CloseDragPreview(); AnimateReorder(path, 0, false); SetDropHighlight(false); return; }
    const auto screen_point = point;
    views::View::ConvertPointFromScreen(this, &point);
    insertion_ = buttons_.size();
    for (size_t i = 0; i < buttons_.size(); ++i) {
      if (point.y() < buttons_[i]->bounds().CenterPoint().y()) { insertion_ = i; break; }
    }
    const bool inside = GetVisibleBounds().Contains(point);
    SetDropHighlight(inside && !finished); SchedulePaint();
    if (!finished) {
      const auto found = std::ranges::find(entries_, path, &Bookmark::path);
      if (found == entries_.end()) return;
      const size_t source_index = std::distance(entries_.begin(), found);
      auto* root = GetWidget()->GetRootView();
      if (!drag_preview_.view()) {
        drag_preview_.SetView(root->AddChildView(std::make_unique<BookmarkDragPreview>(Name(*found))));
      }
      auto location = screen_point; views::View::ConvertPointFromScreen(root, &location);
      auto* preview = drag_preview_.view();
      const auto size = preview->GetPreferredSize();
      preview->SetBounds(location.x() + 6, location.y() + 6, buttons_[source_index]->width() * 9 / 10, size.height());
      AnimateReorder(path, insertion_, inside);
      if (inside) ScrollRectToVisible(gfx::Rect(point.x(), std::max(0, point.y() - 20), 1, 40));
      return;
    }
    CloseDragPreview();
    if (!inside) { AnimateReorder(path, 0, false); return; }
    // Commit rebuilds the buttons. Wait until mouse dispatch has returned.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&BookmarksView::MoveBookmark, weak_.GetWeakPtr(), path, insertion_));
  }
  void MoveBookmark(const base::FilePath& path, size_t insertion) {
    auto entries = entries_;
    const auto found = std::ranges::find(entries, path, &Bookmark::path);
    if (found == entries.end()) return;
    const size_t from = std::distance(entries.begin(), found);
    insertion = std::min(insertion, entries.size());
    if (insertion > from) --insertion;
    if (insertion == from) { AnimateReorder(path, 0, false); return; }
    auto moved = std::move(*found); entries.erase(found);
    entries.insert(entries.begin() + insertion, std::move(moved));
    if (Commit(std::move(entries))) Flash({path});
    else AnimateReorder(path, 0, false);
  }
  void StartMotionTimer() {
    if (!motion_timer_.IsRunning()) motion_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &BookmarksView::AnimateFeedback);
  }
  void SetDropHighlight(bool visible) {
    if (drop_visible_ == visible) return;
    drop_visible_ = visible; drop_start_alpha_ = drop_alpha_; drop_start_ = base::TimeTicks::Now();
    StartMotionTimer();
  }
  void AnimateFeedback() {
    const auto now = base::TimeTicks::Now();
    const float fraction = std::clamp(static_cast<float>((now - drop_start_).InMillisecondsF() /
        (drop_visible_ ? 120.f : 100.f)), 0.f, 1.f);
    drop_alpha_ = drop_start_alpha_ + ((drop_visible_ ? 1.f : 0.f) - drop_start_alpha_) * fraction;
    float flash = 0.f;
    if (!feedback_start_.is_null()) flash = std::clamp(1.f - static_cast<float>(
        (now - feedback_start_ - base::Milliseconds(500)).InMillisecondsF() / 180.f), 0.f, 1.f);
    for (size_t i = 0; i < buttons_.size(); ++i)
      buttons_[i]->SetFeedback(feedback_paths_.contains(entries_[i].path) ? flash : 0.f);
    SchedulePaint();
    if (fraction >= 1.f && flash == 0.f) { motion_timer_.Stop(); feedback_paths_.clear(); }
  }
  void RevealFeedback() {
    for (size_t i = 0; i < buttons_.size(); ++i) if (feedback_paths_.contains(entries_[i].path)) {
      ScrollRectToVisible(buttons_[i]->bounds()); break;
    }
  }
  void Flash(std::set<base::FilePath> paths) {
    feedback_paths_ = std::move(paths); feedback_start_ = base::TimeTicks::Now();
    AnimateFeedback(); StartMotionTimer();
    if (!animate_layout_) RevealFeedback();
  }

  static base::FilePath MediaFolder(bool pictures) {
#if BUILDFLAG(IS_WIN)
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(pictures ? FOLDERID_Pictures : FOLDERID_Videos, 0, nullptr, &value))) {
      base::FilePath path(value); CoTaskMemFree(value); return path;
    }
#endif
#if BUILDFLAG(IS_MAC)
    return MediaDirectoryMac(pictures);
#elif BUILDFLAG(IS_LINUX)
    return base::nix::GetXDGUserDirectory(pictures ? "PICTURES" : "VIDEOS", pictures ? "Pictures" : "Videos");
#else
    return Home().AppendASCII(pictures ? "Pictures" : "Videos");
#endif
  }
  static int DropOperation(const ui::DropTargetEvent& event) {
    if (event.source_operations() & ui::DragDropTypes::DRAG_LINK) return ui::DragDropTypes::DRAG_LINK;
    if (event.source_operations() & ui::DragDropTypes::DRAG_COPY) return ui::DragDropTypes::DRAG_COPY;
    return ui::DragDropTypes::DRAG_NONE;
  }
  std::u16string Name(const Bookmark& entry) const {
    if (!entry.name.empty()) return base::UTF8ToUTF16(entry.name);
    if (entry.kind == 1) return Text(IDS_CRFILES_PICTURES);
    if (entry.kind == 2) return Text(IDS_CRFILES_VIDEOS);
    return entry.path == entry.path.DirName() ? entry.path.AsUTF16Unsafe() : EntryName(entry.path);
  }
  void Rebuild() {
    buttons_.clear(); error_ = nullptr; RemoveAllChildViews();
    for (const auto& entry : entries_) {
      auto* button = AddChildView(std::make_unique<BookmarkButton>(base::BindRepeating(open_, entry.path), Name(entry),
          base::BindRepeating(&BookmarksView::ReorderBookmark, weak_.GetWeakPtr(), entry.path)));
      button->SetPaintToLayer(); button->layer()->SetFillsBoundsOpaquely(false);
      button->SetHorizontalAlignment(gfx::ALIGN_LEFT); button->SetImageLabelSpacing(10);
      button->SetImageModel(views::Button::STATE_NORMAL,
          ui::ImageModel::FromVectorIcon(vector_icons::kFolderOpenIcon, ui::kColorSysOnSurface, 20));
      button->SetTooltipText(entry.path.AsUTF16Unsafe()); button->set_context_menu_controller(this);
      buttons_.push_back(button);
    }
    // Keep an empty bookmark area available as a drop target.
    auto* space = AddChildView(std::make_unique<views::View>());
    space->SetPreferredSize(gfx::Size(0, 36));
    error_ = AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_TAG_DELETE_ERROR)));
    error_->SetSubpixelRenderingEnabled(false);
    error_->SetMultiLine(true); error_->SetVisible(false);
    SetCurrentPath(current_);
  }
  bool Commit(std::vector<Bookmark> entries) {
    base::ListValue items;
    for (const auto& entry : entries) {
      base::DictValue item; item.Set("path", entry.path.AsUTF8Unsafe()); item.Set("name", entry.name); item.Set("kind", entry.kind);
      items.Append(std::move(item));
    }
    base::DictValue stored; stored.Set("items", std::move(items));
    if (!SetCrFilesBookmarks(std::move(stored))) { error_->SetVisible(true); return false; }
    old_positions_.clear();
    for (size_t i = 0; i < buttons_.size(); ++i)
      old_positions_[entries_[i].path] = buttons_[i]->y() + buttons_[i]->layer()->transform().To2dTranslation().y();
    animate_layout_ = true;
    entries_ = std::move(entries); Rebuild();
    InvalidateLayout(); return true;
  }
  void Drop(size_t insertion, const ui::DropTargetEvent& event, ui::mojom::DragOperation& operation, std::unique_ptr<ui::LayerTreeOwner>) {
    OnDragExited(); operation = ui::mojom::DragOperation::kNone;
    if (!CanDrop(event.data()) || !DropOperation(event)) return;
    operation = DropOperation(event) == ui::DragDropTypes::DRAG_LINK ? ui::mojom::DragOperation::kLink : ui::mojom::DragOperation::kCopy;
    std::vector<base::FilePath> paths;
    const auto files = event.data().GetFilenames();
    if (!files) return;
    for (const auto& file : *files) paths.push_back(file.path);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](std::vector<base::FilePath> candidates) {
          std::erase_if(candidates, [](const auto& path) { return !path.IsAbsolute() || path.ReferencesParent() || !base::DirectoryExists(path); });
          return candidates;
        }, std::move(paths)), base::BindOnce(&BookmarksView::AddPaths, weak_.GetWeakPtr(), insertion));
  }
  void AddPaths(size_t insertion, std::vector<base::FilePath> paths) {
    auto entries = entries_;
    std::set<base::FilePath> feedback;
    insertion = std::min(insertion, entries.size());
    for (auto& path : paths) {
      path = path.NormalizePathSeparators().StripTrailingSeparators();
      if (std::ranges::find(entries, path, &Bookmark::path) != entries.end()) { feedback.insert(path); continue; }
      if (entries.size() >= 256) continue;
      entries.insert(entries.begin() + insertion++, {path, {}, 0}); feedback.insert(path);
    }
    if (feedback.empty()) return;
    if (entries.size() != entries_.size() && !Commit(std::move(entries))) return;
    Flash(std::move(feedback));
  }
  void Action(int command, base::FilePath path) {
    const auto found = std::ranges::find(entries_, path, &Bookmark::path);
    if (found == entries_.end()) return;
    if (command == IDS_CRFILES_OPEN_NEW_TAB) { new_tab_.Run(path); return; }
    if (command == IDS_CRFILES_PROPERTIES) { ShowFolderProperties(GetWidget(), path, open_tag_); return; }
    if (command == IDS_CRFILES_REMOVE_BOOKMARK) {
      auto entries = entries_; std::erase_if(entries, [&](const auto& entry) { return entry.path == path; });
      Commit(std::move(entries)); return;
    }
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog->set_fixed_width(440); dialog->SetTitle(Text(command));
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 10));
    if (command == IDS_CRFILES_RENAME) {
      auto* field = contents->AddChildView(std::make_unique<views::Textfield>());
      field->SetText(Name(*found)); field->SetAccessibleName(Text(IDS_CRFILES_RENAME));
      dialog->SetInitiallyFocusedView(field);
      dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) | static_cast<int>(ui::mojom::DialogButton::kCancel));
      dialog->SetAcceptCallbackWithClose(base::BindRepeating([](base::WeakPtr<BookmarksView> view, base::FilePath folder, views::Textfield* input) {
        if (!view) return true;
        std::u16string name; base::TrimWhitespace(input->GetText(), base::TRIM_ALL, &name);
        if (name.empty() || name.size() > 256) { input->SetInvalid(true); return false; }
        auto entries = view->entries_;
        auto entry = std::ranges::find(entries, folder, &Bookmark::path);
        if (entry == entries.end()) return true;
        entry->name = base::UTF16ToUTF8(name); view->Commit(std::move(entries));
        return !view->error_->GetVisible();
      }, weak_.GetWeakPtr(), path, base::Unretained(field)));
    }
    dialog->SetContentsView(std::move(contents));
    ShowCrFilesDialog(std::move(dialog), GetWidget());
  }
  base::RepeatingCallback<void(base::FilePath)> open_, new_tab_;
  OpenTagCallback open_tag_;
  std::vector<Bookmark> entries_;
  std::vector<raw_ptr<SidebarButton>> buttons_;
  raw_ptr<views::Label> error_ = nullptr;
  base::FilePath current_, context_;
  size_t insertion_ = 0;
  bool drop_visible_ = false, animate_layout_ = false;
  bool external_drop_highlight_ = false;
  float drop_alpha_ = 0.f, drop_start_alpha_ = 0.f;
  base::TimeTicks drop_start_, feedback_start_;
  base::RepeatingTimer motion_timer_;
  std::map<base::FilePath, float> old_positions_;
  std::set<base::FilePath> feedback_paths_;
  std::unique_ptr<ui::SimpleMenuModel> menu_;
  std::unique_ptr<views::MenuRunner> runner_;
  views::ViewTracker drag_preview_;
  base::WeakPtrFactory<BookmarksView> weak_{this};
};
BEGIN_METADATA(BookmarksView)
END_METADATA
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
class FolderScrollBar : public libcr::OverlayScrollBar, public views::AnimationDelegateViews {
  METADATA_HEADER(FolderScrollBar, libcr::OverlayScrollBar)
 public:
  FolderScrollBar(views::ScrollBar::Orientation orientation,
                  views::ScrollView* scroll)
      : libcr::OverlayScrollBar(orientation), views::AnimationDelegateViews(this), scroll_(scroll) {
    // Follow content offsets immediately instead of animating thumb bounds.
    // The scrollbar's own layer retains its fade animation.
    GetThumb()->layer()->SetAnimator(ui::LayerAnimator::CreateDefaultAnimator());
    SetRoundedThumb(true);
  }
  ~FolderScrollBar() override { rebound_.Stop(); }
  void StopRebound() {
    rebound_.Stop();
    SetThumbSubpixelOffset(base_offset_);
  }
  void Rebound(bool bottom) {
    StopRebound();
    if (!IsDrawn() || gfx::Animation::PrefersReducedMotion() ||
        !gfx::Animation::ShouldRenderRichAnimation() || !gfx::Animation::ScrollAnimationsEnabledBySystem()) return;
    const int travel = std::max(0, GetTrackBounds().height() - GetThumb()->GetLength());
    amplitude_ = std::min(GetThumb()->GetLength() * 0.16f, travel * 0.05f) * (bottom ? -1.f : 1.f);
    if (amplitude_ != 0.f) rebound_.Start();
  }
  void Update(int viewport_size, int content_size, int contents_scroll_offset) override {
    libcr::OverlayScrollBar::Update(viewport_size, content_size, contents_scroll_offset);
    // ScrollBar::Update receives an integer offset. Read the original layer
    // offset so the visual thumb also follows fractional content movement.
    double offset = contents_scroll_offset;
    if (scroll_->contents()) {
      const auto current = scroll_->CurrentOffset();
      offset = GetOrientation() == Orientation::kHorizontal ? current.x() : current.y();
    }
    const auto track = GetTrackBounds();
    const int track_size = GetOrientation() == Orientation::kHorizontal ? track.width() : track.height();
    const int travel = std::max(0, track_size - GetThumb()->GetLength());
    const double range = GetMaxPosition();
    const double position = range > 0 ? std::clamp(offset, 0.0, range) * travel / range : 0.0;
    base_offset_ = static_cast<float>(position - GetThumb()->GetPosition());
    if (rebound_.is_animating() && (range <= 0 ||
        (amplitude_ > 0 ? offset != 0 : offset != range))) StopRebound();
    ApplyRebound();
  }
 private:
  void ApplyRebound() {
    const double t = rebound_.is_animating() ? rebound_.GetCurrentValue() : 0.0;
    SetThumbSubpixelOffset(base_offset_ + amplitude_ * static_cast<float>(std::sin(3.141592653589793 * t)));
  }
  void AnimationProgressed(const gfx::Animation*) override {
    if (!IsDrawn()) { StopRebound(); return; }
    ApplyRebound();
  }
  void AnimationEnded(const gfx::Animation*) override { SetThumbSubpixelOffset(base_offset_); }
  float base_offset_ = 0.f, amplitude_ = 0.f;
  // AnimationDelegateViews drives this from compositor frames while attached;
  // the default frame rate is only the fallback without a compositor.
  gfx::LinearAnimation rebound_{base::Milliseconds(220), gfx::LinearAnimation::kDefaultFrameRate, this};
  raw_ptr<views::ScrollView> scroll_;
};
BEGIN_METADATA(FolderScrollBar)
END_METADATA
// Animate the content offset, so the thumb and file rows share one trajectory.
class FolderScrollView : public views::ScrollView, public views::AnimationDelegateViews {
  METADATA_HEADER(FolderScrollView, views::ScrollView)
 public:
  FolderScrollView() : views::ScrollView(ScrollWithLayers::kEnabled), views::AnimationDelegateViews(this), cancel_handler_(this) {
    AddPreTargetHandler(&cancel_handler_);
    scroll_subscription_ = AddContentsScrolledCallback(base::BindRepeating(
        &FolderScrollView::CancelExternalScroll, base::Unretained(this)));
  }
  ~FolderScrollView() override {
    wheel_animation_.Stop();
    RemovePreTargetHandler(&cancel_handler_);
  }
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override {
    StopThumbRebound();
    if (!contents() || !IsDrawn() || event.x_offset() || !event.y_offset() ||
        event.IsShiftDown() || event.IsControlDown() ||
        !gfx::Animation::ShouldRenderRichAnimation() ||
        !gfx::Animation::ScrollAnimationsEnabledBySystem() || gfx::Animation::PrefersReducedMotion()) {
      wheel_animation_.Stop();
      return views::ScrollView::OnMouseWheel(event);
    }
    const float current = CurrentOffset().y();
    const float delta = -static_cast<float>(event.y_offset());
    float target = wheel_animation_.is_animating() && wheel_contents_.view() == contents() ? wheel_target_ : current;
    // Reversing direction should respond immediately, without old momentum.
    const bool continuing = wheel_animation_.is_animating() && wheel_contents_.view() == contents();
    if (!continuing || (target - current) * delta < 0) {
      target = current;
      wheel_velocity_ = 0;
    }
    target = std::clamp(target + delta, 0.f, MaxOffset());
    if (target == current) { wheel_animation_.Stop(); wheel_velocity_ = 0; return false; }
    wheel_target_ = target;
    if (!continuing) wheel_tick_ = base::TimeTicks::Now();
    last_wheel_offset_ = CurrentOffset();
    wheel_contents_.SetView(contents());
    // Reset only the timeline, preserving compositor subscription and velocity.
    // The 600 ms settling deadline is measured from the latest wheel input.
    wheel_animation_.SetDuration(base::Milliseconds(600));
    if (!continuing) {
      wheel_animation_.Stop();
      wheel_animation_.Start();
    }
    return true;
  }
 private:
  class CancelHandler : public ui::EventHandler {
   public:
    explicit CancelHandler(FolderScrollView* owner) : owner_(owner) {}
    void OnMouseEvent(ui::MouseEvent* event) override {
      if (event->type() == ui::EventType::kMousePressed) { owner_->wheel_animation_.Stop(); owner_->StopThumbRebound(); }
    }
    void OnKeyEvent(ui::KeyEvent* event) override {
      if (event->type() == ui::EventType::kKeyPressed) { owner_->wheel_animation_.Stop(); owner_->StopThumbRebound(); }
    }
    void OnScrollEvent(ui::ScrollEvent*) override { owner_->wheel_animation_.Stop(); owner_->StopThumbRebound(); }
   private:
    raw_ptr<FolderScrollView> owner_;
  };
  bool BoundaryRowReached(double offset, bool bottom) const;
  void StopThumbRebound() {
    if (auto* bar = views::AsViewClass<FolderScrollBar>(vertical_scroll_bar())) bar->StopRebound();
  }
  float MaxOffset() const {
    return static_cast<float>(std::max(0, contents()->height() - GetVisibleRect().height()));
  }
  void CancelExternalScroll() {
    // Layer scroll notifications may arrive after the synchronous write.
    if (!applying_wheel_ && CurrentOffset() != last_wheel_offset_) {
      wheel_animation_.Stop();
      StopThumbRebound();
    }
  }
  void AnimationProgressed(const gfx::Animation* animation) override {
    if (!contents() || wheel_contents_.view() != contents() || !IsDrawn()) {
      wheel_animation_.Stop();
      return;
    }
    // Critically damped motion preserves velocity as repeated wheel events
    // change the target. Integrate elapsed time rather than restarting easing.
    const auto now = base::TimeTicks::Now();
    const double elapsed = (now - wheel_tick_).InSecondsF();
    wheel_tick_ = now;
    const float max_offset = MaxOffset();
    wheel_target_ = std::clamp(wheel_target_, 0.f, max_offset);
    const double displacement = CurrentOffset().y() - wheel_target_;
    constexpr double damping = 30.0;
    const double decay = std::exp(-damping * elapsed);
    const double impulse = wheel_velocity_ + damping * displacement;
    const double next = wheel_target_ + (displacement + impulse * elapsed) * decay;
    wheel_velocity_ = (wheel_velocity_ - damping * impulse * elapsed) * decay;
    // Snap by the first/last file row, not a fixed pixel-distance threshold.
    const bool bottom = wheel_target_ == max_offset;
    const bool boundary_snap = max_offset > 0 && (wheel_target_ == 0.f || bottom) &&
        BoundaryRowReached(std::clamp(next, 0.0, static_cast<double>(max_offset)), bottom);
    const bool settled = boundary_snap ||
        (std::abs(next - wheel_target_) < 0.05 && std::abs(wheel_velocity_) < 1.0);
    const bool moved = CurrentOffset().y() != wheel_target_;
    const float offset = std::clamp(static_cast<float>(settled || animation->GetCurrentValue() == 1.0 ? wheel_target_ : next),
        0.f, max_offset);
    base::AutoReset<bool> applying(&applying_wheel_, true);
    last_wheel_offset_ = gfx::PointF(CurrentOffset().x(), offset);
    if (CurrentOffset() != last_wheel_offset_) ScrollToOffset(last_wheel_offset_);
    if (settled) {
      wheel_velocity_ = 0;
      wheel_animation_.Stop();
      if (boundary_snap && moved)
        if (auto* bar = views::AsViewClass<FolderScrollBar>(vertical_scroll_bar())) bar->Rebound(bottom);
    }
  }
  void AnimationEnded(const gfx::Animation* animation) override { AnimationProgressed(animation); }
  CancelHandler cancel_handler_;
  base::CallbackListSubscription scroll_subscription_;
  views::ViewTracker wheel_contents_;
  bool applying_wheel_ = false;
  gfx::PointF last_wheel_offset_;
  double wheel_velocity_ = 0;
  base::TimeTicks wheel_tick_;
  float wheel_target_ = 0;
  // Compositor frame cadence adapts to presentation timing. Motion remains
  // elapsed-time based, independent of the number of animation callbacks.
  gfx::LinearAnimation wheel_animation_{base::Milliseconds(600), gfx::LinearAnimation::kDefaultFrameRate, this};
};
BEGIN_METADATA(FolderScrollView)
END_METADATA
#endif
// The viewport remains a drop target even below the last bookmark row.
class BookmarkScrollView : public views::ScrollView {
  METADATA_HEADER(BookmarkScrollView, views::ScrollView)
 public:
  BookmarkScrollView() {
    SetUseContentsPreferredSize(true);
    SetBackgroundColor(std::nullopt);
    SetHorizontalScrollBarMode(ScrollBarMode::kDisabled);
    ClipHeightTo(120, std::numeric_limits<int>::max());
  }
  bool GetDropFormats(int* formats, std::set<ui::ClipboardFormatType>* types) override {
    return contents() && bookmarks()->GetDropFormats(formats, types);
  }
  bool CanDrop(const ui::OSExchangeData& data) override {
    return contents() && bookmarks()->CanDrop(data);
  }
  int OnDragUpdated(const ui::DropTargetEvent& event) override {
    auto point = event.location();
    views::View::ConvertPointToTarget(this, contents(), &point);
    ui::DropTargetEvent translated(event.data(), gfx::PointF(point), event.root_location_f(), event.source_operations());
    return bookmarks()->OnDragUpdated(translated);
  }
  void OnDragExited() override { if (contents()) bookmarks()->OnDragExited(); }
  DropCallback GetDropCallback(const ui::DropTargetEvent& event) override {
    return bookmarks()->GetDropCallback(event);
  }
 private:
  BookmarksView* bookmarks() { return static_cast<BookmarksView*>(contents()); }
};
BEGIN_METADATA(BookmarkScrollView)
END_METADATA

base::FilePath DesktopPath() {
  base::FilePath desktop;
  base::PathService::Get(base::DIR_USER_DESKTOP, &desktop);
  return desktop;
}
views::LabelButton* AddButton(views::View* parent, std::u16string label,
                          base::RepeatingClosure callback) {
  auto* button = parent->AddChildView(std::make_unique<views::LabelButton>(
      std::move(callback), label));
  AnimateButton(button);
  button->SetTooltipText(label);
  button->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(6, 10)));
  return button;
}
views::LabelButton* AddIconButton(views::View* parent, const gfx::VectorIcon& icon,
                                  int label, base::RepeatingClosure callback) {
  auto* button = AddButton(parent, std::u16string(), std::move(callback));
  button->SetPreferredSize(gfx::Size(36, 36));
  button->SetAccessibleName(Text(label)); button->SetTooltipText(Text(label));
  button->SetImageModel(views::Button::STATE_NORMAL,
      ui::ImageModel::FromVectorIcon(icon, ui::kColorSysOnSurface, 20));
  return button;
}
// Observe the whole window, including blank space and non-focusable controls.
gfx::Transform CenteredScale(views::View* view, float scale) {
  gfx::Transform transform;
  transform.Translate(view->width() * (1.f - scale) / 2.f,
                      view->height() * (1.f - scale) / 2.f);
  transform.Scale(scale, scale);
  return transform;
}
void AnimateArrival(views::View* view, float initial_opacity = 0.f) {
  auto* layer = view->layer();
  layer->GetAnimator()->StopAnimating();
  layer->SetOpacity(initial_opacity);
  layer->SetTransform(CenteredScale(view, 0.98f));
  ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
  settings.SetTransitionDuration(base::Milliseconds(160));
  settings.SetTweenType(gfx::Tween::EASE_OUT);
  settings.SetPreemptionStrategy(ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
  layer->SetOpacity(1.f);
  layer->SetTransform(gfx::Transform());
}
class AddressOutsideClickHandler : public ui::EventHandler {
 public:
  AddressOutsideClickHandler(views::View* root, views::Textfield* address,
                            base::RepeatingClosure exit, base::RepeatingCallback<void(gfx::Point)> mouse,
                            base::RepeatingCallback<void(gfx::Point)> pressed)
      : root_(root), address_(address), exit_(std::move(exit)), mouse_(std::move(mouse)), pressed_(std::move(pressed)) {
    root_->AddPreTargetHandler(this);
  }
  ~AddressOutsideClickHandler() override { root_->RemovePreTargetHandler(this); }
  void OnMouseEvent(ui::MouseEvent* event) override {
    auto* target = static_cast<views::View*>(event->target());
    if (!target || target->GetWidget() != root_->GetWidget()) return;
    if (IsCrFilesWindowBlockedByModal(root_->GetWidget())) {
      // Allow mouse-exit cleanup even while new hover/press input is blocked.
      if (event->type() == ui::EventType::kMouseExited) return;
      if (event->cancelable()) event->StopPropagation();
      return;
    }
    auto screen_point = event->location();
    views::View::ConvertPointToScreen(target, &screen_point);
    // Native capture may route a popup-area event through the main window.
    // Do not dismiss editors or update main-window hover beneath any bubble.
    if (IsInsideCrFilesBubble(screen_point)) return;
    if (event->type() == ui::EventType::kMouseMoved || event->type() == ui::EventType::kMouseEntered ||
        event->type() == ui::EventType::kMouseDragged || event->type() == ui::EventType::kMousePressed) {
      auto point = event->location();
      views::View::ConvertPointToTarget(static_cast<views::View*>(event->target()), root_, &point);
      mouse_.Run(point);
    }
    if (event->type() == ui::EventType::kMousePressed) {
      pressed_.Run(screen_point);
    }
    if (event->type() == ui::EventType::kMousePressed && address_->GetVisible() &&
        !address_->Contains(static_cast<views::View*>(event->target()))) exit_.Run();
  }
 private:
  raw_ptr<views::View> root_;
  raw_ptr<views::Textfield> address_;
  base::RepeatingClosure exit_;
  base::RepeatingCallback<void(gfx::Point)> mouse_;
  base::RepeatingCallback<void(gfx::Point)> pressed_;
};
class CompletionRow : public views::Button {
  METADATA_HEADER(CompletionRow, views::Button)
 public:
  explicit CompletionRow(PressedCallback callback) : views::Button(std::move(callback)) {
    SetFocusBehavior(FocusBehavior::NEVER);
    SetLayoutManager(std::make_unique<views::FillLayout>());
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(8, 12)));
    label_ = AddChildView(std::make_unique<views::StyledLabel>());
    label_->SetCanProcessEventsWithinSubtree(false);
    AnimateButton(this);
  }
  void Update(const std::u16string& text, size_t matched) {
    if (text_ == text && matched_ == matched) return;
    if (text_ != text) { label_->SetText(text); SetAccessibleName(text); SetTooltipText(text); }
    text_ = text; matched_ = matched;
    label_->ClearStyleRanges();
    if (matched) {
      views::StyledLabel::RangeStyleInfo style;
      style.override_color_id = ui::kColorSysPrimary;
      style.custom_font = gfx::FontList().DeriveWithWeight(gfx::Font::Weight::BOLD);
      label_->AddStyleRange(gfx::Range(0, std::min(matched, text.size())), style);
    }
  }
 private:
  raw_ptr<views::StyledLabel> label_ = nullptr;
  std::u16string text_;
  size_t matched_ = 0;
};
BEGIN_METADATA(CompletionRow)
END_METADATA
// Completion only enumerates one directory, off the UI thread. Results are
// bounded and invalidated as soon as the user edits the path again.
class PathCompletion {
 public:
  explicit PathCompletion(views::Textfield* field);
  ~PathCompletion();
  void Cancel() { ClosePopup(); }
  void Changed() {
    if (updating_) return;
    ++generation_; pending_ = true; timer_.Stop();
    auto* field = Field();
    if (!field || !field->HasFocus() || field->IsIMEComposing()) { ClosePopup(); return; }
    input_ = std::u16string(field->GetText());
    if (field->GetCursorPosition() != input_.size()) { ClosePopup(); return; }
    timer_.Start(FROM_HERE, base::Milliseconds(120), base::BindOnce(&PathCompletion::Query, weak_.GetWeakPtr()));
  }
  bool Key(const ui::KeyEvent& event) {
    if (event.type() != ui::EventType::kKeyPressed) return false;
    auto* field = Field();
    if (!field || field->IsIMEComposing()) return false;
    if (!popup_ || !popup_->IsVisible() || matches_.empty()) return false;
    if (pending_) {
      if (event.key_code() == ui::VKEY_ESCAPE || event.key_code() == ui::VKEY_RETURN) ClosePopup();
      return false;
    }
    if (event.key_code() == ui::VKEY_ESCAPE) { ClosePopup(); return true; }
    if (event.key_code() == ui::VKEY_RETURN) {
      if (index_ < matches_.size()) { Choose(index_); return true; }
      ClosePopup(); return false;
    }
    if (event.key_code() == ui::VKEY_TAB) {
      Choose(index_ < matches_.size() ? index_ : 0); return true;
    }
    if (event.key_code() != ui::VKEY_UP && event.key_code() != ui::VKEY_DOWN) return false;
    if (index_ >= matches_.size()) index_ = event.key_code() == ui::VKEY_UP ? matches_.size() - 1 : 0;
    else index_ = (index_ + matches_.size() + (event.key_code() == ui::VKEY_UP ? -1 : 1)) % matches_.size();
    for (size_t i = 0; i < rows_.size(); ++i)
      rows_[i]->SetBackground(i == index_ ? views::CreateRoundedRectBackground(ui::kColorSysSecondaryContainer, 6) : nullptr);
    rows_[index_]->ScrollRectToVisible(rows_[index_]->GetLocalBounds());
    return true;
  }
 private:
  views::Textfield* Field() { return static_cast<views::Textfield*>(field_.view()); }
  void Query() {
    auto split = input_.find_last_of(u"/\\");
    if (split == std::u16string::npos) { ClosePopup(); return; }
    auto parent = ParseNavigationPath(std::u16string_view(input_).substr(0, split + 1));
    if (!parent || !parent->IsAbsolute()) { ClosePopup(); return; }
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](base::FilePath directory, std::u16string prefix, std::u16string leaf) {
          std::vector<std::u16string> result;
          auto add = [&](std::u16string name) {
#if BUILDFLAG(IS_WIN)
            const bool match = base::StartsWith(name, leaf, base::CompareCase::INSENSITIVE_ASCII);
#else
            const bool match = name.starts_with(leaf);
#endif
            if (match) result.push_back(prefix + name + u"/");
          };
#if BUILDFLAG(IS_WIN)
          if (IsDrivesLocation(directory)) {
            const DWORD mask = GetLogicalDrives();
            for (int i = 0; i < 26; ++i) if (mask & (1u << i)) add(std::u16string(1, static_cast<char16_t>(u'a' + i)));
          } else
#endif
          {
            base::FileEnumerator entries(directory, false, base::FileEnumerator::DIRECTORIES);
            size_t count = 0;
            for (auto path = entries.Next(); !path.empty() && count++ < 4096 && result.size() < 128; path = entries.Next())
              add(path.BaseName().AsUTF16Unsafe());
          }
          std::ranges::sort(result);
          return result;
        }, *parent, input_.substr(0, split + 1), input_.substr(split + 1)),
        base::BindOnce(&PathCompletion::Ready, weak_.GetWeakPtr(), generation_));
  }
  void Ready(uint64_t generation, std::vector<std::u16string> matches) {
    if (generation != generation_) return;
    pending_ = false;
    auto* field = Field();
    if (!field || !field->HasFocus() || field->IsIMEComposing() || field->GetText() != input_ ||
        field->GetCursorPosition() != input_.size()) return;
    const auto selected = index_ < matches_.size() ? matches_[index_] : std::u16string();
    matches_ = std::move(matches);
    index_ = static_cast<size_t>(std::ranges::find(matches_, selected) - matches_.begin());
    if (!matches_.empty()) ShowPopup();
    else if (popup_) popup_->Hide();
  }
  void Choose(size_t index) {
    auto* field = Field(); if (!field || pending_ || index >= matches_.size()) return;
    const auto text = matches_[index];
    ClosePopup();
    updating_ = true;
    field->SetText(text);
    field->SetSelectedRange(gfx::Range(text.size()));
    field->RequestFocus();
    updating_ = false;
  }
  void ClosePopup() {
    ++generation_; pending_ = false; timer_.Stop();
    visibility_timer_.Stop(); rows_.clear(); list_ = nullptr;
    if (popup_delegate_) popup_delegate_->SetAnchorView(nullptr);
    if (popup_) popup_->MakeCloseSynchronous({});
    popup_.reset(); popup_delegate_.reset();
  }
  void CheckVisibility() {
    auto* field = Field();
    if (!field || !field->HasFocus() || !field->IsDrawn() || field->IsIMEComposing()) ClosePopup();
  }
  void ShowPopup() {
    auto* field = Field(); if (!field) return;
    if (popup_) { UpdateRows(); if (!popup_->IsVisible()) popup_->ShowInactive(); return; }
    popup_delegate_ = std::make_unique<views::BubbleDialogDelegate>(field, views::BubbleBorder::TOP_LEFT);
    popup_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    popup_delegate_->SetCanActivate(false);
    popup_delegate_->SetShowTitle(false);
    popup_delegate_->set_close_on_deactivate(false);
    popup_delegate_->set_margins(gfx::Insets(4));
    popup_delegate_->set_fixed_width(std::max(280, field->width()));
    auto scroll = std::make_unique<views::ScrollView>();
    scroll->ClipHeightTo(0, 288);
    auto list = std::make_unique<views::View>();
    list->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical));
    list_ = list.get();
    scroll->SetContents(std::move(list));
    popup_delegate_->SetContentsView(std::move(scroll));
    popup_ = CreateCrFilesBubble(popup_delegate_.get(), base::BindOnce(
        [](base::WeakPtr<PathCompletion> completion, views::Widget::ClosedReason) {
          if (!completion) return;
          completion->visibility_timer_.Stop(); completion->rows_.clear(); completion->list_ = nullptr;
          completion->popup_delegate_->SetAnchorView(nullptr);
          completion->popup_.reset();
        }, weak_.GetWeakPtr()));
    UpdateRows();
    popup_->ShowInactive();
    visibility_timer_.Start(FROM_HERE, base::Milliseconds(100),
        base::BindRepeating(&PathCompletion::CheckVisibility, weak_.GetWeakPtr()));
  }
  void UpdateRows() {
    const bool resized = rows_.size() != matches_.size();
    while (rows_.size() > matches_.size()) {
      auto* row = rows_.back().get(); rows_.pop_back();
      list_->RemoveChildViewT(row);
    }
    while (rows_.size() < matches_.size()) {
      const size_t index = rows_.size();
      auto* row = list_->AddChildView(std::make_unique<CompletionRow>(base::BindRepeating(
          [](base::WeakPtr<PathCompletion> completion, size_t selected) {
            if (!completion) return;
            base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                base::BindOnce(&PathCompletion::ChooseCurrent, completion, completion->generation_, selected));
          }, weak_.GetWeakPtr(), index)));
      rows_.push_back(row);
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
      rows_[i]->Update(matches_[i], input_.size());
      rows_[i]->SetBackground(i == index_ ? views::CreateRoundedRectBackground(ui::kColorSysSecondaryContainer, 6) : nullptr);
    }
    if (resized) popup_delegate_->SizeToContents();
  }
  void ChooseCurrent(uint64_t generation, size_t index) {
    if (generation == generation_) Choose(index);
  }
  views::ViewTracker field_;
  base::OneShotTimer timer_;
  std::u16string input_;
  std::vector<std::u16string> matches_;
  size_t index_ = 0;
  bool updating_ = false;
  std::unique_ptr<views::BubbleDialogDelegate> popup_delegate_;
  std::unique_ptr<views::Widget> popup_;
  std::vector<raw_ptr<CompletionRow>> rows_;
  raw_ptr<views::View> list_ = nullptr;
  uint64_t generation_ = 0;
  bool pending_ = false;
  base::RepeatingTimer visibility_timer_;
  base::WeakPtrFactory<PathCompletion> weak_{this};
};
PathCompletion::PathCompletion(views::Textfield* field) { field_.SetView(field); }
PathCompletion::~PathCompletion() { ClosePopup(); }
// Track an archive by path through the caller, not by a virtual row pointer.
class ArchivePathBubble : public views::BubbleDialogDelegate {
 public:
  ArchivePathBubble(views::View* pane, base::RepeatingCallback<gfx::Rect()> bounds);
  ~ArchivePathBubble() override;
  gfx::Rect GetBubbleBounds() override { return GetArchiveBubbleBounds(*this); }
  gfx::Rect GetAnchorRect() const override {
    const auto bounds = bounds_.Run();
    if (!bounds.IsEmpty()) last_bounds_ = bounds;
    return last_bounds_.IsEmpty() ? views::BubbleDialogDelegate::GetAnchorRect() : last_bounds_;
  }
 private:
  void UpdateAnchor() {
    if (!GetWidget() || GetWidget()->IsClosed()) { timer_.Stop(); return; }
    const auto bounds = bounds_.Run();
    if (bounds.IsEmpty() && GetBubbleFrameView())
      GetBubbleFrameView()->SetDisplayVisibleArrow(false);
    SetArrowWithoutResizing(bounds.IsEmpty() ? views::BubbleBorder::NONE : views::BubbleBorder::TOP_CENTER);
    OnAnchorBoundsChanged();
  }
  base::RepeatingCallback<gfx::Rect()> bounds_;
  mutable gfx::Rect last_bounds_;
  base::RepeatingTimer timer_;
};
ArchivePathBubble::ArchivePathBubble(views::View* pane, base::RepeatingCallback<gfx::Rect()> bounds)
    : views::BubbleDialogDelegate(pane, views::BubbleBorder::TOP_CENTER), bounds_(std::move(bounds)) {
  set_shadow(views::BubbleBorder::STANDARD_SHADOW);
  timer_.Start(FROM_HERE, base::Milliseconds(100), this, &ArchivePathBubble::UpdateAnchor);
}
ArchivePathBubble::~ArchivePathBubble() = default;
class PathTextfield : public views::Textfield {
  METADATA_HEADER(PathTextfield, views::Textfield)
 public:
  explicit PathTextfield(base::RepeatingClosure blur) : blur_(std::move(blur)) {
    RemoveHoverEffect();
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(2, 16)));
    SetBackgroundColor(ui::kColorSysSurfaceVariant);
    views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(), 999);
  }
  void OnFocus() override {
    SetBackgroundColor(ui::kColorSysBaseContainerElevated);
    views::Textfield::OnFocus();
    SchedulePaint();
  }
  void OnBlur() override {
    SetBackgroundColor(ui::kColorSysSurfaceVariant);
    views::Textfield::OnBlur();
    blur_.Run();
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetColorProvider()->GetColor(HasFocus() ?
        ui::kColorSysBaseContainerElevated : ui::kColorSysSurfaceVariant));
    canvas->DrawRoundRect(GetLocalBounds(), height() / 2, flags);
  }
  void OnPaintBorder(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(1);
    flags.setColor(GetColorProvider()->GetColor(HasFocus() ? ui::kColorSysPrimary : ui::kColorSysDivider));
    gfx::Rect bounds = GetLocalBounds();
    bounds.Inset(1);
    canvas->DrawRoundRect(bounds, bounds.height() / 2, flags);
  }
 private:
  base::RepeatingClosure blur_;
};
BEGIN_METADATA(PathTextfield)
END_METADATA
gfx::ImageSkia ThumbnailImage(const gfx::ImageSkia& image, int edge) {
  const float scale = static_cast<float>(edge) / std::max(image.width(), image.height());
  const gfx::Size size(std::max(1, static_cast<int>(std::round(image.width() * scale))),
                       std::max(1, static_cast<int>(std::round(image.height() * scale))));
  return gfx::ImageSkiaOperations::CreateResizedImage(
      image, skia::ImageOperations::RESIZE_BEST, size);
}
class NavigationScrollView : public views::ScrollView, public views::AnimationDelegateViews {
  METADATA_HEADER(NavigationScrollView, views::ScrollView)
 public:
  explicit NavigationScrollView(base::RepeatingClosure edit)
      : views::ScrollView(ScrollWithLayers::kEnabled), views::AnimationDelegateViews(this), handler_(std::move(edit)) {
    SetUseContentsPreferredSize(true);
    SetBackgroundColor(std::nullopt);
    SetVerticalScrollBarMode(ScrollBarMode::kDisabled);
    SetHorizontalScrollBarMode(ScrollBarMode::kHiddenButEnabled);
    SetTreatAllScrollEventsAsHorizontal(true);
    AddPreTargetHandler(&handler_);
    scroll_subscription_ = AddContentsScrolledCallback(base::BindRepeating(
        &NavigationScrollView::ExternalScroll, base::Unretained(this)));
  }
  ~NavigationScrollView() override { StopScrolling(); RemovePreTargetHandler(&handler_); }
  float MaxOffset() const {
    return contents() ? static_cast<float>(std::max(0, contents()->width() - GetVisibleRect().width())) : 0.f;
  }
  void StopScrolling() { animation_.Stop(); }
  void ScrollPage(int direction) {
    if (!contents() || !IsDrawn()) return;
    const float current = CurrentOffset().x();
    const float previous_target = animation_.is_animating() && (target_ - current) * direction > 0 ? target_ : current;
    const float step = std::max(32.f, GetVisibleRect().width() * 0.7f);
    StopScrolling();
    start_ = current;
    target_ = std::clamp(previous_target + direction * step, 0.f, MaxOffset());
    if (target_ == current) return;
    if (!gfx::Animation::ShouldRenderRichAnimation() ||
        !gfx::Animation::ScrollAnimationsEnabledBySystem() || gfx::Animation::PrefersReducedMotion()) {
      ApplyOffset(target_); return;
    }
    animation_.Start();
  }
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override {
    StopScrolling();
    return views::ScrollView::OnMouseWheel(event);
  }
  void OnScrollEvent(ui::ScrollEvent* event) override {
    StopScrolling();
    views::ScrollView::OnScrollEvent(event);
  }
 private:
  void ApplyOffset(float offset) {
    last_offset_ = gfx::PointF(std::clamp(offset, 0.f, MaxOffset()), 0);
    applying_ = true;
    ScrollToOffset(last_offset_);
    applying_ = false;
  }
  void ExternalScroll() {
    // Keyboard focus and native/compositor scrolling take priority over arrows.
    if (!applying_ && CurrentOffset() != last_offset_) StopScrolling();
  }
  void AnimationProgressed(const gfx::Animation*) override {
    if (!contents() || !IsDrawn()) { StopScrolling(); return; }
    target_ = std::clamp(target_, 0.f, MaxOffset());
    const double t = animation_.GetCurrentValue();
    ApplyOffset(start_ + (target_ - start_) * static_cast<float>(1 - std::pow(1 - t, 3)));
  }
  void AnimationEnded(const gfx::Animation*) override { if (contents()) ApplyOffset(target_); }
  class DoubleClickHandler : public ui::EventHandler {
   public:
    explicit DoubleClickHandler(base::RepeatingClosure edit) : edit_(std::move(edit)) {}
    void OnMouseEvent(ui::MouseEvent* event) override {
      if (event->type() != ui::EventType::kMousePressed || !event->IsOnlyLeftMouseButton()) return;
      const auto point = event->root_location();
      // Navigation may rebuild the clicked segment. Track clicks on the stable
      // path bar instead of relying on the identity of that temporary button.
      // Root coordinates also survive arrows appearing/disappearing after the
      // first click changes the path and shifts the scroll viewport.
      const bool double_click = !last_press_.is_null() &&
          event->time_stamp() - last_press_ <= views::GetDoubleClickInterval() &&
          std::abs(point.x() - last_point_.x()) <= 4 && std::abs(point.y() - last_point_.y()) <= 4;
      last_press_ = event->time_stamp();
      last_point_ = point;
      if (double_click) {
        last_press_ = base::TimeTicks();
        event->SetHandled();
        event->StopPropagation();
        edit_.Run();
      }
    }
   private:
    base::TimeTicks last_press_;
    gfx::Point last_point_;
    base::RepeatingClosure edit_;
  };
  DoubleClickHandler handler_;
  float start_ = 0, target_ = 0;
  bool applying_ = false;
  gfx::PointF last_offset_;
  gfx::LinearAnimation animation_{base::Milliseconds(160), gfx::LinearAnimation::kDefaultFrameRate, this};
  base::CallbackListSubscription scroll_subscription_;
};
BEGIN_METADATA(NavigationScrollView)
END_METADATA
class NavigationScrollButton : public views::ImageButton {
  METADATA_HEADER(NavigationScrollButton, views::ImageButton)
 public:
  NavigationScrollButton(base::RepeatingClosure scroll, const gfx::VectorIcon& icon, int label)
      : views::ImageButton(scroll), scroll_(std::move(scroll)),
        repeater_(base::BindRepeating(&NavigationScrollButton::Repeat, base::Unretained(this))) {
    button_controller()->set_notify_action(views::ButtonController::NotifyAction::kOnPress);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetFlipCanvasOnPaintForRTLUI(false);
    SetTooltipText(Text(label)); SetAccessibleName(Text(label));
    SetImageHorizontalAlignment(ALIGN_CENTER); SetImageVerticalAlignment(ALIGN_MIDDLE);
    for (auto state : {STATE_NORMAL, STATE_HOVERED, STATE_PRESSED, STATE_DISABLED})
      SetImageModel(state, ui::ImageModel::FromVectorIcon(icon, ui::kColorSysOnSurface, 16));
    SetPreferredSize(gfx::Size(24, 28));
    SetPaintToLayer(); layer()->SetFillsBoundsOpaquely(false);
    layer()->SetOpacity(0.f);
    SetAvailable(false);
  }
  void SetAvailable(bool available) {
    SetEnabled(available);
    SetFocusBehavior(available ? FocusBehavior::ALWAYS : FocusBehavior::NEVER);
    SetCanProcessEventsWithinSubtree(available);
    GetViewAccessibility().SetIsIgnored(!available);
    if (!available) repeater_.Stop();
    const float opacity = available ? 1.f : 0.f;
    if (layer()->GetTargetOpacity() == opacity) return;
    if (!IsDrawn() || !gfx::Animation::ShouldRenderRichAnimation() || gfx::Animation::PrefersReducedMotion()) {
      layer()->GetAnimator()->AbortAllAnimations();
      layer()->SetOpacity(opacity);
      return;
    }
    // Keep the button's layout slot while fading, so the path does not shift.
    ui::ScopedLayerAnimationSettings animation(layer()->GetAnimator());
    animation.SetTransitionDuration(base::Milliseconds(160));
    animation.SetTweenType(gfx::Tween::EASE_OUT);
    animation.SetPreemptionStrategy(ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
    layer()->SetOpacity(opacity);
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    const bool handled = views::ImageButton::OnMousePressed(event);
    if (handled && event.IsOnlyLeftMouseButton() && GetEnabled()) repeater_.Start();
    return handled;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    repeater_.Stop(); views::ImageButton::OnMouseReleased(event);
  }
  void OnMouseCaptureLost() override {
    repeater_.Stop(); views::ImageButton::OnMouseCaptureLost();
  }
 private:
  void Repeat() {
    if (!IsDrawn() || !GetEnabled() || GetState() != STATE_PRESSED) { repeater_.Stop(); return; }
    scroll_.Run();
  }
  base::RepeatingClosure scroll_;
  views::RepeatController repeater_;
};
BEGIN_METADATA(NavigationScrollButton)
END_METADATA
class PathEditArea : public views::LabelButton {
  METADATA_HEADER(PathEditArea, views::LabelButton)
 public:
  explicit PathEditArea(base::RepeatingClosure edit)
      : views::LabelButton(std::move(edit), std::u16string()) {
    SetAccessibleName(Text(IDS_CRFILES_LOCATION));
    SetFocusBehavior(FocusBehavior::NEVER);
    SetRequestFocusOnPress(false);
    views::InkDrop::Get(this)->SetMode(views::InkDropHost::InkDropMode::OFF);
  }
  ui::Cursor GetCursor(const ui::MouseEvent&) override {
    return ui::Cursor(ui::mojom::CursorType::kIBeam);
  }
};
BEGIN_METADATA(PathEditArea)
END_METADATA
class NavigationPathView : public views::View {
  METADATA_HEADER(NavigationPathView, views::View)
 public:
  explicit NavigationPathView(base::RepeatingClosure edit) {
    scroll_ = AddChildView(std::make_unique<NavigationScrollView>(edit));
    left_ = AddChildView(std::make_unique<NavigationScrollButton>(
        base::BindRepeating(&NavigationPathView::Scroll, base::Unretained(this), -1),
        kKeyboardArrowLeftIcon, IDS_CRFILES_SCROLL_PATH_LEFT));
    right_ = AddChildView(std::make_unique<NavigationScrollButton>(
        base::BindRepeating(&NavigationPathView::Scroll, base::Unretained(this), 1),
        kKeyboardArrowRightIcon, IDS_CRFILES_SCROLL_PATH_RIGHT));
    left_->SetVisible(false); right_->SetVisible(false);
    caption_left_ = AddChildView(std::make_unique<views::View>());
    caption_right_ = AddChildView(std::make_unique<views::View>());
    for (auto* caption : {caption_left_.get(), caption_right_.get()}) {
      caption->SetProperty(views::kHitTestComponentKey, static_cast<int>(HTCAPTION));
      caption->GetViewAccessibility().SetIsIgnored(true);
      caption->SetVisible(false);
    }
    edit_area_ = AddChildView(std::make_unique<PathEditArea>(std::move(edit)));
    edit_area_->SetVisible(false);
    scroll_subscription_ = scroll_->AddContentsScrolledCallback(base::BindRepeating(
        &NavigationPathView::UpdateButtons, base::Unretained(this)));
  }
  views::View* contents() const { return scroll_->contents(); }
  void SetEditingAllowed(bool allowed) {
    if (editing_allowed_ == allowed) return;
    editing_allowed_ = allowed;
    UpdateBlankAreas();
  }
  bool OnMouseWheel(const ui::MouseWheelEvent& event) override { return scroll_->OnMouseWheel(event); }
  void OnScrollEvent(ui::ScrollEvent* event) override { scroll_->OnScrollEvent(event); }
  void SetContents(std::unique_ptr<views::View> contents) {
    caption_left_->SetVisible(false); caption_right_->SetVisible(false); edit_area_->SetVisible(false);
    scroll_->StopScrolling();
    scroll_->SetContents(std::move(contents));
    reveal_current_ = true;
    InvalidateLayout();
  }
  void Layout(PassKey) override {
    if (!scroll_synchronizer_) scroll_synchronizer_ = scroll_->EnableScrollSynchronization();
    const bool overflow = contents() && contents()->GetPreferredSize({}).width() > width();
    const int edge = overflow ? std::min(24, std::max(0, width() / 3)) : 0;
    left_->SetVisible(overflow); right_->SetVisible(overflow);
    // Child bounds are mirrored by Views under RTL; keep physical arrow sides.
    left_->SetBounds(base::i18n::IsRTL() ? width() - edge : 0, 0, edge, height());
    right_->SetBounds(base::i18n::IsRTL() ? 0 : width() - edge, 0, edge, height());
    scroll_->SetBounds(edge, 0, std::max(0, width() - 2 * edge), height());
    LayoutSuperclass<views::View>(this);
    scroll_->DeprecatedLayoutImmediately();
    if (contents()) contents()->DeprecatedLayoutImmediately();
    if (contents()) {
      const float offset = std::clamp(scroll_->CurrentOffset().x(), 0.f, scroll_->MaxOffset());
      if (offset != scroll_->CurrentOffset().x()) {
        scroll_->StopScrolling();
        scroll_->ScrollToOffset(gfx::PointF(offset, 0));
      }
    }
    if (reveal_current_ && contents() && !contents()->children().empty() &&
        scroll_->GetVisibleRect().width() > 0) {
      reveal_current_ = false;
      auto* current = contents()->children().back().get();
      current->ScrollRectToVisible(current->GetLocalBounds());
    }
    UpdateButtons();
  }
 private:
  void Scroll(int direction) { scroll_->ScrollPage(base::i18n::IsRTL() ? -direction : direction); }
  void UpdateButtons() {
    const float offset = scroll_->CurrentOffset().x(), max_offset = scroll_->MaxOffset();
    const bool before = offset > 0.5f, after = offset < max_offset - 0.5f;
    left_->SetAvailable(base::i18n::IsRTL() ? after : before);
    right_->SetAvailable(base::i18n::IsRTL() ? before : after);
    UpdateBlankAreas();
  }
  void UpdateBlankAreas() {
    if (!contents() || contents()->children().empty() || width() <= 0 ||
        contents()->GetPreferredSize({}).width() > width()) {
      caption_left_->SetVisible(false); caption_right_->SetVisible(false); edit_area_->SetVisible(false);
      return;
    }
    // Inspect actual child bounds: the scroll contents can fill the viewport
    // even when the breadcrumbs use only a small part of it.
    int occupied_left = width(), occupied_right = 0;
    for (const auto& child : contents()->children()) {
      if (!child->GetVisible()) continue;
      const auto bounds = views::View::ConvertRectToTarget(child.get(), this, child->GetLocalBounds());
      occupied_left = std::min(occupied_left, bounds.x());
      occupied_right = std::max(occupied_right, bounds.right());
    }
    occupied_left = std::clamp(occupied_left - contents()->GetInsets().left(), 0, width());
    occupied_right = std::clamp(occupied_right + contents()->GetInsets().right(), occupied_left, width());
    const int edit_width = editing_allowed_ ? std::min(108, width() - occupied_right) : 0;
    // Coordinates above are physical; compensate for Views' RTL mirroring so
    // the editing target stays at the left edge of the blank space after the path.
    auto place = [this](views::View* view, int x, int width) {
      view->SetBounds(GetMirroredXWithWidthInView(x, width), 0, width, height());
      view->SetVisible(width > 0);
    };
    place(caption_left_, 0, occupied_left);
    place(caption_right_, occupied_right + edit_width, width() - occupied_right - edit_width);
    place(edit_area_, occupied_right, edit_width);
  }
  raw_ptr<NavigationScrollView> scroll_ = nullptr;
  raw_ptr<NavigationScrollButton> left_ = nullptr, right_ = nullptr;
  raw_ptr<views::View> caption_left_ = nullptr, caption_right_ = nullptr;
  raw_ptr<PathEditArea> edit_area_ = nullptr;
  bool editing_allowed_ = true;
  bool reveal_current_ = false;
  std::unique_ptr<views::ScrollView::ScopedScrollSynchronizer> scroll_synchronizer_;
  base::CallbackListSubscription scroll_subscription_;
};
BEGIN_METADATA(NavigationPathView)
END_METADATA
class BreadcrumbDivider : public views::View {
  METADATA_HEADER(BreadcrumbDivider, views::View)
 public:
  BreadcrumbDivider() {
    SetPreferredSize(gfx::Size(8, 16));
    SetCanProcessEventsWithinSubtree(false);
  }
  void OnPaint(gfx::Canvas* canvas) override {
    const bool dark = GetWidget()->GetColorMode() == ui::ColorProviderKey::ColorMode::kDark;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    // Adjacent dark and light faces make a recessed separator.
    flags.setColor(SkColorSetA(SK_ColorBLACK, dark ? 100 : 64));
    canvas->DrawRoundRect(gfx::RectF(3, 0, 1, height()), 0.5f, flags);
    flags.setColor(SkColorSetA(SK_ColorWHITE, dark ? 38 : 190));
    canvas->DrawRoundRect(gfx::RectF(4, 0, 1, height()), 0.5f, flags);
  }
};
BEGIN_METADATA(BreadcrumbDivider)
END_METADATA
class BreadcrumbButton : public views::LabelButton {
  METADATA_HEADER(BreadcrumbButton, views::LabelButton)
 public:
  views::View* NameAnchor() const { return label(); }
  BreadcrumbButton(base::RepeatingClosure callback, std::u16string title, bool current, bool first, NavigationStyle style, bool animate_current)
      : views::LabelButton(std::move(callback), title), current_(current), first_(first), style_(style) {
    // Breadcrumbs are painted into a transparent layer for path transitions.
    label()->SetSubpixelRenderingEnabled(false);
    AnimateButton(this);
    views::InkDrop::Get(this)->SetMode(views::InkDropHost::InkDropMode::OFF);
    SetAnimationDuration(base::Milliseconds(120));
    SetBorder(views::CreateEmptyBorder(style == NavigationStyle::kRaised ?
        gfx::Insets::TLBR(4, 15, 4, 13) : gfx::Insets::VH(4, 8)));
    SetEnabledTextColors((style == NavigationStyle::kRaised || style == NavigationStyle::kConnected) ?
        (current ? ui::kColorSysOnPrimaryContainer : ui::kColorSysOnSurface) :
        (current ? ui::kColorSysOnSurface : ui::kColorSysOnSurfaceSecondary));
    underline_animation_.SetSlideDuration(base::Milliseconds(140));
    if (style == NavigationStyle::kUnderline && current) {
      if (animate_current) underline_animation_.Show();
      else underline_animation_.Reset(1.0);
    }
  }
  gfx::Size CalculatePreferredSize(const views::SizeBounds& available) const override {
    auto size = views::LabelButton::CalculatePreferredSize(available);
    // Leave two DIPs above and below the segment in the 32-DIP path bar.
    size.set_height(std::max(28, label()->GetPreferredSize({}).height() + 8));
    return size;
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    const float w = width(), h = height();
    if (w < 12 || h < 12) return;
    if (style_ == NavigationStyle::kConnected) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      const gfx::RectF bounds(0, 0, w, h - 3);
      if (current_) {
        flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimaryContainer));
        canvas->DrawRoundRect(bounds, 6.f, flags);
      } else {
        const bool dark = GetWidget()->GetColorMode() == ui::ColorProviderKey::ColorMode::kDark;
        const SkPoint points[] = {{3, h - 3}, {w - 3, h - 3}};
        const SkScalar positions[] = {0, 0.18f, 0.82f, 1};
        for (int row = 0; row < 3; ++row) {
          const float strength = row == 0 ? 1.f : (row == 1 ? 0.5f : 0.2f);
          const float alpha = (dark ? 0.12f : 0.18f) * strength;
          const SkColor4f colors[] = {{0, 0, 0, 0}, {0, 0, 0, alpha},
                                      {0, 0, 0, alpha}, {0, 0, 0, 0}};
          flags.setShader(cc::PaintShader::MakeLinearGradient(
              points, colors, positions, 4, SkTileMode::kClamp));
          canvas->DrawRect(gfx::RectF(3, h - 3 + row, w - 6, 1), flags);
        }
        flags.setShader(nullptr);
      }
      flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
      flags.setAlphaf(GetState() == STATE_PRESSED ? 0.12f :
          0.06f * static_cast<float>(hover_animation().GetCurrentValue()));
      canvas->DrawRoundRect(bounds, 6.f, flags);
      return;
    }
    if (style_ != NavigationStyle::kRaised) {
      cc::PaintFlags flags; flags.setAntiAlias(true);
      const auto bounds = GetLocalBounds();
      flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
      if (style_ == NavigationStyle::kUnderline) {
        const float progress = current_ ? static_cast<float>(underline_animation_.GetCurrentValue()) :
            static_cast<float>(hover_animation().GetCurrentValue());
        flags.setAlphaf(GetState() == STATE_PRESSED ? 1.f : (current_ ? progress : 0.45f * progress));
        const float line_width = std::max(4.f, w - 16) * (0.6f + 0.4f * progress);
        canvas->DrawRoundRect(gfx::RectF((w - line_width) / 2, h - 3, line_width, 2), 1.f, flags);
        return;
      }
      flags.setAlphaf(GetState() == STATE_PRESSED ? 0.22f :
          0.12f * static_cast<float>(hover_animation().GetCurrentValue()));
      canvas->DrawRoundRect(bounds, 7, flags);
      return;
    }
    // Rounded chevrons keep the path hierarchy without sharp bevels. Geometry
    // stays fixed while pressing; only the surface color changes.
    const float top = 2, bottom = h - 3, middle = (top + bottom) / 2;
    SkPathBuilder builder;
    builder.moveTo(6, top).lineTo(w - 12, top)
        .quadTo(w - 10, top, w - 9, top + 2)
        .lineTo(w - 3, middle - 2)
        .quadTo(w - 1, middle, w - 3, middle + 2)
        .lineTo(w - 9, bottom - 2)
        .quadTo(w - 10, bottom, w - 12, bottom)
        .lineTo(6, bottom);
    if (first_) {
      builder.quadTo(2, bottom, 2, bottom - 4).lineTo(2, top + 4)
          .quadTo(2, top, 6, top);
    } else {
      builder.quadTo(3, bottom, 5, bottom - 3)
          .lineTo(10, middle + 2).quadTo(12, middle, 10, middle - 2)
          .lineTo(5, top + 3).quadTo(3, top, 6, top);
    }
    builder.close();
    const auto path = builder.detach();
    const bool dark = GetWidget()->GetColorMode() == ui::ColorProviderKey::ColorMode::kDark;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    if (current_) {
      flags.setColor(SkColorSetA(SK_ColorBLACK, dark ? 24 : 20));
      canvas->Save();
      canvas->Translate(gfx::Vector2d(0, 1));
      canvas->DrawPath(path, flags);
      canvas->Restore();
    } else {
      // Fade the two-pixel bottom shadow vertically and at both ends.
      const float shadow_left = 6, shadow_right = w - 12;
      if (shadow_right > shadow_left) {
        const SkPoint points[] = {{shadow_left, bottom}, {shadow_right, bottom}};
        const SkScalar positions[] = {0, 0.15f, 0.85f, 1};
        for (int row = 0; row < 2; ++row) {
          const float alpha = (dark ? (row == 0 ? 18.f : 6.f) :
                                     (row == 0 ? 28.f : 10.f)) / 255.f;
          const SkColor4f colors[] = {{0, 0, 0, 0}, {0, 0, 0, alpha},
                                      {0, 0, 0, alpha}, {0, 0, 0, 0}};
          flags.setShader(cc::PaintShader::MakeLinearGradient(
              points, colors, positions, 4, SkTileMode::kClamp));
          canvas->DrawRect(gfx::RectF(shadow_left, bottom + row,
                                    shadow_right - shadow_left, 1), flags);
        }
        flags.setShader(nullptr);
      }
    }
    flags.setColor(GetColorProvider()->GetColor(current_ ?
        ui::kColorSysPrimaryContainer : ui::kColorSysHeaderContainerInactive));
    canvas->DrawPath(path, flags);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
    flags.setAlphaf(GetState() == STATE_PRESSED ? 0.12f :
        0.06f * static_cast<float>(hover_animation().GetCurrentValue()));
    canvas->DrawPath(path, flags);
  }
 private:
  bool current_;
  bool first_;
  NavigationStyle style_;
  gfx::SlideAnimation underline_animation_{this};
};
BEGIN_METADATA(BreadcrumbButton)
END_METADATA
class FileRow : public views::Button {
  METADATA_HEADER(FileRow, views::Button)
 public:
  void SetPointerMotionCallback(base::RepeatingClosure callback) {
    pointer_motion_ = std::move(callback);
  }
  void OnMouseMoved(const ui::MouseEvent& event) override {
    views::Button::OnMouseMoved(event);
    if (!event.IsSynthesized() && pointer_motion_) pointer_motion_.Run();
  }

  explicit FileRow(bool icons) : views::Button(PressedCallback()), icons_(icons) {
    AnimateButton(this);
    SetNotifyEnterExitOnChild(true);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetRequestFocusOnPress(true);
    SetInstallFocusRingOnFocus(false);
    if (icons) views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(), 16);
    else views::InstallRectHighlightPathGenerator(this);
    views::InkDrop::Get(this)->SetMode(views::InkDropHost::InkDropMode::OFF);
  }
  bool ReflowAnimating() const { return reflow_timer_.IsRunning(); }
  bool CanAnimateReflow() const {
    return GetState() != STATE_PRESSED && (!layer() || ReflowAnimating() ||
        (layer()->transform().IsIdentity() && layer()->opacity() == 1.f));
  }
  void StopReflowAnimation() {
    if (!ReflowAnimating()) return;
    reflow_timer_.Stop();
    if (layer()) {
      layer()->GetAnimator()->StopAnimating();
      layer()->SetTransform(gfx::Transform());
      layer()->SetOpacity(1.f);
      if (reflow_owns_layer_) DestroyLayer();
    }
    reflow_owns_layer_ = false;
  }
  void AnimateReflow(const gfx::Vector2dF& offset, float opacity) {
    if (!CanAnimateReflow()) return;
    if (offset.IsZero() && opacity == 1.f) {
      StopReflowAnimation();
      return;
    }
    const bool continuing = ReflowAnimating();
    const auto now = base::TimeTicks::Now();
    if (!continuing) reflow_fade_end_ = now + base::Milliseconds(110);
    reflow_timer_.Stop();
    // Keep the existing layer when retargeting. Finishing the old animation
    // would snap to its destination and destroy the layer before recreating it.
    if (!layer()) {
      reflow_owns_layer_ = true;
      SetPaintToLayer();
    }
    layer()->SetFillsBoundsOpaquely(false);
    layer()->GetAnimator()->AbortAllAnimations();
    gfx::Transform transform;
    transform.Translate(offset.x(), offset.y());
    layer()->SetTransform(transform);
    const auto fade_duration = std::max(base::TimeDelta(), reflow_fade_end_ - now);
    layer()->SetOpacity(fade_duration.is_zero() ? 1.f : opacity);
    {
      ui::ScopedLayerAnimationSettings settings(layer()->GetAnimator());
      settings.SetTransitionDuration(base::Milliseconds(160));
      settings.SetTweenType(gfx::Tween::EASE_OUT);
      settings.SetPreemptionStrategy(ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
      layer()->SetTransform(gfx::Transform());
    }
    {
      ui::ScopedLayerAnimationSettings settings(layer()->GetAnimator());
      settings.SetTransitionDuration(fade_duration);
      settings.SetTweenType(gfx::Tween::EASE_OUT);
      settings.SetPreemptionStrategy(ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
      layer()->SetOpacity(1.f);
    }
    const auto duration = offset.IsZero() ? fade_duration : base::Milliseconds(160);
    reflow_timer_.Start(FROM_HERE, duration + base::Milliseconds(20),
                        this, &FileRow::FinishReflowAnimation);
  }
  void SetTooltipCallback(base::RepeatingCallback<std::u16string()> callback) {
    tooltip_callback_ = std::move(callback);
  }
  views::View* GetTooltipHandlerForPoint(const gfx::Point& point) override {
    return GetVisible() && GetCanProcessEventsWithinSubtree() && HitTestPoint(point) ? this : nullptr;
  }
  std::u16string GetRenderedTooltipText(const gfx::Point&) const override {
    return tooltip_callback_ ? tooltip_callback_.Run() : std::u16string();
  }
  void SetDragDoneCallback(base::RepeatingClosure callback) { drag_done_ = std::move(callback); }
  void OnDragDone() override {
    views::Button::OnDragDone();
    if (drag_done_) drag_done_.Run();
  }
  KeyClickAction GetKeyClickActionForEvent(const ui::KeyEvent& event) override {
    if (event.key_code() == ui::VKEY_RETURN) return KeyClickAction::kOnKeyPress;
    return views::Button::GetKeyClickActionForEvent(event);
  }
  void SetVirtualPosition(size_t position, size_t count) {
    if (position_ == position && count_ == count) return;
    position_ = position; count_ = count;
    GetViewAccessibility().SetPosInSet(static_cast<int>(position + 1));
    GetViewAccessibility().SetSetSize(static_cast<int>(count));
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    if (!icons_) views::Button::OnPaintBackground(canvas);
    cc::PaintFlags flags;
    if (icons_ && background()) {
      flags.setAntiAlias(true);
      flags.setColor(background()->color().ResolveToSkColor(GetColorProvider()));
      canvas->DrawRoundRect(GetLocalBounds(), 16, flags);
    }
    const float opacity = GetState() == STATE_PRESSED ? 0.18f :
        0.09f * static_cast<float>(hover_animation().GetCurrentValue());
    if (opacity <= 0.0f) return;
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
    flags.setAlphaf(opacity);
    if (icons_) {
      flags.setAntiAlias(true);
      canvas->DrawRoundRect(GetLocalBounds(), 16, flags);
    } else {
      canvas->DrawRect(GetLocalBounds(), flags);
    }
  }
 private:
  base::RepeatingClosure pointer_motion_;
  void FinishReflowAnimation() {
    // The one-shot timer is no longer running when its callback is entered.
    if (layer()) {
      layer()->GetAnimator()->StopAnimating();
      layer()->SetTransform(gfx::Transform());
      layer()->SetOpacity(1.f);
      if (reflow_owns_layer_) DestroyLayer();
    }
    reflow_owns_layer_ = false;
  }
  base::OneShotTimer reflow_timer_;
  base::TimeTicks reflow_fade_end_;
  bool reflow_owns_layer_ = false;
  base::RepeatingCallback<std::u16string()> tooltip_callback_;
  base::RepeatingClosure drag_done_;
  size_t position_ = std::numeric_limits<size_t>::max(), count_ = 0;
  bool icons_;
};
BEGIN_METADATA(FileRow)
END_METADATA
// Keep sidebar contents at a readable width while the outer viewport shrinks.
class SidebarViewport : public views::View {
  METADATA_HEADER(SidebarViewport, views::View)
 public:
  SidebarViewport() {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetMasksToBounds(true);
  }
  gfx::Size GetMinimumSize() const override { return gfx::Size(); }
  void Layout(PassKey) override {
    for (const auto& child : children()) child->SetBounds(0, 0, std::max(220, width()), height());
    LayoutSuperclass<views::View>(this);
  }
};
BEGIN_METADATA(SidebarViewport)
END_METADATA
// Enlarge input targeting without changing the visible divider or pane sizes.
class ResizeGripTargeter : public views::ViewTargeterDelegate {
 public:
  ResizeGripTargeter(views::View* grip, bool vertical, base::RepeatingCallback<bool()> dragging)
      : grip_(grip), vertical_(vertical), dragging_(std::move(dragging)) {}
  views::View* TargetForRect(views::View* root, const gfx::Rect& rect) override {
    auto* grip = grip_.view();
    if (grip && root == grip->parent() && grip->GetVisible() && grip->GetEnabled()) {
      auto bounds = grip->GetMirroredBounds();
      bounds.Inset(vertical_ ? gfx::Insets::VH(-4, 0) : gfx::Insets::VH(0, -4));
      if (dragging_.Run() || bounds.Intersects(rect)) return grip;
    }
    return views::ViewTargeterDelegate::TargetForRect(root, rect);
  }
 private:
  views::ViewTracker grip_;
  bool vertical_;
  base::RepeatingCallback<bool()> dragging_;
};
void RestoreCursorAfterResize(views::View* grip) {
  auto* widget = grip->GetWidget();
  if (!widget || widget->IsClosed()) return;
  auto* root = widget->GetRootView();
  auto point = display::Screen::Get()->GetCursorScreenPoint();
  views::View::ConvertPointFromScreen(root, &point);
  ui::Cursor cursor(ui::mojom::CursorType::kPointer);
  if (root->HitTestPoint(point)) {
    if (auto* target = root->GetEventHandlerForPoint(point)) {
      auto local = point;
      views::View::ConvertPointToTarget(root, target, &local);
      ui::MouseEvent event(ui::EventType::kMouseMoved, local, point, base::TimeTicks::Now(), 0, 0);
      cursor = target->GetCursor(event);
      if (cursor.type() == ui::mojom::CursorType::kNull)
        cursor = ui::Cursor(ui::mojom::CursorType::kPointer);
    }
  }
  widget->SetCursor(cursor);
}
int SnapResizePosition(int position, int target, bool& snapped) {
  snapped = std::abs(position - target) <= (snapped ? 16 : 8);
  return snapped ? target : position;
}
constexpr int kPaneOuterInset = 4;
class SidebarResizeGrip : public views::ResizeArea {
  METADATA_HEADER(SidebarResizeGrip, views::ResizeArea)
 public:
  SidebarResizeGrip(views::ResizeAreaDelegate* delegate, base::RepeatingClosure restore)
      : views::ResizeArea(delegate), delegate_(delegate), restore_(std::move(restore)) {}
  void SetCollapsed(bool collapsed) { collapsed_ = collapsed; SchedulePaint(); }
  bool IsDragging() const { return pressed_; }
  void OnMouseEntered(const ui::MouseEvent&) override { hovered_ = true; SchedulePaint(); }
  void OnMouseExited(const ui::MouseEvent&) override { hovered_ = false; SchedulePaint(); }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (!event.IsOnlyLeftMouseButton()) return false;
    if (event.GetClickCount() == 2) { restore_.Run(); return true; }
    gfx::Point point = event.location();
    views::View::ConvertPointToScreen(this, &point);
    initial_x_ = point.x(); amount_ = 0; pressed_ = true;
    delegate_->OnResize(0, false); SchedulePaint();
    return true;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!pressed_ || !event.IsLeftMouseButton()) return false;
    UpdateAmount(event); delegate_->OnResize(amount_, false);
    if (GetWidget()) GetWidget()->SetCursor(GetCursor(event));
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (!pressed_) return;
    UpdateAmount(event); Finish();
  }
  void OnMouseCaptureLost() override { if (pressed_) Finish(); }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    if (collapsed_ && !hovered_ && !pressed_) return;
    const auto color = GetColorProvider()->GetColor((hovered_ || pressed_) ? ui::kColorSysPrimary : ui::kColorSysDivider);
    canvas->FillRect(gfx::Rect((width() - 2) / 2, kPaneOuterInset, 2,
                              std::max(0, height() - 2 * kPaneOuterInset)), color);
  }
 private:
  void UpdateAmount(const ui::MouseEvent& event) {
    gfx::Point point = event.location();
    views::View::ConvertPointToScreen(this, &point);
    amount_ = (point.x() - initial_x_) * (base::i18n::IsRTL() ? -1 : 1);
  }
  void Finish() {
    pressed_ = false;
    delegate_->OnResize(amount_, true); SchedulePaint();
    RestoreCursorAfterResize(this);
  }
  raw_ptr<views::ResizeAreaDelegate> delegate_;
  base::RepeatingClosure restore_;
  int initial_x_ = 0, amount_ = 0;
  bool hovered_ = false, pressed_ = false, collapsed_ = false;
};
BEGIN_METADATA(SidebarResizeGrip)
END_METADATA
class ColumnResizeGrip : public views::ResizeArea {
  METADATA_HEADER(ColumnResizeGrip, views::ResizeArea)
 public:
  explicit ColumnResizeGrip(views::ResizeAreaDelegate* delegate) : views::ResizeArea(delegate) {}
  bool IsDragging() const { return pressed_; }
  void OnMouseEntered(const ui::MouseEvent&) override { hovered_ = true; SchedulePaint(); }
  void OnMouseExited(const ui::MouseEvent&) override { hovered_ = false; SchedulePaint(); }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    pressed_ = views::ResizeArea::OnMousePressed(event);
    SchedulePaint();
    return pressed_;
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    const bool handled = views::ResizeArea::OnMouseDragged(event);
    if (handled && GetWidget()) GetWidget()->SetCursor(GetCursor(event));
    return handled;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    pressed_ = false;
    views::ResizeArea::OnMouseReleased(event);
    SchedulePaint();
    RestoreCursorAfterResize(this);
  }
  void OnMouseCaptureLost() override {
    pressed_ = false;
    views::ResizeArea::OnMouseCaptureLost();
    SchedulePaint();
    RestoreCursorAfterResize(this);
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    // Keep an eight-DIP drag target, but match the thin sidebar divider.
    canvas->FillRect(gfx::Rect(std::max(0, width() - 2), 0, 2, height()), GripColor());
  }

 protected:
  SkColor GripColor() const {
    return GetColorProvider()->GetColor(
        (hovered_ || pressed_) ? ui::kColorSysPrimary : ui::kColorSysDivider);
  }

 private:
  bool hovered_ = false;
  bool pressed_ = false;
};
BEGIN_METADATA(ColumnResizeGrip)
END_METADATA
class ColumnSortButton : public views::LabelButton {
  METADATA_HEADER(ColumnSortButton, views::LabelButton)
 public:
  ColumnSortButton(base::RepeatingClosure callback, const std::u16string& title,
                   std::optional<bool> ascending)
      : views::LabelButton(std::move(callback), title) {
    SetHorizontalAlignment(gfx::ALIGN_LEFT);
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 6)));
    SetTooltipText(title);
    SetAccessibleName(title);
    AnimateButton(this);
    views::InstallRoundRectHighlightPathGenerator(this, gfx::Insets(), 0);
    if (ascending.has_value()) {
      arrow_ = AddChildView(std::make_unique<views::Label>(*ascending ? u"↑" : u"↓"));
      arrow_->SetProperty(views::kViewIgnoredByLayoutKey, true);
      arrow_->SetCanProcessEventsWithinSubtree(false);
      GetViewAccessibility().SetSortDirection(*ascending ?
          ax::mojom::SortDirection::kAscending : ax::mojom::SortDirection::kDescending);
    }
  }
  void Layout(PassKey) override {
    LayoutSuperclass<views::LabelButton>(this);
    if (!arrow_) return;
    const int arrow_width = std::min(16, width());
    arrow_->SetBounds(width() - arrow_width, 0, arrow_width, height());
    auto bounds = label()->bounds();
    bounds.set_width(std::max(0, width() - arrow_width - 6 - bounds.x()));
    label()->SetBoundsRect(bounds);
  }
 private:
  raw_ptr<views::Label> arrow_ = nullptr;
};
BEGIN_METADATA(ColumnSortButton)
END_METADATA
class ColumnHeader : public views::View, public views::ResizeAreaDelegate {
  METADATA_HEADER(ColumnHeader, views::View)
 public:
  ColumnHeader(std::u16string title, std::optional<bool> ascending, base::RepeatingClosure sort,
               base::RepeatingCallback<void(int)> resize)
      : resize_(std::move(resize)) {
    AddChildView(std::make_unique<ColumnSortButton>(std::move(sort), title, ascending));
    auto* grip = AddChildView(std::make_unique<ColumnResizeGrip>(this));
    grip->SetPreferredSize(gfx::Size(8, 0));
    grip->SetAccessibleName(title);
  }
  void Layout(PassKey) override {
    // The drag target overlaps the button instead of reserving blank space
    // between its highlight and the two-DIP separator at the column edge.
    children()[0]->SetBounds(0, 0, std::max(0, width() - 2), height());
    const int grip_width = std::min(8, width());
    children()[1]->SetBounds(width() - grip_width, 0, grip_width, height());
    LayoutSuperclass<views::View>(this);
  }
  void OnResize(int amount, bool done) override {
    if (!start_) start_ = width();
    pending_width_ = std::clamp(*start_ + amount, 60, 1000);
    if (done) {
      resize_timer_.Stop(); FlushResize(); start_.reset();
    } else if (!resize_timer_.IsRunning()) {
      resize_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &ColumnHeader::FlushResize);
    }
  }
 private:
  void FlushResize() {
    if (pending_width_ != applied_width_) {
      applied_width_ = pending_width_; resize_.Run(pending_width_);
    }
  }
  int pending_width_ = 0, applied_width_ = 0;
  base::OneShotTimer resize_timer_;
  std::optional<int> start_;
  base::RepeatingCallback<void(int)> resize_;
};
BEGIN_METADATA(ColumnHeader)
END_METADATA
class SelectionMarqueeBackground : public views::Background {
 public:
  void Paint(gfx::Canvas* canvas, views::View* view) const override {
    cc::PaintFlags flags;
    flags.setColor(view->GetColorProvider()->GetColor(ui::kColorSysPrimary));
    flags.setAlphaf(0.12f);
    canvas->DrawRect(view->GetLocalBounds(), flags);
  }
  void OnViewThemeChanged(views::View* view) override { view->SchedulePaint(); }
};
constexpr int kListHeaderHeight = 28;
class FileLayout : public views::View {
  METADATA_HEADER(FileLayout, views::View)
 public:
  void SetPointerMotionCallback(base::RepeatingClosure callback) {
    pointer_motion_ = std::move(callback);
  }
  void OnMouseMoved(const ui::MouseEvent& event) override {
    views::View::OnMouseMoved(event);
    if (!event.IsSynthesized() && pointer_motion_) pointer_motion_.Run();
  }

  explicit FileLayout(bool icons, int icon_size, int min_width)
      : drag_handler_(this), icons_(icons), row_height_(icons ? std::max(96, icon_size + 51) + 12 : std::max(36, icon_size + 12)), min_width_(min_width) {
    AddPreTargetHandler(&drag_handler_);
    SetLayoutManager(std::make_unique<RowLayout>());
    SetEventTargeter(std::make_unique<views::ViewTargeter>(std::make_unique<FileEventTargeter>(this)));
  }
  ~FileLayout() override { RemovePreTargetHandler(&drag_handler_); }
  void VisibilityChanged(views::View* starting_from, bool is_visible) override {
    views::View::VisibilityChanged(starting_from, is_visible);
    // Page restoration can pass through temporary widths (for example while
    // restoring the sidebar). Those are not user-requested column changes.
    restoring_columns_ = true;
    column_restore_timer_.Stop();
    column_transition_pending_ = false;
    column_snapshots_.clear();
    column_fade_deadline_ = base::TimeTicks();
    animation_viewport_ = gfx::Rect();
    last_layout_columns_.reset();
    for (const auto& [index, view] : slots_)
      if (auto* row = views::AsViewClass<FileRow>(view.get())) row->StopReflowAnimation();
  }
  void SetSelectionCallback(base::RepeatingCallback<void(gfx::Rect, bool, bool, bool)> callback) {
    selection_ = std::move(callback);
    marquee_ = AddChildView(std::make_unique<views::View>());
    marquee_->SetCanProcessEventsWithinSubtree(false);
    marquee_->SetPaintToLayer(); marquee_->layer()->SetFillsBoundsOpaquely(false);
    marquee_->SetBackground(std::make_unique<SelectionMarqueeBackground>());
    marquee_->SetBorder(views::CreateSolidBorder(1, ui::kColorSysOutline));
    marquee_->SetVisible(false);
  }
  views::View* GetTooltipHandlerForPoint(const gfx::Point& point) override {
    if (!GetVisible() || !GetCanProcessEventsWithinSubtree() || !HitTestPoint(point)) return nullptr;
    if (header_ && header_->GetVisible() && header_->GetMirroredBounds().Contains(point)) {
      auto local = point;
      views::View::ConvertPointToTarget(this, header_, &local);
      if (auto* target = header_->GetTooltipHandlerForPoint(local)) return target;
      return this;
    }
    if (auto* row = FileAtPoint(point)) {
      auto local = point;
      views::View::ConvertPointToTarget(this, row, &local);
      return row->GetTooltipHandlerForPoint(local);
    }
    return this;
  }
  bool OnMousePressed(const ui::MouseEvent& event) override { return event.IsOnlyLeftMouseButton(); }
  bool OnMouseDragged(const ui::MouseEvent&) override { return true; }
  void OnMouseCaptureLost() override { FinishDrag(); }
  void SetItemCount(size_t count) {
    for (const auto& [index, view] : slots_)
      if (auto* row = views::AsViewClass<FileRow>(view.get())) row->StopReflowAnimation();
    column_snapshots_.clear();
    column_transition_pending_ = false;
    last_layout_columns_.reset();
    column_fade_deadline_ = base::TimeTicks();
    item_count_ = count; slots_.clear(); PreferredSizeChanged(); InvalidateLayout();
  }
  void SetHeader(views::View* header) { header_ = header; }
  void SetViewportCallback(base::RepeatingClosure callback) { viewport_changed_ = std::move(callback); }
  void SetResizing(bool resizing) {
    resize_columns_ = resizing ? std::make_optional(Columns()) : std::nullopt;
    PreferredSizeChanged();
    InvalidateLayout();
  }
  int ColumnsForWidth(int available) const {
    if (!icons_) return 1;
    const int natural = std::max(1, available / row_height_);
    if (!resize_columns_) return natural;
    // Require 16 extra DIPs before adding a column; removing still happens
    // as soon as the current columns no longer fit.
    const int retained = std::min(*resize_columns_, natural);
    return std::max(retained, std::max(1, (available - 16) / row_height_));
  }
  int Columns() const { return ColumnsForWidth(width()); }
  int RowHeight() const { return row_height_; }
  bool BoundaryRowReached(double offset, int viewport_height, bool bottom) const {
    if (!item_count_) return false;
    const int header = icons_ ? 0 : kListHeaderHeight;
    if (!bottom) return offset < header + row_height_;
    const size_t last_row = (item_count_ - 1) / Columns();
    return offset + viewport_height > header + static_cast<double>(last_row) * row_height_;
  }
  gfx::Rect ItemBounds(size_t index) const {
    const int columns = Columns();
    const int cell = width() / columns;
    const int y = static_cast<int>(std::min<size_t>(index / columns * row_height_ + (icons_ ? 0 : kListHeaderHeight),
        std::numeric_limits<int>::max() - row_height_ - 56));
    const int x = static_cast<int>(index % columns) * cell;
    if (icons_) {
      const int edge = std::max(0, std::min(cell - 12, row_height_ - 12));
      const int left = (width() - columns * (edge + 12)) / 2 + 6;
      return gfx::Rect(left + static_cast<int>(index % columns) * (edge + 12), y + 6, edge, edge);
    }
    return gfx::Rect(x, y, cell, row_height_);
  }
  std::pair<size_t, size_t> ItemsInRect(const gfx::Rect& rect) const {
    if (rect.IsEmpty()) return {0, 0};
    const int header = icons_ ? 0 : kListHeaderHeight;
    const size_t first = static_cast<size_t>(std::max(0, rect.y() - header) / row_height_) * Columns();
    const size_t last = (static_cast<size_t>(std::max(0, rect.bottom() - 1 - header) / row_height_) + 1) * Columns();
    return {std::min(first, item_count_), std::min(last, item_count_)};
  }
  void CaptureColumnTransition(int columns) {
    const bool changed = last_layout_columns_ && *last_layout_columns_ != columns;
    last_layout_columns_ = columns;
    if (!IsDrawn()) return;
    if (restoring_columns_) {
      // Resume only after the restore layout has been quiet for a short time.
      column_restore_timer_.Start(FROM_HERE, base::Milliseconds(80), this,
                                  &FileLayout::FinishColumnRestore);
      return;
    }
    if (!icons_ || !GetFileViewPreferences().animate_folders || animation_viewport_.IsEmpty()) return;
    const bool moving = std::ranges::any_of(slots_, [&](const auto& slot) {
      const auto* row = views::AsViewClass<FileRow>(slot.second.get());
      return row && row->ReflowAnimating() && row->bounds() != ItemBounds(slot.first);
    });
    if ((!changed && !moving) || column_transition_pending_) return;
    column_transition_pending_ = true;
    column_snapshots_.clear();
    for (const auto& [index, view] : slots_) {
      auto* row = views::AsViewClass<FileRow>(view.get());
      if (!row || !row->CanAnimateReflow()) continue;
      gfx::RectF displayed(row->GetMirroredBounds());
      if (row->layer()) displayed.Offset(row->layer()->transform().To2dTranslation());
      if (!displayed.Intersects(gfx::RectF(animation_viewport_))) continue;
      column_snapshots_.emplace(index, ColumnSnapshot{
          row->GetWeakPtr(), gfx::PointF(displayed.x() - animation_viewport_.x(),
                                       displayed.y() - animation_viewport_.y()),
          row->layer() ? row->layer()->opacity() : 1.f});
    }
  }
  void CommitColumnTransition(const gfx::Rect& viewport) {
    animation_viewport_ = viewport;
    if (!IsDrawn() || restoring_columns_) return;
    if (!column_transition_pending_) return;
    column_transition_pending_ = false;
    column_fade_deadline_ = base::TimeTicks::Now() + base::Milliseconds(180);
    for (const auto& [index, view] : slots_) {
      auto* row = views::AsViewClass<FileRow>(view.get());
      if (!row) continue;
      if (!viewport.Intersects(row->bounds())) {
        row->StopReflowAnimation();
        continue;
      }
      const auto old = column_snapshots_.find(index);
      if (old == column_snapshots_.end() || old->second.view.get() != row) {
        row->AnimateReflow(gfx::Vector2dF(), 0.f);
      } else {
        const auto target = row->GetMirroredBounds();
        row->AnimateReflow(gfx::Vector2dF(
            old->second.origin.x() - (target.x() - viewport.x()),
            old->second.origin.y() - (target.y() - viewport.y())), old->second.opacity);
      }
    }
    column_snapshots_.clear();
  }
  void AnimateNewColumnItem(FileRow* row) {
    if (!restoring_columns_ && IsDrawn() && base::TimeTicks::Now() < column_fade_deadline_ &&
        animation_viewport_.Intersects(row->bounds()))
      row->AnimateReflow(gfx::Vector2dF(), 0.f);
  }
  void ResetItemPositions() { slots_.clear(); }
  void PlaceItem(size_t index, views::View* view) {
    slots_[index] = view;
    view->SetBoundsRect(ItemBounds(index));
  }
  std::optional<size_t> IndexOf(views::View* view) const {
    for (const auto& [index, row] : slots_) if (row->Contains(view)) return index;
    return std::nullopt;
  }
  bool HasMissingItems(size_t first, size_t last) const {
    auto slot = slots_.lower_bound(first);
    for (size_t index = first; index < last; ++index, ++slot)
      if (slot == slots_.end() || slot->first != index) return true;
    return false;
  }
  std::vector<size_t> MissingItems(size_t first, size_t last) const {
    std::vector<size_t> missing;
    auto slot = slots_.lower_bound(first);
    for (size_t index = first; index < last; ++index) {
      if (slot != slots_.end() && slot->first == index) ++slot;
      else missing.push_back(index);
    }
    return missing;
  }
  std::vector<size_t> ItemsOutside(size_t first, size_t last) const {
    std::vector<size_t> outside;
    const auto before = slots_.lower_bound(first);
    for (auto slot = slots_.begin(); slot != before; ++slot) outside.push_back(slot->first);
    for (auto slot = slots_.lower_bound(last); slot != slots_.end(); ++slot) outside.push_back(slot->first);
    return outside;
  }
  void OrderItems() {
    std::vector<views::View*> desired;
    if (header_) desired.push_back(header_);
    for (const auto& [index, row] : slots_) desired.push_back(row);
    // Hidden pool rows need no visual/focus ordering. Leave their slots in
    // place instead of moving every newly recycled row to the end.
    std::set<views::View*> pooled(recycled_rows_.begin(), recycled_rows_.end());
    if (marquee_) desired.push_back(marquee_);
    std::vector<views::View*> interleaved;
    interleaved.reserve(children().size());
    auto active = desired.begin();
    for (const auto& child : children()) {
      if (pooled.contains(child.get())) interleaved.push_back(child.get());
      else if (active != desired.end()) interleaved.push_back(*active++);
    }
    desired = std::move(interleaved);
    if (desired.size() == children().size() && std::equal(desired.begin(), desired.end(), children().begin(),
        [](views::View* expected, const auto& child) { return expected == child.get(); })) return;
    std::map<views::View*, size_t> positions;
    for (size_t index = 0; index < children().size(); ++index) positions.emplace(children()[index].get(), index);
    // Keep the longest already-ordered subsequence. Moving a recycled row
    // across the viewport must not reorder every unchanged row it passes.
    std::vector<size_t> tails, predecessor(desired.size(), desired.size());
    for (size_t index = 0; index < desired.size(); ++index) {
      auto tail = std::lower_bound(tails.begin(), tails.end(), positions.at(desired[index]),
          [&](size_t candidate, size_t position) { return positions.at(desired[candidate]) < position; });
      if (tail != tails.begin()) predecessor[index] = *(tail - 1);
      if (tail == tails.end()) tails.push_back(index);
      else *tail = index;
    }
    std::set<views::View*> keep;
    if (!tails.empty())
      for (size_t index = tails.back(); index != desired.size(); index = predecessor[index]) keep.insert(desired[index]);
    views::View* next = nullptr;
    for (auto item = desired.rbegin(); item != desired.rend(); ++item) {
      if (!keep.contains(*item)) {
        const auto current = std::ranges::find(children(), *item);
        size_t target = children().size() - 1;
        if (next) {
          const auto anchor = std::ranges::find(children(), next);
          target = static_cast<size_t>(anchor - children().begin()) - (current < anchor ? 1 : 0);
        }
        ReorderChildView(*item, target);
      }
      next = *item;
    }
  }
  FileRow* TakeRecycledRow() {
    if (recycled_rows_.empty()) return nullptr;
    auto* row = recycled_rows_.back().get();
    recycled_rows_.pop_back();
    return row;
  }
  void RecycleRow(FileRow* row) {
    row->StopReflowAnimation();
    if (recycled_rows_.size() >= 32) { RemoveChildViewT(row); return; }
    row->SetVisible(false);
    recycled_rows_.push_back(row);
  }
  void ForgetItems(const std::set<views::View*>& removed) {
    std::erase_if(slots_, [&removed](const auto& slot) { return removed.contains(slot.second.get()); });
  }
  void SetMinimumWidth(int width) { if (min_width_ != width) { min_width_ = width; PreferredSizeChanged(); } }
  void SetBottomPadding(int padding) {
    padding = std::max(56, padding);
    if (bottom_padding_ == padding) return;
    bottom_padding_ = padding; PreferredSizeChanged();
  }
  void SetIconSize(int size) {
    row_height_ = icons_ ? std::max(96, size + 51) + 12 : std::max(36, size + 12); PreferredSizeChanged(); InvalidateLayout();
  }
  gfx::Size CalculatePreferredSize(const views::SizeBounds& available) const override {
    int w = available.width().is_bounded() ? available.width().value() : 760;
    if (parent() && parent()->width() > 0) w = std::min(w, parent()->width());
    w = std::max(min_width_, w);
    const int columns = ColumnsForWidth(w);
    // Geometry describes the entire model, independently of instantiated views.
    const size_t height = ((item_count_ + columns - 1) / columns) * row_height_ + (icons_ ? 0 : kListHeaderHeight) + bottom_padding_;
    return gfx::Size(w, static_cast<int>(std::min<size_t>(height, std::numeric_limits<int>::max() - row_height_ - 56)));
  }
  void Layout(PassKey) override {
    LayoutSuperclass<views::View>(this);
    // Defer adding/removing rows until layout/event dispatch has unwound.
    if (viewport_changed_) viewport_changed_.Run();
  }
 private:
  base::RepeatingClosure pointer_motion_;
  void FinishColumnRestore() {
    if (!IsDrawn()) return;
    last_layout_columns_ = Columns();
    restoring_columns_ = false;
  }
  bool HasColumnAnimation() const {
    return base::TimeTicks::Now() < column_fade_deadline_;
  }
  // Use the slot index normally, and transformed bounds during a short reflow.
  // The outer file button continues to own input while its layer is moving.
  views::View* FileAtPoint(const gfx::Point& point) const {
    const int header = icons_ ? 0 : kListHeaderHeight;
    if (!GetLocalBounds().Contains(point) || point.y() < header || width() <= 0) return nullptr;
    if (HasColumnAnimation()) {
      for (auto slot = slots_.rbegin(); slot != slots_.rend(); ++slot) {
        auto* row = slot->second.get();
        if (!row->GetVisible() || !row->GetCanProcessEventsWithinSubtree()) continue;
        auto local = point;
        views::View::ConvertPointToTarget(this, row, &local);
        if (row->HitTestPoint(local)) return row;
      }
      return nullptr;
    }
    const int columns = Columns();
    int column = 0;
    if (icons_) {
      const int edge = std::max(0, std::min(width() / columns - 12, row_height_ - 12));
      const int left = (width() - columns * (edge + 12)) / 2 + 6;
      const int x = GetMirroredXWithWidthInView(point.x(), 1) - left;
      if (x < 0) return nullptr;
      column = x / (edge + 12);
      if (column >= columns) return nullptr;
    }
    const size_t index = static_cast<size_t>((point.y() - header) / row_height_) * columns + column;
    const auto slot = slots_.find(index);
    if (slot == slots_.end()) return nullptr;
    auto* row = slot->second.get();
    if (!row->GetVisible() || !row->GetCanProcessEventsWithinSubtree()) return nullptr;
    const auto bounds = row->GetMirroredBounds();
    if (!bounds.Contains(point) || !row->HitTestPoint(gfx::Point(point.x() - bounds.x(), point.y() - bounds.y()))) return nullptr;
    return row;
  }
  class FileEventTargeter : public views::ViewTargeterDelegate {
   public:
    explicit FileEventTargeter(FileLayout* owner) : owner_(owner) {}
    views::View* TargetForRect(views::View* root, const gfx::Rect& rect) override {
      if (root != owner_)
        return views::ViewTargeterDelegate::TargetForRect(root, rect);
      if (!views::UsePointBasedTargeting(rect)) return TargetForArea(rect);
      const auto point = rect.CenterPoint();
      auto* header = owner_->header_.get();
      if (header && header->GetVisible() && header->GetCanProcessEventsWithinSubtree() &&
          header->GetMirroredBounds().Contains(point)) {
        auto local = point;
        views::View::ConvertPointToTarget(root, header, &local);
        if (header->HitTestPoint(local)) return header->GetEventHandlerForPoint(local);
      }
      if (auto* row = owner_->FileAtPoint(point)) return row;
      // Tile gaps, unpopulated rows and the footer padding belong to the
      // existing background selection/drop target. Mouse capture is unchanged.
      return root;
    }
   private:
    views::View* TargetForArea(const gfx::Rect& rect) {
      // A fractional scroll transform can expand a 1x1 pointer rect during
      // enclosing conversion. Keep both these rects and real touch regions
      // indexed, preserving ViewTargeterDelegate's 60% overlap/nearest-center
      // rule and its center-point fallback.
      views::View* area_target = nullptr;
      views::View* point_target = nullptr;
      int distance = std::numeric_limits<int>::max();
      auto consider = [&](views::View* target, const gfx::Rect& bounds) {
        if (views::PercentCoveredBy(bounds, rect) >= 0.6f) {
          const int candidate_distance = views::DistanceSquaredFromCenterToPoint(rect.CenterPoint(), bounds);
          if (!area_target || candidate_distance < distance) {
            area_target = target;
            distance = candidate_distance;
          }
        }
      };
      auto clipped = rect;
      clipped.Intersect(owner_->GetLocalBounds());
      const auto [first, last] = owner_->ItemsInRect(clipped);
      const int columns = owner_->Columns();
      int first_column = 0, last_column = columns - 1;
      if (owner_->icons_ && !clipped.IsEmpty()) {
        const int edge = std::max(0, std::min(owner_->width() / columns - 12, owner_->row_height_ - 12));
        const int left = (owner_->width() - columns * (edge + 12)) / 2 + 6;
        const int x = owner_->GetMirroredXWithWidthInView(clipped.x(), clipped.width()) - left;
        first_column = std::clamp(x / (edge + 12), 0, columns - 1);
        last_column = std::clamp((x + clipped.width() - 1) / (edge + 12), 0, columns - 1);
      }
      const bool animated = owner_->HasColumnAnimation();
      const auto begin = animated ? owner_->slots_.begin() : owner_->slots_.lower_bound(first);
      auto end = animated ? owner_->slots_.end() : owner_->slots_.lower_bound(last);
      // Active child order follows the index; reverse it to retain Z-order
      // tie breaking. Hidden pooled rows and the marquee are not candidates.
      while (end != begin) {
        --end;
        const int column = static_cast<int>(end->first % columns);
        if (!animated && (column < first_column || column > last_column)) continue;
        auto* row = end->second.get();
        if (!row->GetVisible() || !row->GetCanProcessEventsWithinSubtree()) continue;
        gfx::RectF bounds_f(row->GetLocalBounds());
        views::View::ConvertRectToTarget(row, owner_, &bounds_f);
        const auto bounds = gfx::ToEnclosingRect(bounds_f);
        if (!bounds.Intersects(rect)) continue;
        gfx::RectF local_f(rect);
        views::View::ConvertRectToTarget(owner_, row, &local_f);
        const auto local = gfx::ToEnclosingRect(local_f);
        if (!row->HitTestRect(local)) continue;
        consider(row, bounds);
        if (!area_target && !point_target && row->HitTestPoint(local.CenterPoint())) point_target = row;
      }
      auto* header = owner_->header_.get();
      if (header && header->GetVisible() && header->GetCanProcessEventsWithinSubtree() &&
          header->GetMirroredBounds().Intersects(rect)) {
        gfx::RectF local_f(rect);
        views::View::ConvertRectToTarget(owner_, header, &local_f);
        const auto local = gfx::ToEnclosingRect(local_f);
        if (header->HitTestRect(local)) {
          auto* target = header->GetEventHandlerForRect(local);
          gfx::RectF bounds(target->GetLocalBounds());
          views::View::ConvertRectToTarget(target, owner_, &bounds);
          consider(target, gfx::ToEnclosingRect(bounds));
          if (!area_target && !point_target && header->HitTestPoint(local.CenterPoint()))
            point_target = header->GetEventHandlerForPoint(local.CenterPoint());
        }
      }
      return area_target ? area_target : point_target ? point_target : owner_.get();
    }
    raw_ptr<FileLayout> owner_;
  };
  // Having a layout manager lets View skip clean child subtrees. A bare View
  // would lay out every retained file row on every ancestor layout pass.
  class RowLayout : public views::LayoutManager {
   public:
    void Layout(views::View* host) override {
      auto* files = static_cast<FileLayout*>(host);
      files->CaptureColumnTransition(files->Columns());
      if (files->resize_columns_) files->resize_columns_ = files->Columns();
      if (files->header_) files->header_->SetBounds(0, 0, files->width(), kListHeaderHeight);
      for (const auto& [index, row] : files->slots_) {
        const auto bounds = files->ItemBounds(index);
        if (row->bounds() != bounds) row->SetBoundsRect(bounds);
      }
    }
    gfx::Size GetPreferredSize(const views::View* host) const override {
      return GetPreferredSize(host, {});
    }
    gfx::Size GetPreferredSize(const views::View* host, const views::SizeBounds& available) const override {
      return static_cast<const FileLayout*>(host)->CalculatePreferredSize(available);
    }
  };
  void FinishDrag() {
    if (dragging_ && selection_) selection_.Run(gfx::Rect(), additive_, false, true);
    dragging_ = false; pressed_ = false;
    if (marquee_) marquee_->SetVisible(false);
  }
  class DragHandler : public ui::EventHandler {
   public:
    explicit DragHandler(FileLayout* owner) : owner_(owner) {}
    void OnMouseEvent(ui::MouseEvent* event) override {
      if (!owner_->selection_) return;
      const auto type = event->type();
      if (type != ui::EventType::kMousePressed && type != ui::EventType::kMouseDragged &&
          type != ui::EventType::kMouseReleased) return;
      auto point = event->location();
      views::View::ConvertPointToTarget(static_cast<views::View*>(event->target()), owner_, &point);
      if (type == ui::EventType::kMousePressed && event->IsOnlyLeftMouseButton()) {
        if (!owner_->icons_ && point.y() < kListHeaderHeight) return;
        for (auto* target = static_cast<views::View*>(event->target()); target && target != owner_; target = target->parent())
          if (target->drag_controller()) { owner_->pressed_ = false; return; }
        owner_->pressed_ = true; owner_->dragging_ = false;
        owner_->start_ = point; owner_->additive_ = PrimaryModifierDown(*event);
        owner_->selection_.Run(gfx::Rect(), owner_->additive_, true, false);
      } else if (type == ui::EventType::kMouseDragged && owner_->pressed_) {
        if (!owner_->dragging_ && std::abs(point.x() - owner_->start_.x()) +
            std::abs(point.y() - owner_->start_.y()) < 5) return;
        owner_->dragging_ = true;
        owner_->SetMouseHandler(owner_);
        auto rect = gfx::BoundingRect(owner_->start_, point);
        owner_->marquee_->SetBoundsRect(rect); owner_->marquee_->SetVisible(true);
        owner_->selection_.Run(rect, owner_->additive_, false, false);
        owner_->ScrollRectToVisible(gfx::Rect(point, gfx::Size(1, 1)));
        event->SetHandled(); event->StopPropagation();
      } else if (type == ui::EventType::kMouseReleased) {
        const bool dragging = owner_->dragging_;
        if (owner_->pressed_ && !dragging && event->target() == owner_.get())
          owner_->selection_.Run(gfx::Rect(), owner_->additive_, false, false);
        owner_->FinishDrag();
        if (dragging) { event->SetHandled(); event->StopPropagation(); }
      }
    }
   private:
    raw_ptr<FileLayout> owner_;
  };
  size_t item_count_ = 0;
  std::map<size_t, raw_ptr<views::View>> slots_;
  std::vector<raw_ptr<FileRow>> recycled_rows_;
  raw_ptr<views::View> header_ = nullptr;
  base::RepeatingClosure viewport_changed_;
  DragHandler drag_handler_;
  base::RepeatingCallback<void(gfx::Rect, bool, bool, bool)> selection_;
  raw_ptr<views::View> marquee_ = nullptr;
  gfx::Point start_;
  bool pressed_ = false, dragging_ = false, additive_ = false;
  bool icons_;
  int row_height_;
  int min_width_;
  int bottom_padding_ = 56;
  std::optional<int> resize_columns_;
  struct ColumnSnapshot {
    base::WeakPtr<views::View> view;
    gfx::PointF origin;
    float opacity;
  };
  std::optional<int> last_layout_columns_;
  bool restoring_columns_ = false;
  base::OneShotTimer column_restore_timer_;
  gfx::Rect animation_viewport_;
  bool column_transition_pending_ = false;
  std::map<size_t, ColumnSnapshot> column_snapshots_;
  base::TimeTicks column_fade_deadline_;
};
BEGIN_METADATA(FileLayout)
END_METADATA
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
bool FolderScrollView::BoundaryRowReached(double offset, bool bottom) const {
  const auto* layout = views::AsViewClass<FileLayout>(contents());
  return layout && layout->BoundaryRowReached(offset, GetVisibleRect().height(), bottom);
}
#endif
class FileButton : public views::LabelButton {
  METADATA_HEADER(FileButton, views::LabelButton)
 public:
  views::View* NameAnchor() const { return label(); }
  FileButton(views::Button::PressedCallback callback, std::u16string name, bool icons, int icon_size)
      : views::LabelButton(std::move(callback), name), icons_(icons), icon_size_(icon_size) {
    AnimateButton(this);
    // The outer file row owns tooltip text and hit testing.
    label()->SetHandlesTooltips(false);
    label()->SetSubpixelRenderingEnabled(false);
    if (icons_) {
      label()->SetMultiLine(true);
      label()->SetMaxLines(2);
      label()->SetAllowCharacterBreak(true);
      label()->SetHorizontalAlignment(gfx::ALIGN_CENTER);
      label()->SetElideBehavior(gfx::ELIDE_TAIL);
    }
    SetBorder(views::CreateEmptyBorder(gfx::Insets(icons_ ? 0 : 4)));
    if (icons_) SetPreferredSize(gfx::Size(std::max(92, icon_size_ + 47), icon_size_ + 47));
  }
  gfx::Rect FilenameBoundsInScreen() const {
    if (!IsDrawn()) return {};
    const auto* text = label();
    auto bounds = text->GetContentsBounds();
    const int text_width = std::min(bounds.width(), text->GetPreferredSize({}).width());
    const int text_height = std::min(bounds.height(), text->GetHeightForWidth(bounds.width()));
    if (icons_) bounds.Offset((bounds.width() - text_width) / 2, 0);
    bounds.Offset(0, (bounds.height() - text_height) / 2);
    bounds.set_size(gfx::Size(text_width, text_height));
    bounds.Intersect(text->GetVisibleBounds());
    if (!bounds.IsEmpty()) views::View::ConvertRectToScreen(text, &bounds);
    return bounds;
  }
  void SetIconSize(int size) {
    if (icon_size_ == size) return;
    icon_size_ = size;
    if (icons_) SetPreferredSize(gfx::Size(std::max(92, icon_size_ + 47), icon_size_ + 47));
    InvalidateLayout();
  }
  void SetFileImage(const ui::ImageModel& image, bool fade_in = false) {
    auto* image_view = image_container_view();
    if (image_view->layer()) {
      image_view->layer()->GetAnimator()->StopAnimating();
      image_view->layer()->SetOpacity(1.f);
    }
    SetImageModel(STATE_NORMAL, image);
    if (!fade_in || !IsDrawn() || GetVisibleBounds().IsEmpty()) return;
    if (!image_view->layer()) {
      image_view->SetPaintToLayer();
      image_view->layer()->SetFillsBoundsOpaquely(false);
    }
    image_view->layer()->SetOpacity(0.f);
    ui::ScopedLayerAnimationSettings animation(image_view->layer()->GetAnimator());
    animation.SetTransitionDuration(base::Milliseconds(120));
    animation.SetTweenType(gfx::Tween::EASE_OUT);
    image_view->layer()->SetOpacity(1.f);
  }
  void SetDragDoneCallback(base::RepeatingClosure callback) { drag_done_ = std::move(callback); }
  void OnDragDone() override {
    views::LabelButton::OnDragDone();
    if (drag_done_) drag_done_.Run();
  }
  KeyClickAction GetKeyClickActionForEvent(const ui::KeyEvent& event) override {
    if (event.key_code() == ui::VKEY_RETURN) return KeyClickAction::kOnKeyPress;
    return views::LabelButton::GetKeyClickActionForEvent(event);
  }
  views::ProposedLayout CalculateProposedLayout(const views::SizeBounds& size_bounds) const override {
    if (!icons_) return views::LabelButton::CalculateProposedLayout(size_bounds);
    views::ProposedLayout result;
    if (!size_bounds.is_fully_bounded()) return result;
    result.host_size = gfx::Size(size_bounds.width().value(), size_bounds.height().value());
    const int top = std::max(0, (result.host_size.height() - icon_size_ - 47) / 2);
    auto* image = const_cast<FileButton*>(this)->image_container_view();
    auto* manager = static_cast<views::DelegatingLayoutManager*>(GetLayoutManager());
    // Apply the final tile geometry once. The default horizontal button layout
    // would reshape the label at a temporary width before the vertical layout.
    result.child_layouts.emplace_back(image, manager->CanBeVisible(image),
        gfx::Rect((result.host_size.width() - icon_size_) / 2, top, icon_size_, icon_size_), views::SizeBounds());
    result.child_layouts.emplace_back(label(), manager->CanBeVisible(label()),
        gfx::Rect(0, top + icon_size_ + 3, result.host_size.width(), 44), views::SizeBounds());
    return result;
  }
 private:
  base::RepeatingClosure drag_done_;
  bool icons_;
  int icon_size_;
};
BEGIN_METADATA(FileButton)
END_METADATA
constexpr int kTabIconSize = 18;
constexpr int kTabIconSpacing = 2;
constexpr int kTabMinimumWidth = 80 + kTabIconSize + kTabIconSpacing;
constexpr int kTabPreferredWidth = 184 + kTabIconSize + kTabIconSpacing;
class TabButton : public views::LabelButton, public views::ContextMenuController,
                  public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(TabButton, views::LabelButton)
 public:
  void SetTabMenu(bool pinned, base::RepeatingCallback<void(int)> action) {
    pinned_ = pinned; menu_action_ = std::move(action);
    UpdateCloseVisibility();
    set_context_menu_controller(this);
    if (pinned_) {
      SetTooltipText(std::u16string(GetText()) + u" · " + Text(IDS_CRFILES_PIN));
      pin_icon_ = AddChildView(std::make_unique<views::ImageView>());
      pin_icon_->SetProperty(views::kViewIgnoredByLayoutKey, true);
      pin_icon_->SetCanProcessEventsWithinSubtree(false);
      pin_icon_->SetImage(ui::ImageModel::FromVectorIcon(kKeepIcon, ui::kColorSysOnSurfaceSecondary, 14));
    }
  }
  bool pinned() const { return pinned_; }
  void ShowContextMenuForViewImpl(views::View*, const gfx::Point& point,
                                 ui::mojom::MenuSourceType type) override {
    menu_runner_.reset();
    menu_ = std::make_unique<ui::SimpleMenuModel>(this);
    menu_->AddItem(0, Text(IDS_CRFILES_NEW_TAB));
    menu_->AddSeparator(ui::NORMAL_SEPARATOR);
    menu_->AddItem(1, Text(pinned_ ? IDS_CRFILES_UNPIN : IDS_CRFILES_PIN));
    menu_->AddSeparator(ui::NORMAL_SEPARATOR);
    menu_->AddItem(2, Text(IDS_CRFILES_CLOSE));
    menu_->AddItem(3, Text(IDS_CRFILES_CLOSE_OTHER_TABS));
    menu_runner_ = std::make_unique<views::MenuRunner>(menu_.get(), views::MenuRunner::CONTEXT_MENU);
    menu_runner_->RunMenuAt(GetWidget(), nullptr, gfx::Rect(point, gfx::Size()),
        views::MenuAnchorPosition::kTopLeft, type);
  }
  void ExecuteCommand(int command, int) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(menu_action_, command));
  }
  void OnMouseEntered(const ui::MouseEvent& event) override {
    views::LabelButton::OnMouseEntered(event); UpdateCloseVisibility();
  }
  void OnMouseExited(const ui::MouseEvent& event) override {
    views::LabelButton::OnMouseExited(event); UpdateCloseVisibility();
  }
  using DragCallback = base::RepeatingCallback<void(int, bool)>;
  void SetDragCallback(DragCallback callback) {
    pressed_ = dragging_ = false;
    drag_callback_ = std::move(callback);
  }
  bool OnMousePressed(const ui::MouseEvent& event) override {
    pressed_ = event.IsOnlyLeftMouseButton();
    double_click_ = pressed_ && event.GetClickCount() == 2;
    dragging_ = false;
    press_point_ = event.location();
    views::View::ConvertPointToScreen(this, &press_point_);
    return views::LabelButton::OnMousePressed(event);
  }
  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (!pressed_ || !drag_callback_) return views::LabelButton::OnMouseDragged(event);
    auto point = event.location();
    views::View::ConvertPointToScreen(this, &point);
    drag_delta_ = point.x() - press_point_.x();
    if (!dragging_ && std::abs(drag_delta_) < 6) return true;
    dragging_ = true;
    drag_callback_.Run(drag_delta_, false);
    return true;
  }
  void OnMouseReleased(const ui::MouseEvent& event) override {
    const bool double_click = std::exchange(double_click_, false);
    pressed_ = false;
    if (dragging_) {
      dragging_ = false;
      views::LabelButton::OnMouseCaptureLost();
      drag_callback_.Run(drag_delta_, true);
      return;
    }
    if (double_click && HitTestPoint(event.location()) && GetWidget()) {
      auto* window = GetWidget()->GetTopLevelWidget();
      if (!window->IsClosed() && !window->IsFullscreen() &&
          window->widget_delegate()->CanMaximize() && !IsCrFilesWindowBlockedByModal(window)) {
        views::LabelButton::OnMouseCaptureLost();
        // Finish mouse dispatch before changing window bounds or tab layout.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce([](base::WeakPtr<views::Widget> widget) {
              if (!widget || widget->IsClosed() || widget->IsFullscreen() ||
                  !widget->widget_delegate()->CanMaximize() ||
                  IsCrFilesWindowBlockedByModal(widget.get())) return;
              if (widget->IsMaximized()) widget->Restore();
              else widget->Maximize();
            }, window->GetWeakPtr()));
        return;
      }
    }
    views::LabelButton::OnMouseReleased(event);
  }
  void OnMouseCaptureLost() override {
    double_click_ = false;
    pressed_ = false;
    if (dragging_) {
      dragging_ = false;
      drag_callback_.Run(drag_delta_, true);
    }
    views::LabelButton::OnMouseCaptureLost();
  }
  TabButton(base::RepeatingClosure select, base::RepeatingClosure close, std::u16string title, bool active)
      : views::LabelButton(std::move(select), title), active_(active) {
    SetNotifyEnterExitOnChild(true);
    // The custom tab background is drawn on the translucent window frame.
    label()->SetSubpixelRenderingEnabled(false);
    AnimateButton(this);
    SetFocusBehavior(views::View::FocusBehavior::NEVER);
    SetRequestFocusOnPress(false);
    // Tab geometry paints its own hover/pressed state; the generic ink drop
    // would add a second rounded rectangle over the Chrome-shaped tab.
    views::InkDrop::Get(this)->SetMode(views::InkDropHost::InkDropMode::OFF);
    SetInstallFocusRingOnFocus(false);
    SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(5, 16, 0, 32)));
    SetHorizontalAlignment(gfx::ALIGN_LEFT);
    SetTooltipText(title);
    SetEnabledTextColors(active ? ui::kColorSysOnSurface : ui::kColorSysOnSurfaceSecondary);
    SetImageLabelSpacing(kTabIconSpacing);
    SetLocationIcon(ui::ImageModel::FromVectorIcon(vector_icons::kFolderOpenIcon, ui::kColorSysOnSurfaceSecondary, kTabIconSize));
    close_ = AddIconButton(this, kCloseIcon, IDS_CRFILES_CLOSE, std::move(close));
    close_->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    close_->SetRequestFocusOnPress(false);
    close_->SetProperty(views::kViewIgnoredByLayoutKey, true);
    close_->SetBorder(views::CreateEmptyBorder(4));
    close_->SetPreferredSize(gfx::Size(24, 24));
    close_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    close_->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(kCloseIcon, ui::kColorSysOnSurface, 16));
    for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
      close_->SetImageModel(state, ui::ImageModel::FromVectorIcon(kCloseIcon, ui::kColorSysPrimary, 16));
    views::InstallCircleHighlightPathGenerator(close_);
    UpdateCloseVisibility();
  }
  void SetLocationIcon(const ui::ImageModel& icon) {
    if (location_icon_ == icon) return;
    location_icon_ = icon;
    search_state_.reset();
    if (search_icon_) {
      search_icon_->SetVisible(false);
      search_spinner_->Stop(); search_spinner_->SetVisible(false);
    }
    SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(5, 16, 0, 32)));
    SetImageModel(views::Button::STATE_NORMAL, icon);
    SetTooltipText(std::u16string(GetText()) + (pinned_ ? u" · " + Text(IDS_CRFILES_PIN) : std::u16string()));
  }
  void SetSearchState(SearchPageState state) {
    if (search_state_ == state) return;
    search_state_ = state;
    SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel());
    SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(5, 16 + kTabIconSize + kTabIconSpacing, 0, 32)));
    if (!search_icon_) {
      search_icon_ = AddChildView(std::make_unique<views::ImageView>());
      search_spinner_ = AddChildView(std::make_unique<views::Throbber>(kTabIconSize));
      for (views::View* view : {static_cast<views::View*>(search_icon_.get()), static_cast<views::View*>(search_spinner_.get())}) {
        view->SetProperty(views::kViewIgnoredByLayoutKey, true);
        view->SetCanProcessEventsWithinSubtree(false);
      }
      search_spinner_->SetColorId(ui::kColorSysPrimary);
    }
    const gfx::VectorIcon* icon = &vector_icons::kSearchIcon;
    int description = IDS_CRFILES_SEARCH_READY;
    switch (state) {
      case SearchPageState::kRunning: description = IDS_CRFILES_SEARCH_RUNNING; break;
      case SearchPageState::kPaused: icon = &vector_icons::kPauseIcon; description = IDS_CRFILES_SEARCH_PAUSED; break;
      case SearchPageState::kComplete: icon = &kCheckSmallIcon; description = IDS_CRFILES_SEARCH_COMPLETE; break;
      case SearchPageState::kCanceled: icon = &kCloseIcon; description = IDS_CRFILES_SEARCH_CANCELED; break;
      case SearchPageState::kFailed: icon = &vector_icons::kErrorFilledIcon; description = IDS_CRFILES_SEARCH_FAILED; break;
      case SearchPageState::kReady: break;
    }
    const bool working = state == SearchPageState::kRunning;
    search_icon_->SetVisible(!working); search_spinner_->SetVisible(working);
    if (working) search_spinner_->Start(); else search_spinner_->Stop();
    search_icon_->SetImage(ui::ImageModel::FromVectorIcon(*icon,
        state == SearchPageState::kFailed ? ui::kColorSysError : ui::kColorSysPrimary, kTabIconSize));
    SetTooltipText(std::u16string(GetText()) + u" · " + Text(description) +
        (pinned_ ? u" · " + Text(IDS_CRFILES_PIN) : std::u16string()));
    InvalidateLayout(); SchedulePaint();
  }
  void AddedToWidget() override {
    views::LabelButton::AddedToWidget();
    paint_active_subscription_ = GetWidget()->RegisterPaintAsActiveChangedCallback(
        base::BindRepeating([](TabButton* tab) { tab->SchedulePaint(); }, base::Unretained(this)));
    SchedulePaint();
  }
  void RemovedFromWidget() override {
    paint_active_subscription_ = {};
    views::LabelButton::RemovedFromWidget();
  }
  void OnThemeChanged() override {
    views::LabelButton::OnThemeChanged();
    SchedulePaint();
  }
  void Layout(PassKey) override {
    UpdateCloseVisibility();
    LayoutSuperclass<views::LabelButton>(this);
    close_->SetBounds(std::max(0, width() - 34), (height() - 24) / 2, 24, 24);
    if (pin_icon_) pin_icon_->SetBounds(std::max(0, width() - 29), (height() - 14) / 2, 14, 14);
    if (search_icon_) {
      search_icon_->SetBounds(16, (height() - kTabIconSize) / 2, kTabIconSize, kTabIconSize);
      search_spinner_->SetBounds(16, (height() - kTabIconSize) / 2, kTabIconSize, kTabIconSize);
    }
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    if (!active_) {
      const bool frame_active = GetWidget()->ShouldPaintAsActive();
      canvas->FillRect(GetLocalBounds(), GetColorProvider()->GetColor(frame_active ?
          ui::kColorSysHeader : ui::kColorSysHeaderInactive));
      cc::PaintFlags hover;
      hover.setAntiAlias(true);
      hover.setColor(GetColorProvider()->GetColor(frame_active ?
          ui::kColorSysStateHeaderHover : ui::kColorSysStateHeaderHoverInactive));
      hover.setAlphaf(hover.getAlphaf() * (GetState() == STATE_PRESSED ? 1.f :
          static_cast<float>(hover_animation().GetCurrentValue())));
      canvas->DrawRoundRect(gfx::Rect(6, 3, std::max(0, width() - 12), height() - 6), 8, hover);
      if (HasFocus()) {
        hover.setStyle(cc::PaintFlags::kStroke_Style); hover.setStrokeWidth(2);
        hover.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
        canvas->DrawRoundRect(gfx::Rect(6, 3, std::max(0, width() - 12), height() - 6), 8, hover);
      }
      canvas->DrawLine(gfx::Point(width() - 1, 10), gfx::Point(width() - 1, height() - 10),
          GetColorProvider()->GetColor(ui::kColorSysOnHeaderDivider));
    } else {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(GetColorProvider()->GetColor(NavigationColor(this)));
      const float w = width(), h = height();
      SkPathBuilder path;
      path.moveTo(0, h).cubicTo(5, h, 8, h - 3, 8, h - 8)
          .lineTo(8, 14).cubicTo(8, 8, 12, 4, 18, 4)
          .lineTo(w - 18, 4).cubicTo(w - 12, 4, w - 8, 8, w - 8, 14)
          .lineTo(w - 8, h - 8).cubicTo(w - 8, h - 3, w - 5, h, w, h)
          .close();
      const auto shape = path.detach();
      canvas->DrawPath(shape, flags);
      flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
      flags.setAlphaf(GetState() == STATE_PRESSED ? 0.12f :
          0.06f * static_cast<float>(hover_animation().GetCurrentValue()));
      canvas->DrawPath(shape, flags);
      if (HasFocus()) {
        flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
        flags.setStyle(cc::PaintFlags::kStroke_Style); flags.setStrokeWidth(2);
        canvas->DrawPath(shape, flags);
      }
    }
  }
 private:
  void UpdateCloseVisibility() {
    if (close_) close_->SetVisible(!pinned_ && (active_ || IsMouseHovered()));
  }
  bool pinned_ = false;
  base::RepeatingCallback<void(int)> menu_action_;
  std::unique_ptr<ui::SimpleMenuModel> menu_;
  std::unique_ptr<views::MenuRunner> menu_runner_;
  DragCallback drag_callback_;
  gfx::Point press_point_;
  int drag_delta_ = 0;
  bool pressed_ = false, dragging_ = false;
  bool double_click_ = false;
  std::optional<SearchPageState> search_state_;
  ui::ImageModel location_icon_;
  raw_ptr<views::ImageView> pin_icon_ = nullptr;
  raw_ptr<views::ImageView> search_icon_ = nullptr;
  raw_ptr<views::Throbber> search_spinner_ = nullptr;
  base::CallbackListSubscription paint_active_subscription_;
  bool active_;
  raw_ptr<views::LabelButton> close_ = nullptr;
};
BEGIN_METADATA(TabButton)
END_METADATA

class TabStrip : public views::View {
  METADATA_HEADER(TabStrip, views::View)
 public:
  explicit TabStrip(base::RepeatingCallback<void(size_t, size_t)> reorder)
      : reorder_(std::move(reorder)) {}
  void TabsChanged() {
    PreferredSizeChanged();
    InvalidateLayout();
  }
  bool IsDraggingTab() const { return dragged_ != nullptr; }
  void ResetDrag() {
    dragged_ = nullptr;
    for (const auto& child : children())
      if (auto* tab = views::AsViewClass<TabButton>(child.get())) tab->SetDragCallback({});
  }
  gfx::Size CalculatePreferredSize(const views::SizeBounds& available) const override {
    const int count = static_cast<int>(children().size());
    const int minimum = count * (kTabMinimumWidth + 1);
    return gfx::Size(available.width().is_bounded() ?
        std::max(minimum, available.width().value()) : count * (kTabPreferredWidth + 1), 36);
  }
  void Layout(PassKey) override {
    PositionTabs(false);
    LayoutSuperclass<views::View>(this);
  }
  void Drag(TabButton* tab, int delta, bool done) {
    if (children().empty()) return;
    if (!dragged_) {
      dragged_ = tab;
      origin_x_ = tab->x();
      tab->layer()->GetAnimator()->StopAnimating();
      tab->layer()->parent()->StackAtTop(tab->layer());
    }
    const int count = static_cast<int>(children().size());
    const int pinned_count = std::ranges::count_if(children(), [](const auto& child) {
      return static_cast<TabButton*>(child.get())->pinned();
    });
    const int first = tab->pinned() ? 0 : pinned_count;
    const int last = tab->pinned() ? pinned_count - 1 : count - 1;
    const int stride = TabWidth() + 1;
    drag_x_ = std::clamp(origin_x_ + delta, first * stride, last * stride);
    const size_t from = GetIndexOf(tab).value();
    const size_t to = std::clamp((drag_x_ + TabWidth() / 2) / stride, first, last);
    if (from != to) {
      ReorderChildView(tab, to);
      reorder_.Run(from, to);
    }
    PositionTabs(true);
    tab->layer()->parent()->StackAtTop(tab->layer());
    if (done) {
      dragged_ = nullptr;
      ui::ScopedLayerAnimationSettings settings(tab->layer()->GetAnimator());
      settings.SetTransitionDuration(base::Milliseconds(160));
      settings.SetTweenType(gfx::Tween::EASE_OUT);
      tab->layer()->SetTransform(gfx::Transform());
      tab->layer()->SetOpacity(1.f);
    } else {
      tab->layer()->SetOpacity(0.9f);
      tab->ScrollViewToVisible();
    }
  }
 private:
  int TabWidth() const {
    const int count = std::max(1, static_cast<int>(children().size()));
    return std::clamp((width() - count) / count, kTabMinimumWidth, kTabPreferredWidth);
  }
  void PositionTabs(bool animate) {
    const int tab_width = TabWidth();
    int x = 0;
    for (const auto& child : children()) {
      const bool tab = views::AsViewClass<TabButton>(child.get());
      const int old_x = child->x();
      const bool positioned = !child->bounds().IsEmpty();
      auto* layer = child->layer();
      const float offset = layer ? layer->transform().To2dTranslation().x() : 0;
      child->SetBounds(x, 0, tab ? tab_width : 36, 36);
      if (child.get() == dragged_) {
        layer->GetAnimator()->StopAnimating();
        gfx::Transform transform; transform.Translate(drag_x_ - x, 0);
        layer->SetTransform(transform);
      } else if (layer && animate && positioned && old_x != x) {
        layer->GetAnimator()->StopAnimating();
        gfx::Transform transform; transform.Translate(old_x + offset - x, 0);
        layer->SetTransform(transform);
        ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
        settings.SetTransitionDuration(base::Milliseconds(160));
        settings.SetTweenType(gfx::Tween::EASE_OUT);
        layer->SetTransform(gfx::Transform());
      }
      x += (tab ? tab_width : 36) + 1;
    }
  }
  base::RepeatingCallback<void(size_t, size_t)> reorder_;
  raw_ptr<TabButton> dragged_ = nullptr;
  int origin_x_ = 0, drag_x_ = 0;
};
BEGIN_METADATA(TabStrip)
END_METADATA

// Keep the add button next to short tab strips, but outside an overflowing
// viewport. Complete scrolling layout before revealing the selected tab.
constexpr int kNewTabButtonSize = 28;
constexpr int kNewTabButtonLeftPadding = 6;
class NewTabButton : public views::LabelButton {
  METADATA_HEADER(NewTabButton, views::LabelButton)
 public:
  explicit NewTabButton(base::RepeatingClosure callback)
      : views::LabelButton(std::move(callback), std::u16string()) {
    AnimateButton(this);
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    const float opacity = GetState() == STATE_PRESSED ? 0.20f :
        0.12f * static_cast<float>(hover_animation().GetCurrentValue());
    if (opacity <= 0.f) return;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimary));
    flags.setAlphaf(opacity);
    canvas->DrawCircle(gfx::PointF(width() / 2.f, height() / 2.f),
        std::min(width(), height()) / 2.f, flags);
  }
};
BEGIN_METADATA(NewTabButton)
END_METADATA
class TabBar : public views::View {
  METADATA_HEADER(TabBar, views::View)
 public:
  explicit TabBar(base::RepeatingCallback<void(views::ScrollView*)> laid_out)
      : laid_out_(std::move(laid_out)) {}
  void Layout(PassKey) override {
    if (children().size() != 2) return;
    auto* scroll = static_cast<views::ScrollView*>(children()[0].get());
    // Keep offsets on the main side: a newly added tab can extend beyond the
    // compositor's previous content bounds until the next commit. Scrolling
    // against those stale bounds would leave the new tab out of view.
    if (!scroll_synchronizer_)
      scroll_synchronizer_ = scroll->EnableScrollSynchronization();
    const int desired = static_cast<int>(scroll->contents()->children().size()) * (kTabPreferredWidth + 1);
    const int viewport_width = std::min(desired,
        std::max(0, width() - kNewTabButtonSize - kNewTabButtonLeftPadding - 2));
    scroll->SetBounds(0, 0, viewport_width, 36);
    children()[1]->SetBounds(viewport_width + kNewTabButtonLeftPadding, (36 - kNewTabButtonSize) / 2,
                            kNewTabButtonSize, kNewTabButtonSize);
    LayoutSuperclass<views::View>(this);
    scroll->DeprecatedLayoutImmediately();
    scroll->contents()->DeprecatedLayoutImmediately();
    laid_out_.Run(scroll);
  }
 private:
  base::RepeatingCallback<void(views::ScrollView*)> laid_out_;
  std::unique_ptr<views::ScrollView::ScopedScrollSynchronizer> scroll_synchronizer_;
};
BEGIN_METADATA(TabBar)
END_METADATA
// An IME may activate a native window temporarily. Restore the editor only
// when our window becomes active again, never while the IME owns activation.
// File rows can be recycled during a native drag. Observe the window's drag
// completion instead of relying on the continued existence of the source row.
class FileDragCursorObserver : public views::WidgetObserver {
 public:
  FileDragCursorObserver(views::Widget* widget, base::FilePath folder)
      : widget_(widget->GetWeakPtr()), folder_(std::move(folder)) {
    widget->AddObserver(this);
  }
  const base::FilePath& folder() const { return folder_; }
  void SetPreviewCleanup(base::OnceClosure cleanup) { preview_cleanup_ = std::move(cleanup); }
  ~FileDragCursorObserver() override {
    if (widget_) widget_->RemoveObserver(this);
  }
  void OnWidgetDragDropWillStart(views::Widget*) override {
    weak_.InvalidateWeakPtrs();
  }
  void OnWidgetDragDropCompleted(views::Widget*) override {
    folder_.clear();
    if (preview_cleanup_) std::move(preview_cleanup_).Run();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&FileDragCursorObserver::Restore, weak_.GetWeakPtr()));
  }
  void OnWidgetDestroying(views::Widget* widget) override {
    folder_.clear();
    widget->RemoveObserver(this);
    widget_.reset();
    weak_.InvalidateWeakPtrs();
  }
 private:
  void Restore() {
    if (!widget_ || widget_->IsClosed() || !widget_->IsVisible()) return;
    // Wayland screen coordinates may be unavailable immediately after a drag.
    // Restore the source window's arrow even when target hit testing fails.
    ui::Cursor cursor(ui::mojom::CursorType::kPointer);
    auto* root = widget_->GetRootView();
    auto point = display::Screen::Get()->GetCursorScreenPoint();
    views::View::ConvertPointFromScreen(root, &point);
    if (root->HitTestPoint(point)) {
      if (auto* target = root->GetEventHandlerForPoint(point)) {
        auto local = point;
        views::View::ConvertPointToTarget(root, target, &local);
        ui::MouseEvent event(ui::EventType::kMouseMoved, local, point, base::TimeTicks::Now(), 0, 0);
        cursor = target->GetCursor(event);
        if (cursor.type() == ui::mojom::CursorType::kNull)
          cursor = ui::Cursor(ui::mojom::CursorType::kPointer);
      }
    } else if (!widget_->IsActive()) {
      return;
    }
    widget_->SetCursor(cursor);
  }
  base::WeakPtr<views::Widget> widget_;
  base::FilePath folder_;
  base::OnceClosure preview_cleanup_;
  base::WeakPtrFactory<FileDragCursorObserver> weak_{this};
};

class RenameFocusObserver : public views::WidgetObserver {
 public:
  RenameFocusObserver(views::Widget* owner, views::Widget* bubble, views::Textfield* field)
      : owner_(owner), bubble_(bubble), field_(field) {
    owner_->AddObserver(this); bubble_->AddObserver(this);
  }
  ~RenameFocusObserver() override {
    if (owner_) owner_->RemoveObserver(this);
    if (bubble_) bubble_->RemoveObserver(this);
  }
  void OnWidgetActivationChanged(views::Widget*, bool active) override {
    if (!active) return;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&RenameFocusObserver::Restore, weak_.GetWeakPtr()));
  }
  void OnWidgetDestroying(views::Widget* widget) override {
    widget->RemoveObserver(this);
    if (widget == owner_) owner_ = nullptr;
    if (widget == bubble_) { bubble_ = nullptr; field_ = nullptr; }
  }
 private:
  void Restore() {
    if (!owner_ || !bubble_ || !field_ || !field_->GetEnabled() || !bubble_->IsVisible() ||
        (!owner_->IsActive() && !bubble_->IsActive())) return;
    if (!bubble_->IsActive()) bubble_->Activate();
    field_->RequestFocus();
  }
  raw_ptr<views::Widget> owner_;
  raw_ptr<views::Widget> bubble_;
  raw_ptr<views::Textfield> field_;
  base::WeakPtrFactory<RenameFocusObserver> weak_{this};
};
class NewFolderView : public views::View, public views::TextfieldController {
  METADATA_HEADER(NewFolderView, views::View)
 public:
  explicit NewFolderView(base::RepeatingCallback<void(std::u16string)> create, int button_label = IDS_CRFILES_NEW_FOLDER, int error_label = IDS_CRFILES_NEW_FOLDER_ERROR, bool path_input = false)
      : create_(std::move(create)), animate_submission_(path_input) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(path_input ? 6 : 12), 10));
    auto* row = AddChildView(std::make_unique<views::View>());
    row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 10));
    name_ = row->AddChildView(std::make_unique<views::Textfield>());
    name_->RemoveHoverEffect();
    name_->SetAccessibleName(Text(IDS_CRFILES_NAME));
    name_->SetPlaceholderText(Text(IDS_CRFILES_NAME));
    name_->SetPreferredSize(gfx::Size(260, 36));
    name_->set_controller(this);
    if (path_input) {
      completion_ = std::make_unique<PathCompletion>(name_);
      name_->SetAccessibleName(Text(IDS_CRFILES_LOCATION));
      name_->SetPlaceholderText(Text(IDS_CRFILES_LOCATION));
      name_->SetPreferredSize(gfx::Size(400, 36));
    }
    auto* separator = row->AddChildView(std::make_unique<views::Separator>());
    separator->SetOrientation(views::Separator::Orientation::kVertical);
    if (path_input) {
      auto* button = row->AddChildView(std::make_unique<views::MdTextButton>(
          base::BindRepeating(&NewFolderView::Submit, base::Unretained(this)), Text(button_label)));
      button->SetStyle(ui::ButtonStyle::kTonal);
      button->SetCornerRadius(8);
      button->SetCustomPadding(gfx::Insets::VH(8, 16));
      button->SetMinSize(gfx::Size(88, 36));
      // MdTextButton's state colors already contain alpha. The generic
      // AnimateButton opacity would multiply it again and hide the feedback.
      button->SetAnimationDuration(base::Milliseconds(160));
      auto* ink = views::InkDrop::Get(button);
      ink->SetHighlightOpacity(1.f);
      ink->SetVisibleOpacity(1.f);
      ink->GetInkDrop()->SetShowHighlightOnHover(true);
      ink->GetInkDrop()->SetShowHighlightOnFocus(true);
      create_button_ = button;
      create_button_->SetImageModel(views::Button::STATE_NORMAL,
          ui::ImageModel::FromVectorIcon(kCheckOldIcon, ui::kColorSysOnSurface, 18));
    } else {
      create_button_ = AddButton(row, Text(button_label), base::BindRepeating(&NewFolderView::Submit, base::Unretained(this)));
    }
    error_ = AddChildView(std::make_unique<views::Label>(Text(error_label)));
    error_->SetMultiLine(true); error_->SetMaximumWidth(260); error_->SetVisible(false);
  }
  views::Textfield* field() { return name_; }
  void Failed(std::u16string message = {}) {
    if (!message.empty()) error_->SetText(message);
    submitting_ = false; name_->SetEnabled(true); create_button_->SetEnabled(true); error_->SetVisible(true); InvalidateLayout(); name_->RequestFocus();
  }
  void ContentsChanged(views::Textfield*, const std::u16string&) override {
    if (completion_) completion_->Changed();
  }
  bool HandleKeyEvent(views::Textfield*, const ui::KeyEvent& event) override {
    if (completion_ && completion_->Key(event)) return true;
    if (event.type() == ui::EventType::kKeyPressed && event.key_code() == ui::VKEY_RETURN && !name_->IsIMEComposing()) { Submit(); return true; }
    return false;
  }
 private:
  void Submit() {
    if (submitting_ || !create_button_->GetEnabled()) return;
    submitting_ = true;
    name_->SetEnabled(false); error_->SetVisible(false);
    if (animate_submission_) {
      // Keep the button alive and enabled long enough to show its click ripple.
      // The guard above prevents repeat clicks; destroying the view cancels this timer.
      submit_timer_.Start(FROM_HERE, base::Milliseconds(300), this, &NewFolderView::CompleteSubmit);
    } else {
      CompleteSubmit();
    }
  }
  void CompleteSubmit() {
    create_button_->SetEnabled(false);
    create_.Run(std::u16string(name_->GetText()));
  }
  std::unique_ptr<PathCompletion> completion_;
  base::RepeatingCallback<void(std::u16string)> create_;
  const bool animate_submission_;
  bool submitting_ = false;
  raw_ptr<views::Textfield> name_ = nullptr;
  raw_ptr<views::LabelButton> create_button_ = nullptr;
  raw_ptr<views::Label> error_ = nullptr;
  base::OneShotTimer submit_timer_;
};
BEGIN_METADATA(NewFolderView)
END_METADATA
class IconSizeSlider : public views::Slider {
  METADATA_HEADER(IconSizeSlider, views::Slider)
 public:
  explicit IconSizeSlider(views::SliderListener* listener) : views::Slider(listener) {}
  void OnPaint(gfx::Canvas* canvas) override {
    const auto bounds = GetContentsBounds();
    const float left = bounds.x() + kThumbRadius;
    const float span = std::max(0.f, bounds.width() - 2 * kThumbRadius);
    const float y = bounds.CenterPoint().y();
    const float x = left + GetValue() * span;
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSliderTrack));
    canvas->DrawRoundRect(gfx::RectF(left, y - 2, span, 4), 2, flags);
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSliderThumb));
    canvas->DrawRoundRect(gfx::RectF(left, y - 2, x - left, 4), 2, flags);
    if (HasFocus() || IsMouseHovered()) {
      flags.setColor(GetColorProvider()->GetColor(ui::kColorSysPrimaryContainer));
      canvas->DrawCircle(gfx::PointF(x, y), 10.f, flags);
    }
    flags.setColor(GetColorProvider()->GetColor(ui::kColorSliderThumb));
    // Paint the actual snapped value rather than the built-in 150 ms animation.
    canvas->DrawCircle(gfx::PointF(x, y), 5.f, flags);
  }
};
BEGIN_METADATA(IconSizeSlider)
END_METADATA
enum class ViewOptionsPage { kLocal, kNetworkRoot, kNetworkDirectory };
class ViewOptions : public views::View, public views::SliderListener {
  METADATA_HEADER(ViewOptions, views::View)
 public:
  ViewOptions(FileViewOptions options, base::RepeatingCallback<void(FileViewOptions)> changed,
              base::RepeatingClosure resized, ViewOptionsPage page)
      : options_(options), changed_(std::move(changed)), resized_(std::move(resized)) {
    SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(4), 4));
    auto* sizes = AddChildView(std::make_unique<views::View>());
    sizes->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(8), 4));
    size_label_ = sizes->AddChildView(std::make_unique<views::Label>());
    size_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    UpdateSizeLabel();
    slider_ = sizes->AddChildView(std::make_unique<IconSizeSlider>(this));
    slider_->SetAccessibleName(Text(IDS_CRFILES_ICON_SIZE));
    base::flat_set<float> values;
    for (size_t i = 0; i < kFileIconSizes.size(); ++i)
      values.insert(static_cast<float>(i) / (kFileIconSizes.size() - 1));
    slider_->SetAllowedValues(&values);
    slider_->SetValue(static_cast<float>(options_.icon_index) / (kFileIconSizes.size() - 1));
    slider_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(4, 12)));
    slider_->SetPreferredSize(gfx::Size(280, 40));
    auto* ticks = sizes->AddChildView(std::make_unique<views::View>());
    ticks->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 12)));
    auto* ticks_layout = ticks->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
    for (int size : kFileIconSizes) {
      auto* tick = ticks->AddChildView(std::make_unique<views::Label>(base::NumberToString16(size)));
      ticks_layout->SetFlexForView(tick, 1);
    }
    // Discovery and share roots have no file metadata or hidden-file filter.
    if (page == ViewOptionsPage::kNetworkRoot) return;
    Separator();
    hidden_ = AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_SHOW_HIDDEN)));
    AnimateButton(hidden_); hidden_->SetChecked(options_.show_hidden);
    hidden_->SetCallback(base::BindRepeating(&ViewOptions::Hidden, base::Unretained(this)));
    Separator();
    auto* columns = AddButton(this, Text(IDS_CRFILES_VISIBLE_COLUMNS),
        base::BindRepeating(&ViewOptions::ToggleColumns, base::Unretained(this)));
    columns->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kExpandMoreOldIcon, ui::kColorSysOnSurface, 16));
    columns_ = AddChildView(std::make_unique<views::View>());
    columns_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(0, 12), 4));
    auto* name = columns_->AddChildView(std::make_unique<views::Checkbox>(Text(IDS_CRFILES_NAME)));
    name->SetChecked(true); name->SetEnabled(false);
    constexpr std::array column_labels = {IDS_CRFILES_SIZE, IDS_CRFILES_TYPE, IDS_CRFILES_LAST_MODIFIED,
        IDS_CRFILES_CREATED, IDS_CRFILES_PERMISSIONS};
    const std::array checked = {options_.size_column, options_.type_column, options_.modified_column, options_.created_column, options_.permissions_column};
    static_assert(column_labels.size() == checked.size());
    for (size_t i = 0; i < column_labels.size(); ++i) {
      // SMB permissions require separate ACL handling.
      if (page == ViewOptionsPage::kNetworkDirectory && i == 4) continue;
      auto* checkbox = columns_->AddChildView(std::make_unique<views::Checkbox>(Text(column_labels[i])));
      AnimateButton(checkbox); checkbox->SetChecked(checked[i]);
      checkbox->SetCallback(base::BindRepeating(&ViewOptions::Column, base::Unretained(this), static_cast<int>(i), base::Unretained(checkbox)));
    }
    columns_->SetVisible(false);
  }
  void SetIconIndex(int index) {
    options_.icon_index = index;
    slider_->SetValue(static_cast<float>(index) / (kFileIconSizes.size() - 1));
    UpdateSizeLabel();
  }
  void SliderValueChanged(views::Slider*, float value, float, views::SliderChangeReason reason) override {
    if (reason != views::SliderChangeReason::kByUser) return;
    const int last = static_cast<int>(kFileIconSizes.size()) - 1;
    int index = std::clamp(static_cast<int>(std::lround(value * last)), 0, last);
    if (options_.icon_index == index) return;
    options_.icon_index = index; UpdateSizeLabel(); changed_.Run(options_);
  }
 private:
  void UpdateSizeLabel() { size_label_->SetText(Text(IDS_CRFILES_ICON_SIZE) + u" · " + base::NumberToString16(kFileIconSizes[options_.icon_index])); }
  void Separator() { AddChildView(std::make_unique<views::Separator>()); }
  void Hidden() { options_.show_hidden = hidden_->GetChecked(); changed_.Run(options_); }
  void Column(int index, views::Checkbox* checkbox) {
    if (index == 0) options_.size_column = checkbox->GetChecked();
    if (index == 1) options_.type_column = checkbox->GetChecked();
    if (index == 2) options_.modified_column = checkbox->GetChecked();
    if (index == 3) options_.created_column = checkbox->GetChecked();
    if (index == 4) options_.permissions_column = checkbox->GetChecked();
    changed_.Run(options_);
  }
  void ToggleColumns() { columns_->SetVisible(!columns_->GetVisible()); InvalidateLayout(); resized_.Run(); }
  FileViewOptions options_;
  base::RepeatingCallback<void(FileViewOptions)> changed_;
  base::RepeatingClosure resized_;
  raw_ptr<views::Label> size_label_ = nullptr;
  raw_ptr<views::Slider> slider_ = nullptr;
  raw_ptr<views::Checkbox> hidden_ = nullptr;
  raw_ptr<views::View> columns_ = nullptr;
};
BEGIN_METADATA(ViewOptions)
END_METADATA

class SplitResizeGrip : public ColumnResizeGrip {
  METADATA_HEADER(SplitResizeGrip, ColumnResizeGrip)
 public:
  SplitResizeGrip(views::ResizeAreaDelegate* delegate, bool vertical)
      : ColumnResizeGrip(delegate), vertical_(vertical) {
    set_axis(vertical ? Axis::kVertical : Axis::kHorizontal);
  }
  void OnPaintBackground(gfx::Canvas* canvas) override {
    const auto line = vertical_
        ? gfx::Rect(0, (height() - 2) / 2, width(), 2)
        : gfx::Rect((width() - 2) / 2, 0, 2, height());
    canvas->FillRect(line, GripColor());
  }
 private:
  const bool vertical_;
};
BEGIN_METADATA(SplitResizeGrip)
END_METADATA

constexpr int kSplitResizeWidth = 2;
constexpr double kNewPaneRatio = 0.382;

double InitialSplitRatio(bool new_first) {
  return new_first ? kNewPaneRatio : 1.0 - kNewPaneRatio;
}

int SplitMinimumExtent(int available, bool vertical) {
  return std::min(vertical ? 100 : 180, available / 2);
}

int SplitFirstExtent(int available, bool vertical, double ratio) {
  const int minimum = SplitMinimumExtent(available, vertical);
  return std::clamp(static_cast<int>(std::lround(available * ratio)), minimum, available - minimum);
}

gfx::Rect SplitAxisBounds(gfx::Size size, bool vertical, int start, int extent) {
  return vertical ? gfx::Rect(0, start, size.width(), extent) : gfx::Rect(start, 0, extent, size.height());
}

struct SplitBounds {
  gfx::Rect first, second;
};

SplitBounds CalculateSplitBounds(gfx::Size size, bool vertical, double ratio) {
  const int available = std::max(0, (vertical ? size.height() : size.width()) - kSplitResizeWidth);
  const int first = SplitFirstExtent(available, vertical, ratio);
  return {SplitAxisBounds(size, vertical, 0, first),
          SplitAxisBounds(size, vertical, first + kSplitResizeWidth, available - first)};
}

// A binary split retains its children while the divider moves.
class PaneSplit : public views::View, public views::ResizeAreaDelegate, public views::AnimationDelegateViews {
  METADATA_HEADER(PaneSplit, views::View)
 public:
  PaneSplit(bool vertical, std::unique_ptr<views::View> first, std::unique_ptr<views::View> second)
      : views::AnimationDelegateViews(this), vertical_(vertical) {
    first_ = AddChildView(std::move(first)); second_ = AddChildView(std::move(second));
    divider_ = AddChildView(std::make_unique<SplitResizeGrip>(this, vertical));
    divider_->SetAccessibleName(Text(IDS_CRFILES_RESIZE_SPLIT));
    SetEventTargeter(std::make_unique<views::ViewTargeter>(std::make_unique<ResizeGripTargeter>(
        divider_, vertical_, base::BindRepeating(&ColumnResizeGrip::IsDragging,
            base::Unretained(static_cast<SplitResizeGrip*>(divider_.get()))))));
  }
  bool vertical() const { return vertical_; }
  double ratio() const { return ratio_; }
  void SetRatio(double value) { ratio_ = std::clamp(value, 0.1, 0.9); }
  views::View* first() const { return first_; }
  views::View* second() const { return second_; }
  void SetChangedCallback(base::RepeatingClosure callback) { changed_ = std::move(callback); }
  void SetResizeCallback(base::RepeatingCallback<void(bool)> callback) { resizing_ = std::move(callback); }
  views::View* Sibling(views::View* child) { return child == first_ ? second_.get() : first_.get(); }
  void PrepareOpening(bool new_first, std::optional<gfx::Rect> preview_bounds) {
    SetRatio(InitialSplitRatio(new_first));
    opening_from_ = preview_bounds;
    if (gfx::Animation::ShouldRenderRichAnimation() && !gfx::Animation::PrefersReducedMotion())
      opening_pane_ = new_first ? first_ : second_;
  }
  void FinishOpening() {
    if (opening_.is_animating()) opening_.End();
    opening_pane_ = nullptr;
  }
  void Replace(views::View* old, views::View* replacement) {
    if (opening_pane_ == old) opening_pane_ = replacement;
    if (first_ == old) first_ = replacement; else second_ = replacement;
  }
  void OnResize(int amount, bool done) override {
    const int available = std::max(1, (vertical_ ? height() : width()) - kSplitResizeWidth);
    if (!drag_start_) {
      drag_start_ = vertical_ ? divider_->y() : divider_->x();
      FinishOpening();
      if (resizing_) resizing_.Run(true);
    }
    const int minimum = SplitMinimumExtent(available, vertical_);
    const int position = std::clamp(*drag_start_ + amount, minimum, available - minimum);
    const int snapped_position = SnapResizePosition(position, available / 2, snapped_);
    pending_ratio_ = static_cast<double>(snapped_position) / available;
    if (done) {
      resize_timer_.Stop();
      FlushResize();
      drag_start_.reset();
      snapped_ = false;
      if (resizing_) resizing_.Run(false);
      if (changed_) changed_.Run();
    } else if (!resize_timer_.IsRunning()) {
      resize_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &PaneSplit::FlushResize);
    }
  }
  void FlushResize() {
    const int available = std::max(0, (vertical_ ? height() : width()) - kSplitResizeWidth);
    const bool moved = SplitFirstExtent(available, vertical_, ratio_) != SplitFirstExtent(available, vertical_, pending_ratio_);
    ratio_ = pending_ratio_;
    if (moved) InvalidateLayout();
  }
  void Layout(PassKey) override {
    if (opening_pane_ && !opening_.is_animating()) {
      if (size().IsEmpty()) return;
      if (IsDrawn() && GetWidget()) {
        const auto bounds = CalculateSplitBounds(size(), vertical_, ratio_);
        const auto& target = opening_pane_ == first_ ? bounds.second : bounds.first;
        const auto from = opening_from_.value_or(GetLocalBounds());
        if (from == target) {
          opening_pane_ = nullptr;
        } else {
          const int remaining = std::max({std::abs(from.x() - target.x()), std::abs(from.right() - target.right()),
                                          std::abs(from.y() - target.y()), std::abs(from.bottom() - target.bottom())});
          const int travel = vertical_ ? height() - target.height() : width() - target.width();
          opening_.SetDuration(base::Milliseconds(180) * std::min(1.0, static_cast<double>(remaining) / std::max(1, travel)));
          SetPaintToLayer();
          layer()->SetFillsBoundsOpaquely(false);
          layer()->SetMasksToBounds(true);
          opening_.Start();
        }
      } else {
        opening_pane_ = nullptr;
      }
    }
    PositionPanes();
    LayoutSuperclass<views::View>(this);
  }
 private:
  void PositionPanes() {
    auto [first, second] = CalculateSplitBounds(size(), vertical_, ratio_);
    int divider = vertical_ ? first.height() : first.width();
    if (opening_.is_animating()) {
      const bool new_first = opening_pane_ == first_;
      const double progress = gfx::Tween::CalculateValue(gfx::Tween::EASE_OUT, opening_.GetCurrentValue());
      auto& original = new_first ? second : first;
      original = gfx::Tween::RectValueBetween(progress, opening_from_.value_or(GetLocalBounds()), original);
      if (new_first) {
        divider = (vertical_ ? original.y() : original.x()) - kSplitResizeWidth;
        const int extent = vertical_ ? first.height() : first.width();
        first = SplitAxisBounds(size(), vertical_, divider - extent, extent);
      } else {
        divider = vertical_ ? original.bottom() : original.right();
        second = SplitAxisBounds(size(), vertical_, divider + kSplitResizeWidth, vertical_ ? second.height() : second.width());
      }
    }
    first_->SetBoundsRect(first);
    second_->SetBoundsRect(second);
    divider_->SetBoundsRect(SplitAxisBounds(size(), vertical_, divider, kSplitResizeWidth));
  }
  void AnimationProgressed(const gfx::Animation*) override { PositionPanes(); }
  void AnimationEnded(const gfx::Animation*) override {
    opening_pane_ = nullptr;
    PositionPanes();
    DestroyLayer();
  }
  base::RepeatingClosure changed_;
  base::RepeatingCallback<void(bool)> resizing_;
  base::OneShotTimer resize_timer_;
  double pending_ratio_ = 0.5;
  bool snapped_ = false;
  bool vertical_;
  double ratio_ = 0.5;
  std::optional<int> drag_start_;
  raw_ptr<views::View> first_, second_;
  raw_ptr<views::ResizeArea> divider_;
  raw_ptr<views::View> opening_pane_ = nullptr;
  std::optional<gfx::Rect> opening_from_;
  gfx::LinearAnimation opening_{base::Milliseconds(180), gfx::LinearAnimation::kDefaultFrameRate, this};
};
BEGIN_METADATA(PaneSplit)
END_METADATA
// Access logging stays off the UI thread and never prevents navigation.
void RecordAccess(std::string action, base::FilePath path) {
  base::ThreadPool::PostTask(FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE, base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce([](std::string kind, base::FilePath file) {
        FileOperationLog log(kind, "menu", file.AsUTF8Unsafe(), "");
        log.Succeed();
      }, std::move(action), std::move(path)));
}
class HistoryPathLabel : public views::StyledLabel {
  METADATA_HEADER(HistoryPathLabel, views::StyledLabel)
 public:
  HistoryPathLabel(std::u16string text, const std::u16string& query) {
    SetText(text);
    if (!query.empty()) for (size_t pos = text.find(query); pos != std::u16string::npos;
        pos = text.find(query, pos + query.size())) {
      RangeStyleInfo style;
      style.override_color_id = ui::kColorSysPrimary;
      style.custom_font = gfx::FontList().DeriveWithWeight(gfx::Font::Weight::BOLD);
      AddStyleRange(gfx::Range(pos, pos + query.size()), style);
    }
  }
};
BEGIN_METADATA(HistoryPathLabel)
END_METADATA
std::unique_ptr<views::View> CreateHistoryRow(const FileHistoryEntry& entry,
                                             const std::u16string& query) {
  const int action = entry.action == "access" ? IDS_CRFILES_HISTORY_ACCESS :
      entry.action == "open_file" ? IDS_CRFILES_HISTORY_OPEN_FILE : entry.action == "move" ? IDS_CRFILES_HISTORY_MOVE : entry.action == "copy" ? IDS_CRFILES_HISTORY_COPY :
      entry.action == "empty_trash" ? IDS_CRFILES_EMPTY_TRASH : entry.action == "trash" ? IDS_CRFILES_MOVE_TRASH : entry.action == "restore" ? IDS_CRFILES_RESTORE :
      entry.action == "rename" ? IDS_CRFILES_RENAME : entry.action == "compress" ? IDS_CRFILES_COMPRESS : entry.action == "extract" ? IDS_CRFILES_EXTRACT : entry.action == "send" ? IDS_CRFILES_HISTORY_SEND : entry.action == "receive" ? IDS_CRFILES_HISTORY_RECEIVE : IDS_CRFILES_NEW_FOLDER;
  const int trigger = entry.trigger == "drag" ? IDS_CRFILES_HISTORY_DRAG : entry.trigger == "remote" ? IDS_CRFILES_HISTORY_REMOTE : IDS_CRFILES_HISTORY_MENU;
  const int result = entry.result == "success" ? IDS_CRFILES_HISTORY_SUCCESS : entry.result == "failed" ? IDS_CRFILES_HISTORY_FAILED :
      entry.result == "source_retained" ? IDS_CRFILES_HISTORY_RETAINED : entry.result == "canceled" ? IDS_CRFILES_HISTORY_CANCELED : IDS_CRFILES_HISTORY_PENDING;
  auto row = std::make_unique<views::View>();
  row->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(12), 4));
  row->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainer, 8));
  auto add = [&row, &query](std::u16string value, bool path = false) {
    auto* label = row->AddChildView(std::make_unique<HistoryPathLabel>(value, path ? query : std::u16string()));
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label->SetTooltipText(value);
  };
  add(base::TimeFormatShortDateAndTime(entry.time) + u"  ·  " + Text(action) + u"  ·  " + Text(trigger) + u"  ·  " + Text(result));
  if (!entry.source.empty()) add(Text(IDS_CRFILES_HISTORY_SOURCE) + u": " + base::UTF8ToUTF16(entry.source), true);
  if (!entry.destination.empty()) add(Text(IDS_CRFILES_HISTORY_DESTINATION) + u": " + base::UTF8ToUTF16(entry.destination), true);
  return row;
}
// Only visible rows plus one viewport of overscan have Views. Unmeasured rows
// retain an estimated height; measured heights preserve wrapping and scrolling.
class HistoryList : public views::View {
  METADATA_HEADER(HistoryList, views::View)
 public:
  HistoryList(views::ScrollView* scroll, std::u16string query,
              base::RepeatingClosure more)
      : scroll_(scroll), query_(std::move(query)) {
    more_ = AddButton(this, Text(IDS_CRFILES_MORE), std::move(more));
  }
  void Append(std::vector<FileHistoryEntry> entries, bool more) {
    for (auto& entry : entries) entries_.push_back(std::move(entry));
    heights_.resize(entries_.size(), 112);
    more_->SetVisible(more);
    more_->SetEnabled(true);
    RebuildOffsets();
    PreferredSizeChanged();
    InvalidateLayout();
    ScheduleUpdate();
  }
  FileHistoryCursor cursor() const {
    return entries_.empty() ? FileHistoryCursor{} :
        FileHistoryCursor{entries_.back().time, entries_.back().id};
  }
  void SetLoading(bool loading) { more_->SetEnabled(!loading); }
  void ScheduleUpdate() {
    if (!update_.IsRunning())
      update_.Start(FROM_HERE, base::Milliseconds(16), this, &HistoryList::UpdateRows);
  }
  gfx::Size CalculatePreferredSize(const views::SizeBounds& available) const override {
    const int w = available.width().is_bounded() ? available.width().value() : 760;
    return gfx::Size(w, offsets_.empty() ? 32 : offsets_.back() + (more_->GetVisible() ? 52 : 16));
  }
  void Layout(PassKey) override {
    for (const auto& [index, row] : rows_)
      row->SetBounds(16, offsets_[index], std::max(0, width() - 32), heights_[index]);
    if (!offsets_.empty()) more_->SetBounds(16, offsets_.back(), std::max(0, width() - 32), 36);
    LayoutSuperclass<views::View>(this);
    if (pending_scroll_) {
      const int position = *pending_scroll_;
      pending_scroll_.reset();
      scroll_->ScrollToPosition(scroll_->vertical_scroll_bar(), position);
    }
    ScheduleUpdate();
  }
 private:
  void RebuildOffsets() {
    offsets_.resize(heights_.size() + 1);
    offsets_[0] = 16;
    for (size_t i = 0; i < heights_.size(); ++i)
      offsets_[i + 1] = offsets_[i] + heights_[i] + 8;
  }
  size_t RowAt(int y) const {
    auto it = std::upper_bound(offsets_.begin(), offsets_.end(), y);
    return std::min(entries_.size(), static_cast<size_t>(it - offsets_.begin() - (it != offsets_.begin())));
  }
  void UpdateRows() {
    if (!IsDrawn() || width() <= 32 || entries_.empty()) return;
    auto viewport = scroll_->GetVisibleRect();
    if (viewport.IsEmpty()) return;
    const size_t anchor = RowAt(viewport.y());
    const int old_anchor = offsets_[anchor];
    const bool width_changed = measured_width_ != width();
    if (width_changed) {
      measured_width_ = width();
      std::fill(heights_.begin(), heights_.end(), 112);
      RebuildOffsets();
      viewport.Offset(0, offsets_[anchor] - old_anchor);
    }
    const size_t first = RowAt(std::max(0, viewport.y() - viewport.height()));
    const size_t last = std::min(entries_.size(), RowAt(viewport.bottom() + viewport.height()) + 1);
    for (auto it = rows_.begin(); it != rows_.end();) {
      if (it->first >= first && it->first < last) { ++it; continue; }
      auto* row = it->second.get();
      it = rows_.erase(it);
      RemoveChildViewT(row);
    }
    bool changed = width_changed;
    for (size_t i = first; i < last; ++i) {
      if (!rows_.contains(i)) rows_[i] = AddChildView(CreateHistoryRow(entries_[i], query_));
      const int height = std::max(40, rows_[i]->GetHeightForWidth(width() - 32));
      changed |= heights_[i] != height;
      heights_[i] = height;
    }
    if (changed) RebuildOffsets();
    if (changed) PreferredSizeChanged();
    for (const auto& [index, row] : rows_)
      row->SetBounds(16, offsets_[index], width() - 32, heights_[index]);
    more_->SetBounds(16, offsets_.back(), width() - 32, 36);
    // Measuring overscan above the viewport must not move the visible record.
    const int shift = offsets_[anchor] - old_anchor;
    // Apply after ScrollView has adopted the new preferred content height,
    // otherwise a growing row near the bottom can clamp the anchor too early.
    if (shift) pending_scroll_ = std::max(0, scroll_->GetVisibleRect().y() + shift);
    if (changed) InvalidateLayout();
  }
  raw_ptr<views::ScrollView> scroll_;
  std::u16string query_;
  std::vector<FileHistoryEntry> entries_;
  std::vector<int> heights_, offsets_;
  std::map<size_t, raw_ptr<views::View>> rows_;
  raw_ptr<views::LabelButton> more_ = nullptr;
  int measured_width_ = 0;
  std::optional<int> pending_scroll_;
  base::OneShotTimer update_;
};
BEGIN_METADATA(HistoryList)
END_METADATA
class HistorySearchField : public views::Textfield {
  METADATA_HEADER(HistorySearchField, views::Textfield)
 public:
  explicit HistorySearchField(base::RepeatingCallback<void(bool)> focus) : focus_(std::move(focus)) {
    RemoveHoverEffect();
    views::FocusRing::Remove(this);
    SetBackgroundColor(ui::kColorSysBaseContainer);
  }
  void OnFocus() override {
    views::Textfield::OnFocus();
    SetBackgroundColor(ui::kColorSysBaseContainerElevated);
    focus_.Run(true);
  }
  void OnBlur() override {
    views::Textfield::OnBlur();
    SetBackgroundColor(ui::kColorSysBaseContainer);
    focus_.Run(false);
  }
  // The enclosing capsule paints the background and focus outline.
  void OnPaintBackground(gfx::Canvas*) override {}
  void OnPaintBorder(gfx::Canvas*) override {}
 private:
  base::RepeatingCallback<void(bool)> focus_;
};
BEGIN_METADATA(HistorySearchField)
END_METADATA
class HistoryHeader : public views::View {
  METADATA_HEADER(HistoryHeader, views::View)
 public:
  explicit HistoryHeader(views::TextfieldController* controller, base::RepeatingClosure filter, base::RepeatingClosure clear) {
    SetPreferredSize(gfx::Size(0, 64));
    title_ = AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_HISTORY)));
    title_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    group_ = AddChildView(std::make_unique<views::View>());
    group_->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainer, 20));
    filter_ = AddIconButton(group_, vector_icons::kFilterOldIcon, IDS_CRFILES_HISTORY_FILTER, std::move(filter));
    filter_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    filter_->SetBorder(views::CreateEmptyBorder(8));
    filter_->SetBackground(nullptr);
    filter_->SetInstallFocusRingOnFocus(false);
    views::InkDrop::Get(filter_)->SetMode(views::InkDropHost::InkDropMode::OFF);
    for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
      filter_->SetImageModel(state, ui::ImageModel::FromVectorIcon(vector_icons::kFilterOldIcon, ui::kColorSysPrimary, 20));
    chip_ = group_->AddChildView(std::make_unique<views::View>());
    chip_->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysPrimaryContainer, 14));
    chip_label_ = chip_->AddChildView(std::make_unique<views::Label>());
    chip_label_->SetEnabledColor(ui::kColorSysOnPrimaryContainer);
    chip_label_->SetElideBehavior(gfx::ELIDE_TAIL);
    chip_close_ = AddIconButton(chip_, kCloseIcon, IDS_CRFILES_CLOSE, std::move(clear));
    chip_close_->SetBorder(views::CreateEmptyBorder(4));
    chip_close_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    chip_close_->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(kCloseIcon, ui::kColorSysOnPrimaryContainer, 16));
    views::InstallCircleHighlightPathGenerator(chip_close_);
    chip_->SetVisible(false);
    separator_ = group_->AddChildView(std::make_unique<views::Separator>());
    separator_->SetOrientation(views::Separator::Orientation::kVertical);
    search_ = group_->AddChildView(std::make_unique<HistorySearchField>(
        base::BindRepeating(&HistoryHeader::SetSearchFocused, weak_.GetWeakPtr())));
    search_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(9, 12)));
    search_->SetBackgroundColor(ui::kColorSysBaseContainer);
    search_->set_controller(controller);
    search_->SetPlaceholderText(Text(IDS_CRFILES_HISTORY_SEARCH));
    search_->SetAccessibleName(Text(IDS_CRFILES_HISTORY_SEARCH));
  }
  views::Textfield* search() { return search_; }
  views::LabelButton* filter() { return filter_; }
  void SetFilter(std::u16string label) {
    chip_label_->SetText(label);
    chip_label_->SetTooltipText(label);
    chip_close_->SetAccessibleName(Text(IDS_CRFILES_CLOSE) + u" " + label);
    chip_->SetVisible(!label.empty());
    InvalidateLayout();
  }
  void Layout(PassKey) override {
    const int title_width = title_->GetPreferredSize({}).width();
    const int field_width = std::min(560, std::max(0, width() - 2 * (title_width + 32)));
    title_->SetBounds(16, 12, title_width, 40);
    group_->SetBounds((width() - field_width) / 2, 12, field_width, 40);
    const int filter_width = std::min(field_width, 44);
    filter_->SetBounds(4, 2, std::max(0, filter_width - 8), 36);
    int field_start = filter_width;
    if (chip_->GetVisible()) {
      const int chip_width = std::min(chip_label_->GetPreferredSize({}).width() + 40,
          std::max(0, field_width - filter_width - 120));
      chip_->SetBounds(field_start, 6, chip_width, 28);
      chip_label_->SetBounds(10, 0, std::max(0, chip_width - 38), 28);
      chip_close_->SetBounds(std::max(0, chip_width - 26), 2, 24, 24);
      field_start += chip_width + 8;
    }
    separator_->SetBounds(field_start, 10, 1, 20);
    search_->SetBounds(field_start + 1, 1, std::max(0, field_width - field_start - 17), 38);
    LayoutSuperclass<views::View>(this);
  }
 private:
  void SetSearchFocused(bool focused) {
    group_->SetBackground(views::CreateRoundedRectBackground(focused ?
        ui::kColorSysBaseContainerElevated : ui::kColorSysBaseContainer, 20));
    group_->SetBorder(focused ? views::CreateRoundedRectBorder(1, 20, ui::kColorSysPrimary) : nullptr);
  }
  raw_ptr<views::View> chip_ = nullptr;
  raw_ptr<views::Label> chip_label_ = nullptr;
  raw_ptr<views::LabelButton> chip_close_ = nullptr;
  raw_ptr<views::View> group_ = nullptr;
  raw_ptr<views::LabelButton> filter_ = nullptr;
  raw_ptr<views::Separator> separator_ = nullptr;
  raw_ptr<views::Label> title_ = nullptr;
  raw_ptr<views::Textfield> search_ = nullptr;
  base::WeakPtrFactory<HistoryHeader> weak_{this};
};
BEGIN_METADATA(HistoryHeader)
END_METADATA
class ActivePaneBorder : public views::Border, public views::AnimationDelegateViews {
 public:
  ActivePaneBorder(views::View* view, ui::ColorId surface, SplitSelectionStyle style)
      : views::AnimationDelegateViews(view), view_(view), surface_(surface), style_(style) {}
  gfx::Insets GetInsets() const override { return gfx::Insets(4); }
  gfx::Size GetMinimumSize() const override { return gfx::Size(8, 8); }
  void Paint(const views::View& view, gfx::Canvas* canvas) override {
    if (!started_ && gfx::Animation::ShouldRenderRichAnimation() && view.IsDrawn()) {
      started_ = true;
      animation_.SetDuration(base::Milliseconds(style_ == SplitSelectionStyle::kFlow ? 900 :
          style_ == SplitSelectionStyle::kCorners ? 740 : 618));
      animation_.Start();
    }
    const int w = view.width(), h = view.height();
    if (w <= 0 || h <= 0) return;
    const auto surface = view.GetColorProvider()->GetColor(surface_);
    canvas->FillRect(gfx::Rect(0, 0, w, 4), surface);
    canvas->FillRect(gfx::Rect(0, std::max(0, h - 4), w, 4), surface);
    canvas->FillRect(gfx::Rect(0, 0, 4, h), surface);
    canvas->FillRect(gfx::Rect(std::max(0, w - 4), 0, 4, h), surface);
    constexpr double kOneCycle = 6.283185307179586;
    const float pulse = animation_.is_animating() ?
        0.725f + 0.275f * static_cast<float>(std::cos(kOneCycle * animation_.GetCurrentValue())) : 1.f;
    cc::PaintFlags flags;
    flags.setColor(view.GetColorProvider()->GetColor(ui::kColorSysOnSurface));
    flags.setAlphaf(0.18f * (style_ == SplitSelectionStyle::kTopBar ? pulse : 1.f));
    const auto draw_ring = [&](int inset) {
      if (w <= 2 * inset || h <= 2 * inset) return;
      canvas->DrawRect(gfx::Rect(inset, inset, w - 2 * inset, 1), flags);
      canvas->DrawRect(gfx::Rect(inset, h - inset - 1, w - 2 * inset, 1), flags);
      canvas->DrawRect(gfx::Rect(inset, inset + 1, 1, std::max(0, h - 2 * inset - 2)), flags);
      canvas->DrawRect(gfx::Rect(w - inset - 1, inset + 1, 1, std::max(0, h - 2 * inset - 2)), flags);
    };
    draw_ring(0);
    flags.setColor(view.GetColorProvider()->GetColor(ui::kColorSysPrimary));
    const bool moving = animation_.is_animating();
    const float progress = moving ? static_cast<float>(animation_.GetCurrentValue()) : 1.f;
    if (style_ == SplitSelectionStyle::kFlow) {
      const SkPoint points[] = {{0.f, 0.f},
                                {static_cast<float>(w), static_cast<float>(h)}};
      const SkColor4f colors[] = {
          SkColor4f::FromColor(SkColorSetRGB(66, 165, 245)),
          SkColor4f::FromColor(SkColorSetRGB(156, 100, 255))};
      flags.setShader(cc::PaintShader::MakeLinearGradient(
          points, colors, nullptr, 2, SkTileMode::kClamp));
      for (int inset = 1; inset <= 3; ++inset) {
        flags.setAlphaf(0.12f * (4 - inset));
        draw_ring(inset);
      }
      // Intersect each fading trail segment with the four edges so corners
      // remain on the border and never cut across the pane contents.
      const float right = w - 2.f, bottom = h - 2.f;
      const float horizontal = right - 2.f, vertical = bottom - 2.f;
      const float perimeter = 2.f * (horizontal + vertical);
      if (moving && horizontal > 0.f && vertical > 0.f) {
        flags.setAntiAlias(true);
        flags.setStrokeWidth(2.f);
        const std::array<float, 5> edges = {0.f, horizontal, horizontal + vertical,
                                           2.f * horizontal + vertical, perimeter};
        const auto point = [&](int edge, float distance) {
          const float d = distance - edges[edge];
          switch (edge) {
            case 0: return gfx::PointF(2.f + d, 2.f);
            case 1: return gfx::PointF(right, 2.f + d);
            case 2: return gfx::PointF(right - d, bottom);
            default: return gfx::PointF(2.f, bottom - d);
          }
        };
        const float trail = std::min(240.f, perimeter * 0.22f);
        const float fade = std::min(1.f, (1.f - progress) * 10.f);
        for (int i = 0; i < 48; ++i) {
          const float start = progress * perimeter - trail + trail * i / 48.f;
          const float end = start + trail / 48.f;
          flags.setAlphaf(0.85f * (i + 1) / 48.f * fade);
          for (int wrap = 0; wrap <= 1; ++wrap) {
            for (int edge = 0; edge < 4; ++edge) {
              const float from = std::max(start + wrap * perimeter, edges[edge]);
              const float to = std::min(end + wrap * perimeter, edges[edge + 1]);
              if (to > from) canvas->DrawLine(point(edge, from), point(edge, to), flags);
            }
          }
        }
      }
    } else if (style_ == SplitSelectionStyle::kCorners) {
      const float arrival = std::min(1.f, progress * 740.f / 240.f);
      const float inset = 3.f * (1.f - (1.f - arrival) * (1.f - arrival));
      const float length = std::min(20.f, std::min(w, h) * 0.25f);
      flags.setAntiAlias(true);
      flags.setAlphaf(0.45f + 0.45f * arrival);
      for (int side = 0; side < 2; ++side) {
        const float x = side ? w - inset - 1.f : inset;
        canvas->DrawRect(gfx::RectF(side ? x - length + 1.f : x, inset, length, 1.f), flags);
        canvas->DrawRect(gfx::RectF(x, inset, 1.f, length), flags);
      }
      if (moving && arrival >= 1.f) {
        const float scan = (progress * 740.f - 240.f) / 500.f;
        const float scan_length = std::min(80.f, w * 0.2f);
        flags.setAlphaf(0.7f * std::sin(3.14159265f * scan));
        canvas->DrawRect(gfx::RectF(3.f + (w - 6.f - scan_length) * scan, 2.f, scan_length, 1.f), flags);
      }
    } else {
      flags.setAntiAlias(true);
      flags.setAlphaf(pulse);
      const int bar_width = std::min(36, std::max(0, w - 8));
      canvas->DrawRoundRect(gfx::RectF((w - bar_width) / 2.f, 1.f, bar_width, 3.f), 1.5f, flags);
    }
  }
  void AnimationProgressed(const gfx::Animation*) override {
    auto* view = view_.view();
    if (!view || !view->IsDrawn()) return;
    const int w = view->width(), h = view->height();
    if (w <= 0 || h <= 0) return;
    // Repaint only the border strips; selection animations never invalidate layout.
    view->SchedulePaintInRect(gfx::Rect(0, 0, w, 4));
    view->SchedulePaintInRect(gfx::Rect(0, std::max(0, h - 4), w, 4));
    view->SchedulePaintInRect(gfx::Rect(0, 0, 4, h));
    view->SchedulePaintInRect(gfx::Rect(std::max(0, w - 4), 0, 4, h));
  }
  void OnViewVisibilityChanged(views::View*, views::View*, bool visible) override {
    if (!visible) animation_.Stop();
  }
  void AnimationEnded(const gfx::Animation* animation) override { AnimationProgressed(animation); }
 private:
  bool started_ = false;
  views::ViewTracker view_;
  const ui::ColorId surface_;
  const SplitSelectionStyle style_;
  gfx::LinearAnimation animation_{base::Milliseconds(618), gfx::LinearAnimation::kDefaultFrameRate, this};
};
class PaneActivation : public ui::EventHandler {
 public:
  PaneActivation(views::View* view, base::RepeatingCallback<void(bool)> activate) : view_(view), activate_(std::move(activate)) { view_->AddPreTargetHandler(this); }
  ~PaneActivation() override { view_->RemovePreTargetHandler(this); }
  void SetFileKeys(base::RepeatingCallback<bool(const ui::KeyEvent&)> callback) { file_keys_ = std::move(callback); }
  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->type() == ui::EventType::kMousePressed) activate_.Run(true);
  }
  void OnKeyEvent(ui::KeyEvent* event) override {
    if (event->type() == ui::EventType::kKeyPressed && PrimaryModifierDown(*event) &&
        !(event->flags() & (ui::EF_CONTROL_DOWN | ui::EF_COMMAND_DOWN | ui::EF_ALT_DOWN | ui::EF_SHIFT_DOWN) & ~kPrimaryModifier) &&
        event->key_code() == ui::VKEY_A) {
      if (file_keys_ && file_keys_.Run(*event)) { event->SetHandled(); event->StopPropagation(); }
      return;
    }
#if BUILDFLAG(IS_MAC)
    if (event->type() == ui::EventType::kKeyPressed && event->key_code() == ui::VKEY_BACK &&
        event->IsCommandDown() && !event->IsControlDown() && !event->IsAltDown() && !event->IsShiftDown()) {
      if (file_keys_ && file_keys_.Run(*event)) { event->SetHandled(); event->StopPropagation(); }
      return;
    }
#endif
    if (event->type() != ui::EventType::kKeyPressed || event->IsControlDown() || event->IsAltDown() || event->IsCommandDown()) return;
    switch (event->key_code()) {
      case ui::VKEY_CONTROL: case ui::VKEY_SHIFT: case ui::VKEY_MENU:
      case ui::VKEY_LCONTROL: case ui::VKEY_RCONTROL: case ui::VKEY_LSHIFT: case ui::VKEY_RSHIFT:
      case ui::VKEY_LMENU: case ui::VKEY_RMENU:
      case ui::VKEY_LWIN: case ui::VKEY_RWIN: return;
      default: activate_.Run(false);
    }
    if (file_keys_ && file_keys_.Run(*event)) { event->SetHandled(); event->StopPropagation(); }
  }
 private:
  base::RepeatingCallback<bool(const ui::KeyEvent&)> file_keys_;
  raw_ptr<views::View> view_;
  base::RepeatingCallback<void(bool)> activate_;
};
enum ContextCommand {
  kContextOpen = 10000, kContextCut, kContextCopy, kContextMoveTo, kContextCopyTo,
  kContextRename, kContextCompress, kContextTrash, kContextTerminal, kContextPath,
  kContextProperties, kContextNewTab, kContextSplit, kContextOpenWith, kContextPaste,
  kContextSplitLeft, kContextSplitRight, kContextSplitTop, kContextSplitBottom, kContextShare, kContextSync, kContextQuickShare,
  kContextExtractHere, kContextExtractFolder, kContextExtractTo,
  kContextShowPackageContents, kContextNewFolder, kContextSortBy, kContextSelectAll,
  kContextAddTags, kContextDeleteTag,
  kContextSortFirst = 10050,
  kContextApplication = 10100
};
class FilesView;
FilesView* g_contents = nullptr;
base::RepeatingCallbackList<void(bool)>& SidebarObservers() {
  static base::NoDestructor<base::RepeatingCallbackList<void(bool)>> observers;
  return *observers;
}
bool g_quitting = false;
std::unique_ptr<CrFilesTray>& Tray() {
  static base::NoDestructor<std::unique_ptr<CrFilesTray>> tray;
  return *tray;
}
std::vector<base::WeakPtr<views::Widget>>& HiddenOwnedWindows() {
  static base::NoDestructor<std::vector<base::WeakPtr<views::Widget>>> windows;
  return *windows;
}
std::unique_ptr<views::Widget>& Window() {
  static base::NoDestructor<std::unique_ptr<views::Widget>> widget;
  return *widget;
}
std::unique_ptr<views::WidgetDelegate>& Delegate() {
  static base::NoDestructor<std::unique_ptr<views::WidgetDelegate>> delegate;
  return *delegate;
}
struct Tab {
  int id;
  bool pinned = false;
  raw_ptr<views::View> pane_host = nullptr;
  raw_ptr<FilesView> active_pane = nullptr;
  std::optional<base::DictValue> deferred_tree;
  std::string deferred_active, deferred_title;
  uint64_t request = 0;
  base::FilePath path;
  std::vector<base::FilePath> history;
  int history_index = -1;
  FileViewOptions options;
  bool loading = false;
  bool error = false;
  int scroll = 0;
  int sidebar_selection = 0;
  DirectoryEntries entries;
  std::map<base::FilePath, EntryVisuals> visuals;
  std::set<base::FilePath> failed_thumbnails;
  std::set<base::FilePath> removed;
  uint64_t metadata_generation = 0;
  std::set<base::FilePath> selected;
  std::vector<size_t> sorted_entries;
  std::optional<FileSort> sorted_mode;
  bool sorted_hidden = false;
  void InvalidateOrder() { sorted_mode.reset(); }

};
// The shell owns shared navigation, tabs and sharing. Each leaf has its own
// controller and file state; hidden leaf chrome is never registered for shortcuts.
class FilesView : public views::View, public views::TextfieldController, public views::ResizeAreaDelegate, public views::DragController, public ui::SimpleMenuModel::Delegate, public views::ContextMenuController, public ui::SelectFileDialog::Listener, public views::BoundsAnimatorObserver {
  METADATA_HEADER(FilesView, views::View)
 public:
  void NewTabFromTitlebar() { NewCurrentTab(); }
  bool CanReopenTab() const { return !closed_tabs_.empty(); }
  void ReopenTab() {
    if (closed_tabs_.empty()) return;
    auto saved = std::move(closed_tabs_.back()); closed_tabs_.pop_back();
    SuspendPanePopups();
    for (auto& tab : tabs_) tab.pane_host->SetVisible(false);
    tabs_.push_back(Tab{.id = next_id_++}); active_ = tabs_.size() - 1;
    auto* host = split_host_->AddChildView(std::make_unique<views::View>());
    host->SetLayoutManager(std::make_unique<views::FillLayout>());
    Current().pane_host = host;
    Current().deferred_tree = saved.FindDict("tree")->Clone();
    Current().deferred_active = *saved.FindString("active_pane");
    Current().pinned = saved.FindBool("pinned").value_or(false);
    SelectPaneTab(Current().id);
  }
  void BookmarkTabs() {
    std::vector<base::FilePath> paths;
    auto collect = [&](auto&& self, const base::DictValue& tree) -> void {
      if (const auto* first = tree.FindDict("first")) self(self, *first);
      if (const auto* second = tree.FindDict("second")) self(self, *second);
      if (tree.FindBool("history_page").value_or(false) || tree.FindString("tag_id")) return;
      if (const auto* path = tree.FindString("path")) {
        auto folder = base::FilePath::FromUTF8Unsafe(*path);
        if (folder.IsAbsolute() && !folder.ReferencesParent()) paths.push_back(std::move(folder));
      }
    };
    for (const auto& tab : tabs_) {
      auto tree = tab.deferred_tree ? tab.deferred_tree->Clone() :
          SerializePane(tab.pane_host->children().front().get());
      collect(collect, tree);
    }
    bookmarks_->BookmarkPaths(std::move(paths));
  }
  explicit FilesView(FilesView* root = nullptr, base::FilePath initial_path = {}, bool history_page = false, bool deferred_start = false, bool search_page = false, int64_t tag_id = -1, std::string tag_name = {}, SearchScope search_scope = {}, bool network_page = false) : search_scope_(std::move(search_scope)), tag_id_(tag_id), tag_name_(std::move(tag_name)), history_page_(history_page || search_page || network_page), search_page_(search_page), network_page_(network_page), root_(root), sharing_(root ? nullptr : std::make_shared<NearbySharing>()) {
    if (!root_) g_contents = this;
    if (root_) {
      // Keep the pane outline in the same layer subtree as its scrolling content.
      SetPaintToLayer(); layer()->SetFillsBoundsOpaquely(false);
      SetNotifyEnterExitOnChild(true);
    }
    pane_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical));
    if (root_) SetBackground(views::CreateSolidBackground((history_page_ && tag_id_ < 0) ? ui::kColorSysBase : kFolderSurfaceColor));
    else SetBackground(std::make_unique<NavigationBackground>());
    if (!root_) {
    toolbar_ = AddChildView(std::make_unique<views::View>());
    toolbar_->SetBackground(std::make_unique<NavigationBackground>());
    auto* tools = toolbar_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(6, 8), 4));
    tools->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    auto add_navigation_button = [&](const gfx::VectorIcon& icon, int label,
                                     base::RepeatingClosure callback) {
      auto* button = AddIconButton(toolbar_, icon, label, std::move(callback));
      button->SetPreferredSize(gfx::Size(28, 28));
      button->SetBorder(views::CreateEmptyBorder(4));
      button->SetHorizontalAlignment(gfx::ALIGN_CENTER);
      button->SetImageCentered(true);
      views::InstallCircleHighlightPathGenerator(button);
      return button;
    };
    back_ = add_navigation_button(vector_icons::kArrowBackIcon, IDS_CRFILES_BACK,
        base::BindRepeating(&FilesView::History, base::Unretained(this), -1));
    forward_ = add_navigation_button(vector_icons::kArrowForwardIcon, IDS_CRFILES_FORWARD,
        base::BindRepeating(&FilesView::History, base::Unretained(this), 1));
    add_navigation_button(kArrowUpwardIcon, IDS_CRFILES_UP, base::BindRepeating(&FilesView::Up, base::Unretained(this)));
    add_navigation_button(vector_icons::kRefreshIcon, IDS_CRFILES_REFRESH, base::BindRepeating(&FilesView::Refresh, base::Unretained(this)));
    auto* navigation_separator = toolbar_->AddChildView(std::make_unique<views::Separator>());
    navigation_separator->SetOrientation(views::Separator::Orientation::kVertical);
    navigation_separator->SetPreferredLength(20);
    // The toolbar already contributes 4px spacing on each side.
    navigation_separator->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 2)));
    auto* path_area = toolbar_->AddChildView(std::make_unique<views::View>());
    path_area_ = path_area;
    path_area->SetLayoutManager(std::make_unique<views::FillLayout>());
    path_area->SetPreferredSize(gfx::Size(240, 32));
    tools->SetFlexForView(path_area, 1);
    crumbs_scroll_ = path_area->AddChildView(std::make_unique<NavigationPathView>(
        Deferred(base::BindRepeating(&FilesView::EditAddress, weak_.GetWeakPtr()))));
    crumbs_scroll_->SetPaintToLayer();
    crumbs_scroll_->layer()->SetFillsBoundsOpaquely(false);
    address_ = path_area->AddChildView(std::make_unique<PathTextfield>(
        Deferred(base::BindRepeating(&FilesView::EndAddressEditing, weak_.GetWeakPtr()))));
    address_->SetAccessibleName(Text(IDS_CRFILES_LOCATION));
    address_->set_controller(this);
    address_completion_ = std::make_unique<PathCompletion>(address_);
    address_->SetVisible(false);
    address_->SetPaintToLayer();
    address_->layer()->SetFillsBoundsOpaquely(false);
    auto* view_controls_host = toolbar_->AddChildView(std::make_unique<views::View>());
    auto* view_controls_host_layout = view_controls_host->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical));
    view_controls_host_layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    auto* view_controls = view_controls_host->AddChildView(std::make_unique<ViewControlsCapsule>());
    view_toggle_ = AddIconButton(view_controls, kGridViewIcon, IDS_CRFILES_ICONS,
        base::BindRepeating(&FilesView::ToggleViewMode, base::Unretained(this)));
    view_toggle_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 10)));
    view_toggle_->SetPreferredSize(gfx::Size(36, 30));
    views::HighlightPathGenerator::Install(view_toggle_, std::make_unique<SplitButtonHighlightPath>(true));
    views::InkDrop::Get(view_toggle_)->SetHighlightOpacity(0.16f);
    views::InkDrop::Get(view_toggle_)->SetVisibleOpacity(0.26f);
    view_toggle_->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    options_button_ = AddIconButton(view_controls, vector_icons::kExpandMoreOldIcon, IDS_CRFILES_VIEW_OPTIONS,
        base::BindRepeating(&FilesView::ShowViewOptions, base::Unretained(this)));
    options_button_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 10)));
    options_button_->SetPreferredSize(gfx::Size(28, 30));
    views::HighlightPathGenerator::Install(options_button_, std::make_unique<SplitButtonHighlightPath>(false));
    views::InkDrop::Get(options_button_)->SetHighlightOpacity(0.16f);
    views::InkDrop::Get(options_button_)->SetVisibleOpacity(0.26f);
    options_button_->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    }
    auto* body = AddChildView(std::make_unique<views::View>());
    if (root_) pane_body_ = body;
    auto* body_layout = body->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal));
    layout->SetFlexForView(body, 1);
    if (!root_) {
    sidebar_viewport_ = body->AddChildView(std::make_unique<SidebarViewport>());
    sidebar_viewport_->SetBackground(std::make_unique<NavigationBackground>());
    sidebar_viewport_->SetPreferredSize(gfx::Size(sidebar_display_width_, 0));
    sidebar_ = sidebar_viewport_->AddChildView(std::make_unique<views::View>());
    auto* sidebar = sidebar_.get();
    sidebar->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::TLBR(12, 0, 12, 12), 6));
    sidebar->SetPreferredSize(gfx::Size(sidebar_width_, 0));
    auto add_place = [&](const gfx::VectorIcon& icon, int label, base::RepeatingClosure callback) {
      auto* button = sidebar->AddChildView(std::make_unique<SidebarButton>(
          base::BindRepeating(&FilesView::ActivateSidebar, base::Unretained(this), label, std::move(callback)), Text(label)));
      sidebar_buttons_.emplace(label, button);
      button->set_context_menu_controller(this);
      button->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      button->SetImageLabelSpacing(10);
      button->SetImageModel(views::Button::STATE_NORMAL,
          ui::ImageModel::FromVectorIcon(icon, ui::kColorSysOnSurface, 20));
    };
    add_place(vector_icons::kHomeIcon, IDS_CRFILES_HOME, base::BindRepeating(&FilesView::GoHome, base::Unretained(this)));
    add_place(vector_icons::kDesktopWindowsIcon, IDS_CRFILES_DESKTOP, base::BindRepeating(&FilesView::Desktop, base::Unretained(this)));
    add_place(vector_icons::kDownloadIcon, IDS_CRFILES_DOWNLOADS, base::BindRepeating(&FilesView::Downloads, base::Unretained(this)));
#if BUILDFLAG(IS_MAC)
    add_place(vector_icons::kFolderOpenIcon, IDS_CRFILES_APPLICATIONS,
        base::BindRepeating([](base::WeakPtr<FilesView> view) {
          if (view) view->Navigate(base::FilePath(FILE_PATH_LITERAL("/Applications")));
        }, weak_.GetWeakPtr()));
#endif
    add_place(vector_icons::kFolderOpenIcon, IDS_CRFILES_RECEIVED, base::BindRepeating(&FilesView::Received, base::Unretained(this)));
    sidebar_buttons_.at(IDS_CRFILES_RECEIVED)->SetVisible(false);
    add_place(vector_icons::kDevicesIcon, IDS_CRFILES_NETWORK, base::BindRepeating(&FilesView::ShowNetworkTab, base::Unretained(this)));
    sidebar_buttons_.at(IDS_CRFILES_NETWORK)->SetVisible(IsSmbEnabledForSession());
    add_place(kDeleteIcon, IDS_CRFILES_TRASH, base::BindRepeating(&FilesView::Trash, base::Unretained(this)));
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([] { return std::array{LoadTrashIcon(), LoadTrashIcon(kTabIconSize), LoadNetworkWorkgroupIcon(kTabIconSize), LoadNetworkWorkgroupIcon(20)}; }),
        base::BindOnce([](base::WeakPtr<FilesView> view, std::array<gfx::ImageSkia, 4> images) {
          if (!view) return;
          if (!images[0].isNull()) {
            auto* button = view->sidebar_buttons_.at(IDS_CRFILES_TRASH).get();
            for (auto state : {views::Button::STATE_NORMAL, views::Button::STATE_HOVERED,
                               views::Button::STATE_PRESSED, views::Button::STATE_DISABLED})
              button->SetImageModel(state, ui::ImageModel::FromImageSkia(images[0]));
          }
          if (!images[1].isNull()) view->trash_tab_icon_ = ui::ImageModel::FromImageSkia(images[1]);
          if (!images[2].isNull()) view->network_tab_icon_ = ui::ImageModel::FromImageSkia(images[2]);
          if (!images[3].isNull()) {
            view->network_location_icon_ = ui::ImageModel::FromImageSkia(images[3]);
            auto* sidebar = view->sidebar_buttons_.at(IDS_CRFILES_NETWORK).get();
            auto* breadcrumb = views::AsViewClass<BreadcrumbButton>(view->network_breadcrumb_.view());
            for (auto state : {views::Button::STATE_NORMAL, views::Button::STATE_HOVERED,
                               views::Button::STATE_PRESSED, views::Button::STATE_DISABLED}) {
              sidebar->SetImageModel(state, view->network_location_icon_);
              if (breadcrumb) breadcrumb->SetImageModel(state, view->network_location_icon_);
            }
          }
          view->UpdateTabStates();
        }, weak_.GetWeakPtr()));
    sidebar->AddChildView(std::make_unique<views::Separator>());
    add_place(kTagsIcon, IDS_CRFILES_TAGS, base::BindRepeating(&FilesView::ShowTagsTab, base::Unretained(this)));
    sidebar->AddChildView(std::make_unique<views::Separator>());
    auto* bookmark_scroll = sidebar->AddChildView(std::make_unique<BookmarkScrollView>());
    bookmarks_ = bookmark_scroll->SetContents(std::make_unique<BookmarksView>(
        base::BindRepeating([](base::WeakPtr<FilesView> view, base::FilePath path) {
          if (view) view->Navigate(path);
        }, weak_.GetWeakPtr()),
        base::BindRepeating([](base::WeakPtr<FilesView> view, base::FilePath path) {
          if (view) view->AddPaneTab(path);
        }, weak_.GetWeakPtr()), TagOpener()));
    static_cast<views::BoxLayout*>(sidebar->GetLayoutManager())->SetFlexForView(bookmark_scroll, 1);

#if BUILDFLAG(IS_WIN)
    auto* drive_scroll = sidebar->AddChildView(std::make_unique<views::ScrollView>());
    drive_scroll->SetUseContentsPreferredSize(true);
    drive_scroll->SetBackgroundColor(std::nullopt);
    drive_scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    auto drives = std::make_unique<views::View>();
    drives->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(), 6));
    drive_list_ = drive_scroll->SetContents(std::move(drives));
    static_cast<views::BoxLayout*>(sidebar->GetLayoutManager())->SetFlexForView(drive_scroll, 1);
#endif
    auto* resize = body->AddChildView(std::make_unique<SidebarResizeGrip>(this,
        base::BindRepeating(&FilesView::RestoreSidebar, base::Unretained(this))));
    sidebar_resize_ = resize;
    body->SetEventTargeter(std::make_unique<views::ViewTargeter>(std::make_unique<ResizeGripTargeter>(
        resize, false, base::BindRepeating(&SidebarResizeGrip::IsDragging, base::Unretained(resize)))));
    // Two-DIP divider without horizontal padding.
    resize->SetPreferredSize(gfx::Size(2, 0));
    resize->SetAccessibleName(Text(IDS_CRFILES_SIDEBAR_WIDTH));
    split_host_ = body->AddChildView(std::make_unique<views::View>());
    split_host_->SetLayoutManager(std::make_unique<views::FillLayout>());
    // Meet the sidebar divider without a gap; retain the other outer insets.
    split_host_->SetBorder(views::CreateEmptyBorder(
        gfx::Insets::TLBR(kPaneOuterInset, 0, kPaneOuterInset, kPaneOuterInset)));
    body_layout->SetFlexForView(split_host_, 1);
    split_host_->SetVisible(!root_);
    }
    auto* center = body->AddChildView(std::make_unique<views::View>());
    center->SetVisible(root_ != nullptr);
    center->SetBackground(views::CreateSolidBackground((history_page_ && tag_id_ < 0) ? ui::kColorSysBase : kFolderSurfaceColor));
    auto* center_layout = center->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical));
    body_layout->SetFlexForView(center, 1);
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
    if (!history_page_)
      files_ = center->AddChildView(std::make_unique<FolderScrollView>());
    else
#endif
      files_ = center->AddChildView(std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled));
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
    if (!history_page_) {
      files_->SetVerticalScrollBar(std::make_unique<FolderScrollBar>(views::ScrollBar::Orientation::kVertical, files_));
      files_->SetHorizontalScrollBar(std::make_unique<FolderScrollBar>(views::ScrollBar::Orientation::kHorizontal, files_));
    } else {
      auto scrollbar = std::make_unique<libcr::OverlayScrollBar>(views::ScrollBar::Orientation::kVertical);
      scrollbar->SetRoundedThumb(true);
      files_->SetVerticalScrollBar(std::move(scrollbar));
    }
#endif
    file_surface_ = files_;
    files_->SetPaintToLayer();
    files_->layer()->SetFillsBoundsOpaquely(false);
    files_->SetBackgroundColor((history_page_ && tag_id_ < 0) ? ui::kColorSysBase : kFolderSurfaceColor);
    center_layout->SetFlexForView(files_, 1);
    // A plain View in ScrollView otherwise retains its initial zero size.
    files_->SetUseContentsPreferredSize(true);
    files_->SetAccessibleName(
        Text(tag_id_ >= 0 ? IDS_CRFILES_TAGS : history_page_ ? IDS_CRFILES_HISTORY : IDS_CRFILES_FILES));
    files_->SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
    if (root_) files_->set_context_menu_controller(this);
    if (!history_page_) {
      auto* top_fade = files_->SetCustomOverflowIndicator(views::OverflowIndicatorAlignment::kTop,
          std::make_unique<FolderEdgeFade>(files_, true), kFolderEdgeFadeHeight, false);
      auto* bottom_fade = files_->SetCustomOverflowIndicator(views::OverflowIndicatorAlignment::kBottom,
          std::make_unique<FolderEdgeFade>(files_, false), kFolderEdgeFadeHeight, false);
      files_->RegisterPostLayoutCallback(base::BindRepeating(
          [](FilesView* self, views::View* top, views::View* bottom, views::ScrollView* scroll) {
            self->RestoreScroll(scroll);
            // Content and viewport size changes can change the remaining distance
            // without a scroll event. The indicators are owned by this ScrollView.
            top->SchedulePaint();
            bottom->SchedulePaint();
          }, base::Unretained(this), base::Unretained(top_fade), base::Unretained(bottom_fade)));
    } else {
      files_->RegisterPostLayoutCallback(base::BindRepeating(&FilesView::RestoreScroll, base::Unretained(this)));
    }
    file_scroll_subscription_ = files_->AddContentsScrolledCallback(base::BindRepeating(&FilesView::FileContentsScrolled, weak_.GetWeakPtr()));
    files_->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    auto* footer = center->AddChildView(std::make_unique<views::View>());
    footer_ = footer;
    footer->SetProperty(views::kViewIgnoredByLayoutKey, true);
    footer->SetPaintToLayer(); footer->layer()->SetFillsBoundsOpaquely(false);
    footer->layer()->SetOpacity(0.f); footer->SetCanProcessEventsWithinSubtree(false);
    footer->SetBackground(std::make_unique<FooterBackground>((history_page_ && tag_id_ < 0) ? ui::kColorSysBase : kFolderSurfaceColor));
    footer->SetBorder(std::make_unique<FooterGlowBorder>(false));
    auto* footer_layout = footer->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(4, 8), 4));
    // Layer scrolling sizes the plain contents view from its preferred size.
    auto* actions_scroll = footer->AddChildView(std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled));
    normal_footer_actions_ = actions_scroll;
    actions_scroll->SetUseContentsPreferredSize(true);
    actions_scroll->SetBackgroundColor(std::nullopt);
    actions_scroll->SetVerticalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    actions_scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kHiddenButEnabled);
    actions_scroll->SetPreferredSize(gfx::Size(280, 26));
    footer_layout->SetFlexForView(actions_scroll, 1);
    auto* actions = actions_scroll->SetContents(std::make_unique<views::View>());
    actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
    auto action = [&](const gfx::VectorIcon& icon, int label, base::RepeatingClosure callback, views::View* container = nullptr) {
      auto* button = AddIconButton(container ? container : actions, icon, label, std::move(callback));
      button->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 5)));
      button->SetPreferredSize(gfx::Size(30, 26));
      button->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      auto* ink = views::InkDrop::Get(button->ink_drop_view());
      ink->SetHighlightOpacity(0.18f); ink->SetVisibleOpacity(0.30f);
      for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
        button->SetImageModel(state, ui::ImageModel::FromVectorIcon(icon, ui::kColorSysPrimary, 20));
      return button;
    };
    if (root_) {
      share_button_ = action(vector_icons::kScreenShareIcon, IDS_CRFILES_SHARE_PANE, base::BindRepeating(&FilesView::TogglePaneShare, weak_.GetWeakPtr()));
      auto* separator = actions->AddChildView(std::make_unique<views::Separator>());
      separator->SetOrientation(views::Separator::Orientation::kVertical);
    }
    new_folder_button_ = action(vector_icons::kFolderOpenIcon, IDS_CRFILES_NEW_FOLDER,
        base::BindRepeating(&FilesView::ShowNewFolder, base::Unretained(this)));
    trash_footer_actions_ = footer->AddChildView(std::make_unique<views::View>());
    footer_layout->SetFlexForView(trash_footer_actions_, 1);
    auto* trash_actions_layout = trash_footer_actions_->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 0));
    restore_action_ = AddButton(trash_footer_actions_, Text(IDS_CRFILES_RESTORE),
        base::BindRepeating(&FilesView::ChangeTrash, base::Unretained(this), true));
    restore_action_->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kUndoIcon, ui::kColorSysOnSurface, 20));
    views::HighlightPathGenerator::Install(restore_action_, std::make_unique<SplitButtonHighlightPath>(true));
    empty_trash_action_ = AddButton(trash_footer_actions_, Text(IDS_CRFILES_EMPTY_TRASH),
        base::BindRepeating(&FilesView::ConfirmEmptyTrash, base::Unretained(this)));
    empty_trash_action_->SetEnabledTextColors(ui::kColorSysError);
    empty_trash_action_->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(kDeleteIcon, ui::kColorSysError, 20));
    views::InkDrop::Get(empty_trash_action_->ink_drop_view())->SetBaseColor(ui::kColorSysError);
    views::HighlightPathGenerator::Install(empty_trash_action_, std::make_unique<SplitButtonHighlightPath>(false));
    for (auto* button : {restore_action_.get(), empty_trash_action_.get()}) {
      trash_actions_layout->SetFlexForView(button, 1);
      button->SetAnimationDuration(base::Milliseconds(120));
      auto* ink = views::InkDrop::Get(button->ink_drop_view());
      ink->SetHighlightOpacity(0.24f);
      ink->SetVisibleOpacity(0.38f);
    }
    for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED}) {
      restore_action_->SetTextColor(state, ui::kColorSysPrimary);
      restore_action_->SetImageModel(state,
          ui::ImageModel::FromVectorIcon(vector_icons::kUndoIcon, ui::kColorSysPrimary, 20));
    }
    trash_footer_actions_->SetVisible(false);
    search_action_ = action(vector_icons::kSearchIcon, IDS_CRFILES_SEARCH,
        Deferred(base::BindRepeating(&FilesView::ShowSearchTab, weak_.GetWeakPtr())));
    more_button_ = action(vector_icons::kExpandMoreOldIcon, IDS_CRFILES_MORE,
        base::BindRepeating(&FilesView::ShowMoreMenu, base::Unretained(this)));
    footer_status_ = footer->AddChildView(std::make_unique<views::View>());
    footer_layout->SetFlexForView(footer_status_, 2);
    auto* status_layout = footer_status_->SetLayoutManager(std::make_unique<views::FlexLayout>());
    status_layout->SetOrientation(views::LayoutOrientation::kHorizontal)
        .SetMainAxisAlignment(views::LayoutAlignment::kEnd)
        .SetCrossAxisAlignment(views::LayoutAlignment::kCenter);
    const views::FlexSpecification text_flex(views::MinimumFlexSizeRule::kScaleToZero,
                                            views::MaximumFlexSizeRule::kPreferred);
    footer_folder_name_ = footer_status_->AddChildView(std::make_unique<views::Label>());
    footer_folder_name_->SetEnabledColor(ui::kColorSysPrimary);
    footer_folder_name_->SetHorizontalAlignment(gfx::ALIGN_RIGHT);
    footer_folder_name_->SetElideBehavior(gfx::ELIDE_TAIL);
    footer_folder_name_->SetProperty(views::kFlexBehaviorKey, text_flex);
    status_ = footer_status_->AddChildView(std::make_unique<views::Label>());
    status_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    status_->SetElideBehavior(gfx::ELIDE_TAIL);
    status_->SetProperty(views::kFlexBehaviorKey, text_flex);
    status_->SetProperty(views::kMarginsKey, gfx::Insets::TLBR(0, 4, 0, 0));
    if (!root_) for (auto accelerator : {ui::Accelerator(ui::VKEY_T, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_Q, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_C, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_X, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_V, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_W, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_L, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_OEM_PLUS, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_OEM_PLUS, kPrimaryModifier | ui::EF_SHIFT_DOWN),
             ui::Accelerator(ui::VKEY_OEM_MINUS, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_ADD, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_SUBTRACT, kPrimaryModifier),
             ui::Accelerator(ui::VKEY_TAB, ui::EF_CONTROL_DOWN),
             ui::Accelerator(ui::VKEY_TAB, ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN),
             ui::Accelerator(ui::VKEY_LEFT, ui::EF_ALT_DOWN),
             ui::Accelerator(ui::VKEY_RIGHT, ui::EF_ALT_DOWN),
             ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE),
             ui::Accelerator(ui::VKEY_F5, ui::EF_NONE),
             ui::Accelerator(ui::VKEY_F11, ui::EF_NONE)}) AddAccelerator(accelerator);
#if BUILDFLAG(IS_MAC)
    if (!root_) AddAccelerator(ui::Accelerator(ui::VKEY_V, ui::EF_COMMAND_DOWN | ui::EF_ALT_DOWN));
#endif
    if (!root_) {
    fullscreen_exit_ = AddButton(this, std::u16string(), base::BindRepeating(&FilesView::ExitFullscreen, base::Unretained(this)));
    fullscreen_exit_->SetProperty(views::kViewIgnoredByLayoutKey, true);
    fullscreen_exit_->SetAccessibleName(Text(IDS_CRFILES_EXIT_FULLSCREEN));
    fullscreen_exit_->SetTooltipText(Text(IDS_CRFILES_EXIT_FULLSCREEN));
    fullscreen_exit_->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    fullscreen_exit_->SetImageCentered(true);
    fullscreen_exit_->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(kCloseIcon, ui::kColorSysError, 30));
    fullscreen_exit_->SetBorder(views::CreateEmptyBorder(0));
    fullscreen_exit_->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainer, 22));
    views::InstallCircleHighlightPathGenerator(fullscreen_exit_);
    fullscreen_exit_->SetPaintToLayer();
    fullscreen_exit_->layer()->SetFillsBoundsOpaquely(false);
    fullscreen_exit_->SetVisible(false);
    }
    if (root_) {
      activation_ = std::make_unique<PaneActivation>(this, base::BindRepeating(&FilesView::ActivateFromInput, base::Unretained(this)));
      activation_->SetFileKeys(base::BindRepeating(&FilesView::FileKeyPressed, base::Unretained(this)));
      if (tag_id_ >= 0) {
        tabs_.push_back(Tab{.id = next_id_++});
        Current().sidebar_selection = IDS_CRFILES_TAGS;
        tag_history_.push_back({tag_id_, tag_name_, {}, {}});
        LoadTagFiles();
        return;
      }
      if (network_page_) {
        tabs_.push_back(Tab{.id = next_id_++});
        Current().sidebar_selection = IDS_CRFILES_NETWORK;
        files_->SetVisible(false); footer_->SetVisible(false);
        network_view_ = center->AddChildViewAt(CreateNetworkPage(
            base::BindRepeating([](base::WeakPtr<FilesView> pane, base::FilePath path) {
              if (pane) pane->Open(path, false);
            }, weak_.GetWeakPtr()),
            base::BindRepeating([](base::WeakPtr<FilesView> pane, std::unique_ptr<ConnectedSmbSession> session) {
              if (!pane) return;
              auto* shell = pane->root_.get();
              shell->AddPaneTab({}, false, false, -1, {}, {}, true);
              AdoptNetworkSession(shell->ActivePane()->network_view_, std::move(session));
            }, weak_.GetWeakPtr()),
            base::BindRepeating([](base::WeakPtr<FilesView> pane) {
              if (pane && pane->root_) pane->root_->PaneNavigated(pane.get());
            }, weak_.GetWeakPtr()),
            base::BindRepeating([](base::WeakPtr<FilesView> pane) -> views::View* {
              return pane && pane->root_ ? pane->root_->network_address_anchor_.view() : nullptr;
            }, weak_.GetWeakPtr()),
            base::BindRepeating([](base::WeakPtr<FilesView> pane, SmbCredentials credentials, int direction) {
              if (!pane) return;
              if (direction < 0) pane->root_->OpenNetworkDevice(std::move(credentials));
              else pane->root_->SplitPane(pane.get(), direction, {}, std::move(credentials));
            }, weak_.GetWeakPtr()),
            base::BindRepeating([](base::WeakPtr<FilesView> pane, int sort) {
              if (!pane) return;
              pane->Current().options.sort = static_cast<FileSort>(sort);
              pane->root_->ScheduleWorkspaceSave();
            }, weak_.GetWeakPtr())), 0);
        center_layout->SetFlexForView(network_view_, 1);
        ConfigureNetworkFooter();
        return;
      }
      if (search_page_) {
        tabs_.push_back(Tab{.id = next_id_++}); Current().path = initial_path;
        files_->SetVisible(false); footer_->SetVisible(false);
        search_view_ = center->AddChildViewAt(CreateSearchPage(initial_path,
            base::BindRepeating([](base::WeakPtr<FilesView> pane, base::FilePath path, bool directory) {
              if (!pane) return;
#if BUILDFLAG(IS_MAC)
              pane->Open(path, directory);
#else
              if (directory) pane->root_->AddPaneTab(path);
              else pane->Open(path, false);
#endif
            }, weak_.GetWeakPtr()),
            base::BindRepeating([](base::WeakPtr<FilesView> pane, base::FilePath path) {
              if (!pane) return;
              auto* owner = pane->root_.get();
              owner->AddPaneTab(path.DirName());
              auto* destination = owner->ActivePane();
              destination->pending_search_selection_ = path;
              if (!destination->Current().loading) destination->RenderFiles();
            }, weak_.GetWeakPtr()),
            base::BindRepeating([](base::WeakPtr<FilesView> pane) {
              if (pane && pane->root_) pane->root_->UpdateTabStates();
            }, weak_.GetWeakPtr()), search_scope_), 0);
        center_layout->SetFlexForView(search_view_, 1);
        return;
      }
      if (history_page_) {
        auto* header = center->AddChildViewAt(std::make_unique<HistoryHeader>(this, base::BindRepeating(&FilesView::ShowHistoryFilter, weak_.GetWeakPtr()),
            base::BindRepeating(&FilesView::ClearHistoryFilter, weak_.GetWeakPtr())), 0);
        history_header_ = header;
        history_search_ = header->search();
        history_filter_button_ = header->filter();
        tabs_.push_back(Tab{.id = next_id_++});
        LoadHistory();
        return;
      }
      if (deferred_start) {
        tabs_.push_back(Tab{.id = next_id_++}); Current().path = initial_path;
        pending_navigation_ = PendingNavigation{initial_path, -1, false};
      } else NewTab(initial_path.empty() ? Home() : initial_path);
      return;
    }
    auto args = base::CommandLine::ForCurrentProcess()->GetArgs();
    if (args.empty()) { if (!RestoreWorkspace()) NewTab(Home()); }
    else {
      for (const auto& arg : args) {
        auto path = ParseNavigationPath(base::FilePath(arg).AsUTF16Unsafe());
        if (path) NewTab(*path);
      }
      if (tabs_.empty()) NewTab(Home());
    }
    SyncSharedTargets();
    timer_.Start(FROM_HERE, base::Seconds(1), this, &FilesView::UpdateNearby);
    UpdateNearby();
  }
  OpenTagCallback TagOpener() {
    auto* shell = root_ ? root_.get() : this;
    return base::BindRepeating([](base::WeakPtr<FilesView> view, int64_t id, std::string name) {
      if (view) view->AddPaneTab({}, false, false, id, std::move(name));
    }, shell->weak_.GetWeakPtr());
  }
  void ShowTagsTab() {
    auto* shell = root_ ? root_.get() : this;
    auto* pane = shell->ActivePane();
    if (!pane) return;
    shell->EndAddressEditing();
    if (pane->history_page_) {
      // History/search own a different content tree. Replace only this leaf,
      // preserving the current tab and any surrounding split layout.
      auto replacement = std::make_unique<FilesView>(shell, base::FilePath(), false, false, false, 0);
      replacement->pane_id_ = pane->pane_id_;
      auto* added = replacement.get();
      auto* parent = pane->parent();
      pane->PrepareToClose();
      if (auto* split = views::AsViewClass<PaneSplit>(parent)) split->Replace(pane, added);
      shell->Current().active_pane = added;
      auto removed = parent->RemoveChildViewT(pane);
      parent->AddChildView(std::move(replacement));
      removed.reset();
      parent->InvalidateLayout();
    } else {
      if (pane->tag_history_.empty()) {
        const auto& tab = pane->Current();
        for (int i = 0; i <= tab.history_index; ++i)
          pane->tag_history_.push_back({-1, {}, {}, tab.history[i]});
        if (pane->tag_history_.empty()) pane->tag_history_.push_back({-1, {}, {}, tab.path});
        pane->tag_history_index_ = static_cast<int>(pane->tag_history_.size()) - 1;
      }
      pane->NavigateTag({0, {}, {}, {}});
    }
    shell->UpdatePaneControls(); shell->SyncNavigation(); shell->ScheduleWorkspaceSave();
    shell->ActivePane()->DefaultFocusView()->RequestFocus();
  }
  bool IsTagListing() const { return tag_id_ >= 0 && tag_directory_root_.empty(); }
  struct TagLocation {
    int64_t id;
    std::string name;
    base::FilePath root;
    base::FilePath path;
  };
  void NavigateTag(TagLocation location, int history_index = -1) {
    if (tag_id_ == location.id && tag_directory_root_ == location.root && Current().path == location.path) { Refresh(); return; }
    SaveScroll(); CancelDirectoryWork(); CancelFileVisuals(); ClearMaterializedRows();
    ResetDirectoryAnimation(); CloseNewFolder(); CloseViewOptions();
    ++context_generation_;
    if (context_runner_ && context_runner_->IsRunning()) context_runner_->Cancel();
    ++Current().request; ++access_generation_; tag_loading_ = false;
    pending_navigation_.reset();
    tag_id_ = location.id; tag_name_ = location.name; tag_directory_root_ = location.root;
    auto& tab = Current();
    tag_sizes_.clear(); tag_sizes_pending_ = false;
    tab.entries.clear(); tab.sorted_entries.clear(); tab.selected.clear(); tab.removed.clear();
    tab.visuals.clear(); tab.failed_thumbnails.clear(); ++tab.metadata_generation;
    tab.path = {}; tab.scroll = 0; pending_scroll_ = 0; tab.error = false; tab.InvalidateOrder();
    tab.sidebar_selection = location.id >= 0 ? IDS_CRFILES_TAGS : 0;
    files_->SetAccessibleName(Text(location.id >= 0 ? IDS_CRFILES_TAGS : IDS_CRFILES_FILES));
    if (history_index >= 0) tag_history_index_ = history_index;
    else {
      tag_history_.resize(tag_history_index_ + 1);
      tag_history_.push_back(location); tag_history_index_ = tag_history_.size() - 1;
    }
    if (location.id >= 0 && location.root.empty()) LoadTagFiles();
    else Navigate(location.path, -1, true);
    root_->PaneNavigated(this);
  }
  void NavigateTagRoot() {
    auto* pane = ActivePane();
    if (pane->tag_id_ >= 0) pane->NavigateTag({0, {}, {}, {}});
    else ShowTagsTab();
  }
  struct TagFilesResult {
    bool ok = false;
    std::string name;
    std::vector<Entry> entries;
  };
  void LoadTagFiles() {
    if (!IsTagListing() || tag_loading_) return;
    tag_loading_ = true; tag_version_ = FileTagsVersion();
    CancelDirectoryWork();
    // Metadata refresh must not invalidate thumbnail jobs for retained rows.
    const uint64_t request = Current().request;
    Current().loading = true;
    const auto token = read_cancel_;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](int64_t id, CancelFlag token) {
          TagFilesResult result;
          const auto snapshot = ReadFileTags();
          if (!snapshot.ok) return result;
          if (id == 0) {
            result.ok = true;
            for (const auto& tag : snapshot.tags) {
              Entry entry{base::FilePath(FILE_PATH_LITERAL("tags")).AppendASCII(base::NumberToString(tag.id)), true, -1, {}, "inode/directory"};
              entry.tag = tag.id; entry.tag_name = tag.name; entry.tag_count = tag.paths.size();
              entry.name_key = base::i18n::ToLower(base::UTF8ToUTF16(tag.name));
              result.entries.push_back(std::move(entry));
            }
            return result;
          }
          const auto tag = std::ranges::find(snapshot.tags, id, &FileTag::id);
          if (tag == snapshot.tags.end()) return result;
          result.ok = true; result.name = tag->name;
          for (const auto& path : tag->paths) {
            if (Stopped(token)) return result;
            base::File::Info info;
            if (!base::GetFileInfo(path, &info)) continue;
            std::string mime = "inode/directory";
            if (!info.is_directory && !net::GetMimeTypeFromFile(path, &mime)) mime = "application/octet-stream";
            bool hidden = path.BaseName().AsUTF8Unsafe().starts_with(".");
#if BUILDFLAG(IS_WIN)
            const DWORD attributes = GetFileAttributesW(path.value().c_str());
            hidden |= attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN);
#endif
            result.entries.push_back({path, info.is_directory, info.size, info.last_modified, mime, hidden});
            ReadExtraMetadata(result.entries.back());
#if BUILDFLAG(IS_MAC)
            result.entries.back().package = info.is_directory && IsFilePackageMac(path);
#endif
          }
          return result;
        }, tag_id_, token),
        base::BindOnce([](base::WeakPtr<FilesView> pane, CancelFlag token, uint64_t request, TagFilesResult result) {
          if (!pane || Stopped(token) || pane->Current().request != request) return;
          DirectoryResult directory;
          directory.ok = result.ok; directory.entries = std::move(result.entries);
          pane->CompareTagFiles(token, request, std::move(result.name), std::move(directory));
        }, weak_.GetWeakPtr(), token, request));
    UpdateStatus();
  }
  void CompareTagFiles(CancelFlag token, uint64_t request, std::string name, DirectoryResult result) {
    const auto& tab = Current();
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](DirectoryEntries previous, std::set<base::FilePath> removed,
                         uint64_t generation, FileSort sort, bool hidden,
                         DirectoryResult data, CancelFlag canceled) {
          data.sort = sort; data.hidden = hidden;
          data.order = SortDirectory(SortRecords(data.entries, canceled), sort, hidden, false, canceled);
          return CompareDirectory(std::move(previous), std::move(removed), generation, std::move(data), canceled);
        }, tab.entries, tab.removed, tab.metadata_generation, tab.options.sort, tab.options.show_hidden, std::move(result), token),
        base::BindOnce([](base::WeakPtr<FilesView> pane, CancelFlag canceled, uint64_t serial,
                         std::string title, DirectoryResult data) {
          if (!pane || Stopped(canceled) || pane->Current().request != serial) return;
          auto& current = pane->Current();
          if (data.base_generation != current.metadata_generation || data.sort != current.options.sort ||
              data.hidden != current.options.show_hidden) {
            pane->CompareTagFiles(canceled, serial, std::move(title), std::move(data)); return;
          }
          pane->tag_loading_ = false;
          const bool title_changed = data.ok && pane->tag_name_ != title;
          if (data.ok) pane->tag_name_ = std::move(title);
          current.loading = false;
          pane->CancelOrdering();
          if (pane->restored_scroll_) {
            current.scroll = *pane->restored_scroll_; pane->pending_scroll_ = *pane->restored_scroll_;
            pane->restored_scroll_.reset();
          }
          // Reuse the ordinary folder's delta application: unchanged rows,
          // selection, thumbnails and the visible scroll anchor survive.
          if (views::AsViewClass<FileLayout>(pane->files_->contents()) &&
              current.sorted_mode == data.sort && current.sorted_hidden == data.hidden) {
            pane->ApplyLiveDirectory(std::move(data));
            pane->RequestFileVisuals();
          } else {
            pane->SaveScroll(); pane->CancelOrdering(); pane->CancelFileVisuals(); pane->ClearMaterializedRows(true);
            current.error = !data.ok;
            std::erase_if(current.visuals, [&](const auto& item) { return !data.delta->unchanged.contains(item.first); });
            std::erase_if(current.failed_thumbnails, [&](const auto& path) { return !data.delta->unchanged.contains(path); });
            std::erase_if(current.selected, [&](const auto& path) { return !data.delta->positions.contains(path); });
            current.entries = std::move(data.entries); current.removed.clear(); ++current.metadata_generation;
            current.sorted_entries = data.order.values(); current.sorted_mode = data.sort; current.sorted_hidden = data.hidden;
            pane->RenderFiles(); pane->UpdateStatus();
          }
          if (title_changed) pane->root_->PaneNavigated(pane.get());
          if (pane->tag_id_ == 0) pane->LoadTagSizes(canceled, serial);
        }, weak_.GetWeakPtr(), token, request, std::move(name)));
  }
  void LoadTagSizes(CancelFlag token, uint64_t request) {
    tag_sizes_pending_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::BindOnce([](CancelFlag canceled) {
          std::map<int64_t, TagSize> sizes;
          for (auto& tag : ReadFileTags().tags) {
            if (Stopped(canceled)) break;
            sizes.emplace(tag.id, CalculateTagSize(std::move(tag.paths), canceled));
          }
          return sizes;
        }, token), base::BindOnce([](base::WeakPtr<FilesView> pane, CancelFlag canceled,
                                     uint64_t serial, std::map<int64_t, TagSize> sizes) {
          if (!pane || Stopped(canceled) || pane->Current().request != serial) return;
          pane->tag_sizes_pending_ = false;
          pane->SaveScroll(); pane->CancelOrdering(); pane->ClearMaterializedRows();
          auto entries = pane->Current().entries.values();
          for (auto& entry : entries) {
            auto size = sizes.find(entry.tag);
            if (size != sizes.end()) entry.size = static_cast<int64_t>(std::min<uint64_t>(size->second.bytes, std::numeric_limits<int64_t>::max()));
          }
          pane->tag_sizes_ = std::move(sizes);
          pane->Current().entries = std::move(entries); ++pane->Current().metadata_generation;
          pane->Current().InvalidateOrder(); pane->RenderFiles();
        }, weak_.GetWeakPtr(), token, request));
  }
  void ShowNetworkTab() {
    if (!IsSmbEnabledForSession()) return;
    if (root_) { root_->ShowNetworkTab(); return; }
    EndAddressEditing();
    for (const auto& tab : tabs_) if (tab.active_pane && tab.active_pane->network_view_ &&
        NetworkPageUrl(tab.active_pane->network_view_).empty()) { SelectPaneTab(tab.id); return; }
    AddPaneTab({}, false, false, -1, {}, {}, true);
  }
  void OpenNetworkDevice(SmbCredentials credentials) {
    AddPaneTab({}, false, false, -1, {}, {}, true);
    ConnectNetworkPage(ActivePane()->network_view_, std::move(credentials));
  }
  bool NavigateNetworkAddress(std::string address) {
    if (!IsSmbEnabledForSession()) return false;
    auto* pane = ActivePane();
    if (pane->network_view_ && NavigateNetworkPage(pane->network_view_, address)) return true;
    if (address.starts_with('/') && pane->network_view_) {
      const auto current = ParseSmbLocation(NetworkPageUrl(pane->network_view_));
      if (current) address = "smb://" + current->server + address;
    }
    const auto location = ParseSmbLocation(address);
    if (!location) return false;
    AddPaneTab({}, false, false, -1, {}, {}, true);
    PromptNetworkPage(ActivePane()->network_view_, location->Url());
    return true;
  }
  void ShowSearchTab() {
    auto* pane = ActivePane();
    if (!pane || pane->history_page_ || pane->Current().loading || pane->Current().error) return;
    auto* shell = root_ ? root_.get() : this;
    if (pane->tag_id_ > 0 && pane->IsTagListing()) {
      SearchScope scope;
      scope.multiple = true; scope.tag_id = pane->tag_id_;
      scope.title = Text(IDS_CRFILES_TAGS) + u" / " + base::UTF8ToUTF16(pane->tag_name_);
      shell->AddPaneTab({}, false, true, -1, {}, std::move(scope));
    } else if (pane->Current().path.IsAbsolute() && pane->Current().path != TrashRoot()) {
      shell->AddPaneTab(pane->Current().path, false, true);
    }
  }
  void RefreshReceivedSidebar() {
    if (root_ || !GetWidget() || GetWidget()->IsClosed()) return;
    RevealReceivedTarget(sharing_->GetSnapshot());
    CheckReceivedDirectory(true);
  }
  void ShowHistoryTab() {
    if (root_) { root_->ShowHistoryTab(); return; }
    for (const auto& tab : tabs_) if (tab.active_pane && tab.active_pane->history_page_ && !tab.active_pane->search_page_ && !tab.active_pane->network_page_ && tab.active_pane->tag_id_ < 0) { SelectPaneTab(tab.id); return; }
    AddPaneTab({}, true);
  }
  void LoadHistory() { RequestHistory(false); }
  void LoadMoreHistory() { RequestHistory(true); }
  void RequestHistory(bool append) {
    if (!history_page_ || search_page_ || network_page_ || tag_id_ >= 0 || history_loading_) return;
    auto* list = views::AsViewClass<HistoryList>(files_->contents());
    append = append && list && history_cache_valid_ &&
        history_loaded_query_ == history_query_ && history_loaded_filter_ == history_filter_ &&
        history_version_ == FileHistoryVersion();
    const auto cursor = append ? list->cursor() : FileHistoryCursor{};
    history_loading_ = true;
    if (list) list->SetLoading(true);
    const uint64_t generation = history_generation_;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&ReadFileHistory, 201, history_query_, history_filter_, cursor),
        base::BindOnce(&FilesView::HistoryLoaded, weak_.GetWeakPtr(),
                      history_query_, history_filter_, generation, append));
  }
  void HistoryLoaded(std::string query, std::string filter, uint64_t generation,
                     bool append, FileHistoryPage page) {
    history_loading_ = false;
    auto* list = views::AsViewClass<HistoryList>(files_->contents());
    if (list) list->SetLoading(false);
    if (generation != history_generation_ || query != history_query_ || filter != history_filter_ ||
        (append && page.ok && page.version != history_version_)) {
      LoadHistory(); return;
    }
    if (append && !page.ok) return;  // Preserve rows and allow retrying More.
    history_cache_valid_ = page.ok;
    history_version_ = page.version;
    history_loaded_query_ = query;
    history_loaded_filter_ = filter;
    const bool more = page.entries.size() > 200;
    if (more) page.entries.pop_back();
    if (append && list) {
      list->Append(std::move(page.entries), more);
      return;
    }
    if (!page.ok || page.entries.empty()) {
      auto contents = std::make_unique<views::View>();
      contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
      contents->AddChildView(std::make_unique<views::Label>(Text(!page.ok ? IDS_CRFILES_READ_ERROR :
          (query.empty() && filter.empty()) ? IDS_CRFILES_HISTORY_EMPTY : IDS_CRFILES_HISTORY_NO_MATCHES)));
      files_->SetContents(std::move(contents));
      return;
    }
    auto contents = std::make_unique<HistoryList>(files_, base::UTF8ToUTF16(query),
        Deferred(base::BindRepeating(&FilesView::LoadMoreHistory, weak_.GetWeakPtr())));
    contents->Append(std::move(page.entries), more);
    files_->SetContents(std::move(contents));
  }
  ~FilesView() override { network_share_cancel_->store(true); hover_cancel_->store(true); live_cancel_->store(true); if (destination_dialog_) destination_dialog_->ListenerDestroyed(); context_runner_.reset(); read_cancel_->store(true); order_cancel_->store(true); DirectoryReads().Prune(); visual_cancel_->store(true, std::memory_order_relaxed); outside_click_handler_.reset(); CloseNewFolder(); CloseViewOptions(); if (!root_) g_contents = nullptr; }
  void AddedToWidget() override {
    views::View::AddedToWidget();
    if (root_) {
#if BUILDFLAG(IS_MAC)
      if (IsCrFilesSandboxed() && !history_page_ && !search_page_)
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce(&FilesView::Navigate, weak_.GetWeakPtr(), Current().path, -1, true));
#endif
      return;
    }
    outside_click_handler_ = std::make_unique<AddressOutsideClickHandler>(
        GetWidget()->GetRootView(), address_,
        Deferred(base::BindRepeating(&FilesView::EndAddressEditing, weak_.GetWeakPtr())),
        base::BindRepeating(&FilesView::FullscreenPointer, weak_.GetWeakPtr()),
        base::BindRepeating(&FilesView::DismissEditingBubbles, weak_.GetWeakPtr()));
#if BUILDFLAG(IS_MAC)
    titlebar_click_monitor_ = ObserveTitlebarClicksMac(GetWidget(),
        Deferred(base::BindRepeating(&FilesView::EndAddressEditing, weak_.GetWeakPtr())));
#endif
    footer_pointer_timer_.Start(FROM_HERE, base::Milliseconds(50), this, &FilesView::PollFooterHover);
  }
  void RemovedFromWidget() override {
    footer_pointer_timer_.Stop();
    outside_click_handler_.reset();
    views::View::RemovedFromWidget();
  }
  bool HasNavigationChrome() const {
    return !history_page_ || network_page_ || tag_id_ >= 0;
  }
  void OnResize(int amount, bool done) override {
    auto* pane = ActivePane();
    if (!sidebar_viewport_ || !pane || !pane->HasNavigationChrome()) return;
    if (!sidebar_drag_start_) {
      sidebar_animation_timer_.Stop();
      sidebar_drag_start_ = sidebar_display_width_;
    }
    sidebar_display_width_ = SnapResizePosition(
        std::clamp(*sidebar_drag_start_ + amount, 0, 360), 240, sidebar_snapped_);
    UpdateSidebarPresentation();
    if (done) {
      const int target = sidebar_display_width_ < 110 ? 0 : std::clamp(sidebar_display_width_, 220, 360);
      sidebar_drag_start_.reset();
      sidebar_snapped_ = false;
      AnimateSidebarTo(target);
      if (target == 0) ActivePane()->DefaultFocusView()->RequestFocus();
    }
  }
  void UpdateSidebarPresentation() {
    if (!sidebar_viewport_) return;
    const auto* pane = ActivePane();
    const bool available = !pane || pane->HasNavigationChrome();
    sidebar_viewport_->SetPreferredSize(gfx::Size(sidebar_display_width_, 0));
    sidebar_viewport_->SetVisible(available && sidebar_display_width_ > 0);
    sidebar_resize_->SetVisible(available);
    sidebar_resize_->SetCollapsed(sidebar_display_width_ == 0);
    if (!root_) SidebarObservers().Notify(available && sidebar_visible_);
    InvalidateLayout();
  }
  void AnimateSidebarTo(int target) {
    sidebar_animation_timer_.Stop();
    sidebar_visible_ = target > 0;
    if (sidebar_visible_) sidebar_width_ = target;
    sidebar_animation_from_ = sidebar_display_width_;
    sidebar_animation_target_ = target;
    sidebar_animation_start_ = base::TimeTicks::Now();
    ScheduleWorkspaceSave();
    if (target == sidebar_display_width_) { UpdateSidebarPresentation(); return; }
    sidebar_animation_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &FilesView::StepSidebarAnimation);
  }
  void StepSidebarAnimation() {
    const double progress = std::clamp((base::TimeTicks::Now() - sidebar_animation_start_).InMillisecondsF() / 160.0, 0.0, 1.0);
    sidebar_display_width_ = static_cast<int>(std::lround(sidebar_animation_from_ +
        (sidebar_animation_target_ - sidebar_animation_from_) * gfx::Tween::CalculateValue(gfx::Tween::EASE_OUT, progress)));
    if (progress >= 1.0) { sidebar_display_width_ = sidebar_animation_target_; sidebar_animation_timer_.Stop(); }
    UpdateSidebarPresentation();
  }
  void RestoreSidebar() {
    auto* pane = ActivePane();
    if (!pane || !pane->HasNavigationChrome()) return;
    sidebar_drag_start_.reset();
    AnimateSidebarTo(sidebar_width_);
    ActivePane()->DefaultFocusView()->RequestFocus();
  }
  void LayoutPaneFooter() {
    if (root_ && footer_) {
      const auto area = footer_->parent()->GetContentsBounds();
      const bool trash = Current().path == TrashRoot();
      const auto preferred = footer_->GetPreferredSize({});
      const int footer_width = trash ?
          std::min(std::max(0, area.width() - 8), static_cast<int>(std::lround(preferred.width() * 1.12))) : std::max(0, area.width());
      const int footer_height = trash ? static_cast<int>(std::lround(preferred.height() * 1.08)) : preferred.height();
      if (auto* layout = views::AsViewClass<FileLayout>(files_->contents()))
        layout->SetBottomPadding(tag_id_ == 0 ? 0 : footer_height + 10);
      if (network_view_) SetNetworkPageBottomInset(network_view_, network_footer_visible_ ? footer_height + 10 : 0);
      footer_->SetBounds(area.x() + (area.width() - footer_width) / 2,
          std::max(area.y(), area.bottom() - footer_height -
              (Current().path == TrashRoot() ? 2 : 0)), footer_width, footer_height);
      if (trash_footer_animation_pending_ && Current().path == TrashRoot() && area.width() >= 280) {
        trash_footer_animation_pending_ = false;
        // Replay after the capsule has its final bounds, even when the previous
        // directory's full-width toolbar was already visible.
        footer_shown_ = false;
      }
      UpdateFooterVisibility();
      if (footer_->GetVisible()) footer_->DeprecatedLayoutImmediately();
      if (IsDrawn() && area.width() > 0) footer_restore_pending_ = false;
    }
  }
  void Layout(PassKey) override {
    LayoutSuperclass<views::View>(this);
    LayoutPaneFooter();
    if (drop_highlight_) drop_highlight_->SetBoundsRect(drop_highlight_bounds_);
    if (root_) return;
    fullscreen_exit_->SetBounds((width() - kFullscreenExitSize) / 2,
        kFullscreenExitTop, kFullscreenExitSize, kFullscreenExitSize);
    if (GetWidget() && !GetWidget()->IsFullscreen()) {
      fullscreen_hide_timer_.Stop();
      fullscreen_exit_->SetVisible(false);
      fullscreen_shown_ = false;
    }
  }
  views::View* DefaultFocusView() { return root_ ? (network_view_ ? network_view_.get() : search_view_ ? search_view_.get() : files_.get()) : ActivePane()->DefaultFocusView(); }
  gfx::Size GetMinimumSize() const override { return root_ ? gfx::Size(300, 140) : gfx::Size(900, 500); }
  void PrepareToClose() {
    network_share_cancel_->store(true);
    if (!root_) sidebar_buttons_.at(IDS_CRFILES_RECEIVED)->SetBreathing(false);
#if BUILDFLAG(IS_MAC)
    titlebar_click_monitor_.RunAndReset();
#endif
    visual_stopped_ = true;
    if (!root_) CloseHoverPreview(false);
    live_watches_.clear(); timer_.Stop();
    if (search_view_) StopSearchPage(search_view_);
    if (network_view_) StopNetworkPage(network_view_);
    sidebar_animation_timer_.Stop();
    CancelDirectoryWork();
    CancelFileVisuals();
    if (!root_) { workspace_timer_.Stop(); SaveWorkspace(); }
    if (!root_ && !tabs_.empty()) for (auto& tab : tabs_)
      VisitPanes(tab.pane_host, base::BindRepeating([](FilesView* pane) { pane->PrepareToClose(); }));
    outside_click_handler_.reset();
    path_animation_timer_.Stop();
    footer_hide_timer_.Stop();
    footer_pointer_timer_.Stop();
    if (sharing_folders_dialog_.view() && sharing_folders_dialog_.view()->GetWidget()) sharing_folders_dialog_.view()->GetWidget()->Close();
    if (trusted_dialog_.view() && trusted_dialog_.view()->GetWidget()) trusted_dialog_.view()->GetWidget()->Close();
    if (sharing_dialog_.view() && sharing_dialog_.view()->GetWidget()) sharing_dialog_.view()->GetWidget()->Close();
    context_runner_.reset();
    if (destination_dialog_) { destination_dialog_->ListenerDestroyed(); destination_dialog_.reset(); }
    history_filter_runner_.reset();
    more_menu_runner_.reset();
    fullscreen_hide_timer_.Stop();
    CloseNewFolder();
    CloseViewOptions();
    history_search_timer_.Stop();
    session_toast_.reset();
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    if (quick_sharing_dialog_.view() && quick_sharing_dialog_.view()->GetWidget()) quick_sharing_dialog_.view()->GetWidget()->Close();
    quick_session_toast_.reset();
    if (quick_sharing_) quick_sharing_->CancelSession(quick_sharing_->GetSnapshot().session);
    quick_sharing_.reset();
#endif
    timer_.Stop(); weak_.InvalidateWeakPtrs();
    active_tab_ = nullptr; tabs_view_ = nullptr; if (sharing_) sharing_->Disable();
  }
  std::unique_ptr<views::View> CreateTabs() {
    rendered_tab_id_ = -1; rendered_tab_titles_.clear();
    tab_reveal_pending_ = true; tab_viewport_width_ = -1;
    auto container = std::make_unique<TabBar>(base::BindRepeating(
        &FilesView::TabsLaidOut, weak_.GetWeakPtr()));
    auto scroll = std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled);
    scroll->SetUseContentsPreferredSize(true);
    scroll->SetBackgroundColor(std::nullopt);
    scroll->SetVerticalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kHiddenButEnabled);
    tabs_view_ = scroll->SetContents(std::make_unique<TabStrip>(
        base::BindRepeating(&FilesView::ReorderTabs, weak_.GetWeakPtr())));
    container->AddChildView(std::move(scroll));
    auto* add = container->AddChildView(std::make_unique<NewTabButton>(
        Deferred(base::BindRepeating(&FilesView::NewCurrentTab, weak_.GetWeakPtr()))));
    add->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    add->SetRequestFocusOnPress(false);
    add->SetTooltipText(Text(IDS_CRFILES_NEW_TAB));
    add->SetAccessibleName(Text(IDS_CRFILES_NEW_TAB));
    add->SetPreferredSize(gfx::Size(kNewTabButtonSize, kNewTabButtonSize));
    add->SetBorder(views::CreateEmptyBorder(0));
    add->SetBackground(nullptr);
    add->SetHorizontalAlignment(gfx::ALIGN_CENTER);
    add->SetImageCentered(true);
    add->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kAdd2Icon, ui::kColorSysOnSurface, 14));
    add->SetInstallFocusRingOnFocus(false);
    views::InstallCircleHighlightPathGenerator(add);
    // Paint the hover directly on the titlebar, without a separate ink layer.
    add->SetHasInkDropActionOnClick(false);
    views::InkDrop::Get(add)->SetMode(views::InkDropHost::InkDropMode::OFF);
    for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
      add->SetImageModel(state, ui::ImageModel::FromVectorIcon(vector_icons::kAdd2Icon, ui::kColorSysPrimary, 14));
    RenderTabs();
    return container;
  }
  bool QuickShareBusy() const {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    return quick_sharing_ && quick_sharing_->GetSnapshot().busy;
#else
    return false;
#endif
  }
  bool SharingBusy() const { return sharing_ && sharing_->GetSnapshot().busy; }
  void ApplySharing() {
    auto prefs = GetNearbySharingPreferences();
    sharing_->Disable();
    if (prefs.discovery_enabled) sharing_->Enable(prefs.name,
        prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() :
            base::FilePath::FromUTF8Unsafe(prefs.directory), prefs.avatar);
    SyncSharedTargets();
    UpdateNearby();
    RefreshReceivedSidebar();
  }
  void ContentsChanged(views::Textfield* sender, const std::u16string& contents) override {
    if (sender == address_) {
      if ((!tabs_.empty() && Current().active_pane && ActivePane()->network_page_) ||
          base::StartsWith(contents, u"smb://", base::CompareCase::INSENSITIVE_ASCII)) address_completion_->Cancel();
      else address_completion_->Changed();
      return;
    }
    if (sender != history_search_) return;
    history_query_ = base::UTF16ToUTF8(contents);
    ++history_generation_;
    history_search_timer_.Start(FROM_HERE, base::Milliseconds(200), this, &FilesView::LoadHistory);
  }
  bool HandleKeyEvent(views::Textfield* sender, const ui::KeyEvent& event) override {
    if (sender == address_ && address_completion_->Key(event)) return true;
    if (sender == history_search_) {
      if (event.type() == ui::EventType::kKeyPressed && event.key_code() == ui::VKEY_RETURN) {
        history_search_timer_.Stop(); LoadHistory(); return true;
      }
      return false;
    }
    if (event.type() != ui::EventType::kKeyPressed || sender->IsIMEComposing()) return false;
    if (event.key_code() == ui::VKEY_RETURN) {
      const auto address = base::UTF16ToUTF8(address_->GetText());
      if (base::StartsWith(address, "smb://", base::CompareCase::INSENSITIVE_ASCII) ||
          (ActivePane()->network_view_ && address.starts_with('/'))) {
        const bool valid = NavigateNetworkAddress(address);
        address_->SetInvalid(!valid);
        if (valid) EndAddressEditing();
        return true;
      }
      auto path = ParseNavigationPath(address_->GetText());
      if (!path) { address_->SetInvalid(true); return true; }
      address_->SetInvalid(false);
      Navigate(*path);
      EndAddressEditing();
      return true;
    }
    if (event.key_code() == ui::VKEY_ESCAPE) {
      EndAddressEditing(); return true;
    }
    return false;
  }
  bool AcceleratorPressed(const ui::Accelerator& a) override {
    if (a.key_code() == ui::VKEY_C || a.key_code() == ui::VKEY_X || a.key_code() == ui::VKEY_V) {
      auto* pane = ActivePane();
      auto* focused = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
      if (pane && pane->network_view_ && focused && pane->network_view_->Contains(focused)) {
        for (auto* view = focused; view; view = view->parent())
          if (views::AsViewClass<views::Textfield>(view)) return false;
        if (a.key_code() == ui::VKEY_C) CopyNetworkPageSelection(pane->network_view_);
        else if (a.key_code() == ui::VKEY_V) PasteNetworkPage(pane->network_view_);
        return a.key_code() != ui::VKEY_X;
      }
      if (!pane || pane->history_page_ || !focused || !pane->files_->Contains(focused)) return false;
    }
    switch (a.key_code()) {
      case ui::VKEY_OEM_PLUS: case ui::VKEY_ADD: return AdjustIconSize(1);
      case ui::VKEY_OEM_MINUS: case ui::VKEY_SUBTRACT: return AdjustIconSize(-1);
      case ui::VKEY_Q: QuitCrFiles(); break;
      case ui::VKEY_C: ActivePane()->CopySelection(false); break;
      case ui::VKEY_X: ActivePane()->CopySelection(true); break;
      case ui::VKEY_V: ActivePane()->PasteSelection(a.IsAltDown()); break;
      case ui::VKEY_T: NewTab(ActivePane()->Current().path); break;
      case ui::VKEY_W:
        Deferred(base::BindRepeating(&FilesView::CloseActivePaneOrTab,
                                     weak_.GetWeakPtr())).Run();
        break;
      case ui::VKEY_L: EditAddress(); break;
      case ui::VKEY_TAB: SelectTab(tabs_[(active_ + tabs_.size() + (a.IsShiftDown() ? -1 : 1)) % tabs_.size()].id); break;
      case ui::VKEY_LEFT: History(-1); break;
      case ui::VKEY_RIGHT: History(1); break;
      case ui::VKEY_ESCAPE:
        if (!GetWidget() || !GetWidget()->IsFullscreen()) return false;
        ExitFullscreen();
        break;
      case ui::VKEY_F5: Refresh(); break;
      case ui::VKEY_F11: GetWidget()->SetFullscreen(!GetWidget()->IsFullscreen()); break;
      default: return false;
    }
    return true;
  }
  std::string HistoryFilterAction(int id) const {
    switch (id) {
      case IDS_CRFILES_HISTORY_MOVE: return "move";
      case IDS_CRFILES_MOVE_TRASH: return "trash";
      case IDS_CRFILES_HISTORY_ACCESS: return "access";
      case IDS_CRFILES_HISTORY_OPEN_FILE: return "open_file";
      case IDS_CRFILES_HISTORY_SEND: return "send";
      case IDS_CRFILES_HISTORY_RECEIVE: return "receive";
      default: return {};
    }
  }
  bool IsCommandIdChecked(int id) const override {
    if (id >= kContextSortFirst && id < kContextSortFirst + static_cast<int>(kSortMenuLabels.size()))
      return !tabs_.empty() && static_cast<int>(tabs_[active_].options.sort) == id - kContextSortFirst;
    return history_page_ && !history_filter_.empty() && history_filter_ == HistoryFilterAction(id);
  }
  void ClearHistoryFilter() {
    history_filter_.clear(); history_header_->SetFilter({});
    ++history_generation_; history_search_timer_.Stop(); LoadHistory();
    history_search_->RequestFocus();
  }
  void ShowHistoryFilter() {
    if (history_filter_runner_ && history_filter_runner_->IsRunning()) return;
    history_filter_runner_.reset();
    history_filter_model_ = std::make_unique<ui::SimpleMenuModel>(this);
    for (int id : {IDS_CRFILES_HISTORY_MOVE, IDS_CRFILES_MOVE_TRASH,
                  IDS_CRFILES_HISTORY_ACCESS, IDS_CRFILES_HISTORY_OPEN_FILE,
                  IDS_CRFILES_HISTORY_SEND, IDS_CRFILES_HISTORY_RECEIVE})
      history_filter_model_->AddCheckItem(id, Text(id));
    history_filter_runner_ = std::make_unique<views::MenuRunner>(history_filter_model_.get(), views::MenuRunner::CONTEXT_MENU);
    history_filter_runner_->RunMenuAt(GetWidget(), nullptr, history_filter_button_->GetBoundsInScreen(),
        views::MenuAnchorPosition::kTopLeft, ui::mojom::MenuSourceType::kMouse);
  }
  bool IsCommandIdEnabled(int command_id) const override {
    if (command_id == IDS_CRFILES_OPEN_NEW_TAB || command_id == IDS_CRFILES_PROPERTIES)
      return !sidebar_context_path_.empty();
    if (command_id >= kContextOpen && command_id < kContextApplication + 1000) {
      if (!root_ || history_page_ || root_->local_operation_busy_ || trash_busy_ || Current().loading) return false;
      if (command_id == kContextSortBy ||
          (command_id >= kContextSortFirst && command_id < kContextSortFirst + static_cast<int>(kSortMenuLabels.size()))) return true;
      if (command_id == kContextDeleteTag) return tag_id_ == 0 && !context_paths_.empty();
      if (tag_id_ == 0 && command_id != kContextOpen && command_id != kContextSelectAll && command_id != kContextNewFolder) return false;
      if (tag_id_ == 0 && command_id == kContextNewFolder) return true;
      if (command_id == kContextSync) return tag_id_ != 0 && context_paths_.size() == 1;
      if (command_id == kContextSelectAll) return !displayed_entries_.empty() && !order_pending_;
      if (IsTagListing() && (command_id == kContextNewFolder || command_id == kContextPaste ||
          (context_paths_.empty() && (command_id == kContextProperties || command_id == kContextTerminal)))) return false;
      if (command_id == kContextNewFolder)
        return !IsDrivesLocation(Current().path) && Current().path != TrashRoot() && !TrashRoot().IsParent(Current().path);
      if (context_paths_.empty() && (command_id == kContextProperties || command_id == kContextTerminal))
        return !IsDrivesLocation(Current().path);

      if (IsDrivesLocation(Current().path) && command_id != kContextOpen && command_id != kContextNewTab &&
          command_id != kContextSplit && command_id != kContextTerminal && command_id != kContextPath &&
          command_id != kContextProperties && !(command_id >= kContextSplitLeft && command_id <= kContextSplitBottom)) return false;
      if (command_id == kContextPaste) return HasFileClipboard() && !root_->local_operation_busy_ && !Current().loading;
      if (command_id == kContextTrash && (Current().path == TrashRoot() || TrashRoot().IsParent(Current().path))) return false;
      if (context_paths_.empty()) return false;
      const bool single = context_paths_.size() == 1;
      if (command_id == kContextShowPackageContents)
        return single && context_directory_ && context_package_;
      if (command_id >= kContextExtractHere && command_id <= kContextExtractTo)
        return single && !context_directory_ && IsExtractableArchive(context_paths_.front()) && !root_->local_operation_busy_ && !trash_busy_ && !Current().loading;
      if (command_id == kContextRename || command_id == kContextTerminal || command_id == kContextOpen ||
          command_id == kContextOpenWith || command_id >= kContextApplication) return single && !root_->local_operation_busy_;
      if (command_id == kContextNewTab || command_id == kContextSplit ||
          (command_id >= kContextSplitLeft && command_id <= kContextSplitBottom)) return single && context_directory_;
      return !root_->local_operation_busy_ && !trash_busy_ && !Current().loading;
    }
    if (history_page_) return history_header_ && !HistoryFilterAction(command_id).empty();
    if (tabs_.empty()) return false;
    const auto& tab = tabs_[active_];
    switch (command_id) {
      case IDS_CRFILES_RENAME_PANE: return true;
      case IDS_CRFILES_CLOSE_PANE: return pane_can_close_;
      case IDS_CRFILES_MOVE_TRASH:
        return tag_id_ != 0 && !trash_busy_ && !tab.loading && !tab.selected.empty() && tab.path != TrashRoot() && !TrashRoot().IsParent(tab.path);
      default: return false;
    }
  }
  void ExecuteCommand(int command_id, int) override {
    if (!IsCommandIdEnabled(command_id)) return;
    if (command_id == IDS_CRFILES_OPEN_NEW_TAB || command_id == IDS_CRFILES_PROPERTIES) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce([](base::WeakPtr<FilesView> view, int command, base::FilePath path) {
            if (!view) return;
            if (command == IDS_CRFILES_OPEN_NEW_TAB) view->AddPaneTab(path);
            else ShowFolderProperties(view->GetWidget(), path, view->TagOpener());
          }, weak_.GetWeakPtr(), command_id, sidebar_context_path_));
      return;
    }
    if (command_id >= kContextOpen && command_id < kContextApplication + 1000) {
      Deferred(base::BindRepeating(&FilesView::ContextAction, weak_.GetWeakPtr(), command_id, context_generation_, Current().request)).Run(); return;
    }
    if (history_page_ && history_header_) {
      const auto action = HistoryFilterAction(command_id);
      history_filter_ = history_filter_ == action ? std::string() : action;
      history_header_->SetFilter(history_filter_.empty() ? std::u16string() : Text(command_id));
      ++history_generation_; history_search_timer_.Stop(); LoadHistory(); return;
    }
    switch (command_id) {
      case IDS_CRFILES_RENAME_PANE: Deferred(base::BindRepeating(&FilesView::ShowRenamePane, weak_.GetWeakPtr())).Run(); break;
      case IDS_CRFILES_CLOSE_PANE: Deferred(base::BindRepeating(&FilesView::RequestClosePane, weak_.GetWeakPtr())).Run(); break;
      case IDS_CRFILES_MOVE_TRASH: Deferred(base::BindRepeating(&FilesView::ChangeTrash, weak_.GetWeakPtr(), false)).Run(); break;
    }
  }
  void ShowTrustedDevices() {
    if (auto* view = trusted_dialog_.view(); view && view->GetWidget() && !view->GetWidget()->IsClosed()) {
      view->GetWidget()->Activate(); return;
    }
    trusted_dialog_.SetView(ShowTrustedDevicesDialog(GetWidget(), sharing_));
  }
  void ShowSyncing() {
    if (auto* view = syncing_dialog_.view(); view && view->GetWidget() && !view->GetWidget()->IsClosed()) {
      if (view->GetWidget()->IsMinimized()) view->GetWidget()->Restore();
      view->GetWidget()->Show(); view->GetWidget()->Activate(); return;
    }
    syncing_dialog_.SetView(ShowSyncingDialog(GetWidget(), sharing_,
        base::BindRepeating([](base::WeakPtr<FilesView> view, base::FilePath path) {
          if (!view) return;
          if (auto* dialog = view->syncing_dialog_.view(); dialog && dialog->GetWidget()) dialog->GetWidget()->Close();
          view->AddPaneTab(std::move(path));
        }, weak_.GetWeakPtr())));
  }
  void ShowSharingFolders() {
    if (auto* view = sharing_folders_dialog_.view(); view && view->GetWidget() && !view->GetWidget()->IsClosed()) {
      view->GetWidget()->Activate(); return;
    }
    sharing_folders_dialog_.SetView(ShowSharingFoldersDialog(GetWidget(),
        base::BindRepeating([](base::WeakPtr<FilesView> shell) {
          auto targets = shell ? shell->shared_directories_ : std::vector<NearbySharing::SharedTarget>();
          if (!IsSmbEnabledForSession()) std::erase_if(targets, [](const auto& target) { return !target.network_url.empty(); });
          return targets;
        }, weak_.GetWeakPtr()),
        base::BindRepeating([](base::WeakPtr<FilesView> shell, NearbySharing::SharedTarget target) {
          if (!shell) return;
          if (auto* view = shell->sharing_folders_dialog_.view(); view && view->GetWidget())
            view->GetWidget()->Close();
          if (!target.network_url.empty()) {
            const auto credentials = shell->network_share_credentials_.find(target.network_url);
            if (credentials != shell->network_share_credentials_.end()) shell->OpenNetworkDevice(credentials->second);
            else {
              shell->AddPaneTab({}, false, false, -1, {}, {}, true);
              PromptNetworkPage(shell->ActivePane()->network_view_, target.network_url);
            }
          } else if (target.tag_id > 0) shell->AddPaneTab({}, false, false, target.tag_id, target.name);
          else shell->NewTab(target.directory);
        }, weak_.GetWeakPtr()),
        base::BindRepeating([](base::WeakPtr<FilesView> shell, NearbySharing::SharedTarget target) {
          if (!shell) return;
          shell->network_share_checks_.erase(target.network_url);
          shell->network_share_credentials_.erase(target.network_url);
          shell->network_share_attempted_.erase(target.network_url);
          std::erase_if(shell->shared_directories_, [&](const auto& shared) { return shared.id == target.id; });
          shell->SyncSharedTargets(); shell->ScheduleWorkspaceSave();
        }, weak_.GetWeakPtr())));
  }
  void ApplyViewSettings() {
    if (!GetFileViewPreferences().hover_image_preview) CloseHoverPreview(false);
    const bool animate = GetFileViewPreferences().animate_folders;
    for (auto& tab : tabs_) VisitPanes(tab.pane_host, base::BindRepeating([](bool enabled, FilesView* pane) {
      if (pane->history_page_) return;
      if (!enabled) pane->ResetDirectoryAnimation();
      pane->UpdateFooterVisibility();
    }, animate));
    UpdatePaneControls();
    SyncNavigation();
  }
  void ToggleSidebarFromTitlebar() { ToggleSidebar(); }
  bool SidebarVisible() {
    const auto* pane = ActivePane();
    return sidebar_visible_ && (!pane || pane->HasNavigationChrome());
  }
  void ShowMoreMenu() {
    if (!root_ || (more_menu_runner_ && more_menu_runner_->IsRunning())) return;
    more_menu_runner_.reset();
    more_menu_model_ = std::make_unique<ui::SimpleMenuModel>(this);
    auto item = [this](int id, const gfx::VectorIcon& icon) {
      more_menu_model_->AddItemWithIcon(id, Text(id), ui::ImageModel::FromVectorIcon(icon, ui::kColorSysOnSurface, 18));
    };
    item(IDS_CRFILES_RENAME_PANE, kEditIcon);
    if (pane_can_close_) item(IDS_CRFILES_CLOSE_PANE, kCloseIcon);
    more_menu_open_ = true; UpdateFooterVisibility();
    more_menu_runner_ = std::make_unique<views::MenuRunner>(more_menu_model_.get(), views::MenuRunner::CONTEXT_MENU,
        base::BindRepeating([](base::WeakPtr<FilesView> pane) {
          if (!pane) return;
          pane->more_menu_open_ = false; pane->UpdateFooterVisibility();
        }, weak_.GetWeakPtr()));
    more_menu_runner_->RunMenuAt(GetWidget(), nullptr, more_button_->GetBoundsInScreen(),
        views::MenuAnchorPosition::kBubbleBottomRight, ui::mojom::MenuSourceType::kMouse);
  }
  int GetDragOperationsForView(views::View*, const gfx::Point&) override {
    return root_ && !history_page_ && tag_id_ != 0 && !IsDrivesLocation(Current().path) && !Current().loading ? (ui::DragDropTypes::DRAG_COPY | ui::DragDropTypes::DRAG_LINK) : ui::DragDropTypes::DRAG_NONE;
  }
  bool CanStartDragForView(views::View*, const gfx::Point& start, const gfx::Point& point) override {
    return views::View::ExceededDragThreshold(point - start);
  }
  void WriteDragDataForView(views::View* sender, const gfx::Point&, ui::OSExchangeData* data) override {
    if (tag_id_ == 0) return;
    if (IsDrivesLocation(Current().path)) return;
    for (const auto& [path, row] : row_views_) if (row->Contains(sender)) {
      if (!Current().selected.contains(path)) {
        Current().selected = {path};
        for (const auto& [item_path, item] : row_views_)
          item->SetBackground(item_path == path ? views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
      }
      break;
    }
    std::vector<ui::FileInfo> files;
    for (const auto& path : Current().selected) files.emplace_back(path, path.BaseName());
    data->SetFilenames(files); SetFileDragPreview(data); StartFileDrag(); UpdateStatus();
  }
  bool GetDropFormats(int* formats, std::set<ui::ClipboardFormatType>*) override {
    if (!root_) return false;
    *formats = ui::OSExchangeData::FILE_NAME; return true;
  }
  bool AcceptsLocalDrop() {
    return root_ && !history_page_ && !IsTagListing() && !root_->local_operation_busy_ && !Current().loading &&
        !Current().error && !IsDrivesLocation(Current().path) && Current().path != TrashRoot() && !TrashRoot().IsParent(Current().path);
  }
  bool CanDrop(const ui::OSExchangeData& data) override {
    if (!AcceptsLocalDrop()) return false;
    const auto files = data.GetFilenames();
    if (!files || files->empty() || files->size() > 4096) return false;
    for (const auto& file : *files)
      if (!file.path.IsAbsolute() || file.path.ReferencesParent() || IsDriveRoot(file.path)) return false;
    return true;
  }
  base::FilePath DropDirectoryAt(const ui::DropTargetEvent& event) {
    if (!root_ || !files_->contents()) return {};
    auto point = event.location();
    views::View::ConvertPointToTarget(this, files_, &point);
    if (!files_->HitTestPoint(point)) {
      // Empty space uncovered while the body expands is folder background.
      const gfx::Point pane_point(GetMirroredXWithWidthInView(event.x(), 1), event.y());
      if (!split_preview_shown_ && split_hit_area_ && split_hit_area_->Contains(pane_point)) return Current().path;
      return {};
    }
    if (footer_shown_) {
      auto footer_point = event.location();
      views::View::ConvertPointToTarget(this, footer_, &footer_point);
      if (footer_->HitTestPoint(footer_point)) return {};
    }
    point = event.location();
    views::View::ConvertPointToTarget(this, files_->contents(), &point);
    for (const auto& [path, row] : row_views_) {
      if (!row->bounds().Contains(point)) continue;
      auto entry = std::ranges::find(Current().entries, path, &Entry::path);
      return entry != Current().entries.end() && entry->directory && !Current().removed.contains(entry->path) ? path : base::FilePath();
    }
    return Current().path;
  }
  bool ValidDropDestination(const ui::OSExchangeData& data, const base::FilePath& destination) {
    if (destination.empty() || !CanDrop(data)) return false;
    const auto files = data.GetFilenames();
    if (!files) return false;
    for (const auto& file : *files)
      if (file.path == destination || file.path.IsParent(destination) || file.path.DirName() == destination) return false;
    return true;
  }
  struct FolderSplitDrop {
    base::FilePath folder;
    int direction;
    gfx::Rect hit_area, original, preview;
    ui::mojom::DragOperation operation;
  };
  std::optional<FolderSplitDrop> FolderSplitAt(const ui::DropTargetEvent& event) {
    if (!root_ || !root_->file_drag_cursor_observer_ || !files_->contents()) return std::nullopt;
    const auto& folder = root_->file_drag_cursor_observer_->folder();
    if (folder.empty() || !AcceptsLocalDrop()) return std::nullopt;

    const gfx::Rect area = split_hit_area_ ? *split_hit_area_ : SplitDropHitArea();
    // Use the pre-preview viewport even while scrollbars or the footer move.
    const gfx::Point point(GetMirroredXWithWidthInView(event.x(), 1), event.y());
    if (!area.Contains(point)) return std::nullopt;
    const std::array distances = {point.x() - area.x(), area.right() - 1 - point.x(),
                                  point.y() - area.y(), area.bottom() - 1 - point.y()};
    const auto nearest = std::ranges::min_element(distances);
    if (*nearest >= std::min({32, area.width() / 4, area.height() / 4})) return std::nullopt;
    const auto filenames = event.data().GetFilenames();
    if (!filenames || filenames->size() != 1 || filenames->front().path != folder) return std::nullopt;
    const int direction = nearest - distances.begin();  // Left, right, top, bottom.
    const bool new_first = direction == 0 || direction == 2;
    const auto bounds = CalculateSplitBounds(size(), direction >= 2, InitialSplitRatio(new_first));
    const auto& preview = new_first ? bounds.first : bounds.second;
    if (!preview.Intersects(area)) return std::nullopt;
    // Wayland can expose only COPY for a source offering both COPY and LINK.
    auto operation = ui::mojom::DragOperation::kLink;
    if (!(event.source_operations() & ui::DragDropTypes::DRAG_LINK)) {
      if (!(event.source_operations() & ui::DragDropTypes::DRAG_COPY)) return std::nullopt;
      operation = ui::mojom::DragOperation::kCopy;
    }
    return FolderSplitDrop{folder, direction, area, new_first ? bounds.second : bounds.first, preview, operation};
  }
  gfx::Rect SplitDropHitArea() {
    const auto bounds_in_pane = [this](views::View* view) {
      return GetMirroredRect(views::View::ConvertRectToTarget(view, this, view->GetLocalBounds()));
    };
    gfx::Rect area = bounds_in_pane(files_->contents()->parent());
    auto* vertical = files_->vertical_scroll_bar();
    if (vertical->GetVisible()) {
      const auto bar = bounds_in_pane(vertical);
      area.Subtract(gfx::Rect(bar.x(), area.y(), bar.width(), area.height()));
    }
    int bottom = area.bottom();
    if (files_->horizontal_scroll_bar()->GetVisible())
      bottom = std::min(bottom, bounds_in_pane(files_->horizontal_scroll_bar()).y());
    if (footer_shown_) bottom = std::min(bottom, bounds_in_pane(footer_).y());
    area.set_height(std::max(0, bottom - area.y()));

    return area;
  }
  int OnDragUpdated(const ui::DropTargetEvent& event) override {
    if (auto split = FolderSplitAt(event)) {
      SetDropHighlight(false);
      ShowSplitPreview(*split);
      return static_cast<int>(split->operation);
    }
    HideSplitPreview();
    const auto destination = DropDirectoryAt(event);
    if (!ValidDropDestination(event.data(), destination)) { OnDragExited(); return ui::DragDropTypes::DRAG_NONE; }
    const int operation = ui::DragDropTypes::DRAG_COPY;
    if (!(event.source_operations() & operation)) { OnDragExited(); return ui::DragDropTypes::DRAG_NONE; }
    gfx::Rect highlight = GetLocalBounds();
    if (destination != Current().path) {
      auto found = row_views_.find(destination);
      if (found != row_views_.end()) {
        gfx::Point origin; views::View::ConvertPointToTarget(found->second.get(), this, &origin);
        highlight = gfx::Rect(origin, found->second->size());
        gfx::Point viewport_origin; views::View::ConvertPointToTarget(files_, this, &viewport_origin);
        highlight.Intersect(gfx::Rect(viewport_origin, files_->size()));
      }
    }
    SetDropHighlight(true, highlight);
    return operation;
  }
  void OnDragExited() override {
    HideSplitPreview(); SetDropHighlight(false);
    if (root_) root_->UpdatePaneControls();
  }
  DropCallback GetDropCallback(const ui::DropTargetEvent& event) override {
    if (auto split = FolderSplitAt(event)) {
      return base::BindOnce(&FilesView::DropFolderInSplit, weak_.GetWeakPtr(), Current().path, std::move(*split));
    }
    // If the displayed split target disappeared at release, cancel the drop.
    // In particular, Wayland's COPY result must not start a file transfer here.
    if (split_preview_shown_) {
      OnDragExited();
      return base::DoNothing();
    }
    return base::BindOnce(&FilesView::DropFiles, weak_.GetWeakPtr(), Current().path, DropDirectoryAt(event));
  }
  void DropFolderInSplit(base::FilePath target_directory, FolderSplitDrop split, const ui::DropTargetEvent& event,
                         ui::mojom::DragOperation& operation, std::unique_ptr<ui::LayerTreeOwner>) {
    operation = ui::mojom::DragOperation::kNone;
    if (Current().path != target_directory || !CanDrop(event.data())) { OnDragExited(); return; }
    operation = split.operation;
    std::optional<gfx::Rect> original;
    if (split_hit_area_) {
      original = pane_body_->bounds();
      split_preview_animator_->Cancel();
      if (root_->file_drag_cursor_observer_) root_->file_drag_cursor_observer_->SetPreviewCleanup({});
    }
    // Reparent the target after the native drop callback has unwound.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
        [](base::WeakPtr<FilesView> pane, base::FilePath previous, int direction, base::FilePath folder,
           std::optional<gfx::Rect> original) {
          if (!pane) return;
          if (pane->Current().path != previous || !pane->root_->Current().pane_host->Contains(pane.get())) {
            pane->OnDragExited(); return;
          }
          pane->ResetSplitPreview();
          pane->root_->SplitPane(pane.get(), direction, std::move(folder), std::nullopt, original);
        }, weak_.GetWeakPtr(), std::move(target_directory), split.direction, std::move(split.folder), original));
  }
  void DropFiles(base::FilePath source_view_directory, base::FilePath destination, const ui::DropTargetEvent& event,
                 ui::mojom::DragOperation& operation, std::unique_ptr<ui::LayerTreeOwner>) {
    operation = ui::mojom::DragOperation::kNone;
    OnDragExited();
    if (source_view_directory != Current().path || !ValidDropDestination(event.data(), destination)) return;
    if (destination != Current().path) {
      auto entry = std::ranges::find(Current().entries, destination, &Entry::path);
      if (entry == Current().entries.end() || Current().removed.contains(entry->path) || !entry->directory) return;
    }
    if (!(event.source_operations() & ui::DragDropTypes::DRAG_COPY)) return;
    auto files = event.data().GetFilenames(); if (!files) return;
    std::vector<base::FilePath> paths; std::set<base::FilePath> affected{destination, Current().path};
    for (const auto& file : *files) { paths.push_back(file.path); affected.insert(file.path.DirName()); }
    root_->local_operation_busy_ = true;
    status_->SetText(Text(IDS_CRFILES_TRANSFERRING));
    operation = ui::mojom::DragOperation::kCopy;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&TransferLocalFiles, std::move(paths), destination, false, std::string("drag")),
        base::BindOnce(&FilesView::LocalTransferFinished, root_->weak_.GetWeakPtr(), std::move(affected)));
  }
  void LocalTransferFinished(std::set<base::FilePath> affected, FileOperationResult result) {
    local_operation_busy_ = false;
    for (const auto& path : result.removed) std::erase(clipboard_paths_, path);
    for (auto& tab : tabs_) VisitPanes(tab.pane_host,
        base::BindRepeating([](const std::set<base::FilePath>& folders, const FileOperationResult& changes, FilesView* pane) {
          if (pane->history_page_) return;
          if (pane->IsTagListing()) {
            pane->local_operation_error_ = changes.failed != 0;
            pane->local_sources_retained_ = changes.sources_retained != 0;
            pane->LoadTagFiles(); return;
          }
#if BUILDFLAG(IS_WIN)
          if (!folders.contains(pane->Current().path)) {
            const auto current = pane->Current().path.NormalizePathSeparators().StripTrailingSeparators();
            if (std::ranges::any_of(folders, [&](const auto& folder) {
                  return base::FilePath::CompareEqualIgnoreCase(
                      folder.NormalizePathSeparators().StripTrailingSeparators().value(), current.value());
                })) pane->Refresh();
            return;
          }
#else
          if (!folders.contains(pane->Current().path)) return;
#endif
          pane->local_operation_error_ = changes.failed != 0;
          pane->local_sources_retained_ = changes.sources_retained != 0;
          const auto directory = pane->Current().path;
          std::vector<base::FilePath> removed;
          for (const auto& path : changes.removed)
            if (path.DirName() == directory) removed.push_back(path);
          // Moving into a child directory only removes source rows. Keep the
          // viewport, header, selection and existing thumbnails alive.
          if (!removed.empty()) pane->RemoveCompletedEntries(directory, removed);
          const bool received = std::ranges::any_of(changes.changed, [&](const base::FilePath& path) {
            return path.DirName() == directory;
          });
          if (received) pane->Refresh();
          else pane->UpdateStatus();
        }, affected, result));
  }

  std::vector<base::FilePath> SelectionPaths() const {
    if (!root_ || history_page_ || tag_id_ == 0 || tabs_.empty()) return {};
    const auto& selected = tabs_[active_].selected;
    return {selected.begin(), selected.end()};
  }
  base::FilePath SidebarPath(int label) const {
    switch (label) {
      case IDS_CRFILES_HOME: return Home();
      case IDS_CRFILES_DESKTOP: return DesktopPath();
      case IDS_CRFILES_DOWNLOADS: return NearbySharing::DefaultReceiveDirectory().DirName();
#if BUILDFLAG(IS_MAC)
      case IDS_CRFILES_APPLICATIONS: return base::FilePath(FILE_PATH_LITERAL("/Applications"));
#endif
      case IDS_CRFILES_RECEIVED: {
        const auto prefs = GetNearbySharingPreferences();
        return prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() :
            base::FilePath::FromUTF8Unsafe(prefs.directory);
      }
      case IDS_CRFILES_TRASH: return TrashRoot();
      default: return {};
    }
  }
  void ShowContextMenuForViewImpl(views::View* source, const gfx::Point& point, ui::mojom::MenuSourceType type) override {
    for (const auto& [label, button] : sidebar_buttons_) {
      if (!button->Contains(source)) continue;
      if (context_runner_ && context_runner_->IsRunning()) return;
      sidebar_context_path_ = SidebarPath(label);
      if (sidebar_context_path_.empty()) return;
      context_runner_.reset();
      context_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
      context_menu_->AddItem(IDS_CRFILES_OPEN_NEW_TAB, Text(IDS_CRFILES_OPEN_NEW_TAB));
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      context_menu_->AddItem(IDS_CRFILES_PROPERTIES, Text(IDS_CRFILES_PROPERTIES));
      context_runner_ = std::make_unique<views::MenuRunner>(context_menu_.get(), views::MenuRunner::CONTEXT_MENU);
      context_runner_->RunMenuAt(GetWidget(), nullptr, gfx::Rect(point, gfx::Size(1, 1)),
                                views::MenuAnchorPosition::kTopLeft, type);
      return;
    }
    if (!root_ || history_page_ || Current().loading || root_->local_operation_busy_) return;
    if (context_runner_ && context_runner_->IsRunning()) return;
    ActivateSelf(); context_paths_.clear(); context_apps_.clear(); context_directory_ = false;
    for (const auto& [path, row] : row_views_) if (row->Contains(source)) {
      if (!Current().selected.contains(path)) {
        Current().selected.clear(); Current().selected.insert(path);
        for (const auto& [item, view] : row_views_) view->SetBackground(item == path ?
            views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
        keyboard_position_ = static_cast<FileLayout*>(files_->contents())->IndexOf(row);
        keyboard_anchor_ = keyboard_position_; UpdateStatus();
      }
      // Context menus also operate on virtual tag entries. SelectionPaths()
      // intentionally excludes them from filesystem operations.
      context_paths_.assign(Current().selected.begin(), Current().selected.end());
      auto entry = std::ranges::find(Current().entries, path, &Entry::path);
      context_directory_ = entry != Current().entries.end() && entry->directory;
      break;
    }
    if (context_paths_.empty() && type == ui::mojom::MenuSourceType::kKeyboard) {
      context_paths_.assign(Current().selected.begin(), Current().selected.end());
      if (!context_paths_.empty()) {
        auto entry = std::ranges::find(Current().entries, context_paths_.front(), &Entry::path);
        context_directory_ = entry != Current().entries.end() && entry->directory;
      }
    }
    const uint64_t generation = ++context_generation_;
    context_package_ = false;
    if (context_paths_.size() == 1) {
      const auto entry = std::ranges::find(Current().entries, context_paths_.front(), &Entry::path);
      context_package_ = entry != Current().entries.end() && entry->package;
    }
    if (context_paths_.size() == 1 && (!context_directory_ || context_package_)) {
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&FileApplications, context_paths_.front()),
          base::BindOnce(&FilesView::ContextApplicationsLoaded, weak_.GetWeakPtr(), generation, Current().request, point, type));
    } else ShowFileMenu(point, type);
  }
  void ContextApplicationsLoaded(uint64_t generation, uint64_t request, gfx::Point point,
      ui::mojom::MenuSourceType type, std::vector<FileApplication> apps) {
    if (generation != context_generation_ || request != Current().request || !IsDrawn()) return;
    context_apps_ = std::move(apps); if (context_apps_.size() > 1000) context_apps_.resize(1000);
    ShowFileMenu(point, type);
  }
  void ShowFileMenu(gfx::Point point, ui::mojom::MenuSourceType type) {
    context_runner_.reset(); context_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
    context_split_ = std::make_unique<ui::SimpleMenuModel>(this);
    context_open_with_ = std::make_unique<ui::SimpleMenuModel>(this);
    auto image = [](const gfx::VectorIcon& icon) {
      return ui::ImageModel::FromVectorIcon(icon, ui::kColorMenuIcon, 18);
    };
    auto app_image = [&](const FileApplication& app) {
      return app.icon.isNull() ? image(kOpenInNewIcon) : ui::ImageModel::FromImageSkia(app.icon);
    };
    auto add = [&](int command, int label, const gfx::VectorIcon* icon = nullptr) {
      if (icon) context_menu_->AddItemWithIcon(command, Text(label), image(*icon));
      else context_menu_->AddItem(command, Text(label));
    };
    if (context_paths_.empty()) {
      add(kContextNewFolder, tag_id_ == 0 ? IDS_CRFILES_TAG_CREATE : IDS_CRFILES_NEW_FOLDER, tag_id_ == 0 ? &kTagsIcon : &vector_icons::kFolderOpenIcon);
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      context_sort_ = std::make_unique<ui::SimpleMenuModel>(this);
      for (size_t i = 0; i < kSortMenuLabels.size(); ++i) {
        const auto sort = static_cast<FileSort>(i);
        if (sort == FileSort::kTypeDescending || sort == FileSort::kPermissionsAscending ||
            sort == FileSort::kPermissionsDescending) continue;
        context_sort_->AddRadioItem(kContextSortFirst + static_cast<int>(i), Text(kSortMenuLabels[i]), 1);
      }
      context_menu_->AddSubMenu(kContextSortBy, Text(IDS_CRFILES_SORT_BY), context_sort_.get());
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      if (tag_id_ != 0) add(kContextPaste, IDS_CRFILES_PASTE, &kContentPasteIcon);
      add(kContextSelectAll, IDS_CRFILES_SELECT_ALL);
      if (tag_id_ != 0) {
        context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
        add(kContextTerminal, IDS_CRFILES_OPEN_TERMINAL, &kCodeIcon);
        context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
        add(kContextProperties, IDS_CRFILES_PROPERTIES, &kInfoIcon);
      }
    } else if (tag_id_ == 0) {
      add(kContextOpen, IDS_CRFILES_OPEN, &kTagFolderIcon);
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      add(kContextDeleteTag, IDS_CRFILES_TAG_DELETE, &kDeleteIcon);
    } else {
      if (context_paths_.size() == 1 && context_directory_ && !context_package_) {
        add(kContextNewTab, IDS_CRFILES_OPEN_NEW_TAB, &kOpenInNewIcon);
        const std::array labels = {IDS_CRFILES_SPLIT_LEFT, IDS_CRFILES_SPLIT_RIGHT, IDS_CRFILES_SPLIT_TOP, IDS_CRFILES_SPLIT_BOTTOM};
        const std::array<const gfx::VectorIcon*, 4> icons = {&kSplitSceneLeftIcon, &kSplitSceneRightIcon, &kSplitSceneUpIcon, &kSplitSceneDownIcon};
        for (size_t i = 0; i < labels.size(); ++i)
          context_split_->AddItemWithIcon(kContextSplitLeft + i, Text(labels[i]), image(*icons[i]));
        context_menu_->AddSubMenuWithIcon(kContextSplit, Text(IDS_CRFILES_OPEN_SPLIT), context_split_.get(), image(kSplitSceneRightIcon));
      } else if (context_paths_.size() == 1) {
        const auto preferred = std::ranges::find(context_apps_, true, &FileApplication::is_default);
        if (preferred != context_apps_.end())
          context_menu_->AddItemWithIcon(kContextApplication + std::distance(context_apps_.begin(), preferred), Text(IDS_CRFILES_OPEN_WITH) + u" " + base::UTF8ToUTF16(preferred->name), app_image(*preferred));
        else add(kContextOpen, IDS_CRFILES_OPEN, &kOpenInNewIcon);
        for (size_t i = 0; i < context_apps_.size(); ++i)
          context_open_with_->AddItemWithIcon(kContextApplication + i, base::UTF8ToUTF16(context_apps_[i].name), app_image(context_apps_[i]));
        if (context_apps_.empty()) context_open_with_->AddItemWithIcon(kContextOpen, Text(IDS_CRFILES_OPEN), image(kOpenInNewIcon));
        context_menu_->AddSubMenuWithIcon(kContextOpenWith, Text(IDS_CRFILES_OPEN_WITH), context_open_with_.get(), image(kOpenInNewIcon));
        if (context_package_)
          add(kContextShowPackageContents, IDS_CRFILES_SHOW_PACKAGE_CONTENTS, &vector_icons::kFolderOpenIcon);
      }
      if (context_paths_.size() == 1 && !context_directory_ && IsExtractableArchive(context_paths_.front())) {
        context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
        add(kContextExtractHere, IDS_CRFILES_EXTRACT_HERE, &vector_icons::kFolderOpenIcon);
        auto extract_label = l10n_util::GetStringFUTF16(
            IDS_CRFILES_EXTRACT_NAMED, ArchiveFolderName(context_paths_.front()).AsUTF16Unsafe());
#if BUILDFLAG(IS_WIN)
        // The separator follows the name inside some translations, not at the end.
        std::ranges::replace(extract_label, u'/', u'\\');
#endif
        context_menu_->AddItemWithIcon(kContextExtractFolder,
            extract_label,
            image(vector_icons::kFolderOpenIcon));
        add(kContextExtractTo, IDS_CRFILES_EXTRACT_TO, &vector_icons::kFolderOpenIcon);
      }
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      add(kContextCut, IDS_CRFILES_CUT, &kContentCutIcon); add(kContextCopy, IDS_CRFILES_HISTORY_COPY, &vector_icons::kContentCopyIcon);
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      add(kContextAddTags, IDS_CRFILES_TAGS, &kTagsIcon);
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      add(kContextShare, IDS_CRFILES_SHARE_TO, &vector_icons::kScreenShareIcon);
      add(kContextSync, IDS_CRFILES_SYNC_TO, &vector_icons::kSyncIcon);
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      context_menu_->AddItemWithIcon(kContextQuickShare, Text(IDS_CRFILES_QUICK_SHARE),
          ui::ImageModel::FromVectorIcon(kQuickShareAndroidIcon, ui::kColorSysTertiary, 18));
#endif
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      add(kContextRename, IDS_CRFILES_RENAME, &kEditIcon); add(kContextCompress, IDS_CRFILES_COMPRESS, &kCompressIcon); add(kContextTrash, IDS_CRFILES_MOVE_TRASH, &kDeleteIcon);
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      add(kContextTerminal, IDS_CRFILES_OPEN_TERMINAL, &kCodeIcon); add(kContextPath, IDS_CRFILES_COPY_PATH, &vector_icons::kLinkIcon);
      context_menu_->AddSeparator(ui::NORMAL_SEPARATOR); add(kContextProperties, IDS_CRFILES_PROPERTIES, &kInfoIcon);
    }
    context_runner_ = std::make_unique<views::MenuRunner>(context_menu_.get(), views::MenuRunner::CONTEXT_MENU);
    context_runner_->RunMenuAt(GetWidget(), nullptr, gfx::Rect(point, gfx::Size(1, 1)), views::MenuAnchorPosition::kTopLeft, type);
  }
  void CopySelection(bool cut) {
    if (IsDrivesLocation(Current().path)) return;
    auto paths = SelectionPaths(); if (paths.empty()) return;
    root_->clipboard_paths_ = paths; root_->clipboard_move_ = cut;
    std::string uris;
    for (const auto& path : paths) uris += net::FilePathToFileURL(path).spec() + "\r\n";
    {
      ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
      writer.WriteFilenames(uris);
    }
#if BUILDFLAG(IS_WIN)
    SetWindowsClipboardMove(cut);
#endif
    root_->clipboard_sequence_ = ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(ui::ClipboardBuffer::kCopyPaste);
    NetworkFileClipboard();
  }
  bool HasFileClipboard() const {
    if (NetworkFileClipboard()) return true;
#if BUILDFLAG(IS_MAC)
    if (HasFileClipboardMac()) return true;
#endif
#if BUILDFLAG(IS_WIN)
    if (HasWindowsFileClipboard()) return true;
#endif
    return root_ && !root_->clipboard_paths_.empty() && root_->clipboard_sequence_ &&
        *root_->clipboard_sequence_ == ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(ui::ClipboardBuffer::kCopyPaste);
  }
  void PasteSelection(bool force_move = false) {
    if (network_view_) { PasteNetworkPage(network_view_); return; }
    if (IsTagListing()) return;
    if (IsDrivesLocation(Current().path)) return;
    if (!root_ || history_page_ || Current().loading || root_->local_operation_busy_ || !HasFileClipboard()) return;
    if (auto source = NetworkFileClipboard()) {
      const auto destination = Current().path;
      EnsureDirectoryAccess(GetWidget(), destination, true, base::BindOnce(
          [](base::WeakPtr<FilesView> pane, std::shared_ptr<const NetworkClipboard> source, base::FilePath destination, bool granted) {
            if (!pane || !granted) return;
            CopyNetworkSelection(pane->GetWidget(), std::move(source), {}, std::nullopt, destination,
                base::BindOnce([](base::WeakPtr<FilesView> pane, base::FilePath destination) {
                  if (pane && pane->Current().path == destination) pane->QueueLiveRead();
                }, pane, destination));
          }, weak_.GetWeakPtr(), std::move(source), destination));
      return;
    }
#if BUILDFLAG(IS_WIN)
    if (!root_->clipboard_paths_.empty() && root_->clipboard_sequence_ &&
        *root_->clipboard_sequence_ == ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(ui::ClipboardBuffer::kCopyPaste)) {
      StartMenuTransfer(root_->clipboard_paths_, Current().path, root_->clipboard_move_ || force_move);
      return;
    }
    auto clipboard = ReadWindowsFileClipboard();
    if (!clipboard.paths.empty()) {
      StartMenuTransfer(std::move(clipboard.paths), Current().path, clipboard.move || force_move);
      return;
    }
    return;
#elif BUILDFLAG(IS_MAC)
    const bool own_clipboard = root_->clipboard_sequence_ &&
        *root_->clipboard_sequence_ == ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(ui::ClipboardBuffer::kCopyPaste);
    auto paths = ReadFileClipboardMac();
    if (!paths.empty()) StartMenuTransfer(std::move(paths), Current().path,
        force_move || (own_clipboard && root_->clipboard_move_));
#else
    StartMenuTransfer(root_->clipboard_paths_, Current().path, root_->clipboard_move_ || force_move);
#endif
  }
  void StartMenuTransfer(std::vector<base::FilePath> paths, base::FilePath destination, bool move) {
    EnsureDirectoryAccess(GetWidget(), destination, true, base::BindOnce(
        [](base::WeakPtr<FilesView> pane, std::vector<base::FilePath> paths,
           base::FilePath destination, bool move, bool allowed) {
          if (!pane) return;
          if (allowed) pane->StartMenuTransferAuthorized(std::move(paths), std::move(destination), move);
          else pane->status_->SetText(Text(IDS_CRFILES_READ_ERROR));
        }, weak_.GetWeakPtr(), std::move(paths), destination, move));
  }
  void StartMenuTransferAuthorized(std::vector<base::FilePath> paths, base::FilePath destination, bool move) {
    if (root_->local_operation_busy_ || paths.empty() || IsDrivesLocation(destination) ||
        std::ranges::any_of(paths, IsDriveRoot)) return;
    std::set<base::FilePath> affected{destination};
    for (const auto& path : paths) affected.insert(path.DirName());
    root_->local_operation_busy_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&TransferLocalFiles, std::move(paths), destination, move, std::string("menu")),
        base::BindOnce(&FilesView::LocalTransferFinished, root_->weak_.GetWeakPtr(), std::move(affected)));
  }
  void FileSelected(const ui::SelectedFileInfo& file, int) override {
    RememberFileAccess(file.path());
    destination_dialog_.reset();
    if (selecting_send_) {
      selecting_send_ = false;
      ShowSharing({file.path()});
      return;
    }
    StartMenuTransfer(std::move(destination_sources_), file.path(), destination_move_);
  }
  void MultiFilesSelected(const std::vector<ui::SelectedFileInfo>& files) override {
    destination_dialog_.reset();
    selecting_send_ = false;
    std::vector<base::FilePath> paths;
    for (const auto& file : files) {
      RememberFileAccess(file.path());
      paths.push_back(file.path());
    }
    if (!paths.empty()) ShowSharing(std::move(paths));
  }
  void FileSelectionCanceled() override {
    destination_dialog_.reset(); destination_sources_.clear(); selecting_send_ = false;
  }
  void ChooseFilesToSend() {
    if (destination_dialog_) return;
    if (SharingBusy()) { ShowSharing({}); return; }
    selecting_send_ = true;
    destination_dialog_ = ui::SelectFileDialog::Create(this, nullptr);
    destination_dialog_->SelectFile(ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE,
        Text(IDS_CRFILES_TRAY_SEND), ActivePane()->Current().path, nullptr, 0, {},
        GetWidget()->GetNativeWindow(), nullptr);
  }
  void ShowReceivedFiles() { Received(); }
  void ShowReceivedDirectory(const base::FilePath& directory) {
    if (directory.empty() || !directory.IsAbsolute()) return;
    if (auto* pane = ActivePane()) {
      if (pane->network_page_ || pane->history_page_) AddPaneTab(directory);
      else pane->Navigate(directory);
    }
  }
  void ChooseDestination(bool move) {
    if (destination_dialog_) return;
    destination_sources_ = context_paths_; destination_move_ = move;
    destination_dialog_ = ui::SelectFileDialog::Create(this, nullptr);
    destination_dialog_->SelectFile(ui::SelectFileDialog::SELECT_FOLDER, Text(move ? IDS_CRFILES_MOVE_TO : IDS_CRFILES_COPY_TO),
        Current().path, nullptr, 0, {}, GetWidget()->GetNativeWindow(), nullptr);
  }
  void StartExtraction(base::FilePath archive, base::FilePath destination, bool same_name_folder) {
    EnsureDirectoryAccess(GetWidget(), destination, true, base::BindOnce(
        [](base::WeakPtr<FilesView> pane, base::FilePath archive, base::FilePath destination,
           bool same_name_folder, bool allowed) {
          if (!pane) return;
          if (allowed) pane->StartExtractionAuthorized(std::move(archive), std::move(destination), same_name_folder);
          else pane->status_->SetText(Text(IDS_CRFILES_READ_ERROR));
        }, weak_.GetWeakPtr(), archive, destination, same_name_folder));
  }
  void StartExtractionAuthorized(base::FilePath archive, base::FilePath destination, bool same_name_folder) {
    if (root_->local_operation_busy_ || !IsExtractableArchive(archive) || IsDrivesLocation(destination)) return;
    root_->local_operation_busy_ = true;
    ShowArchiveDialog(this, archive, destination, same_name_folder,
        base::BindRepeating([](base::WeakPtr<FilesView> pane, const base::FilePath& path) {
          if (!pane) return gfx::Rect();
          auto found = pane->file_buttons_.find(path);
          if (found == pane->file_buttons_.end() || !found->second) return gfx::Rect();
          return static_cast<FileButton*>(found->second.get())->FilenameBoundsInScreen();
        }, weak_.GetWeakPtr(), archive),
        base::BindOnce(&FilesView::LocalTransferFinished, root_->weak_.GetWeakPtr(), std::set<base::FilePath>{destination}));
  }
  void ChooseExtractionDestination() {
    if (context_paths_.size() != 1) return;
    const auto archive = context_paths_.front();
    CloseNewFolder();
    new_folder_delegate_ = std::make_unique<ArchivePathBubble>(this, base::BindRepeating(
        [](base::WeakPtr<FilesView> pane, const base::FilePath& path) {
          if (!pane) return gfx::Rect();
          auto found = pane->file_buttons_.find(path);
          return found == pane->file_buttons_.end() || !found->second ? gfx::Rect() :
              static_cast<FileButton*>(found->second.get())->FilenameBoundsInScreen();
        }, weak_.GetWeakPtr(), archive));
    const auto margins = new_folder_delegate_->margins();
    new_folder_delegate_->set_margins(gfx::Insets::TLBR(margins.top() / 2, margins.left() / 2,
        margins.bottom() / 2, margins.right() / 2));
    new_folder_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    new_folder_delegate_->set_close_on_deactivate(false);
    auto contents = std::make_unique<NewFolderView>(base::BindRepeating(
        [](base::WeakPtr<FilesView> pane, base::FilePath source, std::u16string text) {
          if (!pane) return;
          auto path = ParseNavigationPath(text);
          if (!path || !path->IsAbsolute() || IsDrivesLocation(*path)) { pane->new_folder_contents_->Failed(); return; }
          pane->Deferred(base::BindRepeating([](base::WeakPtr<FilesView> view, base::FilePath input, base::FilePath destination) {
            if (!view) return;
            view->CloseNewFolder(); view->StartExtraction(input, destination, false);
          }, pane, source, *path)).Run();
        }, weak_.GetWeakPtr(), archive), IDS_CRFILES_EXTRACT, IDS_CRFILES_EXTRACT_ERROR, true);
    new_folder_contents_ = contents.get();
    auto initial = NavigationPath(archive.DirName());
    if (!initial.ends_with(u"/") && !initial.ends_with(u"\\")) initial += u"/";
    contents->field()->SetText(initial);
    new_folder_delegate_->SetInitiallyFocusedView(contents->field());
    new_folder_delegate_->SetContentsView(std::move(contents));
    new_folder_widget_ = CreateCrFilesBubble(new_folder_delegate_.get(),
        base::BindOnce([](base::WeakPtr<FilesView> pane, views::Widget::ClosedReason) {
          if (!pane) return;
          pane->rename_focus_observer_.reset();
          pane->new_folder_contents_ = nullptr;
          pane->new_folder_delegate_->SetAnchorView(nullptr); pane->new_folder_widget_.reset();
        }, weak_.GetWeakPtr()));
    rename_focus_observer_ = std::make_unique<RenameFocusObserver>(GetWidget(), new_folder_widget_.get(), new_folder_contents_->field());
    new_folder_widget_->Show();
  }
  void ContextAction(int command, uint64_t generation, uint64_t request) {
    if (generation != context_generation_ || request != Current().request || !IsDrawn() || !IsCommandIdEnabled(command)) return;
    if (command == kContextPaste) { PasteSelection(); return; }
    if (command == kContextNewFolder) { ShowNewFolder(); return; }
    if (command >= kContextSortFirst && command < kContextSortFirst + static_cast<int>(kSortMenuLabels.size())) {
      CloseViewOptions();
      auto options = Current().options;
      options.sort = static_cast<FileSort>(command - kContextSortFirst);
      ViewOptionsChanged(Current().id, options);
      return;
    }
    if (command == kContextSelectAll) {
      auto& selected = Current().selected;
      selected.clear();
      for (size_t index : displayed_entries_) selected.insert(Current().entries[index].path);
      for (const auto& [path, row] : row_views_)
        row->SetBackground(selected.contains(path) ?
            views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
      UpdateStatus();
      files_->RequestFocus();
      return;
    }
    if (context_paths_.empty() && command == kContextProperties) {
      ShowFolderProperties(GetWidget(), Current().path, TagOpener());
      return;
    }
    if (context_paths_.empty() && command == kContextTerminal) {
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
          {base::MayBlock(), base::WithBaseSyncPrimitives()},
          base::BindOnce(&OpenFolderTerminal, Current().path),
          base::BindOnce(&FilesView::ContextLaunchFinished, weak_.GetWeakPtr()));
      return;
    }

    if (context_paths_.empty()) return;
    const auto path = context_paths_.front();
    if (command >= kContextApplication && static_cast<size_t>(command - kContextApplication) < context_apps_.size()) {
      // macOS waits for NSWorkspace to report whether the launch succeeded.
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
          {base::MayBlock(), base::WithBaseSyncPrimitives()},
          base::BindOnce(&OpenFileWith, path, context_apps_[command - kContextApplication].id),
          base::BindOnce(&FilesView::ContextLaunchFinished, weak_.GetWeakPtr())); return;
    }
    if (command >= kContextSplitLeft && command <= kContextSplitBottom) { root_->SplitPane(this, command - kContextSplitLeft, path); return; }
    switch (command) {
      case kContextAddTags: ShowFileTagEditor(GetWidget(), context_paths_); break;
      case kContextDeleteTag: DeleteSelectedTags(); break;
      case kContextOpen: Open(path, context_directory_); break;
      case kContextShowPackageContents: Navigate(path); break;
      case kContextNewTab: root_->AddPaneTab(path); break;
      case kContextCut: CopySelection(true); break;
      case kContextCopy: CopySelection(false); break;
      case kContextMoveTo: ChooseDestination(true); break;
      case kContextCopyTo: ChooseDestination(false); break;
      case kContextExtractHere: StartExtraction(path, path.DirName(), false); break;
      case kContextExtractFolder: StartExtraction(path, path.DirName(), true); break;
      case kContextExtractTo: ChooseExtractionDestination(); break;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      case kContextQuickShare:
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce(&FilesView::ShowQuickSharing,
                (root_ ? root_.get() : this)->weak_.GetWeakPtr(), context_paths_));
        break;
#endif
      case kContextSync:
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce(&FilesView::ShowSyncTo,
                (root_ ? root_.get() : this)->weak_.GetWeakPtr(), context_paths_));
        break;
      case kContextShare:
        // Finish native context-menu dispatch before opening a modal window.
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
            base::BindOnce(&FilesView::ShowSharing,
                (root_ ? root_.get() : this)->weak_.GetWeakPtr(), context_paths_));
        break;
      case kContextRename: case kContextCompress: ShowEntryName(command == kContextCompress); break;
      case kContextTrash: Current().selected = std::set<base::FilePath>(context_paths_.begin(), context_paths_.end()); ChangeTrash(false); break;
      case kContextTerminal:
        base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
            base::BindOnce(&OpenFolderTerminal, context_directory_ ? path : path.DirName()),
            base::BindOnce(&FilesView::ContextLaunchFinished, weak_.GetWeakPtr())); break;
      case kContextPath: {
        std::u16string text; for (const auto& item : context_paths_) { if (!text.empty()) text += u"\n"; text += item.AsUTF16Unsafe(); }
        ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste).WriteText(text); break;
      }
      case kContextProperties: ShowEntryProperties(); break;
    }
  }
  void DeleteSelectedTags() {
    std::vector<int64_t> ids;
    std::u16string locations;
    for (const auto& entry : Current().entries) {
      if (entry.tag <= 0 || std::ranges::find(context_paths_, entry.path) == context_paths_.end()) continue;
      ids.push_back(entry.tag);
      if (!locations.empty()) locations += u"\n";
      locations += Text(IDS_CRFILES_TAGS) + u"/" + base::UTF8ToUTF16(entry.tag_name);
    }
    if (ids.empty()) return;
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog->SetTitle(Text(IDS_CRFILES_TAG_DELETE));
    dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) |
                       static_cast<int>(ui::mojom::DialogButton::kCancel));
    dialog->SetDefaultButton(static_cast<int>(ui::mojom::DialogButton::kCancel));
    dialog->SetButtonLabel(ui::mojom::DialogButton::kOk, Text(IDS_CRFILES_TAG_DELETE));
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(20), 12));
    auto* explanation = contents->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_TAG_DELETE_CONFIRM)));
    explanation->SetMultiLine(true); explanation->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    explanation->SizeToFit(420);
    auto* scroll = contents->AddChildView(std::make_unique<views::ScrollView>());
    scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    scroll->ClipHeightTo(0, 240);
    auto names = std::make_unique<views::Label>(locations);
    names->SetMultiLine(true); names->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    names->SizeToFit(420); scroll->SetContents(std::move(names));
    dialog->SetContentsView(std::move(contents));
    dialog->SetAcceptCallbackWithClose(base::BindRepeating([](base::WeakPtr<FilesView> pane, const std::vector<int64_t>& selected_ids) {
      if (!pane) return true;
      if (pane->root_->local_operation_busy_) return false;
      pane->DeleteTagsConfirmed(selected_ids);
      return true;
    }, weak_.GetWeakPtr(), std::move(ids)));
    ShowCrFilesDialog(std::move(dialog), GetWidget());
  }
  void DeleteTagsConfirmed(std::vector<int64_t> ids) {
    root_->local_operation_busy_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&DeleteFileTags, ids),
        base::BindOnce([](base::WeakPtr<FilesView> shell, std::vector<int64_t> deleted, TagWriteResult result) {
          if (!shell) return;
          shell->local_operation_busy_ = false;
          if (result != TagWriteResult::kOk) {
            auto dialog = std::make_unique<views::DialogDelegate>();
            dialog->SetTitle(Text(IDS_CRFILES_TAG_DELETE));
            auto label = std::make_unique<views::Label>(Text(IDS_CRFILES_TAG_DELETE_ERROR));
            label->SetBorder(views::CreateEmptyBorder(gfx::Insets(16)));
            dialog->SetContentsView(std::move(label));
            ShowCrFilesDialog(std::move(dialog), shell->GetWidget());
            return;
          }
          std::erase_if(shell->shared_directories_, [&](const auto& target) {
            return std::ranges::find(deleted, target.tag_id) != deleted.end();
          });
          shell->SyncSharedTargets();
          for (auto& tab : shell->tabs_) {
            if (!tab.pane_host) continue;
            shell->VisitPanes(tab.pane_host, base::BindRepeating([](const std::vector<int64_t>& deleted_ids, FilesView* pane) {
              if (std::ranges::find(deleted_ids, pane->tag_id_) != deleted_ids.end())
                pane->NavigateTag({0, {}, {}, {}});
              else if (pane->tag_id_ == 0) pane->LoadTagFiles();
            }, deleted));
          }
          shell->ScheduleWorkspaceSave();
        }, root_->weak_.GetWeakPtr(), ids));
  }
  void ContextLaunchFinished(bool ok) { if (!ok) status_->SetText(Text(IDS_CRFILES_OPEN_ERROR)); }
  void ShowEntryName(bool compress) {
    auto dialog = std::make_unique<views::DialogDelegate>(); dialog->SetTitle(Text(compress ? IDS_CRFILES_COMPRESS : IDS_CRFILES_RENAME));
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
    auto* name = contents->AddChildView(std::make_unique<views::Textfield>());
    name->SetAccessibleName(Text(IDS_CRFILES_NAME)); name->SetPreferredSize(gfx::Size(360, 36)); name->RemoveHoverEffect();
    name->SetText(context_paths_.front().BaseName().AsUTF16Unsafe() + (compress ? u".zip" : u""));
    dialog->SetInitiallyFocusedView(name);
    dialog->SetAcceptCallbackWithClose(base::BindRepeating([](base::WeakPtr<FilesView> pane,
        const std::vector<base::FilePath>& paths, bool archive, views::Textfield* field) {
      return !pane || pane->SubmitEntryName(paths, archive, field);
    }, weak_.GetWeakPtr(), context_paths_, compress, base::Unretained(name)));
    dialog->SetContentsView(std::move(contents));
    ShowCrFilesDialog(std::move(dialog), GetWidget());
  }
  bool SubmitEntryName(std::vector<base::FilePath> paths, bool compress, views::Textfield* name) {
    auto text = std::u16string(name->GetText()); const auto child = base::FilePath::FromUTF16Unsafe(text);
    if (root_->local_operation_busy_ || !IsValidEntryName(text)) { name->SetInvalid(true); return false; }
    auto parent = paths.front().DirName(); auto target = parent.Append(child);
    root_->local_operation_busy_ = true;
    if (compress) {
      const auto source = paths.front();
      ShowCompressDialog(this, std::move(paths), target,
          base::BindRepeating([](base::WeakPtr<FilesView> pane, const base::FilePath& path) {
            if (!pane) return gfx::Rect();
            auto found = pane->file_buttons_.find(path);
            return found == pane->file_buttons_.end() || !found->second ? gfx::Rect() :
                static_cast<FileButton*>(found->second.get())->FilenameBoundsInScreen();
          }, weak_.GetWeakPtr(), source),
          base::BindOnce(&FilesView::LocalTransferFinished, root_->weak_.GetWeakPtr(), std::set<base::FilePath>{parent}));
      return true;
    }
    auto task = base::BindOnce(&RenameLocalFile, paths.front(), target);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE}, std::move(task),
        base::BindOnce(&FilesView::LocalTransferFinished, root_->weak_.GetWeakPtr(), std::set<base::FilePath>{parent}));
    return true;
  }
  void ShowEntryProperties() {
    ShowFileProperties(GetWidget(), context_paths_, Current().entries.values(),
                       !IsDrivesLocation(Current().path), TagOpener());
  }

 private:
  void CloseHoverPreview(bool animate = true) {
    hover_wait_timer_.Stop(); hover_cancel_->store(true);
    hover_owner_.reset(); hover_path_.clear(); hover_ready_ = false;
    if (!hover_panel_) return;
    hover_hide_timer_.Stop();
    hover_panel_->layer()->GetAnimator()->StopAnimating();
    if (!animate) { hover_panel_->SetVisible(false); return; }
    ui::ScopedLayerAnimationSettings settings(hover_panel_->layer()->GetAnimator());
    settings.SetTransitionDuration(base::Milliseconds(120));
    settings.SetTweenType(gfx::Tween::EASE_OUT);
    hover_panel_->layer()->SetOpacity(0.f);
    hover_hide_timer_.Start(FROM_HERE, base::Milliseconds(120), base::BindOnce([](base::WeakPtr<FilesView> shell) {
      if (shell && shell->hover_panel_) shell->hover_panel_->SetVisible(false);
    }, weak_.GetWeakPtr()));
  }
  bool HoverPreviewValid() {
    if (!GetFileViewPreferences().hover_image_preview) return false;
    if (!hover_owner_ || hover_path_.empty() || !GetWidget() || !GetWidget()->IsActive() || !display::Screen::Get()) return false;
    auto* pane = hover_owner_.get();
    if (!pane->IsDrawn() || pane->LiveUpdateBusy() || GetBoundsInScreen() != hover_root_bounds_) return false;
    const auto row = pane->row_views_.find(hover_path_);
    if (row == pane->row_views_.end()) return false;
    const auto point = display::Screen::Get()->GetCursorScreenPoint();
    return row->second->GetBoundsInScreen().Contains(point) && pane->files_->GetBoundsInScreen().Contains(point);
  }
  void PollImageHover(gfx::Point point) {
    if (!GetFileViewPreferences().hover_image_preview) {
      if (!hover_path_.empty()) CloseHoverPreview(false);
      return;
    }
    FilesView* owner = nullptr;
    base::FilePath path;
    if (GetWidget() && GetWidget()->IsActive()) VisitPanes(Current().pane_host,
        base::BindRepeating([](gfx::Point point, FilesView** owner, base::FilePath* path, FilesView* pane) {
          if (*owner || pane->history_page_ || !pane->IsDrawn() || pane->LiveUpdateBusy() ||
              !pane->files_->GetBoundsInScreen().Contains(point) ||
              (pane->footer_shown_ && pane->footer_->GetBoundsInScreen().Contains(point))) return;
          for (const auto& [entry, button] : pane->visible_buttons_) {
            if (entry->directory || pane->Current().removed.contains(entry->path) ||
                (!entry->mime.starts_with("image/") && !IsHeicThumbnailPath(entry->path) &&
                 !entry->path.MatchesExtension(FILE_PATH_LITERAL(".svg")))) continue;
            auto row = pane->row_views_.find(entry->path);
            if (row != pane->row_views_.end() && row->second->GetBoundsInScreen().Contains(point)) {
              *owner = pane; *path = entry->path; return;
            }
          }
        }, point, &owner, &path));
    if (owner && hover_owner_.get() == owner && hover_path_ == path && GetBoundsInScreen() == hover_root_bounds_) return;
    if (!hover_path_.empty()) CloseHoverPreview();
    if (!owner) return;
    hover_owner_ = owner->weak_.GetWeakPtr(); hover_path_ = std::move(path);
    hover_root_bounds_ = GetBoundsInScreen();
    hover_cancel_ = std::make_shared<std::atomic_bool>(false);
    hover_wait_timer_.Start(FROM_HERE, base::Milliseconds(400), this, &FilesView::StartHoverPreview);
  }
  void StartHoverPreview() {
    if (!HoverPreviewValid()) { CloseHoverPreview(); return; }
    hover_ready_ = true;
    for (const auto& [entry, button] : hover_owner_->visible_buttons_)
      if (entry->path == hover_path_ && !hover_owner_->Current().visuals[entry->path].thumbnail.isNull()) { ShowHoverImage(hover_owner_->Current().visuals[entry->path].thumbnail); break; }
    LoadHoverPreview();
  }
  void LoadHoverPreview() {
    if (hover_decode_running_ || !hover_ready_ || !HoverPreviewValid()) return;
    hover_decode_running_ = true;
    const auto canceled = hover_cancel_;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&LoadCachedThumbnail, hover_path_, 720, canceled),
        base::BindOnce(&FilesView::HoverImageLoaded, weak_.GetWeakPtr(), canceled));
  }
  void HoverImageLoaded(CancelFlag canceled, gfx::ImageSkia image) {
    hover_decode_running_ = false;
    if (canceled != hover_cancel_ || Stopped(canceled)) { LoadHoverPreview(); return; }
    if (!HoverPreviewValid()) { CloseHoverPreview(); return; }
    if (!image.isNull()) ShowHoverImage(image);
  }
  void ShowHoverImage(const gfx::ImageSkia& image) {
    if (!HoverPreviewValid() || image.isNull() || image.width() <= 0 || image.height() <= 0) return;
    gfx::Rect area = GetLocalBounds(); area.Inset(8);
    gfx::Rect anchor = hover_owner_->row_views_.at(hover_path_)->GetBoundsInScreen();
    anchor.Offset(-hover_root_bounds_.x(), -hover_root_bounds_.y());
    // Choose the largest available side without covering the hovered row.
    // This also works for full-width list rows and short windows.
    const std::array rooms = {
      gfx::Rect(anchor.right() + 12, area.y(), std::max(0, area.right() - anchor.right() - 12), area.height()),
      gfx::Rect(area.x(), area.y(), std::max(0, anchor.x() - area.x() - 12), area.height()),
      gfx::Rect(area.x(), anchor.bottom() + 12, area.width(), std::max(0, area.bottom() - anchor.bottom() - 12)),
      gfx::Rect(area.x(), area.y(), area.width(), std::max(0, anchor.y() - area.y() - 12))};
    double scale = 0;
    size_t side = 0;
    for (size_t index = 0; index < rooms.size(); ++index) {
      if (rooms[index].width() < 48 || rooms[index].height() < 48) continue;
      const double fit = std::min(static_cast<double>(std::min(360, rooms[index].width() - 16)) / image.width(),
          static_cast<double>(std::min(280, rooms[index].height() - 16)) / image.height());
      if (fit > scale) { scale = fit; side = index; }
    }
    if (scale <= 0) return;
    const gfx::Size size(std::max(1, static_cast<int>(std::floor(image.width() * scale))),
                         std::max(1, static_cast<int>(std::floor(image.height() * scale))));
    const int w = size.width() + 16, h = size.height() + 16;
    const auto& room = rooms[side];
    gfx::Rect bounds(side < 2 ? (side == 0 ? room.x() : room.right() - w) :
        display::Screen::Get()->GetCursorScreenPoint().x() - hover_root_bounds_.x() - w / 2,
        side < 2 ? anchor.y() : (side == 2 ? room.y() : room.bottom() - h), w, h);
    bounds.AdjustToFit(room);
    if (bounds.Intersects(anchor)) return;
    if (!hover_panel_) {
      hover_panel_ = AddChildView(std::make_unique<views::View>());
      hover_panel_->SetProperty(views::kViewIgnoredByLayoutKey, true);
      hover_panel_->SetCanProcessEventsWithinSubtree(false);
      hover_panel_->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainerElevated, 12));
      hover_panel_->SetBorder(views::CreateRoundedRectBorder(1, 12, ui::kColorSysOutline));
      hover_panel_->SetPaintToLayer(); hover_panel_->layer()->SetFillsBoundsOpaquely(false);
      hover_panel_->SetVisible(false);
      hover_image_ = hover_panel_->AddChildView(std::make_unique<views::ImageView>());
      hover_image_->SetCornerRadius(6);
      hover_image_->GetViewAccessibility().SetIsIgnored(true);
    }
    const bool entering = !hover_panel_->GetVisible() || hover_hide_timer_.IsRunning();
    hover_hide_timer_.Stop(); hover_panel_->layer()->GetAnimator()->StopAnimating();
    hover_panel_->SetBoundsRect(bounds);
    hover_image_->SetBounds(8, 8, size.width(), size.height()); hover_image_->SetImageSize(size);
    hover_image_->SetImage(ui::ImageModel::FromImageSkia(image));
    ReorderChildView(hover_panel_, children().size() - 1);
    hover_panel_->SetVisible(true);
    if (entering) hover_panel_->layer()->SetOpacity(0.f);
    ui::ScopedLayerAnimationSettings settings(hover_panel_->layer()->GetAnimator());
    settings.SetTransitionDuration(base::Milliseconds(120)); settings.SetTweenType(gfx::Tween::EASE_OUT);
    hover_panel_->layer()->SetOpacity(1.f);
  }
  void SyncLiveDirectories() {
    if (root_ || tabs_.empty() || !GetWidget()) return;
    const bool active = GetWidget()->IsActive();
    const bool activated = active && !live_window_active_;
    live_window_active_ = active;
    std::set<base::FilePath> paths;
    VisitPanes(Current().pane_host, base::BindRepeating([](bool activated,
                                                        std::set<base::FilePath>* paths, FilesView* pane) {
      if (pane->history_page_ || pane->IsTagListing() || pane->tabs_.empty()) return;
      const auto& path = pane->Current().path;
      const bool visible = pane->IsDrawn() && pane->GetWidget()->IsVisible() &&
          !pane->GetWidget()->IsMinimized() && path.IsAbsolute() && !IsDrivesLocation(path)
#if BUILDFLAG(IS_MAC)
          && !IsCrFilesApplicationHiddenMac()
#endif
          ;
      if (visible) {
        paths->insert(path);
        if (!pane->live_visible_ || activated) pane->QueueLiveRead();
      } else if (pane->live_visible_) pane->CancelLiveRead();
      pane->live_visible_ = visible;
    }, activated, &paths));
    std::erase_if(live_watches_, [&paths](const auto& item) { return !paths.contains(item.first); });
    if (!paths.empty() && !live_watch_runner_)
      live_watch_runner_ = base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::USER_VISIBLE});
    for (const auto& path : paths) if (!live_watches_.contains(path)) {
      live_watches_.emplace(path, base::SequenceBound<LiveDirectoryWatch>(live_watch_runner_, path,
          base::BindPostTaskToCurrentDefault(base::BindRepeating(&FilesView::LiveDirectoryChanged, weak_.GetWeakPtr()))));
    }
  }
  void LiveDirectoryChanged(base::FilePath path) {
    if (tabs_.empty() || !live_watches_.contains(path)) return;
    CachedDirectories().Remove(path);
    VisitPanes(Current().pane_host, base::BindRepeating([](const base::FilePath& path, FilesView* pane) {
      if (!pane->history_page_ && pane->IsDrawn() && pane->Current().path == path) pane->QueueLiveRead();
    }, path));
  }
  void CancelLiveRead() {
    live_cancel_->store(true);
    DirectoryReads().Prune();
    live_cancel_ = std::make_shared<std::atomic_bool>(false);
    live_read_timer_.Stop(); live_read_pending_ = false; live_dirty_ = false; live_visible_ = false;
    live_dirty_since_ = {};
    last_file_scroll_ = {};
  }
  void QueueLiveRead() {
    if (IsTagListing()) return;
    if (live_dirty_since_.is_null()) live_dirty_since_ = base::TimeTicks::Now();
    live_dirty_ = true;
    if (!live_read_pending_ && !live_read_timer_.IsRunning())
      live_read_timer_.Start(FROM_HERE, base::Milliseconds(50), this, &FilesView::ReadLiveDirectory);
  }
  bool LiveUpdateBusy() {
    return Current().loading || order_pending_ || root_->local_operation_busy_ || trash_busy_ ||
        !dragging_rows_.empty() || (GetWidget() && GetWidget()->HasCapture()) ||
        (context_runner_ && context_runner_->IsRunning()) || more_menu_open_ || new_folder_widget_ || options_widget_ ||
        std::ranges::any_of(row_views_, [](const auto& item) { return item.second->GetState() == views::Button::STATE_PRESSED; });
  }
  void ReadLiveDirectory() {
    if (!root_ || history_page_ || !IsDrawn() || !files_->contents()) return;
    if (LiveUpdateBusy()) {
      live_read_timer_.Start(FROM_HERE, base::Milliseconds(200), this, &FilesView::ReadLiveDirectory);
      return;
    }
    const auto now = base::TimeTicks::Now();
    if (!last_file_scroll_.is_null() && !live_dirty_since_.is_null() &&
        now - last_file_scroll_ < base::Milliseconds(120) && now - live_dirty_since_ < base::Milliseconds(750)) {
      const auto delay = std::min({base::Milliseconds(50),
          base::Milliseconds(120) - (now - last_file_scroll_),
          base::Milliseconds(750) - (now - live_dirty_since_)});
      live_read_timer_.Start(FROM_HERE, delay, this, &FilesView::ReadLiveDirectory);
      return;
    }
    live_dirty_since_ = {};
    live_dirty_ = false; live_read_pending_ = true;
    const auto& tab = Current();
    DirectoryReads().Request(tab.path, tab.options.sort, tab.options.show_hidden, live_cancel_,
        base::BindOnce(&FilesView::LiveDirectoryRead, weak_.GetWeakPtr(), live_cancel_, tab.request, tab.path));
  }
  void LiveDirectoryRead(CancelFlag token, uint64_t request, base::FilePath path, DirectoryResult result) {
    if (token != live_cancel_ || Stopped(token)) return;
    live_read_pending_ = false;
    if (!IsDrawn() || Current().request != request || Current().path != path) return;
    if (LiveUpdateBusy() ||
        (result.ok && !result.Current() && (!result.epoch || result.epoch->writers.load() == 0))) {
      QueueLiveRead(); return;
    }
    if (result.ok && (result.sort != Current().options.sort || result.hidden != Current().options.show_hidden)) {
      // Shared reads may serve panes with different view options. Sort their
      // metadata on the worker instead of repeatedly reading the same folder.
      live_read_pending_ = true;
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::TaskPriority::USER_VISIBLE},
          base::BindOnce([](DirectoryResult data, FileSort sort, bool hidden, CancelFlag canceled) {
            data.order = SortDirectory(SortRecords(data.entries, canceled), sort, hidden, data.path == TrashRoot(), canceled);
            data.sort = sort; data.hidden = hidden; data.delta.reset(); return data;
          }, std::move(result), Current().options.sort, Current().options.show_hidden, token),
          base::BindOnce(&FilesView::LiveDirectoryRead, weak_.GetWeakPtr(), token, request, path));
      return;
    }
    if (!result.delta || result.base_generation != Current().metadata_generation) {
      live_read_pending_ = true;
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&CompareDirectory, Current().entries, Current().removed, Current().metadata_generation, std::move(result), token),
          base::BindOnce(&FilesView::LiveDirectoryRead, weak_.GetWeakPtr(), token, request, path));
      return;
    }
    ApplyLiveDirectory(std::move(result));
    if (live_dirty_) QueueLiveRead();
  }
  void ApplyLiveDirectory(DirectoryResult result) {
    auto& tab = Current();
    CachedDirectories().Put(result);
    if (result.ok && result.delta->equal) {
      tab.error = false; UpdateStatus(); return;
    }
    tab.error = !result.ok;
    if (!result.ok) { result.entries.clear(); result.order.clear(); CachedDirectories().Remove(tab.path); }
    CancelFileVisuals(); virtual_rows_timer_.Stop(); SaveScroll();
    auto* layout = static_cast<FileLayout*>(files_->contents());
    base::FilePath anchor_path, focus_path, keyboard_path, selection_anchor_path;
    const auto viewport = files_->GetVisibleRect();
    const auto first = layout->ItemsInRect(viewport).first;
    int anchor_offset = 0;
    if (first < displayed_entries_.size()) {
      anchor_path = tab.entries[displayed_entries_[first]].path;
      anchor_offset = viewport.y() - layout->ItemBounds(first).y();
    }
    auto* focused = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
    for (const auto& [path, row] : row_views_) if (focused && row->Contains(focused)) focus_path = path;
    if (keyboard_position_ && *keyboard_position_ < displayed_entries_.size()) keyboard_path = tab.entries[displayed_entries_[*keyboard_position_]].path;
    if (keyboard_anchor_ && *keyboard_anchor_ < displayed_entries_.size()) selection_anchor_path = tab.entries[displayed_entries_[*keyboard_anchor_]].path;
    const auto& positions = result.delta->positions;
    const auto& unchanged = result.delta->unchanged;
    std::erase_if(tab.visuals, [&](const auto& item) { return !unchanged.contains(item.first); });
    std::erase_if(tab.failed_thumbnails, [&](const auto& path) { return !unchanged.contains(path); });
    std::vector<base::FilePath> remove;
    for (const auto& [path, row] : row_views_) if (!unchanged.contains(path)) remove.push_back(path);
    for (const auto& path : remove) {
      if (path == focus_path && GetWidget() && GetWidget()->IsActive()) files_->RequestFocus();
    }
    RemoveFileRows(remove);
    // Retained row callbacks use paths, but visual work keeps Entry pointers.
    // Rebind every pointer before destroying the old model storage.
    for (auto& [entry, button] : visible_buttons_) entry = &result.entries[positions.at(entry->path)];
    std::erase_if(tab.selected, [&positions](const auto& path) { return !positions.contains(path); });
    tab.entries = std::move(result.entries); tab.removed.clear(); ++tab.metadata_generation; tab.sorted_entries = result.order.values();
    tab.sorted_mode = tab.options.sort; tab.sorted_hidden = tab.options.show_hidden;
    RebuildDisplayedEntries(result.delta.get()); layout->SetItemCount(displayed_entries_.size());
    if (auto position = result.delta->DisplayPosition(anchor_path))
      tab.scroll = std::max(0, layout->ItemBounds(*position).y() + anchor_offset);
    keyboard_position_ = result.delta->DisplayPosition(keyboard_path);
    keyboard_anchor_ = result.delta->DisplayPosition(selection_anchor_path);
    for (const auto& [path, row] : row_views_)
      if (auto position = result.delta->DisplayPosition(path)) layout->PlaceItem(*position, row);
    // Restore geometry and scroll before materializing the new visible range.
    layout->OrderItems();
    pending_scroll_ = tab.scroll; files_->InvalidateLayout(); files_->DeprecatedLayoutImmediately();
    UpdateVirtualRows();
    if (!focus_path.empty() && GetWidget() && GetWidget()->IsActive()) {
      if (auto row = row_views_.find(focus_path); row != row_views_.end()) row->second->RequestFocus();
    }
    UpdateStatus(); root_->ScheduleWorkspaceSave();
  }
  void PollFooterHover() {
    if (root_ || tabs_.empty() || !display::Screen::Get()) return;
    if (IsCrFilesWindowBlockedByModal(GetWidget())) {
      if (!modal_hover_blocked_) {
        modal_hover_blocked_ = true;
        auto* root = GetWidget()->GetRootView();
        modal_previous_mouse_input_ = root->GetCanProcessEventsWithinSubtree();
        root->SetCanProcessEventsWithinSubtree(false);
        GetWidget()->ReleaseCapture();
        CloseHoverPreview(false);
        // Clear hover left behind when AppKit begins a sheet without sending
        // a mouse-exit event to the parent Views controls.
        const auto clear_hover = [](auto&& self, views::View* view) -> void {
          if (auto* button = views::AsViewClass<views::Button>(view);
              button && button->GetState() == views::Button::STATE_HOVERED)
            button->SetState(views::Button::STATE_NORMAL);
          for (views::View* child : view->children()) self(self, child);
        };
        clear_hover(clear_hover, GetWidget()->GetRootView());
        VisitPanes(Current().pane_host, base::BindRepeating([](FilesView* pane) {
          if (!pane->footer_hovered_) return;
          pane->footer_hovered_ = false;
          pane->UpdateFooterVisibility();
        }));
        fullscreen_hide_timer_.Stop();
        fullscreen_shown_ = false;
        fullscreen_exit_->SetVisible(false);
      }
      return;
    }
    if (modal_hover_blocked_) {
      GetWidget()->GetRootView()->SetCanProcessEventsWithinSubtree(modal_previous_mouse_input_);
      modal_hover_blocked_ = false;
    }
    const auto cursor = display::Screen::Get()->GetCursorScreenPoint();
    // Screen coordinates alone also hit panes underneath a bubble. Preserve
    // their hover state while the pointer belongs to an owned popup instead.
    if (GetWidget()) {
      for (views::Widget* popup : views::Widget::GetAllOwnedWidgets(GetWidget()->GetNativeView())) {
        if (popup->IsVisible() && popup->GetWindowBoundsInScreen().Contains(cursor)) return;
      }
    }
    PollImageHover(cursor);
    VisitPanes(Current().pane_host, base::BindRepeating([](gfx::Point point, FilesView* pane) {
      if (pane->history_page_ || !pane->footer_) return;
      // Hover anywhere in either an active or inactive pane reveals its footer.
      // Use the fixed pane bounds, never the animated footer or event target.
      // A button becoming visible under the cursor cannot change this region.
      const bool hovered = pane->IsDrawn() && pane->GetBoundsInScreen().Contains(point);
      if (pane->footer_hovered_ == hovered) return;
      pane->footer_hovered_ = hovered;
      pane->UpdateFooterVisibility();
    }, cursor));
  }
  void UpdateFooterVisibility() {
    if (!root_ || !footer_) return;
    if (network_page_) {
      footer_hide_timer_.Stop(); footer_expired_ = false;
      footer_shown_ = network_footer_visible_; footer_restore_pending_ = false;
      footer_->SetVisible(network_footer_visible_);
      footer_->SetCanProcessEventsWithinSubtree(network_footer_visible_);
      auto* layer = footer_->layer(); layer->GetAnimator()->StopAnimating();
      layer->SetTransform(gfx::Transform()); layer->SetOpacity(network_footer_visible_ ? 1.f : 0.f);
      return;
    }
    if (history_page_ || tag_id_ == 0) {
      footer_hide_timer_.Stop(); footer_shown_ = false; footer_restore_pending_ = true;
      footer_->SetVisible(false); return;
    }
    const bool active = root_->ActivePane() == this;
    if (footer_active_ != active) {
      footer_active_ = active; footer_expired_ = false; footer_hide_timer_.Stop();
      if (active) footer_hide_timer_.Start(FROM_HERE, base::Seconds(8), base::BindOnce([](base::WeakPtr<FilesView> pane) {
        if (!pane) return;
        pane->footer_expired_ = true; pane->UpdateFooterVisibility();
      }, weak_.GetWeakPtr()));
    }
    constexpr int kMinimumFooterWidth = 280;
    const bool fits = footer_->parent()->width() >= kMinimumFooterWidth;
    footer_->SetVisible(fits);
    const bool show = fits && (Current().path == TrashRoot() || !GetFileViewPreferences().auto_hide_toolbar || footer_hovered_ || (active && !footer_expired_) || new_folder_widget_ || more_menu_open_);
    if (footer_shown_ == show && !footer_restore_pending_) return;
    footer_shown_ = show;
    footer_->SetCanProcessEventsWithinSubtree(show);
    auto* layer = footer_->layer();
    layer->GetAnimator()->StopAnimating();
    if (footer_restore_pending_) {
      // Tab restoration settles even an interrupted animation whose logical
      // visibility already matches. Keep this mode through the first layout.
      ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
      settings.SetTransitionDuration(base::TimeDelta());
      settings.SetPreemptionStrategy(ui::LayerAnimator::IMMEDIATELY_SET_NEW_TARGET);
      gfx::Transform offset; if (!show) offset.Translate(0, 6);
      layer->SetTransform(offset); layer->SetOpacity(show ? 1.f : 0.f);
      return;
    }
    const bool reveal_capsule = show && Current().path == TrashRoot();
    if (reveal_capsule) {
      auto initial = CenteredScale(footer_, 0.96f);
      initial.Translate(0, 8);
      layer->SetTransform(initial); layer->SetOpacity(0.f);
    }
    ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
    settings.SetTransitionDuration(base::Milliseconds(reveal_capsule ? 220 : 160));
    settings.SetTweenType(gfx::Tween::EASE_OUT);
    settings.SetPreemptionStrategy(ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
    gfx::Transform offset; if (!show) offset.Translate(0, 6);
    layer->SetTransform(offset); layer->SetOpacity(show ? 1.f : 0.f);
    // The overlay never participates in file viewport layout.
  }
  views::View* CreateDropOverlay() {
    auto* overlay = AddChildView(std::make_unique<views::View>());
    overlay->SetProperty(views::kViewIgnoredByLayoutKey, true);
    overlay->SetCanProcessEventsWithinSubtree(false);
    overlay->SetBorder(views::CreateSolidBorder(2, ui::kColorSysPrimary));
    overlay->SetPaintToLayer(); overlay->layer()->SetFillsBoundsOpaquely(false);
    overlay->layer()->SetOpacity(0.f);
    return overlay;
  }
  void ShowSplitPreview(const FolderSplitDrop& split) {
    if (!split_preview_) {
      split_preview_ = CreateDropOverlay();
      split_preview_->SetBackground(std::make_unique<SelectionMarqueeBackground>());
      split_preview_animator_ = std::make_unique<views::BoundsAnimator>(this);
      split_preview_animator_->AddObserver(this);
      split_preview_animator_->SetAnimationDuration(base::Milliseconds(160));
      split_preview_animator_->set_tween_type(gfx::Tween::EASE_OUT);
    }
    const bool animate = gfx::Animation::ShouldRenderRichAnimation() && !gfx::Animation::PrefersReducedMotion();
    if (split_preview_shown_ && split_preview_animator_->GetTargetBounds(split_preview_) == split.preview &&
        (animate || !split_preview_animator_->IsAnimating(split_preview_))) return;
    if (!split_preview_shown_)
      root_->file_drag_cursor_observer_->SetPreviewCleanup(base::BindOnce(&FilesView::HideSplitPreview, weak_.GetWeakPtr()));
    if (!split_hit_area_) {
      split_hit_area_ = split.hit_area;
      // The FilesView remains the full-size drop target while the body reflows.
      pane_body_->SetProperty(views::kViewIgnoredByLayoutKey, true);
      if (animate) {
        // View bounds use the same mirrored layout coordinates as SplitPane.
        gfx::Rect strip = split.preview;
        if (split.direction < 2) {
          if (split.direction == 1) strip.set_x(strip.right() - 2);
          strip.set_width(2);
        } else {
          if (split.direction == 3) strip.set_y(strip.bottom() - 2);
          strip.set_height(2);
        }
        split_preview_->SetBoundsRect(strip);
      }
    }
    split_preview_shown_ = true;
    split_preview_->layer()->GetAnimator()->StopAnimating();
    split_preview_->layer()->SetOpacity(1.f);
    if (animate) {
      // On an edge change, continue from the currently displayed rectangle.
      split_preview_animator_->AnimateViewTo(split_preview_, split.preview);
      split_preview_animator_->AnimateViewTo(pane_body_, split.original);
    } else {
      split_preview_animator_->Cancel();
      split_preview_->SetBoundsRect(split.preview);
      pane_body_->SetBoundsRect(split.original);
      LayoutPaneFooter();
    }
  }
  void HideSplitPreview() {
    if (!split_preview_shown_) return;
    split_preview_shown_ = false;
    split_preview_animator_->StopAnimatingView(split_preview_);
    auto* layer = split_preview_->layer();
    ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
    const bool animate = gfx::Animation::ShouldRenderRichAnimation() && !gfx::Animation::PrefersReducedMotion();
    settings.SetTransitionDuration(animate ? base::Milliseconds(90) : base::TimeDelta());
    settings.SetTweenType(gfx::Tween::EASE_OUT);
    layer->SetOpacity(0.f);
    if (animate) split_preview_animator_->AnimateViewTo(pane_body_, GetLocalBounds());
    else ResetSplitPreview();
  }
  void OnBoundsAnimatorProgressed(views::BoundsAnimator*) override {
    if (split_hit_area_) LayoutPaneFooter();
  }
  void OnBoundsAnimatorDone(views::BoundsAnimator*) override {
    if (!split_preview_shown_ && split_hit_area_) {
      split_hit_area_.reset();
      pane_body_->SetProperty(views::kViewIgnoredByLayoutKey, false);
    }
  }
  void ResetSplitPreview() {
    if (!split_preview_) return;
    split_hit_area_.reset();
    split_preview_shown_ = false;
    split_preview_animator_->Cancel();
    split_preview_->layer()->GetAnimator()->StopAnimating();
    split_preview_->layer()->SetOpacity(0.f);
    pane_body_->SetProperty(views::kViewIgnoredByLayoutKey, false);
    InvalidateLayout();
  }
  void OnBoundsChanged(const gfx::Rect& previous) override {
    if (split_hit_area_ && previous.size() != size()) ResetSplitPreview();
  }
  void SetDropHighlight(bool show, gfx::Rect bounds = {}) {
    if (!root_) return;
    if (show) {
      drop_highlight_bounds_ = bounds;
      if (drop_highlight_) drop_highlight_->SetBoundsRect(bounds);
    }
    if (!show && !drop_highlight_shown_) return;
    if (!drop_highlight_) drop_highlight_ = CreateDropOverlay();
    if (show == drop_highlight_shown_) return;
    drop_highlight_shown_ = show;
    drop_highlight_->SetBoundsRect(drop_highlight_bounds_);
    auto* layer = drop_highlight_->layer(); layer->GetAnimator()->StopAnimating();
    ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
    settings.SetTransitionDuration(base::Milliseconds(140)); settings.SetTweenType(gfx::Tween::EASE_OUT);
    layer->SetOpacity(show ? 1.f : 0.f);
  }
  void SetFileDragPreview(ui::OSExchangeData* data) {
    if (Current().selected.empty()) return;
    const auto& path = *Current().selected.begin();
    const auto entry = std::ranges::find(Current().entries, path, &Entry::path);
    auto* colors = GetColorProvider();
    constexpr float kDragPreviewScale = 1.12f;
    gfx::Canvas canvas(gfx::Size(static_cast<int>(std::ceil(280 * kDragPreviewScale)),
                                static_cast<int>(std::ceil(88 * kDragPreviewScale))), 1.f, false);
    canvas.Scale(kDragPreviewScale, kDragPreviewScale);
    cc::PaintFlags flags; flags.setAntiAlias(true);
    const int stack = std::min<size_t>(3, Current().selected.size());
    for (int i = stack - 1; i >= 0; --i) {
      flags.setStyle(cc::PaintFlags::kFill_Style);
      flags.setColor(colors->GetColor(i ? ui::kColorSysSurfaceVariant : ui::kColorSysBaseContainerElevated));
      canvas.DrawRoundRect(gfx::Rect(4 + i * 4, 4 + i * 5, 260, 64), 10, flags);
      flags.setStyle(cc::PaintFlags::kStroke_Style); flags.setStrokeWidth(1);
      flags.setColor(colors->GetColor(ui::kColorSysPrimary));
      canvas.DrawRoundRect(gfx::Rect(4 + i * 4, 4 + i * 5, 260, 64), 10, flags);
    }
    if (entry != Current().entries.end()) {
      const bool suppress_text = IsTextThumbnailPath(entry->path) &&
          (!Current().options.icons || kFileIconSizes[Current().options.icon_index] <= 48);
      auto icon = suppress_text || Current().visuals[entry->path].thumbnail.isNull() ? FileImage(*entry).Rasterize(colors) : Current().visuals[entry->path].thumbnail;
      if (!icon.isNull() && icon.width() > 0 && icon.height() > 0) {
        const float scale = 40.f / std::max(icon.width(), icon.height());
        const gfx::Size size(std::max(1, static_cast<int>(icon.width() * scale)), std::max(1, static_cast<int>(icon.height() * scale)));
        icon = gfx::ImageSkiaOperations::CreateResizedImage(icon, skia::ImageOperations::RESIZE_BEST, size);
        canvas.DrawImageInt(icon, 16 + (40 - size.width()) / 2, 16 + (40 - size.height()) / 2);
      }
    }
    canvas.DrawStringRectWithFlags(path.BaseName().AsUTF16Unsafe(), gfx::FontList(),
        colors->GetColor(ui::kColorSysOnSurface), gfx::Rect(68, 12, 184, 24), gfx::Canvas::NO_SUBPIXEL_RENDERING);
    canvas.DrawStringRectWithFlags(base::NumberToString16(Current().selected.size()) + u" " + Text(IDS_CRFILES_ITEMS),
        gfx::FontList(), colors->GetColor(ui::kColorSysOnSurface), gfx::Rect(68, 36, 184, 20), gfx::Canvas::NO_SUBPIXEL_RENDERING);
    const int hotspot = static_cast<int>(std::lround(20 * kDragPreviewScale));
    data->provider().SetDragImage(gfx::ImageSkia::CreateFrom1xBitmap(canvas.GetBitmap()), gfx::Vector2d(hotspot, hotspot));
  }
  void StartFileDrag() {
    FinishFileDrag();
    base::FilePath folder;
    if (Current().selected.size() == 1 && !IsTagListing() && Current().path != TrashRoot() &&
        !TrashRoot().IsParent(Current().path)) {
      const auto& path = *Current().selected.begin();
      const auto entry = std::ranges::find(Current().entries, path, &Entry::path);
      if (entry != Current().entries.end() && entry->directory && !entry->package && !Current().removed.contains(path))
        folder = path;
    }
    root_->file_drag_cursor_observer_ = std::make_unique<FileDragCursorObserver>(GetWidget(), std::move(folder));
    for (const auto& path : Current().selected) {
      auto found = row_views_.find(path); if (found == row_views_.end()) continue;
      auto* row = found->second.get();
      row->StopReflowAnimation();
      row->SetPaintToLayer(); row->layer()->SetFillsBoundsOpaquely(false);
      dragging_rows_.push_back(row->GetWeakPtr());
      auto* layer = row->layer(); layer->GetAnimator()->StopAnimating();
      ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
      settings.SetTransitionDuration(base::Milliseconds(140)); settings.SetTweenType(gfx::Tween::EASE_OUT);
      layer->SetTransform(CenteredScale(row, 0.97f)); layer->SetOpacity(0.55f);
    }
  }
  void FinishFileDrag() {
    for (const auto& row : dragging_rows_) {
      if (!row || !row->layer()) continue;
      auto* layer = row->layer(); layer->GetAnimator()->StopAnimating();
      ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
      settings.SetTransitionDuration(base::Milliseconds(160)); settings.SetTweenType(gfx::Tween::EASE_OUT);
      layer->SetTransform(gfx::Transform()); layer->SetOpacity(1.f);
    }
    dragging_rows_.clear();
    FileViewportChanged();
  }

  Tab& Current() { return tabs_[active_]; }
  const Tab& Current() const { return tabs_[active_]; }
  std::string PaneName() const {
    if (network_page_) {
      const auto location = network_view_ ? ParseSmbLocation(NetworkPageUrl(network_view_)) : std::nullopt;
      return location ? location->server : base::UTF16ToUTF8(Text(IDS_CRFILES_NETWORK));
    }
    if (tag_id_ > 0 && !IsTagListing()) return base::UTF16ToUTF8(EntryName(Current().path));
    if (tag_id_ >= 0) return base::UTF16ToUTF8(Text(IDS_CRFILES_TAGS)) + (tag_id_ > 0 ? " · " + tag_name_ : "");
    if (search_page_) return base::UTF16ToUTF8(Text(IDS_CRFILES_SEARCH) + u" · " +
        (search_scope_.title.empty() ? EntryName(Current().path) : search_scope_.title));
    if (history_page_) return base::UTF16ToUTF8(Text(IDS_CRFILES_HISTORY));
    if (!pane_name_.empty()) return pane_name_;
    if (!tabs_.empty() && !EntryName(tabs_[active_].path).empty())
      return base::UTF16ToUTF8(EntryName(tabs_[active_].path));
    return base::UTF16ToUTF8(Text(IDS_CRFILES_FOLDER));
  }
  void ScheduleWorkspaceSave() {
    if (root_) { root_->ScheduleWorkspaceSave(); return; }
    if (restoring_workspace_) return;
    workspace_timer_.Start(FROM_HERE, base::Milliseconds(600), this, &FilesView::SaveWorkspace);
  }
  base::DictValue SerializePane(views::View* view) {
    base::DictValue result;
    if (auto* split = views::AsViewClass<PaneSplit>(view)) {
      result.Set("vertical", split->vertical()); result.Set("ratio", split->ratio());
      result.Set("first", SerializePane(split->first())); result.Set("second", SerializePane(split->second()));
    } else if (auto* pane = views::AsViewClass<FilesView>(view)) {
      pane->SaveScroll();
      const auto& tab = pane->Current(); const auto& options = tab.options;
      if (pane->tag_id_ >= 0) { result.Set("tag_id", base::NumberToString(pane->tag_id_)); result.Set("tag_name", pane->tag_name_); result.Set("tag_root", pane->tag_directory_root_.AsUTF8Unsafe()); }
      if (pane->search_scope_.tag_id > 0) {
        result.Set("search_tag", base::NumberToString(pane->search_scope_.tag_id));
        result.Set("search_scope_title", base::UTF16ToUTF8(pane->search_scope_.title));
      }
      result.Set("network_page", pane->network_page_);
      result.Set("history_page", pane->history_page_); result.Set("search_page", pane->search_page_);
      result.Set("id", pane->pane_id_); result.Set("name", pane->pane_name_);
      result.Set("path", tab.path.AsUTF8Unsafe()); result.Set("scroll", pane->restored_scroll_.value_or(tab.scroll));
      result.Set("icons", options.icons); result.Set("icon_index", options.icon_index);
      result.Set("sort", static_cast<int>(options.sort)); result.Set("hidden", options.show_hidden);
      result.Set("size_column", options.size_column); result.Set("type_column", options.type_column);
      result.Set("modified_column", options.modified_column);
      result.Set("created_column", options.created_column);
      result.Set("permissions_column", options.permissions_column);
      base::ListValue widths; for (int width : options.column_widths) widths.Append(width);
      result.Set("widths", std::move(widths));
    }
    return result;
  }
  void SaveWorkspace() {
    if (root_ || restoring_workspace_ || tabs_.empty()) return;
    base::DictValue workspace; workspace.Set("version", 1); workspace.Set("active", static_cast<int>(active_));
    workspace.Set("sidebar", sidebar_visible_); workspace.Set("sidebar_width", sidebar_width_);
    base::ListValue shares;
    for (const auto& target : shared_directories_) {
      base::DictValue item;
      item.Set("id", target.id); item.Set("name", target.name);
      item.Set("path", target.directory.AsUTF8Unsafe());
      if (!target.network_url.empty()) item.Set("network_url", target.network_url);
      if (target.tag_id > 0) item.Set("tag_id", base::NumberToString(target.tag_id));
      shares.Append(std::move(item));
    }
    workspace.Set("shared_directories", std::move(shares));
    base::ListValue tabs;
    for (const auto& tab : tabs_) {
      if (tab.deferred_tree) {
        auto tree = tab.deferred_tree->Clone();
        ClearSavedShares(tree);
        base::DictValue item; item.Set("tree", std::move(tree));
        item.Set("pinned", tab.pinned);
        item.Set("active_pane", tab.deferred_active); tabs.Append(std::move(item)); continue;
      }
      if (!tab.pane_host || tab.pane_host->children().empty()) continue;
      base::DictValue item;
      item.Set("tree", SerializePane(tab.pane_host->children().front().get()));
      item.Set("pinned", tab.pinned);
      item.Set("active_pane", tab.active_pane->pane_id_); tabs.Append(std::move(item));
    }
    workspace.Set("tabs", std::move(tabs));
    if (!SetCrFilesWorkspace(std::move(workspace), base::BindOnce([](base::WeakPtr<FilesView> view, bool saved) {
      if (!saved && view && view->ActivePane()) view->ActivePane()->status_->SetText(Text(IDS_CRFILES_CONFIG_ERROR));
    }, weak_.GetWeakPtr())) && ActivePane())
      ActivePane()->status_->SetText(Text(IDS_CRFILES_CONFIG_ERROR));
  }
  std::unique_ptr<views::View> RestorePane(const base::DictValue& data, size_t depth, size_t& leaves,
                                          std::set<std::string>& ids, const std::string& active_id) {
    if (depth > 10 || leaves >= 128) return nullptr;
    if (const auto* first = data.FindDict("first")) {
      const auto* second = data.FindDict("second"); if (!second) return nullptr;
      auto left = RestorePane(*first, depth + 1, leaves, ids, active_id);
      auto right = RestorePane(*second, depth + 1, leaves, ids, active_id);
      if (!left || !right) return nullptr;
      auto split = std::make_unique<PaneSplit>(data.FindBool("vertical").value_or(false), std::move(left), std::move(right));
      const double ratio = data.FindDouble("ratio").value_or(0.5);
      split->SetRatio(std::isfinite(ratio) ? ratio : 0.5);
      split->SetChangedCallback(base::BindRepeating(&FilesView::ScheduleWorkspaceSave, weak_.GetWeakPtr()));
      split->SetResizeCallback(base::BindRepeating(&FilesView::SplitResizeChanged, weak_.GetWeakPtr()));
      return split;
    }
    if (const auto* saved_tag = data.FindString("tag_id")) {
      int64_t id;
      if (!base::StringToInt64(*saved_tag, &id) || id < 0) return nullptr;
      ++leaves;
      const auto* name = data.FindString("tag_name");
      auto pane = std::make_unique<FilesView>(this, base::FilePath(), false, false, false, id, name ? *name : "");
      if (const auto* saved_id = data.FindString("id"); saved_id && base::Uuid::ParseLowercase(*saved_id).is_valid() && ids.insert(*saved_id).second)
        pane->pane_id_ = *saved_id;
      auto& options = pane->Current().options;
      options.icons = data.FindBool("icons").value_or(true);
      options.icon_index = std::clamp(data.FindInt("icon_index").value_or(1), 0, static_cast<int>(kFileIconSizes.size()) - 1);
      options.sort = static_cast<FileSort>(std::clamp(
          data.FindInt("sort").value_or(static_cast<int>(options.sort)), 0, 11));
      options.show_hidden = data.FindBool("hidden").value_or(false);
      options.size_column = data.FindBool("size_column").value_or(true);
      options.type_column = data.FindBool("type_column").value_or(true);
      options.modified_column = data.FindBool("modified_column").value_or(true);
      options.created_column = data.FindBool("created_column").value_or(false);
      options.permissions_column = data.FindBool("permissions_column").value_or(false);
      if (const auto* widths = data.FindList("widths"); widths)
        for (size_t i = 0; i < std::min(widths->size(), options.column_widths.size()); ++i) options.column_widths[i] = std::clamp((*widths)[i].GetIfInt().value_or(options.column_widths[i]), 60, 1000);
      if (id > 0) {
        const auto* saved_root = data.FindString("tag_root");
        const auto* saved_path = data.FindString("path");
        if (saved_root && saved_path) {
          auto directory_root = base::FilePath::FromUTF8Unsafe(*saved_root);
          auto directory = base::FilePath::FromUTF8Unsafe(*saved_path);
          if (directory_root.IsAbsolute() && !directory_root.ReferencesParent() &&
              !directory.ReferencesParent() && (directory_root == directory || directory_root.IsParent(directory)))
            pane->NavigateTag({id, pane->tag_name_, directory_root, directory});
        }
      }
      pane->restored_scroll_ = std::max(0, data.FindInt("scroll").value_or(0));
      return pane;
    }
    if (data.FindBool("network_page").value_or(false)) {
      ++leaves;
      auto pane = std::make_unique<FilesView>(this, base::FilePath(), false, false, false, -1, std::string(), SearchScope(), true);
      auto& options = pane->Current().options;
      options.icons = data.FindBool("icons").value_or(true);
      options.icon_index = std::clamp(data.FindInt("icon_index").value_or(1), 0, static_cast<int>(kFileIconSizes.size()) - 1);
      options.show_hidden = data.FindBool("hidden").value_or(false);
      options.size_column = data.FindBool("size_column").value_or(true);
      options.type_column = data.FindBool("type_column").value_or(true);
      options.modified_column = data.FindBool("modified_column").value_or(true);
      options.created_column = data.FindBool("created_column").value_or(false);
      options.sort = static_cast<FileSort>(std::clamp(data.FindInt("sort").value_or(2), 0, 9));
      SetNetworkPageLayout(pane->network_view_, options.icons, kFileIconSizes[options.icon_index], options.show_hidden, options.NetworkColumnOptions(), static_cast<int>(options.sort));
      return pane;
    }
    if (data.FindBool("search_page").value_or(false)) {
      const auto* saved = data.FindString("path");
      if (!saved) return nullptr;
      const auto folder = base::FilePath::FromUTF8Unsafe(*saved);
      if (!folder.IsAbsolute() || folder.ReferencesParent()) return nullptr;
      SearchScope scope;
      if (const auto* tag = data.FindString("search_tag")) {
        if (!base::StringToInt64(*tag, &scope.tag_id) || scope.tag_id <= 0) return nullptr;
        scope.multiple = true;
        const auto* title = data.FindString("search_scope_title");
        scope.title = title ? base::UTF8ToUTF16(*title) : Text(IDS_CRFILES_TAGS);
      }
      ++leaves; return std::make_unique<FilesView>(this, folder, false, false, true, -1, std::string(), std::move(scope));
    }
    if (data.FindBool("history_page").value_or(false)) { ++leaves; return std::make_unique<FilesView>(this, base::FilePath(), true); }
    const auto* path = data.FindString("path"); if (!path) return nullptr;
    const auto folder = base::FilePath::FromUTF8Unsafe(*path);
    if ((!folder.IsAbsolute() && !IsDrivesLocation(folder)) || folder.ReferencesParent()) return nullptr;
    ++leaves;
    auto pane = std::make_unique<FilesView>(this, folder, false, true);
    if (const auto* id = data.FindString("id"); id && base::Uuid::ParseLowercase(*id).is_valid() && ids.insert(*id).second)
      pane->pane_id_ = *id;
    if (const auto* name = data.FindString("name"); name && ValidPaneName(*name)) pane->pane_name_ = *name;
    auto& options = pane->Current().options;
    options.icons = data.FindBool("icons").value_or(true);
    options.icon_index = std::clamp(data.FindInt("icon_index").value_or(1), 0, static_cast<int>(kFileIconSizes.size()) - 1);
    options.sort = static_cast<FileSort>(std::clamp(
        data.FindInt("sort").value_or(static_cast<int>(options.sort)), 0, 11));
    options.show_hidden = data.FindBool("hidden").value_or(false);
    options.size_column = data.FindBool("size_column").value_or(true);
    options.type_column = data.FindBool("type_column").value_or(true);
    options.modified_column = data.FindBool("modified_column").value_or(true);
    options.created_column = data.FindBool("created_column").value_or(false);
    options.permissions_column = data.FindBool("permissions_column").value_or(false);
    if (const auto* widths = data.FindList("widths"); widths)
      for (size_t i = 0; i < std::min(widths->size(), options.column_widths.size()); ++i) options.column_widths[i] = std::clamp((*widths)[i].GetIfInt().value_or(options.column_widths[i]), 60, 1000);
    pane->restored_scroll_ = std::max(0, data.FindInt("scroll").value_or(0));
    pane->UpdateShareButton();
    return pane;
  }
  bool ValidateSavedTree(const base::DictValue& data, size_t depth, size_t& leaves,
                         std::set<std::string>& ids) {
    if (depth > 10 || leaves >= 128) return false;
    if (const auto* first = data.FindDict("first")) {
      const auto* second = data.FindDict("second");
      return second && ValidateSavedTree(*first, depth + 1, leaves, ids) && ValidateSavedTree(*second, depth + 1, leaves, ids);
    }
    ++leaves;
    if (const auto* saved_tag = data.FindString("tag_id")) {
      int64_t tag_id;
      if (!base::StringToInt64(*saved_tag, &tag_id) || tag_id < 0) return false;
      if (const auto* id = data.FindString("id"); id && base::Uuid::ParseLowercase(*id).is_valid()) return ids.insert(*id).second;
      return true;
    }
    if (data.FindBool("history_page").value_or(false)) return true;
    const auto* path = data.FindString("path");
    if (!path) return false;
    const auto folder = base::FilePath::FromUTF8Unsafe(*path);
    if ((!folder.IsAbsolute() && !IsDrivesLocation(folder)) || folder.ReferencesParent()) return false;
    if (const auto* id = data.FindString("id"); id && base::Uuid::ParseLowercase(*id).is_valid()) return ids.insert(*id).second;
    return true;
  }
  const base::DictValue* SavedActivePane(const base::DictValue& data, const std::string& id) {
    if (const auto* first = data.FindDict("first")) {
      const auto* second = data.FindDict("second");
      if (auto* found = SavedActivePane(*first, id)) return found;
      return second ? SavedActivePane(*second, id) : nullptr;
    }
    const auto* saved = data.FindString("id");
    return id.empty() || (saved && *saved == id) ? &data : nullptr;
  }
  void MaterializeCurrentTab() {
    auto& tab = Current();
    if (!tab.deferred_tree) return;
    auto tree = std::move(*tab.deferred_tree); tab.deferred_tree.reset();
    const auto selected = tab.deferred_active;
    size_t leaves = 0; std::set<std::string> ids;
    auto restored = RestorePane(tree, 0, leaves, ids, selected);
    if (!restored) restored = std::make_unique<FilesView>(this, Home());
    tab.pane_host->AddChildView(std::move(restored));
    VisitPanes(tab.pane_host, base::BindRepeating([](FilesView* shell, const std::string& id, FilesView* pane) {
      if (!shell->Current().active_pane || pane->pane_id_ == id) shell->Current().active_pane = pane;
    }, base::Unretained(this), selected));
  }
  // Older workspaces can contain Network panes even without an SMB preference.
  // Replace just those leaves so pinned tabs, splits and their active pane survive.
  void RestoreNetworkPanesAsHome(base::DictValue& tree) {
    if (auto* first = tree.FindDict("first")) {
      RestoreNetworkPanesAsHome(*first);
      RestoreNetworkPanesAsHome(*tree.FindDict("second"));
    } else if (tree.FindBool("network_page").value_or(false)) {
      tree.Remove("network_page"); tree.Remove("history_page");
      tree.Remove("name"); tree.Remove("scroll");
      tree.Set("path", Home().AsUTF8Unsafe());
    }
  }
  bool RestoreWorkspace() {
    auto workspace = GetCrFilesWorkspace();
    auto* saved = workspace.FindList("tabs");
    if (workspace.FindInt("version") != 1 || !saved || saved->empty() || saved->size() > 64) return false;
    restoring_workspace_ = true;
    size_t leaves = 0; std::set<std::string> ids;
    for (auto& value : *saved) {
      auto* item = value.GetIfDict(); auto* tree = item ? item->FindDict("tree") : nullptr;
      if (!tree || !ValidateSavedTree(*tree, 0, leaves, ids)) continue;
      if (!IsSmbEnabledForSession()) RestoreNetworkPanesAsHome(*tree);
      tabs_.push_back(Tab{.id = next_id_++}); active_ = tabs_.size() - 1;
      auto* host = split_host_->AddChildView(std::make_unique<views::View>());
      host->SetLayoutManager(std::make_unique<views::FillLayout>()); host->SetVisible(false);
      auto& tab = Current(); tab.pane_host = host; tab.deferred_tree = tree->Clone();
      tab.pinned = item->FindBool("pinned").value_or(false);
      if (const auto* id = item->FindString("active_pane")) tab.deferred_active = *id;
      const auto* active = SavedActivePane(*tree, tab.deferred_active);
      if (!active) active = SavedActivePane(*tree, {});
      if (active) {
        if (const auto* path = active->FindString("path")) tab.path = base::FilePath::FromUTF8Unsafe(*path);
        if (const auto* name = active->FindString("name")) tab.deferred_title = *name;
        if (active->FindBool("history_page").value_or(false)) tab.deferred_title = base::UTF16ToUTF8(Text(active->FindBool("search_page").value_or(false) ? IDS_CRFILES_SEARCH : IDS_CRFILES_HISTORY));
        if (active->FindBool("network_page").value_or(false)) tab.deferred_title = base::UTF16ToUTF8(Text(IDS_CRFILES_NETWORK));
        if (active->FindString("search_tag")) {
          if (const auto* title = active->FindString("search_scope_title"))
            tab.deferred_title = base::UTF16ToUTF8(Text(IDS_CRFILES_SEARCH)) + " · " + *title;
        }
        if (active->FindString("tag_id")) {
          tab.deferred_title = base::UTF16ToUTF8(Text(IDS_CRFILES_TAGS));
          if (const auto* name = active->FindString("tag_name"); name && !name->empty()) tab.deferred_title += " · " + *name;
        }
      }
      if (tab.deferred_title.empty()) tab.deferred_title = base::UTF16ToUTF8(EntryName(tab.path));
    }
    if (tabs_.empty()) { restoring_workspace_ = false; active_ = 0; return false; }
    // Read the central registry before materializing any tab. An explicitly
    // empty registry must not resurrect legacy shares in deferred tab trees.
    shared_directories_.clear();
    network_share_cancel_->store(true); network_share_cancel_ = std::make_shared<std::atomic<bool>>(false);
    network_share_credentials_.clear(); network_share_checks_.clear(); network_share_attempted_.clear(); network_share_errors_.clear();
    if (const auto* shares = workspace.FindList("shared_directories")) {
      for (const auto& value : *shares) {
        const auto* item = value.GetIfDict();
        if (item) RestoreSharedDirectory(item->FindString("id"),
            item->FindString("name"), item->FindString("path"), item->FindString("tag_id"), item->FindString("network_url"));
      }
    } else {
      for (const auto& tab : tabs_)
        if (tab.deferred_tree) RestoreLegacyShares(*tab.deferred_tree);
    }
    active_ = std::clamp(workspace.FindInt("active").value_or(0), 0, static_cast<int>(tabs_.size()) - 1);
    const int restored_active = Current().id;
    std::stable_partition(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.pinned; });
    for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].id == restored_active) active_ = i;
    sidebar_width_ = std::clamp(workspace.FindInt("sidebar_width").value_or(240), 220, 360);
    sidebar_visible_ = workspace.FindBool("sidebar").value_or(true);
    sidebar_display_width_ = sidebar_visible_ ? sidebar_width_ : 0;
    UpdateSidebarPresentation();
    SelectPaneTab(Current().id); restoring_workspace_ = false; return true;
  }
  static bool ValidPaneName(std::string_view name) {
    return !name.empty() && name.size() <= 64 && base::IsStringUTF8(name) &&
        std::ranges::none_of(base::UTF8ToUTF16(name), [](char16_t c) {
          return c < 32 || (c >= 0x7f && c <= 0x9f) || c == 0x061c || c == 0x200e || c == 0x200f ||
              (c >= 0x2028 && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
        });
  }
  static base::FilePath ShareDirectoryKey(const base::FilePath& path) {
    return path.NormalizePathSeparators().StripTrailingSeparators();
  }
  bool IsDirectoryShared(const base::FilePath& path) const {
    const auto* shell = root_ ? root_.get() : this;
    const auto directory = ShareDirectoryKey(path);
    return std::ranges::any_of(shell->shared_directories_, [&](const auto& target) {
      return target.tag_id == 0 && target.network_url.empty() && target.directory == directory;
    });
  }
  void UpdateNetworkShareButton() {
    if (!network_share_button_) return;
    auto* shell = root_ ? root_.get() : this;
    const auto url = NetworkPageUrl(network_view_);
    const auto target = std::ranges::find(shell->shared_directories_, url, &NearbySharing::SharedTarget::network_url);
    const bool shared = !url.empty() && target != shell->shared_directories_.end() && target->remote;
    const bool checking = shell->network_share_checks_.contains(url);
    const auto error = shell->network_share_errors_.find(url);
    const bool denied = NetworkPageWriteAccess(network_view_) == mojom::SmbWriteAccess::kReadOnly ||
        (error != shell->network_share_errors_.end() && error->second == mojom::SmbError::kAccessDenied);
    const auto credentials = NetworkPageCredentials(network_view_);
    const bool available = credentials && !credentials->location.share.empty();
    network_share_button_->SetEnabled(!network_footer_busy_ && !checking && !denied && available &&
        error == shell->network_share_errors_.end());
    network_share_button_->GetViewAccessibility().SetRole(ax::mojom::Role::kToggleButton);
    network_share_button_->GetViewAccessibility().SetCheckedState(shared ? ax::mojom::CheckedState::kTrue : ax::mojom::CheckedState::kFalse);
    network_share_button_->SetBackground(shared ? views::CreateRoundedRectBackground(ui::kColorSysPrimaryContainer, 8) : nullptr);
    network_share_button_->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(
        vector_icons::kScreenShareIcon, shared ? ui::kColorSysOnPrimaryContainer : ui::kColorSysOnSurface, 20));
    const int label = checking ? IDS_CRFILES_NETWORK_CHECKING_WRITE : denied ? IDS_CRFILES_NETWORK_NO_WRITE :
        error != shell->network_share_errors_.end() ? IDS_CRFILES_NETWORK_WRITE_UNKNOWN :
        shared ? IDS_CRFILES_SHARED_PANE : IDS_CRFILES_SHARE_PANE;
    network_share_button_->SetTooltipText(Text(label) + (url.empty() ? std::u16string() : u"\n" + base::UTF8ToUTF16(url)));
    network_share_button_->SetAccessibleName(Text(label));
  }
  void BeginNetworkShare(SmbCredentials credentials) {
    if (root_) { root_->BeginNetworkShare(std::move(credentials)); return; }
    const auto url = credentials.location.Url();
    if (credentials.location.share.empty() || network_share_checks_.contains(url)) return;
    auto found = std::ranges::find(shared_directories_, url, &NearbySharing::SharedTarget::network_url);
    NearbySharing::SharedTarget target;
    if (found != shared_directories_.end()) target = *found;
    else {
      target.id = base::Uuid::GenerateRandomV4().AsLowercaseString(); target.network_url = url;
      const auto& location = credentials.location;
      target.name = location.path.empty() ? location.share : location.path.substr(location.path.find_last_of('/') + 1);
      if (!ValidPaneName(target.name)) target.name = base::UTF16ToUTF8(Text(IDS_CRFILES_FOLDER));
      const auto prefs = GetNearbySharingPreferences();
      target.directory = (prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() :
          base::FilePath::FromUTF8Unsafe(prefs.directory)).AppendASCII("SMB-" + target.id);
    }
    const uint64_t serial = ++network_share_serial_;
    network_share_checks_[url] = serial; network_share_attempted_.insert(url); network_share_errors_.erase(url);
    SyncSharedTargets();
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&CheckNetworkShareWritable, credentials, target.directory,
            base::BindRepeating([](std::shared_ptr<std::atomic<bool>> canceled) { return canceled->load(); }, network_share_cancel_)),
        base::BindOnce([](base::WeakPtr<FilesView> shell, uint64_t serial, NearbySharing::SharedTarget target,
                         SmbCredentials credentials, mojom::SmbError error) {
          if (!shell || shell->network_share_cancel_->load()) return;
          const auto check = shell->network_share_checks_.find(target.network_url);
          if (check == shell->network_share_checks_.end() || check->second != serial) return;
          shell->network_share_checks_.erase(check);
          auto found = std::ranges::find(shell->shared_directories_, target.network_url, &NearbySharing::SharedTarget::network_url);
          if (error == mojom::SmbError::kOk) {
            target.remote = CreateNetworkReceiveTarget(credentials);
            shell->network_share_credentials_[target.network_url] = std::move(credentials);
            if (found == shell->shared_directories_.end()) shell->shared_directories_.push_back(std::move(target));
            else *found = std::move(target);
          } else {
            shell->network_share_errors_[target.network_url] = error;
            shell->network_share_credentials_.erase(target.network_url);
            if (found != shell->shared_directories_.end()) found->remote.reset();
          }
          shell->SyncSharedTargets(); shell->ScheduleWorkspaceSave();
        }, weak_.GetWeakPtr(), serial, target, credentials));
  }
  void ToggleNetworkShare() {
    if (!network_share_button_ || !network_share_button_->GetEnabled()) return;
    auto credentials = NetworkPageCredentials(network_view_);
    if (!credentials || credentials->location.share.empty()) return;
    auto* shell = root_ ? root_.get() : this;
    const auto url = credentials->location.Url();
    const auto found = std::ranges::find(shell->shared_directories_, url, &NearbySharing::SharedTarget::network_url);
    if (found != shell->shared_directories_.end() && found->remote) {
      shell->shared_directories_.erase(found); shell->network_share_credentials_.erase(url);
      shell->network_share_checks_.erase(url); shell->network_share_attempted_.erase(url);
      shell->SyncSharedTargets(); shell->ScheduleWorkspaceSave();
    } else shell->BeginNetworkShare(std::move(*credentials));
  }
  void UpdateShareButton() {
    if (network_view_) { UpdateNetworkShareButton(); return; }
    if (!share_button_ || tabs_.empty()) return;
    const bool tag_target = tag_id_ > 0 && IsTagListing();
    const bool shared = tag_target ? std::ranges::any_of(root_->shared_directories_,
        [&](const auto& target) { return target.tag_id == tag_id_; }) : IsDirectoryShared(Current().path);
    share_button_->SetEnabled(tag_id_ != 0 && (shared || !IsDrivesLocation(Current().path)));
    share_button_->GetViewAccessibility().SetRole(ax::mojom::Role::kToggleButton);
    share_button_->GetViewAccessibility().SetCheckedState(shared ? ax::mojom::CheckedState::kTrue : ax::mojom::CheckedState::kFalse);
    share_button_->SetBackground(shared ? views::CreateRoundedRectBackground(ui::kColorSysPrimaryContainer, 8) : nullptr);
    share_button_->SetImageModel(views::Button::STATE_NORMAL,
        ui::ImageModel::FromVectorIcon(vector_icons::kScreenShareIcon, shared ? ui::kColorSysOnPrimaryContainer : ui::kColorSysOnSurface, 20));
    share_button_->SetTooltipText(Text(shared ? IDS_CRFILES_SHARED_PANE : IDS_CRFILES_SHARE_PANE) +
        (shared ? u"\n" + (tag_target ? base::UTF8ToUTF16(tag_name_) : Current().path.AsUTF16Unsafe()) : std::u16string()));
    share_button_->SetAccessibleName(Text(shared ? IDS_CRFILES_SHARED_PANE : IDS_CRFILES_SHARE_PANE));
  }
  void RemoveSharedDirectory(const base::FilePath& path) {
    const auto directory = ShareDirectoryKey(path);
    std::erase_if(shared_directories_, [&](const auto& target) { return target.tag_id == 0 && target.network_url.empty() && target.directory == directory; });
    SyncSharedTargets(); ScheduleWorkspaceSave();
  }
  void TogglePaneShare() {
    if (network_view_) { ToggleNetworkShare(); return; }
    if (tag_id_ == 0) return;
    if (IsTagListing()) {
      auto& targets = root_->shared_directories_;
      if (std::erase_if(targets, [&](const auto& target) { return target.tag_id == tag_id_; }) == 0) {
        if (Current().loading || Current().error) return;
        const auto prefs = GetNearbySharingPreferences();
        const auto directory = prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() : base::FilePath::FromUTF8Unsafe(prefs.directory);
        EnsureDirectoryAccess(GetWidget(), directory, true, base::BindOnce(
            [](base::WeakPtr<FilesView> pane, int64_t id, std::string name, base::FilePath destination, bool allowed) {
              if (!pane || pane->tag_id_ != id || !pane->IsTagListing()) return;
              if (!allowed) { pane->status_->SetText(Text(IDS_CRFILES_READ_ERROR)); return; }
              auto& shared_targets = pane->root_->shared_directories_;
              if (std::ranges::none_of(shared_targets, [&](const auto& target) { return target.tag_id == id; }))
                shared_targets.push_back({base::Uuid::GenerateRandomV4().AsLowercaseString(), std::move(name), destination, id});
              pane->root_->SyncSharedTargets(); pane->root_->ScheduleWorkspaceSave();
            }, weak_.GetWeakPtr(), tag_id_, tag_name_, directory));
        return;
      }
      root_->SyncSharedTargets(); root_->ScheduleWorkspaceSave(); return;
    }
    const auto directory = ShareDirectoryKey(Current().path);
    if (IsDirectoryShared(directory)) { root_->RemoveSharedDirectory(directory); return; }
    EnsureDirectoryAccess(GetWidget(), directory, true, base::BindOnce(
        [](base::WeakPtr<FilesView> pane, base::FilePath path, bool allowed) {
          if (!pane || ShareDirectoryKey(pane->Current().path) != path) return;
          if (allowed && !pane->IsDirectoryShared(path)) pane->TogglePaneShareAuthorized();
          else if (!allowed) pane->status_->SetText(Text(IDS_CRFILES_READ_ERROR));
        }, weak_.GetWeakPtr(), directory));
  }
  void TogglePaneShareAuthorized() {
    const auto directory = ShareDirectoryKey(Current().path);
    auto& targets = root_->shared_directories_;
    if (IsDirectoryShared(directory)) {
      root_->RemoveSharedDirectory(directory);
      return;
    } else {
      if (IsDrivesLocation(directory) || Current().loading || Current().error ||
          !directory.IsAbsolute() || directory.ReferencesParent() ||
          directory == TrashRoot() || TrashRoot().IsParent(directory)) return;
      const auto name = ValidPaneName(PaneName()) ? PaneName() : base::UTF16ToUTF8(Text(IDS_CRFILES_FOLDER));
      targets.push_back({base::Uuid::GenerateRandomV4().AsLowercaseString(), name, directory});
    }
    root_->SyncSharedTargets(); root_->ScheduleWorkspaceSave();
  }
  void ClearSavedShares(base::DictValue& data) {
    data.Remove("share"); data.Remove("receive_path");
    if (auto* first = data.FindDict("first")) ClearSavedShares(*first);
    if (auto* second = data.FindDict("second")) ClearSavedShares(*second);
  }
  void RestoreSharedDirectory(const std::string* id, const std::string* name,
                              const std::string* path, const std::string* saved_tag = nullptr,
                              const std::string* network_url = nullptr) {
    int64_t tag = 0;
    if (saved_tag && (!base::StringToInt64(*saved_tag, &tag) || tag <= 0)) return;
    if (!id || !path || !base::Uuid::ParseLowercase(*id).is_valid()) return;
    const auto directory = ShareDirectoryKey(base::FilePath::FromUTF8Unsafe(*path));
    if (network_url) {
      const auto location = ParseSmbLocation(*network_url);
      if (!location || location->share.empty() || !directory.IsAbsolute() || directory.ReferencesParent() ||
          std::ranges::any_of(shared_directories_, [&](const auto& target) {
            return target.id == *id || target.network_url == location->Url();
          })) return;
      shared_directories_.push_back({*id, name && ValidPaneName(*name) ? *name : location->share,
          directory, 0, location->Url()});
      return;  // Login and a fresh write probe are required before publication.
    }
    if (!directory.IsAbsolute() || directory.ReferencesParent() ||
        directory == TrashRoot() || TrashRoot().IsParent(directory) ||
        (tag == 0 ? IsDirectoryShared(directory) : std::ranges::any_of(shared_directories_, [&](const auto& target) { return target.tag_id == tag; }))) return;
    // Legacy entries used pane IDs; keep the first ID for each directory so
    // restoring or materializing another tab cannot change its receive target.
    if (std::ranges::any_of(shared_directories_, [&](const auto& target) { return target.id == *id; })) return;
    const auto fallback = directory.BaseName().AsUTF8Unsafe();
    shared_directories_.push_back({*id, name && ValidPaneName(*name) ? *name :
        ValidPaneName(fallback) ? fallback : base::UTF16ToUTF8(Text(IDS_CRFILES_FOLDER)), directory, tag});
  }
  void RestoreLegacyShares(const base::DictValue& data) {
    if (const auto* first = data.FindDict("first")) {
      RestoreLegacyShares(*first);
      if (const auto* second = data.FindDict("second")) RestoreLegacyShares(*second);
      return;
    }
    if (data.FindBool("share").value_or(false))
      RestoreSharedDirectory(data.FindString("id"), data.FindString("name"), data.FindString("receive_path"));
  }
  void SyncSharedTargets() {
    if (root_) { root_->SyncSharedTargets(); return; }
    for (auto& tab : tabs_) {
      if (tab.deferred_tree) continue;
      VisitPanes(tab.pane_host, base::BindRepeating([](FilesView* pane) {
        pane->UpdateShareButton();
      }));
    }
    const auto share_prefs = GetNearbySharingPreferences();
    const auto default_directory = share_prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() : base::FilePath::FromUTF8Unsafe(share_prefs.directory);
    for (auto& target : shared_directories_) if (target.tag_id > 0) target.directory = default_directory;
    auto targets = shared_directories_;
    std::erase_if(targets, [](const auto& target) { return !target.network_url.empty() && !target.remote; });
    for (auto& target : targets) if (target.tag_id > 0) {
      target.name = std::string(base::TruncateUTF8ToByteSize(target.name, 64));
      if (!NearbySharing::IsValidConfiguration(target.name, target.directory)) target.name = base::UTF16ToUTF8(Text(IDS_CRFILES_TAGS)) + " " + base::NumberToString(target.tag_id);
    }
    // Withdraw only revoked/changed targets immediately. An unrelated pane
    // update must not cancel an ongoing receive to a still-valid target.
    std::erase_if(published_targets_, [&](const auto& old) {
      return std::ranges::none_of(targets, [&](const auto& target) {
        return target.id == old.id && target.directory == old.directory && target.tag_id == old.tag_id && target.remote == old.remote;
      });
    });
    sharing_->SetSharedTargets(published_targets_);
    const auto generation = ++shared_targets_generation_;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](std::vector<NearbySharing::SharedTarget> list) {
          const auto tags = ReadFileTags();
          std::erase_if(list, [&](const auto& target) {
            if (target.tag_id > 0) return !tags.ok || std::ranges::none_of(tags.tags, [&](const auto& tag) { return tag.id == target.tag_id; }) || !base::CreateDirectory(target.directory);
            return !base::DirectoryExists(target.directory);
          }); return list;
        }, std::move(targets)),
        base::BindOnce([](base::WeakPtr<FilesView> shell, uint64_t serial, std::vector<NearbySharing::SharedTarget> list) {
          if (!shell || shell->shared_targets_generation_ != serial) return;
          if (!list.empty() && !shell->sharing_->GetSnapshot().enabled) {
            const auto prefs = GetNearbySharingPreferences();
            shell->sharing_->Enable(prefs.name, prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() :
                base::FilePath::FromUTF8Unsafe(prefs.directory), prefs.avatar);
          }
          shell->published_targets_ = list;
          shell->sharing_->SetSharedTargets(std::move(list));
        }, weak_.GetWeakPtr(), generation));
  }
  void DismissEditingBubbles(gfx::Point screen_point) {
    CloseHoverPreview();
    if (tabs_.empty()) return;
    VisitPanes(Current().pane_host, base::BindRepeating([](gfx::Point point, FilesView* pane) {
      if (!pane->new_folder_widget_) return;
      // Native mouse capture can deliver a bubble-area press to the owner.
      // Blank space and controls inside the bubble are not outside clicks.
      if (pane->new_folder_widget_->IsVisible() &&
          pane->new_folder_widget_->GetWindowBoundsInScreen().Contains(point)) return;
      // Defer destruction until mouse dispatch unwinds, and never close a new
      // bubble opened by the same click. IME windows are outside this root.
      pane->Deferred(base::BindRepeating([](base::WeakPtr<FilesView> view, uint64_t generation) {
        if (view && view->new_folder_generation_ == generation) view->CloseNewFolder();
      }, pane->weak_.GetWeakPtr(), pane->new_folder_generation_)).Run();
    }, screen_point));
  }
  void ShowRenamePane() {
    CloseNewFolder();
    new_folder_delegate_ = std::make_unique<views::BubbleDialogDelegate>(more_button_, views::BubbleBorder::BOTTOM_LEFT);
    // Visible arrows require a Views-drawn shadow, including on macOS.
    new_folder_delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    new_folder_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    new_folder_delegate_->SetShowTitle(false); new_folder_delegate_->SetShowCloseButton(false);
    // Input-method switching can temporarily activate another window. Close on
    // explicit outside clicks instead of treating activation loss as dismissal.
    new_folder_delegate_->set_close_on_deactivate(false); new_folder_delegate_->set_adjust_if_offscreen(true);
    new_folder_delegate_->set_margins(gfx::Insets());
    auto contents = std::make_unique<NewFolderView>(base::BindRepeating(&FilesView::RenamePane, weak_.GetWeakPtr()), IDS_CRFILES_RENAME_PANE, IDS_CRFILES_PANE_NAME_ERROR);
    new_folder_contents_ = contents.get(); contents->field()->SetText(base::UTF8ToUTF16(PaneName()));
    new_folder_delegate_->SetInitiallyFocusedView(contents->field());
    new_folder_delegate_->SetContentsView(std::move(contents));
    new_folder_widget_ = CreateCrFilesBubble(new_folder_delegate_.get(),
        base::BindOnce([](base::WeakPtr<FilesView> view, views::Widget::ClosedReason) {
          if (!view) return;
          view->rename_focus_observer_.reset();
          view->new_folder_contents_ = nullptr; view->new_folder_delegate_->SetAnchorView(nullptr); view->new_folder_widget_.reset(); view->UpdateFooterVisibility();
        }, weak_.GetWeakPtr()));
    new_folder_delegate_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    rename_focus_observer_ = std::make_unique<RenameFocusObserver>(
        GetWidget(), new_folder_widget_.get(), new_folder_contents_->field());
    new_folder_widget_->Show(); UpdateFooterVisibility();
  }
  void RenamePane(std::u16string name) {
    const auto value = base::UTF16ToUTF8(name);
    if (!ValidPaneName(value)) { new_folder_contents_->Failed(); new_folder_delegate_->SizeToContents(); return; }
    pane_name_ = value; Deferred(base::BindRepeating(&FilesView::CloseNewFolder, weak_.GetWeakPtr())).Run(); UpdateStatus();
    root_->SyncSharedTargets(); root_->SyncNavigation(); root_->ScheduleWorkspaceSave();
  }

  FilesView* ActivePane() {
    return root_ ? this : (tabs_.empty() ? nullptr : Current().active_pane.get());
  }
  void VisitPanes(views::View* view, const base::RepeatingCallback<void(FilesView*)>& visit) {
    if (auto* pane = views::AsViewClass<FilesView>(view)) { visit.Run(pane); return; }
    for (const auto& child : view->children()) VisitPanes(child.get(), visit);
  }
  void UpdatePaneControls() {
    if (tabs_.empty()) return;
    auto* host = Current().pane_host.get();
    size_t count = 0;
    VisitPanes(host, base::BindRepeating([](size_t* total, FilesView*) { ++*total; }, &count));
    VisitPanes(host, base::BindRepeating([](FilesView* active, bool multiple, FilesView* pane) {
      pane->pane_can_close_ = multiple;
      pane->UpdateFooterVisibility();
      const bool selected = pane == active && multiple;
      pane->footer_folder_name_->SetEnabledColor(!multiple || selected ?
          ui::kColorSysPrimary : ui::kColorSysOnSurfaceSecondary);
      const auto surface = (pane->history_page_ && pane->tag_id_ < 0) ? ui::kColorSysBase : kFolderSurfaceColor;
      if (selected) {
        const auto style = GetSplitSelectionStyle();
        if (!pane->pane_border_active_ || pane->pane_border_style_ != style)
          pane->pane_body_->SetBorder(std::make_unique<ActivePaneBorder>(pane->pane_body_, surface, style));
        pane->pane_border_style_ = style;
      } else {
        pane->pane_body_->SetBorder(views::CreateSolidBorder(multiple ? 4 : 1, surface));
      }
      pane->pane_border_active_ = selected;
      const bool trash = pane->trash_footer_actions_->GetVisible();
      if (selected && pane->pane_border_style_ == SplitSelectionStyle::kCorners) {
        pane->footer_->SetBorder(std::make_unique<FooterGlowBorder>(trash));
      } else if (trash) {
        pane->footer_->SetBorder(views::CreateRoundedRectBorder(1, 24, ui::kColorSysOutline));
      } else {
        pane->footer_->SetBorder(std::make_unique<FooterGlowBorder>(false));
      }
    }, base::Unretained(ActivePane()), count > 1));
  }
  void ActivateFromInput(bool mouse_press) {
    // Preserve the state before activation: the button callback runs later,
    // after this pane has already become active.
    if (mouse_press) mouse_press_activated_pane_ = root_ && root_->ActivePane() != this;
    ActivateSelf();
  }
  void ActivateSelf() {
    UpdateFooterVisibility();
    if (!root_ || root_->tabs_.empty() || root_->ActivePane() == this ||
        !root_->Current().pane_host->Contains(this)) return;
    if (auto* previous = root_->ActivePane()) {
      previous->CloseNewFolder(); previous->CloseViewOptions();
    }
    root_->EndAddressEditing();
    root_->Current().active_pane = this;
    files_->RequestFocus();
    root_->ScheduleWorkspaceSave();
    root_->UpdatePaneControls();
    // Keep the originating event's target alive until dispatch completes.
    root_->Deferred(base::BindRepeating(&FilesView::SyncNavigation, root_->weak_.GetWeakPtr())).Run();
  }
  void SuspendPanePopups() {
    if (tabs_.empty()) return;
    EndAddressEditing();
    VisitPanes(Current().pane_host, base::BindRepeating([](FilesView* pane) {
      pane->history_filter_runner_.reset();
      pane->CancelFileVisuals(); pane->SaveScroll(); pane->CloseNewFolder(); pane->CloseViewOptions();
      pane->CancelDirectoryWork(); ++pane->Current().request;
      if (pane->IsTagListing() && (pane->tag_loading_ || pane->tag_sizes_pending_))
        pane->tag_version_ = std::numeric_limits<uint64_t>::max();
      pane->tag_loading_ = false; pane->tag_sizes_pending_ = false;
    }));
  }
  void AddPaneTab(base::FilePath path, bool history_page = false, bool search_page = false,
                  int64_t tag_id = -1, std::string tag_name = {}, SearchScope search_scope = {}, bool network_page = false) {
    SuspendPanePopups();
    for (auto& tab : tabs_) tab.pane_host->SetVisible(false);
    tabs_.push_back(Tab{.id = next_id_++}); active_ = tabs_.size() - 1;
    auto* host = split_host_->AddChildView(std::make_unique<views::View>());
    host->SetLayoutManager(std::make_unique<views::FillLayout>());
    Current().pane_host = host;
    auto pane = std::make_unique<FilesView>(this, path.empty() ? Home() : path, history_page, false, search_page, tag_id, std::move(tag_name), std::move(search_scope), network_page);
    Current().active_pane = host->AddChildView(std::move(pane));
    UpdatePaneControls(); SyncNavigation();
    if (GetWidget()) ActivePane()->DefaultFocusView()->RequestFocus();
    ScheduleWorkspaceSave();
  }
  void SelectPaneTab(int id) {
    const auto target = std::ranges::find(tabs_, id, &Tab::id);
    if (target == tabs_.end()) return;
    const bool first_activation = target->deferred_tree.has_value();
    const gfx::Rect previous_bounds = Current().pane_host->bounds();
    // Workspace restoration also selects a tab from the constructor, before
    // this view belongs to a widget or has a usable viewport.
    const bool prepare_first_viewport =
        first_activation && GetWidget() && !previous_bounds.IsEmpty();
    SuspendPanePopups();
    for (size_t i = 0; i < tabs_.size(); ++i) {
      const bool selected = tabs_[i].id == id;
      if (!selected) tabs_[i].pane_host->SetVisible(false);
      if (selected) active_ = i;
    }
    MaterializeCurrentTab();
    ActivePane()->footer_active_ = false;
    VisitPanes(Current().pane_host, base::BindRepeating([](FilesView* pane) {
      pane->footer_restore_pending_ = true;
      pane->footer_hovered_ = false;
      pane->UpdateFooterVisibility();
      pane->InvalidateLayout();
    }));
    // A restored tab has no geometry yet. Give its hidden tree the current
    // viewport before RenderFiles prepares rows from the directory cache.
    // Otherwise the first viewport can be populated at a temporary 1px width.
    if (prepare_first_viewport) {
      Current().pane_host->SetBoundsRect(previous_bounds);
      Current().pane_host->DeprecatedLayoutImmediately();
    } else Current().pane_host->SetVisible(true);
    VisitPanes(Current().pane_host, base::BindRepeating([](FilesView* pane) {
      if (pane->history_page_) return;
      if (pane->pending_navigation_) {
        const auto pending = *pane->pending_navigation_;
        pane->Navigate(pending.path, pending.history_index, pending.refresh);
      } else if (pane->Current().sorted_mode != pane->Current().options.sort ||
                 pane->Current().sorted_hidden != pane->Current().options.show_hidden) pane->RenderFiles();
    }));
    if (prepare_first_viewport) Current().pane_host->SetVisible(true);
    if (auto* pane = ActivePane(); pane->history_page_ && !pane->history_loading_ &&
        (!pane->history_cache_valid_ || pane->history_version_ != FileHistoryVersion() ||
         pane->history_loaded_query_ != pane->history_query_ || pane->history_loaded_filter_ != pane->history_filter_))
      pane->LoadHistory();
    if (auto* list = views::AsViewClass<HistoryList>(ActivePane()->files_->contents()))
      list->ScheduleUpdate();
    UpdatePaneControls(); SyncNavigation(); ScheduleWorkspaceSave();
    if (prepare_first_viewport) {
      // Navigation chrome and scrollbars can change the final viewport. Fill
      // that range synchronously before painting, without waiting for disk IO.
      GetWidget()->GetRootView()->DeprecatedLayoutImmediately();
      VisitPanes(Current().pane_host, base::BindRepeating([](FilesView* pane) {
        pane->UpdateFileViewport();
      }));
    }
    ActivePane()->DefaultFocusView()->RequestFocus();
    SyncLiveDirectories();
  }
  void ClosePaneTab(int id) {
    auto found = std::ranges::find(tabs_, id, &Tab::id);
    if (found == tabs_.end()) return;
    if (tabs_.size() == 1) { GetWidget()->Close(); return; }
    SuspendPanePopups();
    base::DictValue saved;
    saved.Set("tree", found->deferred_tree ? found->deferred_tree->Clone() :
        SerializePane(found->pane_host->children().front().get()));
    saved.Set("active_pane", found->deferred_tree ? found->deferred_active : found->active_pane->pane_id_);
    saved.Set("pinned", found->pinned);
    if (closed_tabs_.size() >= 20) closed_tabs_.erase(closed_tabs_.begin());
    closed_tabs_.push_back(std::move(saved));
    const int previous_id = Current().id;
    auto* host = found->pane_host.get();
    found->active_pane = nullptr; found->pane_host = nullptr;
    tabs_.erase(found);
    active_ = std::min(active_, tabs_.size() - 1);
    for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].id == previous_id) active_ = i;
    split_host_->RemoveChildViewT(host);
    SyncSharedTargets(); ScheduleWorkspaceSave();
    SelectPaneTab(Current().id);
  }
  void RequestSplit(int direction) {
    if (root_) root_->SplitPane(this, direction);
  }
  void SplitPane(FilesView* target, int direction, base::FilePath path = {}, std::optional<SmbCredentials> network = std::nullopt,
                 std::optional<gfx::Rect> preview_bounds = std::nullopt) {
    if (!Current().pane_host->Contains(target)) return;
    target->CloseNewFolder(); target->CloseViewOptions();
    if (!network && target->network_view_) network = NetworkPageCredentials(target->network_view_);
    auto pane = std::make_unique<FilesView>(this, network ? base::FilePath() : path.empty() ? (target->tag_id_ >= 0 ? Home() : target->Current().path) : path,
        false, true, false, -1, std::string(), SearchScope(), network.has_value());
    pane->Current().options = target->Current().options;
    auto* added = pane.get();
    auto* parent = target->parent();
    auto* outer = views::AsViewClass<PaneSplit>(parent);
    if (outer) outer->FinishOpening();
    auto original = parent->RemoveChildViewT(target);
    const bool before = direction == 0 || direction == 2;
    std::unique_ptr<views::View> first = before ? std::move(pane) : std::move(original);
    std::unique_ptr<views::View> second = before ? std::move(original) : std::move(pane);
    auto split = std::make_unique<PaneSplit>(direction >= 2, std::move(first), std::move(second));
    split->PrepareOpening(before, preview_bounds);
    split->SetChangedCallback(base::BindRepeating(&FilesView::ScheduleWorkspaceSave, weak_.GetWeakPtr()));
    split->SetResizeCallback(base::BindRepeating(&FilesView::SplitResizeChanged, weak_.GetWeakPtr()));
    if (outer) outer->Replace(target, split.get());
    parent->AddChildView(std::move(split));
    Current().active_pane = added;
    if (network) {
      const auto& options = added->Current().options;
      SetNetworkPageLayout(added->network_view_, options.icons, kFileIconSizes[options.icon_index], options.show_hidden, options.NetworkColumnOptions(), static_cast<int>(options.sort));
      ConnectNetworkPage(added->network_view_, std::move(*network));
    } else added->Navigate(added->Current().path);
    EndAddressEditing(); UpdatePaneControls(); SyncNavigation();
    parent->InvalidateLayout(); added->DefaultFocusView()->RequestFocus();
    ScheduleWorkspaceSave();
  }
  void RequestClosePane() { if (root_) root_->ClosePane(this); }
  void CloseActivePaneOrTab() {
    auto* shell = root_ ? root_.get() : this;
    auto* pane = shell->ActivePane();
    if (!pane) return;
    if (views::AsViewClass<PaneSplit>(pane->parent())) {
      shell->ClosePane(pane);
    } else {
      shell->CloseTab(shell->Current().id);
    }
  }
  void ClosePane(FilesView* target) {
    if (!Current().pane_host->Contains(target)) return;
    auto* split = views::AsViewClass<PaneSplit>(target->parent());
    if (!split) return;
    split->FinishOpening();
    target->CloseNewFolder(); target->CloseViewOptions();
    auto* parent = split->parent();
    auto* sibling = split->Sibling(target);
    // Choose a surviving leaf before destroying the old branch.
    Current().active_pane = nullptr;
    VisitPanes(sibling, base::BindRepeating([](FilesView* shell, FilesView* pane) {
      if (!shell->Current().active_pane) shell->Current().active_pane = pane;
    }, base::Unretained(this)));
    auto survivor = split->RemoveChildViewT(sibling);
    if (auto* outer = views::AsViewClass<PaneSplit>(parent)) outer->Replace(split, survivor.get());
    auto removed = parent->RemoveChildViewT(split);
    parent->AddChildView(std::move(survivor));
    removed.reset();
    SyncSharedTargets(); ScheduleWorkspaceSave();
    EndAddressEditing(); UpdatePaneControls(); SyncNavigation();
    parent->InvalidateLayout(); ActivePane()->DefaultFocusView()->RequestFocus();
  }

  void ExitFullscreen() {
    fullscreen_hide_timer_.Stop();
    fullscreen_shown_ = false;
    fullscreen_exit_->SetVisible(false);
    GetWidget()->SetFullscreen(false);
    ActivePane()->DefaultFocusView()->RequestFocus();
  }
  void FullscreenPointer(gfx::Point point) {
    if (!GetWidget()->IsFullscreen()) return;
    if (point.y() <= 6 || (fullscreen_shown_ &&
        point.y() < kFullscreenExitTop + kFullscreenExitSize + 22 &&
        std::abs(point.x() - width() / 2) < 60)) {
      fullscreen_hide_timer_.Stop();
      if (fullscreen_shown_) return;
      fullscreen_shown_ = true;
      fullscreen_exit_->SetBounds((width() - kFullscreenExitSize) / 2,
          kFullscreenExitTop, kFullscreenExitSize, kFullscreenExitSize);
      fullscreen_exit_->SetVisible(true);
      auto* layer = fullscreen_exit_->layer();
      layer->GetAnimator()->StopAnimating();
      gfx::Transform above; above.Translate(0, -kFullscreenExitSlide);
      layer->SetTransform(above); layer->SetOpacity(0.f);
      ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
      settings.SetTransitionDuration(base::Milliseconds(180));
      settings.SetTweenType(gfx::Tween::EASE_OUT);
      layer->SetTransform(gfx::Transform()); layer->SetOpacity(1.f);
    } else if (fullscreen_shown_ && !fullscreen_hide_timer_.IsRunning()) {
      fullscreen_hide_timer_.Start(FROM_HERE, base::Milliseconds(450),
          base::BindOnce(&FilesView::HideFullscreenExit, weak_.GetWeakPtr()));
    }
  }
  void HideFullscreenExit() {
    fullscreen_shown_ = false;
    auto* layer = fullscreen_exit_->layer();
    layer->GetAnimator()->StopAnimating();
    {
      ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
      settings.SetTransitionDuration(base::Milliseconds(160));
      settings.SetTweenType(gfx::Tween::EASE_IN);
      gfx::Transform above; above.Translate(0, -kFullscreenExitSlide);
      layer->SetTransform(above); layer->SetOpacity(0.f);
    }
    fullscreen_hide_timer_.Start(FROM_HERE, base::Milliseconds(160),
        base::BindOnce([](base::WeakPtr<FilesView> view) {
          if (view && !view->fullscreen_shown_) view->fullscreen_exit_->SetVisible(false);
        }, weak_.GetWeakPtr()));
  }
  void ToggleSidebar() {
    auto* pane = ActivePane();
    if (!pane || !pane->HasNavigationChrome()) return;
    EndAddressEditing();
    sidebar_drag_start_.reset();
    AnimateSidebarTo(sidebar_visible_ ? 0 : sidebar_width_);
    ActivePane()->DefaultFocusView()->RequestFocus();
  }
  void ActivateSidebar(int label, base::RepeatingClosure action) {
    ActivePane()->DefaultFocusView()->RequestFocus();
    if (label != IDS_CRFILES_NETWORK && !ActivePane()->network_page_)
      ActivePane()->Current().sidebar_selection = label;
    action.Run();
    UpdateSidebarHighlight();
  }
#if BUILDFLAG(IS_WIN)
  void UpdateWindowsDrives() {
    if (!drive_list_) return;
    const DWORD mask = GetLogicalDrives();
    if (!mask || mask == drive_mask_) return;
    drive_mask_ = mask;
    drive_buttons_.clear(); drive_list_->RemoveAllChildViews();
    for (int i = 0; i < 26; ++i) {
      if (!(mask & (DWORD{1} << i))) continue;
      std::wstring name = L"A:\\"; name.front() = static_cast<wchar_t>(L'A' + i);
      const base::FilePath path(name);
      auto* button = drive_list_->AddChildView(std::make_unique<SidebarButton>(
          Deferred(base::BindRepeating([](base::WeakPtr<FilesView> view, base::FilePath path) {
            if (view) view->ActivePane()->Navigate(path);
          }, weak_.GetWeakPtr(), path)), path.AsUTF16Unsafe()));
      button->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      button->SetImageLabelSpacing(10);
      button->SetImageModel(views::Button::STATE_NORMAL,
          ui::ImageModel::FromVectorIcon(vector_icons::kDesktopWindowsIcon, ui::kColorSysOnSurface, 20));
      drive_buttons_.emplace(path, button);
    }
    UpdateSidebarHighlight();
    for (auto& tab : tabs_) VisitPanes(tab.pane_host, base::BindRepeating([](FilesView* pane) {
      if (!pane->history_page_ && !pane->Current().loading && IsDrivesLocation(pane->Current().path))
        pane->Navigate(pane->Current().path, -1, true);
    }));
  }
#endif
  void UpdateSidebarHighlight() {
    const bool bookmark_selected = bookmarks_ && bookmarks_->SetCurrentPath(ActivePane()->Current().path);
#if BUILDFLAG(IS_WIN)
    const auto current = ActivePane()->Current().path.NormalizePathSeparators().GetComponents();
    for (const auto& [path, button] : drive_buttons_) {
      const auto components = path.GetComponents();
      button->SetSelected(!bookmark_selected && ActivePane()->Current().sidebar_selection == 0 && !current.empty() && !components.empty() &&
          base::FilePath::CompareEqualIgnoreCase(current.front(), components.front()));
    }
#endif
    for (const auto& [label, button] : sidebar_buttons_)
      button->SetSelected(!bookmark_selected && label == ActivePane()->Current().sidebar_selection);
  }
  void SelectSidebarForPath(Tab& tab) {
    const auto prefs = GetNearbySharingPreferences();
    const auto received = prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() :
        base::FilePath::FromUTF8Unsafe(prefs.directory);
    const std::array locations = {
        std::pair{IDS_CRFILES_HOME, Home()}, std::pair{IDS_CRFILES_DESKTOP, DesktopPath()},
        std::pair{IDS_CRFILES_DOWNLOADS, NearbySharing::DefaultReceiveDirectory().DirName()},
#if BUILDFLAG(IS_MAC)
        std::pair{IDS_CRFILES_APPLICATIONS, base::FilePath(FILE_PATH_LITERAL("/Applications"))},
#endif
        std::pair{IDS_CRFILES_RECEIVED, received}};
    if (TrashRoot() == tab.path || TrashRoot().IsParent(tab.path)) {
      tab.sidebar_selection = IDS_CRFILES_TRASH; return;
    }
    tab.sidebar_selection = 0;
    size_t longest = 0;
    for (const auto& [label, path] : locations) {
      if (!path.empty() && (path == tab.path || path.IsParent(tab.path)) && path.value().size() >= longest) {
        longest = path.value().size(); tab.sidebar_selection = label;
      }
    }
  }
  void Desktop() {
    auto path = DesktopPath();
    if (path.empty()) ActivePane()->status_->SetText(Text(IDS_CRFILES_READ_ERROR)); else Navigate(path);
  }
  void Trash() {
    const auto root = TrashRoot();
    if (root.empty()) { ActivePane()->status_->SetText(Text(IDS_CRFILES_READ_ERROR)); return; }
    Navigate(root);
  }
  void ShowTrashFailure() {
    if (!GetWidget()) return;
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog->SetTitle(Text(IDS_CRFILES_TRASH));
    dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
    auto message = std::make_unique<views::Label>(Text(IDS_CRFILES_TRASH_ERROR));
    message->SetMultiLine(true); message->SetBorder(views::CreateEmptyBorder(20));
    message->SetPreferredSize(gfx::Size(420, 100));
    dialog->SetContentsView(std::move(message));
    ShowCrFilesDialog(std::move(dialog), GetWidget());
  }
  void ConfirmEmptyTrash() {
    if (!root_ || Current().path != TrashRoot() || trash_busy_ || root_->local_operation_busy_) return;
    CloseNewFolder();
    // Reuse the pane's transient bubble slot, including outside-click dismissal
    // and cleanup when navigating, switching panes or closing the window.
    new_folder_delegate_ = std::make_unique<views::BubbleDialogDelegate>(empty_trash_action_, views::BubbleBorder::BOTTOM_CENTER);
    // Visible arrows require a Views-drawn shadow, including on macOS.
    new_folder_delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    new_folder_delegate_->SetTitle(Text(IDS_CRFILES_EMPTY_TRASH));
    new_folder_delegate_->SetShowTitle(false);
    new_folder_delegate_->SetShowCloseButton(false);
    new_folder_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    new_folder_delegate_->set_close_on_deactivate(true);
    new_folder_delegate_->set_adjust_if_offscreen(true);
    new_folder_delegate_->set_margins(gfx::Insets());
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 12));
    auto* message = contents->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_EMPTY_TRASH_CONFIRM)));
    message->SetMultiLine(true); message->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    message->SizeToFit(320);
    auto* confirm = AddButton(contents.get(), Text(IDS_CRFILES_EMPTY_TRASH),
        Deferred(base::BindRepeating([](base::WeakPtr<FilesView> pane, uint64_t generation) {
          if (!pane || !pane->new_folder_widget_ || pane->new_folder_generation_ != generation) return;
          pane->CloseNewFolder();
          pane->StartEmptyTrash();
        }, weak_.GetWeakPtr(), new_folder_generation_)));
    confirm->SetEnabledTextColors(ui::kColorSysError);
    views::InkDrop::Get(confirm->ink_drop_view())->SetBaseColor(ui::kColorSysError);
    new_folder_delegate_->SetContentsView(std::move(contents));
    new_folder_widget_ = CreateCrFilesBubble(new_folder_delegate_.get(),
        base::BindOnce([](base::WeakPtr<FilesView> pane, views::Widget::ClosedReason) {
          if (!pane) return;
          ++pane->new_folder_generation_;
          pane->new_folder_delegate_->SetAnchorView(nullptr);
          pane->new_folder_widget_.reset();
          pane->UpdateFooterVisibility();
        }, weak_.GetWeakPtr()));
    new_folder_delegate_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    new_folder_delegate_->SizeToContents();
    new_folder_widget_->Show(); UpdateFooterVisibility();
  }
  void StartEmptyTrash() {
    if (!root_ || Current().path != TrashRoot() || trash_busy_ || root_->local_operation_busy_) return;
    root_->local_operation_busy_ = true;
    trash_busy_ = true; trash_error_ = false; UpdateStatus();
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&EmptyTrash),
        base::BindOnce([](base::WeakPtr<FilesView> shell, base::WeakPtr<FilesView> origin, bool ok) {
          if (!shell) return;
          shell->local_operation_busy_ = false;
          if (origin) { origin->trash_busy_ = false; origin->trash_error_ = !ok; origin->UpdateStatus(); }
          for (auto& tab : shell->tabs_) shell->VisitPanes(tab.pane_host, base::BindRepeating([](bool succeeded, FilesView* pane) {
            if (!pane->history_page_ && (pane->Current().path == TrashRoot() || TrashRoot().IsParent(pane->Current().path))) {
              pane->trash_error_ = !succeeded; pane->Refresh();
            }
          }, ok));
          if (!ok) shell->ShowTrashFailure();
        }, root_->weak_.GetWeakPtr(), weak_.GetWeakPtr()));
  }
  void ChangeTrash(bool restore) {
    if (tag_id_ == 0 || IsDrivesLocation(Current().path) || Current().selected.empty() || trash_busy_ || root_->local_operation_busy_) return;
    trash_busy_ = true;
    trash_error_ = false;
    UpdateStatus();
    std::vector<base::FilePath> paths(Current().selected.begin(), Current().selected.end());
    const int id = Current().id;
    const auto directory = Current().path;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](std::vector<base::FilePath> files, bool restore) {
          auto affected = files; affected.push_back(TrashRoot());
          ScopedFileCacheMutation mutation(std::move(affected));
          std::vector<base::FilePath> completed;
          for (const auto& file : files)
            if (restore ? RestoreFromTrash(file) : MoveToTrash(file)) completed.push_back(file);
          return std::pair{std::move(completed), files.size()};
        }, std::move(paths), restore),
        base::BindOnce(&FilesView::TrashChanged, weak_.GetWeakPtr(), id, directory));
  }
  void TrashChanged(int id, base::FilePath directory,
                    std::pair<std::vector<base::FilePath>, size_t> result) {
    trash_busy_ = false;
    if (directory == TrashRoot() && result.first.size() != result.second) ShowTrashFailure();
    if (Current().id == id && Current().path == directory) trash_error_ = result.first.size() != result.second;
    for (auto& tab : root_->tabs_) root_->VisitPanes(tab.pane_host,
        base::BindRepeating([](const base::FilePath& folder,
                              const std::vector<base::FilePath>& completed, FilesView* pane) {
          pane->RemoveCompletedEntries(folder, completed);
        }, directory, result.first));
    UpdateStatus();
  }
  void RemoveCompletedEntries(const base::FilePath& directory,
                              const std::vector<base::FilePath>& completed) {
    if (history_page_) return;
    if (IsTagListing()) { LoadTagFiles(); return; }
    const auto affected = [&](const base::FilePath& folder) {
      return folder == directory || (directory.empty() && std::ranges::any_of(completed,
          [&](const auto& path) { return path.DirName() == folder; }));
    };
    for (auto& tab : tabs_) {
      if (!affected(tab.path)) continue;
      for (const auto& path : completed) {
        auto entry = std::ranges::find(tab.entries, path, &Entry::path);
        if (entry != tab.entries.end()) { tab.removed.insert(path); tab.visuals.erase(path); tab.failed_thumbnails.erase(path); ++tab.metadata_generation; }
        tab.selected.erase(path);
      }
    }
    if (affected(Current().path)) {
      SaveScroll();
      RemoveFileRows(completed);
      CancelOrdering();
      std::erase_if(Current().sorted_entries, [&](size_t index) { return Current().removed.contains(Current().entries[index].path); });
      RebuildDisplayedEntries();
      static_cast<FileLayout*>(files_->contents())->SetItemCount(displayed_entries_.size());
      UpdateVirtualRows();
      files_->InvalidateLayout();
      if (Current().sorted_mode != Current().options.sort || Current().sorted_hidden != Current().options.show_hidden) RequestOrder();
    }
    UpdateStatus();
  }
  void Up() {
    if (auto* pane = ActivePane(); pane->network_view_) {
      auto location = ParseSmbLocation(NetworkPageUrl(pane->network_view_));
      if (!location) return;
      if (location->share.empty()) { ShowNetworkTab(); return; }
      if (location->path.empty()) location->share.clear();
      else { const auto slash = location->path.rfind('/'); location->path = slash == std::string::npos ? "" : location->path.substr(0, slash); }
      NavigateNetworkPage(pane->network_view_, location->Url()); return;
    }
    if (auto* pane = ActivePane(); pane->tag_id_ >= 0) {
      if (pane->IsTagListing()) { if (pane->tag_id_ > 0) pane->NavigateTag({0, {}, {}, {}}); }
      else if (pane->Current().path == pane->tag_directory_root_) pane->NavigateTag({pane->tag_id_, pane->tag_name_, {}, {}});
      else pane->NavigateTag({pane->tag_id_, pane->tag_name_, pane->tag_directory_root_, pane->Current().path.DirName()});
      return;
    }
    const auto& path = ActivePane()->Current().path;
    if (IsDrivesLocation(path)) return;
    if (IsDriveRoot(path)) Navigate(base::FilePath(FILE_PATH_LITERAL("/")));
    else if (path == TrashRoot()) Navigate(Home());
    else if (path.DirName().DirName().DirName() == TrashRoot() && path.DirName().BaseName().value() == FILE_PATH_LITERAL("files")) Navigate(TrashRoot());
    else Navigate(path.DirName());
  }
  void GoHome() { Navigate(Home()); }
  void Downloads() { Navigate(NearbySharing::DefaultReceiveDirectory().DirName()); }
  void Received() {
    auto prefs = GetNearbySharingPreferences();
    Navigate(prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() :
        base::FilePath::FromUTF8Unsafe(prefs.directory));
  }
  void NewCurrentTab() { NewTab(ActivePane()->Current().path); }
  void RubberBand(gfx::Rect rect, bool additive, bool begin, bool end) {
    if (order_pending_ || displayed_entries_.empty()) return;
    if (begin) { drag_selection_ = Current().selected; return; }
    if (end) { drag_selection_.clear(); return; }
    auto& selected = Current().selected;
    selected = additive ? drag_selection_ : std::set<base::FilePath>();
    auto* layout = static_cast<FileLayout*>(files_->contents());
    const auto [first, last] = layout->ItemsInRect(rect);
    for (size_t index = first; index < last; ++index)
      if (rect.Intersects(layout->ItemBounds(index))) selected.insert(Current().entries[displayed_entries_[index]].path);
    for (const auto& [path, row] : row_views_) {
      row->SetState(views::Button::STATE_NORMAL);
      row->SetBackground(selected.contains(path) ?
          views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
    }
    for (const auto& [entry, button] : visible_buttons_)
      if (button) button->SetState(views::Button::STATE_NORMAL);
    UpdateStatus();
  }
  void SelectFile(base::FilePath path, bool directory, views::View* tile, const ui::Event& event) {
    if (order_pending_) return;
    const auto position = static_cast<FileLayout*>(files_->contents())->IndexOf(tile);
    if (!position || *position >= displayed_entries_.size()) return;
    if (!event.IsShiftDown() || !keyboard_anchor_) keyboard_anchor_ = position;
    keyboard_position_ = position;
    const bool single_click_folder = directory && GetFileViewPreferences().single_click_folders &&
        event.IsMouseEvent() && event.AsMouseEvent()->IsOnlyLeftMouseButton() &&
        event.AsMouseEvent()->GetClickCount() == 1 && !event.IsControlDown() &&
        !event.IsCommandDown() &&
        !event.IsShiftDown() && !event.IsAltDown();
    if (single_click_folder && mouse_press_activated_pane_) return;
    if (single_click_folder ||
        (event.IsMouseEvent() && event.AsMouseEvent()->GetClickCount() == 2) ||
        (event.IsKeyEvent() && event.AsKeyEvent()->key_code() == ui::VKEY_RETURN)) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&FilesView::Open, weak_.GetWeakPtr(), path, directory));
      return;
    }
    auto& selected = Current().selected;
    if (!PrimaryModifierDown(event)) selected.clear();
    if (event.IsShiftDown()) {
      const size_t anchor = std::min(*keyboard_anchor_, displayed_entries_.size() - 1);
      for (size_t index = std::min(anchor, *position); index <= std::max(anchor, *position); ++index)
        selected.insert(Current().entries[displayed_entries_[index]].path);
    } else if (selected.contains(path)) selected.erase(path);
    else selected.insert(path);
    for (const auto& [item_path, row] : row_views_)
      row->SetBackground(selected.contains(item_path) ?
          views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
    UpdateStatus();
  }
  void NewTab(base::FilePath path) {
    if (!root_) { AddPaneTab(path); return; }
    CloseViewOptions();
    if (!tabs_.empty()) SaveScroll();
    tabs_.push_back(Tab{.id = next_id_++}); active_ = tabs_.size() - 1;
    Navigate(path.empty() ? Home() : path);
  }
  void SelectTab(int id) {
    if (!root_) { SelectPaneTab(id); return; }
    CloseNewFolder();
    ResetDirectoryAnimation();
    EndAddressEditing();
    CloseViewOptions();
    SaveScroll();
    for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].id == id) active_ = i;
    Render();
    files_->RequestFocus();
  }
  void CloseTab(int id) {
    if (!root_) { ClosePaneTab(id); return; }
    CloseNewFolder();
    ResetDirectoryAnimation();
    EndAddressEditing();
    CloseViewOptions();
    if (tabs_.size() == 1) { GetWidget()->Close(); return; }
    int active_id = Current().id;
    std::erase_if(tabs_, [id](const Tab& tab) { return tab.id == id; });
    active_ = std::min(active_, tabs_.size() - 1);
    if (active_id != id) for (size_t i = 0; i < tabs_.size(); ++i)
      if (tabs_[i].id == active_id) active_ = i;
    Render();
  }
  void ResetDirectoryAnimation() {
    directory_animation_pending_ = false;
    file_surface_->layer()->GetAnimator()->StopAnimating();
    file_surface_->layer()->SetOpacity(1.f);
    file_surface_->layer()->SetTransform(gfx::Transform());
  }
  void Navigate(base::FilePath path, int history_index = -1, bool refresh = false) {
    if (root_ && tag_id_ >= 0 && !refresh) {
      if (tag_id_ == 0) {
        auto entry = std::ranges::find(Current().entries, path, &Entry::path);
        if (entry != Current().entries.end() && entry->tag > 0) NavigateTag({entry->tag, entry->tag_name, {}, {}});
        else if (path.IsAbsolute()) NavigateTag({-1, {}, {}, path}, history_index);
        return;
      }
      if (path.IsAbsolute() && ((!tag_directory_root_.empty() &&
          (path == tag_directory_root_ || tag_directory_root_.IsParent(path))) ||
          (IsTagListing() && std::ranges::any_of(Current().entries, [&](const auto& entry) { return entry.path == path && entry.directory; })))) {
        NavigateTag({tag_id_, tag_name_, IsTagListing() ? path : tag_directory_root_, path}, history_index); return;
      }
      if (path.IsAbsolute()) NavigateTag({-1, {}, {}, path}, history_index);
      return;
    }
    if (root_ && !refresh && !tag_history_.empty() &&
        (path.IsAbsolute() || IsDrivesLocation(path))) {
      NavigateTag({-1, {}, {}, path}, history_index); return;
    }
    if (!root_) { EndAddressEditing(); if (ActivePane()->history_page_) AddPaneTab(path); else ActivePane()->Navigate(path, history_index, refresh); return; }
    if (history_page_) return;
    if (!path.IsAbsolute() && !path.empty() && !IsDrivesLocation(path)) {
      if (IsDrivesLocation(Current().path)) return;
      path = (Current().path.empty() ? Home() : Current().path).Append(path);
    }
    const auto generation = ++access_generation_;
    if (!GetWidget()) { NavigateAuthorized(std::move(path), history_index, refresh); return; }
    EnsureDirectoryAccess(GetWidget(), path, false, base::BindOnce(
        [](base::WeakPtr<FilesView> pane, uint64_t generation, base::FilePath path,
           int history_index, bool refresh, bool allowed) {
          if (!pane || generation != pane->access_generation_) return;
          if (allowed) pane->NavigateAuthorized(std::move(path), history_index, refresh);
          else pane->status_->SetText(Text(IDS_CRFILES_READ_ERROR));
        }, weak_.GetWeakPtr(), generation, path, history_index, refresh));
  }
  void NavigateAuthorized(base::FilePath path, int history_index, bool refresh) {
    if (path.empty()) return;
    ++context_generation_;
    if (context_runner_ && context_runner_->IsRunning()) context_runner_->Cancel();
    CloseNewFolder();
    if (root_ && root_->ActivePane() == this) root_->EndAddressEditing();
    if (!refresh) { trash_error_ = false; local_operation_error_ = false; local_sources_retained_ = false; }
    EndAddressEditing();
    ResetDirectoryAnimation();
    if (path.empty()) return;
    // Refreshes after file operations must not take focus from their bubbles.
    if (!refresh && GetWidget() && GetWidget()->IsActive() &&
        root_->ActivePane() == this && IsDrawn()) files_->RequestFocus();
    if (!path.IsAbsolute() && !IsDrivesLocation(path)) {
      // Relative names at the drive list have no filesystem parent.
      if (IsDrivesLocation(Current().path)) return;
      path = (Current().path.empty() ? Home() : Current().path).Append(path);
    }
    pending_search_selection_.clear();
    CancelDirectoryWork();
    CancelFileVisuals();
    auto& tab = Current(); tab.loading = true; tab.error = false;
    if (tab.path.empty()) tab.path = path;
    auto request = ++tab.request;
    bool cached = false;
    if (!refresh) {
      if (auto snapshot = CachedDirectories().Get(path); snapshot &&
          snapshot->sort == tab.options.sort && snapshot->hidden == tab.options.show_hidden) {
        Loaded(tab.id, request, history_index, false, false, std::move(*snapshot));
        cached = true;
      }
    }
    pending_navigation_ = PendingNavigation{path, history_index, refresh || cached};
    DirectoryReads().Request(path, tab.options.sort, tab.options.show_hidden, read_cancel_,
        base::BindOnce(&FilesView::Loaded, weak_.GetWeakPtr(), tab.id, request, history_index, refresh || cached, cached));
    UpdateStatus();
  }
  void Loaded(int id, uint64_t request, int history_index, bool refresh, bool revalidate, DirectoryResult result) {
    auto found = std::ranges::find(tabs_, id, &Tab::id);
    if (found == tabs_.end() || found->request != request) return;
    if (Stopped(read_cancel_)) return;
    if (!result.Current()) {
      const bool live_contents = result.ok && result.epoch && result.epoch->writers.load() != 0;
      if (!live_contents) {
        // A completed mutation invalidated this read; obtain a fresh snapshot.
        directory_retry_.Start(FROM_HERE, base::Milliseconds(250),
            base::BindOnce(&FilesView::Navigate, weak_.GetWeakPtr(), result.path, history_index, refresh));
        return;
      }
      // Receiving a large batch can keep this directory (and its ancestors)
      // marked as writing for minutes. Display its current contents immediately;
      // DirectoryCache::Put still rejects snapshots overlapping a mutation.
      // Refresh in place so polling does not add history or clear selection.
      directory_retry_.Start(FROM_HERE, base::Seconds(1),
          base::BindOnce(&FilesView::Navigate, weak_.GetWeakPtr(), result.path, -1, true));
    }
    if (result.ok && (result.sort != found->options.sort || result.hidden != found->options.show_hidden)) {
      const auto sort = found->options.sort; const bool hidden = found->options.show_hidden;
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::TaskPriority::USER_VISIBLE},
          base::BindOnce([](DirectoryResult data, FileSort mode, bool show, CancelFlag canceled) {
            data.order = SortDirectory(SortRecords(data.entries, canceled), mode, show, data.path == TrashRoot(), canceled);
            data.sort = mode; data.hidden = show; data.delta.reset(); return data;
          }, std::move(result), sort, hidden, read_cancel_),
          base::BindOnce(&FilesView::Loaded, weak_.GetWeakPtr(), id, request, history_index, refresh, revalidate));
      return;
    }
    if (result.ok && found->path == result.path && (refresh || revalidate) &&
        (!result.delta || result.base_generation != found->metadata_generation)) {
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&CompareDirectory, found->entries, found->removed, found->metadata_generation, std::move(result), read_cancel_),
          base::BindOnce(&FilesView::Loaded, weak_.GetWeakPtr(), id, request, history_index, refresh, revalidate));
      return;
    }
    pending_navigation_.reset();
    auto& tab = *found; tab.loading = false; tab.error = !result.ok;
#if BUILDFLAG(IS_WIN)
    if (IsDrivesLocation(result.path)) LOG(INFO) << "[crFiles /] directory_result pane=" << id
        << " request=" << request << " ok=" << result.ok << " count=" << result.entries.size()
        << " refresh=" << refresh << " revalidate=" << revalidate;
#endif
    // Cache only worker metadata, before attaching images or moving entries.
    CachedDirectories().Put(result);
    const bool same_directory = result.ok && tab.path == result.path;
    if (same_directory && (refresh || revalidate) && tab.sorted_mode == tab.options.sort &&
        tab.sorted_hidden == tab.options.show_hidden && result.delta->equal) {
      if (!revalidate) {
        tab.failed_thumbnails.clear();
        for (auto& [path, visual] : tab.visuals)
          if (visual.thumbnail.isNull()) visual.thumbnail_attempted = false;
        visual_queue_state_.reset();
      }
      UpdateStatus(); RequestFileVisuals(); return;
    }
    if (revalidate && !result.ok) {
      // Do not leave a deleted/inaccessible directory looking like a valid hit.
      CachedDirectories().Remove(tab.path);
      CancelOrdering(); ClearMaterializedRows();
      CancelFileVisuals(); tab.entries.clear(); tab.visuals.clear(); tab.failed_thumbnails.clear(); tab.removed.clear(); ++tab.metadata_generation; tab.sorted_entries.clear();
      tab.sorted_mode = tab.options.sort; tab.sorted_hidden = tab.options.show_hidden; tab.selected.clear();
    }
    if (result.ok && !refresh && !revalidate && !IsDrivesLocation(result.path)) RecordAccess("access", result.path);
    const bool animate_directory = result.ok && !tab.path.empty() && tab.path != result.path;
    if (result.ok) {
      CancelFileVisuals();
      if (tag_id_ > 0 && restored_scroll_) {
        tab.scroll = *restored_scroll_; pending_scroll_ = *restored_scroll_; restored_scroll_.reset();
      }
      if (same_directory && (refresh || revalidate)) {
        SaveScroll();
        std::erase_if(tab.selected, [&](const auto& path) { return !result.delta->positions.contains(path); });
        std::erase_if(tab.visuals, [&](const auto& item) { return !result.delta->unchanged.contains(item.first); });
        std::erase_if(tab.failed_thumbnails, [&](const auto& path) { return !result.delta->unchanged.contains(path); });
        if (!revalidate) {
          tab.failed_thumbnails.clear();
          for (auto& [path, visual] : tab.visuals)
            if (visual.thumbnail.isNull()) visual.thumbnail_attempted = false;
        }
      } else {
        tab.selected.clear(); tab.visuals.clear(); tab.failed_thumbnails.clear();
        tab.scroll = restored_scroll_.value_or(0); restored_scroll_.reset();
      }
      CancelOrdering(); ClearMaterializedRows(true);
      tab.path = result.path; tab.entries = std::move(result.entries); tab.removed.clear(); ++tab.metadata_generation;
      tab.sorted_entries = result.order.values(); tab.sorted_mode = result.sort; tab.sorted_hidden = result.hidden;
      if (history_index >= 0) tab.history_index = history_index;
      else if (!refresh) {
        tab.history.resize(tab.history_index + 1);
        tab.history.push_back(tab.path); tab.history_index = tab.history.size() - 1;
      }
    }
    if (tag_id_ >= 0) tab.sidebar_selection = IDS_CRFILES_TAGS;
    else SelectSidebarForPath(tab);
    root_->PaneNavigated(this);
    if (Current().id == id) {
      directory_animation_pending_ = animate_directory && GetFileViewPreferences().animate_folders;
      Render();
    } else RenderTabs();
  }
  void History(int delta) {
    if (ActivePane()->network_view_) { NetworkPageGo(ActivePane()->network_view_, delta); return; }
    if (ActivePane()->history_page_) return;
    if (!root_) { ActivePane()->History(delta); return; }
    if (!tag_history_.empty()) {
      const int index = tag_history_index_ + delta;
      if (index >= 0 && index < static_cast<int>(tag_history_.size())) NavigateTag(tag_history_[index], index);
      return;
    }
    int index = Current().history_index + delta;
    if (index >= 0 && index < static_cast<int>(Current().history.size())) Navigate(Current().history[index], index);
  }
  void Refresh() { if (ActivePane()->network_view_) { RefreshNetworkPage(ActivePane()->network_view_); return; } if (ActivePane()->IsTagListing()) { ActivePane()->LoadTagFiles(); return; } if (ActivePane()->search_view_) RefreshSearchPage(ActivePane()->search_view_); else if (ActivePane()->history_page_) ActivePane()->LoadHistory(); else Navigate(ActivePane()->Current().path, -1, true); }
  void ToggleViewMode() {
    if (!root_) { ActivePane()->ToggleViewMode(); return; }
    if (network_view_) {
      CloseViewOptions();
      Current().options.icons = !Current().options.icons;
      SetNetworkPageLayout(network_view_, Current().options.icons, kFileIconSizes[Current().options.icon_index],
          Current().options.show_hidden, Current().options.NetworkColumnOptions(), static_cast<int>(Current().options.sort));
      root_->SyncNavigation(); root_->ScheduleWorkspaceSave(); return;
    }
    CloseViewOptions();
    CancelFileVisuals();
    Current().options.icons = !Current().options.icons; Current().scroll = 0;
    root_->SyncNavigation(); RenderFiles();
  }
  void CloseViewOptions() {
    if (options_delegate_) options_delegate_->SetAnchorView(nullptr);
    if (options_widget_) {
      options_widget_->MakeCloseSynchronous({}); options_widget_.reset();
    }
    options_delegate_.reset();
  }
  void OptionsClosed(views::Widget::ClosedReason) {
    options_delegate_->SetAnchorView(nullptr);
    options_widget_.reset();
    // The delegate stays alive until the close callback stack unwinds.
  }
  void ResizeOptions() { if (options_widget_) options_delegate_->SizeToContents(); }
  void ViewOptionsChanged(int id, FileViewOptions options) {
    auto found = std::ranges::find(tabs_, id, &Tab::id);
    if (found == tabs_.end()) return;
    if (network_view_) {
      found->options = options;
      if (Current().id == id) SetNetworkPageLayout(network_view_, options.icons,
          kFileIconSizes[options.icon_index], options.show_hidden, options.NetworkColumnOptions(), static_cast<int>(options.sort));
      root_->ScheduleWorkspaceSave(); return;
    }
    auto previous = found->options;
    previous.icon_index = options.icon_index;
    if (previous == options) {
      found->options = options;
      if (Current().id == id) UpdateIconSize();
      return;
    }
    found->options = options; found->scroll = 0;
    if (!options.show_hidden) for (const auto& entry : found->entries)
      if (entry.hidden) found->selected.erase(entry.path);
    if (Current().id == id) RenderFiles();
  }
  bool AdjustIconSize(int delta) {
    auto* pane = ActivePane();
    if (!pane || (pane->history_page_ && !pane->network_view_)) return false;
    for (auto* focus = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
         focus; focus = focus->parent())
      if (views::AsViewClass<views::Textfield>(focus)) return false;
    auto& index = pane->Current().options.icon_index;
    const int next = std::clamp(index + delta, 0, static_cast<int>(kFileIconSizes.size()) - 1);
    if (next == index) return true;
    index = next;
    pane->UpdateIconSize();
    if (pane->options_widget_ && pane->options_delegate_) {
      if (auto* options = views::AsViewClass<ViewOptions>(pane->options_delegate_->GetContentsView()))
        options->SetIconIndex(index);
    }
    return true;
  }
  void UpdateIconSize() {
    if (network_view_) {
      SetNetworkPageLayout(network_view_, Current().options.icons, kFileIconSizes[Current().options.icon_index],
          Current().options.show_hidden, Current().options.NetworkColumnOptions(), static_cast<int>(Current().options.sort));
      root_->ScheduleWorkspaceSave(); return;
    }
    CancelFileVisuals();
    root_->ScheduleWorkspaceSave();
    const auto index = Current().options.icon_index;
    const int size = kFileIconSizes[index];
    for (const auto& [entry, weak_button] : visible_buttons_) {
      if (!weak_button) continue;
      auto* button = static_cast<FileButton*>(weak_button.get());
      button->SetIconSize(size);
      button->SetFileImage(FileImage(*entry));
    }
    if (!Current().options.icons) {
      const int row_height = std::max(36, size + 12);
      for (size_t column = 0; column < column_cells_.size(); ++column) {
        // The first cell is the fixed-height column header.
        for (size_t row = 1; row < column_cells_[column].size(); ++row)
          column_cells_[column][row]->SetPreferredSize(
              gfx::Size(Current().options.column_widths[column], row_height - 8));
      }
    }
    if (files_->contents()) static_cast<FileLayout*>(files_->contents())->SetIconSize(size);
    files_->InvalidateLayout(); RequestFileVisuals();
  }
  void CloseNewFolder() {
    rename_focus_observer_.reset();
    ++new_folder_generation_;
    new_folder_network_url_.clear();
    new_folder_contents_ = nullptr;
    if (new_folder_delegate_) new_folder_delegate_->SetAnchorView(nullptr);
    if (new_folder_widget_) {
      new_folder_widget_->MakeCloseSynchronous({}); new_folder_widget_.reset();
    }
    new_folder_delegate_.reset();
  }
  void ShowNewFolder() {
    if (network_view_) {
      if (!network_new_folder_ || !network_new_folder_->IsDrawn() || !network_new_folder_->GetEnabled()) return;
    } else {
      if (tag_id_ == 0) { ShowFileTagEditor(GetWidget(), {}, base::BindRepeating(&FilesView::LoadTagFiles, weak_.GetWeakPtr())); return; }
      if (IsTagListing()) return;
      if (IsDrivesLocation(Current().path)) return;
    }
    if (new_folder_widget_) { CloseNewFolder(); return; }
    CloseNewFolder();
    new_folder_network_url_ = network_view_ ? NetworkPageUrl(network_view_) : std::string();
    views::View* anchor = network_view_ ? network_new_folder_.get() : new_folder_button_.get();
    new_folder_delegate_ = std::make_unique<views::BubbleDialogDelegate>(anchor, views::BubbleBorder::BOTTOM_LEFT);
    // Visible arrows require a Views-drawn shadow, including on macOS.
    new_folder_delegate_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    new_folder_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    new_folder_delegate_->SetShowTitle(false);
    new_folder_delegate_->SetTitle(Text(IDS_CRFILES_NEW_FOLDER));
    new_folder_delegate_->set_close_on_deactivate(true);
    new_folder_delegate_->set_adjust_if_offscreen(true);
    new_folder_delegate_->set_margins(gfx::Insets());
    auto contents = std::make_unique<NewFolderView>(base::BindRepeating(&FilesView::CreateFolder, weak_.GetWeakPtr()));
    new_folder_contents_ = contents.get();
    new_folder_delegate_->SetInitiallyFocusedView(contents->field());
    new_folder_delegate_->SetContentsView(std::move(contents));
    new_folder_widget_ = CreateCrFilesBubble(new_folder_delegate_.get(),
        base::BindOnce([](base::WeakPtr<FilesView> view, views::Widget::ClosedReason) {
          if (!view) return;
          view->new_folder_contents_ = nullptr;
          view->new_folder_network_url_.clear();
          view->new_folder_delegate_->SetAnchorView(nullptr); view->new_folder_widget_.reset(); view->UpdateFooterVisibility();
        }, weak_.GetWeakPtr()));
    new_folder_delegate_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    new_folder_delegate_->SizeToContents();
    new_folder_widget_->Show(); UpdateFooterVisibility();
  }
  void CreateFolder(std::u16string name) {
    if (network_view_) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&FilesView::CreateSharedFolder, weak_.GetWeakPtr(), new_folder_network_url_, new_folder_generation_, std::move(name)));
      return;
    }
    if (IsTagListing()) return;
    if (IsDrivesLocation(Current().path)) return;
    const auto child = base::FilePath::FromUTF16Unsafe(name);
    if (!IsValidEntryName(name)) {
      new_folder_contents_->Failed(); new_folder_delegate_->SizeToContents(); return;
    }
    const auto parent = Current().path;
    const auto path = parent.Append(child);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](base::FilePath path) -> std::optional<Entry> {
          ScopedFileCacheMutation mutation({path});
          FileOperationLog log("create", "menu", {}, path.AsUTF8Unsafe());
          if (!log.valid()) return std::nullopt;
          if (base::PathExists(path) || base::IsLink(path) || !base::CreateDirectory(path)) return std::nullopt;
          log.Succeed();
          base::File::Info info;
          if (!base::GetFileInfo(path, &info)) return std::nullopt;
          Entry entry{path, true, info.size, info.last_modified, "inode/directory", path.BaseName().AsUTF8Unsafe().starts_with(".")};
          ReadExtraMetadata(entry);
          return entry;
        }, path),
        base::BindOnce(&FilesView::FolderCreated, weak_.GetWeakPtr(), Current().id, parent, new_folder_generation_));
  }
  void CreateSharedFolder(std::string url, uint64_t generation, std::u16string name) {
    if (generation != new_folder_generation_ || !new_folder_contents_) return;
    if (!network_view_ || url.empty() || url != NetworkPageUrl(network_view_)) { CloseNewFolder(); return; }
    CreateNetworkFolder(network_view_, std::move(name),
        base::BindOnce(&FilesView::SharedFolderCreated, weak_.GetWeakPtr(), url, generation));
  }
  void SharedFolderCreated(std::string url, uint64_t generation, mojom::SmbError error) {
    if (!network_view_ || url != NetworkPageUrl(network_view_)) return;
    if (generation != new_folder_generation_ || !new_folder_contents_) return;
    if (error == mojom::SmbError::kOk) {
      CloseNewFolder();
      if (root_->ActivePane() == this && IsDrawn()) network_view_->RequestFocus();
      return;
    }
    const int message = error == mojom::SmbError::kAlreadyExists ? IDS_CRFILES_NETWORK_EXISTS :
        error == mojom::SmbError::kInvalidAddress ? IDS_CRFILES_NEW_FOLDER_ERROR :
        error == mojom::SmbError::kAccessDenied ? IDS_CRFILES_NETWORK_ERROR : IDS_CRFILES_NETWORK_OPERATION_ERROR;
    new_folder_contents_->Failed(Text(message)); new_folder_delegate_->SizeToContents();
  }
  void FolderCreated(int id, base::FilePath parent, uint64_t generation, std::optional<Entry> entry) {
    if (Current().id != id || Current().path != parent) return;
    if (!entry) {
      if (generation == new_folder_generation_ && new_folder_contents_) { new_folder_contents_->Failed(); new_folder_delegate_->SizeToContents(); }
      return;
    }
    if (generation == new_folder_generation_) CloseNewFolder();
    for (auto& tab : root_->tabs_) root_->VisitPanes(tab.pane_host,
        base::BindRepeating([](const base::FilePath& folder, const Entry& created, FilesView* source, FilesView* pane) {
          if (pane->Current().path != folder) return;
          pane->SaveScroll();
          if (pane == source) pane->Current().selected = {created.path};
          pane->QueueLiveRead();
        }, parent, *entry, base::Unretained(this)));
    if (root_->ActivePane() == this && IsDrawn()) files_->RequestFocus();
  }
  void ShowViewOptions() {
    if (!root_) { ActivePane()->ShowViewOptions(); return; }
    if (options_widget_) { CloseViewOptions(); return; }
    CloseViewOptions();
    options_delegate_ = std::make_unique<views::BubbleDialogDelegate>(root_ ? root_->options_button_.get() : options_button_.get(), views::BubbleBorder::TOP_RIGHT);
    options_delegate_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
    options_delegate_->SetTitle(Text(IDS_CRFILES_VIEW_OPTIONS));
    options_delegate_->SetShowTitle(false); options_delegate_->SetShowCloseButton(false);
    options_delegate_->set_close_on_deactivate(true); options_delegate_->set_margins(gfx::Insets(8));
    options_delegate_->set_adjust_if_offscreen(true);
    auto page = ViewOptionsPage::kLocal;
    if (network_view_) {
      const auto location = ParseSmbLocation(NetworkPageUrl(network_view_));
      page = location && !location->share.empty() ? ViewOptionsPage::kNetworkDirectory : ViewOptionsPage::kNetworkRoot;
    }
    options_delegate_->SetContentsView(std::make_unique<ViewOptions>(Current().options,
        base::BindRepeating(&FilesView::ViewOptionsChanged, weak_.GetWeakPtr(), Current().id),
        base::BindRepeating(&FilesView::ResizeOptions, weak_.GetWeakPtr()), page));
    options_widget_ = CreateCrFilesBubble(options_delegate_.get(),
        base::BindOnce(&FilesView::OptionsClosed, weak_.GetWeakPtr()));
    options_widget_->Show();
  }
  void TransitionPath(bool editing) {
    if (address_editing_ == editing) return;
    address_editing_ = editing;
    path_animation_timer_.Stop();
    views::View* incoming = editing ? static_cast<views::View*>(address_.get()) : crumbs_scroll_.get();
    views::View* outgoing = editing ? static_cast<views::View*>(crumbs_scroll_.get()) : address_.get();
    incoming->SetBoundsRect(outgoing->bounds());
    incoming->SetVisible(true);
    incoming->SetCanProcessEventsWithinSubtree(true);
    outgoing->SetCanProcessEventsWithinSubtree(false);
    address_->SetFocusBehavior(editing ? views::View::FocusBehavior::ALWAYS : views::View::FocusBehavior::NEVER);
    // Both controls occupy the same fixed capsule; surrounding buttons stay put.
    AnimateArrival(incoming);
    outgoing->layer()->GetAnimator()->StopAnimating();
    {
      ui::ScopedLayerAnimationSettings settings(outgoing->layer()->GetAnimator());
      settings.SetTransitionDuration(base::Milliseconds(160));
      settings.SetTweenType(gfx::Tween::EASE_OUT);
      outgoing->layer()->SetOpacity(0.f);
      outgoing->layer()->SetTransform(CenteredScale(outgoing, 0.98f));
    }
    path_animation_timer_.Start(FROM_HERE, base::Milliseconds(160),
        base::BindOnce(&FilesView::FinishPathTransition, weak_.GetWeakPtr()));
    InvalidateLayout();
  }
  void FinishPathTransition() {
    views::View* outgoing = address_editing_ ? static_cast<views::View*>(crumbs_scroll_.get()) : address_.get();
    outgoing->SetVisible(false);
  }
  void EndAddressEditing() { TransitionPath(false); }
  void EditAddress() {
    if (!toolbar_ || !toolbar_->GetVisible() || ActivePane()->tag_id_ >= 0) return;
    if (!address_editing_) {
      address_->SetInvalid(false);
      address_->SetText(ActivePane()->network_view_ ? base::UTF8ToUTF16(NetworkPageUrl(ActivePane()->network_view_)) : NavigationPath(ActivePane()->Current().path));
      TransitionPath(true);
    }
    address_->RequestFocus(); address_->SelectAll(false);
  }
  ui::ImageModel TabLocationIcon(const Tab& tab) {
    const auto vector = [](const gfx::VectorIcon& icon) {
      return ui::ImageModel::FromVectorIcon(icon, ui::kColorSysOnSurfaceSecondary, kTabIconSize);
    };
    const auto network = [&] {
      return network_tab_icon_.IsEmpty() ? vector(vector_icons::kDevicesIcon) : network_tab_icon_;
    };
    auto path = tab.path;
    if (const auto* pane = tab.active_pane.get()) {
      if (pane->network_page_) return network();
      if (pane->IsTagListing()) return vector(kTagsIcon);
      if (pane->search_page_) return vector(vector_icons::kSearchIcon);
      if (pane->history_page_ && pane->tag_id_ < 0) return vector(vector_icons::kHistoryIcon);
      path = pane->Current().path;
    } else if (tab.deferred_tree) {
      const auto* saved = SavedActivePane(*tab.deferred_tree, tab.deferred_active);
      if (!saved) saved = SavedActivePane(*tab.deferred_tree, {});
      if (saved) {
        if (saved->FindBool("network_page").value_or(false)) return network();
        const auto* tag_root = saved->FindString("tag_root");
        if (saved->FindString("tag_id") && (!tag_root || tag_root->empty())) return vector(kTagsIcon);
        if (saved->FindBool("search_page").value_or(false)) return vector(vector_icons::kSearchIcon);
        if (saved->FindBool("history_page").value_or(false) && !saved->FindString("tag_id"))
          return vector(vector_icons::kHistoryIcon);
      }
    }
    if (path == TrashRoot() || TrashRoot().IsParent(path))
      return trash_tab_icon_.IsEmpty() ? vector(kDeleteIcon) : trash_tab_icon_;
    if (IsDrivesLocation(path)) return vector(vector_icons::kDevicesIcon);
    if (!path.empty()) {
      if (path == Home()) return vector(vector_icons::kHomeIcon);
      if (path == DesktopPath()) return vector(vector_icons::kDesktopWindowsIcon);
      if (path == NearbySharing::DefaultReceiveDirectory().DirName()) return vector(vector_icons::kDownloadIcon);
    }
    return vector(vector_icons::kFolderOpenIcon);
  }
  void UpdateTabStates() {
    if (!tabs_view_) return;
    for (size_t index = 0; index < tabs_.size() && index < tabs_view_->children().size(); ++index) {
      auto* pane = tabs_[index].active_pane.get();
      auto* button = views::AsViewClass<TabButton>(tabs_view_->children()[index].get());
      if (!button) continue;
      button->SetLocationIcon(TabLocationIcon(tabs_[index]));
      if (pane && pane->search_view_) button->SetSearchState(GetSearchPageState(pane->search_view_));
    }
  }
  void TabMenuAction(int id, int command) {
    auto found = std::ranges::find(tabs_, id, &Tab::id);
    if (found == tabs_.end()) return;
    if (command == 0) { NewCurrentTab(); return; }
    if (command == 2) { CloseTab(id); return; }
    if (command == 1) {
      const int selected = Current().id;
      found->pinned = !found->pinned;
      std::stable_partition(tabs_.begin(), tabs_.end(), [](const Tab& tab) { return tab.pinned; });
      for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].id == selected) active_ = i;
      rendered_tab_id_ = -1;
      RenderTabs(); ScheduleWorkspaceSave();
    } else if (command == 3) {
      SelectTab(id);
      std::vector<int> closing;
      for (const auto& tab : tabs_) if (tab.id != id && !tab.pinned) closing.push_back(tab.id);
      for (int tab_id : closing) CloseTab(tab_id);
    }
  }
  void ReorderTabs(size_t from, size_t to) {
    if (from >= tabs_.size() || to >= tabs_.size() || from == to) return;
    const int selected = Current().id;
    if (from < to) {
      std::rotate(tabs_.begin() + from, tabs_.begin() + from + 1, tabs_.begin() + to + 1);
      std::rotate(rendered_tab_titles_.begin() + from, rendered_tab_titles_.begin() + from + 1,
                  rendered_tab_titles_.begin() + to + 1);
    } else {
      std::rotate(tabs_.begin() + to, tabs_.begin() + from, tabs_.begin() + from + 1);
      std::rotate(rendered_tab_titles_.begin() + to, rendered_tab_titles_.begin() + from,
                  rendered_tab_titles_.begin() + from + 1);
    }
    for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].id == selected) active_ = i;
    ScheduleWorkspaceSave();
  }
  void RenderTabs() {
    if (!tabs_view_) return;
    std::vector<std::pair<int, std::u16string>> titles;
    for (const auto& tab : tabs_) titles.emplace_back(tab.id,
        tab.active_pane ? base::UTF8ToUTF16(tab.active_pane->PaneName()) :
        (tab.deferred_title.empty() ? Text(IDS_CRFILES_NEW_TAB) : base::UTF8ToUTF16(tab.deferred_title)));
    if (rendered_tab_id_ == Current().id && rendered_tab_titles_ == titles) { UpdateTabStates(); return; }
    rendered_tab_id_ = Current().id; rendered_tab_titles_ = std::move(titles);
    active_tab_ = nullptr;
    static_cast<TabStrip*>(tabs_view_.get())->ResetDrag();
    tabs_view_->RemoveAllChildViews();
    for (const auto& tab : tabs_) {
      auto title = tab.active_pane ? base::UTF8ToUTF16(tab.active_pane->PaneName()) :
        (tab.deferred_title.empty() ? Text(IDS_CRFILES_NEW_TAB) : base::UTF8ToUTF16(tab.deferred_title));
      auto* button = tabs_view_->AddChildView(std::make_unique<TabButton>(
          Deferred(base::BindRepeating(&FilesView::SelectTab, weak_.GetWeakPtr(), tab.id)),
          Deferred(base::BindRepeating(&FilesView::CloseTab, weak_.GetWeakPtr(), tab.id)), title, tab.id == Current().id));
      button->SetPreferredSize(gfx::Size(kTabPreferredWidth, 36));
      button->SetLocationIcon(TabLocationIcon(tab));
      button->SetTabMenu(tab.pinned,
          base::BindRepeating(&FilesView::TabMenuAction, weak_.GetWeakPtr(), tab.id));
      button->SetPaintToLayer();
      button->layer()->SetFillsBoundsOpaquely(false);
      button->SetDragCallback(base::BindRepeating(&TabStrip::Drag,
          base::Unretained(static_cast<TabStrip*>(tabs_view_.get())), base::Unretained(button)));
      if (tab.active_pane && tab.active_pane->search_view_) button->SetSearchState(GetSearchPageState(tab.active_pane->search_view_));
      if (tab.id == Current().id) active_tab_ = button;
    }
    tab_reveal_pending_ = true;
    static_cast<TabStrip*>(tabs_view_.get())->TabsChanged();
  }
  void TabsLaidOut(views::ScrollView* viewport) {
    if (!tabs_view_ || !active_tab_) return;
    const auto visible = viewport->GetVisibleRect();
    if (visible.width() <= 0 || active_tab_->bounds().IsEmpty()) return;
    if (tab_viewport_width_ != visible.width()) {
      tab_viewport_width_ = visible.width();
      tab_reveal_pending_ = true;
    }
    if (!tab_reveal_pending_ || static_cast<TabStrip*>(tabs_view_.get())->IsDraggingTab()) return;
    const auto target = active_tab_->bounds();
    int offset = visible.x();
    if (target.right() > visible.right()) offset = target.right() - visible.width();
    if (target.x() < offset) offset = target.x();
    viewport->ScrollToOffset(gfx::PointF(std::max(0, offset), 0));
    tab_reveal_pending_ = !viewport->GetVisibleRect().Contains(target);
  }
  // Rebuilding controls is deferred until the originating button dispatch ends.
  base::RepeatingClosure Deferred(base::RepeatingClosure action) {
    return base::BindRepeating([](base::RepeatingClosure callback) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, callback);
    }, std::move(action));
  }
  void Render() {
    if (!root_) { SyncNavigation(); return; }
    RenderFiles();
  }
  void PaneNavigated(FilesView* pane) {
    ScheduleWorkspaceSave();
    for (auto& tab : tabs_) if (tab.active_pane == pane) tab.path = pane->Current().path;
    if (ActivePane() == pane) SyncNavigation(); else RenderTabs();
  }
  void SyncNavigation() {
    if (tabs_.empty() || !Current().active_pane) return;
    auto* pane = ActivePane();
    crumbs_scroll_->SetEditingAllowed(pane->tag_id_ < 0);
    // The outer inset belongs to navigation chrome; each pane paints its own
    // content surface. Keep the sidebar and toolbar consistent across tabs.
    SetBackground(std::make_unique<NavigationBackground>());
    toolbar_->SetVisible(pane->HasNavigationChrome());
    view_toggle_->SetEnabled(true);
    options_button_->SetEnabled(true);
    if (!pane->HasNavigationChrome() && sidebar_drag_start_) {
      sidebar_drag_start_.reset();
      AnimateSidebarTo(sidebar_display_width_ < 110 ? 0 : std::clamp(sidebar_display_width_, 220, 360));
    }
    UpdateSidebarPresentation();
    Current().path = pane->Current().path;
    UpdateSidebarHighlight(); RenderTabs();
    back_->SetEnabled(pane->tag_history_.empty() ? pane->Current().history_index > 0 : pane->tag_history_index_ > 0);
    forward_->SetEnabled(pane->tag_history_.empty() ?
        pane->Current().history_index + 1 < static_cast<int>(pane->Current().history.size()) :
        pane->tag_history_index_ + 1 < static_cast<int>(pane->tag_history_.size()));
    VisitPanes(Current().pane_host, base::BindRepeating([](FilesView* leaf) { leaf->FileViewportChanged(); }));
    if (pane->network_view_) {
      address_completion_->Cancel();
      const bool icons = pane->Current().options.icons;
      view_toggle_->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(
          icons ? kGridViewIcon : kListAltIcon, ui::kColorSysOnSurface, 20));
      view_toggle_->SetTooltipText(Text(icons ? IDS_CRFILES_LIST : IDS_CRFILES_ICONS));
      view_toggle_->SetAccessibleName(Text(icons ? IDS_CRFILES_LIST : IDS_CRFILES_ICONS));
      back_->SetEnabled(NetworkPageCanGo(pane->network_view_, -1));
      forward_->SetEnabled(NetworkPageCanGo(pane->network_view_, 1));
      const auto url = NetworkPageUrl(pane->network_view_);
      if (rendered_network_url_ != url) pane->CloseViewOptions();
      const auto style = GetNavigationStyle();
      if (rendered_network_url_ == url && rendered_crumb_pane_ == pane->pane_id_ && rendered_navigation_style_ == style) return;
      rendered_network_url_ = url; rendered_crumb_pane_ = pane->pane_id_; rendered_navigation_style_ = style;
      network_address_anchor_.SetView(nullptr);
      rendered_crumb_path_ = {}; rendered_crumb_buttons_.clear();
      auto crumbs = std::make_unique<views::View>();
      auto* layout = crumbs->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal,
          gfx::Insets::VH(2, style == NavigationStyle::kConnected ? 2 : 0), 0));
      layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
      if (style == NavigationStyle::kConnected) crumbs->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysHeaderContainerInactive, 8));
      auto* network = crumbs->AddChildView(std::make_unique<BreadcrumbButton>(
          Deferred(base::BindRepeating(&FilesView::ShowNetworkTab, weak_.GetWeakPtr())),
          std::u16string(), url.empty(), true, style, false));
      network_breadcrumb_.SetView(network);
      network->SetAccessibleName(Text(IDS_CRFILES_NETWORK));
      network->SetTooltipText(Text(IDS_CRFILES_NETWORK));
      network->SetHorizontalAlignment(gfx::ALIGN_CENTER); network->SetImageCentered(true);
      const auto color = url.empty() && (style == NavigationStyle::kRaised || style == NavigationStyle::kConnected) ?
          ui::kColorSysOnPrimaryContainer : ui::kColorSysOnSurface;
      for (auto state : {views::Button::STATE_NORMAL, views::Button::STATE_HOVERED,
                         views::Button::STATE_PRESSED, views::Button::STATE_DISABLED}) {
        network->SetImageModel(state, network_location_icon_.IsEmpty() ?
            ui::ImageModel::FromVectorIcon(vector_icons::kDevicesIcon,
                state == views::Button::STATE_DISABLED ? ui::kColorSysStateDisabled : color, 20) : network_location_icon_);
      }
      const auto location = ParseSmbLocation(url);
      if (location) {
        SmbLocation segment{location->server, {}, {}};
        auto add = [&](std::u16string title) {
          if (style == NavigationStyle::kConnected) crumbs->AddChildView(std::make_unique<BreadcrumbDivider>());
          else if (style != NavigationStyle::kRaised) {
            auto* divider = crumbs->AddChildView(std::make_unique<views::Label>(base::i18n::IsRTL() ? u"‹" : u"›"));
            divider->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 3)));
          }
          auto* button = crumbs->AddChildView(std::make_unique<BreadcrumbButton>(Deferred(base::BindRepeating(
              [](base::WeakPtr<FilesView> pane, std::string url) { if (pane) NavigateNetworkPage(pane->network_view_, url); },
              pane->weak_.GetWeakPtr(), segment.Url())), title, segment.Url() == url, false, style, false));
          button->SetTooltipText(base::UTF8ToUTF16(segment.Url()));
          if (segment.share.empty()) network_address_anchor_.SetView(button->NameAnchor());
        };
        add(base::UTF8ToUTF16(segment.Url()));
        if (!location->share.empty()) {
          segment.share = location->share; add(base::UTF8ToUTF16(segment.share));
          for (const auto& part : base::SplitString(location->path, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
            if (!segment.path.empty()) segment.path += '/';
            segment.path += part; add(base::UTF8ToUTF16(part));
          }
        }
      }
      crumbs_scroll_->SetContents(std::move(crumbs));
      return;
    }
    rendered_network_url_.clear();
    network_address_anchor_.SetView(nullptr);
    if (pane->tag_id_ >= 0) {
      rendered_crumb_path_ = {}; rendered_crumb_pane_.clear(); rendered_crumb_buttons_.clear();
      view_toggle_->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(
          pane->Current().options.icons ? kGridViewIcon : kListAltIcon, ui::kColorSysOnSurface, 20));
      view_toggle_->SetTooltipText(Text(pane->Current().options.icons ? IDS_CRFILES_LIST : IDS_CRFILES_ICONS));
      view_toggle_->SetAccessibleName(Text(pane->Current().options.icons ? IDS_CRFILES_LIST : IDS_CRFILES_ICONS));
      const auto style = GetNavigationStyle();
      auto crumbs = std::make_unique<views::View>();
      auto* layout = crumbs->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal,
          gfx::Insets::VH(2, style == NavigationStyle::kConnected ? 2 : 0), 0));
      layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
      if (style == NavigationStyle::kConnected)
        crumbs->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysHeaderContainerInactive, 8));
      auto* root = crumbs->AddChildView(std::make_unique<BreadcrumbButton>(
          Deferred(base::BindRepeating(&FilesView::NavigateTagRoot, weak_.GetWeakPtr())),
          Text(IDS_CRFILES_TAGS), pane->tag_id_ == 0, true, style, false));
      root->SetTooltipText(Text(IDS_CRFILES_TAGS));
      root->SetImageLabelSpacing(6);
      const auto color = pane->tag_id_ == 0 &&
          (style == NavigationStyle::kRaised || style == NavigationStyle::kConnected) ?
          ui::kColorSysOnPrimaryContainer : ui::kColorSysOnSurface;
      for (auto state : {views::Button::STATE_NORMAL, views::Button::STATE_HOVERED,
                         views::Button::STATE_PRESSED, views::Button::STATE_DISABLED})
        root->SetImageModel(state, ui::ImageModel::FromVectorIcon(kTagsIcon,
            state == views::Button::STATE_DISABLED ? ui::kColorSysStateDisabled : color, 20));
      auto add_segment = [&](std::u16string title, TagLocation location, bool current) {
        if (style == NavigationStyle::kConnected) {
          crumbs->AddChildView(std::make_unique<BreadcrumbDivider>());
        } else if (style != NavigationStyle::kRaised) {
          auto* divider = crumbs->AddChildView(std::make_unique<views::Label>(base::i18n::IsRTL() ? u"‹" : u"›"));
          divider->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
          divider->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 3)));
        }
        auto* segment = crumbs->AddChildView(std::make_unique<BreadcrumbButton>(
            Deferred(base::BindRepeating(&FilesView::NavigateTag, pane->weak_.GetWeakPtr(), location, -1)),
            title, current, false, style, false));
        segment->SetTooltipText(location.path.empty() ? title : location.path.AsUTF16Unsafe());
      };
      if (pane->tag_id_ > 0) {
        add_segment(base::UTF8ToUTF16(pane->tag_name_), {pane->tag_id_, pane->tag_name_, {}, {}}, pane->IsTagListing());
        std::vector<base::FilePath> parents;
        if (!pane->tag_directory_root_.empty()) {
          for (auto path = pane->Current().path; path == pane->tag_directory_root_ || pane->tag_directory_root_.IsParent(path); path = path.DirName()) {
            parents.push_back(path);
            if (path == pane->tag_directory_root_) break;
          }
        }
        std::ranges::reverse(parents);
        for (const auto& path : parents)
          add_segment(EntryName(path), {pane->tag_id_, pane->tag_name_, pane->tag_directory_root_, path}, path == pane->Current().path);
      }
      crumbs_scroll_->SetContents(std::move(crumbs));
      back_->SetEnabled(pane->tag_history_index_ > 0);
      forward_->SetEnabled(pane->tag_history_index_ + 1 < static_cast<int>(pane->tag_history_.size()));
      return;
    }
    const auto navigation_style = GetNavigationStyle();
    const bool path_changed = rendered_crumb_path_ != pane->Current().path;
    if (!crumbs_scroll_->contents() || path_changed || rendered_navigation_style_ != navigation_style) {
      const bool animate_current = !rendered_crumb_path_.empty() && path_changed;
      rendered_crumb_buttons_.clear();
      rendered_navigation_style_ = navigation_style;
      rendered_crumb_pane_ = pane->pane_id_; rendered_crumb_path_ = pane->Current().path;
    auto crumbs = std::make_unique<views::View>();
    auto* crumb_layout = crumbs->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal,
        gfx::Insets::VH(2, navigation_style == NavigationStyle::kConnected ? 2 : 0), 0));
    if (navigation_style == NavigationStyle::kConnected)
      crumbs->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysHeaderContainerInactive, 8));
    crumb_layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    std::vector<base::FilePath> parents;
    for (auto path = pane->Current().path; !path.empty();) {
      parents.push_back(path);
      if (IsDrivesLocation(path)) break;
      if (IsDriveRoot(path)) { parents.emplace_back(FILE_PATH_LITERAL("/")); break; }
      if (path.DirName() == path || path == TrashRoot()) break;
      if (path.DirName().DirName().DirName() == TrashRoot() && path.DirName().BaseName().value() == FILE_PATH_LITERAL("files")) {
        parents.push_back(TrashRoot()); break;
      }
      path = path.DirName();
    }
    if (TrashRoot() == pane->Current().path) parents = {pane->Current().path};
    std::ranges::reverse(parents);
    for (const auto& path : parents) {
      if (navigation_style == NavigationStyle::kConnected && path != parents.front()) {
        crumbs->AddChildView(std::make_unique<BreadcrumbDivider>());
      } else if (navigation_style != NavigationStyle::kRaised && path != parents.front()) {
        auto* divider = crumbs->AddChildView(std::make_unique<views::Label>(base::i18n::IsRTL() ? u"‹" : u"›"));
        divider->SetSubpixelRenderingEnabled(false);
        divider->SetEnabledColor(ui::kColorSysOnSurfaceSecondary);
        divider->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 3)));
        divider->SetCanProcessEventsWithinSubtree(false);
      }
      auto* crumb = crumbs->AddChildView(std::make_unique<BreadcrumbButton>(
          Deferred(base::BindRepeating(&FilesView::Navigate, pane->weak_.GetWeakPtr(), path, -1, false)),
          (path == TrashRoot() ? Text(IDS_CRFILES_TRASH) : BreadcrumbName(path)), path == pane->Current().path, path == parents.front(), navigation_style, animate_current));
      rendered_crumb_buttons_.emplace_back(path, crumb->GetWeakPtr());
      if (path != TrashRoot() && BreadcrumbName(path) == u"/") {
        crumb->SetText(std::u16string());
        crumb->SetAccessibleName(NavigationPath(path));
        const auto color = (navigation_style == NavigationStyle::kRaised ||
                            navigation_style == NavigationStyle::kConnected) && path == pane->Current().path ?
            ui::kColorSysOnPrimaryContainer : ui::kColorSysOnSurface;
        for (auto state : {views::Button::STATE_NORMAL, views::Button::STATE_HOVERED,
                           views::Button::STATE_PRESSED, views::Button::STATE_DISABLED}) {
          crumb->SetImageModel(state, ui::ImageModel::FromVectorIcon(kBreadcrumbDiskIcon,
              state == views::Button::STATE_DISABLED ? ui::kColorSysStateDisabled : color, 20));
        }
      }
      crumb->SetTooltipText(NavigationPath(path));
    }
    crumbs_scroll_->SetContents(std::move(crumbs));
    } else if (rendered_crumb_pane_ != pane->pane_id_) {
      // Keep the visual state and scroll position for identical paths, but
      // route subsequent clicks to the newly active pane.
      for (const auto& [path, button] : rendered_crumb_buttons_) {
        if (button) static_cast<BreadcrumbButton*>(button.get())->SetCallback(
            Deferred(base::BindRepeating(&FilesView::Navigate, pane->weak_.GetWeakPtr(), path, -1, false)));
      }
      rendered_crumb_pane_ = pane->pane_id_;
    }
    const bool icons = pane->Current().options.icons;
    view_toggle_->SetImageModel(views::Button::STATE_NORMAL, ui::ImageModel::FromVectorIcon(
        icons ? kGridViewIcon : kListAltIcon, ui::kColorSysOnSurface, 20));
    view_toggle_->SetTooltipText(Text(icons ? IDS_CRFILES_LIST : IDS_CRFILES_ICONS));
    view_toggle_->SetAccessibleName(Text(icons ? IDS_CRFILES_LIST : IDS_CRFILES_ICONS));
  }
  void SplitResizeChanged(bool resizing) {
    split_resize_active_ = resizing;
    VisitPanes(Current().pane_host, base::BindRepeating([](bool active, FilesView* pane) {
      if (pane->history_page_ || !pane->files_->contents()) return;
      auto* layout = views::AsViewClass<FileLayout>(pane->files_->contents());
      if (!layout) return;
      if (active) {
        const auto viewport = pane->files_->GetVisibleRect();
        const auto [first, last] = layout->ItemsInRect(viewport);
        if (first < last)
          pane->resize_anchor_ = std::make_pair(first, viewport.y() - layout->ItemBounds(first).y());
        pane->row_details_timer_.Stop();
      }
      pane->split_resize_active_ = active;
      layout->SetResizing(active);
      pane->files_->InvalidateLayout();
      pane->FileViewportChanged();
    }, resizing));
  }
  void SaveScroll() {
    if (!pending_scroll_ && files_->contents()) Current().scroll = files_->GetVisibleRect().y();
  }
  void RestoreScroll(views::ScrollView* scroll) {
    if (resize_anchor_ && !pending_scroll_) {
      if (auto* layout = views::AsViewClass<FileLayout>(scroll->contents())) {
        const auto [index, offset] = *resize_anchor_;
        scroll->ScrollToPosition(scroll->vertical_scroll_bar(),
            std::max(0, layout->ItemBounds(index).y() + offset));
      }
      if (!split_resize_active_) resize_anchor_.reset();
    }
    if (pending_scroll_) {
      resize_anchor_.reset();
      const int position = *pending_scroll_; pending_scroll_.reset();
      scroll->ScrollToPosition(scroll->vertical_scroll_bar(), position);
    }
    if (auto* layout = views::AsViewClass<FileLayout>(scroll->contents()))
      layout->CommitColumnTransition(scroll->GetVisibleRect());
    if (directory_animation_pending_) {
      directory_animation_pending_ = false;
      // Keep directory contents visible throughout the transition. Fading the
      // whole scroll surface from zero makes breadcrumb navigation flash.
      if (GetFileViewPreferences().animate_folders) AnimateArrival(file_surface_, 0.85f);
    }
    if (root_ && !history_page_) FileViewportChanged();
  }
  int ListMinimumWidth() const {
    const auto& o = tabs_[active_].options;
    return o.column_widths[0] + (o.size_column ? o.column_widths[1] : 0) +
        (o.type_column ? o.column_widths[2] : 0) + (o.modified_column ? o.column_widths[3] : 0) +
        (o.created_column ? o.column_widths[4] : 0) + (o.permissions_column ? o.column_widths[5] : 0);
  }
  void SortByColumn(size_t index) {
    ActivateSelf();
    CloseViewOptions();
    auto options = Current().options;
    switch (index) {
      case 0:
        options.sort = options.sort == FileSort::kNameAscending ?
            FileSort::kNameDescending : FileSort::kNameAscending;
        break;
      case 1:
        options.sort = options.sort == FileSort::kSize ? FileSort::kSizeAscending : FileSort::kSize;
        break;
      case 2:
        options.sort = options.sort == FileSort::kType ? FileSort::kTypeDescending : FileSort::kType;
        break;
      case 3:
        options.sort = options.sort == FileSort::kModifiedDescending ?
            FileSort::kModifiedAscending : FileSort::kModifiedDescending;
        break;
      case 4:
        options.sort = options.sort == FileSort::kCreatedAscending ? FileSort::kCreatedDescending : FileSort::kCreatedAscending;
        break;
      case 5:
        options.sort = options.sort == FileSort::kPermissionsAscending ? FileSort::kPermissionsDescending : FileSort::kPermissionsAscending;
        break;
      default: return;
    }
    ViewOptionsChanged(Current().id, options);
  }
  void ResizeColumn(size_t index, int width) {
    if (Current().options.column_widths[index] == width) return;
    Current().options.column_widths[index] = width;
    for (const auto& cell : column_cells_[index])
      cell->SetPreferredSize(gfx::Size(width, cell->height()));
    if (files_->contents()) static_cast<FileLayout*>(files_->contents())->SetMinimumWidth(ListMinimumWidth());
    files_->InvalidateLayout(); root_->ScheduleWorkspaceSave();
  }
  bool FileKeyPressed(const ui::KeyEvent& event) {
    if (network_view_) return NetworkPageKey(network_view_, event);
    if (history_page_ || Current().loading || order_pending_ || displayed_entries_.empty() || !files_->contents()) return false;
    auto* focused = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
    if (!focused || !files_->Contains(focused)) return false;
    auto* layout = static_cast<FileLayout*>(files_->contents());
    if (focused != files_ && !layout->Contains(focused)) return false;
#if BUILDFLAG(IS_MAC)
    const bool trash_key = event.key_code() == ui::VKEY_BACK && event.IsCommandDown();
#else
    const bool trash_key = event.key_code() == ui::VKEY_DELETE;
#endif
    if (trash_key && !event.IsControlDown() && !event.IsAltDown() && !event.IsShiftDown()) {
      if (!event.is_repeat() && !Current().selected.empty() && !trash_busy_ && !root_->local_operation_busy_ &&
          Current().path != TrashRoot() && !TrashRoot().IsParent(Current().path)) ChangeTrash(false);
      return true;
    }
    if (PrimaryModifierDown(event) && event.key_code() == ui::VKEY_A) {
      // Ignore key repeat so holding the select-all shortcut cannot repeatedly toggle selection.
      if (event.is_repeat()) return true;
      auto& selected = Current().selected;
      const bool all_selected = std::ranges::all_of(displayed_entries_, [&](size_t index) {
        return selected.contains(Current().entries[index].path);
      });
      selected.clear();
      if (!all_selected) for (size_t index : displayed_entries_) selected.insert(Current().entries[index].path);
      for (const auto& [path, row] : row_views_)
        row->SetBackground(selected.contains(path) ?
            views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
      UpdateStatus();
      return true;
    }
    const size_t last = displayed_entries_.size() - 1;
    const auto focused_position = layout->IndexOf(focused);
    const size_t current = std::min(focused_position.value_or(keyboard_position_.value_or(0)), last);
    if (event.key_code() >= ui::VKEY_A && event.key_code() <= ui::VKEY_Z) {
      // A held key is one press; another press advances to the next match.
      if (event.is_repeat()) return true;
      const auto& matches = initial_positions_[event.key_code() - ui::VKEY_A];
      if (matches.empty()) return true;
      const bool has_current = focused_position.has_value() ||
          (keyboard_position_ && Current().selected.contains(Current().entries[displayed_entries_[current]].path));
      const size_t start = has_current ? current + 1 : 0;
      auto found = std::lower_bound(matches.begin(), matches.end(), start);
      const size_t next = found == matches.end() ? matches.front() : *found;
      keyboard_position_ = next; keyboard_anchor_ = next;
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
          base::BindOnce(&FilesView::FocusFilePosition, weak_.GetWeakPtr(), virtual_model_generation_, next, next));
      return true;
    }
    const size_t columns = layout->Columns();
    const size_t page = std::max(1, files_->GetVisibleRect().height() / layout->RowHeight()) * columns;
    size_t next = current;
    switch (event.key_code()) {
      case ui::VKEY_UP: next = current >= columns ? current - columns : 0; break;
      case ui::VKEY_DOWN: next = std::min(last, current + columns); break;
      case ui::VKEY_LEFT:
        if (!Current().options.icons) return false;
        next = current > 0 ? current - 1 : 0; break;
      case ui::VKEY_RIGHT:
        if (!Current().options.icons) return false;
        next = std::min(last, current + 1); break;
      case ui::VKEY_HOME: next = 0; break;
      case ui::VKEY_END: next = last; break;
      case ui::VKEY_PRIOR: next = current >= page ? current - page : 0; break;
      case ui::VKEY_NEXT: next = std::min(last, current + page); break;
      default: return false;
    }
    if (!focused_position && !keyboard_position_ &&
        (event.key_code() == ui::VKEY_UP || event.key_code() == ui::VKEY_DOWN ||
         event.key_code() == ui::VKEY_LEFT || event.key_code() == ui::VKEY_RIGHT)) next = 0;
    if (!event.IsShiftDown() || !keyboard_anchor_) keyboard_anchor_ = event.IsShiftDown() ? current : next;
    keyboard_position_ = next;
    // Focus/row lifetime changes occur after the original key target returns.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&FilesView::FocusFilePosition, weak_.GetWeakPtr(), virtual_model_generation_,
            next, event.IsShiftDown() ? *keyboard_anchor_ : next));
    return true;
  }
  void FocusFilePosition(uint64_t generation, size_t position, size_t anchor) {
    if (generation != virtual_model_generation_ || position >= displayed_entries_.size() || anchor >= displayed_entries_.size()) return;
    auto* layout = static_cast<FileLayout*>(files_->contents());
    auto& selected = Current().selected;
    selected.clear();
    for (size_t index = std::min(position, anchor); index <= std::max(position, anchor); ++index)
      selected.insert(Current().entries[displayed_entries_[index]].path);
    const auto item = layout->ItemBounds(position);
    layout->ScrollRectToVisible(item);
    if (footer_ && footer_->GetVisible() && (footer_shown_ || footer_->layer()->opacity() > 0.f)) {
      // The toolbar overlays the scroll viewport, so ScrollRectToVisible alone
      // considers covered rows visible. Keep an extra 8px above the toolbar.
      const auto viewport = files_->GetVisibleRect();
      const auto toolbar = views::View::ConvertRectToTarget(footer_->parent(), layout, footer_->bounds());
      const int visible_bottom = std::clamp(toolbar.y() - 8, viewport.y(), viewport.bottom());
      if (item.bottom() > visible_bottom) {
        const int offset = std::min(item.y(), viewport.y() + item.bottom() - visible_bottom);
        files_->ScrollToPosition(files_->vertical_scroll_bar(), std::max(0, offset));
      }
    }
    UpdateVirtualRows();
    for (const auto& [path, row] : row_views_)
      row->SetBackground(selected.contains(path) ? views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
    const auto& path = Current().entries[displayed_entries_[position]].path;
    auto row = row_views_.find(path);
    if (row != row_views_.end()) {
      row->second->RequestFocus();
    }
    FileViewportChanged(); UpdateStatus();
  }
  void CancelOrdering() {
    order_cancel_->store(true); order_pending_ = false;
    order_cancel_ = std::make_shared<std::atomic_bool>(false);
  }
  void CancelDirectoryWork() {
    // Hidden tabs retain their rows. Resume pending details when shown again;
    // RenderFiles/RemoveFileRows discard bindings during actual replacement.
    row_details_timer_.Stop();
    CancelLiveRead();
    read_cancel_->store(true); DirectoryReads().Prune();
    read_cancel_ = std::make_shared<std::atomic_bool>(false);
    directory_retry_.Stop(); CancelOrdering();
  }
  void ClearMaterializedRows(bool preserve_visuals = false) {
    if (!files_->contents() || history_page_) return;
    std::vector<base::FilePath> paths;
    for (const auto& [path, row] : row_views_) paths.push_back(path);
    RemoveFileRows(paths, false, preserve_visuals);
    // Keep the old scroll extent until the sorted replacement is ready.
    displayed_entries_.clear();
  }
  void RequestOrder() {
    const auto& options = Current().options;
    if (order_pending_ && requested_sort_ == options.sort && requested_hidden_ == options.show_hidden) return;
    CancelOrdering(); order_pending_ = true;
    requested_sort_ = options.sort; requested_hidden_ = options.show_hidden;
    const auto canceled = order_cancel_;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](DirectoryEntries entries, std::set<base::FilePath> removed,
                           FileSort sort, bool hidden, bool trash, CancelFlag token) {
          return SortDirectory(SortRecords(entries, token, removed), sort, hidden, trash, token);
        }, Current().entries, Current().removed, options.sort, options.show_hidden,
            Current().path == TrashRoot(), canceled),
        base::BindOnce([](base::WeakPtr<FilesView> pane, CancelFlag token, uint64_t request,
                         FileSort sort, bool hidden, std::vector<size_t> order) {
          if (!pane || Stopped(token) || pane->order_cancel_ != token || pane->Current().request != request) return;
          pane->order_pending_ = false;
          pane->Current().sorted_entries = std::move(order);
          pane->Current().sorted_mode = sort; pane->Current().sorted_hidden = hidden;
          pane->RenderFiles();
        }, weak_.GetWeakPtr(), canceled, Current().request, options.sort, options.show_hidden));
  }
  void RebuildDisplayedEntries(const DirectoryDelta* delta = nullptr) {
    // An index anchor is valid only while the displayed model is unchanged.
    resize_anchor_.reset();
    ++virtual_model_generation_;
    keyboard_position_.reset(); keyboard_anchor_.reset();
    auto& tab = Current();
    displayed_entries_ = tab.sorted_entries;
    for (auto& positions : initial_positions_) positions.clear();
    materialized_positions_.clear();
    if (delta) {
      initial_positions_ = delta->initials;
      for (const auto& [path, row] : row_views_)
        if (auto position = delta->DisplayPosition(path)) materialized_positions_.emplace(path, *position);
      return;
    }
    for (size_t position = 0; position < displayed_entries_.size(); ++position) {
      const auto& entry = tab.entries[displayed_entries_[position]];
      if (!entry.name_key.empty() && entry.name_key.front() >= u'a' && entry.name_key.front() <= u'z')
        initial_positions_[entry.name_key.front() - u'a'].push_back(position);
      if (!row_views_.empty() && row_views_.contains(entry.path))
        materialized_positions_.emplace(entry.path, position);
    }
  }
  void RemoveFileRows(const std::vector<base::FilePath>& paths, bool recycle = false, bool preserve_visuals = false) {
    std::set<base::FilePath> removed_paths;
    std::set<views::View*> removed_rows;
    for (const auto& path : paths) {
      auto row = row_views_.find(path);
      if (row == row_views_.end()) continue;
      removed_paths.insert(path);
      removed_rows.insert(row->second.get());
    }
    if (removed_rows.empty()) return;
    ++materialized_rows_generation_;
    if (root_ && root_->hover_owner_.get() == this && removed_paths.contains(root_->hover_path_)) root_->CloseHoverPreview();
    // Column cells are direct children of their file row. Compact each shared
    // collection once, before destroying any of the views it refers to.
    for (auto& column : column_cells_)
      std::erase_if(column, [&](const auto& cell) { return removed_rows.contains(cell->parent()); });
    std::erase_if(visible_buttons_, [&](const auto& item) { return removed_paths.contains(item.first->path); });
    auto* layout = static_cast<FileLayout*>(files_->contents());
    layout->ForgetItems(removed_rows);
    for (const auto& path : removed_paths) {
      if (recycle) {
        auto visual = Current().visuals.find(path);
        if (visual != Current().visuals.end()) RememberRowThumbnail(path, visual->second);
      }
      if (!preserve_visuals) Current().visuals.erase(path);
      file_buttons_.erase(path);
      row_views_.erase(path);
      pending_row_details_.erase(path);
      materialized_positions_.erase(path);
      visual_new_rows_.erase(path);
      visual_cache_checked_.erase(path);
      if (auto pending = visual_item_cancels_.find(path); pending != visual_item_cancels_.end())
        pending->second->store(true, std::memory_order_relaxed);
    }
    for (auto* row : removed_rows) {
      if (recycle) {
        // Drop image references before pooling; decoded cache memory remains bounded.
        auto* button = static_cast<FileButton*>(row->children().front().get());
        button->SetFileImage(ui::ImageModel());
        layout->RecycleRow(static_cast<FileRow*>(row));
      } else {
        layout->RemoveChildViewT(row);
      }
    }
  }
  void FileContentsScrolled() {
    if (history_page_) {
      if (auto* list = views::AsViewClass<HistoryList>(files_->contents())) list->ScheduleUpdate();
      return;
    }
    const auto offset = files_->CurrentOffset();
    if (offset != last_file_scroll_offset_) {
      last_file_scroll_ = base::TimeTicks::Now();
      last_file_scroll_offset_ = offset;
    }
    if (root_ && root_->hover_owner_.get() == this) root_->CloseHoverPreview();
    FileViewportChanged();
  }
  void FileViewportChanged() {
    if (!root_ || history_page_ || !files_->contents()) return;
    auto* layout = views::AsViewClass<FileLayout>(files_->contents());
    if (!layout) return;
    const auto [first, last] = layout->ItemsInRect(files_->GetVisibleRect());
    const bool urgent = layout->HasMissingItems(first, last);
    const auto delay = urgent ? base::Milliseconds(0) : base::Milliseconds(16);
    // An urgent gap may advance an existing prefetch timer. Repeated callbacks
    // share the same task, outside input/layout dispatch.
    if (!virtual_rows_timer_.IsRunning() ||
        (urgent && virtual_rows_timer_.desired_run_time() > base::TimeTicks::Now()))
      virtual_rows_timer_.Start(FROM_HERE, delay, this, &FilesView::UpdateFileViewport);
  }
  void UpdateFileViewport() {
    UpdateVirtualRows();
    ScheduleRowDetails();
    RequestFileVisuals();
  }
  bool IsFileRowPinned(const base::FilePath& path, FileRow* row, views::View* focused) const {
    const auto button = file_buttons_.find(path);
    return (focused && row->Contains(focused)) || row->GetState() == views::Button::STATE_PRESSED ||
        (button != file_buttons_.end() && button->second && button->second->GetState() == views::Button::STATE_PRESSED) ||
        std::ranges::any_of(dragging_rows_, [row](const auto& dragged) { return dragged.get() == row; });
  }
  void UpdateVirtualRows() {
    if (!root_ || history_page_ || tabs_.empty() || order_pending_ || displayed_entries_.empty() || !files_->contents() || !IsDrawn()) return;
    auto* layout = static_cast<FileLayout*>(files_->contents());
    auto viewport = files_->GetVisibleRect();
    if (viewport.IsEmpty() || layout->width() <= 0) return;
    // Populate one extra screen, but retain existing rows for two screens.
    // The wider eviction boundary avoids rebuilding rows on small reversals.
    const auto [visible_first, visible_last] = layout->ItemsInRect(viewport);
    auto keep_viewport = viewport;
    keep_viewport.Inset(gfx::Insets::VH(-2 * viewport.height(), 0));
    const auto [keep_first, keep_last] = layout->ItemsInRect(keep_viewport);
    viewport.Inset(gfx::Insets::VH(-viewport.height(), 0));
    const auto [first, last] = layout->ItemsInRect(viewport);
    FileRowRange range{virtual_model_generation_, materialized_rows_generation_, first, last,
                       layout->width(), layout->RowHeight(), keep_first, keep_last};
    const auto previous = virtual_rows_range_;
    const bool stable_geometry = previous && previous->model_generation == range.model_generation &&
        previous->row_generation == range.row_generation && previous->width == range.width &&
        previous->row_height == range.row_height;
    if (!previous || previous->model_generation != range.model_generation) {
      virtual_scroll_down_ = true;
    } else if (virtual_viewport_y_ && files_->GetVisibleRect().y() != *virtual_viewport_y_) {
      virtual_scroll_down_ = files_->GetVisibleRect().y() > *virtual_viewport_y_;
    }
    virtual_viewport_y_ = files_->GetVisibleRect().y();
    auto* focused = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
    if (virtual_rows_complete_ && previous == range && std::ranges::all_of(retained_virtual_rows_, [&](const auto& path) {
          auto row = row_views_.find(path);
          return row != row_views_.end() && IsFileRowPinned(path, row->second, focused);
        })) return;
    const auto deadline = base::TimeTicks::Now() + base::Milliseconds(4);
    bool changed = false;
    const bool index_changed = !previous || previous->model_generation != range.model_generation ||
        previous->row_generation != range.row_generation;
    if (index_changed) {
      // Only model/index changes rebind slots and accessibility metadata.
      // RowLayout already handles size changes for retained rows.
      layout->ResetItemPositions();
      std::vector<base::FilePath> invalid;
      for (const auto& [path, row] : row_views_) {
        const auto position = materialized_positions_.find(path);
        if (position == materialized_positions_.end() || position->second >= displayed_entries_.size() ||
            Current().entries[displayed_entries_[position->second]].path != path) {
          invalid.push_back(path);
          continue;
        }
        changed |= row->bounds() != layout->ItemBounds(position->second);
        layout->PlaceItem(position->second, row);
        row->SetVirtualPosition(position->second, displayed_entries_.size());
      }
      changed |= !invalid.empty();
      RemoveFileRows(invalid);
    }
    retained_virtual_rows_.clear();
    std::vector<base::FilePath> remove;
    bool complete = true;
    // The ordered slot index visits only rows beyond the retention boundaries.
    for (size_t index : layout->ItemsOutside(keep_first, keep_last)) {
      if (split_resize_active_) break;
      const auto& path = Current().entries[displayed_entries_[index]].path;
      auto* row = row_views_.at(path).get();
      if (IsFileRowPinned(path, row, focused)) {
        retained_virtual_rows_.insert(path);
      } else if (remove.size() < 32) {
        remove.push_back(path);
      } else {
        complete = false;
      }
    }
    changed |= !remove.empty();
    RemoveFileRows(remove, true);
    size_t created = 0;
    const bool overlap_complete = stable_geometry && virtual_rows_complete_;
    auto populate = [&](size_t begin, size_t end, bool budgeted, bool reverse) {
      std::vector<size_t> missing;
      auto append = [&](size_t start, size_t finish) {
        if (start >= finish) return;
        auto part = layout->MissingItems(start, finish);
        missing.insert(missing.end(), part.begin(), part.end());
      };
      if (overlap_complete) {
        // The previously completed buffered range has no holes.
        append(begin, std::min(end, previous->first));
        append(std::max(begin, previous->last), end);
      } else {
        // A previous budget may have left gaps, including after a direction change.
        append(begin, end);
      }
      if (reverse) std::reverse(missing.begin(), missing.end());
      for (size_t index : missing) {
        if (budgeted && (created >= 16 || base::TimeTicks::Now() >= deadline)) {
          complete = false;
          return;
        }
        auto* row = CreateFileRow(index, layout);
        layout->PlaceItem(index, row);
        layout->AnimateNewColumnItem(row);
        ++created;
        changed = true;
      }
    };
    // Visible placeholders are never deferred. Prefetch the nearest rows in
    // the direction of travel first, then spend any remaining budget behind.
    populate(visible_first, visible_last, false, false);
    if (split_resize_active_) {
      // Keep visible rows complete; prefetch resumes after the drag.
      complete = false;
    } else if (virtual_scroll_down_) {
      populate(visible_last, last, true, false);
      populate(first, visible_first, true, true);
    } else {
      populate(first, visible_first, true, true);
      populate(visible_last, last, true, false);
    }
    if (changed || index_changed) {
      layout->OrderItems();
      layout->InvalidateLayout();
    }
    range.row_generation = materialized_rows_generation_;
    virtual_rows_range_ = range;
    virtual_rows_complete_ = complete;
    if (!complete && !split_resize_active_ && !virtual_rows_timer_.IsRunning())
      virtual_rows_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &FilesView::UpdateFileViewport);
    RequestFileVisuals();
  }
  void RenderFiles() {
    if (root_ && root_->hover_owner_.get() == this) root_->CloseHoverPreview();
    if (history_page_) return;
    if (Current().sorted_mode != Current().options.sort || Current().sorted_hidden != Current().options.show_hidden) {
      RequestOrder(); return;
    }
    virtual_rows_timer_.Stop();
    root_->ScheduleWorkspaceSave();
    file_buttons_.clear();
    row_views_.clear();
    row_details_timer_.Stop(); pending_row_details_.clear();
    visual_new_rows_.clear();
    visible_buttons_.clear();
    for (auto& cells : column_cells_) cells.clear();
    auto& tab = Current();
    const auto& options = tab.options;
    const int icon_size = kFileIconSizes[options.icon_index];
    RebuildDisplayedEntries();
    if (!pending_search_selection_.empty() && pending_search_selection_.DirName() == tab.path) {
      const auto entry = std::ranges::find(tab.entries, pending_search_selection_, &Entry::path);
      if (entry != tab.entries.end() && entry->hidden && !tab.options.show_hidden) {
        tab.options.show_hidden = true; RequestOrder(); return;
      }
    }
    const int minimum_width = options.icons ? 0 : ListMinimumWidth();
    files_->SetHorizontalScrollBarMode(options.icons ? views::ScrollView::ScrollBarMode::kDisabled :
        views::ScrollView::ScrollBarMode::kEnabled);
    auto contents = std::make_unique<FileLayout>(options.icons, icon_size, minimum_width);
    contents->SetBottomPadding(tag_id_ == 0 ? 0 : footer_->GetPreferredSize({}).height() + 10);
    contents->SetSelectionCallback(base::BindRepeating(&FilesView::RubberBand, weak_.GetWeakPtr()));
    // Header follows the same column visibility and widths as the data rows.
    if (!options.icons) {
      auto header = std::make_unique<views::View>();
      header->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 0));
      const std::array labels = {IDS_CRFILES_NAME, IDS_CRFILES_SIZE, IDS_CRFILES_TYPE, IDS_CRFILES_LAST_MODIFIED, IDS_CRFILES_CREATED, IDS_CRFILES_PERMISSIONS};
      const std::array visible = {true, options.size_column, options.type_column, options.modified_column, options.created_column, options.permissions_column};
      const std::array ascending_modes = {FileSort::kNameAscending, FileSort::kSizeAscending,
          FileSort::kType, FileSort::kModifiedAscending, FileSort::kCreatedAscending, FileSort::kPermissionsAscending};
      const std::array descending_modes = {FileSort::kNameDescending, FileSort::kSize,
          FileSort::kTypeDescending, FileSort::kModifiedDescending, FileSort::kCreatedDescending, FileSort::kPermissionsDescending};
      for (size_t index = 0; index < labels.size(); ++index) {
        if (!visible[index]) continue;
        std::optional<bool> ascending;
        if (options.sort == ascending_modes[index]) ascending = true;
        else if (options.sort == descending_modes[index]) ascending = false;
        auto* cell = header->AddChildView(std::make_unique<ColumnHeader>(Text(labels[index]), ascending,
            Deferred(base::BindRepeating(&FilesView::SortByColumn, weak_.GetWeakPtr(), index)),
            base::BindRepeating(&FilesView::ResizeColumn, base::Unretained(this), index)));
        cell->SetPreferredSize(gfx::Size(options.column_widths[index], kListHeaderHeight));
        column_cells_[index].push_back(cell);
      }
      contents->SetHeader(contents->AddChildView(std::move(header)));
    }
    contents->SetItemCount(displayed_entries_.size());
    // Populate the first viewport before installing the new content tree. The
    // normal virtual-row timer is intentionally deferred, but must not leave
    // a freshly navigated directory empty for its first paint.
    const auto viewport = files_->GetVisibleRect();
    const int initial_width = std::max(minimum_width,
        viewport.width() > 0 ? viewport.width() : std::max(1, files_->width()));
    const int viewport_height = std::max(1,
        viewport.height() > 0 ? viewport.height() : files_->height());
    const int content_height = contents->GetHeightForWidth(initial_width);
    contents->SetBounds(0, 0, initial_width, content_height);
    if (!pending_search_selection_.empty() && pending_search_selection_.DirName() == tab.path) {
      for (size_t position = 0; position < displayed_entries_.size(); ++position) {
        if (tab.entries[displayed_entries_[position]].path != pending_search_selection_) continue;
        tab.selected.clear(); tab.selected.insert(pending_search_selection_);
        keyboard_position_ = position; keyboard_anchor_ = position;
        tab.scroll = contents->ItemBounds(position).y();
        break;
      }
      pending_search_selection_.clear();
    }
    const int initial_scroll = std::clamp(tab.scroll, 0, std::max(0, content_height - viewport_height));
    gfx::Rect initial_viewport(0, initial_scroll, initial_width, viewport_height);
    initial_viewport.Inset(gfx::Insets::VH(-viewport_height, 0));
    const auto [first, last] = contents->ItemsInRect(initial_viewport);
    for (size_t position = first; position < last; ++position)
      contents->PlaceItem(position, CreateFileRow(position, contents.get()));
    contents->OrderItems();
    // Retain decoded images for the prepared rows; release offscreen images
    // so rebuilding a view does not defeat the bounded visual cache.
    std::erase_if(tab.visuals, [&](const auto& item) { return !row_views_.contains(item.first); });
    contents->SetPointerMotionCallback(base::BindRepeating(&FilesView::EnableFileTooltips, weak_.GetWeakPtr()));
    contents->SetViewportCallback(base::BindRepeating(&FilesView::FileViewportChanged, weak_.GetWeakPtr()));
    pending_scroll_ = tab.scroll;
    files_->SetContents(std::move(contents));
    InvalidateLayout();
    UpdateStatus();
    FileViewportChanged();
  }
  struct RowDisplayData {
    std::array<std::optional<std::u16string>, 6> text;
    std::optional<std::u16string> tooltip;
    gfx::ImageSkia thumbnail;
    int edge = 0;
    size_t bytes = 0;
    uint64_t used = 0;
  };
  void TrimRowDisplayCache() {
    while (row_display_cache_.size() > 128 || row_display_bytes_ > 16 * 1024 * 1024) {
      auto oldest = std::min_element(row_display_cache_.begin(), row_display_cache_.end(),
          [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
      row_display_bytes_ -= oldest->second.bytes;
      row_display_cache_.erase(oldest);
    }
  }
  RowDisplayData& RowDisplay(const base::FilePath& path) {
    const auto& tab = Current();
    if (row_display_tab_ != tab.id || row_display_request_ != tab.request ||
        row_display_generation_ != tab.metadata_generation) {
      row_display_cache_.clear(); row_display_bytes_ = 0;
      row_display_tab_ = tab.id; row_display_request_ = tab.request;
      row_display_generation_ = tab.metadata_generation;
    }
    auto& data = row_display_cache_[path];
    data.used = ++row_display_clock_;
    TrimRowDisplayCache();
    return data;
  }
  std::u16string TagEntrySize(const Entry& entry) const {
    if (entry.size < 0) return Text(IDS_CRFILES_CALCULATING_SIZE);
    const auto found = tag_sizes_.find(entry.tag);
    return (found != tag_sizes_.end() && found->second.incomplete ? Text(IDS_CRFILES_SIZE_AT_LEAST) + u": " : std::u16string()) +
        ui::FormatBytes(base::ByteSize(static_cast<uint64_t>(entry.size)));
  }
  std::u16string RowText(const Entry& entry, size_t column) {
    auto& value = RowDisplay(entry.path).text[column];
    if (!value) {
      switch (column) {
        case 0: value = entry.tag > 0 ? base::UTF8ToUTF16(entry.tag_name) + u" · " + base::NumberToString16(entry.tag_count) + u" " + Text(IDS_CRFILES_ITEMS) : EntryName(entry.path); break;
        case 1: value = entry.tag > 0 ? TagEntrySize(entry) : entry.directory ? std::u16string() : ui::FormatBytes(base::ByteSize(static_cast<uint64_t>(std::max<int64_t>(0, entry.size)))); break;
        case 2: value = entry.tag > 0 ? Text(IDS_CRFILES_TAGS) : entry.directory ? Text(IDS_CRFILES_FOLDER) : base::UTF8ToUTF16(entry.mime); break;
        case 3: value = entry.modified.is_null() ? std::u16string() : base::TimeFormatShortDateAndTime(entry.modified); break;
        case 4: value = CreatedText(entry.created); break;
        case 5: value = PermissionsText(entry.permissions); break;
      }
    }
    return *value;
  }
  void EnableFileTooltips() {
    if (file_tooltips_enabled_) return;
    file_tooltips_enabled_ = true;
    if (GetWidget() && GetWidget()->GetTooltipManager())
      GetWidget()->GetTooltipManager()->UpdateTooltip();
  }
  std::u16string RowTooltip(const Entry& entry) {
    if (entry.tag > 0) return base::UTF8ToUTF16(entry.tag_name) + u"\n" + base::NumberToString16(entry.tag_count) +
        u" " + Text(IDS_CRFILES_ITEMS) + u"\n" + TagEntrySize(entry);
    auto& data = RowDisplay(entry.path);
    if (!data.tooltip) {
      const auto size = entry.directory || entry.size < 0 ? std::u16string(u"—") : RowText(entry, 1);
      data.tooltip = Text(IDS_CRFILES_NAME) + u": " + RowText(entry, 0) +
          u"\n" + Text(IDS_CRFILES_SIZE) + u": " + size +
          u"\n" + Text(IDS_CRFILES_PERMISSIONS) + u": " + RowText(entry, 5);
    }
    if (tag_id_ > 0 && IsTagListing())
      return *data.tooltip + u"\n" + Text(IDS_CRFILES_LOCATION) + u": " + entry.path.AsUTF16Unsafe();
    return *data.tooltip;
  }
  void RememberRowThumbnail(const base::FilePath& path, const EntryVisuals& visual) {
    if (visual.thumbnail_display.isNull()) return;
    // Copy only already-rendered representations. Do not retain the lazy
    // resize source (and its potentially large original) or generate pixels here.
    gfx::ImageSkia snapshot;
    size_t bytes = 0;
    for (const auto& rep : visual.thumbnail_display.image_reps()) {
      if (rep.is_null()) continue;
      bytes += rep.GetBitmap().computeByteSize();
      snapshot.AddRepresentation(rep);
    }
    if (snapshot.isNull() || bytes > 16 * 1024 * 1024) return;
    auto& data = RowDisplay(path);
    row_display_bytes_ -= data.bytes;
    data.thumbnail = std::move(snapshot); data.edge = visual.thumbnail_display_edge; data.bytes = bytes;
    row_display_bytes_ += bytes;
    TrimRowDisplayCache();
  }
  void ScheduleRowDetails() {
    if (split_resize_active_) return;
    if (!pending_row_details_.empty() && !row_details_timer_.IsRunning())
      row_details_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &FilesView::FlushRowDetails);
  }
  void FlushRowDetails() {
    if (split_resize_active_) return;
    if (!root_ || history_page_ || tabs_.empty() || !IsDrawn() || Current().loading || order_pending_) return;
    auto* layout = views::AsViewClass<FileLayout>(files_->contents());
    if (!layout) return;
    const auto [first, last] = layout->ItemsInRect(files_->GetVisibleRect());
    const auto deadline = base::TimeTicks::Now() + base::Milliseconds(3);
    size_t applied = 0;
    // Visible details precede prefetch details, regardless of path ordering.
    for (bool visible : {true, false}) {
      for (auto item = pending_row_details_.begin(); item != pending_row_details_.end();) {
        if (applied >= 12 || (applied && base::TimeTicks::Now() >= deadline)) break;
        const auto position = materialized_positions_.find(*item);
        const auto row = row_views_.find(*item);
        if (position == materialized_positions_.end() || row == row_views_.end() ||
            position->second >= displayed_entries_.size()) {
          item = pending_row_details_.erase(item);
          continue;
        }
        const bool in_view = position->second >= first && position->second < last;
        if (visible != in_view) { ++item; continue; }
        const auto& entry = Current().entries[displayed_entries_[position->second]];
        if (entry.path != *item) { item = pending_row_details_.erase(item); continue; }
        auto* tile = row->second.get();
        const auto& options = Current().options;
        if (!options.icons) {
          size_t child = 1;
          const std::array enabled = {false, options.size_column, options.type_column,
              options.modified_column, options.created_column, options.permissions_column};
          for (size_t column = 1; column < enabled.size(); ++column) {
            if (!enabled[column]) continue;
            auto* label = static_cast<views::Label*>(tile->children()[child++].get());
            const auto text = RowText(entry, column);
            label->SetText(text);
          }
        }
        item = pending_row_details_.erase(item);
        ++applied;
      }
    }
    ScheduleRowDetails();
  }
  FileRow* CreateFileRow(size_t position, FileLayout* layout) {
    ++materialized_rows_generation_;
    auto& entry = Current().entries[displayed_entries_[position]];
    const auto name = RowText(entry, 0);
    const auto& tab = Current();
    const auto& options = tab.options;
    const int icon_size = kFileIconSizes[options.icon_index];
    const int row_height = std::max(36, icon_size + 12);
    auto* tile = layout->TakeRecycledRow();
    const bool reused = tile != nullptr;
    if (!tile) tile = layout->AddChildView(std::make_unique<FileRow>(options.icons));
    size_t child_index = 1;
    bool missing_details = false;
    auto column = [&](views::View* row, size_t index) {
      auto* label = reused ? static_cast<views::Label*>(row->children()[child_index++].get()) :
          row->AddChildView(std::make_unique<views::Label>());
      if (!reused) label->SetHandlesTooltips(false);
      // Already formatted values can be restored directly. Do not clear them
      // and then reshape the same text again on the detail timer.
      const auto& cached = RowDisplay(entry.path).text[index];
      missing_details |= !cached.has_value();
      label->SetText(cached.value_or(std::u16string()));
      if (!reused) {
        label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
        label->SetElideBehavior(gfx::ELIDE_TAIL);
        label->SetCanProcessEventsWithinSubtree(false);
        label->SetSubpixelRenderingEnabled(false);
      }
      label->SetPreferredSize(gfx::Size(options.column_widths[index], row_height - 8));
      column_cells_[index].push_back(label);
    };
      row_views_.emplace(entry.path, tile);
      tile->set_drag_controller(this);
      tile->set_context_menu_controller(this);
      tile->SetDragDoneCallback(base::BindRepeating(&FilesView::FinishFileDrag, weak_.GetWeakPtr()));
      tile->SetCallback(base::BindRepeating(&FilesView::SelectFile, weak_.GetWeakPtr(),
          entry.path, entry.directory, base::Unretained(tile)));
      tile->SetAccessibleName(name);
      // Query current metadata only when a tooltip is requested. Rebinding a
      // recycled row must not trigger a root-wide tooltip lookup per cell.
      tile->SetPointerMotionCallback(base::BindRepeating(&FilesView::EnableFileTooltips, weak_.GetWeakPtr()));
      tile->SetTooltipCallback(base::BindRepeating(
          [](base::WeakPtr<FilesView> pane, base::FilePath path) -> std::u16string {
            if (!pane || pane->tabs_.empty()) return {};
            // This is pane state, so recycled or newly visible rows retain the
            // same tooltip eligibility instead of requiring another movement.
            if (!pane->file_tooltips_enabled_) return {};
            const auto position = pane->materialized_positions_.find(path);
            if (position == pane->materialized_positions_.end() || position->second >= pane->displayed_entries_.size()) return {};
            const auto& entry = pane->Current().entries[pane->displayed_entries_[position->second]];
            return entry.path == path ? pane->RowTooltip(entry) : std::u16string();
          }, weak_.GetWeakPtr(), entry.path));
      if (!reused) {
        auto* tile_layout = tile->SetLayoutManager(std::make_unique<views::BoxLayout>(
            options.icons ? views::BoxLayout::Orientation::kVertical : views::BoxLayout::Orientation::kHorizontal,
            gfx::Insets(options.icons ? 2 : 0), options.icons ? 4 : 0));
        if (options.icons) tile_layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
      }
      tile->SetState(views::Button::STATE_NORMAL);
      tile->SetBackground(tab.selected.contains(entry.path) ?
          views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
      auto callback = base::BindRepeating(&FilesView::SelectFile, weak_.GetWeakPtr(),
          entry.path, entry.directory, base::Unretained(tile));
      auto* open = reused ? static_cast<FileButton*>(tile->children().front().get()) :
          tile->AddChildView(std::make_unique<FileButton>(callback, name, options.icons, icon_size));
      open->SetCallback(callback);
      open->SetText(name);
      open->SetIconSize(icon_size);
      open->SetState(views::Button::STATE_NORMAL);
      // The outer row owns hit testing and all hover/selection painting.
      open->SetCanProcessEventsWithinSubtree(false);
      open->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      views::InkDrop::Get(open->ink_drop_view())->SetMode(views::InkDropHost::InkDropMode::OFF);
      open->set_drag_controller(this);
      open->set_context_menu_controller(this);
      open->SetDragDoneCallback(base::BindRepeating(&FilesView::FinishFileDrag, weak_.GetWeakPtr()));
      if (!options.icons) {
        open->SetHorizontalAlignment(gfx::ALIGN_LEFT);
        open->SetPreferredSize(gfx::Size(options.column_widths[0], row_height - 8));
        column_cells_[0].push_back(open);
        open->SetCanProcessEventsWithinSubtree(false);
        open->SetFocusBehavior(views::View::FocusBehavior::NEVER);
        views::InkDrop::Get(open->ink_drop_view())->SetMode(views::InkDropHost::InkDropMode::OFF);
        if (options.size_column) column(tile, 1);
        if (options.type_column) column(tile, 2);
        if (options.modified_column) column(tile, 3);
        if (options.created_column) column(tile, 4);
        if (options.permissions_column) column(tile, 5);
      }
      open->SetFileImage(FileImage(entry));
      file_buttons_.emplace(entry.path, open->GetWeakPtr());
      visible_buttons_.emplace_back(&entry, open->GetWeakPtr());
    // Cached details are ready; uncached cells were cleared before showing.
    if (missing_details) {
      pending_row_details_.insert(entry.path);
      ScheduleRowDetails();
    }
    tile->SetVisible(true);
    visual_new_rows_.insert(entry.path);
    materialized_positions_[entry.path] = position;
    tile->SetVirtualPosition(position, displayed_entries_.size());
    return tile;
  }
  void CancelFileVisuals() {
#if BUILDFLAG(IS_WIN)
    if (root_ && !tabs_.empty() && IsDrivesLocation(Current().path))
      LOG(INFO) << "[crFiles /] cancel_visuals pane=" << Current().id << " request=" << Current().request
          << " pending=" << visual_pending_.size() << " ready=" << visual_ready_.size();
#endif
    if (root_ && root_->hover_owner_.get() == this) root_->CloseHoverPreview();
    visual_cancel_->store(true, std::memory_order_relaxed);
    for (const auto& [path, token] : visual_item_cancels_)
      token->store(true, std::memory_order_relaxed);
    visual_item_cancels_.clear();
    visual_cancel_ = std::make_shared<std::atomic_bool>(false);
    visual_pending_.clear();
    visual_cache_checked_.clear();
    visual_ready_.clear();
    visual_queue_state_.reset();
    visual_new_rows_.clear();
    visual_queued_.clear();
    visual_queues_ = {};
    visual_flush_timer_.Stop();
    visual_icon_retry_timer_.Stop();
  }
  void RequestFileVisuals() {
    auto* shell = root_ ? root_.get() : this;
    if (shell->split_resize_active_) return;
    if (shell->visual_stopped_ || shell->visual_pump_pending_) return;
    shell->visual_pump_pending_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce([](base::WeakPtr<FilesView> shell) {
          if (!shell) return;
          shell->visual_pump_pending_ = false;
          shell->PumpFileVisuals();
        }, shell->weak_.GetWeakPtr()));
  }
  void ScheduleFileIconRetry(base::TimeTicks deadline) {
    if (visual_icon_retry_timer_.IsRunning() && visual_icon_retry_timer_.desired_run_time() <= deadline) return;
    visual_icon_retry_timer_.Start(FROM_HERE, std::max(deadline - base::TimeTicks::Now(), base::Milliseconds(1)),
        base::BindOnce([](base::WeakPtr<FilesView> pane) {
          if (!pane) return;
          pane->visual_queue_state_.reset();
          pane->RequestFileVisuals();
        }, weak_.GetWeakPtr()));
  }
  ui::ImageModel FileImage(const Entry& entry) {
    const auto& options = Current().options;
    const int edge = kFileIconSizes[options.icon_index];
    if (entry.tag > 0) return ui::ImageModel::FromVectorIcon(kTagFolderIcon, ui::kColorSysPrimary, edge);
    const float display_scale = files_->layer() ? files_->layer()->device_scale_factor() : 1.f;
#if BUILDFLAG(IS_WIN)
    if (IsDrivesLocation(Current().path) && IsDriveRoot(entry.path)) {
      const auto& visual = Current().visuals[entry.path];
      LOG(INFO) << "[crFiles /] display_icon path=" << entry.path.AsUTF8Unsafe() << " edge=" << edge
          << " pane=" << Current().id << " request=" << Current().request
          << " pane_image=" << !visual.images[options.icon_index].isNull()
          << " loaded=" << visual.icon_loaded[options.icon_index]
          << " attempts=" << visual.icon_attempts[options.icon_index];
    }
#endif
    const bool allow_thumbnail = !IsTextThumbnailPath(entry.path) || (options.icons && edge > 48);
    auto& visual = Current().visuals[entry.path];
    if (visual.thumbnail_display_only && (visual.thumbnail_display_edge != edge || !allow_thumbnail ||
        !visual.thumbnail_display.HasRepresentation(display_scale))) {
      visual.thumbnail_display = {}; visual.thumbnail_display_only = false;
      visual.thumbnail_attempted = false;
    }
    if (allow_thumbnail && visual.thumbnail.isNull()) {
      auto& cached = RowDisplay(entry.path);
      if (!cached.thumbnail.isNull() && cached.edge == edge &&
          cached.thumbnail.HasRepresentation(display_scale)) {
        visual.thumbnail_display = cached.thumbnail; visual.thumbnail_display_edge = edge;
        visual.thumbnail_display_only = true; visual.thumbnail_attempted = true;
        return ui::ImageModel::FromImageSkia(visual.thumbnail_display);
      }
      if (visual.thumbnail_display_only) return ui::ImageModel::FromImageSkia(visual.thumbnail_display);
      if (!entry.directory) {
        visual.thumbnail = FindMemoryThumbnail(entry.path, entry.size, entry.modified, kFileIconSizes.back());
        if (!visual.thumbnail.isNull()) visual.thumbnail_attempted = true;
      }
    }
    if (allow_thumbnail && !visual.thumbnail.isNull()) {
      // ImageSkia retains generated representations for each device scale.
      // Keep only the current display size, bounded by materialized rows.
      if (visual.thumbnail_display.isNull() || visual.thumbnail_display_edge != edge) {
        visual.thumbnail_display = ThumbnailImage(visual.thumbnail, edge);
        visual.thumbnail_display_edge = edge;
      }
      return ui::ImageModel::FromImageSkia(visual.thumbnail_display);
    }
    if (!Current().visuals[entry.path].images[options.icon_index].isNull())
      return ui::ImageModel::FromImageSkia(Current().visuals[entry.path].images[options.icon_index]);
    if (auto cached = FindCachedFileIcon(entry.path, entry.mime, entry.directory, edge); cached && !cached->isNull())
      return ui::ImageModel::FromImageSkia(*cached);
    if (IsDriveRoot(entry.path))
      return ui::ImageModel::FromVectorIcon(vector_icons::kDatabaseIcon, ui::kColorSysOnSurfaceVariant, edge);
    return ui::ImageModel::FromVectorIcon(entry.directory ? vector_icons::kFolderFilledIcon : vector_icons::kDescriptionIcon,
        ui::kColorSysOnSurfaceVariant, edge);
  }
  void QueueFileVisual(size_t position) {
    if (tag_id_ == 0) return;
    if (!visual_queue_state_ || position >= displayed_entries_.size() ||
        position < visual_queue_state_->rows.first || position >= visual_queue_state_->rows.last) return;
    const auto& entry = Current().entries[displayed_entries_[position]];
    auto button = file_buttons_.find(entry.path);
    if (button == file_buttons_.end() || !button->second || Current().removed.contains(entry.path) ||
        visual_pending_.contains(entry.path) || visual_queued_.contains(entry.path)) return;
    const auto& options = Current().options;
    const bool text = IsTextThumbnailPath(entry.path);
    const bool text_preview = text && options.icons && kFileIconSizes[options.icon_index] > 48;
    auto& visuals = Current().visuals[entry.path];
    const bool has_thumbnail = (!text || text_preview) && (!visuals.thumbnail.isNull() || visuals.thumbnail_display_only);
    // Probe the memory-only icon cache once when a candidate is classified,
    // rather than once per lane and available worker slot.
    if (!has_thumbnail && !UsesApplicationIcon(entry.path, entry.directory) && !visuals.icon_loaded[options.icon_index]) {
      if (auto cached = FindCachedFileIcon(entry.path, entry.mime, entry.directory, kFileIconSizes[options.icon_index])) {
        visuals.images[options.icon_index] = *cached;
        visuals.icon_loaded[options.icon_index] = true;
        visuals.icon_retry_at[options.icon_index] = {};
#if BUILDFLAG(IS_WIN)
        if (IsDrivesLocation(Current().path) && IsDriveRoot(entry.path))
          LOG(INFO) << "[crFiles /] apply_cached_icon path=" << entry.path.AsUTF8Unsafe()
              << " edge=" << kFileIconSizes[options.icon_index] << " pane=" << Current().id
              << " request=" << Current().request << " image_empty=" << cached->isNull();
#endif
        static_cast<FileButton*>(button->second.get())->SetFileImage(FileImage(entry));
      }
    }
    const bool video = IsVideoThumbnailPath(entry.path);
    const bool pdf = entry.mime == "application/pdf" || entry.path.MatchesExtension(FILE_PATH_LITERAL(".pdf"));
    const bool thumbnail = !entry.directory && !Current().failed_thumbnails.contains(entry.path) && !visuals.thumbnail_attempted &&
        (text ? text_preview : (video || pdf || IsHeicThumbnailPath(entry.path) ||
            entry.mime.starts_with("image/") || entry.path.MatchesExtension(FILE_PATH_LITERAL(".svg"))));
    FileVisualRequest request{entry.path, entry.mime, entry.directory, options.icon_index, false, thumbnail};
    FileVisualLane lane;
    if (thumbnail && !visual_cache_checked_.contains(entry.path)) {
      lane = FileVisualLane::kCache;
      request.probe = true;
    } else {
      lane = thumbnail && video ? FileVisualLane::kVideo : thumbnail && pdf ? FileVisualLane::kPdf : FileVisualLane::kRaster;
      const bool retry_waiting = visuals.icon_retry_at[options.icon_index] > base::TimeTicks::Now();
      if (!visuals.icon_loaded[options.icon_index] && !has_thumbnail && retry_waiting)
        ScheduleFileIconRetry(visuals.icon_retry_at[options.icon_index]);
      request.icon = !visuals.icon_loaded[options.icon_index] && !has_thumbnail && !retry_waiting;
      if (!request.icon && !thumbnail) return;
      request.large = thumbnail && !text && lane == FileVisualLane::kRaster && entry.size >= 8 * 1024 * 1024;
    }
    const bool prefetch = position < visual_queue_state_->visible.first || position >= visual_queue_state_->visible.second;
    visual_queued_.insert(entry.path);
    visual_queues_[prefetch ? 1 : 0][request.large ? 4 : static_cast<size_t>(lane)].push_back(std::move(request));
  }
  void QueueFileVisual(const base::FilePath& path) {
    auto position = materialized_positions_.find(path);
    if (position != materialized_positions_.end()) QueueFileVisual(position->second);
  }
  void CancelOutsideVisualRange(size_t first, size_t last) {
    for (const auto& [path, token] : visual_item_cancels_) {
      auto position = materialized_positions_.find(path);
      if (position == materialized_positions_.end() || position->second < first || position->second >= last)
        token->store(true, std::memory_order_relaxed);
    }
  }
  void PrepareFileVisualQueue() {
    if (visual_stopped_ || history_page_ || tabs_.empty() || Current().loading || order_pending_ ||
        !IsDrawn() || !files_->contents()) return;
    auto* layout = static_cast<FileLayout*>(files_->contents());
    auto viewport = files_->GetVisibleRect();
    if (viewport.IsEmpty() || layout->width() <= 0) {
      CancelOutsideVisualRange(0, 0);
      visual_queue_state_.reset();
      return;
    }
    const auto visible = layout->ItemsInRect(viewport);
    viewport.Inset(gfx::Insets::VH(-viewport.height(), 0));
    const auto [first, last] = layout->ItemsInRect(viewport);
    const auto& options = Current().options;
    const FileVisualQueueState state{{virtual_model_generation_, materialized_rows_generation_, first, last,
                                      layout->width(), layout->RowHeight()},
                                     visible, options.icon_index, options.icons, Current().metadata_generation};
    if (visual_queue_state_ == state && visual_new_rows_.empty()) return;
    const auto previous = visual_queue_state_;
    const bool incremental = previous && previous->rows.model_generation == state.rows.model_generation &&
        previous->metadata_generation == state.metadata_generation && previous->icon_index == state.icon_index &&
        previous->icons == state.icons && previous->rows.width == state.rows.width &&
        previous->rows.row_height == state.rows.row_height;
    CancelOutsideVisualRange(first, last);
    visual_queue_state_ = state;
    if (!incremental) {
      visual_queued_.clear();
      visual_queues_ = {};
      for (size_t position = first; position < last; ++position) QueueFileVisual(position);
    } else {
      // Retain classification for overlapping rows. Only queued requests need
      // pruning/reprioritization; completed rows need no repeated cache lookup.
      if (previous->rows.first != first || previous->rows.last != last ||
          previous->visible != visible || previous->rows.row_generation != state.rows.row_generation) {
        auto queues = std::move(visual_queues_);
        visual_queues_ = {};
        visual_queued_.clear();
        for (auto& priority : queues) {
          for (size_t lane = 0; lane < priority.size(); ++lane) {
            for (auto& item : priority[lane]) {
              const auto position = materialized_positions_.find(item.path);
              if (position == materialized_positions_.end() || position->second < first || position->second >= last ||
                  !file_buttons_.contains(item.path) || visual_pending_.contains(item.path)) continue;
              const bool prefetch = position->second < visible.first || position->second >= visible.second;
              visual_queued_.insert(item.path);
              visual_queues_[prefetch ? 1 : 0][lane].push_back(std::move(item));
            }
          }
        }
      }
      // Classify only the parts newly entering the buffered range.
      for (size_t position = first; position < std::min(last, previous->rows.first); ++position)
        QueueFileVisual(position);
      for (size_t position = std::max(first, previous->rows.last); position < last; ++position)
        QueueFileVisual(position);
      // Rows can be populated in a later batch without changing that range.
      for (const auto& path : visual_new_rows_) QueueFileVisual(path);
    }
    visual_new_rows_.clear();
  }
  std::vector<FileVisualRequest> TakeVisualBatch(bool prefetch, FileVisualLane lane, bool allow_large) {
    std::vector<FileVisualRequest> batch;
    if (visual_stopped_ || history_page_ || tabs_.empty() || Current().loading || order_pending_ || !IsDrawn() ||
        !files_->contents() || files_->GetVisibleRect().IsEmpty() || visual_ready_.size() >= 64) return batch;
    const bool batch_drives = lane == FileVisualLane::kRaster && IsDrivesLocation(Current().path);
    auto& queues = visual_queues_[prefetch ? 1 : 0];
    while (true) {
      size_t index = static_cast<size_t>(lane);
      // Blocked large decodes never require rescanning the normal candidates.
      if (lane == FileVisualLane::kRaster && queues[index].empty() && allow_large) index = 4;
      auto& queue = queues[index];
      if (queue.empty()) break;
      auto item = std::move(queue.front()); queue.pop_front();
      visual_queued_.erase(item.path);
      auto button = file_buttons_.find(item.path);
      if (button == file_buttons_.end() || !button->second || Current().removed.contains(item.path) ||
          visual_pending_.contains(item.path) || (batch_drives && !IsDriveRoot(item.path))) continue;
      visual_pending_.insert(item.path);
      item.canceled = std::make_shared<std::atomic_bool>(false);
      visual_item_cancels_[item.path] = item.canceled;
      batch.push_back(std::move(item));
      if ((lane != FileVisualLane::kCache && !batch_drives) || batch.size() >= 32) break;
    }
    return batch;
  }
  void PumpFileVisuals() {
    if (split_resize_active_) return;
    if (visual_stopped_ || root_ || tabs_.empty()) return;
    std::vector<FilesView*> panes;
    VisitPanes(Current().pane_host, base::BindRepeating([](std::vector<FilesView*>* list, FilesView* pane) {
      if (pane->history_page_) return;
      if (pane->IsDrawn()) list->push_back(pane);
      else pane->CancelOutsideVisualRange(0, 0);
    }, &panes));
    if (panes.empty()) return;
    for (auto* pane : panes) pane->PrepareFileVisualQueue();
    // One memory/disk cache probe, up to four images/icons, one PDF and one video.
    // Large encoded images share one slot; the decoder also budgets pixel memory.
    constexpr std::array limits = {1, 4, 1, 1};
    for (auto lane : {FileVisualLane::kCache, FileVisualLane::kRaster, FileVisualLane::kPdf, FileVisualLane::kVideo}) {
      const auto index = static_cast<size_t>(lane);
      while (visual_jobs_running_[index] < limits[index]) {
        FilesView* chosen = nullptr;
        std::vector<FileVisualRequest> batch;
        for (bool prefetch : {false, true}) {
          for (size_t offset = 0; offset < panes.size(); ++offset) {
            const size_t pane_index = (next_visual_pane_ + offset) % panes.size();
            batch = panes[pane_index]->TakeVisualBatch(prefetch, lane, !large_visual_running_);
            if (batch.empty()) continue;
            chosen = panes[pane_index]; next_visual_pane_ = (pane_index + 1) % panes.size(); break;
          }
          if (chosen) break;
        }
        if (!chosen) break;
        ++visual_jobs_running_[index];
        const bool large = batch.front().large;
        if (large) large_visual_running_ = true;
        auto canceled = chosen->visual_cancel_;
        int com_slot = -1;
#if BUILDFLAG(IS_WIN)
        scoped_refptr<base::SingleThreadTaskRunner> com_runner;
        const bool drive_batch = IsDrivesLocation(chosen->Current().path) && IsDriveRoot(batch.front().path);
        if (drive_batch) {
          // Reuse one STA for drive batches across panes and retries, avoiding
          // concurrent first-time drive queries without serializing thumbnails.
          if (!visual_drive_runner_) visual_drive_runner_ = base::ThreadPool::CreateCOMSTATaskRunner(
              {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
              base::SingleThreadTaskRunnerThreadMode::DEDICATED);
          com_runner = visual_drive_runner_;
          LOG(INFO) << "[crFiles /] dispatch_drive_batch count=" << batch.size()
              << " pane=" << chosen->Current().id << " request=" << chosen->Current().request;
        } else if (lane != FileVisualLane::kCache) {
          // Other decoding jobs keep independent STAs so a slow Shell handler
          // cannot serialize unrelated thumbnails.
          auto available = std::ranges::find(visual_com_busy_, false);
          CHECK(available != visual_com_busy_.end());
          com_slot = static_cast<int>(available - visual_com_busy_.begin());
          visual_com_busy_.at(com_slot) = true;
          auto& runner = visual_com_runners_.at(com_slot);
          if (!runner) runner = base::ThreadPool::CreateCOMSTATaskRunner(
              {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
              base::SingleThreadTaskRunnerThreadMode::DEDICATED);
          com_runner = runner;
        }
        for (const auto& item : batch) if (IsDriveRoot(item.path))
          LOG(INFO) << "[crFiles /] dispatch_icon path=" << item.path.AsUTF8Unsafe()
              << " edge=" << kFileIconSizes[item.icon_index] << " pane=" << chosen->Current().id
              << " request=" << chosen->Current().request << " com_slot=" << com_slot
              << " drive_batch=" << drive_batch << " batch_count=" << batch.size()
              << " attempt=" << chosen->Current().visuals[item.path].icon_attempts[item.icon_index] + 1;
#endif
        auto task = base::BindOnce(&LoadFileVisuals, std::move(batch), canceled);
        auto reply = base::BindOnce(&FilesView::FileVisualsFinished, weak_.GetWeakPtr(), chosen->weak_.GetWeakPtr(),
            chosen->Current().id, chosen->Current().request, canceled, lane, large, com_slot);
#if BUILDFLAG(IS_WIN)
        if (com_runner) {
          com_runner->PostTaskAndReplyWithResult(FROM_HERE, std::move(task), std::move(reply));
          continue;
        }
#endif
        base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE,
            {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskPriority::USER_VISIBLE},
            std::move(task), std::move(reply));
      }
    }
  }
  void FileVisualsFinished(base::WeakPtr<FilesView> pane, int id, uint64_t request,
                          std::shared_ptr<std::atomic_bool> canceled, FileVisualLane lane, bool large, int com_slot,
                          std::vector<FileVisualResult> results) {
#if BUILDFLAG(IS_WIN)
    if (com_slot >= 0) visual_com_busy_.at(com_slot) = false;
#else
    (void)com_slot;
#endif
    --visual_jobs_running_[static_cast<size_t>(lane)];
    if (large) large_visual_running_ = false;
#if BUILDFLAG(IS_WIN)
    for (const auto& result : results) if (IsDriveRoot(result.request.path))
      LOG(INFO) << "[crFiles /] worker_finished path=" << result.request.path.AsUTF8Unsafe()
          << " edge=" << kFileIconSizes[result.request.icon_index] << " pane=" << id << " request=" << request
          << " canceled=" << canceled->load(std::memory_order_relaxed) << " pane_alive=" << !!pane
          << " current_request=" << (pane && !pane->tabs_.empty() ? pane->Current().request : 0)
          << " transient_failure=" << result.retry_icon << " image_empty=" << result.icon.isNull();
#endif
    if (pane && pane->visual_cancel_ == canceled && !canceled->load(std::memory_order_relaxed) &&
        pane->Current().id == id && pane->Current().request == request) {
      for (auto& result : results) {
        const auto& item = result.request;
        if (item.canceled->load(std::memory_order_relaxed)) {
          pane->visual_pending_.erase(item.path);
          pane->visual_item_cancels_.erase(item.path);
          pane->visual_cache_checked_.erase(item.path);
          pane->QueueFileVisual(item.path);
          continue;
        }
        auto button = pane->file_buttons_.find(item.path);
        if (button == pane->file_buttons_.end() || !button->second) {
          pane->visual_item_cancels_.erase(item.path);
          pane->visual_pending_.erase(item.path); pane->visual_cache_checked_.erase(item.path); continue;
        }
        if (item.probe) {
          pane->visual_cache_checked_.insert(item.path);
          if (result.thumbnail.isNull()) {
            pane->visual_pending_.erase(item.path);
            pane->visual_item_cancels_.erase(item.path);
            pane->QueueFileVisual(item.path);
            continue;
          }
        }
        // Pending stays set until the result is applied, preventing duplicate
        // decode while the short UI coalescing window is open.
        pane->visual_ready_.push_back(std::move(result));
      }
      if (!pane->visual_ready_.empty() && !pane->visual_flush_timer_.IsRunning())
        pane->visual_flush_timer_.Start(FROM_HERE, base::Milliseconds(24), pane.get(), &FilesView::FlushFileVisuals);
    }
    // Refill workers immediately, independently of the next UI update frame.
    PumpFileVisuals();
  }
  void FlushFileVisuals() {
    const auto deadline = base::TimeTicks::Now() + base::Milliseconds(4);
    size_t applied = 0;
    while (!visual_ready_.empty() && applied < 16 && base::TimeTicks::Now() < deadline) {
      auto result = std::move(visual_ready_.front()); visual_ready_.pop_front(); ++applied;
      const auto& item = result.request;
      visual_pending_.erase(item.path);
      visual_item_cancels_.erase(item.path);
      if (item.canceled->load(std::memory_order_relaxed)) {
        visual_cache_checked_.erase(item.path);
        QueueFileVisual(item.path);
        continue;
      }
      auto button = file_buttons_.find(item.path);
      if (button == file_buttons_.end() || !button->second) { visual_cache_checked_.erase(item.path); continue; }
      auto position = materialized_positions_.find(item.path);
      if (position == materialized_positions_.end() || position->second >= displayed_entries_.size() ||
          Current().removed.contains(item.path)) continue;
      const auto* entry = &Current().entries[displayed_entries_[position->second]];
      if (entry->path != item.path) continue;
      if (item.icon) {
        auto& visuals = Current().visuals[entry->path];
#if BUILDFLAG(IS_WIN)
        const bool previous_image = !visuals.images[item.icon_index].isNull();
        const bool previous_loaded = visuals.icon_loaded[item.icon_index];
#endif
        visuals.images[item.icon_index] = std::move(result.icon);
        const unsigned attempt = ++visuals.icon_attempts[item.icon_index];
        const bool retry = result.retry_icon && attempt <= kFileIconRetryDelays.size();
        visuals.icon_loaded[item.icon_index] = !retry;
        visuals.icon_retry_at[item.icon_index] = retry ?
            base::TimeTicks::Now() + kFileIconRetryDelays[attempt - 1] : base::TimeTicks();
        if (retry) ScheduleFileIconRetry(visuals.icon_retry_at[item.icon_index]);
#if BUILDFLAG(IS_WIN)
        if (IsDrivesLocation(Current().path) && IsDriveRoot(entry->path))
          LOG(INFO) << "[crFiles /] apply_icon path=" << entry->path.AsUTF8Unsafe()
              << " edge=" << kFileIconSizes[item.icon_index] << " pane=" << Current().id
              << " request=" << Current().request << " attempt=" << attempt
              << " previous_image=" << previous_image << " previous_loaded=" << previous_loaded
              << " source=" << (visuals.images[item.icon_index].isNull() ? "drive-placeholder" :
                  result.retry_icon ? "bundled-folder" : "system")
              << " transient_failure=" << result.retry_icon << " retry_scheduled=" << retry
              << " retry_delay_ms=" << (retry ? kFileIconRetryDelays[attempt - 1].InMilliseconds() : 0)
              << " retries_exhausted=" << (result.retry_icon && !retry);
#endif
      }
      bool fade_thumbnail = false;
      if (item.thumbnail) {
        auto& visuals = Current().visuals[entry->path];
        if (result.thumbnail.isNull() && visuals.thumbnail.isNull() && !visuals.thumbnail_display_only)
          Current().failed_thumbnails.insert(entry->path);
        else Current().failed_thumbnails.erase(entry->path);
        fade_thumbnail = !item.probe && !result.thumbnail.isNull() &&
            visuals.thumbnail.isNull() && !visuals.thumbnail_display_only;
        visuals.thumbnail_attempted = true;
        // A late failed job must not replace an already available preview.
        if (!result.thumbnail.isNull()) {
          visuals.thumbnail = std::move(result.thumbnail);
          visuals.thumbnail_display = {};
          visuals.thumbnail_display_edge = 0;
          visuals.thumbnail_display_only = false;
        }
      }
      static_cast<FileButton*>(button->second.get())->SetFileImage(FileImage(*entry), fade_thumbnail);
      QueueFileVisual(item.path);
    }
    if (!visual_ready_.empty())
      visual_flush_timer_.Start(FROM_HERE, base::Milliseconds(16), this, &FilesView::FlushFileVisuals);
    RequestFileVisuals();
  }
  void ConfigureNetworkFooter() {
    normal_footer_actions_->SetVisible(false); trash_footer_actions_->SetVisible(false);
    footer_folder_name_->SetVisible(false);
    auto* outer = static_cast<views::BoxLayout*>(footer_->GetLayoutManager());
    outer->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
    outer->SetFlexForView(footer_status_, 1);
    // Retain the ordinary footer's labels and FlexLayout for directory stats.
    auto* middle = static_cast<views::FlexLayout*>(footer_status_->GetLayoutManager());
    middle->SetMainAxisAlignment(views::LayoutAlignment::kStart);
    middle->SetCrossAxisAlignment(views::LayoutAlignment::kCenter);
    status_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    status_->SetVerticalAlignment(gfx::ALIGN_MIDDLE);
    status_->SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(0, 6, 0, 0)));
    status_->SetProperty(views::kMarginsKey, gfx::Insets());
    auto* left = footer_->AddChildViewAt(std::make_unique<views::View>(), 0);
    auto* right = footer_->AddChildView(std::make_unique<views::View>());
    network_footer_left_ = left; network_footer_right_ = right;
    // Reserve space for actions at the toolbar edges.
    for (auto* side : {left, right}) {
      side->SetPreferredSize(gfx::Size(102, 26));
      auto* layout = side->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 4));
      layout->set_cross_axis_alignment(views::BoxLayout::CrossAxisAlignment::kCenter);
      layout->set_main_axis_alignment(side == left ? views::BoxLayout::MainAxisAlignment::kStart : views::BoxLayout::MainAxisAlignment::kEnd);
    }
    auto button = [&](views::View* side, const gfx::VectorIcon& icon, int label, base::RepeatingClosure callback) {
      auto* action = AddIconButton(side, icon, label, std::move(callback));
      action->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(3, 5)));
      action->SetPreferredSize(gfx::Size(30, 26)); action->SetFocusBehavior(views::View::FocusBehavior::NEVER);
      auto* ink = views::InkDrop::Get(action->ink_drop_view());
      ink->SetHighlightOpacity(0.18f); ink->SetVisibleOpacity(0.30f);
      for (auto state : {views::Button::STATE_HOVERED, views::Button::STATE_PRESSED})
        action->SetImageModel(state, ui::ImageModel::FromVectorIcon(icon, ui::kColorSysPrimary, 20));
      return action;
    };
    network_share_button_ = button(left, vector_icons::kScreenShareIcon, IDS_CRFILES_SHARE_PANE,
        base::BindRepeating(&FilesView::TogglePaneShare, weak_.GetWeakPtr()));
    network_new_folder_ = button(left, vector_icons::kFolderOpenIcon, IDS_CRFILES_NEW_FOLDER,
        base::BindRepeating(&FilesView::ShowNewFolder, weak_.GetWeakPtr()));
    network_paste_ = button(left, kContentPasteIcon, IDS_CRFILES_PASTE,
        base::BindRepeating([](base::WeakPtr<FilesView> pane) { if (pane) PasteNetworkPage(pane->network_view_); }, weak_.GetWeakPtr()));
    network_cancel_ = button(right, kCloseIcon, IDS_CRFILES_CANCEL,
        base::BindRepeating([](base::WeakPtr<FilesView> pane) { if (pane) CancelNetworkPageOperation(pane->network_view_); }, weak_.GetWeakPtr()));
    SetNetworkPageNewFolderHandler(network_view_, base::BindRepeating(&FilesView::ShowNewFolder, weak_.GetWeakPtr()));
    SetNetworkPageFooter(network_view_, base::BindRepeating(&FilesView::UpdateNetworkFooter, weak_.GetWeakPtr()));
  }
  void UpdateNetworkFooter(NetworkFooterState state) {
    if (!new_folder_network_url_.empty() &&
        (!state.visible || !state.show_actions || new_folder_network_url_ != NetworkPageUrl(network_view_))) CloseNewFolder();
    const bool visibility_changed = network_footer_visible_ != state.visible;
    const bool actions_changed = network_new_folder_->GetVisible() != state.show_actions;
    const bool summary_changed = footer_folder_name_->GetVisible() != state.show_summary;
    auto* shell = root_ ? root_.get() : this;
    const auto network_url = NetworkPageUrl(network_view_);
    if (state.busy && !network_footer_busy_) {
      shell->network_share_errors_.erase(network_url); shell->network_share_attempted_.erase(network_url);
    }
    network_footer_busy_ = state.busy;
    network_footer_visible_ = state.visible;
    footer_folder_name_->SetVisible(state.show_summary);
    auto* middle = static_cast<views::FlexLayout*>(footer_status_->GetLayoutManager());
    middle->SetMainAxisAlignment(state.show_summary ? views::LayoutAlignment::kEnd : views::LayoutAlignment::kStart);
    status_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    status_->SetBorder(views::CreateEmptyBorder(state.show_summary ? gfx::Insets() : gfx::Insets::TLBR(0, 6, 0, 0)));
    status_->SetProperty(views::kMarginsKey, state.show_summary ? gfx::Insets::TLBR(0, 4, 0, 0) : gfx::Insets());
    if (state.show_summary) {
      const auto url = NetworkPageUrl(network_view_);
      const auto location = ParseSmbLocation(url);
      const auto name = location ? base::UTF8ToUTF16(location->share.empty() ? location->server :
          location->path.empty() ? location->share : location->path.substr(location->path.find_last_of('/') + 1)) :
          Text(IDS_CRFILES_NETWORK);
      footer_folder_name_->SetText(name);
      footer_folder_name_->SetTooltipText(url.empty() ? name : base::UTF8ToUTF16(url));
      status_->SetText(u"· " + state.text);
      status_->SetTooltipText(name + u" · " + state.text +
          (url.empty() ? std::u16string() : u"\n" + base::UTF8ToUTF16(url)));
    } else {
      status_->SetText(state.text); status_->SetTooltipText(state.text);
    }
    network_share_button_->SetVisible(state.show_actions);
    network_new_folder_->SetVisible(state.show_actions);
    network_paste_->SetVisible(state.show_actions);
    network_new_folder_->SetEnabled(state.connected && !state.busy);
    network_paste_->SetEnabled(state.connected && !state.busy);
    network_cancel_->SetVisible(state.show_actions && state.busy);
    auto* outer = static_cast<views::BoxLayout*>(footer_->GetLayoutManager());
    // Idle network pages use the same action/info flex ratio as local folders.
    // Progress and errors start at the left edge of the status area.
    outer->SetFlexForView(network_footer_left_, state.show_summary ? 1 : 0);
    outer->SetFlexForView(footer_status_, state.show_summary ? 2 : 1);
    network_footer_left_->SetPreferredSize(gfx::Size(state.show_summary ? 280 : state.show_actions ? 102 : 0, 26));
    network_footer_right_->SetPreferredSize(gfx::Size(state.show_actions ? 102 : 0, 26));
    network_footer_right_->SetVisible(!state.show_summary);
    if (actions_changed) footer_->SetBackground(std::make_unique<FooterBackground>(
        state.show_actions ? kFolderSurfaceColor : ui::kColorSysBase));
    UpdateFooterVisibility();
    if (!state.busy && state.connected && state.show_actions) {
      const auto target = std::ranges::find(shell->shared_directories_, network_url, &NearbySharing::SharedTarget::network_url);
      auto credentials = NetworkPageCredentials(network_view_);
      if (target != shell->shared_directories_.end() && credentials) {
        const auto active = shell->network_share_credentials_.find(network_url);
        if (state.write_access == mojom::SmbWriteAccess::kReadOnly && target->remote &&
            active != shell->network_share_credentials_.end() && active->second.user == credentials->user &&
            active->second.domain == credentials->domain && active->second.password == credentials->password) {
          target->remote.reset(); shell->network_share_credentials_.erase(active);
          shell->network_share_errors_[network_url] = mojom::SmbError::kAccessDenied;
          shell->SyncSharedTargets();
        } else if (!target->remote && state.write_access != mojom::SmbWriteAccess::kReadOnly &&
            !shell->network_share_attempted_.contains(network_url)) shell->BeginNetworkShare(std::move(*credentials));
      }
    }
    UpdateNetworkShareButton();
    if (visibility_changed || actions_changed || summary_changed) InvalidateLayout();
  }
  void UpdateStatus() {
    if (network_page_) { UpdateFooterVisibility(); return; }
    auto& tab = Current();
    const auto root = TrashRoot();
    const bool tag_results = tag_id_ > 0 && IsTagListing();
    new_folder_button_->SetVisible(!tag_results);
    more_button_->SetVisible(!tag_results);
    new_folder_button_->SetTooltipText(Text(tag_id_ == 0 ? IDS_CRFILES_TAG_CREATE : IDS_CRFILES_NEW_FOLDER));
    new_folder_button_->SetAccessibleName(Text(tag_id_ == 0 ? IDS_CRFILES_TAG_CREATE : IDS_CRFILES_NEW_FOLDER));
    new_folder_button_->SetEnabled((tag_id_ == 0 || !IsTagListing()) && !IsDrivesLocation(tab.path) && tab.path != root && !root.IsParent(tab.path) && !tab.loading);
    search_action_->SetEnabled(!tab.loading && !tab.error &&
        ((tag_id_ > 0 && IsTagListing()) || (tab.path.IsAbsolute() && tab.path != root)));
    const bool trash = tab.path == root;
    const bool toolbar_changed = trash_footer_actions_->GetVisible() != trash;
    normal_footer_actions_->SetVisible(!trash);
    trash_footer_actions_->SetVisible(trash);
    footer_status_->SetVisible(!trash);
    restore_action_->SetEnabled(!trash_busy_ && !tab.loading && !root_->local_operation_busy_ && !tab.selected.empty());
    empty_trash_action_->SetEnabled(!trash_busy_ && !tab.loading && !root_->local_operation_busy_ &&
        std::ranges::any_of(tab.entries, [&tab](const Entry& entry) { return !tab.removed.contains(entry.path); }));
    if (toolbar_changed) {
      trash_footer_animation_pending_ = trash;
      static_cast<views::BoxLayout*>(footer_->GetLayoutManager())->set_inside_border_insets(
          trash ? gfx::Insets() : gfx::Insets::VH(4, 8));
      if (trash) {
        footer_->SetBackground(views::CreateRoundedRectBackground(ui::kColorSysBaseContainerElevated, 24));
        footer_->SetBorder(views::CreateRoundedRectBorder(1, 24, ui::kColorSysOutline));
      } else {
        footer_->SetBackground(std::make_unique<FooterBackground>((history_page_ && tag_id_ < 0) ? ui::kColorSysBase : kFolderSurfaceColor));
        footer_->SetBorder(std::make_unique<FooterGlowBorder>(false));
      }
      if (pane_border_active_ && pane_border_style_ == SplitSelectionStyle::kCorners)
        footer_->SetBorder(std::make_unique<FooterGlowBorder>(trash));
      InvalidateLayout();
    }
    const size_t visible_count = displayed_entries_.size();
    status_->SetText(tab.loading ? Text(IDS_CRFILES_LOADING) : tab.error ? Text(IDS_CRFILES_READ_ERROR) :
        visible_count == 0 ? Text(IDS_CRFILES_EMPTY) :
        base::NumberToString16(visible_count) + u" " + Text(IDS_CRFILES_ITEMS) + u" · " +
        base::NumberToString16(tab.selected.size()) + u" " + Text(IDS_CRFILES_SELECTED));
    if (!tab.selected.empty()) {
      int64_t selected_bytes = 0;
      bool has_selected_file = false;
      for (const auto& entry : tab.entries) {
        if (!tab.removed.contains(entry.path) && !entry.directory && tab.selected.contains(entry.path)) {
          has_selected_file = true;
          if (entry.size > 0) selected_bytes += std::min(entry.size, std::numeric_limits<int64_t>::max() - selected_bytes);
        }
      }
      if (has_selected_file || tab.selected.size() > 1)
        status_->SetText(std::u16string(status_->GetText()) + u" · " + ui::FormatBytes(base::ByteSize(static_cast<uint64_t>(selected_bytes))));
    }
    if (trash_error_) status_->SetText(Text(IDS_CRFILES_TRASH_ERROR));
    if (local_operation_error_) status_->SetText(Text(local_sources_retained_ ?
        IDS_CRFILES_MOVE_SOURCE_RETAINED : IDS_CRFILES_FILE_OPERATION_ERROR));
    footer_folder_name_->SetText(base::UTF8ToUTF16(PaneName()));
    footer_folder_name_->SetTooltipText(NavigationPath(tab.path));
    status_->SetText(u"· " + std::u16string(status_->GetText()));
    status_->SetTooltipText(base::UTF8ToUTF16(PaneName()) + u" " +
        std::u16string(status_->GetText()) + u"\n" + NavigationPath(tab.path));
    UpdateShareButton(); UpdateFooterVisibility();
  }
  void OpenSelected() {
    if (Current().selected.size() != 1) return;
    auto path = *Current().selected.begin();
    auto found = std::ranges::find(Current().entries, path, &Entry::path);
    if (found != Current().entries.end()) Open(path, found->directory);
  }
  void Open(base::FilePath path, bool directory) {
    if (tag_id_ == 0) { Navigate(std::move(path)); return; }
#if BUILDFLAG(IS_MAC)
    if (directory) {
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
          base::BindOnce(&IsFilePackageMac, path),
          base::BindOnce([](base::WeakPtr<FilesView> view, base::FilePath target,
                            int tab_id, uint64_t request, bool package) {
            if (!view || view->tabs_.empty() || view->Current().id != tab_id ||
                view->Current().request != request) return;
            if (package) view->Open(std::move(target), false);
            else if (view->search_page_) view->root_->AddPaneTab(target);
            else view->Navigate(target);
          }, weak_.GetWeakPtr(), path, Current().id, Current().request));
      return;
    }
#endif
    if (directory) { Navigate(path); return; }
    const auto found = std::ranges::find(Current().entries, path, &Entry::path);
    if (IsPdfReaderFile(path, found == Current().entries.end() ? std::string_view() : found->mime)) {
      OpenCrFilesPdf(path, base::BindOnce([](base::WeakPtr<FilesView> view) {
        if (view) view->status_->SetText(Text(IDS_CRFILES_OPEN_ERROR));
      }, weak_.GetWeakPtr()));
      return;
    }
    if (IsImageViewerFile(path, found == Current().entries.end() ? std::string_view() : found->mime)) {
      std::vector<base::FilePath> images;
      for (const auto index : displayed_entries_) {
        const auto& entry = Current().entries[index];
        if (!entry.directory && !Current().removed.contains(entry.path) && entry.path.DirName() == path.DirName() &&
            IsImageViewerFile(entry.path, entry.mime)) images.push_back(entry.path);
      }
      ShowCrFilesImageViewer(GetWidget(), std::move(images), path);
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()}, base::BindOnce([](base::FilePath file) {
      FileOperationLog log("open_file", "menu", file.AsUTF8Unsafe(), "");
#if BUILDFLAG(IS_WIN)
      base::win::ScopedCOMInitializer com;
      const bool ok = com.Succeeded() && reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", file.value().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif BUILDFLAG(IS_MAC)
      const bool ok = OpenFileWithMac(file, {});
#else
      const bool ok = base::LaunchProcess(std::vector<std::string>{"xdg-open", file.value()}, base::LaunchOptions()).IsValid();
#endif
      if (ok) log.Succeed();
      return ok;
    }, path), base::BindOnce([](base::WeakPtr<FilesView> view, bool ok) {
      if (view && !ok) view->status_->SetText(Text(IDS_CRFILES_OPEN_ERROR));
    }, weak_.GetWeakPtr()));
  }
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  void EnsureQuickSharing() {
    if (!quick_sharing_) quick_sharing_ = CreateQuickShareService();
    if (!quick_session_toast_) quick_session_toast_ = CreateShareSessionToast(GetWidget(), quick_sharing_,
        base::BindRepeating([](base::WeakPtr<FilesView> owner, const ShareService::Snapshot& state) {
          if (!owner) return ShareBubbleAnchor();
          if (auto* widget = owner->GetWidget(); widget && widget->IsVisible() && !widget->IsMinimized()) {
            if (auto bounds = FileShareSessionBounds(owner->quick_sharing_dialog_.view()))
              return ShareBubbleAnchor{*bounds, views::BubbleBorder::TOP_CENTER, false, true,
                  owner->quick_sharing_dialog_.view()->GetWidget()->GetWeakPtr()};
          }
          // Quick Share has its own configurable receive directory. Do not
          // anchor it to the desktop protocol's Received files shortcut.
          auto fallback = state; fallback.receiving = false;
          return owner->SessionBubbleAnchor(fallback, false);
        }, weak_.GetWeakPtr()));
  }
  void ShowQuickSharing(std::vector<base::FilePath> paths) {
    EnsureQuickSharing();
    if (auto* view = quick_sharing_dialog_.view(); view && view->GetWidget() && !view->GetWidget()->IsClosed()) {
      if (!paths.empty()) SetFileShareSelection(view, std::move(paths));
      if (view->GetWidget()->IsMinimized()) view->GetWidget()->Restore();
      view->GetWidget()->Show(); view->GetWidget()->Activate(); return;
    }
    quick_sharing_dialog_.SetView(ShowFileShareDialog(GetWidget(), quick_sharing_, std::move(paths)));
  }
#endif
  void ShowSyncTo(std::vector<base::FilePath> paths) {
    if (auto* view = sync_dialog_.view(); view && view->GetWidget() && !view->GetWidget()->IsClosed()) {
      view->GetWidget()->Show(); view->GetWidget()->Activate(); return;
    }
    sync_dialog_.SetView(ShowSyncDialog(GetWidget(), sharing_, std::move(paths)));
  }
  void ShowSharing(std::vector<base::FilePath> paths) {
    if (auto* view = sharing_dialog_.view(); view && view->GetWidget() && !view->GetWidget()->IsClosed()) {
      if (!paths.empty()) SetFileShareSelection(view, std::move(paths));
      if (view->GetWidget()->IsMinimized()) view->GetWidget()->Restore();
      view->GetWidget()->Show();
      view->GetWidget()->Activate(); return;
    }
    sharing_dialog_.SetView(ShowFileShareDialog(GetWidget(), sharing_, std::move(paths)));
  }
  std::optional<gfx::Rect> IncomingToastBounds(const std::string& target) {
    std::optional<gfx::Rect> area;
    const auto shared = std::ranges::find(shared_directories_, target, &NearbySharing::SharedTarget::id);
    if (shared != shared_directories_.end() && !tabs_.empty()) VisitPanes(Current().pane_host,
        base::BindRepeating([](const base::FilePath& directory, int64_t tag, const std::string& network_url,
                              std::optional<gfx::Rect>* bounds, FilesView* pane) {
          if (!pane->history_page_ && !pane->search_page_ && !pane->tabs_.empty() &&
              (!network_url.empty() ? pane->network_view_ && NetworkPageUrl(pane->network_view_) == network_url :
              tag > 0 ? pane->tag_id_ == tag && pane->IsTagListing() : ShareDirectoryKey(pane->Current().path) == directory) && pane->IsDrawn())
            *bounds = pane->GetBoundsInScreen();
        }, shared->directory, shared->tag_id, shared->network_url, &area));
    return area;
  }
  ShareBubbleAnchor SessionBubbleAnchor(const NearbySharing::Snapshot& state, bool use_share_dialog = true) {
    RevealReceivedTarget(state);
    auto* widget = GetWidget();
    bool foreground = widget && widget->IsVisible() && !widget->IsMinimized();
#if BUILDFLAG(IS_MAC)
    foreground = foreground && !IsCrFilesApplicationHiddenMac();
#endif
    if (foreground) {
      if (!state.receiving) {
        if (auto bounds = use_share_dialog ? FileShareSessionBounds(sharing_dialog_.view()) : std::nullopt)
          return {*bounds, views::BubbleBorder::TOP_CENTER, false, true};
      } else if (state.receive_target.empty()) {
        auto* received = sidebar_buttons_.at(IDS_CRFILES_RECEIVED).get();
        if (received->IsDrawn()) {
          const auto bounds = received->GetVisibleBounds();
          if (!bounds.IsEmpty()) {
            auto point = bounds.CenterPoint();
            point.set_y(bounds.bottom());
            views::View::ConvertPointToScreen(received, &point);
            return {gfx::Rect(point, gfx::Size(1, 1)),
                    views::BubbleBorder::TOP_CENTER, false, true};
          }
        }
      } else if (const auto area = IncomingToastBounds(state.receive_target)) {
        return {gfx::Rect(std::max(area->x(), area->right() - 12),
                         std::max(area->y(), area->bottom() - 12), 1, 1),
                views::BubbleBorder::BOTTOM_RIGHT, false, true};
      }
    }
    const auto icon = Tray() ? Tray()->GetIconBounds() : std::nullopt;
    auto* screen = display::Screen::Get();
    const auto monitor = icon ? screen->GetDisplayMatching(*icon) : screen->GetPrimaryDisplay();
    auto anchor = TrayShareBubbleAnchor(icon, monitor.bounds(), monitor.work_area(),
                                       BUILDFLAG(IS_MAC));
    if (foreground) {
      anchor.background = false;
      if (!anchor.show_arrow) {
        const auto area = GetBoundsInScreen();
        return {gfx::Rect(std::max(area.x(), area.right() - 12),
                         std::max(area.y(), area.bottom() - 12), 1, 1),
                views::BubbleBorder::BOTTOM_RIGHT, false, false};
      }
    }
    return anchor;
  }
  void RevealReceivedTarget(const NearbySharing::Snapshot& state) {
    if (!state.receiving || !state.session || !state.receive_target.empty()) return;
    auto* received = sidebar_buttons_.at(IDS_CRFILES_RECEIVED).get();
    if (received->GetVisible()) return;
    received->SetVisible(true);
    // The session bubble requests this anchor every 250 ms. Make the sidebar
    // geometry available immediately instead of waiting for directory I/O.
    if (auto* widget = GetWidget(); widget && !widget->IsClosed())
      widget->GetRootView()->DeprecatedLayoutImmediately();
  }
  void CheckReceivedDirectory(bool force = false) {
    const auto prefs = GetNearbySharingPreferences();
    const auto path = prefs.directory.empty() ? NearbySharing::DefaultReceiveDirectory() : base::FilePath::FromUTF8Unsafe(prefs.directory);
    if (received_check_pending_) {
      if (force || path != received_checked_path_) received_recheck_ = true;
      return;
    }
    const auto now = base::TimeTicks::Now();
    if (!force && path == received_checked_path_ &&
        !received_check_time_.is_null() && now - received_check_time_ < base::Seconds(30)) return;
    received_check_pending_ = true;
    received_checked_path_ = path;
    received_check_time_ = now;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](base::FilePath folder) { return base::DirectoryExists(folder); }, path),
        base::BindOnce([](base::WeakPtr<FilesView> view, base::FilePath checked, bool exists) {
          if (!view) return;
          view->received_check_pending_ = false;
          const bool recheck = std::exchange(view->received_recheck_, false);
          const auto latest = GetNearbySharingPreferences();
          const auto current = latest.directory.empty() ? NearbySharing::DefaultReceiveDirectory() : base::FilePath::FromUTF8Unsafe(latest.directory);
          if (current != checked) { view->CheckReceivedDirectory(true); return; }
          const auto state = view->sharing_->GetSnapshot();
          view->sidebar_buttons_.at(IDS_CRFILES_RECEIVED)->SetVisible(
              exists || (state.receiving && state.session && state.receive_target.empty()));
          if (recheck) view->CheckReceivedDirectory(true);
        }, weak_.GetWeakPtr(), path));
  }
  void UpdateNearby() {
    // The constructor invokes this before SetContentsView attaches the view
    // to a Widget. Initial directory checks run once the main window is shown.
    auto* widget = GetWidget();
    if (!widget || widget->IsClosed()) return;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    EnsureQuickSharing();
    const auto quick_state = quick_sharing_->GetSnapshot();
    if (quick_state.receiving && quick_state.stage == ShareService::Stage::kComplete &&
        quick_state.session != last_quick_received_session_) {
      last_quick_received_session_ = quick_state.session;
      for (auto& tab : tabs_) VisitPanes(tab.pane_host, base::BindRepeating([](const base::FilePath& batch, FilesView* pane) {
        if (!batch.empty() && (pane->Current().path == batch || pane->Current().path == batch.DirName())) pane->Refresh();
      }, quick_state.batch_directory));
    }
#endif
    SyncLiveDirectories();
    const bool visible = widget->IsVisible()
#if BUILDFLAG(IS_MAC)
        && !IsCrFilesApplicationHiddenMac()
#endif
        ;
    if (visible) CheckReceivedDirectory();
#if BUILDFLAG(IS_WIN)
    if (visible) UpdateWindowsDrives();
#endif
    if (auto* pane = ActivePane(); visible && pane && pane->history_page_ && pane->history_version_ != FileHistoryVersion()) pane->LoadHistory();
    if (visible) VisitPanes(Current().pane_host, base::BindRepeating([](FilesView* pane) {
      if (pane->IsTagListing() && (pane->tag_version_ != FileTagsVersion() ||
          (pane->Current().loading && !pane->tag_loading_))) pane->LoadTagFiles();
    }));
    auto s = sharing_->GetSnapshot();
    RevealReceivedTarget(s);
    sidebar_buttons_.at(IDS_CRFILES_RECEIVED)->SetBreathing(
        visible && !widget->IsMinimized() && s.busy && s.receiving &&
        s.receive_target.empty());
    if (s.receiving && s.session != last_received_session_ && (s.stage == NearbySharing::Stage::kComplete ||
        (!s.receive_url.empty() && !s.busy && (s.stage == NearbySharing::Stage::kError || s.stage == NearbySharing::Stage::kCanceled)))) {
      last_received_session_ = s.session;
      for (auto& tab : tabs_) VisitPanes(tab.pane_host, base::BindRepeating([](const base::FilePath& batch, const std::string& url, FilesView* pane) {
        if (!url.empty() && pane->network_view_ && NetworkPageUrl(pane->network_view_) == url) {
          RefreshNetworkPage(pane->network_view_); return;
        }
        if (!batch.empty() && (pane->Current().path == batch || pane->Current().path == batch.DirName())) pane->Refresh();
      }, s.batch_directory, s.receive_url));
      if (s.receive_write_denied && !s.receive_url.empty()) {
        const auto target = std::ranges::find(shared_directories_, s.receive_target, &NearbySharing::SharedTarget::id);
        if (target != shared_directories_.end()) target->remote.reset();
        network_share_credentials_.erase(s.receive_url); network_share_checks_.erase(s.receive_url);
        network_share_attempted_.insert(s.receive_url); network_share_errors_[s.receive_url] = mojom::SmbError::kAccessDenied;
        SyncSharedTargets();
      }
    }
    // A single controller handles both directions, even while the browser and
    // sender dialog are hidden. Its bubble has no native parent.
    // Its independent bubble does not require closing the device selector.
    // `receiving` is retained with completed sessions and cannot be used to
    // dismiss a newly opened Share to dialog.
    if (!session_toast_) session_toast_ = CreateShareSessionToast(widget, sharing_,
        base::BindRepeating([](base::WeakPtr<FilesView> view,
                              const NearbySharing::Snapshot& state) {
          return view ? view->SessionBubbleAnchor(state) : ShareBubbleAnchor{};
        }, weak_.GetWeakPtr()));
  }

  gfx::Rect drop_highlight_bounds_;
  raw_ptr<views::View> pane_body_ = nullptr;
  std::optional<gfx::Rect> split_hit_area_;
  raw_ptr<views::View> split_preview_ = nullptr;
  std::unique_ptr<views::BoundsAnimator> split_preview_animator_;
  bool split_preview_shown_ = false;
  raw_ptr<views::View> footer_ = nullptr, drop_highlight_ = nullptr;
  base::OneShotTimer footer_hide_timer_;
  base::RepeatingTimer footer_pointer_timer_;
  bool footer_active_ = false, footer_expired_ = false;
  bool footer_restore_pending_ = true;
  bool modal_hover_blocked_ = false;
  bool modal_previous_mouse_input_ = true;
  bool footer_hovered_ = false, footer_shown_ = false, drop_highlight_shown_ = false;
  bool trash_footer_animation_pending_ = false;
  std::vector<base::WeakPtr<views::View>> dragging_rows_;
  std::unique_ptr<FileDragCursorObserver> file_drag_cursor_observer_;
  std::unique_ptr<ShareSessionToast> session_toast_;
  views::ViewTracker sharing_dialog_, trusted_dialog_, sharing_folders_dialog_, sync_dialog_, syncing_dialog_;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  std::shared_ptr<ShareService> quick_sharing_;
  std::unique_ptr<ShareSessionToast> quick_session_toast_;
  views::ViewTracker quick_sharing_dialog_;
  uint64_t last_quick_received_session_ = 0;
#endif
  uint64_t last_received_session_ = 0;
  uint64_t shared_targets_generation_ = 0;
  std::vector<NearbySharing::SharedTarget> published_targets_;
  std::optional<int> restored_scroll_;
  std::string pane_id_, pane_name_;
  // Root-owned directory registry; independent of tab/pane lifetimes.
  std::vector<NearbySharing::SharedTarget> shared_directories_;
  std::map<std::string, SmbCredentials> network_share_credentials_;
  std::map<std::string, uint64_t> network_share_checks_;
  std::set<std::string> network_share_attempted_;
  std::map<std::string, mojom::SmbError> network_share_errors_;
  uint64_t network_share_serial_ = 0;
  std::shared_ptr<std::atomic<bool>> network_share_cancel_ = std::make_shared<std::atomic<bool>>(false);
  raw_ptr<views::LabelButton> share_button_ = nullptr;
  base::OneShotTimer workspace_timer_;
  bool restoring_workspace_ = false;
  bool local_operation_busy_ = false, local_operation_error_ = false;
  bool local_sources_retained_ = false;
  SearchScope search_scope_;
  int64_t tag_id_ = -1;
  std::string tag_name_;
  base::FilePath tag_directory_root_;
  std::vector<TagLocation> tag_history_;
  int tag_history_index_ = 0;
  std::map<int64_t, TagSize> tag_sizes_;
  bool tag_loading_ = false;
  bool tag_sizes_pending_ = false;
  uint64_t tag_version_ = 0;
  bool history_page_ = false;
  base::FilePath pending_search_selection_;
  bool search_page_ = false;
  bool network_page_ = false;
  raw_ptr<views::View> network_view_ = nullptr;
  raw_ptr<views::View> search_view_ = nullptr;
  raw_ptr<views::LabelButton> search_action_ = nullptr;
  bool history_loading_ = false;
  uint64_t history_generation_ = 0;
  bool history_cache_valid_ = false;
  std::string history_loaded_query_, history_loaded_filter_;
  std::string history_query_, history_filter_;
  std::vector<base::FilePath> context_paths_, destination_sources_, clipboard_paths_;
  std::vector<FileApplication> context_apps_;
  base::FilePath sidebar_context_path_;
  bool context_directory_ = false, destination_move_ = false, clipboard_move_ = false;
  bool context_package_ = false;
  uint64_t context_generation_ = 0;
  std::optional<ui::ClipboardSequenceNumberToken> clipboard_sequence_;
  scoped_refptr<ui::SelectFileDialog> destination_dialog_;
  bool selecting_send_ = false;
  std::unique_ptr<ui::SimpleMenuModel> context_menu_, context_split_, context_open_with_, context_sort_;
  std::unique_ptr<views::MenuRunner> context_runner_;
  raw_ptr<HistoryHeader> history_header_ = nullptr;
  raw_ptr<views::LabelButton> history_filter_button_ = nullptr;
  std::unique_ptr<ui::SimpleMenuModel> history_filter_model_;
  std::unique_ptr<views::MenuRunner> history_filter_runner_;
  raw_ptr<views::Textfield> history_search_ = nullptr;
  base::OneShotTimer history_search_timer_;
  uint64_t history_version_ = 0;
  raw_ptr<FilesView> root_ = nullptr;
  raw_ptr<views::View> split_host_ = nullptr;
  bool pane_can_close_ = false;
  raw_ptr<views::LabelButton> more_button_ = nullptr;
  raw_ptr<views::View> footer_status_ = nullptr;
  raw_ptr<views::Label> footer_folder_name_ = nullptr;
  std::unique_ptr<ui::SimpleMenuModel> more_menu_model_;
  std::unique_ptr<views::MenuRunner> more_menu_runner_;
  bool more_menu_open_ = false;
  bool file_tooltips_enabled_ = false;
  std::unique_ptr<PaneActivation> activation_;
  bool mouse_press_activated_pane_ = false;
  bool pane_border_active_ = false;
  SplitSelectionStyle pane_border_style_ = SplitSelectionStyle::kFlow;
  std::vector<Tab> tabs_;
  std::vector<base::DictValue> closed_tabs_;
  size_t active_ = 0;
  int next_id_ = 1;
  raw_ptr<views::View> tabs_view_ = nullptr;
  raw_ptr<views::View> active_tab_ = nullptr;
  bool tab_reveal_pending_ = false;
  int tab_viewport_width_ = -1;
  raw_ptr<BookmarksView> bookmarks_ = nullptr;
  std::map<int, raw_ptr<SidebarButton>> sidebar_buttons_;
#if BUILDFLAG(IS_WIN)
  raw_ptr<views::View> drive_list_ = nullptr;
  DWORD drive_mask_ = 0;
  std::map<base::FilePath, raw_ptr<SidebarButton>> drive_buttons_;
#endif
  uint64_t new_folder_generation_ = 0;
  raw_ptr<views::LabelButton> new_folder_button_ = nullptr;
  raw_ptr<NewFolderView> new_folder_contents_ = nullptr;
  std::string new_folder_network_url_;
  std::unique_ptr<RenameFocusObserver> rename_focus_observer_;
  std::unique_ptr<views::BubbleDialogDelegate> new_folder_delegate_;
  std::unique_ptr<views::Widget> new_folder_widget_;
  std::set<base::FilePath> drag_selection_;
  // Filtered model positions, not pointers into entries (which can reallocate).
  std::vector<size_t> displayed_entries_;
  std::array<std::vector<size_t>, 26> initial_positions_;
  uint64_t virtual_model_generation_ = 0;
  uint64_t materialized_rows_generation_ = 0;
  std::optional<FileRowRange> virtual_rows_range_;
  bool split_resize_active_ = false;
  std::optional<std::pair<size_t, int>> resize_anchor_;
  bool virtual_rows_complete_ = false;
  bool virtual_scroll_down_ = true;
  std::optional<int> virtual_viewport_y_;
  std::set<base::FilePath> retained_virtual_rows_;
  std::optional<size_t> keyboard_position_, keyboard_anchor_;
  base::OneShotTimer virtual_rows_timer_;
  base::OneShotTimer row_details_timer_;
  std::set<base::FilePath> pending_row_details_;
  base::TimeTicks last_file_scroll_, live_dirty_since_;
  gfx::PointF last_file_scroll_offset_;
  std::map<base::FilePath, RowDisplayData> row_display_cache_;
  size_t row_display_bytes_ = 0;
  int row_display_tab_ = -1;
  uint64_t row_display_request_ = 0, row_display_generation_ = 0, row_display_clock_ = 0;
  std::map<base::FilePath, size_t> materialized_positions_;
  std::map<base::FilePath, raw_ptr<FileRow>> row_views_;
  std::vector<std::pair<const Entry*, base::WeakPtr<views::Button>>> visible_buttons_;
  std::array<std::vector<raw_ptr<views::View>>, 6> column_cells_;
  raw_ptr<views::LabelButton> fullscreen_exit_ = nullptr;
  bool fullscreen_shown_ = false;
  base::OneShotTimer fullscreen_hide_timer_;
  bool trash_busy_ = false, trash_error_ = false;
  raw_ptr<views::LabelButton> restore_action_ = nullptr, empty_trash_action_ = nullptr;
  raw_ptr<views::View> normal_footer_actions_ = nullptr, trash_footer_actions_ = nullptr;
  raw_ptr<views::LabelButton> network_share_button_ = nullptr, network_new_folder_ = nullptr, network_paste_ = nullptr, network_cancel_ = nullptr;
  raw_ptr<views::View> network_footer_left_ = nullptr, network_footer_right_ = nullptr;
  views::ViewTracker network_address_anchor_;
  views::ViewTracker network_breadcrumb_;
  bool network_footer_visible_ = false, network_footer_busy_ = false;
  bool received_check_pending_ = false;
  bool received_recheck_ = false;
  base::FilePath received_checked_path_;
  base::TimeTicks received_check_time_;
  raw_ptr<views::View> toolbar_ = nullptr;
  raw_ptr<views::View> file_surface_ = nullptr;
  bool address_editing_ = false;
  bool directory_animation_pending_ = false;
  base::OneShotTimer path_animation_timer_;
  raw_ptr<views::View> sidebar_ = nullptr;
  raw_ptr<SidebarViewport> sidebar_viewport_ = nullptr;
  raw_ptr<SidebarResizeGrip> sidebar_resize_ = nullptr;
  bool sidebar_visible_ = true;
  int sidebar_width_ = 240;
  int sidebar_display_width_ = 240;
  bool sidebar_snapped_ = false;
  int sidebar_animation_from_ = 240, sidebar_animation_target_ = 240;
  base::TimeTicks sidebar_animation_start_;
  base::RepeatingTimer sidebar_animation_timer_;
  std::optional<int> sidebar_drag_start_;
  std::optional<int> pending_scroll_;
  raw_ptr<views::LabelButton> view_toggle_ = nullptr, options_button_ = nullptr;
  std::unique_ptr<views::BubbleDialogDelegate> options_delegate_;
  std::unique_ptr<views::Widget> options_widget_;
  std::optional<NavigationStyle> rendered_navigation_style_;
  raw_ptr<NavigationPathView> crumbs_scroll_ = nullptr;
  raw_ptr<views::View> path_area_ = nullptr;
  raw_ptr<views::ScrollView> files_ = nullptr;
  raw_ptr<views::Textfield> address_ = nullptr;
  std::unique_ptr<PathCompletion> address_completion_;
  raw_ptr<views::LabelButton> back_ = nullptr, forward_ = nullptr;
  raw_ptr<views::Label> status_ = nullptr;
  std::shared_ptr<NearbySharing> sharing_;
  std::map<base::FilePath, base::WeakPtr<views::Button>> file_buttons_;
  struct PendingNavigation { base::FilePath path; int history_index; bool refresh; };
  std::optional<PendingNavigation> pending_navigation_;
  uint64_t access_generation_ = 0;
  CancelFlag read_cancel_ = std::make_shared<std::atomic_bool>(false);
  CancelFlag order_cancel_ = std::make_shared<std::atomic_bool>(false);
  base::OneShotTimer directory_retry_;
  raw_ptr<views::View> hover_panel_ = nullptr;
  raw_ptr<views::ImageView> hover_image_ = nullptr;
  base::WeakPtr<FilesView> hover_owner_;
  base::FilePath hover_path_;
  gfx::Rect hover_root_bounds_;
  CancelFlag hover_cancel_ = std::make_shared<std::atomic_bool>(false);
  base::OneShotTimer hover_wait_timer_, hover_hide_timer_;
  bool hover_ready_ = false, hover_decode_running_ = false;
  std::map<base::FilePath, base::SequenceBound<LiveDirectoryWatch>> live_watches_;
  scoped_refptr<base::SequencedTaskRunner> live_watch_runner_;
  CancelFlag live_cancel_ = std::make_shared<std::atomic_bool>(false);
  base::OneShotTimer live_read_timer_;
  bool live_visible_ = false, live_window_active_ = false;
  bool live_read_pending_ = false, live_dirty_ = false;
  bool order_pending_ = false, requested_hidden_ = false;
  FileSort requested_sort_ = FileSort::kNameAscending;
  std::shared_ptr<std::atomic_bool> visual_cancel_ = std::make_shared<std::atomic_bool>(false);
  std::set<base::FilePath> visual_pending_;
  std::map<base::FilePath, std::shared_ptr<std::atomic_bool>> visual_item_cancels_;
  std::optional<FileVisualQueueState> visual_queue_state_;
  std::set<base::FilePath> visual_new_rows_;
  std::set<base::FilePath> visual_queued_;
  // Visible/prefetch queues, with a separate fifth queue for large rasters.
  std::array<std::array<std::deque<FileVisualRequest>, 5>, 2> visual_queues_;
  base::CallbackListSubscription file_scroll_subscription_;
  bool visual_pump_pending_ = false;
  std::array<int, 4> visual_jobs_running_ = {};
#if BUILDFLAG(IS_WIN)
  scoped_refptr<base::SingleThreadTaskRunner> visual_drive_runner_;
  std::array<scoped_refptr<base::SingleThreadTaskRunner>, 6> visual_com_runners_ = {};
  std::array<bool, 6> visual_com_busy_ = {};
#endif
  bool large_visual_running_ = false, visual_stopped_ = false;
  std::set<base::FilePath> visual_cache_checked_;
  std::deque<FileVisualResult> visual_ready_;
  base::OneShotTimer visual_flush_timer_;
  base::OneShotTimer visual_icon_retry_timer_;
  size_t next_visual_pane_ = 0;
  int rendered_tab_id_ = -1;
  ui::ImageModel trash_tab_icon_, network_tab_icon_;
  ui::ImageModel network_location_icon_;
  std::vector<std::pair<int, std::u16string>> rendered_tab_titles_;
  std::string rendered_crumb_pane_;
  std::string rendered_network_url_;
  std::vector<std::pair<base::FilePath, base::WeakPtr<views::View>>> rendered_crumb_buttons_;
  base::FilePath rendered_crumb_path_;
  base::RepeatingTimer timer_;
  std::unique_ptr<AddressOutsideClickHandler> outside_click_handler_;
#if BUILDFLAG(IS_MAC)
  base::ScopedClosureRunner titlebar_click_monitor_;
#endif
  base::WeakPtrFactory<FilesView> weak_{this};
};
BEGIN_METADATA(FilesView)
END_METADATA
class FilesDelegate : public views::WidgetDelegate {
 public:
  explicit FilesDelegate(base::OnceClosure quit) : quit_(std::move(quit)) {
    SetTitle(u"crFiles"); SetHasWindowSizeControls(true);
    SetAppIcon(CreateCrFilesAppIconModel()); SetIcon(CreateCrFilesAppIconModel());
    auto contents = std::make_unique<FilesView>();
    SetInitiallyFocusedView(contents->DefaultFocusView());
    SetContentsView(std::move(contents));
  }
  std::unique_ptr<views::FrameView> CreateFrameView(views::Widget* widget) override {
    return CreateFilesFrameView(widget);
  }
  bool OnCloseRequested(views::Widget::ClosedReason reason) override {
    if (!g_quitting && Tray() && Tray()->IsAvailable()) {
      HideCrFilesWindow();
      return false;
    }
    return true;
  }
  void WindowClosing() override { if (quit_) std::move(quit_).Run(); }
 private:
  base::OnceClosure quit_;
};
}
namespace {
struct NetworkDisplayItem {
  std::string name, id;
  bool directory;
  uint64_t size;
  base::Time modified, created;
};
std::unique_ptr<views::View> CreateNetworkItemGrid(const std::vector<NetworkDisplayItem>& items,
    bool icons, int icon_size, bool devices, NetworkColumns columns, const gfx::ImageSkia& image,
    views::ContextMenuController* menu, base::RepeatingCallback<void(std::string, bool)> open,
    base::RepeatingCallback<void(size_t, bool, const ui::Event&)> select = {}) {
  const std::array visible = {columns.size, columns.type, columns.modified, columns.created};
  constexpr std::array widths = {90, 140, 160, 160};
  constexpr std::array labels = {IDS_CRFILES_SIZE, IDS_CRFILES_TYPE, IDS_CRFILES_LAST_MODIFIED, IDS_CRFILES_CREATED};
  int metadata_width = 0;
  for (size_t i = 0; i < visible.size(); ++i) if (visible[i]) metadata_width += widths[i];
  auto contents = std::make_unique<FileLayout>(icons, icon_size, icons || !metadata_width ? 0 : 280 + metadata_width);
  contents->SetItemCount(items.size());
  contents->set_context_menu_controller(menu);
  if (!icons) {
    auto header = std::make_unique<views::View>();
    auto* layout = header->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal));
    auto* name = header->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_NAME)));
    name->SetHorizontalAlignment(gfx::ALIGN_LEFT); name->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 8)));
    layout->SetFlexForView(name, 1);
    for (size_t i = 0; i < visible.size(); ++i) {
      if (!visible[i]) continue;
      auto* label = header->AddChildView(std::make_unique<views::Label>(Text(labels[i])));
      label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetPreferredSize(gfx::Size(widths[i], kListHeaderHeight));
    }
    contents->SetHeader(contents->AddChildView(std::move(header)));
  }
  for (size_t index = 0; index < items.size(); ++index) {
    const auto& item = items[index];
    auto* row = contents->AddChildView(std::make_unique<FileRow>(icons));
    row->SetAccessibleName(base::UTF8ToUTF16(item.name));
    row->set_context_menu_controller(menu);
    row->SetVirtualPosition(index, items.size());
    row->SetTooltipCallback(base::BindRepeating([](std::string text) { return base::UTF8ToUTF16(text); }, devices ? "smb://" + item.id + "/" : item.name));
    row->SetCallback(base::BindRepeating(
        [](FileLayout* contents, FileRow* selected, base::RepeatingCallback<void(std::string, bool)> open,
           std::string id, bool directory, bool device, size_t index,
           base::RepeatingCallback<void(size_t, bool, const ui::Event&)> select, const ui::Event& event) {
          if (!select) for (const auto& child : contents->children()) {
            if (auto* row = views::AsViewClass<FileRow>(child.get()))
              row->SetBackground(row == selected ? views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
          }
          const bool single = directory && (device || GetFileViewPreferences().single_click_folders) && event.IsMouseEvent() &&
              event.AsMouseEvent()->IsOnlyLeftMouseButton() && !event.IsControlDown() &&
              !event.IsCommandDown() && !event.IsShiftDown() && !event.IsAltDown();
          const bool activate = single || (event.IsMouseEvent() && event.AsMouseEvent()->GetClickCount() == 2) ||
              (event.IsKeyEvent() && event.AsKeyEvent()->key_code() == ui::VKEY_RETURN);
          if (select) select.Run(index, activate, event);
          else if (activate) open.Run(std::move(id), directory);
        }, base::Unretained(contents.get()), base::Unretained(row), open, item.id, item.directory, devices, index, select));
    auto* layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(icons ?
        views::BoxLayout::Orientation::kVertical : views::BoxLayout::Orientation::kHorizontal,
        gfx::Insets(icons ? 2 : 0), icons ? 4 : 0));
    if (icons) layout->set_main_axis_alignment(views::BoxLayout::MainAxisAlignment::kCenter);
    auto* button = row->AddChildView(std::make_unique<FileButton>(views::Button::PressedCallback(),
        base::UTF8ToUTF16(item.name), icons, icon_size));
    button->SetCanProcessEventsWithinSubtree(false);
    button->SetFocusBehavior(views::View::FocusBehavior::NEVER);
    views::InkDrop::Get(button->ink_drop_view())->SetMode(views::InkDropHost::InkDropMode::OFF);
    if (!icons) { button->SetHorizontalAlignment(gfx::ALIGN_LEFT); layout->SetFlexForView(button, 1); }
    const auto& fallback = devices ? vector_icons::kDevicesIcon : item.directory ? vector_icons::kFolderOpenIcon : vector_icons::kInsertDriveFileOutlineOldIcon;
    button->SetFileImage(image.isNull() ? ui::ImageModel::FromVectorIcon(fallback, ui::kColorSysOnSurface, icon_size) : ui::ImageModel::FromImageSkia(image));
    if (!icons) {
      for (size_t i = 0; i < visible.size(); ++i) {
        if (!visible[i]) continue;
        std::u16string value;
        switch (i) {
          case 0: value = item.directory ? std::u16string() : ui::FormatBytes(base::ByteSize(item.size)); break;
          case 1: {
            if (item.directory) value = Text(IDS_CRFILES_FOLDER);
            else {
              // Use the built-in MIME table; remote names are not local files.
              auto extension = base::FilePath::FromUTF8Unsafe(item.name).FinalExtension();
              if (!extension.empty()) extension.erase(0, 1);
              std::string mime;
              if (!net::GetWellKnownMimeTypeFromExtension(extension, &mime)) mime = "application/octet-stream";
              value = base::UTF8ToUTF16(mime);
            }
            break;
          }
          case 2: value = item.modified.is_null() ? std::u16string() : base::TimeFormatShortDateAndTime(item.modified); break;
          case 3: value = CreatedText(item.created); break;
        }
        auto* label = row->AddChildView(std::make_unique<views::Label>(value));
        label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetSubpixelRenderingEnabled(false);
        label->SetCanProcessEventsWithinSubtree(false); label->SetHandlesTooltips(false);
        label->SetPreferredSize(gfx::Size(widths[i], std::max(36, icon_size + 12) - 8));
      }
    }
    contents->PlaceItem(index, row);
  }
  return contents;
}
}  // namespace
std::unique_ptr<views::View> CreateNetworkDevicesView(const std::vector<mojom::SmbServerPtr>& servers,
    bool icons, int icon_size, const gfx::ImageSkia& image, base::RepeatingCallback<void(std::string)> open) {
  std::vector<NetworkDisplayItem> items;
  for (const auto& server : servers) items.push_back({server->name,
      server->address + (server->port == 445 ? std::string() : ":" + base::NumberToString(server->port)), true, 0, {}, {}});
  return CreateNetworkItemGrid(items, icons, icon_size, true, {false, false, false, false}, image, nullptr, base::BindRepeating(
      [](base::RepeatingCallback<void(std::string)> open, std::string address, bool) { open.Run(std::move(address)); }, std::move(open)));
}
std::unique_ptr<views::View> CreateNetworkFilesView(base::span<const mojom::SmbEntryPtr> entries,
    bool icons, int icon_size, NetworkColumns columns, views::ContextMenuController* menu,
    base::RepeatingCallback<void(size_t, bool, const ui::Event&)> select) {
  std::vector<NetworkDisplayItem> items;
  for (const auto& entry : entries) items.push_back({entry->name, entry->name, entry->directory, entry->size, entry->modified, entry->created});
  return CreateNetworkItemGrid(items, icons, icon_size, false, columns, gfx::ImageSkia(), menu, {}, std::move(select));
}
void SetNetworkFilesDragController(views::View* contents, views::DragController* controller,
    base::RepeatingClosure done) {
  for (const auto& child : contents->children()) if (auto* row = views::AsViewClass<FileRow>(child.get())) {
    row->set_drag_controller(controller);
    row->SetDragDoneCallback(done);
  }
}
void SetNetworkFileSelection(views::View* contents, const std::set<size_t>& selected,
    std::optional<size_t> drop_target) {
  size_t index = 0;
  for (const auto& child : contents->children()) if (auto* row = views::AsViewClass<FileRow>(child.get())) {
    const bool incoming = drop_target == index;
    const bool active = selected.contains(index++);
    row->SetBackground(incoming ? views::CreateRoundedRectBackground(ui::kColorSysPrimaryContainer, 8) :
        active ? views::CreateRoundedRectBackground(ui::kColorSysSurfaceVariant, 8) : nullptr);
    row->GetViewAccessibility().SetIsSelected(active);
  }
}
void SetNetworkMarquee(views::View* contents,
    base::RepeatingCallback<void(std::set<size_t>, bool, bool, bool)> select) {
  auto* layout = views::AsViewClass<FileLayout>(contents);
  if (!layout) return;
  layout->SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
  layout->GetViewAccessibility().SetIsMultiselectable(true);
  layout->SetSelectionCallback(base::BindRepeating(
      [](FileLayout* layout, base::RepeatingCallback<void(std::set<size_t>, bool, bool, bool)> select,
         gfx::Rect rect, bool additive, bool begin, bool end) {
        std::set<size_t> indices;
        const auto [first, last] = layout->ItemsInRect(rect);
        for (size_t i = first; i < last; ++i) if (layout->ItemBounds(i).Intersects(rect)) indices.insert(i);
        select.Run(std::move(indices), additive, begin, end);
      }, base::Unretained(layout), std::move(select)));
}
size_t NetworkFileColumns(views::View* contents) {
  auto* layout = views::AsViewClass<FileLayout>(contents);
  return layout ? std::max<size_t>(1, layout->Columns()) : 1;
}
void FocusNetworkFile(views::View* contents, size_t index) {
  auto* layout = views::AsViewClass<FileLayout>(contents);
  if (!layout) return;
  layout->ScrollRectToVisible(layout->ItemBounds(index));
  size_t current = 0;
  for (const auto& child : contents->children()) if (auto* row = views::AsViewClass<FileRow>(child.get())) {
    if (current++ == index) { row->RequestFocus(); return; }
  }
}
std::optional<size_t> NetworkFileIndex(views::View* contents, views::View* source) {
  auto* layout = views::AsViewClass<FileLayout>(contents);
  return layout ? layout->IndexOf(source) : std::nullopt;
}
views::View* NetworkFileNameAnchor(views::View* contents, size_t index) {
  if (!contents) return nullptr;
  size_t position = 0;
  for (const auto& child : contents->children()) if (auto* row = views::AsViewClass<FileRow>(child.get())) {
    if (position++ != index || row->children().empty()) continue;
    return static_cast<FileButton*>(row->children().front().get())->NameAnchor();
  }
  return nullptr;
}
void SetNetworkFilesImages(views::View* contents, const std::vector<gfx::ImageSkia>& images) {
  if (!views::AsViewClass<FileLayout>(contents)) return;
  size_t index = 0;
  for (const auto& child : contents->children()) {
    auto* row = views::AsViewClass<FileRow>(child.get());
    if (!row) continue;
    if (index >= images.size()) break;
    if (!row->children().empty() && !images[index].isNull())
      static_cast<FileButton*>(row->children().front().get())->SetFileImage(ui::ImageModel::FromImageSkia(images[index]));
    ++index;
  }
}
void SetNetworkDevicesImage(views::View* contents, const gfx::ImageSkia& image) {
  if (image.isNull() || !views::AsViewClass<FileLayout>(contents)) return;
  for (const auto& child : contents->children()) {
    auto* row = views::AsViewClass<FileRow>(child.get());
    if (row && !row->children().empty())
      static_cast<FileButton*>(row->children().front().get())->SetFileImage(ui::ImageModel::FromImageSkia(image));
  }
}
std::unique_ptr<views::View> CreateCrFilesTabStrip(views::Widget*) { return g_contents ? g_contents->CreateTabs() : nullptr; }
void ShowCrFilesTrustedDevices() { if (g_contents) g_contents->ShowTrustedDevices(); }
void ShowCrFilesSyncing() { if (g_contents) g_contents->ShowSyncing(); }
void ShowCrFilesSharingFolders() { if (g_contents) g_contents->ShowSharingFolders(); }
void ShowCrFilesReceivedDirectory(const base::FilePath& directory) {
  RestoreCrFilesWindow();
  if (g_contents) g_contents->ShowReceivedDirectory(directory);
}
void ShowCrFilesHistory() { if (g_contents) g_contents->ShowHistoryTab(); }
bool IsCrFilesQuickShareBusy() { return g_contents && g_contents->QuickShareBusy(); }
bool IsCrFilesSharingBusy() { return g_contents && g_contents->SharingBusy(); }
void ApplyCrFilesViewSettings() { if (g_contents) g_contents->ApplyViewSettings(); }
bool IsCrFilesSidebarVisible() { return g_contents && g_contents->SidebarVisible(); }
base::CallbackListSubscription ObserveCrFilesSidebar(base::RepeatingCallback<void(bool)> callback) {
  return SidebarObservers().Add(std::move(callback));
}
void ToggleCrFilesSidebar() {
  if (g_contents && !IsCrFilesWindowBlockedByModal(g_contents->GetWidget()))
    g_contents->ToggleSidebarFromTitlebar();
}
void NewCrFilesTab() { if (g_contents) g_contents->NewTabFromTitlebar(); }
bool CanReopenCrFilesTab() { return g_contents && g_contents->CanReopenTab(); }
void ReopenCrFilesTab() { if (g_contents) g_contents->ReopenTab(); }
void BookmarkCrFilesTabs() { if (g_contents) g_contents->BookmarkTabs(); }
void ApplyCrFilesSharingSettings() { if (g_contents) g_contents->ApplySharing(); }
void HideCrFilesWindow() {
  if (g_quitting || !Window() || Window()->IsClosed() || !Window()->IsVisible()) return;
  HiddenOwnedWindows().clear();
  for (const auto& owned : views::Widget::GetAllOwnedWidgets(Window()->GetNativeView())) {
    if (owned->IsVisible()) HiddenOwnedWindows().push_back(owned->GetWeakPtr());
  }
  // Snapshot weak references before Hide dispatches activation callbacks.
  for (const auto& owned : HiddenOwnedWindows()) {
    if (owned && !owned->IsClosed()) owned->Hide();
  }
#if BUILDFLAG(IS_WIN)
  // Tray visibility changes should not animate the translucent custom frame.
  Window()->SetVisibilityChangedAnimationsEnabled(false);
#endif
  Window()->Hide();
#if BUILDFLAG(IS_WIN)
  Window()->SetVisibilityChangedAnimationsEnabled(true);
#endif
}
void RestoreCrFilesWindow() {
  if (g_quitting || !Window() || Window()->IsClosed()) return;
#if BUILDFLAG(IS_MAC)
  ActivateCrFilesApplicationMac();
#endif
#if BUILDFLAG(IS_WIN)
  Window()->SetVisibilityChangedAnimationsEnabled(false);
  if (Window()->IsMinimized()) Window()->Restore();
  if (!Window()->IsVisible()) Window()->Show();
  // Show already activates on Windows; retry only if activation did not occur.
  if (!Window()->IsActive()) Window()->Activate();
  Window()->SetVisibilityChangedAnimationsEnabled(true);
#else
  if (Window()->IsMinimized()) Window()->Restore();
  Window()->Show();
  Window()->Activate();
#endif
  if (g_contents) g_contents->RefreshReceivedSidebar();
  auto hidden = std::move(HiddenOwnedWindows());
  HiddenOwnedWindows().clear();
  for (const auto& owned : hidden) {
    if (owned && !owned->IsClosed()) owned->Show();
  }
}
void QuitCrFiles() {
  if (g_quitting || !Window() || Window()->IsClosed()) return;
  g_quitting = true;
  Window()->Close();
}
#if BUILDFLAG(IS_LINUX)
class FilesWindowTreeHostLinux : public views::DesktopWindowTreeHostLinux {
 public:
  FilesWindowTreeHostLinux(views::Widget* widget, views::DesktopNativeWidgetAura* native)
      : views::DesktopWindowTreeHostLinux(widget, native), widget_(widget) {}
  void DispatchEvent(ui::Event* event) override {
    // The Linux host consumes caption clicks before Aura pre-target handlers.
    // Intercept only our titlebar's context menu; keep native move/resize paths.
    if (event->type() == ui::EventType::kMousePressed &&
        event->AsMouseEvent()->IsOnlyRightMouseButton() &&
        widget_->non_client_view() && !widget_->IsClosed() &&
        !IsCrFilesWindowBlockedByModal(widget_)) {
      auto* frame = widget_->non_client_view()->frame_view();
      const auto location = event->AsMouseEvent()->location_f();
      auto point = gfx::ToRoundedPoint(GetRootTransform().InverseMapPoint(location).value_or(location));
      views::View::ConvertPointToTarget(widget_->GetRootView(), frame, &point);
      if (frame->NonClientHitTest(point) == HTCAPTION) {
        views::View::ConvertPointToScreen(frame, &point);
        event->SetHandled();
        frame->ShowContextMenu(point, ui::mojom::MenuSourceType::kMouse);
        return;
      }
    }
    views::DesktopWindowTreeHostLinux::DispatchEvent(event);
  }
 private:
  raw_ptr<views::Widget> widget_;
};
#endif
void ShowCrFilesWindow(base::OnceClosure quit) {
  g_quitting = false;
  InitializeCrFilesTheme();
  Delegate() = std::make_unique<FilesDelegate>(std::move(quit));
  Window() = std::make_unique<views::Widget>();
  views::Widget::InitParams params(views::Widget::InitParams::CLIENT_OWNS_WIDGET,
                                  views::Widget::InitParams::TYPE_WINDOW);
  params.delegate = Delegate().get(); params.bounds = gfx::Rect(1180, 760);
  params.name = "crfiles_window"; params.remove_standard_frame = true;
#if BUILDFLAG(IS_MAC)
  params.remove_standard_frame = GetWindowsTitlebarStyleEnabled();
#endif
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
#if BUILDFLAG(IS_MAC)
  // NativeWidgetMac requires the same corner radius on all four corners.
  params.rounded_corners = gfx::RoundedCornersF(12);
  params.shadow_type = views::Widget::InitParams::ShadowType::kDefault;
#else
  params.rounded_corners = gfx::RoundedCornersF(12, 12, 0, 0);
  params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
#endif
#if BUILDFLAG(IS_LINUX)
  auto* native = new views::DesktopNativeWidgetAura(Window().get());
  params.native_widget = native;
  params.desktop_window_tree_host = new FilesWindowTreeHostLinux(Window().get(), native);
  params.wm_class_name = "libcr.crfiles"; params.wm_class_class = "libcr.crfiles";
  params.wayland_app_id = "libcr.crfiles";
#endif
  Window()->Init(std::move(params)); Window()->CenterWindow(gfx::Size(1180, 760)); Window()->Show();
  if (g_contents) g_contents->RefreshReceivedSidebar();
#if BUILDFLAG(IS_MAC)
  InstallCrFilesApplicationMenuMac(Window().get());
#endif
  ApplyCrFilesTraySettings();
}
void ApplyCrFilesTraySettings() {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if (g_quitting || !Window() || Window()->IsClosed()) return;
  if (!GetSystemTrayEnabled()) {
    if (Tray()) RestoreCrFilesWindow();
    Tray().reset();
    return;
  }
  if (Tray()) return;
  Tray() = CreateCrFilesTray(base::BindRepeating([](TrayCommand command) {
    if (g_quitting || !g_contents || !Tray() || !Window() ||
        Window()->IsClosed()) return;
    switch (command) {
      case TrayCommand::kOpen:
        RestoreCrFilesWindow();
        return;
      case TrayCommand::kSettings:
        RestoreCrFilesWindow();
        ShowCrFilesSettingsDialog(Window().get());
        return;
      case TrayCommand::kQuit:
        QuitCrFiles();
        return;
      case TrayCommand::kToggle:
        break;
    }
    bool visible = Window() && Window()->IsVisible() && !Window()->IsMinimized();
#if BUILDFLAG(IS_MAC)
    visible = visible && !IsCrFilesApplicationHiddenMac();
#endif
    if (visible) HideCrFilesWindow();
    else RestoreCrFilesWindow();
  }));
#endif
}
void DestroyCrFilesWindow() {
  g_quitting = true;
  Tray().reset();
  HiddenOwnedWindows().clear();
#if BUILDFLAG(IS_MAC)
  RemoveCrFilesApplicationMenuMac();
#endif
  if (g_contents) g_contents->PrepareToClose();
  CloseCrFilesDialogs();
  Window().reset(); Delegate().reset();
}
}
