// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_CRFILES_PROCESS_SINGLETON_H_
#define CRFILES_CRFILES_PROCESS_SINGLETON_H_

#include <memory>

#include "base/functional/callback_forward.h"

namespace crfiles {

class CrFilesProcessSingleton {
 public:
  enum class Result {
    kPrimary,
    kNotifiedExistingInstance,
    kError,
  };

  explicit CrFilesProcessSingleton(base::RepeatingClosure activate_callback);
  CrFilesProcessSingleton(const CrFilesProcessSingleton&) = delete;
  CrFilesProcessSingleton& operator=(const CrFilesProcessSingleton&) = delete;
  ~CrFilesProcessSingleton();

  Result NotifyOtherProcessOrCreate();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace crfiles

#endif  // CRFILES_CRFILES_PROCESS_SINGLETON_H_
