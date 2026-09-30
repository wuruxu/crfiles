// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CRFILES_SHARING_CONNECT_CLIENT_H_
#define CRFILES_SHARING_CONNECT_CLIENT_H_

#include <memory>
#include <string>
#include "base/functional/callback.h"
#include "base/files/file_path.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "crFiles/sharing/connect_discovery.h"

namespace crfiles {

enum class ConnectAction { kLoad, kPair, kConnect, kForget, kImport, kList, kDownload, kReceive, kUpload };
enum class ConnectStatus {
  kNeedsPairing, kSaved, kConnected, kForgotten, kInvalidInput,
  kNetworkError, kCertificateMismatch, kUnauthorized, kPairRejected,
  kInvalidResponse, kStorageError, kUploading,
};
struct ConnectRequest {
  ConnectAction action = ConnectAction::kLoad;
  ConnectPeer peer;
  std::string fingerprint;
  std::string code;
  std::string token;
  std::string client_id;
  // Internal sync requests only. Destination is an exclusive temporary file.
  std::string path;
  base::FilePath destination;
  bool thumbnail = false;
  // Receive control JSON or a bounded streaming media upload.
  std::string body;
  base::FilePath source;
  int64_t source_size = 0;
  base::Time source_modified;
};
struct ConnectResult {
  ConnectStatus status = ConnectStatus::kNeedsPairing;
  std::string fingerprint;
  bool saved = false;
  std::string body;
  std::string sha256;
  int64_t bytes = 0;
  int http_status = 0;
  int net_error = 0;
  int media_count = 0;  // First page only, not the phone's total media count.
};

// All network and credential IO runs on a dedicated thread. Callback runs there;
// callers should BindPostTaskToCurrentDefault with a WeakPtr to their view.
// Destruction joins that thread. ThreadPool owners must declare
// WithBaseSyncPrimitives in addition to MayBlock.
class ConnectClient {
 public:
  using Callback = base::RepeatingCallback<void(ConnectResult)>;
  explicit ConnectClient(Callback callback);
  ~ConnectClient();
  void Request(ConnectRequest request);
  // Memory-only UI hint, tied to a recent pinned, authorized HTTPS response
  // from this exact endpoint. Never used to authorize a request.
  static bool IsVerifiedPeer(const ConnectPeer& peer,
                             base::TimeDelta minimum_validity = base::TimeDelta());
 private:
  class Backend;
  void StartOnIO(Callback callback);
  void RequestOnIO(ConnectRequest request);
  void StopOnIO();
  Callback callback_;
  base::Thread thread_{"crPhotosConnectClient"};
  bool started_ = false;
  std::unique_ptr<Backend> backend_;
};

}  // namespace crfiles
#endif  // CRFILES_SHARING_CONNECT_CLIENT_H_
