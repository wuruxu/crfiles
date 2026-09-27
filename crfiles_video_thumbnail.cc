// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_video_thumbnail.h"
#include "crFiles/crfiles_video_thumbnail.mojom.h"
#include "base/check.h"
#include "base/functional/callback.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cerrno>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include "base/base_paths.h"
#include "base/compiler_specific.h"
#include "base/containers/auto_spanification_helper.h"
#include "base/containers/span_reader.h"
#include "base/scoped_native_library.h"
#include "base/logging.h"
#include "cc/paint/tone_map_util.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/time/time.h"
#include "media/media_buildflags.h"
#include "build/build_config.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/effects/SkColorMatrix.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#if BUILDFLAG(ENABLE_FFMPEG)
#include "media/base/media.h"
#include "media/base/video_decoder_config.h"
#include "media/ffmpeg/ffmpeg_common.h"
#include "media/filters/ffmpeg_glue.h"
#include "crFiles/third_party/libde265/de265.h"
extern "C" {
#include <libavutil/mem.h>
}
#endif
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif
namespace crfiles {
namespace {
constexpr int kMaximumEdge = 128;
#if BUILDFLAG(ENABLE_FFMPEG)
// Local-file-only custom AVIO: never pass a filename/URL to FFmpeg. Bound total
// reads (not source file size), probing, decoder threads and decoded dimensions.
class ThumbnailInput final : public media::FFmpegURLProtocol {
 public:
  explicit ThumbnailInput(const base::FilePath& path);
  ~ThumbnailInput();
  int Read(base::span<uint8_t> data) override {
    if (data.empty()) return 0;
    if (Expired() || data.size() > remaining_) return AVERROR(EIO);
    auto count = file_.ReadAtCurrentPos(data);
    if (!count) return AVERROR(EIO);
    if (*count == 0) return AVERROR_EOF;
    remaining_ -= *count;
    return static_cast<int>(*count);
  }
  bool GetPosition(int64_t* value) override { *value = file_.Seek(base::File::FROM_CURRENT, 0); return *value >= 0; }
  bool SetPosition(int64_t value) override { return !Expired() && value >= 0 && value <= size_ && file_.Seek(base::File::FROM_BEGIN, value) == value; }
  bool GetSize(int64_t* value) override { *value = size_; return size_ >= 0; }
  bool IsStreaming() override { return false; }
  bool Expired() const { return base::TimeTicks::Now() >= deadline_; }
 private:
  base::File file_;
  int64_t size_;
  size_t remaining_ = 64 * 1024 * 1024;
  base::TimeTicks deadline_ = base::TimeTicks::Now() + base::Seconds(6);
};
ThumbnailInput::ThumbnailInput(const base::FilePath& path)
    : file_(path, base::File::FLAG_OPEN | base::File::FLAG_READ), size_(file_.GetLength()) {}
ThumbnailInput::~ThumbnailInput() = default;

// Sample planar YUV at thumbnail resolution in floating point, retaining
// 10/12-bit precision and out-of-gamut values until HDR-to-SDR tone mapping.
SkBitmap FrameThumbnail(const AVFrame& frame, const media::VideoDecoderConfig& config, int edge) {
  if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192 ||
      static_cast<int64_t>(frame.width) * frame.height > 16 * 1024 * 1024) return {};
  int depth = 8, subsample_x = 2, subsample_y = 2;
  switch (frame.format) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: break;
    case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: subsample_y = 1; break;
    case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P: subsample_x = subsample_y = 1; break;
    case AV_PIX_FMT_YUV420P10LE: depth = 10; break;
    case AV_PIX_FMT_YUV422P10LE: depth = 10; subsample_y = 1; break;
    case AV_PIX_FMT_YUV444P10LE: depth = 10; subsample_x = subsample_y = 1; break;
    case AV_PIX_FMT_YUV420P12LE: depth = 12; break;
    case AV_PIX_FMT_YUV422P12LE: depth = 12; subsample_y = 1; break;
    case AV_PIX_FMT_YUV444P12LE: depth = 12; subsample_x = subsample_y = 1; break;
    default: return {};
  }
  const int sample_bytes = depth > 8 ? 2 : 1;
  const float maximum = (1 << depth) - 1;
  std::array<base::span<const uint8_t>, 3> planes;
  std::array<int, 3> columns, rows, strides;
  const auto data = base::span(frame.data);
  const auto linesize = base::span(frame.linesize);
  for (size_t plane = 0; plane < planes.size(); ++plane) {
    columns[plane] = plane ? (frame.width + subsample_x - 1) / subsample_x : frame.width;
    rows[plane] = plane ? (frame.height + subsample_y - 1) / subsample_y : frame.height;
    strides[plane] = linesize[plane];
    if (!data[plane] || strides[plane] < columns[plane] * sample_bytes || strides[plane] > 128 * 1024) return {};
    const size_t length = static_cast<size_t>(rows[plane] - 1) * strides[plane] + columns[plane] * sample_bytes;
    // SAFETY: decoder-owned planar image data remains alive throughout this
    // call. Include only visible row bytes, not padding after the last row.
    planes[plane] = UNSAFE_BUFFERS(base::span(data[plane], length));
  }
  const auto fallback = config.color_space_info();
  const bool hdr = frame.color_trc == AVCOL_TRC_SMPTE2084 || frame.color_trc == AVCOL_TRC_ARIB_STD_B67 || fallback.IsHDR();
  const int primaries = frame.color_primaries != AVCOL_PRI_UNSPECIFIED ? frame.color_primaries :
      (hdr ? AVCOL_PRI_BT2020 : static_cast<int>(fallback.primaries()));
  const int transfer = frame.color_trc != AVCOL_TRC_UNSPECIFIED ? frame.color_trc : static_cast<int>(fallback.transfer());
  const int matrix_id = frame.colorspace != AVCOL_SPC_UNSPECIFIED ? frame.colorspace :
      (hdr ? AVCOL_SPC_BT2020_NCL : static_cast<int>(fallback.matrix()));
  const bool jpeg = frame.format == AV_PIX_FMT_YUVJ420P || frame.format == AV_PIX_FMT_YUVJ422P || frame.format == AV_PIX_FMT_YUVJ444P;
  const auto range = frame.color_range == AVCOL_RANGE_JPEG || jpeg ? gfx::ColorSpace::RangeID::FULL :
      frame.color_range == AVCOL_RANGE_MPEG ? gfx::ColorSpace::RangeID::LIMITED : fallback.range();
  const auto colors = media::VideoColorSpace(primaries, transfer, matrix_id, range).GuessGfxColorSpace();
  SkYUVColorSpace yuv_space;
  if (colors.GetMatrixID() == gfx::ColorSpace::MatrixID::RGB || !colors.ToSkYUVColorSpace(depth, &yuv_space)) return {};
  std::array<float, 20> matrix;
  SkColorMatrix::YUVtoRGB(yuv_space).getRowMajor(matrix.data());
  const double sample_scale = std::min(1.0, static_cast<double>(edge) / std::max(frame.width, frame.height));
  const int sample_width = std::max(1, static_cast<int>(std::round(frame.width * sample_scale)));
  const int sample_height = std::max(1, static_cast<int>(std::round(frame.height * sample_scale)));
  SkBitmap pixels;
  if (!pixels.tryAllocPixels(SkImageInfo::Make(sample_width, sample_height, kRGBA_F32_SkColorType,
      kPremul_SkAlphaType, colors.GetAsFullRangeRGB().ToSkColorSpace()))) return {};
  auto output = base::subtle::reinterpret_span<float>(UNSAFE_SKBITMAP_TO_BYTES_SPAN(pixels));
  auto sample = [&](size_t plane, float x, float y) {
    x = std::clamp((x + 0.5f) / (plane ? subsample_x : 1) - 0.5f, 0.0f, static_cast<float>(columns[plane] - 1));
    y = std::clamp((y + 0.5f) / (plane ? subsample_y : 1) - 0.5f, 0.0f, static_cast<float>(rows[plane] - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, columns[plane] - 1), y1 = std::min(y0 + 1, rows[plane] - 1);
    auto read = [&](int column, int row) {
      const size_t offset = static_cast<size_t>(row) * strides[plane] + column * sample_bytes;
      unsigned value = planes[plane][offset];
      if (sample_bytes == 2) value |= static_cast<unsigned>(planes[plane][offset + 1]) << 8;
      return value / maximum;
    };
    return std::lerp(std::lerp(read(x0, y0), read(x1, y0), x - x0),
        std::lerp(read(x0, y1), read(x1, y1), x - x0), y - y0);
  };
  for (int y = 0; y < sample_height; ++y) {
    for (int x = 0; x < sample_width; ++x) {
      const float sx = (x + 0.5f) * frame.width / sample_width - 0.5f;
      const float sy = (y + 0.5f) * frame.height / sample_height - 0.5f;
      const std::array<float, 3> yuv = {sample(0, sx, sy), sample(1, sx, sy), sample(2, sx, sy)};
      const size_t offset = y * (pixels.rowBytes() / sizeof(float)) + x * 4;
      for (size_t c = 0; c < 3; ++c)
        output[offset + c] = matrix[c * 5] * yuv[0] + matrix[c * 5 + 1] * yuv[1] + matrix[c * 5 + 2] * yuv[2] + matrix[c * 5 + 4];
      output[offset + 3] = 1.f;
    }
  }
  const auto transform = config.video_transformation();
  double width = config.natural_size().width(), height = config.natural_size().height();
  if (width <= 0 || height <= 0) { width = frame.width; height = frame.height; }
  const bool transpose = transform.rotation == media::VIDEO_ROTATION_90 || transform.rotation == media::VIDEO_ROTATION_270;
  if (transpose) std::swap(width, height);
  const double scale = edge / std::max(width, height);
  const int target_width = std::clamp(static_cast<int>(std::round(width * scale)), 1, edge);
  const int target_height = std::clamp(static_cast<int>(std::round(height * scale)), 1, edge);
  SkBitmap result;
  if (!result.tryAllocPixels(SkImageInfo::MakeN32Premul(target_width, target_height, SkColorSpace::MakeSRGB()))) return {};
  SkCanvas canvas(result);
  canvas.clear(SK_ColorBLACK);
  canvas.translate(target_width / 2.f, target_height / 2.f);
  if (transform.mirrored) canvas.scale(-1, 1);
  canvas.rotate(static_cast<float>(transform.rotation));
  const float draw_width = transpose ? target_height : target_width;
  const float draw_height = transpose ? target_width : target_height;
  const auto image = pixels.asImage();
  if (!image) return {};
  SkPaint paint;
  paint.setBlendMode(SkBlendMode::kSrc);
  if (cc::ToneMapUtil::UseGlobalToneMapFilter(image.get()))
    cc::ToneMapUtil::AddGlobalToneMapFilterToPaint(paint, image.get(), config.hdr_metadata(), 0.f);
  canvas.drawImageRect(image, SkRect::MakeXYWH(-draw_width / 2, -draw_height / 2, draw_width, draw_height),
      SkSamplingOptions(SkFilterMode::kLinear), &paint);
  return result;
}

// The stable libde265 C API provides software HEVC decoding independently of
// GPU drivers and the FFmpeg codec selection used by the rest of Chromium.
#define CRFILES_DE265_FUNCTIONS(V) \
  V(de265_get_version_number_major) \
  V(de265_get_version_number_minor) \
  V(de265_get_version_number_maintenance) \
  V(de265_new_decoder) \
  V(de265_free_decoder) \
  V(de265_push_NAL) \
  V(de265_push_data) \
  V(de265_push_end_of_frame) \
  V(de265_flush_data) \
  V(de265_decode) \
  V(de265_peek_next_picture) \
  V(de265_get_image_width) \
  V(de265_get_image_height) \
  V(de265_get_chroma_format) \
  V(de265_get_bits_per_pixel) \
  V(de265_get_image_plane) \
  V(de265_get_image_full_range_flag) \
  V(de265_get_image_colour_primaries) \
  V(de265_get_image_transfer_characteristics) \
  V(de265_get_image_matrix_coefficients) \
  V(de265_get_default_image_allocation_functions) \
  V(de265_set_image_allocation_functions)

class HevcDecoder {
 public:
  HevcDecoder();
  ~HevcDecoder();
  bool valid() const { return context_ != nullptr; }
  DISABLE_CFI_DLSYM bool Configure(base::span<const uint8_t> extra);
  DISABLE_CFI_DLSYM bool Push(base::span<const uint8_t> packet);
  DISABLE_CFI_DLSYM SkBitmap Drain(bool flush, const media::VideoDecoderConfig& config,
      int edge, const ThumbnailInput& input);
  bool failed() const { return failed_; }
 private:
  DISABLE_CFI_DLSYM bool PushNal(base::span<const uint8_t> nal);
  DISABLE_CFI_DLSYM SkBitmap Convert(const de265_image* image, const media::VideoDecoderConfig& config, int edge);
  DISABLE_CFI_DLSYM static int Allocate(de265_decoder_context* decoder_context, de265_image_spec* spec,
      de265_image* image, void* user);
  DISABLE_CFI_DLSYM static void Release(de265_decoder_context* decoder_context, de265_image* image, void* user);
  base::ScopedNativeLibrary library_;
#define DECLARE_DE265(name) decltype(&::name) name##_ = nullptr;
  CRFILES_DE265_FUNCTIONS(DECLARE_DE265)
#undef DECLARE_DE265
  de265_decoder_context* context_ = nullptr;
  const de265_image_allocation* default_allocation_ = nullptr;
  de265_image_allocation allocation_ = {&Allocate, &Release};
  std::map<de265_image*, size_t> allocations_;
  size_t bytes_ = 0;
  int length_size_ = 0;
  bool failed_ = false;
};
DISABLE_CFI_DLSYM HevcDecoder::HevcDecoder() {
#if BUILDFLAG(IS_LINUX)
  library_.reset(base::LoadNativeLibrary(base::FilePath(FILE_PATH_LITERAL("libde265.so.0")), nullptr));
#elif BUILDFLAG(IS_WIN)
  base::FilePath directory;
  if (base::PathService::Get(base::DIR_EXE, &directory)) {
    for (const auto* filename : {"libde265.dll", "de265.dll"}) {
      library_.reset(LoadLibraryExW(directory.AppendASCII(filename).value().c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
      if (library_.is_valid()) break;
    }
  }
#endif
  if (!library_.is_valid()) { LOG(WARNING) << "HEVC thumbnails require libde265.so.0 or app-local libde265.dll"; return; }
#define LOAD_DE265(name) \
  name##_ = reinterpret_cast<decltype(name##_)>(library_.GetFunctionPointer(#name)); \
  if (!name##_) return;
  CRFILES_DE265_FUNCTIONS(LOAD_DE265)
#undef LOAD_DE265
  if (std::tuple(de265_get_version_number_major_(), de265_get_version_number_minor_(),
      de265_get_version_number_maintenance_()) < std::tuple(1, 0, 16)) { LOG(WARNING) << "HEVC thumbnails require libde265 >= 1.0.16"; return; }
  default_allocation_ = de265_get_default_image_allocation_functions_();
  if (!default_allocation_ || !default_allocation_->get_buffer || !default_allocation_->release_buffer) return;
  context_ = de265_new_decoder_();
  // No worker threads: the parent already limits this process to one video job.
  if (context_) de265_set_image_allocation_functions_(context_, &allocation_, this);
}
DISABLE_CFI_DLSYM HevcDecoder::~HevcDecoder() {
  if (context_) de265_free_decoder_(context_);
}
DISABLE_CFI_DLSYM int HevcDecoder::Allocate(de265_decoder_context* decoder_context, de265_image_spec* spec,
    de265_image* image, void* user) {
  auto& self = *static_cast<HevcDecoder*>(user);
  if (spec->width <= 0 || spec->height <= 0 || spec->width > 8192 || spec->height > 8192 ||
      spec->alignment < 0 || spec->alignment > 4096 ||
      static_cast<int64_t>(spec->width) * spec->height > 16 * 1024 * 1024) return 0;
  // Conservative allowance for 12-bit 4:4:4 plus alignment. Bound retained
  // decoder picture buffers before allocation, including reference pictures.
  const size_t bytes = static_cast<size_t>(spec->width + 2 * spec->alignment) *
      (spec->height + 2 * spec->alignment) * 8;
  if (self.allocations_.contains(image) || self.allocations_.size() >= 32 || bytes > 384 * 1024 * 1024 - self.bytes_) return 0;
  if (!self.default_allocation_->get_buffer(decoder_context, spec, image, nullptr)) return 0;
  self.allocations_.emplace(image, bytes); self.bytes_ += bytes;
  return 1;
}
DISABLE_CFI_DLSYM void HevcDecoder::Release(de265_decoder_context* decoder_context, de265_image* image, void* user) {
  auto& self = *static_cast<HevcDecoder*>(user);
  self.default_allocation_->release_buffer(decoder_context, image, nullptr);
  auto found = self.allocations_.find(image);
  if (found != self.allocations_.end()) { self.bytes_ -= found->second; self.allocations_.erase(found); }
}
DISABLE_CFI_DLSYM bool HevcDecoder::PushNal(base::span<const uint8_t> nal) {
  return nal.size() >= 2 && nal.size() <= 8 * 1024 * 1024 &&
      de265_push_NAL_(context_, nal.data(), static_cast<int>(nal.size()), 0, nullptr) == DE265_OK;
}
DISABLE_CFI_DLSYM bool HevcDecoder::Configure(base::span<const uint8_t> extra) {
  if (extra.size() > 1024 * 1024) return false;
  if (extra.empty()) return true;  // Annex B parameters arrive in the stream.
  if (extra[0] != 1) return extra.size() <= 1024 * 1024 &&
      de265_push_data_(context_, extra.data(), static_cast<int>(extra.size()), 0, nullptr) == DE265_OK;
  if (extra.size() < 23) return false;
  length_size_ = (extra[21] & 3) + 1;
  const int arrays = extra[22];
  base::SpanReader reader(extra.subspan(23u));
  for (int array = 0; array < arrays; ++array) {
    uint16_t count = 0;
    if (!reader.Skip(1u) || !reader.ReadU16BigEndian(count)) return false;
    for (int i = 0; i < count; ++i) {
      uint16_t length = 0;
      if (!reader.ReadU16BigEndian(length)) return false;
      auto nal = reader.Read(length);
      if (!nal || !PushNal(*nal)) return false;
    }
  }
  return reader.remaining() == 0;
}
DISABLE_CFI_DLSYM bool HevcDecoder::Push(base::span<const uint8_t> packet) {
  if (packet.empty() || packet.size() > 8 * 1024 * 1024) return false;
  if (!length_size_) {
    if (de265_push_data_(context_, packet.data(), static_cast<int>(packet.size()), 0, nullptr) != DE265_OK) return false;
  } else {
    base::SpanReader reader(packet);
    while (reader.remaining()) {
      auto prefix = reader.Read(static_cast<size_t>(length_size_));
      if (!prefix) return false;
      uint32_t length = 0;
      for (uint8_t byte : *prefix) length = (length << 8) | byte;
      auto nal = reader.Read(length);
      if (!nal || !PushNal(*nal)) return false;
    }
  }
  de265_push_end_of_frame_(context_);
  return true;
}
DISABLE_CFI_DLSYM SkBitmap HevcDecoder::Convert(const de265_image* image,
    const media::VideoDecoderConfig& config, int edge) {
  AVFrame frame = {};
  frame.width = de265_get_image_width_(image, 0); frame.height = de265_get_image_height_(image, 0);
  const int depth = de265_get_bits_per_pixel_(image, 0);
  const auto chroma = de265_get_chroma_format_(image);
  if (chroma == de265_chroma_420)
    frame.format = depth == 8 ? AV_PIX_FMT_YUV420P : depth == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P12LE;
  else if (chroma == de265_chroma_422)
    frame.format = depth == 8 ? AV_PIX_FMT_YUV422P : depth == 10 ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P12LE;
  else if (chroma == de265_chroma_444)
    frame.format = depth == 8 ? AV_PIX_FMT_YUV444P : depth == 10 ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P12LE;
  else return {};
  if (depth != 8 && depth != 10 && depth != 12) return {};
  auto planes = base::span(frame.data); auto strides = base::span(frame.linesize);
  for (int plane = 0; plane < 3; ++plane) {
    if (de265_get_bits_per_pixel_(image, plane) != depth) return {};
    const int columns = plane && chroma != de265_chroma_444 ? (frame.width + 1) / 2 : frame.width;
    const int rows = plane && chroma == de265_chroma_420 ? (frame.height + 1) / 2 : frame.height;
    if (de265_get_image_width_(image, plane) != columns || de265_get_image_height_(image, plane) != rows) return {};
    // Borrow the picture only until the synchronous conversion returns.
    planes[plane] = const_cast<uint8_t*>(de265_get_image_plane_(image, plane, &strides[plane]));
  }
  frame.color_primaries = static_cast<AVColorPrimaries>(de265_get_image_colour_primaries_(image));
  frame.color_trc = static_cast<AVColorTransferCharacteristic>(de265_get_image_transfer_characteristics_(image));
  frame.colorspace = static_cast<AVColorSpace>(de265_get_image_matrix_coefficients_(image));
  frame.color_range = de265_get_image_full_range_flag_(image) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
  return FrameThumbnail(frame, config, edge);
}
DISABLE_CFI_DLSYM SkBitmap HevcDecoder::Drain(bool flush, const media::VideoDecoderConfig& config,
    int edge, const ThumbnailInput& input) {
  if (flush && de265_flush_data_(context_) != DE265_OK) { failed_ = true; return {}; }
  for (int work = 0; work < 1024 && !input.Expired(); ++work) {
    if (const auto* image = de265_peek_next_picture_(context_)) {
      auto result = Convert(image, config, edge); failed_ = result.isNull(); return result;
    }
    int more = 0;
    const auto error = de265_decode_(context_, &more);
    if (const auto* image = de265_peek_next_picture_(context_)) {
      auto result = Convert(image, config, edge); failed_ = result.isNull(); return result;
    }
    if (error == DE265_ERROR_WAITING_FOR_INPUT_DATA || (error == DE265_OK && !more)) return {};
    if (error != DE265_OK && error < 1000) { failed_ = true; return {}; }
  }
  failed_ = true;
  return {};
}
#undef CRFILES_DE265_FUNCTIONS

SkBitmap DecodeHevc(AVFormatContext* format, AVStream* video, const media::VideoDecoderConfig& config,
    int edge, const ThumbnailInput& input) {
  HevcDecoder decoder;
  if (!decoder.valid() || !decoder.Configure(media::AVCodecParametersExtraDataToSpan(video->codecpar))) return {};
  std::unique_ptr<AVPacket, media::ScopedPtrAVFreePacket> packet(av_packet_alloc());
  if (!packet) return {};
  for (int count = 0; count < 512 && !input.Expired(); ++count) {
    const int read = av_read_frame(format, packet.get());
    if (read < 0) return read == AVERROR_EOF ? decoder.Drain(true, config, edge, input) : SkBitmap();
    if (packet->stream_index == video->index) {
      if (!decoder.Push(media::AVPacketData(*packet))) return {};
      auto result = decoder.Drain(false, config, edge, input);
      if (!result.isNull() || decoder.failed()) return result;
    }
    av_packet_unref(packet.get());
  }
  return {};
}

SkBitmap DecodeVideo(const base::FilePath& path, int edge) {
  media::InitializeMediaLibrary();
  av_max_alloc(64 * 1024 * 1024);
  ThumbnailInput input(path);
  media::FFmpegGlue glue(&input);
  auto* format = glue.format_context();
  format->max_streams = 32;
  format->probesize = 2 * 1024 * 1024;
  format->max_analyze_duration = AV_TIME_BASE;
  format->interrupt_callback.callback = [](void* value) { return static_cast<ThumbnailInput*>(value)->Expired() ? 1 : 0; };
  format->interrupt_callback.opaque = &input;
  // MP4/MOV/Matroska headers carry decoder initialization data. Avoid
  // find_stream_info: it may start additional unbounded probe decoders.
  if (!glue.OpenContext(true) || input.Expired()) return {};
  AVStream* video = nullptr;
  for (auto* stream : media::AVFormatContextToSpan(format)) {
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      video = stream; break;
    }
  }
  if (!video) return {};
  media::VideoDecoderConfig config;
  if (!media::AVStreamToVideoDecoderConfig(video, &config)) return {};
  if (video->codecpar->codec_id == AV_CODEC_ID_HEVC) return DecodeHevc(format, video, config, edge, input);
  auto context = media::AVStreamToAVCodecContext(video);
  const auto* decoder = avcodec_find_decoder(video->codecpar->codec_id);
  if (!context || !decoder) return {};
  context->thread_count = 1;
  context->max_pixels = 16 * 1024 * 1024;
  if (avcodec_open2(context.get(), decoder, nullptr) < 0) return {};
  // Seek near the start when possible; use the first decoded frame after the
  // preceding keyframe. Tiny/nonseekable clips can still provide their first frame.
  if (video->time_base.num > 0 && video->time_base.den > 0) {
    const int64_t start = video->start_time == AV_NOPTS_VALUE ? 0 : video->start_time;
    const int64_t offset = video->time_base.den / video->time_base.num;
    if (start >= 0 && start <= std::numeric_limits<int64_t>::max() - offset && av_seek_frame(format, video->index, start + offset, AVSEEK_FLAG_BACKWARD) >= 0)
      avcodec_flush_buffers(context.get());
  }
  std::unique_ptr<AVPacket, media::ScopedPtrAVFreePacket> packet(av_packet_alloc());
  std::unique_ptr<AVFrame, media::ScopedPtrAVFreeFrame> frame(av_frame_alloc());
  if (!packet || !frame) return {};
  for (int count = 0; count < 512 && !input.Expired(); ++count) {
    const int read = av_read_frame(format, packet.get());
    if (read < 0 && read != AVERROR_EOF) break;
    if (read >= 0 && packet->stream_index != video->index) { av_packet_unref(packet.get()); continue; }
    const int sent = avcodec_send_packet(context.get(), read < 0 ? nullptr : packet.get());
    av_packet_unref(packet.get());
    if (sent < 0) break;
    const int received = avcodec_receive_frame(context.get(), frame.get());
    if (received == 0) return FrameThumbnail(*frame, config, edge);
    if (read < 0 || received != AVERROR(EAGAIN)) break;
  }
  return {};
}
#endif
}
bool IsVideoThumbnailPath(const base::FilePath& path) {
  for (const auto* extension : {FILE_PATH_LITERAL(".mp4"), FILE_PATH_LITERAL(".mov"), FILE_PATH_LITERAL(".m4v"),
      FILE_PATH_LITERAL(".mkv"), FILE_PATH_LITERAL(".webm")})
    if (path.MatchesExtension(extension)) return true;
  return false;
}
namespace {
class VideoThumbnailService : public mojom::VideoThumbnailService {
 public:
  VideoThumbnailService(mojo::PendingReceiver<mojom::VideoThumbnailService> receiver, base::OnceClosure quit);
  ~VideoThumbnailService() override;
  void Decode(const base::FilePath& path, uint32_t edge, DecodeCallback callback) override;
 private:
  mojo::Receiver<mojom::VideoThumbnailService> receiver_;
};
VideoThumbnailService::VideoThumbnailService(mojo::PendingReceiver<mojom::VideoThumbnailService> receiver,
    base::OnceClosure quit) : receiver_(this, std::move(receiver)) {
  receiver_.set_disconnect_handler(std::move(quit));
}
VideoThumbnailService::~VideoThumbnailService() = default;
void VideoThumbnailService::Decode(const base::FilePath& path, uint32_t edge, DecodeCallback callback) {
  mojom::VideoThumbnailPtr result;
  if (!path.IsAbsolute() || edge == 0 || edge > kMaximumEdge) {
    std::move(callback).Run(std::move(result)); return;
  }
#if BUILDFLAG(ENABLE_FFMPEG)
  {
    const auto bitmap = DecodeVideo(path, static_cast<int>(edge));
    if (!bitmap.isNull() && bitmap.width() <= static_cast<int>(edge) && bitmap.height() <= static_cast<int>(edge)) {
      result = mojom::VideoThumbnail::New();
      result->width = bitmap.width(); result->height = bitmap.height();
      result->pixels.resize(128u * 128u * 4u, 0);
      if (!bitmap.readPixels(SkImageInfo::MakeN32Premul(result->width, result->height),
          result->pixels.data(), result->width * 4, 0, 0)) result.reset();
    }
  }
#endif
  std::move(callback).Run(std::move(result));
}
}
int RunVideoThumbnailProcess() {
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::IO);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFilesVideo");
  base::Thread ipc_thread("crFilesVideoIPC");
  CHECK(ipc_thread.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0)));
  mojo::core::ScopedIPCSupport ipc(ipc_thread.task_runner(), mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
  auto endpoint = mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(*base::CommandLine::ForCurrentProcess());
  if (!endpoint.is_valid()) { base::ThreadPoolInstance::Get()->Shutdown(); return 1; }
  auto pipe = mojo::IncomingInvitation::AcceptIsolated(std::move(endpoint));
  if (!pipe.is_valid()) { base::ThreadPoolInstance::Get()->Shutdown(); return 1; }
  base::RunLoop loop;
  {
    VideoThumbnailService service(mojo::PendingReceiver<mojom::VideoThumbnailService>(std::move(pipe)), loop.QuitClosure());
    loop.Run();
  }
  base::ThreadPoolInstance::Get()->Shutdown();
  return 0;
}
}
