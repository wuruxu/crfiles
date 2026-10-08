// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_decode_memory.h"
#include "base/no_destructor.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
namespace crfiles {
namespace {
struct Budget {
  base::Lock lock;
  base::ConditionVariable available{&lock};
  size_t used = 0;
};
Budget& GetBudget(DecodeMemory::Kind kind) {
  static base::NoDestructor<Budget> pixels;
  static base::NoDestructor<Budget> encoded;
  return kind == DecodeMemory::Kind::kEncoded ? *encoded : *pixels;
}
}  // namespace
DecodeMemory::DecodeMemory(size_t bytes,
    const std::shared_ptr<std::atomic_bool>& canceled, Kind kind) : kind_(kind) {
  const size_t limit = (kind == Kind::kEncoded ? 128 : 320) * 1024 * 1024;
  // Existing HEIC support accepts up to 64 MP, including HDR. Large requests
  // keep that support but run alone in the pixel pool, never alongside another
  // admitted pixel allocation. Codec-private scratch is not a hard RSS bound.
  const size_t maximum = kind == Kind::kEncoded ? limit : 800 * 1024 * 1024;
  if (!bytes || bytes > maximum) return;
  auto& budget = GetBudget(kind);
  base::AutoLock guard(budget.lock);
  while (!canceled || !canceled->load(std::memory_order_relaxed)) {
    if (bytes > limit ? budget.used == 0 : budget.used <= limit && bytes <= limit - budget.used) {
      budget.used += bytes; bytes_ = bytes; return;
    }
    budget.available.TimedWait(base::Milliseconds(50));
  }
}
DecodeMemory::~DecodeMemory() {
  if (!bytes_) return;
  auto& budget = GetBudget(kind_);
  base::AutoLock guard(budget.lock);
  budget.used -= bytes_;
  budget.available.Broadcast();
}
}  // namespace crfiles
