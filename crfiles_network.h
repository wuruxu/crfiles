// Copyright 2026 The Chromium Authors
#ifndef CRFILES_NETWORK_H_
#define CRFILES_NETWORK_H_
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "base/functional/callback.h"
#include "base/process/process.h"
#include "crFiles/crfiles_network.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
namespace views { class View; class ContextMenuController; class DragController; }
namespace gfx { class ImageSkia; }
namespace ui { class Event; class KeyEvent; }
namespace crfiles {
inline constexpr char kSmbProcessSwitch[] = "crfiles-smb-process";
inline constexpr char kSmbDiscoverySwitch[] = "crfiles-smb-discover";
inline constexpr uint64_t kSmbPreviewLimit = uint64_t{2} << 30;
inline constexpr uint64_t kSmbNearbyFileLimit = uint64_t{2} << 30;
int RunSmbProcess();
int RunSmbDiscovery();
struct SmbLocation {
  std::string server, share, path;
  std::string Url() const;
};
std::optional<SmbLocation> ParseSmbLocation(std::string address);
struct SmbCredentials {
  SmbLocation location;
  std::string user, password, domain;
};
struct NetworkColumns {
  bool size = true, type = true, modified = true, created = false;
  bool operator==(const NetworkColumns&) const = default;
};
struct NetworkFooterState {
  bool visible = false, busy = false, connected = false;
  bool show_actions = false;
  // Idle network-page statistics use the ordinary file footer layout.
  bool show_summary = false;
  std::u16string text;
  mojom::SmbWriteAccess write_access = mojom::SmbWriteAccess::kUnknown;
};
class SmbConnection : private mojom::SmbTransferProgress {
 public:
  SmbConnection();
  ~SmbConnection() override;
  bool Start(base::OnceClosure disconnected);
  void TakeOver(SmbConnection& other, base::OnceClosure disconnected);
  void Stop();
  mojo::PendingAssociatedRemote<mojom::SmbTransferProgress> ObserveTransfer(
      base::RepeatingCallback<void(uint64_t, uint64_t)> progress);
  mojom::SmbService* service() { return service_.get(); }
 private:
  void Advanced(uint64_t bytes, uint64_t total) override;
  mojo::AssociatedReceiver<mojom::SmbTransferProgress> receiver_{this};
  base::RepeatingCallback<void(uint64_t, uint64_t)> progress_;
  base::Process process_;
  mojo::Remote<mojom::SmbService> service_;
};
struct ConnectedSmbSession {
  SmbCredentials credentials;
  std::unique_ptr<SmbConnection> connection;
  mojom::SmbListingPtr listing;
};
std::unique_ptr<views::View> CreateNetworkPage(base::RepeatingCallback<void(base::FilePath)> open,
    base::RepeatingCallback<void(std::unique_ptr<ConnectedSmbSession>)> open_device, base::RepeatingClosure changed,
    base::RepeatingCallback<views::View*()> address_anchor,
    base::RepeatingCallback<void(SmbCredentials, int)> open_location,
    base::RepeatingCallback<void(int)> sorted);
// Implemented beside the ordinary file views, reusing their layout and cells.
std::unique_ptr<views::View> CreateNetworkDevicesView(const std::vector<mojom::SmbServerPtr>& servers,
    bool icons, int icon_size, const gfx::ImageSkia& image, base::RepeatingCallback<void(std::string)> open);
void SetNetworkDevicesImage(views::View* contents, const gfx::ImageSkia& image);
std::unique_ptr<views::View> CreateNetworkFilesView(base::span<const mojom::SmbEntryPtr> entries,
    bool icons, int icon_size, NetworkColumns columns, views::ContextMenuController* menu,
    base::RepeatingCallback<void(size_t, bool, const ui::Event&)> select);
std::optional<size_t> NetworkFileIndex(views::View* contents, views::View* source);
views::View* NetworkFileNameAnchor(views::View* contents, size_t index);
void SetNetworkFilesImages(views::View* contents, const std::vector<gfx::ImageSkia>& images);
void SetNetworkFileSelection(views::View* contents, const std::set<size_t>& selected,
    std::optional<size_t> drop_target = std::nullopt);
void SetNetworkFilesDragController(views::View* contents, views::DragController* controller,
    base::RepeatingClosure done);
void SetNetworkMarquee(views::View* contents,
    base::RepeatingCallback<void(std::set<size_t>, bool, bool, bool)> select);
size_t NetworkFileColumns(views::View* contents);
void FocusNetworkFile(views::View* contents, size_t index);
void SetNetworkPageLayout(views::View* page, bool icons, int icon_size, bool show_hidden, NetworkColumns columns, int sort);
bool NetworkPageKey(views::View* page, const ui::KeyEvent& event);
void CopyNetworkPageSelection(views::View* page);
void PasteNetworkPage(views::View* page);
void SetNetworkPageFooter(views::View* page, base::RepeatingCallback<void(NetworkFooterState)> changed);
void SetNetworkPageBottomInset(views::View* page, int height);
// The pane owns the same anchored form as local folders; the network page owns
// validation and the asynchronous mutation. Stop also resolves pending results.
void SetNetworkPageNewFolderHandler(views::View* page, base::RepeatingClosure show);
void CreateNetworkFolder(views::View* page, std::u16string name,
    base::OnceCallback<void(mojom::SmbError)> done);
void CancelNetworkPageOperation(views::View* page);
std::optional<SmbCredentials> NetworkPageCredentials(views::View* page);
mojom::SmbWriteAccess NetworkPageWriteAccess(views::View* page);
void ConnectNetworkPage(views::View* page, SmbCredentials credentials);
void AdoptNetworkSession(views::View* page, std::unique_ptr<ConnectedSmbSession> session);
void PromptNetworkPage(views::View* page, const std::string& address);
bool NavigateNetworkPage(views::View* page, const std::string& address);
std::string NetworkPageUrl(views::View* page);
bool NetworkPageCanGo(views::View* page, int delta);
void NetworkPageGo(views::View* page, int delta);
void RefreshNetworkPage(views::View* page);
void StopNetworkPage(views::View* page);
}
#endif
