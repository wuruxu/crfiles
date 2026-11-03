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
  // Returns the approved destination, or nullopt when consent was declined.
  virtual std::optional<base::FilePath> Consent(const base::FilePath& destination,
      const std::string& name, bool single_file, int error) = 0;
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
  // Creator A's grants to receiver B, mirrored unchanged on both devices.
  // Read permits A -> B; Write permits B -> A.
  bool creator = false;
  bool permissions_known = false;
  bool read = true;
  bool write = false;
  bool paused = false;
  bool removed = false;
  int status = 0;  // Localized resource ID; never peer-supplied text.
  bool operator==(const SyncJob&) const = default;
};

struct SyncEndpoint {
  std::string address;
  std::string fingerprint;
  bool removal = false;
};

struct SyncRequest {
  std::string id;
  std::string address;
  std::string fingerprint;
  bool removal = false;
};

struct SyncStartResult {
  std::string id;
  int error = 0;  // Localized resource ID; zero means the command was queued.
};

struct SyncWakeup {
  std::string id;
  std::string fingerprint;
  uint64_t counter = 0;
};

// The UI only queues commands and reads snapshots. The existing desktop worker
// owns database, scan and file protocol work. Watchers live on a separate sequence.
class SyncService {
 public:
  explicit SyncService(base::FilePath state_directory);
  ~SyncService();
  SyncService(const SyncService&) = delete;
  SyncService& operator=(const SyncService&) = delete;

  // Returns a queued ID or an immediate localized error. Registration and
  // connection may still fail after a command is queued.
  SyncStartResult Queue(const base::FilePath& source, std::string fingerprint,
             std::string peer, int avatar, bool write);
  // Retry the same relationship, preserving its original source and peer.
  SyncStartResult Retry(const std::string& id, const std::string& fingerprint, bool write);
  void Pause(std::string id, bool paused);
  void Permissions(std::string id, bool read, bool write);
  void Remove(std::string id);
  void Revoke(std::string fingerprint);
  std::vector<SyncJob> Snapshot() const;

  // Identity workers only touch published state, never the database/I/O lock.
  std::vector<SyncWakeup> PendingWakeups() const;
  void WakeAttempt(const SyncWakeup& wake);
  bool RequestWake(const std::string& id, const std::string& fingerprint);

  // Worker-only. Calls from successive transport generations are serialized.
  void Poll();
  std::optional<SyncRequest> Next(const std::vector<SyncEndpoint>& peers);
  bool CanConnect(const std::string& id, const std::string& fingerprint);
  bool Revoked(const std::string& id, const std::string& fingerprint);
  // Removal-only authorization never grants access to files or ordinary sharing.
  bool CanRemove(const std::string& id, const std::string& fingerprint, bool outgoing);
  bool RemoteRemoved(const std::string& id, const std::string& fingerprint);
  bool RunRemoval(SyncWire& wire, bool outgoing, const std::string& id,
                  const std::string& fingerprint);
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
// Errors which should remain visible even when the relationship is paused.
bool IsSyncErrorStatus(int status);
}  // namespace crfiles
#endif
