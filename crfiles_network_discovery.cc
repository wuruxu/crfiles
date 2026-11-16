// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_network_discovery.h"
#include "base/strings/string_number_conversions.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "net/net_buildflags.h"

#if BUILDFLAG(ENABLE_MDNS)
#include "net/base/address_list.h"
#include "net/base/net_errors.h"
#include "net/base/network_interfaces.h"
#include "net/dns/mdns_client.h"
#include "net/dns/public/dns_protocol.h"
#include "net/dns/public/util.h"
#include "net/dns/record_rdata.h"
#include "net/log/net_log_source.h"
#include "net/socket/datagram_server_socket.h"
#include "net/socket/socket_performance_watcher.h"
#include "net/socket/tcp_client_socket.h"
#include "net/socket/udp_server_socket.h"
#endif

namespace crfiles {
namespace {
#if BUILDFLAG(ENABLE_MDNS)
constexpr char kService[] = "_smb._tcp.local";
constexpr size_t kMaxPeers = 128;
constexpr size_t kMaxInterfaces = 16;
constexpr size_t kMaxAddressesPerPeer = 8;
constexpr size_t kMaxProbeEndpoints = 256;
constexpr size_t kMaxConcurrentProbes = 8;
using InterfaceAddresses = std::map<uint32_t, std::map<net::IPAddress, uint32_t>>;

std::string Canonical(std::string name) {
  if (name.ends_with('.')) name.pop_back();
  return base::ToLowerASCII(name);
}

bool PrivateIPv4(const net::IPAddress& address) {
  if (!address.IsIPv4()) return false;
  const auto& bytes = address.bytes();
  return bytes[0] == 10 || (bytes[0] == 172 && (bytes[1] & 0xf0) == 16) ||
      (bytes[0] == 192 && bytes[1] == 168) || (bytes[0] == 169 && bytes[1] == 254);
}

// Browse local IPv4 interfaces. Use Chromium's
// shared mDNS cache, as its default factory does, with bounded local interfaces.
class InterfaceSockets : public net::MDnsSocketFactory {
 public:
  explicit InterfaceSockets(std::set<uint32_t> interfaces) : interfaces_(std::move(interfaces)) {}
  void CreateSockets(std::vector<std::unique_ptr<net::DatagramServerSocket>>* sockets) override {
    for (uint32_t index : interfaces_) {
      auto socket = std::make_unique<net::UDPServerSocket>(nullptr, net::NetLogSource());
      socket->AllowAddressSharingForMulticast();
      // Chromium's generic factory leaves multicast TTL at 1. DNS-SD queries
      // use 255; these options must be set before Listen(), not afterwards.
      int error = socket->SetMulticastInterface(index);
      if (error == net::OK) error = socket->SetMulticastTimeToLive(255);
      if (error == net::OK) error = socket->Listen(net::dns_util::GetMdnsReceiveEndPoint(net::ADDRESS_FAMILY_IPV4));
      if (error == net::OK) error = socket->JoinGroup(net::dns_util::GetMdnsGroupEndPoint(net::ADDRESS_FAMILY_IPV4).address());
      if (error != net::OK) {
        LOG(WARNING) << "SMB mDNS socket failed: interface=" << index << " error=" << error;
        continue;
      }
      LOG(INFO) << "SMB mDNS listening: interface=" << index;
      sockets->push_back(std::move(socket));
    }
  }
 private:
  std::set<uint32_t> interfaces_;
};

class Browser : public net::MDnsListener::Delegate {
 public:
  Browser(std::set<uint32_t> interfaces, InterfaceAddresses networks,
          std::set<net::IPAddress> local_addresses, SmbDiscovery::Callback callback)
      : callback_(std::move(callback)), sockets_(std::move(interfaces)),
        networks_(std::move(networks)), local_addresses_(std::move(local_addresses)) {}
  ~Browser() override {
    refresh_.Stop();
    reconcile_.Stop();
    probes_.clear();
    watches_.clear();
    if (client_) client_->StopListening();
  }
  bool Start() {
    client_ = net::MDnsClient::CreateDefault();
    const int error = client_->StartListening(&sockets_);
    if (error != net::OK) {
      LOG(WARNING) << "SMB mDNS listen failed: error=" << error;
      return false;
    }
    LOG(INFO) << "SMB discovery started: Chromium mDNS, IPv4, SMB services";
    Reconcile();
    refresh_.Start(FROM_HERE, base::Seconds(15), this, &Browser::Refresh);
    return true;
  }
  void OnRecordUpdate(net::MDnsListener::UpdateType, const net::RecordParsed*) override {
    Schedule();
  }
  void OnNsecRecord(const std::string&, uint32_t) override { Schedule(); }
  void OnCachePurged() override { Schedule(); }
  void RequestRefresh() {
    // An explicit refresh retries failures immediately without reconnecting to
    // every healthy server. Periodic refreshes retain the failure backoff.
    for (auto& [endpoint, probe] : probes_) {
      if (!probe.pending && !probe.reachable) probe.checked = {};
    }
    Refresh();
  }

