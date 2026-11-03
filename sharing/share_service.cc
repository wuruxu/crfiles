// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crFiles/sharing/share_service.h"

namespace crfiles {
bool ShareService::SetSyncReceiveDirectory(uint64_t, const base::FilePath&) { return false; }
bool ShareService::RetryWithBluetoothReconnect(uint64_t) { return false; }
bool ShareService::IsQuickShare() const { return false; }
}  // namespace crfiles
