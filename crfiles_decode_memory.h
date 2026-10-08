// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef CRFILES_CRFILES_DECODE_MEMORY_H_
#define CRFILES_CRFILES_DECODE_MEMORY_H_
#include <atomic>
#include <cstddef>
#include <memory>
namespace crfiles {
// Worker-only admission for transient buffers, not retained image caches.
// Encoded and pixel pools are independent: holding encoded bytes while waiting
// for pixels cannot exhaust the pool needed by another decoder to finish.
class DecodeMemory {
 public:
  enum class Kind { kPixels, kEncoded };
  DecodeMemory(size_t bytes, const std::shared_ptr<std::atomic_bool>& canceled,
               Kind kind = Kind::kPixels);
  ~DecodeMemory();
  DecodeMemory(const DecodeMemory&) = delete;
  DecodeMemory& operator=(const DecodeMemory&) = delete;
  bool acquired() const { return bytes_ != 0; }
 private:
  size_t bytes_ = 0;
  Kind kind_;
};
}  // namespace crfiles
#endif
