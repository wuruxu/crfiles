// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRFILES_SHARING_SHARE_SERVICE_H_
#define CRFILES_SHARING_SHARE_SERVICE_H_
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "base/files/file_path.h"
#include "base/time/time.h"
#include "crFiles/sharing/connect_discovery.h"
#include "ui/gfx/image/image_skia.h"
namespace crfiles {
// UI contract shared by the desktop and Android Quick Share transports.
class ShareService {
 public:
  static constexpr int kAvatarCount = 9;
  // Blocking interface enumeration; call on a worker. Matches Connect's policy.
  enum class InterfaceType { kOther, kWifi, kEthernet };
  struct LocalAddress {
    std::string address;
    InterfaceType type = InterfaceType::kOther;
  };
  enum class Stage {
    kOff, kStarting, kReady, kConnecting, kPairing, kWaiting,
    kReceivingOffer, kTransferring, kComplete, kCanceled, kError,
    kReceivingConflict, kSent,
  };
  struct SharedTarget {
    std::string id;
    std::string name;
    base::FilePath directory;  // Local only; never sent to another device.
    int64_t tag_id = 0;  // Local-only association for successful receives.
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
    std::string bluetooth_address = {};  // Display only, not a connection target.
    bool ble_only = false;
    bool bluetooth_connectable = false;  // BLE discovery with an implemented send route.
    int quick_share_avatar = -1;
    uint8_t quick_share_application = 0;
    uint8_t device_type = 0;  // Quick Share's advertised type; 0 means unknown.
    std::vector<SharedTarget> targets = {};
    // Requires both sync-v1 and its permission-negotiation extension.
    bool sync_v1 = false;
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
    bool discovery_ready = false;
    bool busy = false;
    bool receiving = false;
    bool syncing = false;
    bool sync_background = false;
    bool trust_error = false;
    // macOS Quick Share only: explicit recovery for a failed Bluetooth setup.
    bool can_reconnect_bluetooth = false;
    Stage stage = Stage::kOff;
    int status_message_id = 0;
    int warning_message_id = 0;
    std::string diagnostic;
    int discovery_message_id = 0;
    uint64_t session = 0;
    uint64_t conflict_id = 0;
    base::TimeTicks started_at;
    base::TimeTicks finished_at;
    // Quick Share outgoing results remain visible until discovery resumes.
    base::TimeTicks discovery_resume_at;
    std::string code;
    std::string device_name;
    int avatar = 0;
    std::string receive_target;
    std::string receive_target_name;
    std::string peer_name;
    int peer_quick_share_avatar = -1;
    uint8_t peer_quick_share_application = 0;
    uint8_t peer_device_type = 0;
    int peer_avatar = 0;
    bool peer_trusted = false;
    std::string peer_custom_avatar;
    size_t skipped_files = 0;
    std::vector<std::string> files;
    uint32_t text_count = 0;  // Quick Share text/URL attachments destined for the clipboard.
    std::string current_file;  // Last transfer entry, retained with the result.
    std::map<base::FilePath, gfx::ImageSkia> previews;
    uint64_t current_file_index = 0;  // One-based introduction order; zero before transfer.
    uint64_t current_file_transferred = 0;
    uint64_t current_file_total = 0;
    uint64_t completed_files = 0;  // Payloads finished, not a remote delivery receipt.
    uint64_t bytes_per_second = 0;
    int64_t remaining_seconds = -1;  // Unknown until a speed estimate is available.
    uint64_t transferred = 0;
    uint64_t total = 0;
    base::FilePath receive_directory;
    base::FilePath batch_directory;
    std::vector<Peer> peers;
    // Raw HTTPS discovery records for Connect sync and the combined device UI.
    std::vector<ConnectPeer> connect_peers;
    uint16_t port = 0;
  };

  virtual ~ShareService() = default;
  virtual Snapshot GetSnapshot() const = 0;
  virtual bool Send(std::string address, std::vector<base::FilePath> paths,
                    std::map<base::FilePath, gfx::ImageSkia> previews = {},
                    std::optional<ConnectPeer> phone = std::nullopt,
                    std::string target_id = {}) = 0;
  virtual void Decide(uint64_t session, Stage prompt, bool accept,
                      bool trust = false, uint64_t conflict_id = 0) = 0;
  virtual void CancelSession(uint64_t session) = 0;
  virtual bool RetryWithBluetoothReconnect(uint64_t session);
  virtual std::vector<TrustedDevice> GetTrustedDevices() const = 0;
  virtual void ForgetTrustedDevice(std::string fingerprint) = 0;
  virtual bool IsQuickShare() const;
  // Discovery UI lifetime; closing the dialog never cancels a transfer.
  virtual void SetDiscoveryActive(bool) {}
};
}  // namespace crfiles
#endif
