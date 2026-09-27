// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_cache_state.h"
#include "crFiles/crfiles_image_preview.h"
#include "crFiles/crfiles_thumbnail_cache.h"
#include "crFiles/crfiles_text_thumbnail.h"
#include "crFiles/crfiles_video_thumbnail.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <list>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>
#include <string>
#include <utility>
#include "base/files/file_util.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "third_party/pdfium/public/cpp/fpdf_scopers.h"
#include "third_party/pdfium/public/fpdf_progressive.h"
#include "third_party/pdfium/public/fpdfview.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/condition_variable.h"
#include "ui/gfx/image/image_skia_rep.h"
#include "third_party/skia/include/codec/SkCodec.h"
#include "third_party/skia/include/codec/SkEncodedOrigin.h"
#include "third_party/skia/include/codec/SkJpegDecoder.h"
#include "third_party/skia/include/codec/SkWebpDecoder.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/modules/svg/include/SkSVGDOM.h"
#include "third_party/skia/modules/svg/include/SkSVGSVG.h"
namespace crfiles {
namespace {
// Bound simultaneous raster bitmap allocations even when small compressed
// files expand to large images. Encoded buffers and codec scratch are separate.
struct RasterBudget {
  RasterBudget();
  ~RasterBudget();
  base::Lock lock;
  base::ConditionVariable available{&lock};
  size_t used = 0;
};
RasterBudget::RasterBudget() = default;
RasterBudget::~RasterBudget() = default;
RasterBudget& GetRasterBudget() {
  static base::NoDestructor<RasterBudget> budget;
  return *budget;
}
class RasterMemory {
 public:
  RasterMemory(size_t bytes, const std::shared_ptr<std::atomic_bool>& canceled);
  ~RasterMemory();
  bool acquired() const { return bytes_ != 0; }
 private:
  size_t bytes_ = 0;
};
RasterMemory::RasterMemory(size_t bytes, const std::shared_ptr<std::atomic_bool>& canceled) {
  constexpr size_t limit = 320 * 1024 * 1024;
  if (!bytes || bytes > limit) return;
  auto& budget = GetRasterBudget();
  base::AutoLock guard(budget.lock);
  while (!canceled || !canceled->load(std::memory_order_relaxed)) {
    if (bytes <= limit - budget.used) { budget.used += bytes; bytes_ = bytes; return; }
    budget.available.TimedWait(base::Milliseconds(50));
  }
}
RasterMemory::~RasterMemory() {
  if (!bytes_) return;
  auto& budget = GetRasterBudget();
  base::AutoLock guard(budget.lock);
  budget.used -= bytes_; budget.available.Broadcast();
}

// crFiles owns its PDFium instance. All PDF handles, including destruction,
// stay under this lock; workers may change between requests. Keep the SDK alive
// for the process lifetime so shutdown cannot race with a file worker.
struct PdfPreviewRuntime {
  PdfPreviewRuntime() {
    FPDF_LIBRARY_CONFIG config = {};
    config.version = 2;  // Use the renderer compiled into PDFium by default.
    FPDF_InitLibraryWithConfig(&config);
  }
  base::Lock lock;
};
struct PdfPreviewPause {
  IFSDK_PAUSE pause_api = {};
  const std::atomic_bool* canceled = nullptr;
  base::TimeTicks deadline;
  bool Stopped() const {
    return (canceled && canceled->load(std::memory_order_relaxed)) || base::TimeTicks::Now() >= deadline;
  }
};
gfx::ImageSkia RenderPdfThumbnail(const std::string& encoded, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled) {
  static base::NoDestructor<PdfPreviewRuntime> runtime;
  base::AutoLock guard(runtime->lock);
  PdfPreviewPause pause;
  pause.canceled = canceled.get();
  pause.deadline = base::TimeTicks::Now() + base::Seconds(2);
  pause.pause_api.version = 1;
  pause.pause_api.user = &pause;
  pause.pause_api.NeedToPauseNow = [](IFSDK_PAUSE* pause_api) -> FPDF_BOOL {
    return static_cast<PdfPreviewPause*>(pause_api->user)->Stopped();
  };
  if (pause.Stopped()) return {};
  // Loading/parsing is synchronous. Observe cancellation between PDFium calls
  // and cooperatively inside progressive rendering. No form/JavaScript host.
  ScopedFPDFDocument document(FPDF_LoadMemDocument64(encoded.data(), encoded.size(), nullptr));
  if (!document || pause.Stopped() || FPDF_GetPageCount(document.get()) < 1) return {};
  ScopedFPDFPage page(FPDF_LoadPage(document.get(), 0));
  if (!page || pause.Stopped()) return {};
  const double page_width = FPDF_GetPageWidthF(page.get());
  const double page_height = FPDF_GetPageHeightF(page.get());
  if (!std::isfinite(page_width) || !std::isfinite(page_height) || page_width <= 0 || page_height <= 0) return {};
  const double scale = maximum_edge / std::max(page_width, page_height);
  const int width = std::clamp(static_cast<int>(std::round(page_width * scale)), 1, maximum_edge);
  const int height = std::clamp(static_cast<int>(std::round(page_height * scale)), 1, maximum_edge);
  SkBitmap result;
  if (!result.tryAllocN32Pixels(width, height, true)) return {};
  result.eraseColor(SK_ColorWHITE);
  ScopedFPDFBitmap bitmap(FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx,
      result.getPixels(), static_cast<int>(result.rowBytes())));
  if (!bitmap || pause.Stopped()) return {};
  int status = FPDF_RenderPageBitmap_Start(bitmap.get(), page.get(), 0, 0, width, height, 0,
      FPDF_ANNOT | FPDF_RENDER_LIMITEDIMAGECACHE, &pause.pause_api);
  while (status == FPDF_RENDER_TOBECONTINUED && !pause.Stopped())
    status = FPDF_RenderPage_Continue(page.get(), &pause.pause_api);
  FPDF_RenderPage_Close(page.get());
  if (status != FPDF_RENDER_DONE || pause.Stopped()) return {};
  result.setImmutable();
  auto image = gfx::ImageSkia::CreateFromBitmap(result, 1.f);
  image.MakeThreadSafe();
  return image;
}
using ThumbnailKey = std::tuple<base::FilePath, int64_t, base::Time, base::Time, int>;
struct ThumbnailItem {
  ThumbnailKey key; gfx::ImageSkia image; size_t bytes;
  std::shared_ptr<FileCacheEpoch> epoch;
  uint64_t revision;
};
struct ThumbnailCache {
  base::Lock lock;
  std::list<ThumbnailItem> recent;
  std::map<ThumbnailKey, std::list<ThumbnailItem>::iterator> index;
  size_t bytes = 0;
  void Erase(std::list<ThumbnailItem>::iterator item) {
    bytes -= item->bytes; index.erase(item->key); recent.erase(item);
  }
};
ThumbnailCache& Thumbnails() {
  static base::NoDestructor<ThumbnailCache> cache;
  return *cache;
}
}
namespace {
gfx::ImageSkia LoadThumbnailImpl(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled, bool decode) {
  auto stopped = [&] { return canceled && canceled->load(std::memory_order_relaxed); };
  if (stopped() || maximum_edge <= 0 || maximum_edge > 720) return {};
  auto epoch = ObserveFileCachePath(path, false);
  const auto revision = epoch->revision.load();
  base::File::Info before;
  if (!base::GetFileInfo(path, &before) || before.is_directory || stopped()) return {};
  const ThumbnailKey key{path, before.size, before.last_modified, before.creation_time, maximum_edge};
  auto& cache = Thumbnails();
  {
    base::AutoLock guard(cache.lock);
    auto found = cache.index.find(key);
    if (found != cache.index.end() && !found->second->epoch->Matches(found->second->revision)) {
      cache.Erase(found->second); found = cache.index.end();
    }
    if (epoch->Matches(revision) && found != cache.index.end()) {
      cache.recent.splice(cache.recent.begin(), cache.recent, found->second);
      return found->second->image;
    }
  }
  if (!epoch->Matches(revision)) return {};
  // Disk hits share the cache-probe lane and bypass decoder slots entirely.
  // Neither filesystem IO nor image decoding holds the shared memory lock.
  const bool text = IsTextThumbnailPath(path);
  auto image = text ? gfx::ImageSkia() : ReadDiskThumbnail(path, before, maximum_edge, canceled);
  const bool disk_hit = !image.isNull();
  if (!disk_hit) {
    if (!decode || stopped()) return {};
    image = LoadImagePreviewCancelable(path, maximum_edge, canceled);
  }
  if (stopped() || image.isNull()) return {};
  base::File::Info after;
  if (!base::GetFileInfo(path, &after) || after.is_directory || before.size != after.size ||
      before.last_modified != after.last_modified || before.creation_time != after.creation_time) return {};
  if (!epoch->Matches(revision) || stopped()) return {};
  if (!disk_hit && !text) WriteDiskThumbnail(path, after, maximum_edge, image, canceled);
  const size_t bytes = sizeof(ThumbnailItem) + sizeof(ThumbnailKey) + 8 * sizeof(void*) +
      2 * path.value().capacity() * sizeof(base::FilePath::CharType) +
      image.GetRepresentation(1.f).GetBitmap().computeByteSize();
  constexpr size_t kMaxBytes = 64 * 1024 * 1024;
  base::AutoLock guard(cache.lock);
  if (stopped()) return {};
  if (!epoch->Matches(revision)) return {};
  if (bytes > kMaxBytes) return image;
  // Replace obsolete versions of this path instead of retaining every edit.
  auto item = cache.index.lower_bound(ThumbnailKey{path, std::numeric_limits<int64_t>::min(),
      base::Time(), base::Time(), 0});
  while (item != cache.index.end() && std::get<0>(item->first) == path) {
    auto old = item++;
    if (std::get<4>(old->first) == maximum_edge) cache.Erase(old->second);
  }
  while (!cache.recent.empty() && (cache.recent.size() >= 1024 || cache.bytes + bytes > kMaxBytes))
    cache.Erase(std::prev(cache.recent.end()));
  cache.recent.push_front({key, image, bytes, std::move(epoch), revision});
  cache.index.emplace(key, cache.recent.begin()); cache.bytes += bytes;
  return image;
}
}
gfx::ImageSkia FindCachedThumbnail(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled) {
  return LoadThumbnailImpl(path, maximum_edge, canceled, false);
}
gfx::ImageSkia LoadCachedThumbnail(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled) {
  return LoadThumbnailImpl(path, maximum_edge, canceled, true);
}
gfx::ImageSkia LoadImagePreview(const base::FilePath& path, int maximum_edge) {
  return LoadImagePreviewCancelable(path, maximum_edge, {});
}
gfx::ImageSkia LoadImagePreviewCancelable(const base::FilePath& path, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled) {
  auto stopped = [&] { return canceled && canceled->load(std::memory_order_relaxed); };
  if (stopped()) return {};
  if (maximum_edge <= 0 || maximum_edge > 2048) return {};
  if (IsTextThumbnailPath(path)) return LoadTextThumbnail(path, maximum_edge, canceled);
  if (IsVideoThumbnailPath(path)) return LoadVideoThumbnail(path, maximum_edge, canceled);
  std::string encoded;
  if (!base::ReadFileToStringWithMaxSize(path, &encoded, 64 * 1024 * 1024)) return {};
  if (stopped()) return {};
  if (path.MatchesExtension(FILE_PATH_LITERAL(".pdf")) || encoded.starts_with("%PDF-"))
    return RenderPdfThumbnail(encoded, maximum_edge, canceled);
  SkBitmap result;
  if (path.MatchesExtension(FILE_PATH_LITERAL(".svg"))) {
    SkMemoryStream stream(encoded.data(), encoded.size(), true);
    auto dom = SkSVGDOM::MakeFromStream(stream);
    if (!dom || !dom->getRoot()) return {};
    auto size = dom->containerSize();
    if (size.isEmpty() && dom->getRoot()->getViewBox()) {
      const auto& box = *dom->getRoot()->getViewBox();
      size = SkSize::Make(box.width(), box.height());
    }
    if (size.isEmpty()) size = SkSize::Make(300, 150);
    if (!std::isfinite(size.width()) || !std::isfinite(size.height())) return {};
    const float scale = maximum_edge / std::max(size.width(), size.height());
    if (!std::isfinite(scale)) return {};
    const int width = std::max(1, static_cast<int>(std::round(size.width() * scale)));
    const int height = std::max(1, static_cast<int>(std::round(size.height() * scale)));
    if (!result.tryAllocN32Pixels(width, height)) return {};
    result.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas(result);
    canvas.scale(scale, scale);
    // Resolve percentage dimensions against the document's viewport.
    dom->setContainerSize(size);
    if (stopped()) return {};
    dom->render(&canvas);
  } else {
    auto data = SkData::MakeWithCopy(encoded.data(), encoded.size());
    std::unique_ptr<SkCodec> codec;
    SkCodec::Result status;
    if (SkJpegDecoder::IsJpeg(encoded.data(), encoded.size()))
      codec = SkJpegDecoder::Decode(std::move(data), &status);
    else if (SkWebpDecoder::IsWebp(encoded.data(), encoded.size()))
      codec = SkWebpDecoder::Decode(std::move(data), &status);
    else codec = SkCodec::MakeFromData(std::move(data));
    if (!codec) return {};
    const auto original = codec->getInfo();
    if (original.width() <= 0 || original.height() <= 0) return {};
    const float scale = std::min(1.f, static_cast<float>(maximum_edge) /
        std::max(original.width(), original.height()));
    const auto dimensions = codec->getScaledDimensions(scale);
    // Some codecs must decode at full resolution before downsampling.
    if (dimensions.width() <= 0 || dimensions.height() <= 0 ||
        static_cast<int64_t>(dimensions.width()) * dimensions.height() > 64 * 1024 * 1024) return {};
    RasterMemory memory(static_cast<size_t>(dimensions.width()) * dimensions.height() * 4 +
        static_cast<size_t>(maximum_edge) * maximum_edge * 4, canceled);
    if (!memory.acquired() || stopped()) return {};
    auto info = SkImageInfo::MakeN32Premul(dimensions.width(), dimensions.height());
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(info)) return {};
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    if (stopped()) return {};
    if (codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes()) != SkCodec::kSuccess) return {};
    if (stopped()) return {};
    int width = std::max(1, static_cast<int>(std::round(original.width() * scale)));
    int height = std::max(1, static_cast<int>(std::round(original.height() * scale)));
    const auto origin = codec->getOrigin();
    const bool swap = SkEncodedOriginSwapsWidthHeight(origin);
    if (!result.tryAllocN32Pixels(swap ? height : width, swap ? width : height)) return {};
    result.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas(result);
    canvas.concat(SkEncodedOriginToMatrix(origin, result.width(), result.height()));
    canvas.drawImageRect(bitmap.asImage(), SkRect::MakeWH(width, height),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr);
  }
  if (stopped()) return {};
  result.setImmutable();
  auto image = gfx::ImageSkia::CreateFromBitmap(result, 1.f);
  image.MakeThreadSafe();
  return image;
}
}
