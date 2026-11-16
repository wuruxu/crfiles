// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_NETWORK_DISCOVERY_H_
#define CRFILES_NETWORK_DISCOVERY_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/threading/thread.h"

namespace crfiles {

// Unauthenticated DNS-SD hints; a discovered name is not an identity guarantee.
struct SmbPeer {
  std::string instance;
  std::string name;
  std::string host;
  std::string address;
  uint16_t port = 0;
  bool operator==(const SmbPeer&) const = default;
};

// Owns an IO thread. Callback runs there; callers must not access Views.
// Destruction cancels all discovery and joins the thread before returning.
class SmbDiscovery {
 public:
  using Callback = base::RepeatingCallback<void(std::vector<SmbPeer>)>;
  explicit SmbDiscovery(Callback callback);
  ~SmbDiscovery();
  SmbDiscovery(const SmbDiscovery&) = delete;
  SmbDiscovery& operator=(const SmbDiscovery&) = delete;
  void Refresh();

 private:
  class Backend;
  void StartOnIO(Callback callback);
  void StopOnIO();
  void RefreshOnIO();
  base::Thread thread_{"crFilesSmbDiscovery"};
  bool started_ = false;
  std::unique_ptr<Backend> backend_;
};

}  // namespace crfiles
#endif  // CRFILES_NETWORK_DISCOVERY_H_