 private:
  using Key = std::pair<uint16_t, std::string>;
  struct Watch {
    std::unique_ptr<net::MDnsListener> listener;
    std::unique_ptr<net::MDnsTransaction> query;
  };
  using Endpoint = std::pair<net::IPAddress, uint16_t>;
  struct Probe {
    std::unique_ptr<net::TCPClientSocket> socket;
    base::OneShotTimer timeout;
    base::TimeTicks checked;
    bool pending = false;
    bool reachable = false;
  };
  struct Candidate {
    SmbPeer peer;
    std::vector<Endpoint> endpoints;
  };
  bool OnLink(const net::IPAddress& address) const {
    for (const auto& [index, addresses] : networks_) {
      for (const auto& [local, prefix] : addresses) {
        if (prefix > 0 && prefix <= 32 && net::IPAddressMatchesPrefix(address, local, prefix))
          return true;
      }
    }
    return false;
  }
  void ProbeFinished(const Endpoint& endpoint, int result) {
    auto it = probes_.find(endpoint);
    if (it == probes_.end() || !it->second.pending) return;
    auto& probe = it->second;
    probe.timeout.Stop();
    probe.pending = false;
    const bool reachable = result == net::OK;
    if (probe.checked.is_null() || probe.reachable != reachable) {
      LOG(INFO) << "SMB discovery TCP probe: address=" << endpoint.first.ToString()
                << " port=" << endpoint.second << " reachable=" << reachable
                << " error=" << result;
    }
    probe.reachable = reachable;
    probe.checked = base::TimeTicks::Now();
    // Cancel pending IO on timeout, but destroy the socket after its callback.
    probe.socket->Disconnect();
    Schedule();
  }
  void PublishReachable(std::vector<Candidate> candidates) {
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
      return a.peer.instance < b.peer.instance;
    });
    std::set<Endpoint> desired;
    std::vector<Endpoint> ordered;
    // Reserve each server's preferred address before spending the bounded
    // budget on alternative addresses from the same server.
    for (size_t rank = 0; rank < kMaxAddressesPerPeer; ++rank) {
      for (const auto& candidate : candidates) {
        if (rank >= candidate.endpoints.size()) continue;
        const auto& endpoint = candidate.endpoints[rank];
        if (desired.size() < kMaxProbeEndpoints && desired.insert(endpoint).second)
          ordered.push_back(endpoint);
      }
    }
    // Removing a DNS record or changing networks also cancels its pending IO.
    std::erase_if(probes_, [&desired](const auto& entry) { return !desired.contains(entry.first); });
    size_t pending = 0;
    for (auto& [endpoint, probe] : probes_) {
      if (probe.pending) ++pending;
      else probe.socket.reset();
    }
    for (const auto& endpoint : ordered) probes_.try_emplace(endpoint);
    // First-time probes precede retries; oldest results get refreshed first so
    // unreachable servers cannot starve devices at the end of a large list.
    std::stable_sort(ordered.begin(), ordered.end(), [this](const Endpoint& a, const Endpoint& b) {
      return probes_.at(a).checked < probes_.at(b).checked;
    });
    for (const auto& endpoint : ordered) {
      if (pending >= kMaxConcurrentProbes) break;
      auto& probe = probes_.at(endpoint);
      const auto lifetime = probe.reachable ? base::Seconds(60) : base::Seconds(15);
      if (probe.pending || (!probe.checked.is_null() &&
          base::TimeTicks::Now() - probe.checked < lifetime)) continue;
      probe.socket = std::make_unique<net::TCPClientSocket>(
          net::AddressList::CreateFromIPAddress(endpoint.first, endpoint.second),
          nullptr, nullptr, nullptr, net::NetLogSource(), net::handles::kInvalidNetworkHandle);
      probe.pending = true;
      // Only connect to the advertised TCP port: no SMB login or file IO.
      const int result = probe.socket->Connect(base::BindOnce(
          &Browser::ProbeFinished, base::Unretained(this), endpoint));
      if (result == net::ERR_IO_PENDING) {
        ++pending;
        probe.timeout.Start(FROM_HERE, base::Seconds(2), base::BindOnce(
            &Browser::ProbeFinished, base::Unretained(this), endpoint, net::ERR_TIMED_OUT));
      } else {
        ProbeFinished(endpoint, result);
      }
    }
    std::vector<SmbPeer> peers;
    for (auto& candidate : candidates) {
      for (const auto& endpoint : candidate.endpoints) {
        if (!desired.contains(endpoint)) continue;
        const auto& probe = probes_.at(endpoint);
        if (probe.reachable) {
          candidate.peer.address = endpoint.first.ToString();
          peers.push_back(std::move(candidate.peer));
          break;
        }
      }
    }
    // DNS cache iteration order must not reorder the UI on every refresh.
    std::ranges::sort(peers, {}, &SmbPeer::instance);
    if (peers != peers_) {
      peers_ = peers;
      callback_.Run(std::move(peers));
    }
  }
  void Schedule() {
    // Never destroy listeners/client inside their callbacks. Coalesce a packet's
    // records and resolve from cache on the next task, after all records arrive.
    if (!reconcile_.IsRunning())
      reconcile_.Start(FROM_HERE, base::Milliseconds(50), this, &Browser::Reconcile);
  }
  void CacheResult(net::MDnsTransaction::Result result, const net::RecordParsed* record) {
    if (result == net::MDnsTransaction::RESULT_RECORD && record && cache_results_.size() < kMaxPeers)
      cache_results_.push_back(record);
  }
  std::vector<const net::RecordParsed*> Cached(const Key& key) {
    cache_results_.clear();
    // QUERY_CACHE-only callbacks complete synchronously in MDnsTransaction::Start.
    auto query = client_->CreateTransaction(key.first, key.second, net::MDnsTransaction::QUERY_CACHE,
        base::BindRepeating(&Browser::CacheResult, base::Unretained(this)));
    query->Start();
    return std::exchange(cache_results_, {});
  }
  void Query(const Key& key, Watch& watch) {
    watch.query = client_->CreateTransaction(key.first, key.second,
        net::MDnsTransaction::QUERY_NETWORK, base::DoNothing());
    if (!watch.query->Start()) watch.query.reset();
  }
  std::vector<const net::RecordParsed*> Read(uint16_t type, const std::string& name) {
    const Key key(type, Canonical(name));
    desired_.insert(key);
    auto [it, added] = watches_.try_emplace(key);
    if (added) {
      it->second.listener = client_->CreateListener(type, key.second, this);
      if (!it->second.listener->Start()) {
        watches_.erase(it);
        return {};
      }
      it->second.listener->SetActiveRefresh(true);
      Query(key, it->second);
    }
    return Cached(key);
  }
  void Refresh() {
    for (auto& [key, watch] : watches_) {
      // Resolved records refresh according to their TTL. Retry unresolved names
      // and browse PTR periodically so newly started devices are discovered.
      if (key.first == net::dns_protocol::kTypePTR || Cached(key).empty()) Query(key, watch);
    }
    Reconcile();
  }
  void Reconcile() {
    desired_.clear();
    std::vector<Candidate> candidates;
    size_t instances = 0;
    size_t local_services = 0;
    for (const char* service : {kService}) {
      for (const auto* ptr : Read(net::dns_protocol::kTypePTR, service)) {
        if (++instances > kMaxPeers) break;
        SmbPeer peer;
        peer.instance = ptr->rdata<net::PtrRecordRdata>()->ptrdomain();
        if (peer.instance.ends_with('.')) peer.instance.pop_back();
        const std::string suffix = std::string(".") + service;
        if (!Canonical(peer.instance).ends_with(suffix) || peer.instance.size() > 253) continue;
        peer.name = peer.instance.substr(0, peer.instance.size() - suffix.size());
        if (peer.name.empty() || peer.name.size() > 63 || !base::IsStringUTF8(peer.name) ||
            std::ranges::any_of(peer.name, [](unsigned char c) { return c < 32 || c == 127; })) continue;
        const auto srv = Read(net::dns_protocol::kTypeSRV, peer.instance);
        if (srv.empty()) continue;
        const auto* data = srv.front()->rdata<net::SrvRecordRdata>();
        peer.host = Canonical(data->target());
        peer.port = data->port();
        if (!peer.port || !peer.host.ends_with(".local") || peer.host.size() > 253) continue;
        const auto addresses = Read(net::dns_protocol::kTypeA, peer.host);
        // Suppress the entire local service, including its other advertised
        // addresses. Capture all local interfaces, not just those we browse.
        if (std::ranges::any_of(addresses, [this](const net::RecordParsed* record) {
              return local_addresses_.contains(record->rdata<net::ARecordRdata>()->address());
            })) {
          ++local_services;
          continue;
        }
        std::vector<Endpoint> endpoints;
        for (const auto* record : addresses) {
          const auto& address = record->rdata<net::ARecordRdata>()->address();
          if (!PrivateIPv4(address)) continue;
          endpoints.emplace_back(address, peer.port);
        }
        // Prefer the local subnet; retain routed private addresses as fallback.
        // Use a stable tie-breaker rather than mDNS response arrival order.
        std::ranges::sort(endpoints, [this](const Endpoint& a, const Endpoint& b) {
          const bool a_local = OnLink(a.first), b_local = OnLink(b.first);
          return a_local != b_local ? a_local : a < b;
        });
        endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
        if (endpoints.size() > kMaxAddressesPerPeer) endpoints.resize(kMaxAddressesPerPeer);
        if (!endpoints.empty()) candidates.push_back({std::move(peer), std::move(endpoints)});
      }
    }
    if (local_services != local_services_) {
      LOG(INFO) << "SMB discovery local services hidden: count=" << local_services;
      local_services_ = local_services;
    }
    std::erase_if(watches_, [this](const auto& entry) { return !desired_.contains(entry.first); });
    PublishReachable(std::move(candidates));
  }

