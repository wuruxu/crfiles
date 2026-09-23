// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/sharing/connect_discovery.h"
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
#include "base/timer/timer.h"
#include "net/net_buildflags.h"

#if BUILDFLAG(ENABLE_MDNS)
#include "net/base/net_errors.h"
#include "net/base/network_interfaces.h"
#include "net/dns/mdns_client.h"
#include "net/dns/public/dns_protocol.h"
#include "net/dns/public/util.h"
#include "net/dns/record_rdata.h"
#include "net/log/net_log_source.h"
#include "net/socket/datagram_server_socket.h"
#include "net/socket/udp_server_socket.h"
#endif

namespace crfiles {
namespace {
#if BUILDFLAG(ENABLE_MDNS)
constexpr char kService[] = "_crphotos-media._tcp.local";
constexpr char kLegacyService[] = "_crphotos-connect._tcp.local";
constexpr size_t kMaxPeers = 128;
constexpr size_t kMaxInterfaces = 16;

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

// Connect v1 binds private IPv4; do not publish IPv6-only peers. Use Chromium's
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
      if (socket->SetMulticastInterface(index) != net::OK ||
          socket->SetMulticastTimeToLive(255) != net::OK ||
          socket->Listen(net::dns_util::GetMdnsReceiveEndPoint(net::ADDRESS_FAMILY_IPV4)) != net::OK ||
          socket->JoinGroup(net::dns_util::GetMdnsGroupEndPoint(net::ADDRESS_FAMILY_IPV4).address()) != net::OK) continue;
      sockets->push_back(std::move(socket));
    }
  }
 private:
  std::set<uint32_t> interfaces_;
};

class Browser : public net::MDnsListener::Delegate {
 public:
  Browser(std::set<uint32_t> interfaces, ConnectDiscovery::Callback callback)
      : callback_(std::move(callback)), sockets_(std::move(interfaces)) {}
  ~Browser() override {
    refresh_.Stop();
    reconcile_.Stop();
    watches_.clear();
    if (client_) client_->StopListening();
  }
  bool Start() {
    client_ = net::MDnsClient::CreateDefault();
    const int error = client_->StartListening(&sockets_);
    if (error != net::OK) {
      LOG(WARNING) << "Connect mDNS listen failed: error=" << error;
      return false;
    }
    LOG(INFO) << "Connect discovery started: Chromium mDNS, IPv4, current and legacy services";
    Reconcile();
    refresh_.Start(FROM_HERE, base::Seconds(15), this, &Browser::Refresh);
    return true;
  }
  void OnRecordUpdate(net::MDnsListener::UpdateType, const net::RecordParsed*) override {
    Schedule();
  }
  void OnNsecRecord(const std::string&, uint32_t) override { Schedule(); }
  void OnCachePurged() override { Schedule(); }

 private:
  using Key = std::pair<uint16_t, std::string>;
  struct Watch {
    std::unique_ptr<net::MDnsListener> listener;
    std::unique_ptr<net::MDnsTransaction> query;
  };
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
  bool ParseTxt(const net::RecordParsed* record, ConnectPeer& peer) {
    std::map<std::string, std::string> fields;
    size_t total = 0;
    for (const auto& text : record->rdata<net::TxtRecordRdata>()->texts()) {
      total += text.size();
      if (total > 4096) return false;
      const auto equal = text.find('=');
      const auto key = base::ToLowerASCII(text.substr(0, equal));
      if (key.empty() || !base::IsStringASCII(key) || fields.contains(key)) return false;
      fields.emplace(key, equal == std::string::npos ? "" : text.substr(equal + 1));
    }
    if (fields["v"] != "1" || fields["tls"] != "1") return false;
    peer.id = fields["id"];
    if (peer.id.size() != 32 || peer.id.find_first_not_of("0123456789abcdef") != std::string::npos)
      return false;
    const auto& name = fields["name"];
    if (!name.empty() && name.size() <= 63 && base::IsStringUTF8(name) &&
        !std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; })) peer.name = name;
    int avatar = 0;
    if (base::StringToInt(fields["avatar"], &avatar) && avatar >= 0 && avatar < 9) peer.avatar = avatar;
    peer.capabilities = base::SplitString(fields["caps"], ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    for (const char* required : {"list", "thumb", "download"}) {
      if (!std::ranges::contains(peer.capabilities, required)) return false;
    }
    return true;
  }
  void Reconcile() {
    desired_.clear();
    std::vector<ConnectPeer> peers;
    size_t instances = 0;
    for (const char* service : {kService, kLegacyService}) {
      for (const auto* ptr : Read(net::dns_protocol::kTypePTR, service)) {
        if (++instances > kMaxPeers) break;
        ConnectPeer peer;
        peer.instance = ptr->rdata<net::PtrRecordRdata>()->ptrdomain();
        if (peer.instance.ends_with('.')) peer.instance.pop_back();
        const std::string suffix = std::string(".") + service;
        if (!Canonical(peer.instance).ends_with(suffix) || peer.instance.size() > 253) continue;
        peer.name = peer.instance.substr(0, peer.instance.size() - suffix.size());
        if (peer.name.empty() || peer.name.size() > 63 || !base::IsStringUTF8(peer.name) ||
            std::ranges::any_of(peer.name, [](unsigned char c) { return c < 32 || c == 127; })) continue;
        peer.legacy_service = service == kLegacyService;
        const auto srv = Read(net::dns_protocol::kTypeSRV, peer.instance);
        const auto txt = Read(net::dns_protocol::kTypeTXT, peer.instance);
        if (srv.empty()) continue;
        const auto* data = srv.front()->rdata<net::SrvRecordRdata>();
        peer.host = Canonical(data->target());
        peer.port = data->port();
        if (!peer.port || !peer.host.ends_with(".local") || peer.host.size() > 253) continue;
        const auto addresses = Read(net::dns_protocol::kTypeA, peer.host);
        if (txt.empty() || !ParseTxt(txt.front(), peer)) continue;
        for (const auto* record : addresses) {
          const auto& address = record->rdata<net::ARecordRdata>()->address();
          if (!PrivateIPv4(address)) continue;
          peer.address = address.ToString();
          peers.push_back(peer);
          break;
        }
      }
    }
    std::erase_if(watches_, [this](const auto& entry) { return !desired_.contains(entry.first); });
    if (peers != peers_) {
      peers_ = peers;
      callback_.Run(std::move(peers));
    }
  }

  ConnectDiscovery::Callback callback_;
  InterfaceSockets sockets_;
  std::unique_ptr<net::MDnsClient> client_;
  std::map<Key, Watch> watches_;
  std::set<Key> desired_;
  std::vector<const net::RecordParsed*> cache_results_;
  std::vector<ConnectPeer> peers_;
  base::RepeatingTimer refresh_;
  base::OneShotTimer reconcile_;
};
#endif
}  // namespace

