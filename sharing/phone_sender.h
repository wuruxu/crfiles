// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CRFILES_SHARING_PHONE_SENDER_H_
#define CRFILES_SHARING_PHONE_SENDER_H_
#include <vector>
#include "base/functional/callback.h"
#include "crFiles/sharing/nearby_sharing.h"
namespace crfiles {
struct PhoneSendProgress {
  NearbySharing::Stage stage = NearbySharing::Stage::kConnecting;
  uint64_t transferred = 0;
  uint64_t total = 0;
  int message_id = 0;
  bool verified = false;
};
// Blocking transport adapter for the Nearby worker. Never calls Views. The
// cancellation callback also covers shutdown and obsolete session generations.
// Both functions require MayBlock and WithBaseSyncPrimitives task traits:
// ConnectClient destruction joins its dedicated IO thread.
// Read-only verification on a worker; unpaired devices cause no HTTPS request.
void VerifyDiscoveredPhone(const ConnectPeer& peer, base::RepeatingCallback<bool()> canceled);
void SendFilesToPhone(const ConnectPeer& peer, const std::vector<base::FilePath>& paths,
                      base::RepeatingCallback<bool()> canceled,
                      base::RepeatingCallback<void(PhoneSendProgress)> report);
}
#endif  // CRFILES_SHARING_PHONE_SENDER_H_