  SmbDiscovery::Callback callback_;
  InterfaceSockets sockets_;
  InterfaceAddresses networks_;
  std::set<net::IPAddress> local_addresses_;
  std::map<Endpoint, Probe> probes_;
  std::unique_ptr<net::MDnsClient> client_;
  std::map<Key, Watch> watches_;
  std::set<Key> desired_;
  std::vector<const net::RecordParsed*> cache_results_;
  std::vector<SmbPeer> peers_;
  size_t local_services_ = 0;
  base::RepeatingTimer refresh_;
  base::OneShotTimer reconcile_;
};
#endif
}  // namespace

class SmbDiscovery::Backend {
 public:
  explicit Backend(Callback callback) : callback_(std::move(callback)) {
#if BUILDFLAG(ENABLE_MDNS)
    PollInterfaces();
    timer_.Start(FROM_HERE, base::Seconds(5), this, &Backend::PollInterfaces);
#else
    LOG(WARNING) << "SMB discovery unavailable: ENABLE_MDNS is disabled";
#endif
  }
  void Refresh() {
#if BUILDFLAG(ENABLE_MDNS)
    PollInterfaces();
    if (browser_) browser_->RequestRefresh();
#endif
  }
 private:
#if BUILDFLAG(ENABLE_MDNS)
  void PollInterfaces() {
    net::NetworkInterfaceList networks;
    if (!net::GetNetworkList(&networks, net::EXCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES)) {
      LOG(WARNING) << "SMB discovery could not enumerate network interfaces";
      return;
    }
    // Some virtual interfaces are intentionally excluded from browsing, but
    // their addresses still identify this machine's own advertisements.
    net::NetworkInterfaceList all_networks;
    if (!net::GetNetworkList(&all_networks, net::INCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES)) {
      LOG(WARNING) << "SMB discovery could not enumerate local addresses";
      return;
    }
    std::set<net::IPAddress> local_addresses;
    for (const auto& network : all_networks) {
      if (network.address.IsIPv4()) local_addresses.insert(network.address);
    }
    InterfaceAddresses current;
    for (const auto& network : networks) {
      if (network.address.IsIPv4()) local_addresses.insert(network.address);
      if (PrivateIPv4(network.address) && (current.contains(network.interface_index) || current.size() < kMaxInterfaces))
        current[network.interface_index][network.address] = network.prefix_length;
    }
    // No NetworkChangeNotifier is installed by this standalone application.
    // Poll off the UI thread; address/index changes invalidate the DNS-SD cache.
    if (current != interfaces_ || local_addresses != local_addresses_) {
      LOG(INFO) << "SMB discovery interface count=" << current.size();
      browser_.reset();
      interfaces_ = std::move(current);
      local_addresses_ = std::move(local_addresses);
      Publish({});
    }
    if (!browser_ && !interfaces_.empty()) {
      std::set<uint32_t> indices;
      for (const auto& entry : interfaces_) indices.insert(entry.first);
      auto browser = std::make_unique<Browser>(std::move(indices), interfaces_, local_addresses_,
          base::BindRepeating(&Backend::Publish, base::Unretained(this)));
      if (browser->Start()) browser_ = std::move(browser);
    }
    if (interfaces_.empty()) LOG(WARNING) << "SMB discovery has no eligible local IPv4 interface";
  }
  void Publish(std::vector<SmbPeer> peers) {
    if (peers == published_) return;
    LOG(INFO) << "SMB discovery server count=" << peers.size();
    published_ = peers;
    callback_.Run(std::move(peers));
  }
  InterfaceAddresses interfaces_;
  std::set<net::IPAddress> local_addresses_;
  std::unique_ptr<Browser> browser_;
  std::vector<SmbPeer> published_;
  base::RepeatingTimer timer_;
#endif
  Callback callback_;
};

SmbDiscovery::SmbDiscovery(Callback callback) {
  started_ = thread_.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0));
  if (started_) thread_.task_runner()->PostTask(FROM_HERE,
      base::BindOnce(&SmbDiscovery::StartOnIO, base::Unretained(this), std::move(callback)));
  else LOG(WARNING) << "SMB discovery IO thread could not start";
}
SmbDiscovery::~SmbDiscovery() {
  if (!started_) return;
  thread_.task_runner()->PostTask(FROM_HERE,
      base::BindOnce(&SmbDiscovery::StopOnIO, base::Unretained(this)));
  thread_.Stop();
}
void SmbDiscovery::StartOnIO(Callback callback) { backend_ = std::make_unique<Backend>(std::move(callback)); }
void SmbDiscovery::StopOnIO() { backend_.reset(); }
void SmbDiscovery::Refresh() {
  if (started_) thread_.task_runner()->PostTask(FROM_HERE,
      base::BindOnce(&SmbDiscovery::RefreshOnIO, base::Unretained(this)));
}
void SmbDiscovery::RefreshOnIO() { if (backend_) backend_->Refresh(); }

}  // namespace crfiles