class ConnectDiscovery::Backend {
 public:
  explicit Backend(Callback callback) : callback_(std::move(callback)) {
#if BUILDFLAG(ENABLE_MDNS)
    PollInterfaces();
    timer_.Start(FROM_HERE, base::Seconds(5), this, &Backend::PollInterfaces);
#else
    LOG(WARNING) << "Connect discovery unavailable: ENABLE_MDNS is disabled";
#endif
  }
 private:
#if BUILDFLAG(ENABLE_MDNS)
  void PollInterfaces() {
    net::NetworkInterfaceList networks;
    if (!net::GetNetworkList(&networks, net::EXCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES)) return;
    std::map<uint32_t, std::set<std::string>> current;
    for (const auto& network : networks) {
      if (PrivateIPv4(network.address) && (current.contains(network.interface_index) || current.size() < kMaxInterfaces))
        current[network.interface_index].insert(network.address.ToString());
    }
    // No NetworkChangeNotifier is installed by this standalone application.
    // Poll off the UI thread; address/index changes invalidate the DNS-SD cache.
    if (current != interfaces_) {
      browser_.reset();
      interfaces_ = std::move(current);
      Publish({});
    }
    if (!browser_ && !interfaces_.empty()) {
      std::set<uint32_t> indices;
      for (const auto& entry : interfaces_) indices.insert(entry.first);
      auto browser = std::make_unique<Browser>(std::move(indices),
          base::BindRepeating(&Backend::Publish, base::Unretained(this)));
      if (browser->Start()) browser_ = std::move(browser);
    }
  }
  void Publish(std::vector<ConnectPeer> peers) {
    if (peers == published_) return;
    for (const auto& peer : peers) {
      if (!std::ranges::contains(published_, peer))
        LOG(INFO) << "Connect discovered: address=" << peer.address << ":" << peer.port
                  << " legacy_service=" << peer.legacy_service;
    }
    LOG(INFO) << "Connect discovery updated: peers=" << peers.size();
    published_ = peers;
    callback_.Run(std::move(peers));
  }
  std::map<uint32_t, std::set<std::string>> interfaces_;
  std::unique_ptr<Browser> browser_;
  std::vector<ConnectPeer> published_;
  base::RepeatingTimer timer_;
#endif
  Callback callback_;
};

ConnectDiscovery::ConnectDiscovery(Callback callback) {
  started_ = thread_.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0));
  if (started_) thread_.task_runner()->PostTask(FROM_HERE,
      base::BindOnce(&ConnectDiscovery::StartOnIO, base::Unretained(this), std::move(callback)));
  else LOG(WARNING) << "Connect discovery IO thread could not start";
}
ConnectDiscovery::~ConnectDiscovery() {
  if (!started_) return;
  thread_.task_runner()->PostTask(FROM_HERE,
      base::BindOnce(&ConnectDiscovery::StopOnIO, base::Unretained(this)));
  thread_.Stop();
}
void ConnectDiscovery::StartOnIO(Callback callback) { backend_ = std::make_unique<Backend>(std::move(callback)); }
void ConnectDiscovery::StopOnIO() { backend_.reset(); }

}  // namespace crfiles
