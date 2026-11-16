// Copyright 2026 The Chromium Authors
#ifndef CRFILES_CRFILES_NETWORK_SHARE_H_
#define CRFILES_CRFILES_NETWORK_SHARE_H_
#include "crFiles/crfiles_network.h"
#include "crFiles/sharing/share_service.h"
namespace crfiles {
// Blocking; requires MayBlock and WithBaseSyncPrimitives task traits.
// All SMB work remains in a child process.
mojom::SmbError CheckNetworkShareWritable(SmbCredentials credentials, base::FilePath cache_directory,
    base::RepeatingCallback<bool()> canceled);
std::shared_ptr<ShareService::RemoteReceiveTarget> CreateNetworkReceiveTarget(SmbCredentials credentials);
}
#endif
