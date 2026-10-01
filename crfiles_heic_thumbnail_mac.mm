// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_heic_thumbnail.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <cstdint>
#include "base/apple/scoped_cftyperef.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/utils/mac/SkCGUtils.h"

namespace crfiles {
namespace {
bool Stopped(const std::shared_ptr<std::atomic_bool>& canceled) {
  return canceled && canceled->load(std::memory_order_relaxed);
}
bool ReadDimension(CFDictionaryRef properties, CFStringRef key, int* value) {
  const void* const number = CFDictionaryGetValue(properties, key);
  return number && CFGetTypeID(number) == CFNumberGetTypeID() &&
      CFNumberGetValue(static_cast<CFNumberRef>(number), kCFNumberIntType, value);
}
}  // namespace

SkBitmap DecodeHeicThumbnail(const std::string& encoded, int maximum_edge,
    const std::shared_ptr<std::atomic_bool>& canceled) {
  if (Stopped(canceled) || encoded.empty() || encoded.size() > 64 * 1024 * 1024 ||
      maximum_edge <= 0 || maximum_edge > 2048) return {};
  // ImageIO may allocate codec scratch for the primary image even when its
  // output is small. Serialize HEIC decodes across thumbnail and hover workers.
  static base::NoDestructor<base::Lock> lock;
  base::AutoLock guard(*lock);
  if (Stopped(canceled)) return {};
  base::apple::ScopedCFTypeRef<CFDataRef> data(CFDataCreateWithBytesNoCopy(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(encoded.data()),
      static_cast<CFIndex>(encoded.size()), kCFAllocatorNull));
  if (!data) return {};
  const void* source_keys[] = {kCGImageSourceShouldCache};
  const void* source_values[] = {kCFBooleanFalse};
  base::apple::ScopedCFTypeRef<CFDictionaryRef> source_options(CFDictionaryCreate(
      kCFAllocatorDefault, source_keys, source_values, 1,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  base::apple::ScopedCFTypeRef<CGImageSourceRef> source(
      CGImageSourceCreateWithData(data.get(), source_options.get()));
  if (!source || Stopped(canceled)) return {};
  const size_t index = CGImageSourceGetPrimaryImageIndex(source.get());
  if (index >= CGImageSourceGetCount(source.get())) return {};
  base::apple::ScopedCFTypeRef<CFDictionaryRef> properties(
      CGImageSourceCopyPropertiesAtIndex(source.get(), index, nullptr));
  int width = 0, height = 0;
  if (!properties ||
      !ReadDimension(properties.get(), kCGImagePropertyPixelWidth, &width) ||
      !ReadDimension(properties.get(), kCGImagePropertyPixelHeight, &height) ||
      width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
      static_cast<int64_t>(width) * height > 64 * 1000 * 1000) return {};
  base::apple::ScopedCFTypeRef<CFNumberRef> edge(
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maximum_edge));
  if (!edge) return {};
  const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways,
      kCGImageSourceCreateThumbnailWithTransform, kCGImageSourceThumbnailMaxPixelSize,
      kCGImageSourceShouldCacheImmediately, kCGImageSourceShouldAllowFloat};
  const void* values[] = {kCFBooleanTrue, kCFBooleanTrue, edge.get(),
      kCFBooleanTrue, kCFBooleanFalse};
  base::apple::ScopedCFTypeRef<CFDictionaryRef> options(CFDictionaryCreate(
      kCFAllocatorDefault, keys, values, 5,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  if (!options || Stopped(canceled)) return {};
  base::apple::ScopedCFTypeRef<CGImageRef> image(
      CGImageSourceCreateThumbnailAtIndex(source.get(), index, options.get()));
  if (!image || Stopped(canceled)) return {};
  const size_t output_width = CGImageGetWidth(image.get());
  const size_t output_height = CGImageGetHeight(image.get());
  if (!output_width || !output_height ||
      output_width > static_cast<size_t>(maximum_edge) ||
      output_height > static_cast<size_t>(maximum_edge)) return {};
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(
      static_cast<int>(output_width), static_cast<int>(output_height),
      SkColorSpace::MakeSRGB()))) return {};
  // ImageIO has already applied orientation. Skia's Quartz conversion handles
  // row order, N32 channel layout, alpha and the destination sRGB color space.
  if (!SkCopyPixelsFromCGImage(bitmap.info(), bitmap.rowBytes(),
      bitmap.getPixels(), image.get())) return {};
  return Stopped(canceled) ? SkBitmap() : bitmap;
}
}  // namespace crfiles
