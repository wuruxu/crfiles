// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_SHARING_NEARBY_SHARING_H_
#define CRFILES_SHARING_NEARBY_SHARING_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "crFiles/sharing/connect_discovery.h"
#include "ui/gfx/image/image_skia.h"
#include "crFiles/sharing/share_service.h"
#include "crFiles/sharing/sync_service.h"

namespace crfiles {

// No network activity until Enable(). The worker never calls Views directly.
// Discovery is only an untrusted address hint, never an authorization decision.
class NearbySharing : public ShareService {
 public:
  static std::vector<LocalAddress> GetLocalIPv4Addresses();

  NearbySharing();
  ~NearbySharing() override;
  NearbySharing(const NearbySharing&) = delete;
  NearbySharing& operator=(const NearbySharing&) = delete;

  static base::FilePath DefaultReceiveDirectory();
  static bool IsValidConfiguration(std::string_view name,
                                   const base::FilePath& directory);
  void Enable(std::string device_name, base::FilePath receive_directory,
              int avatar = 0);
  void Disable();
  void SetSharedTargets(std::vector<SharedTarget> targets);
  Snapshot GetSnapshot() const override;
  std::vector<TrustedDevice> GetTrustedDevices() const override;
  void ForgetTrustedDevice(std::string fingerprint) override;
  bool Send(std::string address, std::vector<base::FilePath> paths,
            std::map<base::FilePath, gfx::ImageSkia> previews = {},
            std::optional<ConnectPeer> phone = std::nullopt,
            std::string target_id = {}) override;
  // Decisions are scoped to a prompt and session, so stale buttons cannot
  // approve a subsequent connection or a different consent step. Replacement
  // prompts also require the matching conflict_id for that particular entry.
  void Decide(uint64_t session, Stage prompt, bool accept, bool trust = false,
              uint64_t conflict_id = 0) override;
  void Cancel();
  // Ignore cancellation from a UI displaying a different transfer.
  void CancelSession(uint64_t session) override;

  bool StartSync(const Peer& peer, const base::FilePath& source);
  void CancelSync(const std::string& id);
  std::shared_ptr<SyncService> sync_service() const { return sync_; }

  struct State;
  struct TrustStore;

 private:
  std::shared_ptr<State> state_;
  std::shared_ptr<SyncService> sync_;
  std::shared_ptr<TrustStore> trust_;
  std::unique_ptr<ConnectDiscovery> connect_discovery_;
};

}  // namespace crfiles
#endif  // CRFILES_SHARING_NEARBY_SHARING_H_
