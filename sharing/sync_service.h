// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRFILES_SHARING_SYNC_SERVICE_H_
#define CRFILES_SHARING_SYNC_SERVICE_H_

#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/values.h"
#include "base/time/time.h"

namespace crfiles {

// A short-lived, authenticated session. All methods run on the transport worker.
class SyncWire {
 public:
  virtual ~SyncWire() = default;
  virtual bool Write(base::DictValue message) = 0;
  virtual std::optional<base::DictValue> Read() = 0;
  virtual bool Bytes(base::span<uint8_t> bytes, bool sending) = 0;
  virtual bool Canceled() = 0;
  virtual bool KeepAlive() = 0;
  virtual bool Consent(const base::FilePath& directory, const std::string& name) = 0;
  virtual void Begin(uint64_t total) = 0;
  virtual void Progress(const std::string& path, uint64_t bytes) = 0;
};

struct SyncJob {
  std::string id;
  std::string peer;
  std::string fingerprint;
  base::FilePath root;
  base::FilePath recovery;
  std::string name;
  int avatar = 0;
  base::Time started;
  base::Time last_synced;
  // Local grants to the authenticated remote device.
  bool read = true;
  bool write = true;
  bool paused = false;
  bool removed = false;
  int status = 0;  // Localized resource ID; never peer-supplied text.
  bool operator==(const SyncJob&) const = default;
};

struct SyncEndpoint {
  std::string address;
  std::string fingerprint;
};

struct SyncRequest {
  std::string id;
  std::string address;
  std::string fingerprint;
};

// The UI only queues commands and reads snapshots. The existing desktop worker
// owns all database, scan and protocol work. Watchers live on a separate sequence.
class SyncService {
 public:
  explicit SyncService(base::FilePath state_directory);
  ~SyncService();
  SyncService(const SyncService&) = delete;
  SyncService& operator=(const SyncService&) = delete;

  // Returns the queued request ID; registration and connection may still fail.
  std::string Queue(const base::FilePath& source, std::string fingerprint,
             std::string peer, int avatar, bool write);
  void Pause(std::string id, bool paused);
  void Permissions(std::string id, bool read, bool write);
  void Remove(std::string id);
  void Revoke(std::string fingerprint);
  std::vector<SyncJob> Snapshot() const;

  // Worker-only. Calls from successive transport generations are serialized.
  void Poll();
  std::optional<SyncRequest> Next(const std::vector<SyncEndpoint>& peers);
  bool CanConnect(const std::string& id, const std::string& fingerprint);
  bool Revoked(const std::string& id, const std::string& fingerprint);
  bool Authorized(const std::string& id, const std::string& fingerprint);
  bool Run(SyncWire& wire, bool outgoing, const std::string& id,
           const std::string& fingerprint, const std::string& peer, int avatar,
           const base::FilePath& receive_directory);
  void Failed(const std::string& id);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

bool IsSyncId(const std::string& value);
}  // namespace crfiles
#endif
