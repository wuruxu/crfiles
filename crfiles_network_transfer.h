// Copyright 2026 The Chromium Authors
#ifndef CRFILES_NETWORK_TRANSFER_H_
#define CRFILES_NETWORK_TRANSFER_H_
#include "crFiles/crfiles_network.h"
namespace views { class Widget; }
namespace crfiles {
struct NetworkClipboard {
  SmbCredentials credentials;
  std::vector<mojom::SmbEntryPtr> entries;
};
// Credentials stay in this process; the system clipboard contains URLs only.
void CopyNetworkFiles(SmbCredentials credentials, std::vector<mojom::SmbEntryPtr> entries);
std::shared_ptr<const NetworkClipboard> NetworkFileClipboard();
void ReadLocalFileClipboard(base::OnceCallback<void(std::vector<base::FilePath>)> callback);
// A modal, cancelable job owns independent SMB workers. Missing destination
// credentials mean a local destination. Sources are always retained.
void CopyNetworkSelection(views::Widget* parent,
    std::shared_ptr<const NetworkClipboard> remote_source,
    std::vector<base::FilePath> local_sources,
    std::optional<SmbCredentials> remote_destination,
    base::FilePath local_destination, base::OnceClosure changed);
// Conservative identity checks: aliases and different credentials use copying.
bool CanMoveNetworkSelection(const NetworkClipboard& source, const SmbCredentials& destination);
bool ValidNetworkTransferDestination(const NetworkClipboard& source, const SmbCredentials& destination);
// Same-share rename only. A failure never falls back to copying and deleting.
void MoveNetworkSelection(views::Widget* parent,
    std::shared_ptr<const NetworkClipboard> source, SmbCredentials destination,
    base::OnceClosure changed);
}
#endif
