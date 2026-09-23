// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_SHARING_CONNECT_DISCOVERY_H_
#define CRFILES_SHARING_CONNECT_DISCOVERY_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/threading/thread.h"

namespace crfiles {

// Unauthenticated DNS-SD hints. Neither id nor capabilities establish trust.
// Keep separate from NearbySharing::Peer: this endpoint speaks HTTPS.
struct ConnectPeer {
  std::string instance;
  std::string name;
  std::string id;
  std::string host;
  std::string address;
  uint16_t port = 0;
  std::vector<std::string> capabilities;
  bool legacy_service = false;
  int avatar = 0;
  bool operator==(const ConnectPeer&) const = default;
};

// Owns an IO thread. Callback runs there; callers must not access Views.
// Destruction cancels all discovery and joins the thread before returning.
class ConnectDiscovery {
 public:
  using Callback = base::RepeatingCallback<void(std::vector<ConnectPeer>)>;
  explicit ConnectDiscovery(Callback callback);
  ~ConnectDiscovery();
  ConnectDiscovery(const ConnectDiscovery&) = delete;
  ConnectDiscovery& operator=(const ConnectDiscovery&) = delete;

 private:
  class Backend;
  void StartOnIO(Callback callback);
  void StopOnIO();
  base::Thread thread_{"crPhotosConnectDiscovery"};
  bool started_ = false;
  std::unique_ptr<Backend> backend_;
};

}  // namespace crfiles
#endif  // CRFILES_SHARING_CONNECT_DISCOVERY_H_
