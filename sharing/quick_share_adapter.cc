// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crFiles/sharing/quick_share_adapter.h"
#include <algorithm>
#include <utility>
#include "base/strings/string_number_conversions.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/timer/timer.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_history.h"
#include "crFiles/sharing/nearby_sharing.h"
#include "crFiles/grit/crfiles_strings.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
#include "crQuickShare/ble_wakeup.h"
#endif
#include "crQuickShare/bluetooth_receiver.h"
#include "crQuickShare/ble_discovery.h"
#include "crQuickShare/bootstrap_probe.h"
#include "crQuickShare/lan_discovery.h"
#include "device/bluetooth/bluetooth_adapter.h"
#endif
#if BUILDFLAG(IS_MAC)
#include "crQuickShare/corewlan_client_mac.h"
#include "crQuickShare/mac_fast_init_experiment.h"
#include "crQuickShare/mac_bluetooth_reconnect.h"
#endif
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "crQuickShare/quick_share_service.h"
namespace crfiles {
namespace {
class QuickShareJournal final : public crquickshare::TransferJournal {
 public:
  QuickShareJournal(bool incoming, const std::string& peer,
                    const base::FilePath& local, const std::string& remote)
      : log_(incoming ? "receive" : "send", incoming ? "remote" : "menu",
             incoming ? "Quick Share/" + peer + "/" + remote : local.AsUTF8Unsafe(),
             incoming ? local.AsUTF8Unsafe() : "Quick Share/" + peer + "/" + remote) {}
  bool valid() const { return log_.valid(); }
  void Finish(bool success, bool cancelled, const base::FilePath& saved_path) override {
    if (!saved_path.empty()) log_.SetDestination(saved_path.AsUTF8Unsafe());
    if (success) log_.Succeed();
    else if (cancelled) log_.Cancel();
  }
 private:
  FileOperationLog log_;
};
std::unique_ptr<crquickshare::TransferJournal> CreateQuickShareJournal(
    bool incoming, const std::string& peer, const base::FilePath& local,
    const std::string& remote) {
  auto journal = std::make_unique<QuickShareJournal>(incoming, peer, local, remote);
  if (!journal->valid()) return nullptr;
  return journal;
}
std::string PeerKey(const crquickshare::Peer& peer) {
  // Match discovery's SameBluetoothRoute: rotating advertisement metadata
  // must not invalidate a displayed send target. This is a routing key only;
  // the connection still performs its normal handshake and confirmation.
  if (peer.ble_only && !peer.classic_bluetooth_address.empty())
    return "ble-route:" + base::HexEncode(peer.id) + ":" + base::HexEncode(peer.classic_bluetooth_address);
  if (peer.ble_only) return "ble:" + base::HexEncode(peer.id) + ":" + base::HexEncode(peer.endpoint_info);
  return peer.id + "@" + peer.address + ":" + base::NumberToString(peer.port);
}
class QuickShareAdapter final : public ShareService {
 public:
  QuickShareAdapter() {
    UpdateWakeup();
    timer_.Start(FROM_HERE, base::Milliseconds(250), this, &QuickShareAdapter::UpdateWakeup);
  }
  ~QuickShareAdapter() override {
    timer_.Stop();
#if BUILDFLAG(IS_MAC)
    crquickshare::CancelDeferredCoreWlanAccessRequest();
#endif
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    receiver_.Update(nullptr);
    sender_.Cancel(); discovery_.Update(false);
    wakeup_.Update(false);
#endif
  }
  bool IsQuickShare() const override { return true; }
  void SetDiscoveryActive(bool active) override {
    discovery_active_ = active;
    if (active && service_) service_->RefreshDiscovery();
    UpdateWakeup();
#if BUILDFLAG(IS_MAC)
    if (active && service_) crquickshare::RequestCoreWlanAccess();
    if (!active) crquickshare::CancelDeferredCoreWlanAccessRequest();
#endif
  }
  Snapshot GetSnapshot() const override {
    Snapshot result;
    result.device_name = preferences_.name;
    result.receive_directory = ReceiveDirectory();
    if (!service_) {
      result.status_message_id = IDS_CRFILES_QUICK_SHARE_OFF;
      return result;
    }
    const auto source = service_->GetSnapshot();
    result.peer_device_type = source.peer_device_type;
    result.diagnostic = source.diagnostic;
    if (source.network_cleanup_failed) result.warning_message_id = IDS_CRFILES_QS_NETWORK_CLEANUP_FAILED;
    result.enabled = preferences_.enabled;
    result.receiving = source.incoming;
    result.batch_directory = source.received_directory;
    result.discovery_ready = source.available;
    result.busy = source.busy;
    result.session = source.session ? session_base_ + source.session : 0;
    if (result.session && observed_session_ != result.session) {
      observed_session_ = result.session;
      started_at_ = base::TimeTicks::Now(); finished_at_ = {};
    }
    result.peer_name = source.peer_name;
    result.code = source.code;
    result.files = source.incoming ? source.files : source.files.empty() ? files_ : source.files;
    result.current_file = source.current_file;
    result.text_count = source.text_count;
    if (source.text_count) result.current_file = l10n_util::GetStringUTF8(IDS_CRFILES_QS_TEXT_CONTENT);
    result.current_file_index = source.current_file_index;
    result.current_file_transferred = source.current_file_transferred;
    result.current_file_total = source.current_file_total;
    result.completed_files = source.completed_files;
    result.bytes_per_second = source.bytes_per_second;
    result.remaining_seconds = source.remaining_seconds;
    result.transferred = source.transferred;
    result.total = source.total;
    result.started_at = started_at_;
    if (!source.busy && source.session && finished_at_.is_null()) finished_at_ = base::TimeTicks::Now();
    result.finished_at = finished_at_;
    using Phase = crquickshare::Phase;
    switch (source.phase) {
      case Phase::kIdle:
        result.stage = Stage::kReady;
        result.status_message_id = IDS_CRFILES_QUICK_SHARE_HINT;
        break;
      case Phase::kConnecting: result.stage = Stage::kConnecting; break;
      case Phase::kConfirm: result.stage = Stage::kPairing; break;
      case Phase::kRestoringNetwork:
        result.stage = Stage::kWaiting;
        result.status_message_id = IDS_CRFILES_QS_RESTORING_NETWORK;
        break;
      case Phase::kWaiting: result.stage = Stage::kWaiting; break;
      case Phase::kTransferring: result.stage = Stage::kTransferring; break;
      case Phase::kComplete:
        result.stage = Stage::kComplete;
        if (source.text_count && copied_text_session_ == result.session)
          result.status_message_id = IDS_CRFILES_QS_TEXT_COPIED;
        break;
      case Phase::kSent:
        result.stage = Stage::kSent;
        result.status_message_id = IDS_CRFILES_QUICK_SHARE_UNCONFIRMED;
        break;
      case Phase::kCancelled: result.stage = Stage::kCanceled; break;
      case Phase::kRejected:
        result.stage = Stage::kError;
        result.status_message_id = IDS_CRFILES_QUICK_SHARE_REJECTED;
        break;
      case Phase::kTimedOut:
        result.stage = Stage::kError;
        result.status_message_id = IDS_CRFILES_QUICK_SHARE_TIMEOUT;
        break;
      case Phase::kError:
        result.stage = Stage::kError;
        switch (source.failure) {
          case crquickshare::Failure::kConnection: result.status_message_id = IDS_CRFILES_QS_ERROR_CONNECTION; break;
          case crquickshare::Failure::kNetwork: result.status_message_id = IDS_CRFILES_QS_ERROR_NETWORK; break;
          case crquickshare::Failure::kStorage: result.status_message_id = IDS_CRFILES_QS_ERROR_STORAGE; break;
          case crquickshare::Failure::kSourceFile: result.status_message_id = IDS_CRFILES_QS_ERROR_SOURCE; break;
          case crquickshare::Failure::kUnsupported: result.status_message_id = IDS_CRFILES_QS_ERROR_UNSUPPORTED; break;
          case crquickshare::Failure::kProtocol: result.status_message_id = IDS_CRFILES_QS_ERROR_PROTOCOL; break;
          case crquickshare::Failure::kHotspotUnavailable: result.status_message_id = IDS_CRFILES_QS_HOTSPOT_REQUIRED; break;
          case crquickshare::Failure::kNone: break;
        }
        break;
    }
#if BUILDFLAG(IS_MAC)
    result.can_reconnect_bluetooth = retry_peer_ && !retry_paths_.empty() &&
        retry_session_ == result.session && !source.busy && !source.incoming &&
        source.failure == crquickshare::Failure::kConnection &&
        (source.phase == Phase::kError || source.phase == Phase::kTimedOut) &&
        preferences_.enabled && GetQuickSharePreferences() == preferences_ &&
        !crquickshare::MacBluetoothReconnectInProgress(retry_peer_->classic_bluetooth_address) &&
        sender_.CanRetryWithBluetoothReconnect();
    if (source.busy && source.phase == Phase::kConnecting && sender_.reconnecting_bluetooth())
      result.status_message_id = IDS_CRFILES_QS_RECONNECTING_BLUETOOTH;
#endif
    if (!source.available && !source.busy && !source.session) result.status_message_id = IDS_CRFILES_NEARBY_DISCOVERY_UNAVAILABLE;
    if (!source.available) result.discovery_message_id = IDS_CRFILES_NEARBY_DISCOVERY_UNAVAILABLE;
    if (!result.busy && !result.receiving && !result.finished_at.is_null() &&
        (result.stage == Stage::kComplete || result.stage == Stage::kSent))
      result.discovery_resume_at = result.finished_at + base::Seconds(3);
    const bool discovery_paused = base::TimeTicks::Now() < result.discovery_resume_at;
    auto peers = source.peers;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    if (source.available && discovery_active_ && !source.busy && !discovery_paused) {
      result.discovery_message_id = discovery_.state() == crquickshare::BleDiscovery::State::kUnavailable ?
          IDS_CRFILES_QUICK_SHARE_BLE_UNAVAILABLE : IDS_CRFILES_QUICK_SHARE_BLE_SCANNING;
    }
    peers = discovery_.MergeWithLan(source.peers);
#else
    if (source.available && discovery_active_ && !source.busy && !discovery_paused)
      result.discovery_message_id = IDS_CRFILES_QUICK_SHARE_HINT;
#endif
    for (const auto& peer : peers) {
      bool bluetooth_connectable = false;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      bluetooth_connectable = peer.ble_only && !crquickshare::EndpointName(peer.endpoint_info).empty() && !peer.classic_bluetooth_address.empty() &&
          !discovery_.bootstrap_active();
#endif
      if (peer.ble_only && !bluetooth_connectable) {
        if (source.available && discovery_active_ && !source.busy && !discovery_paused)
          result.discovery_message_id = IDS_CRFILES_QUICK_SHARE_BLE_HINT;
        continue;
      }
      if (peer.name.empty()) continue;
      Peer item;
      item.id = peer.id;
      item.name = peer.name.empty() ? l10n_util::GetStringUTF8(IDS_CRFILES_QUICK_SHARE_BLE_DEVICE) : peer.name;
      item.ble_only = peer.ble_only;
      item.bluetooth_connectable = bluetooth_connectable;
      item.device_type = peer.endpoint_info.empty() ? 0 : (static_cast<uint8_t>(peer.endpoint_info[0]) >> 1) & 7;
      item.bluetooth_address = peer.bluetooth_address;
      item.address = PeerKey(peer);
      result.peers.push_back(std::move(item));
    }
    return result;
  }
  bool Send(std::string address, std::vector<base::FilePath> paths,
            std::map<base::FilePath, gfx::ImageSkia>,
            std::optional<ConnectPeer> phone, std::string target_id) override {
    const auto reject = [](const char* reason) {
      LOG(WARNING) << "crQuickShare: Send rejected before session; component=adapter; reason=" << reason;
      return false;
    };
    if (!service_) return reject("service_missing");
    if (!GetQuickSharePreferences().enabled) return reject("sharing_disabled");
    if (phone || !target_id.empty()) return reject("unsupported_target");
    if (paths.empty()) return reject("empty_selection");
    const auto source = service_->GetSnapshot();
    auto peers = source.peers;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    peers = discovery_.MergeWithLan(std::move(peers));
#endif
    const auto peer = std::ranges::find_if(peers, [&](const auto& item) {
      return !item.name.empty() && PeerKey(item) == address;
    });
    if (peer == peers.end()) return reject("peer_not_found_or_unnamed");
    std::vector<std::string> names;
    for (const auto& path : paths) names.push_back(path.BaseName().AsUTF8Unsafe());
    if (peer->ble_only) {
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      if (peer->classic_bluetooth_address.empty()) return reject("bluetooth_route_missing");
      if (crquickshare::EndpointName(peer->endpoint_info).empty()) return reject("public_name_missing");
      if (discovery_.bootstrap_active()) return reject("discovery_bootstrap_busy");
      const auto selected = *peer;
#if BUILDFLAG(IS_MAC)
      const auto retry_paths = paths;
#endif
      auto adapter = discovery_.adapter();
      discovery_.Update(false, true);
      wakeup_.Update(false);
      if (!sender_.Send(std::move(adapter), selected, *service_, std::move(paths), hotspot_,
                        [this](base::OnceClosure ready) { discovery_.WhenConnectionReady(std::move(ready)); })) {
        UpdateWakeup(); return reject("bluetooth_sender_rejected");
      }
#if BUILDFLAG(IS_MAC)
      retry_peer_ = selected;
      retry_paths_ = retry_paths;
      retry_session_ = session_base_ + service_->GetSnapshot().session;
#endif
#else
      return reject("bluetooth_unsupported");
#endif
    } else {
      if (!service_->Send(*peer, std::move(paths))) return reject("lan_service_rejected");
#if BUILDFLAG(IS_MAC)
      retry_peer_.reset(); retry_paths_.clear(); retry_session_ = 0;
#endif
    }
    files_ = std::move(names);
    started_at_ = base::TimeTicks::Now();
    finished_at_ = base::TimeTicks();
    UpdateWakeup();
    return true;
  }
#if BUILDFLAG(IS_MAC)
  bool RetryWithBluetoothReconnect(uint64_t session) override {
    // Recheck the displayed session before consuming the retained device and
    // immutable selection. Discovery may have withdrawn it after the failure.
    const auto state = GetSnapshot();
    if (!state.can_reconnect_bluetooth || state.session != session) return false;
    auto peer = *retry_peer_;
    auto paths = retry_paths_;
    auto adapter = discovery_.adapter();
    discovery_.Update(false, true);
    wakeup_.Update(false);
    if (!sender_.Send(std::move(adapter), peer, *service_, std::move(paths), hotspot_,
                      [this](base::OnceClosure ready) { discovery_.WhenConnectionReady(std::move(ready)); }, true)) {
      UpdateWakeup();
      return false;
    }
    const auto next = service_->GetSnapshot();
    retry_session_ = session_base_ + next.session;
    started_at_ = base::TimeTicks::Now(); finished_at_ = {};
    LOG(INFO) << "crQuickShare: macOS Bluetooth recovery authorized; previous_session=" << session
              << "; session=" << retry_session_ << "; files=" << retry_paths_.size();
    UpdateWakeup();
    return true;
  }
#endif
  void Decide(uint64_t session, Stage prompt, bool accept, bool trust,
              uint64_t conflict_id) override {
    if (service_ && session > session_base_ && prompt == Stage::kPairing && !trust && !conflict_id)
      service_->Confirm(session - session_base_, accept);
  }
  void CancelSession(uint64_t session) override {
    if (service_ && session > session_base_) service_->Cancel(session - session_base_);
  }
  std::vector<TrustedDevice> GetTrustedDevices() const override { return {}; }
  void ForgetTrustedDevice(std::string) override {}
 private:
  base::FilePath ReceiveDirectory() const {
    return preferences_.directory.empty() ? NearbySharing::DefaultReceiveDirectory() :
        base::FilePath::FromUTF8Unsafe(preferences_.directory);
  }
  void CopyReceivedTexts() {
    // Runs on the UI sequence even when the sharing dialog is closed. Consume
    // the core's completion queue before a preferences change replaces it.
    if (service_) for (auto& received : service_->TakeReceivedTexts()) {
      {
        ui::ScopedClipboardWriter clipboard(ui::ClipboardBuffer::kCopyPaste);
        clipboard.WriteText(base::UTF8ToUTF16(received.text));
      }
      copied_text_session_ = session_base_ + received.session;
    }
  }
  void UpdateWakeup() {
    CopyReceivedTexts();
    const auto desired = GetQuickSharePreferences();
    if (!configured_ || preferences_ != desired) {
      // Do not replace a backend while its worker owns a transfer. The core
      // atomically closes incoming admission before the idle backend is reset.
      if (service_ && !service_->StopReceivingIfIdle()) {
#if BUILDFLAG(IS_MAC)
        wakeup_.Update(false);
#endif
        return;
      }
      CopyReceivedTexts();
      if (service_) session_base_ += service_->GetSnapshot().session + 1;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
      receiver_.Update(nullptr);
      sender_.Cancel();
#endif
      service_.reset();
      preferences_ = desired; configured_ = true;
      files_.clear(); started_at_ = {}; finished_at_ = {};
#if BUILDFLAG(IS_MAC)
      retry_peer_.reset(); retry_paths_.clear(); retry_session_ = 0;
#endif
      if (preferences_.enabled) service_ = std::make_unique<crquickshare::QuickShareService>(preferences_.name, ReceiveDirectory(), true, CreateQuickShareJournal);
    }
    const auto state = service_ ? service_->GetSnapshot() : crquickshare::Snapshot();
    if (service_ && state.available && !state.receiving) service_->SetReceiving(true, true);
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
    const auto snapshot = GetSnapshot();
    const bool discovery_paused = base::TimeTicks::Now() < snapshot.discovery_resume_at;
    const bool active = service_ && discovery_active_ && !state.busy && !discovery_paused;
    if (!state.busy) {
      sender_.Cancel();
#if BUILDFLAG(IS_LINUX)
      const auto* command = base::CommandLine::ForCurrentProcess();
      const auto interface = command->HasSwitch("quick-share-upgrade-interface") ?
          command->GetSwitchValueASCII("quick-share-upgrade-interface") : preferences_.upgrade_interface;
      const bool direct_go = command->HasSwitch("quick-share-direct-go") || preferences_.direct_enabled;
      // Direct opt-in uses an independent GO/client interface and preserves STA.
      // Never fall back to joining the phone's hotspot on this same radio.
      hotspot_ = direct_go ? crquickshare::HotspotConnector() : crquickshare::MakeLinuxHotspotConnector(interface);
      if (service_) service_->SetPreferHotspotUpgrade(
          !direct_go && command->HasSwitch("quick-share-prefer-hotspot"));
      if (service_) service_->SetWifiDirectClientConnector(direct_go ?
          crquickshare::MakeLinuxWifiDirectClientConnector(interface) : crquickshare::WifiDirectClientConnector());
      if (service_) service_->SetWifiDirectGoFactory(direct_go ?
          crquickshare::MakeLinuxWifiDirectGoFactory(interface) : crquickshare::WifiDirectGoFactory(),
          command->HasSwitch("quick-share-prefer-direct-go"));
#elif BUILDFLAG(IS_WIN)
      const auto* command = base::CommandLine::ForCurrentProcess();
      const auto requested_interface = command->HasSwitch("quick-share-upgrade-interface") ?
          base::WideToUTF8(command->GetSwitchValueNative("quick-share-upgrade-interface")) : preferences_.upgrade_interface;
      hotspot_ = crquickshare::MakeWindowsHotspotConnector(requested_interface);
      if (service_) service_->SetPreferHotspotUpgrade(command->HasSwitch("quick-share-prefer-hotspot"));
#elif BUILDFLAG(IS_MAC)
      const auto* command = base::CommandLine::ForCurrentProcess();
      const auto interface = command->HasSwitch("quick-share-upgrade-interface") ?
          command->GetSwitchValueASCII("quick-share-upgrade-interface") : preferences_.upgrade_interface;
      hotspot_ = crquickshare::MakeMacHotspotConnector(interface);
      const bool direct_client = !command->HasSwitch("disable-quick-share-mac-direct-client");
      const bool prefer_hotspot = command->HasSwitch("quick-share-prefer-hotspot");
      if (service_) {
        service_->SetPreferHotspotUpgrade(prefer_hotspot);
        service_->SetWifiDirectClientConnector(direct_client ?
            crquickshare::MakeMacWifiDirectClientConnector(interface) : crquickshare::WifiDirectClientConnector(),
            !prefer_hotspot && command->HasSwitch("quick-share-prefer-direct-client"));
      }
#endif
    }
    discovery_.Update(active, service_ && discovery_active_ && (state.busy || discovery_paused));
#if BUILDFLAG(IS_MAC)
    // Share the local-name advertiser: foreground discovery uses FE2C, idle
    // reception uses FEF3. Keep the receiver's RFCOMM listener and GATT service.
    const bool bluetooth_receiver_enabled = !base::CommandLine::ForCurrentProcess()->HasSwitch(
        "disable-quick-share-mac-bluetooth-receiver");
    const bool wants_fast_init = active && crquickshare::IsMacFastInitExperimentEnabled();
    // Stop the previous advertiser before allowing the other one to start.
    if (!wants_fast_init) wakeup_.Update(false);
    const bool receiver_advertising_stopped =
        receiver_.SetAdvertisingPausedForDiscovery(wants_fast_init);
    receiver_.Update(bluetooth_receiver_enabled ? service_.get() : nullptr, hotspot_);
    if (wants_fast_init) wakeup_.Update(receiver_advertising_stopped);
#else
    wakeup_.Update(active);
    receiver_.Update(service_.get(), hotspot_);
#endif
#endif
  }
  QuickSharePreferences preferences_;
  bool configured_ = false;
  uint64_t session_base_ = 0;
  uint64_t copied_text_session_ = 0;
  mutable uint64_t observed_session_ = 0;
  std::unique_ptr<crquickshare::QuickShareService> service_;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_WIN)
  crquickshare::BleWakeup wakeup_;
#elif BUILDFLAG(IS_MAC)
  crquickshare::MacFastInitExperiment wakeup_;
#endif
  crquickshare::BleDiscovery discovery_;
  crquickshare::BootstrapProbe sender_{false};
  crquickshare::BluetoothReceiver receiver_;
  crquickshare::HotspotConnector hotspot_;
#endif
  base::RepeatingTimer timer_;
  bool discovery_active_ = false;
#if BUILDFLAG(IS_MAC)
  std::optional<crquickshare::Peer> retry_peer_;
  std::vector<base::FilePath> retry_paths_;
  uint64_t retry_session_ = 0;
#endif
  std::vector<std::string> files_;
  mutable base::TimeTicks started_at_;
  mutable base::TimeTicks finished_at_;
};
}  // namespace
std::shared_ptr<ShareService> CreateQuickShareService() {
  return std::make_shared<QuickShareAdapter>();
}
}  // namespace crfiles
