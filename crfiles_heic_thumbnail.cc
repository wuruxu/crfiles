// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_heic_thumbnail.h"
#include "crFiles/crfiles_decode_memory.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "build/build_config.h"
#if !BUILDFLAG(IS_MAC)
#include "base/compiler_specific.h"
#include "base/base_paths.h"
#include "base/path_service.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/native_library.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif
#include "crFiles/third_party/libheif/heif.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColorFilter.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "third_party/skia/include/private/SkHdrMetadata.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "skia/ext/color_profile.h"
#include "skia/ext/image_operations.h"
#endif

namespace crfiles {
bool IsHeicThumbnailPath(const base::FilePath& path) {
  return path.MatchesExtension(FILE_PATH_LITERAL(".heic")) ||
         path.MatchesExtension(FILE_PATH_LITERAL(".heif"));
}
}  // namespace crfiles

#if !BUILDFLAG(IS_MAC)
namespace crfiles {
namespace {

constexpr size_t kMaxEncodedBytes = 64 * 1024 * 1024;
constexpr int kMaxDimension = 16384;
constexpr int64_t kMaxPixels = 64 * 1000 * 1000;
struct HeicMetadata {
  bool has_alpha = false;
  bool is_hdr = false;
};
bool Stopped(const std::shared_ptr<std::atomic_bool>& canceled) {
  return canceled && canceled->load(std::memory_order_relaxed);
}
SkBitmap ResizeThumbnail(SkBitmap bitmap, int edge,
                         const std::shared_ptr<std::atomic_bool>& canceled) {
  if (bitmap.drawsNothing() || Stopped(canceled)) return {};
  const int longest = std::max(bitmap.width(), bitmap.height());
  if (longest > edge) {
    const double scale = static_cast<double>(edge) / longest;
    bitmap = skia::ImageOperations::Resize(bitmap,
        skia::ImageOperations::RESIZE_BEST,
        std::max(1, static_cast<int>(bitmap.width() * scale)),
        std::max(1, static_cast<int>(bitmap.height() * scale)));
  }
  return Stopped(canceled) ? SkBitmap() : bitmap;
}

// The official public header supplies exact ABI types; no system include or
// link paths are injected into Chromium's sysroot. Keep the module initialized
// for process lifetime, including all outstanding worker tasks.
#define HEIC_SYMBOLS(V) \
  V(heif_get_version_number) \
  V(heif_init) \
  V(heif_have_decoder_for_format) \
  V(heif_context_alloc) \
  V(heif_context_free) \
  V(heif_context_set_maximum_image_size_limit) \
  V(heif_context_set_max_decoding_threads) \
  V(heif_context_read_from_memory_without_copy) \
  V(heif_context_get_number_of_top_level_images) \
  V(heif_context_get_primary_image_handle) \
  V(heif_image_handle_get_list_of_thumbnail_IDs) \
  V(heif_image_handle_get_thumbnail) \
  V(heif_image_handle_release) \
  V(heif_image_handle_get_width) \
  V(heif_image_handle_get_height) \
  V(heif_image_handle_has_alpha_channel) \
  V(heif_image_handle_get_raw_color_profile_size) \
  V(heif_image_handle_get_raw_color_profile) \
  V(heif_image_handle_get_nclx_color_profile) \
  V(heif_nclx_color_profile_free) \
  V(heif_decoding_options_alloc) \
  V(heif_decoding_options_free) \
  V(heif_decode_image) \
  V(heif_image_release) \
  V(heif_image_get_primary_width) \
  V(heif_image_get_primary_height) \
  V(heif_image_get_colorspace) \
  V(heif_image_get_chroma_format) \
  V(heif_image_is_premultiplied_alpha) \
  V(heif_image_get_bits_per_pixel_range) \
  V(heif_image_handle_get_content_light_level) \
  V(heif_image_handle_get_mastering_display_colour_volume) \
  V(heif_mastering_display_colour_volume_decode) \
  V(heif_image_get_plane_readonly)

struct HeicApi {
  DISABLE_CFI_DLSYM HeicApi() {
#if BUILDFLAG(IS_LINUX)
    library = base::LoadNativeLibrary(
        base::FilePath(FILE_PATH_LITERAL("libheif.so.1")), nullptr);
#elif BUILDFLAG(IS_WIN)
    base::FilePath executable_directory;
    if (base::PathService::Get(base::DIR_EXE, &executable_directory)) {
      // Only app-local codecs and system dependencies, never the current
      // working directory or PATH. The packaged backend is built in, not a plugin.
      library = LoadLibraryExW(
          executable_directory.AppendASCII("heif.dll").value().c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
#endif
    if (!library) {
      LOG(WARNING) << "HEIC requires libheif with an HEVC decoder "
                      "(libheif.so.1 on Linux, app-local heif.dll on Windows)";
      return;
    }
#define LOAD_HEIC(name) \
    name = reinterpret_cast<decltype(name)>( \
        base::GetFunctionPointerFromNativeLibrary(library, #name)); \
    if (!name) { \
      LOG(WARNING) << "HEIC library is missing " << #name; \
      return; \
    }
    HEIC_SYMBOLS(LOAD_HEIC)
#undef LOAD_HEIC
    if (heif_get_version_number() < LIBHEIF_NUMERIC_VERSION ||
        heif_init(nullptr).code != heif_error_Ok ||
        !heif_have_decoder_for_format(heif_compression_HEVC)) {
      LOG(WARNING) << "HEIC requires libheif >= 1.19.8 with an HEVC decoder";
      return;
    }
    available = true;
  }

#define DECLARE_HEIC(name) decltype(&::name) name = nullptr;
  HEIC_SYMBOLS(DECLARE_HEIC)
#undef DECLARE_HEIC
  base::NativeLibrary library = nullptr;
  bool available = false;
};
#undef HEIC_SYMBOLS

// Release every object through the same module that allocated it. Avoid
// applying Chromium's internal CFI type checks to external dlsym calls.
struct DecodeResources {
  explicit DecodeResources(const HeicApi& api) : api(api) {}
  DISABLE_CFI_DLSYM ~DecodeResources() {
    if (image) api.heif_image_release(image);
    if (options) api.heif_decoding_options_free(options);
    if (nclx) api.heif_nclx_color_profile_free(nclx);
    if (handle) api.heif_image_handle_release(handle);
    if (context) api.heif_context_free(context);
  }
  // Released after the destructor frees libheif pixel planes and context.
  std::unique_ptr<DecodeMemory> memory;
  const HeicApi& api;
  heif_context* context = nullptr;
  heif_image_handle* handle = nullptr;
  heif_color_profile_nclx* nclx = nullptr;
  heif_decoding_options* options = nullptr;
  heif_image* image = nullptr;
};

bool ValidSize(int width, int height) {
  return width > 0 && height > 0 && width <= kMaxDimension &&
         height <= kMaxDimension &&
         static_cast<int64_t>(width) * height <= kMaxPixels;
}

DISABLE_CFI_DLSYM sk_sp<SkColorSpace> ReadColorSpace(
    DecodeResources& resources, HeicMetadata* metadata) {
  const auto& api = resources.api;
  const size_t icc_size =
      api.heif_image_handle_get_raw_color_profile_size(resources.handle);
  if (icc_size > 0) {
    if (icc_size > 1024 * 1024) {
      return nullptr;
    }
    std::string icc(icc_size, '\0');
    if (api.heif_image_handle_get_raw_color_profile(
            resources.handle, icc.data()).code != heif_error_Ok) {
      return nullptr;
    }
    auto profile = skia::ColorProfile::Make(base::as_byte_span(icc));
    if (!profile) {
      return nullptr;
    }
    auto space = profile->GetSkColorSpace();
    metadata->is_hdr = gfx::ColorSpace(*space).IsHDR();
    return space;
  }
  if (api.heif_image_handle_get_nclx_color_profile(
          resources.handle, &resources.nclx).code != heif_error_Ok ||
      !resources.nclx) {
    return SkColorSpace::MakeSRGB();
  }
  using Primaries = gfx::ColorSpace::PrimaryID;
  using Transfer = gfx::ColorSpace::TransferID;
  Primaries primaries;
  switch (resources.nclx->color_primaries) {
    case 1:
    case 2: primaries = Primaries::BT709; break;
    case 9: primaries = Primaries::BT2020; break;
    case 12: primaries = Primaries::P3; break;
    default: return nullptr;
  }
  Transfer transfer;
  switch (resources.nclx->transfer_characteristics) {
    case 1: transfer = Transfer::BT709; break;
    case 2:
    case 13: transfer = Transfer::SRGB; break;
    case 6: transfer = Transfer::SMPTE170M; break;
    case 8: transfer = Transfer::LINEAR; break;
    case 14: transfer = Transfer::BT2020_10; break;
    case 15: transfer = Transfer::BT2020_12; break;
    case 16: transfer = Transfer::PQ; break;
    case 18: transfer = Transfer::HLG; break;
    default: return nullptr;
  }
  const gfx::ColorSpace space(primaries, transfer);
  metadata->is_hdr = space.IsHDR();
  return space.ToSkColorSpace();
}

DISABLE_CFI_DLSYM skhdr::Metadata ReadHdrMetadata(DecodeResources& resources) {
  skhdr::Metadata metadata;
  heif_content_light_level clli{};
  if (resources.api.heif_image_handle_get_content_light_level(
          resources.handle, &clli)) {
    metadata.setContentLightLevelInformation(
        skhdr::ContentLightLevelInformation::MakeUint16(
            clli.max_content_light_level, clli.max_pic_average_light_level));
  }
  heif_mastering_display_colour_volume encoded{};
  heif_decoded_mastering_display_colour_volume decoded{};
  if (resources.api.heif_image_handle_get_mastering_display_colour_volume(
          resources.handle, &encoded) &&
      resources.api.heif_mastering_display_colour_volume_decode(
          &encoded, &decoded).code == heif_error_Ok) {
    skhdr::MasteringDisplayColorVolume mdcv;
    // HEVC mastering-display primaries are ordered G, B, R.
    mdcv.fDisplayPrimaries = {
        decoded.display_primaries_x[2], decoded.display_primaries_y[2],
        decoded.display_primaries_x[0], decoded.display_primaries_y[0],
        decoded.display_primaries_x[1], decoded.display_primaries_y[1],
        decoded.white_point_x, decoded.white_point_y};
    mdcv.fMaximumDisplayMasteringLuminance =
        decoded.max_display_mastering_luminance;
    mdcv.fMinimumDisplayMasteringLuminance =
        decoded.min_display_mastering_luminance;
    metadata.setMasteringDisplayColorVolume(mdcv);
  }
  return metadata;
}

bool ToneMapHdrPixels(const uint8_t* pixels, int stride, int bit_depth,
                      SkAlphaType alpha_type, sk_sp<SkColorSpace> color_space,
                      const skhdr::Metadata& metadata, SkBitmap& output,
                      const std::shared_ptr<std::atomic_bool>& canceled) {
  if (bit_depth < 8 || bit_depth > 16) {
    return false;
  }
  const float maximum = (1u << bit_depth) - 1;
  const int width = output.width();
  const int height = output.height();
  constexpr int kChunkRows = 64;
  std::vector<std::array<float, 4>> chunk(static_cast<size_t>(width) * kChunkRows);
  SkPaint paint;
  paint.setBlendMode(SkBlendMode::kSrc);
  paint.setColorFilter(metadata.makeToneMapColorFilter(0.0f, color_space.get()));
  SkCanvas canvas(output);
  // Process floating-point pixels in small row batches to bound peak memory
  // while avoiding per-scanline SkImage allocations.
  for (int y_start = 0; y_start < height; y_start += kChunkRows) {
    if (Stopped(canceled)) return false;
    const int rows_in_chunk = std::min(kChunkRows, height - y_start);
    for (int r = 0; r < rows_in_chunk; ++r) {
      const int y = y_start + r;
      // SAFETY: DecodeHandle validated the decoded dimensions and the stride
      // against eight bytes per pixel. libheif owns all decoded image rows.
      const auto bytes = UNSAFE_BUFFERS(base::span(
          pixels + static_cast<size_t>(y) * stride,
          static_cast<size_t>(width) * 8));
      const auto row_data = base::span(chunk).subspan(
          static_cast<size_t>(r) * width, static_cast<size_t>(width));
      for (int x = 0; x < width; ++x) {
        for (size_t channel = 0; channel < 4; ++channel) {
          const size_t offset = static_cast<size_t>(x) * 8 + channel * 2;
          const unsigned value = bytes[offset] |
              (static_cast<unsigned>(bytes[offset + 1]) << 8);
          row_data[x][channel] = value / maximum;
        }
        if (alpha_type == kOpaque_SkAlphaType) {
          row_data[x][3] = 1.0f;
        }
      }
    }
    const auto chunk_info = SkImageInfo::Make(
        width, rows_in_chunk, kRGBA_F32_SkColorType, alpha_type, color_space);
    const auto image = SkImages::RasterFromPixmap(
        SkPixmap(chunk_info, chunk.data(), chunk_info.minRowBytes()), nullptr, nullptr);
    if (!image) {
      return false;
    }
    canvas.drawImage(image, 0, y_start, SkSamplingOptions(), &paint);
  }
  return true;
}

// Keep each attempt's resources independent so a broken thumbnail can
// safely fall back to the primary image.
DISABLE_CFI_DLSYM SkBitmap DecodeHandle(DecodeResources& resources,
                                       HeicMetadata* metadata,
                                       const std::shared_ptr<std::atomic_bool>& canceled) {
  if (Stopped(canceled)) return {};
  const HeicApi* api = &resources.api;
  metadata->has_alpha =
      api->heif_image_handle_has_alpha_channel(resources.handle) != 0;
  auto source_space = ReadColorSpace(resources, metadata);
  if (!source_space) {
    return {};
  }
  const int source_width = api->heif_image_handle_get_width(resources.handle);
  const int source_height = api->heif_image_handle_get_height(resources.handle);
  if (!ValidSize(source_width, source_height)) return {};
  // libheif's interleaved plane plus our full-size N32 output coexist. Leave
  // room for stride alignment, HDR scanlines and the bounded resize output.
  const size_t pixel_count = static_cast<size_t>(source_width) * source_height;
  resources.memory = std::make_unique<DecodeMemory>(
      pixel_count * (metadata->is_hdr ? 12 : 8) + 32 * 1024 * 1024, canceled);
  if (!resources.memory->acquired() || Stopped(canceled)) return {};
  resources.options = api->heif_decoding_options_alloc();
  if (!resources.options) {
    return {};
  }
  resources.options->ignore_transformations = false;
  resources.options->strict_decoding = true;
  resources.options->convert_hdr_to_8bit = false;
  const heif_chroma chroma = metadata->is_hdr
      ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA;
  const auto result = api->heif_decode_image(
      resources.handle, &resources.image, heif_colorspace_RGB,
      chroma, resources.options);
  if (result.code != heif_error_Ok || !resources.image) {
    LOG(WARNING) << "HEIC decode failed: code=" << result.code
                 << " subcode=" << result.subcode;
    return {};
  }
  if (Stopped(canceled)) return {};
  // The decoded image can differ from the handle after irot/imir/clap. Never
  // use the handle's dimensions to access the returned pixel plane.
  const int width = api->heif_image_get_primary_width(resources.image);
  const int height = api->heif_image_get_primary_height(resources.image);
  if (!ValidSize(width, height) ||
      api->heif_image_get_colorspace(resources.image) != heif_colorspace_RGB ||
      api->heif_image_get_chroma_format(resources.image) != chroma) {
    return {};
  }
  int stride = 0;
  const uint8_t* pixels = api->heif_image_get_plane_readonly(
      resources.image, heif_channel_interleaved, &stride);
  if (!pixels || stride < static_cast<int64_t>(width) *
                             (metadata->is_hdr ? 8 : 4)) {
    return {};
  }
  const SkAlphaType alpha_type =
      !metadata->has_alpha ? kOpaque_SkAlphaType
          : api->heif_image_is_premultiplied_alpha(resources.image)
              ? kPremul_SkAlphaType : kUnpremul_SkAlphaType;
  SkBitmap bitmap;
  const auto output_info = SkImageInfo::MakeN32Premul(
      width, height, SkColorSpace::MakeSRGB());
  if (!bitmap.tryAllocPixels(output_info)) {
    return {};
  }
  if (metadata->is_hdr) {
    const int bit_depth = api->heif_image_get_bits_per_pixel_range(
        resources.image, heif_channel_interleaved);
    if (!ToneMapHdrPixels(pixels, stride, bit_depth, alpha_type,
                          std::move(source_space), ReadHdrMetadata(resources),
                          bitmap, canceled)) {
      return {};
    }
  } else {
    const auto source_info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, alpha_type, std::move(source_space));
    if (!SkPixmap(source_info, pixels, stride).readPixels(bitmap.pixmap())) {
      return {};
    }
  }
  return bitmap;
}

}  // namespace

DISABLE_CFI_DLSYM SkBitmap DecodeHeicThumbnail(const std::string& encoded,
    int maximum_edge, const std::shared_ptr<std::atomic_bool>& canceled) {
  if (encoded.empty() || encoded.size() > kMaxEncodedBytes ||
      maximum_edge <= 0 || maximum_edge > 2048) return {};
  HeicMetadata primary_metadata;
  HeicMetadata* metadata = &primary_metadata;
  // Serialize large full-image decodes to bound peak memory during grid loads.
  static base::NoDestructor<base::Lock> lock;
  base::AutoLock guard(*lock);
  if (Stopped(canceled)) return {};
  static const HeicApi api_instance;
  const HeicApi* api = &api_instance;
  if (!api->available) {
    return {};
  }

  DecodeResources resources(*api);
  resources.context = api->heif_context_alloc();
  if (!resources.context) {
    return {};
  }
  api->heif_context_set_maximum_image_size_limit(resources.context,
                                                kMaxDimension);
  api->heif_context_set_max_decoding_threads(resources.context, 2);
  const auto read_result = api->heif_context_read_from_memory_without_copy(
      resources.context, encoded.data(), encoded.size(), nullptr);
  if (read_result.code != heif_error_Ok ||
      api->heif_context_get_number_of_top_level_images(resources.context) != 1 ||
      api->heif_context_get_primary_image_handle(
          resources.context, &resources.handle).code != heif_error_Ok ||
      !resources.handle) {
    LOG(WARNING) << "HEIC primary still image unavailable";
    return {};
  }
  if (!ValidSize(api->heif_image_handle_get_width(resources.handle),
                 api->heif_image_handle_get_height(resources.handle))) {
    return {};
  }
  const gfx::Size primary_size(
      api->heif_image_handle_get_width(resources.handle),
      api->heif_image_handle_get_height(resources.handle));
  if (maximum_edge > 0) {
    // Validate the primary image even when an embedded thumbnail is available.
    const auto primary_space = ReadColorSpace(resources, metadata);
    if (!primary_space) {
      return {};
    }
    if (resources.nclx) {
      api->heif_nclx_color_profile_free(resources.nclx);
      resources.nclx = nullptr;
    }

    std::array<heif_item_id, 16> ids{};
    // HDR thumbnails are derived from the primary pixels and their metadata,
    // so an independently graded embedded SDR thumbnail cannot change colors.
    const int count = metadata->is_hdr ? 0
        : api->heif_image_handle_get_list_of_thumbnail_IDs(
              resources.handle, ids.data(), static_cast<int>(ids.size()));
    for (int index = 0; index < std::clamp(count, 0, 16); ++index) {
      DecodeResources thumbnail(*api);
      if (api->heif_image_handle_get_thumbnail(
              resources.handle, ids.at(index), &thumbnail.handle).code !=
              heif_error_Ok || !thumbnail.handle) {
        continue;
      }
      const auto suitable = [&](int width, int height) {
        return ValidSize(width, height) &&
            std::max(width, height) >= maximum_edge &&
            std::abs(static_cast<double>(width) * primary_size.height() /
                         (static_cast<double>(height) * primary_size.width()) -
                     1.0) < 0.02;
      };
      if (!suitable(api->heif_image_handle_get_width(thumbnail.handle),
                    api->heif_image_handle_get_height(thumbnail.handle))) {
        continue;
      }
      HeicMetadata thumbnail_metadata;
      SkBitmap bitmap = DecodeHandle(thumbnail, &thumbnail_metadata, canceled);
      if (!bitmap.drawsNothing() && suitable(bitmap.width(), bitmap.height())) {
        return ResizeThumbnail(std::move(bitmap), maximum_edge, canceled);
      }
    }
  }
  return ResizeThumbnail(DecodeHandle(resources, metadata, canceled),
                         maximum_edge, canceled);
}

}  // namespace crfiles

#endif  // !BUILDFLAG(IS_MAC)
