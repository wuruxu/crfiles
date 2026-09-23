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

namespace crfiles {

// No network activity until Enable(). The worker never calls Views directly.
// Discovery is only an untrusted address hint, never an authorization decision.
class NearbySharing {
 public:
  static constexpr int kAvatarCount = 9;
  // Blocking interface enumeration; call on a worker. Matches Connect's policy.
  enum class InterfaceType { kOther, kWifi, kEthernet };
  struct LocalAddress {
    std::string address;
    InterfaceType type = InterfaceType::kOther;
  };
  static std::vector<LocalAddress> GetLocalIPv4Addresses();
  enum class Stage {
    kOff, kStarting, kReady, kConnecting, kPairing, kWaiting,
    kReceivingOffer, kTransferring, kComplete, kCanceled, kError,
  };
  struct SharedTarget {
    std::string id;
    std::string name;
    base::FilePath directory;  // Local only; never sent to another device.
    bool operator==(const SharedTarget&) const = default;
  };
  struct Peer {
    std::string id;
    std::string name;
    std::string address;
    int avatar = 0;
    uint16_t identity_port = 0;
    std::string verified_fingerprint = {};
    base::TimeTicks identity_expires;
    bool trusted = false;
    // Present only on display snapshots. Route explicitly to HTTPS; never use
    // this address with the desktop framed TLS protocol.
    std::optional<ConnectPeer> phone = {};
    bool folder_transfer = false;
    std::vector<SharedTarget> targets = {};
    bool operator==(const Peer&) const = default;
  };
  struct TrustedDevice {
    std::string fingerprint;
    std::string name;
    int avatar = 0;
    base::Time trusted_at;
    base::Time last_active;
    uint64_t sent_bytes = 0;
    uint64_t received_bytes = 0;
    bool operator==(const TrustedDevice&) const = default;
  };
  struct Snapshot {
    bool enabled = false;
    bool busy = false;
    bool receiving = false;
    bool trust_error = false;
    Stage stage = Stage::kOff;
    int status_message_id = 0;
    uint64_t session = 0;
    std::string code;
    std::string device_name;
    int avatar = 0;
    std::string receive_target;
    std::string receive_target_name;
    std::string peer_name;
    int peer_avatar = 0;
    bool peer_trusted = false;
    std::string peer_custom_avatar;
    size_t skipped_files = 0;
    std::vector<std::string> files;
    std::map<base::FilePath, gfx::ImageSkia> previews;
    uint64_t transferred = 0;
    uint64_t total = 0;
    base::FilePath receive_directory;
    base::FilePath batch_directory;
    std::vector<Peer> peers;
    // Raw HTTPS discovery records for Connect sync and the combined device UI.
    std::vector<ConnectPeer> connect_peers;
    uint16_t port = 0;
  };

  NearbySharing();
  ~NearbySharing();
  NearbySharing(const NearbySharing&) = delete;
  NearbySharing& operator=(const NearbySharing&) = delete;

  static base::FilePath DefaultReceiveDirectory();
  static bool IsValidConfiguration(std::string_view name,
                                   const base::FilePath& directory);
  void Enable(std::string device_name, base::FilePath receive_directory,
              int avatar = 0);
  void Disable();
  void SetSharedTargets(std::vector<SharedTarget> targets);
  Snapshot GetSnapshot() const;
  std::vector<TrustedDevice> GetTrustedDevices() const;
  void ForgetTrustedDevice(std::string fingerprint);
  bool Send(std::string address, std::vector<base::FilePath> paths,
            std::map<base::FilePath, gfx::ImageSkia> previews = {},
            std::optional<ConnectPeer> phone = std::nullopt,
            std::string target_id = {});
  // Decisions are scoped to a prompt and session, so stale buttons cannot
  // approve a subsequent connection or a different consent step.
  void Decide(uint64_t session, Stage prompt, bool accept, bool trust = false);
  void Cancel();
  // Ignore cancellation from a UI displaying a different transfer.
  void CancelSession(uint64_t session);

  struct State;
  struct TrustStore;

 private:
  std::shared_ptr<State> state_;
  std::shared_ptr<TrustStore> trust_;
  std::unique_ptr<ConnectDiscovery> connect_discovery_;
};

}  // namespace crfiles
#endif  // CRFILES_SHARING_NEARBY_SHARING_H_
