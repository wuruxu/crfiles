// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRFILES_SHARING_QUICK_SHARE_ADAPTER_H_
#define CRFILES_SHARING_QUICK_SHARE_ADAPTER_H_
#include <memory>
#include "crFiles/sharing/share_service.h"
namespace crfiles {
// Desktop Linux only. Owns an independent Quick Share service for sending and receiving.
std::shared_ptr<ShareService> CreateQuickShareService();
}  // namespace crfiles
#endif
