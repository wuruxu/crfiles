// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_network.h"
#include "crFiles/crfiles_network_transfer.h"
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_file_icons.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <array>
#include <string>
#include <utility>
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/i18n/time_formatting.h"
#include "net/base/mime_util.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_format_type.h"
#include "ui/base/dragdrop/drag_drop_types.h"
#include "ui/base/dragdrop/drop_target_event.h"
#include "ui/base/dragdrop/mojom/drag_drop_types.mojom.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/dragdrop/os_exchange_data_provider.h"
#include "ui/base/text/bytes_formatting.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/pickle.h"
#include "base/uuid.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "components/vector_icons/vector_icons.h"
#include "crFiles/grit/crfiles_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer_tree_owner.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/context_menu_controller.h"
#include "ui/views/drag_controller.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"

namespace crfiles {
namespace {
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
using Error = mojom::SmbError;
using Mutation = mojom::SmbMutation;
enum class ConnectionMessageKind { kInfo, kProgress, kError };
// Only an opaque, per-drag token crosses the OS drag service. Credentials and
// source refresh callbacks remain in this process and expire when dragging ends.
struct NetworkDragState {
  std::string token;
  std::shared_ptr<const NetworkClipboard> source;
  base::RepeatingClosure changed;
};
NetworkDragState& NetworkDrag() { static base::NoDestructor<NetworkDragState> state; return *state; }
ui::ClipboardFormatType NetworkDragFormat() {
  return ui::ClipboardFormatType::CustomPlatformType("application/x-crfiles-smb-selection");
}
bool MatchesNetworkDrag(const ui::OSExchangeData& data) {
  const auto& drag = NetworkDrag();
  if (!drag.source || drag.token.empty()) return false;
  auto pickle = data.GetPickledData(NetworkDragFormat());
  if (!pickle) return false;
  base::PickleIterator reader(*pickle);
  std::string token;
  return reader.ReadString(&token) && token == drag.token;
}
// Views' DropHelper recreates drop events without modifier flags. Capture the
// modifier before entering the native drag loop and advertise copy-only then.
class NetworkDragModifiers : public ui::EventHandler {
 public:
  void OnMouseEvent(ui::MouseEvent* event) override {
    if (event->type() == ui::EventType::kMousePressed || event->type() == ui::EventType::kMouseDragged)
#if BUILDFLAG(IS_MAC)
      copy_ = event->IsAltDown();
#else
      copy_ = event->IsControlDown();
#endif
  }
  bool copy() const { return copy_; }
 private:
  bool copy_ = false;
};
constexpr std::array kNetworkSortLabels = {IDS_CRFILES_SORT_A_Z, IDS_CRFILES_SORT_Z_A,
    IDS_CRFILES_LAST_MODIFIED, IDS_CRFILES_FIRST_MODIFIED, IDS_CRFILES_SIZE, IDS_CRFILES_TYPE,
    IDS_CRFILES_SORT_SIZE_ASCENDING, IDS_CRFILES_SORT_TYPE_DESCENDING,
    IDS_CRFILES_SORT_CREATED_ASCENDING, IDS_CRFILES_SORT_CREATED_DESCENDING};
std::u16string EntryType(const mojom::SmbEntry& entry) {
  if (entry.directory) return Text(IDS_CRFILES_FOLDER);
  auto extension = base::FilePath::FromUTF8Unsafe(entry.name).FinalExtension();
  if (!extension.empty()) extension.erase(0, 1);
  std::string mime;
  if (!net::GetWellKnownMimeTypeFromExtension(extension, &mime)) mime = "application/octet-stream";
  return base::UTF8ToUTF16(mime);
}
int ErrorText(Error error) {
  switch (error) {
    case Error::kAccessDenied: return IDS_CRFILES_NETWORK_AUTH_ERROR;
    case Error::kInvalidAddress: return IDS_CRFILES_NETWORK_ADDRESS_ERROR;
    case Error::kTooLarge: return IDS_CRFILES_NETWORK_TOO_LARGE;
    case Error::kLocalFile: return IDS_CRFILES_NETWORK_SAVE_ERROR;
    case Error::kAlreadyExists: return IDS_CRFILES_NETWORK_EXISTS;
    default: return IDS_CRFILES_NETWORK_ERROR;
  }
}
// A private staging directory prevents a remote name from replacing a local
// file. Only a completed download is published to the user's chosen location.
struct DownloadFile {
  base::ScopedTempDir directory;
  base::File file;
  base::FilePath staging, destination;
  bool open = false;
  ~DownloadFile() {
    file.Close();
    if (directory.IsValid()) base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
        base::BindOnce([](base::FilePath path) { base::DeletePathRecursively(path); }, directory.Take()));
  }
};
std::shared_ptr<DownloadFile> PrepareDownload(std::string name, base::FilePath destination) {
  auto result = std::make_shared<DownloadFile>();
  const auto filename = base::FilePath::FromUTF8Unsafe(name);
  if (filename.empty() || filename.IsAbsolute() || filename.BaseName() != filename || filename.ReferencesParent()) return result;
  result->open = destination.empty();
  result->destination = destination;
  base::FilePath parent;
  if (result->open) {
    if (!base::GetTempDir(&parent)) return result;
    // Completed cache copies can outlive the page and independent PDF reader.
    // Prune copies older than seven days on the next download.
    base::FileEnumerator old(parent, false, base::FileEnumerator::DIRECTORIES, FILE_PATH_LITERAL("crFiles-SMB-*"));
    for (auto path = old.Next(); !path.empty(); path = old.Next())
      if (old.GetInfo().GetLastModifiedTime() < base::Time::Now() - base::Days(7)) base::DeletePathRecursively(path);
  } else parent = destination.DirName();
  if (!result->directory.CreateUniqueTempDirUnderPath(parent,
      result->open ? FILE_PATH_LITERAL("crFiles-SMB-") : FILE_PATH_LITERAL(".crFiles-SMB-"))) return result;
  result->staging = result->directory.GetPath().Append(filename);
  uint32_t flags = base::File::FLAG_CREATE | base::File::FLAG_WRITE;
#if BUILDFLAG(IS_WIN)
  flags |= base::File::FLAG_WIN_SHARE_DELETE;
#endif
  result->file = base::File(result->staging, flags);
  return result;
}
base::FilePath PublishDownload(std::shared_ptr<DownloadFile> file) {
  if (file->open) { file->directory.Take(); return file->staging; }
  if (!base::ReplaceFile(file->staging, file->destination, nullptr)) return {};
  return file->destination;
}

class NetworkPage : public views::View, public views::TextfieldController,
                    public views::ContextMenuController,
                    public views::DragController,
                    public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(NetworkPage, views::View)
 public:
  NetworkPage(base::RepeatingCallback<void(base::FilePath)> open,
              base::RepeatingCallback<void(std::unique_ptr<ConnectedSmbSession>)> open_device, base::RepeatingClosure changed,
              base::RepeatingCallback<views::View*()> address_anchor,
              base::RepeatingCallback<void(SmbCredentials, int)> open_location,
              base::RepeatingCallback<void(int)> sorted)
      : open_(std::move(open)), open_device_(std::move(open_device)), changed_(std::move(changed)), address_anchor_(std::move(address_anchor)), open_location_(std::move(open_location)), sorted_(std::move(sorted)) {
    AddPreTargetHandler(&drag_modifiers_);
    SetFocusBehavior(FocusBehavior::ALWAYS);
    SetBackground(views::CreateSolidBackground(ui::kColorSysSurface1));
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(4), 8));
    actions_ = AddChildView(std::make_unique<views::View>());
    auto* actions = actions_.get();
    auto* action_layout = actions->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    action_layout->SetFlexForView(actions->AddChildView(std::make_unique<views::View>()), 1);
    cancel_ = Button(actions, IDS_CRFILES_CANCEL, base::BindRepeating(&NetworkPage::Cancel, base::Unretained(this)));
    cancel_->SetVisible(false); actions_->SetVisible(false);
    status_ = AddChildView(std::make_unique<views::Label>());
    status_->SetHorizontalAlignment(gfx::ALIGN_LEFT); status_->SetMultiLine(true);
    // Preferred-size contents receive bounds through the layered scrolling
    // path. Without it, the plain list View stays at zero size on Linux/Windows.
    scroll_ = AddChildView(std::make_unique<views::ScrollView>(views::ScrollView::ScrollWithLayers::kEnabled));
    auto* scroll = scroll_.get();
    scroll->SetUseContentsPreferredSize(true);
    scroll->SetHorizontalScrollBarMode(views::ScrollView::ScrollBarMode::kDisabled);
    scroll->SetBackgroundColor(std::nullopt);
    scroll->set_context_menu_controller(this);
    list_ = scroll->SetContents(std::make_unique<views::View>());
    list_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
    layout->SetFlexForView(scroll, 1);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(&NetworkPage::Home, weak_.GetWeakPtr()));
  }
  ~NetworkPage() override { RemovePreTargetHandler(&drag_modifiers_); Stop(); }
  void SetFooter(base::RepeatingCallback<void(NetworkFooterState)> changed) {
    footer_changed_ = std::move(changed); UpdateStatusPresentation();
  }
  void SetBottomInset(int height) {
    if (bottom_inset_ == height) return;
    bottom_inset_ = height;
    SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(0, 0, height, 0)));
  }
  void SetNewFolderHandler(base::RepeatingClosure show) { show_new_folder_ = std::move(show); }
  void CreateFolder(std::u16string name, base::OnceCallback<void(Error)> done) {
    if (!connected_ || busy_ || share_.empty()) { std::move(done).Run(Error::kFailed); return; }
    const auto target = base::UTF16ToUTF8(name);
    if (target.empty() || target == "." || target == ".." || target.size() > 1024 ||
        !base::IsStringUTF8(target) || ChildPath(target).size() > 4096 ||
        std::ranges::any_of(target, [](unsigned char c) { return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':'; })) {
      std::move(done).Run(Error::kInvalidAddress); return;
    }
    create_folder_done_ = std::move(done);
    Busy(true); Watchdog(); SetStatus(Text(IDS_CRFILES_LOADING));
    connection_.service()->Mutate(share_, ChildPath(target), Mutation::kCreateDirectory, {},
        base::BindOnce(&NetworkPage::FolderCreated, weak_.GetWeakPtr()));
  }
  void CancelOperation() { if (busy_) Cancel(); }
  void Stop() {
    FinishDrag(); drop_text_.clear(); drop_target_.reset();
    CloseFileMenu(); entries_.clear(); visible_entries_.clear(); selected_.clear(); anchor_.reset(); cursor_.reset(); ++listing_generation_;
    CloseConnection(false);
    weak_.InvalidateWeakPtrs(); poll_.Stop(); deadline_.Stop(); empty_.Stop();
    connection_.Stop(); running_ = false; connected_ = false; busy_ = false; discovering_ = false; refresh_requested_ = false; download_.reset();
    pending_history_ = -1; icon_loading_ = false;
    // Resolve the form even when cancellation/disconnection invalidates the IPC
    // callback. Defer UI notification until Stop (or destruction) has unwound.
    if (create_folder_done_) base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(std::move(create_folder_done_), Error::kFailed));
  }
  void Refresh() {
    if (busy_) return;
    if (connected_) { Load(share_, path_); return; }
    if (device_mode_) {
      ShowCredentials();
      return;
    }
    if (!running_) { Home(); return; }
    // Keep the worker, cache and listeners alive while sending a fresh query.
    // Repeated clicks must not restart the eight-second discovery window.
    ArmDiscoveryTimeout();
    Discover(true);
  }
  std::string Url() const { return server_address_.empty() ? std::string() : SmbLocation{server_address_, share_, path_}.Url(); }
  bool CanGo(int delta) const {
    const int next = history_index_ + delta;
    return connected_ && !busy_ && next >= 0 && next < static_cast<int>(history_.size());
  }
  void Go(int delta) {
    if (!CanGo(delta)) return;
    const int next = history_index_ + delta;
    const auto location = ParseSmbLocation(history_[next]);
    if (location && connected_) { pending_history_ = next; Load(location->share, location->path); }
  }
  bool Navigate(const std::string& address) {
    const auto location = ParseSmbLocation(address.starts_with('/') && !server_address_.empty()
        ? "smb://" + server_address_ + address : address);
    if (!location || location->server != server_address_) return false;
    if (busy_) return true;
    if (connected_) Load(location->share, location->path);
    else Prompt(location->Url());
    return true;
  }
  void SetLayout(bool icons, int icon_size, bool show_hidden, NetworkColumns columns, int sort) {
    if (icons_ == icons && icon_size_ == icon_size && show_hidden_ == show_hidden && columns_ == columns && sort_ == sort) return;
    CloseConnection();
    const bool filter_changed = show_hidden_ != show_hidden;
    icons_ = icons;
    show_hidden_ = show_hidden; columns_ = columns; sort_ = std::clamp(sort, 0, 9);
    if (icon_size_ != icon_size) { icon_size_ = icon_size; server_icon_ = {}; icon_loading_ = false; }
    if (device_mode_) {
      if (connected_) {
        RenderListing();
        if (filter_changed && !busy_) UpdateListingStatus();
      }
    }
    else RenderServers();
  }
  void Prompt(const std::string& address) {
    const auto location = ParseSmbLocation(address);
    if (!location) return;
    Stop(); device_mode_ = true; credentials_needed_ = true;
    server_address_ = location->server; share_ = location->share; path_ = location->path;
    credential_url_ = location->Url(); ResetList(); SetStatus({}); Busy(false);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&NetworkPage::ShowCredentials, weak_.GetWeakPtr()));
  }
  void StartConnection(SmbCredentials credentials) {
    Stop(); device_mode_ = true; credentials_needed_ = false; credential_url_.clear();
    credentials_ = credentials;
    server_address_ = credentials.location.server;
    share_ = credentials.location.share; path_ = credentials.location.path;
    changed_.Run();
    if (!StartWorker()) { Failed(); return; }
    Busy(true); Watchdog(); SetStatus(Text(IDS_CRFILES_LOADING));
    connection_.service()->Connect(server_address_, credentials.user, credentials.password, credentials.domain, share_, path_,
        base::BindOnce(&NetworkPage::Loaded, weak_.GetWeakPtr(), share_, path_));
  }
  void AdoptSession(std::unique_ptr<ConnectedSmbSession> session) {
    Stop(); device_mode_ = true; credentials_needed_ = false; credential_url_.clear();
    credentials_ = std::move(session->credentials);
    server_address_ = credentials_.location.server;
    share_ = credentials_.location.share; path_ = credentials_.location.path;
    connection_.TakeOver(*session->connection, base::BindOnce(&NetworkPage::Failed, weak_.GetWeakPtr()));
    running_ = true;
    Loaded(share_, path_, std::move(session->listing));
  }
  std::optional<SmbCredentials> Credentials() const {
    if (!connected_) return std::nullopt;
    auto credentials = credentials_; credentials.location = {server_address_, share_, path_}; return credentials;
  }
  mojom::SmbWriteAccess WriteAccess() const {
    return connected_ && !share_.empty() ? write_access_ : mojom::SmbWriteAccess::kUnknown;
  }
  void Copy() {
    if (busy_ || !connected_ || share_.empty()) return;
    std::vector<mojom::SmbEntryPtr> entries;
    for (const auto& entry : visible_entries_) if (selected_.contains(entry->name)) entries.push_back(entry.Clone());
    CopyNetworkFiles(*Credentials(), std::move(entries));
  }
  void Paste() {
    if (busy_ || !connected_ || share_.empty()) return;
    const auto url = Url();
    auto done = base::BindOnce([](base::WeakPtr<NetworkPage> page, std::string url) {
      if (page && page->Url() == url) page->Refresh();
    }, weak_.GetWeakPtr(), url);
    if (auto source = NetworkFileClipboard()) {
      CopyNetworkSelection(GetWidget(), std::move(source), {}, Credentials(), {}, std::move(done)); return;
    }
    ReadLocalFileClipboard(base::BindOnce(
        [](base::WeakPtr<NetworkPage> page, std::string url, base::OnceClosure done, std::vector<base::FilePath> paths) {
          if (!page || page->busy_ || !page->connected_ || page->Url() != url || paths.empty()) return;
          CopyNetworkSelection(page->GetWidget(), {}, std::move(paths), page->Credentials(), {}, std::move(done));
        }, weak_.GetWeakPtr(), url, std::move(done)));
  }
  int GetDragOperationsForView(views::View* sender, const gfx::Point&) override {
    return connected_ && !busy_ && !share_.empty() && NetworkFileIndex(list_, sender) ?
        ui::DragDropTypes::DRAG_COPY | (drag_modifiers_.copy() ? 0 : ui::DragDropTypes::DRAG_MOVE) : ui::DragDropTypes::DRAG_NONE;
  }
  bool CanStartDragForView(views::View*, const gfx::Point& start, const gfx::Point& point) override {
    return ExceededDragThreshold(point - start);
  }
  void WriteDragDataForView(views::View* sender, const gfx::Point&, ui::OSExchangeData* data) override {
    if (!connected_ || busy_ || share_.empty()) return;
    const auto index = NetworkFileIndex(list_, sender);
    if (!index || *index >= visible_entries_.size()) return;
    const auto& name = visible_entries_[*index]->name;
    if (!selected_.contains(name)) { selected_.clear(); selected_.insert(name); }
    anchor_ = index; cursor_ = index; UpdateSelection();
    auto source = std::make_shared<NetworkClipboard>(); source->credentials = *Credentials();
    for (const auto& entry : visible_entries_) if (selected_.contains(entry->name)) source->entries.push_back(entry.Clone());
    FinishDrag();
    drag_token_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
    auto& drag = NetworkDrag(); drag.token = drag_token_; drag.source = std::move(source);
    drag.changed = base::BindRepeating([](base::WeakPtr<NetworkPage> page, std::string url) {
      if (page && page->Url() == url) page->Refresh();
    }, weak_.GetWeakPtr(), Url());
    base::Pickle pickle; pickle.WriteString(drag_token_);
    data->SetPickledData(NetworkDragFormat(), pickle);
    if (const auto* colors = GetColorProvider()) {
      gfx::Canvas canvas(gfx::Size(260, 64), 1.f, false);
      canvas.FillRect(gfx::Rect(260, 64), colors->GetColor(ui::kColorSysSurfaceVariant));
      canvas.DrawStringRectWithFlags(base::UTF8ToUTF16(name), gfx::FontList(),
          colors->GetColor(ui::kColorSysOnSurface), gfx::Rect(12, 8, 236, 24), gfx::Canvas::NO_SUBPIXEL_RENDERING);
      canvas.DrawStringRectWithFlags(base::NumberToString16(selected_.size()) + u" " + Text(IDS_CRFILES_ITEMS),
          gfx::FontList(), colors->GetColor(ui::kColorSysOnSurface), gfx::Rect(12, 34, 236, 22), gfx::Canvas::NO_SUBPIXEL_RENDERING);
      data->provider().SetDragImage(gfx::ImageSkia::CreateFrom1xBitmap(canvas.GetBitmap()), gfx::Vector2d(16, 16));
    }
  }
  void FinishDrag() {
    if (!drag_token_.empty() && NetworkDrag().token == drag_token_) NetworkDrag() = {};
    drag_token_.clear();
  }
  bool GetDropFormats(int* formats, std::set<ui::ClipboardFormatType>* types) override {
    if (!connected_ || busy_ || share_.empty()) return false;
    *formats = ui::OSExchangeData::PICKLED_DATA; types->insert(NetworkDragFormat()); return true;
  }
  bool CanDrop(const ui::OSExchangeData& data) override {
    return connected_ && !busy_ && !share_.empty() && MatchesNetworkDrag(data);
  }
  std::optional<SmbCredentials> DropDestination(const ui::DropTargetEvent& event,
      std::optional<size_t>* target) {
    target->reset();
    if (!CanDrop(event.data())) return std::nullopt;
    auto point = event.location();
    views::View::ConvertPointToTarget(this, scroll_, &point);
    if (!scroll_->GetContentsBounds().Contains(point)) return std::nullopt;
    point = event.location();
    views::View::ConvertPointToTarget(this, list_, &point);
    if (!scroll_->GetVisibleRect().Contains(point)) return std::nullopt;
    // Headers and scrollbars are not drop targets. Empty file-grid space is.
    auto* hit = list_->GetEventHandlerForPoint(point);
    const auto index = hit ? NetworkFileIndex(list_, hit) : std::nullopt;
    auto destination = Credentials();
    if (index) {
      if (*index >= visible_entries_.size() || !visible_entries_[*index]->directory) return std::nullopt;
      destination->location.path = ChildPath(visible_entries_[*index]->name); *target = index;
    } else if (hit && hit != list_) return std::nullopt;
    if (!ValidNetworkTransferDestination(*NetworkDrag().source, *destination)) return std::nullopt;
    return destination;
  }
  int DropOperation(const ui::DropTargetEvent& event, const SmbCredentials& destination) {
    const int operation = (event.source_operations() & ui::DragDropTypes::DRAG_MOVE) &&
        CanMoveNetworkSelection(*NetworkDrag().source, destination) ?
        ui::DragDropTypes::DRAG_MOVE : ui::DragDropTypes::DRAG_COPY;
    return event.source_operations() & operation;
  }
  int OnDragUpdated(const ui::DropTargetEvent& event) override {
    std::optional<size_t> target;
    const auto destination = DropDestination(event, &target);
    const int operation = destination ? DropOperation(event, *destination) : ui::DragDropTypes::DRAG_NONE;
    if (!operation) { OnDragExited(); return operation; }
    drop_target_ = target;
    drop_text_ = Text(operation == ui::DragDropTypes::DRAG_MOVE ? IDS_CRFILES_HISTORY_MOVE : IDS_CRFILES_HISTORY_COPY) +
        u" · " + base::UTF8ToUTF16(destination->location.Url());
    UpdateSelection(); return operation;
  }
  void OnDragExited() override {
    if (drop_text_.empty() && !drop_target_) return;
    drop_text_.clear(); drop_target_.reset(); UpdateSelection();
  }
  DropCallback GetDropCallback(const ui::DropTargetEvent&) override {
    return base::BindOnce(&NetworkPage::DropFiles, weak_.GetWeakPtr(), Url(), listing_generation_);
  }
  void DropFiles(std::string url, uint64_t generation, const ui::DropTargetEvent& event,
      ui::mojom::DragOperation& operation, std::unique_ptr<ui::LayerTreeOwner>) {
    operation = ui::mojom::DragOperation::kNone; OnDragExited();
    if (Url() != url || listing_generation_ != generation) return;
    std::optional<size_t> target;
    auto destination = DropDestination(event, &target);
    const int action = destination ? DropOperation(event, *destination) : ui::DragDropTypes::DRAG_NONE;
    if (!action) return;
    const bool move = action == ui::DragDropTypes::DRAG_MOVE;
    operation = move ? ui::mojom::DragOperation::kMove : ui::mojom::DragOperation::kCopy;
    auto done = base::BindOnce([](base::RepeatingClosure source_changed, base::WeakPtr<NetworkPage> page, std::string url) {
      source_changed.Run();
      if (page && page->Url() == url) page->Refresh();
    }, NetworkDrag().changed, weak_.GetWeakPtr(), url);
    // Leave native drop dispatch before showing the modal progress dialog.
    // Retain the snapshot: OnDragDone expires the OS token before the job ends.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
        [](base::WeakPtr<NetworkPage> page, std::string url, std::shared_ptr<const NetworkClipboard> source,
           SmbCredentials destination, bool move, base::OnceClosure done) {
          if (!page || page->busy_ || !page->connected_ || page->Url() != url) return;
          if (move) MoveNetworkSelection(page->GetWidget(), std::move(source), std::move(destination), std::move(done));
          else CopyNetworkSelection(page->GetWidget(), std::move(source), {}, std::move(destination), {}, std::move(done));
        }, weak_.GetWeakPtr(), url, NetworkDrag().source, std::move(*destination), move, std::move(done)));
  }
  bool FileKey(const ui::KeyEvent& event) {
    auto* focus = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
    if (busy_ || !connected_ || !focus || (focus != this && !scroll_->Contains(focus))) return false;
    const bool primary =
#if BUILDFLAG(IS_MAC)
        event.IsCommandDown();
#else
        event.IsControlDown();
#endif
    if (primary) {
      if (event.key_code() == ui::VKEY_A) { SelectAll(); return true; }
      if (event.key_code() == ui::VKEY_C) { Copy(); return true; }
      if (event.key_code() == ui::VKEY_V) { Paste(); return true; }
      return false;
    }
    if (event.IsAltDown() || event.IsControlDown() || event.IsCommandDown()) return false;
    if (event.key_code() == ui::VKEY_DELETE && !event.is_repeat()) { DeleteSelection(); return true; }
    if (event.key_code() == ui::VKEY_F2 && selected_.size() == 1 && !share_.empty()) {
      EditEntry(Mutation::kRename, *selected_.begin()); return true;
    }
    if (visible_entries_.empty()) return false;
    const size_t last = visible_entries_.size() - 1;
    const size_t current = std::min(cursor_.value_or(0), last);
    size_t next = current; const auto columns = NetworkFileColumns(list_);
    switch (event.key_code()) {
      case ui::VKEY_RETURN:
        if (selected_.size() == 1) for (const auto& entry : visible_entries_)
          if (selected_.contains(entry->name)) { Select(entry->name, entry->directory); break; }
        return true;
      case ui::VKEY_UP: next = current >= columns ? current - columns : 0; break;
      case ui::VKEY_DOWN: next = std::min(last, current + columns); break;
      case ui::VKEY_LEFT: if (!icons_) return false; next = current ? current - 1 : 0; break;
      case ui::VKEY_RIGHT: if (!icons_) return false; next = std::min(last, current + 1); break;
      case ui::VKEY_HOME: next = 0; break;
      case ui::VKEY_END: next = last; break;
      case ui::VKEY_SPACE: SelectIndex(current, false, event); return true;
      default: return false;
    }
    if (!cursor_ && event.key_code() != ui::VKEY_END) next = 0;
    SelectIndex(next, false, event);
    const auto generation = listing_generation_;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
        [](base::WeakPtr<NetworkPage> page, uint64_t generation, size_t index) {
          if (!page || page->busy_ || page->listing_generation_ != generation || !page->IsDrawn() ||
              !page->GetWidget() || !page->GetWidget()->IsActive()) return;
          auto* focused = page->GetFocusManager() ? page->GetFocusManager()->GetFocusedView() : nullptr;
          if (focused && (focused == page.get() || page->list_->Contains(focused))) FocusNetworkFile(page->list_, index);
        }, weak_.GetWeakPtr(), generation, next));
    return true;
  }
 private:
  void VisibilityChanged(views::View* starting_from, bool is_visible) override {
    views::View::VisibilityChanged(starting_from, is_visible);
    if (!is_visible) { CloseFileMenu(); CloseConnection(); }
  }
  void ClearConnectionFields() {
    form_ = nullptr; user_ = nullptr;
    password_ = nullptr; domain_ = nullptr; connection_status_ = nullptr;
    connection_message_kind_ = ConnectionMessageKind::kInfo;
  }
  void CloseConnection(bool update_status = true) {
    const bool canceled = CancelAuthentication();
    if (password_) password_->SetText({});
    ClearConnectionFields();
    if (connection_dialog_) connection_dialog_->SetAnchorView(nullptr);
    if (connection_widget_) {
      connection_widget_->MakeCloseSynchronous({}); connection_widget_.reset();
    }
    connection_dialog_.reset();
    if (canceled && update_status) RestoreAuthenticationStatus();
  }
  void ShowCredentials() {
    auto* anchor = address_anchor_.Run();
    if (anchor) ShowConnection(anchor, base::UTF8ToUTF16(server_address_), credential_url_.empty() ? Url() : credential_url_);
  }
  void ShowConnection(views::View* anchor, const std::u16string& name, const std::string& address) {
    if (busy_ || !anchor || !GetWidget() || !IsDrawn()) return;
    CloseConnection();
    GetWidget()->LayoutRootViewIfNecessary();
    anchor->ScrollRectToVisible(anchor->GetLocalBounds());
    connection_address_ = address;
    connection_dialog_ = std::make_unique<views::BubbleDialogDelegate>(anchor, views::BubbleBorder::TOP_CENTER);
    connection_dialog_->set_shadow(views::BubbleBorder::STANDARD_SHADOW);
    connection_dialog_->set_adjust_if_offscreen(true);
    connection_dialog_->set_close_on_deactivate(true);
    connection_dialog_->SetTitle(name);
    connection_dialog_->SetShowCloseButton(true);
    connection_dialog_->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
    connection_dialog_->SetButtonLabel(ui::mojom::DialogButton::kOk, Text(IDS_CRFILES_NETWORK_CONNECT));
    connection_dialog_->SetAcceptCallbackWithClose(base::BindRepeating([](base::WeakPtr<NetworkPage> page) {
      if (page) page->QueueConnect();
      return false;
    }, weak_.GetWeakPtr()));
    auto form = std::make_unique<views::View>();
    form_ = form.get();
    form_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 10));
    user_ = Field(form_, IDS_CRFILES_NETWORK_USER);
    password_ = Field(form_, IDS_CRFILES_NETWORK_PASSWORD);
    password_->SetTextInputType(ui::TEXT_INPUT_TYPE_PASSWORD);
    domain_ = Field(form_, IDS_CRFILES_NETWORK_DOMAIN);
    connection_status_ = form_->AddChildView(std::make_unique<views::Label>());
    connection_status_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    connection_status_->SetEnabledColor(ui::kColorSysOnSurfaceVariant);
    connection_status_->SetMultiLine(true); connection_status_->SetMaximumWidth(360);
    connection_status_->SetVisible(false);
    connection_dialog_->set_fixed_width(400);
    connection_dialog_->SetInitiallyFocusedView(user_);
    connection_dialog_->SetContentsView(std::move(form));
    connection_widget_ = CreateCrFilesBubble(connection_dialog_.get(), base::BindOnce(
        [](base::WeakPtr<NetworkPage> page, views::Widget::ClosedReason) {
          if (!page) return;
          if (page->password_) page->password_->SetText({});
          page->ClearConnectionFields();
          page->connection_dialog_->SetAnchorView(nullptr);
          page->connection_widget_.reset();
          if (page->CancelAuthentication()) page->RestoreAuthenticationStatus();
        }, weak_.GetWeakPtr()));
    connection_dialog_->GetBubbleFrameView()->SetDisplayVisibleArrow(true);
    // Enabling the arrow after widget creation requires recalculating its bounds.
    connection_dialog_->SizeToContents();
    connection_widget_->Show();
  }
  void ConnectionStatus(int message, ConnectionMessageKind kind) {
    if (!connection_status_ || !connection_widget_ || connection_widget_->IsClosed()) return;
    connection_message_kind_ = kind;
    switch (kind) {
      case ConnectionMessageKind::kInfo: connection_status_->SetEnabledColor(ui::kColorSysOnSurfaceVariant); break;
      case ConnectionMessageKind::kProgress: connection_status_->SetEnabledColor(ui::kColorSysPrimary); break;
      case ConnectionMessageKind::kError: connection_status_->SetEnabledColor(ui::kColorSysError); break;
    }
    connection_status_->SetText(Text(message)); connection_status_->SetVisible(true);
    connection_dialog_->SizeToContents();
  }
  void RequestCredentialsFor(Error error) {
    if (error != Error::kAccessDenied) return;
    credentials_needed_ = true; credential_url_ = Url();
    ShowCredentials(); ConnectionStatus(IDS_CRFILES_NETWORK_ERROR, ConnectionMessageKind::kError);
  }
  void QueueConnect() {
    if (busy_ || !connection_widget_ || connection_widget_->IsClosed()) return;
    // A launch failure can close the bubble. Leave button/key dispatch before
    // connecting, and never submit a replacement bubble with a stale click.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
        [](base::WeakPtr<NetworkPage> page, base::WeakPtr<views::Widget> widget) {
          if (page && widget && page->connection_widget_.get() == widget.get()) page->Connect();
        }, weak_.GetWeakPtr(), connection_widget_->GetWeakPtr()));
  }
  views::LabelButton* Button(views::View* parent, int id, base::RepeatingClosure action) {
    auto* button = parent->AddChildView(std::make_unique<views::LabelButton>(std::move(action), Text(id)));
    button->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(8, 12)));
    return button;
  }
  views::Textfield* Field(views::View* parent, int id) {
    auto* field = parent->AddChildView(std::make_unique<views::Textfield>());
    field->SetPlaceholderText(Text(id)); field->SetAccessibleName(Text(id));
    field->SetController(this); field->SetDefaultWidthInChars(12);
    return field;
  }
  void ContentsChanged(views::Textfield* field, const std::u16string&) override {
    if (busy_ || connection_message_kind_ != ConnectionMessageKind::kError ||
        (field != user_ && field != password_ && field != domain_) ||
        !connection_status_ || !connection_widget_ || connection_widget_->IsClosed()) return;
    // Only user edits notify this controller; programmatic password clearing
    // during submission/closure must not erase a new authentication status.
    connection_message_kind_ = ConnectionMessageKind::kInfo;
    connection_status_->SetText({}); connection_status_->SetVisible(false);
    connection_status_->SetEnabledColor(ui::kColorSysOnSurfaceVariant);
    connection_dialog_->SizeToContents();
  }
  bool HandleKeyEvent(views::Textfield* field, const ui::KeyEvent& event) override {
    if (event.type() != ui::EventType::kKeyPressed || event.key_code() != ui::VKEY_RETURN || busy_ || field->IsIMEComposing()) return false;
    QueueConnect(); return true;
  }
  void KeepFocusInFileArea() {
    if (!list_ || !IsDrawn() || !GetWidget() || !GetWidget()->IsActive()) return;
    auto* focused = GetFocusManager() ? GetFocusManager()->GetFocusedView() : nullptr;
    // Disabled rows trigger automatic focus traversal, and replacing the list
    // destroys its focused row. Park only the list's current focus on this
    // stable, enabled page before either happens. Completion never steals it
    // back from an address field, popup, another pane or another window.
    if (focused && list_->Contains(focused)) RequestFocus();
  }
  void Busy(bool busy) {
    if (busy) KeepFocusInFileArea();
    busy_ = busy;
    cancel_->SetVisible(busy);
    list_->SetEnabled(!busy);
    if (form_) form_->SetEnabled(!busy);
    if (connection_widget_) connection_dialog_->SetButtonEnabled(ui::mojom::DialogButton::kOk, !busy);
    UpdateStatusPresentation();
    changed_.Run();
  }
  void Watchdog() {
    deadline_.Start(FROM_HERE, base::Seconds(40), this, &NetworkPage::Failed);
  }
  bool StartWorker() {
    if (running_) return true;
    running_ = connection_.Start(base::BindOnce(&NetworkPage::Failed, weak_.GetWeakPtr()));
    return running_;
  }
  void Home() {
    if (busy_) return;
    if (device_mode_) { if (connected_) Load({}, {}); else ShowCredentials(); return; }
    Stop(); running_ = false; share_.clear(); path_.clear(); server_address_.clear();
    ResetList();
    servers_.clear(); SetStatus(Text(IDS_CRFILES_NETWORK_SEARCHING)); Busy(false);
    if (!StartWorker()) { Failed(); return; }
    ArmDiscoveryTimeout();
    Discover(false);
    poll_.Start(FROM_HERE, base::Seconds(3), base::BindRepeating(&NetworkPage::Discover, base::Unretained(this), false));
  }
  void ArmDiscoveryTimeout() {
    if (!servers_.empty() || empty_.IsRunning()) return;
    SetStatus(Text(IDS_CRFILES_NETWORK_SEARCHING));
    empty_.Start(FROM_HERE, base::Seconds(8), base::BindOnce([](base::WeakPtr<NetworkPage> page) {
      if (page && !page->connected_ && !page->busy_ && page->servers_.empty())
        page->SetStatus(Text(IDS_CRFILES_NETWORK_NO_SERVERS));
    }, weak_.GetWeakPtr()));
  }
  void Cancel() { Stop(); ResetList(); Busy(false); SetStatus(Text(IDS_CRFILES_CANCELED)); if (!device_mode_) Home(); }
  void Failed() {
    Stop(); running_ = false;
    Busy(false); ResetList();
    SetStatus(Text(IDS_CRFILES_NETWORK_DISCONNECTED));
  }
  void Discover(bool refresh) {
    if (refresh) LOG(INFO) << "SMB page refresh requested; running=" << running_
                           << "; pending=" << discovering_;
    refresh_requested_ |= refresh;
    if (!running_ || device_mode_ || connected_ || busy_ || discovering_) return;
    discovering_ = true; Watchdog();
    connection_.service()->Discover(std::exchange(refresh_requested_, false), base::BindOnce([](base::WeakPtr<NetworkPage> page, std::vector<mojom::SmbServerPtr> servers) {
      if (!page) return;
      page->discovering_ = false;
      if (page->connected_ || page->busy_) return;
      page->deadline_.Stop();
      // Keep the device serving as the connection bubble's anchor alive.
      // The next poll applies discovery changes after the bubble closes.
      if (page->connection_widget_) return;
      // Do not replace controls while a pointer event is being dispatched.
      if (page->servers_.size() == servers.size() && std::ranges::equal(page->servers_, servers,
          [](const auto& a, const auto& b) { return a->name == b->name && a->address == b->address && a->port == b->port; })) return;
      LOG(INFO) << "SMB page received server count=" << servers.size();
      page->servers_ = std::move(servers); page->RenderServers();
    }, weak_.GetWeakPtr()));
  }
  void ResetList() {
    FinishDrag(); drop_target_.reset(); drop_text_.clear();
    CloseFileMenu(); ++listing_generation_;
    KeepFocusInFileArea();
    list_ = nullptr;
    list_ = scroll_->SetContents(std::make_unique<views::View>());
    list_->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
  }
  void SetStatus(std::u16string text) {
    status_text_ = std::move(text); UpdateStatusPresentation();
  }
  void UpdateStatusPresentation() {
    const bool shared_directory = device_mode_ && !share_.empty();
    const bool has_footer = static_cast<bool>(footer_changed_);
    auto text = drop_text_.empty() ? status_text_ : drop_text_;
    status_->SetText(text);
    status_->SetVisible(!has_footer && !text.empty());
    actions_->SetVisible(busy_ && !has_footer);
    if (!has_footer) return;
    const bool summary = (!device_mode_ || connected_) && !busy_ && text.empty();
    if (text.empty() && !busy_) {
      if (connected_) {
        text = visible_entries_.empty() ? Text(IDS_CRFILES_EMPTY) :
            base::NumberToString16(visible_entries_.size()) + u" " + Text(IDS_CRFILES_ITEMS) + u" · " +
            base::NumberToString16(selected_.size()) + u" " + Text(IDS_CRFILES_SELECTED);
        if (summary && shared_directory && !selected_.empty()) {
          uint64_t selected_bytes = 0;
          bool has_selected_file = false;
          constexpr uint64_t max_bytes = std::numeric_limits<int64_t>::max();
          for (const auto& entry : visible_entries_) {
            if (!entry->directory && selected_.contains(entry->name)) {
              has_selected_file = true;
              selected_bytes += std::min(entry->size, max_bytes - selected_bytes);
            }
          }
          // Match the local footer: count regular files only, without walking
          // folders. Saturate untrusted sizes before formatting the total.
          if (has_selected_file || selected_.size() > 1)
            text += u" · " + ui::FormatBytes(base::ByteSize(selected_bytes));
        }
      } else if (!device_mode_ && !servers_.empty())
        text = base::NumberToString16(servers_.size()) + u" " + Text(IDS_CRFILES_ITEMS);
    }
    footer_changed_.Run(NetworkFooterState{true, busy_, connected_, shared_directory, summary, std::move(text), WriteAccess()});
  }
  void OpenDevice(std::string address) {
    if (busy_) return;
    // Leave the row's input dispatch before opening a popup that takes focus.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&NetworkPage::ShowDeviceConnection, weak_.GetWeakPtr(), std::move(address)));
  }
  void ShowDeviceConnection(const std::string& address) {
    if (device_mode_ || busy_) return;
    const auto location = ParseSmbLocation("smb://" + address);
    if (!location) return;
    for (size_t i = 0; i < servers_.size(); ++i) {
      const auto& server = servers_[i];
      const auto endpoint = server->address + (server->port == 445 ? std::string() : ":" + base::NumberToString(server->port));
      if (endpoint != address) continue;
      ShowConnection(NetworkFileNameAnchor(list_, i), base::UTF8ToUTF16(server->name), location->Url());
      return;
    }
  }
  void RenderServers() {
    if (device_mode_) return;
    if (!servers_.empty()) empty_.Stop();
    KeepFocusInFileArea();
    list_ = nullptr;
    list_ = scroll_->SetContents(CreateNetworkDevicesView(servers_, icons_, icon_size_, server_icon_,
        base::BindRepeating(&NetworkPage::OpenDevice, weak_.GetWeakPtr())));
    SetStatus(servers_.empty() ? Text(empty_.IsRunning() ? IDS_CRFILES_NETWORK_SEARCHING : IDS_CRFILES_NETWORK_NO_SERVERS) : std::u16string());
    if (server_icon_.isNull() && !icon_loading_) {
      icon_loading_ = true;
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&LoadNetworkServerIcon, icon_size_), base::BindOnce(
              [](base::WeakPtr<NetworkPage> page, int edge, gfx::ImageSkia image) {
                if (!page || page->icon_size_ != edge) return;
                page->icon_loading_ = false; page->server_icon_ = std::move(image);
                if (!page->device_mode_) SetNetworkDevicesImage(page->list_, page->server_icon_);
              }, weak_.GetWeakPtr(), icon_size_));
    }
  }
  void Connect() {
    if (busy_ || !connection_widget_ || connection_widget_->IsClosed()) return;
    std::string address = connection_address_;
    base::TrimWhitespaceASCII(address, base::TRIM_ALL, &address);
    if (!base::StartsWith(address, "smb://", base::CompareCase::INSENSITIVE_ASCII)) address = "smb://" + address;
    const auto location = ParseSmbLocation(address);
    if (!location) { ConnectionStatus(IDS_CRFILES_NETWORK_ADDRESS_ERROR, ConnectionMessageKind::kError); return; }
    SmbCredentials credentials{*location, base::UTF16ToUTF8(user_->GetText()),
        base::UTF16ToUTF8(password_->GetText()), base::UTF16ToUTF8(domain_->GetText())};
    password_->SetText({});
    BeginAuthentication(std::move(credentials));
  }
  bool CancelAuthentication() {
    const bool canceled = std::exchange(authenticating_, false);
    authentication_weak_.InvalidateWeakPtrs(); authentication_deadline_.Stop();
    authentication_connection_.reset();
    return canceled;
  }
  void RestoreAuthenticationStatus() {
    Busy(false);
    if (connected_) UpdateListingStatus();
    else if (!device_mode_ && servers_.empty())
      SetStatus(Text(empty_.IsRunning() ? IDS_CRFILES_NETWORK_SEARCHING : IDS_CRFILES_NETWORK_NO_SERVERS));
    else SetStatus({});
  }
  void BeginAuthentication(SmbCredentials credentials) {
    CancelAuthentication();
    authenticating_ = true; Busy(true); deadline_.Stop();
    ConnectionStatus(IDS_CRFILES_NETWORK_CONNECTING, ConnectionMessageKind::kProgress);
    SetStatus(Text(IDS_CRFILES_NETWORK_CONNECTING));
    authentication_connection_ = std::make_unique<SmbConnection>();
    if (!authentication_connection_->Start(base::BindOnce(&NetworkPage::AuthenticationFailed,
            authentication_weak_.GetWeakPtr(), IDS_CRFILES_NETWORK_DISCONNECTED))) {
      AuthenticationFailed(IDS_CRFILES_NETWORK_DISCONNECTED); return;
    }
    authentication_deadline_.Start(FROM_HERE, base::Seconds(40), base::BindOnce(&NetworkPage::AuthenticationFailed,
        authentication_weak_.GetWeakPtr(), IDS_CRFILES_NETWORK_DISCONNECTED));
    // Keep discovery and any existing authenticated directory intact while the
    // submitted credentials are checked in a separate worker.
    const auto& location = credentials.location;
    authentication_connection_->service()->Connect(location.server, credentials.user, credentials.password, credentials.domain,
        location.share, location.path, base::BindOnce(&NetworkPage::Authenticated,
            authentication_weak_.GetWeakPtr(), credentials));
  }
  void AuthenticationFailed(int message) {
    CancelAuthentication(); RestoreAuthenticationStatus();
    ConnectionStatus(message, ConnectionMessageKind::kError);
    if (password_ && connection_widget_ && !connection_widget_->IsClosed()) password_->RequestFocus();
  }
  void Authenticated(SmbCredentials credentials, mojom::SmbListingPtr listing) {
    if (listing->error != Error::kOk) { AuthenticationFailed(ErrorText(listing->error)); return; }
    authentication_deadline_.Stop(); authenticating_ = false;
    auto session = std::make_unique<ConnectedSmbSession>();
    session->credentials = std::move(credentials);
    session->listing = std::move(listing);
    session->connection = std::move(authentication_connection_);
    const bool reuse_page = device_mode_ && session->credentials.location.server == server_address_;
    CloseConnection(); RestoreAuthenticationStatus();
    if (reuse_page) AdoptSession(std::move(session));
    else open_device_.Run(std::move(session));
  }
  void Load(std::string share, std::string path) {
    if (busy_ || !running_) return;
    CloseFileMenu(); CloseConnection();
    Busy(true); Watchdog(); SetStatus(Text(IDS_CRFILES_LOADING));
    connection_.service()->List(share, path, false, base::BindOnce(&NetworkPage::Loaded, weak_.GetWeakPtr(), share, path));
  }
  void Loaded(std::string share, std::string path, mojom::SmbListingPtr listing) {
    deadline_.Stop();
    if (listing->error != Error::kOk) {
      Busy(false); SetStatus(Text(connected_ && listing->error == Error::kAccessDenied ? IDS_CRFILES_NETWORK_ERROR : ErrorText(listing->error)));
      ConnectionStatus(ErrorText(listing->error), ConnectionMessageKind::kError);
      if (password_ && connection_widget_ && !connection_widget_->IsClosed()) password_->RequestFocus();
      pending_history_ = -1;
      credentials_needed_ = listing->error == Error::kAccessDenied;
      if (credentials_needed_) credential_url_ = SmbLocation{server_address_, share, path}.Url();
      if (device_mode_ && credentials_needed_ && !connection_widget_) {
        ShowCredentials(); ConnectionStatus(ErrorText(listing->error), ConnectionMessageKind::kError);
      }
      return;
    }
    CloseConnection();
    if (share_ != share || path_ != path) { selected_.clear(); anchor_.reset(); cursor_.reset(); }
    connected_ = true; credentials_needed_ = false; credential_url_.clear(); share_ = std::move(share); path_ = std::move(path);
    if (pending_history_ >= 0) history_index_ = std::exchange(pending_history_, -1);
    else if (history_index_ < 0 || history_[history_index_] != Url()) {
      history_.resize(history_index_ + 1); history_.push_back(Url()); history_index_ = history_.size() - 1;
    }
    write_access_ = listing->write_access;
    Busy(false);
    entries_ = std::move(listing->entries);
    listing_limited_ = listing->limited;
    RenderListing();
    UpdateListingStatus();
  }
  void UpdateListingStatus() {
    SetStatus(listing_limited_ ? Text(IDS_CRFILES_NETWORK_LIMIT) : std::u16string());
  }
  void RenderListing() {
    FinishDrag(); drop_target_.reset(); drop_text_.clear();
    CloseFileMenu();
    KeepFocusInFileArea();
    const auto generation = ++listing_generation_;
    visible_entries_.clear();
    std::ranges::sort(entries_, [this](const auto& a, const auto& b) {
      if (a->directory != b->directory) return a->directory > b->directory;
      switch (sort_) {
        case 1: return base::ToLowerASCII(a->name) > base::ToLowerASCII(b->name);
        case 2: if (a->modified != b->modified) return a->modified > b->modified; break;
        case 3: if (a->modified != b->modified) return a->modified < b->modified; break;
        case 4: if (a->size != b->size) return a->size > b->size; break;
        case 5: if (EntryType(*a) != EntryType(*b)) return EntryType(*a) < EntryType(*b); break;
        case 6: if (a->size != b->size) return a->size < b->size; break;
        case 7: if (EntryType(*a) != EntryType(*b)) return EntryType(*a) > EntryType(*b); break;
        case 8: if (a->created != b->created) return a->created < b->created; break;
        case 9: if (a->created != b->created) return a->created > b->created; break;
      }
      return base::ToLowerASCII(a->name) < base::ToLowerASCII(b->name);
    });
    for (const auto& entry : entries_) {
      // Shares are service names; hidden-file filtering starts inside a share.
      if (!share_.empty() && !show_hidden_ && entry->name.starts_with('.')) continue;
      visible_entries_.push_back(entry.Clone());
    }
    scroll_->SetHorizontalScrollBarMode(icons_ ? views::ScrollView::ScrollBarMode::kDisabled : views::ScrollView::ScrollBarMode::kEnabled);
    list_ = nullptr;
    const auto columns = share_.empty() ? NetworkColumns{false, false, false, false} : columns_;
    list_ = scroll_->SetContents(CreateNetworkFilesView(visible_entries_, icons_, icon_size_, columns, this,
        base::BindRepeating(&NetworkPage::SelectIndex, weak_.GetWeakPtr())));
    SetNetworkMarquee(list_, base::BindRepeating(&NetworkPage::Marquee, weak_.GetWeakPtr()));
    if (!share_.empty()) SetNetworkFilesDragController(list_, this,
        base::BindRepeating(&NetworkPage::FinishDrag, weak_.GetWeakPtr()));
    list_->SetEnabled(!busy_);
    std::erase_if(selected_, [this](const std::string& name) {
      return !std::ranges::any_of(visible_entries_, [&](const auto& entry) { return entry->name == name; });
    });
    anchor_.reset(); cursor_.reset(); UpdateSelection();
    std::vector<std::pair<std::string, bool>> names;
    names.reserve(visible_entries_.size());
    for (const auto& entry : visible_entries_) names.emplace_back(entry->name, entry->directory);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce([](std::vector<std::pair<std::string, bool>> names, int edge) {
          std::vector<gfx::ImageSkia> images; images.reserve(names.size());
          for (const auto& [name, directory] : names) images.push_back(LoadRemoteFileIcon(name, directory, edge));
          return images;
        }, std::move(names), icon_size_), base::BindOnce(
            [](base::WeakPtr<NetworkPage> page, uint64_t generation, std::vector<gfx::ImageSkia> images) {
              if (page && page->listing_generation_ == generation) SetNetworkFilesImages(page->list_, images);
            }, weak_.GetWeakPtr(), generation));
  }
  void UpdateSelection() {
    std::set<size_t> indices;
    for (size_t i = 0; i < visible_entries_.size(); ++i) if (selected_.contains(visible_entries_[i]->name)) indices.insert(i);
    SetNetworkFileSelection(list_, indices, drop_target_);
    UpdateStatusPresentation();
  }
  void Marquee(std::set<size_t> indices, bool additive, bool begin, bool end) {
    if (busy_) return;
    if (begin) { marquee_base_ = selected_; return; }
    if (end) { marquee_base_.clear(); return; }
    selected_ = additive ? marquee_base_ : std::set<std::string>();
    for (size_t i : indices) if (i < visible_entries_.size()) selected_.insert(visible_entries_[i]->name);
    anchor_.reset(); cursor_.reset(); UpdateSelection();
  }
  void SelectAll() {
    if (busy_ || !connected_) return;
    selected_.clear();
    for (const auto& entry : visible_entries_) selected_.insert(entry->name);
    UpdateSelection();
  }
  void SelectIndex(size_t index, bool activate, const ui::Event& event) {
    if (busy_ || index >= visible_entries_.size()) return;
#if BUILDFLAG(IS_MAC)
    const bool toggle = event.IsCommandDown();
#else
    const bool toggle = event.IsControlDown();
#endif
    const auto& entry = visible_entries_[index];
    if (event.IsShiftDown() && anchor_) {
      if (!toggle) selected_.clear();
      for (size_t i = std::min(*anchor_, index); i <= std::max(*anchor_, index); ++i) selected_.insert(visible_entries_[i]->name);
    } else {
      if (toggle) {
        if (!selected_.erase(entry->name)) selected_.insert(entry->name);
      } else { selected_.clear(); selected_.insert(entry->name); }
      anchor_ = index;
    }
    cursor_ = index; UpdateSelection();
    if (activate && !toggle && !event.IsShiftDown()) Select(entry->name, entry->directory);
  }
  void CloseFileMenu() { menu_runner_.reset(); menu_.reset(); sort_menu_.reset(); split_menu_.reset(); }
  void ShowContextMenuForViewImpl(views::View* source, const gfx::Point& point, ui::mojom::MenuSourceType type) override {
    if (busy_ || !connected_ || (menu_runner_ && menu_runner_->IsRunning())) return;
    CloseFileMenu();
    const auto index = NetworkFileIndex(list_, source);
    if (index && *index < visible_entries_.size()) {
      const auto& name = visible_entries_[*index]->name;
      if (!selected_.contains(name)) { selected_.clear(); selected_.insert(name); }
      cursor_ = index; anchor_ = index;
    } else if (type != ui::mojom::MenuSourceType::kKeyboard) selected_.clear();
    UpdateSelection();
    context_name_.clear(); context_directory_ = false;
    if (selected_.size() == 1) for (const auto& entry : visible_entries_) if (selected_.contains(entry->name)) {
      context_name_ = entry->name; context_directory_ = entry->directory;
    }
    context_url_ = Url();
    const auto generation = listing_generation_;
    ReadLocalFileClipboard(base::BindOnce(
        [](base::WeakPtr<NetworkPage> page, std::string url, uint64_t generation, gfx::Point point,
           ui::mojom::MenuSourceType type, std::vector<base::FilePath> paths) {
          if (!page || page->busy_ || !page->connected_ || page->Url() != url || page->listing_generation_ != generation || !page->IsDrawn()) return;
          page->clipboard_available_ = NetworkFileClipboard() || !paths.empty();
          page->ShowMenu(point, type);
        }, weak_.GetWeakPtr(), context_url_, generation, point, type));
  }
  void ShowMenu(gfx::Point point, ui::mojom::MenuSourceType type) {
    menu_ = std::make_unique<ui::SimpleMenuModel>(this);
    auto image = [](const gfx::VectorIcon& icon) { return ui::ImageModel::FromVectorIcon(icon, ui::kColorMenuIcon, 18); };
    auto add = [&](int id, const gfx::VectorIcon* icon = nullptr) {
      if (icon) menu_->AddItemWithIcon(id, Text(id), image(*icon));
      else menu_->AddItem(id, Text(id));
    };
    if (selected_.empty()) {
      if (!share_.empty()) { add(IDS_CRFILES_NEW_FOLDER, &vector_icons::kFolderOpenIcon); menu_->AddSeparator(ui::NORMAL_SEPARATOR); }
      sort_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
      for (size_t i = 0; i < kNetworkSortLabels.size(); ++i) {
        if (i == 7 || (share_.empty() && i > 1)) continue;
        sort_menu_->AddRadioItem(kNetworkSortLabels[i], Text(kNetworkSortLabels[i]), 1);
      }
      menu_->AddSubMenu(IDS_CRFILES_SORT_BY, Text(IDS_CRFILES_SORT_BY), sort_menu_.get());
      menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      if (!share_.empty()) add(IDS_CRFILES_PASTE, &kContentPasteIcon);
      add(IDS_CRFILES_SELECT_ALL);
      menu_->AddSeparator(ui::NORMAL_SEPARATOR); add(IDS_CRFILES_PROPERTIES, &kInfoIcon);
    } else {
      if (selected_.size() == 1) {
        add(IDS_CRFILES_OPEN, &kOpenInNewIcon);
        if (context_directory_) {
          add(IDS_CRFILES_OPEN_NEW_TAB, &kOpenInNewIcon);
          split_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
          const std::array labels = {IDS_CRFILES_SPLIT_LEFT, IDS_CRFILES_SPLIT_RIGHT, IDS_CRFILES_SPLIT_TOP, IDS_CRFILES_SPLIT_BOTTOM};
          const std::array<const gfx::VectorIcon*, 4> icons = {&kSplitSceneLeftIcon, &kSplitSceneRightIcon, &kSplitSceneUpIcon, &kSplitSceneDownIcon};
          for (size_t i = 0; i < labels.size(); ++i) split_menu_->AddItemWithIcon(labels[i], Text(labels[i]), image(*icons[i]));
          menu_->AddSubMenuWithIcon(IDS_CRFILES_OPEN_SPLIT, Text(IDS_CRFILES_OPEN_SPLIT), split_menu_.get(), image(kSplitSceneRightIcon));
        }
        menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      }
      if (!share_.empty()) {
        add(IDS_CRFILES_HISTORY_COPY, &vector_icons::kContentCopyIcon);
        menu_->AddSeparator(ui::NORMAL_SEPARATOR);
        if (selected_.size() == 1) add(IDS_CRFILES_RENAME, &kEditIcon);
        add(IDS_CRFILES_NETWORK_DELETE, &kDeleteIcon);
        menu_->AddSeparator(ui::NORMAL_SEPARATOR);
      }
      add(IDS_CRFILES_COPY_PATH, &vector_icons::kLinkIcon);
      menu_->AddSeparator(ui::NORMAL_SEPARATOR); add(IDS_CRFILES_PROPERTIES, &kInfoIcon);
    }
    menu_runner_ = std::make_unique<views::MenuRunner>(menu_.get(), views::MenuRunner::CONTEXT_MENU);
    menu_runner_->RunMenuAt(GetWidget(), nullptr, gfx::Rect(point, gfx::Size()), views::MenuAnchorPosition::kTopLeft, type);
  }
  bool IsCommandIdEnabled(int command) const override {
    if (!connected_ || busy_) return false;
    if (command == IDS_CRFILES_PASTE) return !share_.empty() && clipboard_available_;
    if (command == IDS_CRFILES_SELECT_ALL) return !visible_entries_.empty();
    return true;
  }
  bool IsCommandIdChecked(int command) const override { return command == kNetworkSortLabels[share_.empty() && sort_ > 1 ? 0 : sort_]; }
  void ExecuteCommand(int command, int) override {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
        [](base::WeakPtr<NetworkPage> page, std::string url, std::string name, bool directory, int command) {
          if (!page || page->busy_ || !page->connected_ || page->Url() != url) return;
          page->CloseFileMenu();
          auto sort = std::ranges::find(kNetworkSortLabels, command);
          if (sort != kNetworkSortLabels.end()) {
            page->sort_ = static_cast<int>(sort - kNetworkSortLabels.begin());
            page->sorted_.Run(page->sort_); page->RenderListing(); return;
          }
          switch (command) {
            case IDS_CRFILES_OPEN: page->Select(std::move(name), directory); break;
            case IDS_CRFILES_RENAME: page->EditEntry(Mutation::kRename, std::move(name)); break;
            case IDS_CRFILES_NETWORK_DELETE: page->DeleteSelection(); break;
            case IDS_CRFILES_NEW_FOLDER: page->NewFolder(); break;
            case IDS_CRFILES_HISTORY_COPY: page->Copy(); break;
            case IDS_CRFILES_PASTE: page->Paste(); break;
            case IDS_CRFILES_SELECT_ALL: page->SelectAll(); break;
            case IDS_CRFILES_COPY_PATH: page->CopyPaths(); break;
            case IDS_CRFILES_PROPERTIES: page->Properties(); break;
            case IDS_CRFILES_OPEN_NEW_TAB: page->OpenLocation(name, -1); break;
            case IDS_CRFILES_SPLIT_LEFT: page->OpenLocation(name, 0); break;
            case IDS_CRFILES_SPLIT_RIGHT: page->OpenLocation(name, 1); break;
            case IDS_CRFILES_SPLIT_TOP: page->OpenLocation(name, 2); break;
            case IDS_CRFILES_SPLIT_BOTTOM: page->OpenLocation(name, 3); break;
          }
        }, weak_.GetWeakPtr(), context_url_, context_name_, context_directory_, command));
  }
  SmbLocation EntryLocation(const std::string& name) const {
    return share_.empty() ? SmbLocation{server_address_, name, {}} : SmbLocation{server_address_, share_, ChildPath(name)};
  }
  void OpenLocation(const std::string& name, int direction) {
    auto credentials = *Credentials(); credentials.location = EntryLocation(name);
    open_location_.Run(std::move(credentials), direction);
  }
  void CopyPaths() {
    std::u16string text;
    for (const auto& entry : visible_entries_) if (selected_.contains(entry->name)) {
      if (!text.empty()) text += u"\n";
      text += base::UTF8ToUTF16(EntryLocation(entry->name).Url());
    }
    if (!text.empty()) { ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste); writer.WriteText(text); }
    NetworkFileClipboard();
  }
  void Properties() {
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog->SetTitle(Text(IDS_CRFILES_PROPERTIES)); dialog->SetModalType(ui::mojom::ModalType::kWindow);
    dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk));
    dialog->SetButtonLabel(ui::mojom::DialogButton::kOk, Text(IDS_CRFILES_CLOSE)); dialog->SetSingleButtonWidthRatio(0.182);
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
    auto add = [&](int id, const std::u16string& value) {
      auto* label = contents->AddChildView(std::make_unique<views::Label>(Text(id) + u": " + value));
      label->SetHorizontalAlignment(gfx::ALIGN_LEFT); label->SetMultiLine(true); label->SetMaximumWidth(480); label->SetSelectable(true);
    };
    if (selected_.size() == 1) {
      for (const auto& entry : visible_entries_) if (selected_.contains(entry->name)) {
        add(IDS_CRFILES_NAME, base::UTF8ToUTF16(entry->name));
        add(IDS_CRFILES_LOCATION, base::UTF8ToUTF16(EntryLocation(entry->name).Url()));
        add(IDS_CRFILES_TYPE, EntryType(*entry));
        if (!entry->directory) add(IDS_CRFILES_SIZE, ui::FormatBytes(base::ByteSize(entry->size)));
        if (!entry->modified.is_null()) add(IDS_CRFILES_LAST_MODIFIED, base::TimeFormatShortDateAndTime(entry->modified));
        if (!entry->created.is_null()) add(IDS_CRFILES_CREATED, base::TimeFormatShortDateAndTime(entry->created));
      }
    } else {
      add(IDS_CRFILES_LOCATION, base::UTF8ToUTF16(Url()));
      if (!selected_.empty()) add(IDS_CRFILES_ITEMS, base::NumberToString16(selected_.size()));
      else add(IDS_CRFILES_TYPE, Text(IDS_CRFILES_FOLDER));
    }
    dialog->SetContentsView(std::move(contents)); ShowCrFilesDialog(std::move(dialog), GetWidget());
  }
  void DeleteSelection() {
    if (busy_ || !connected_ || share_.empty() || selected_.empty()) return;
    std::vector<std::pair<std::string, bool>> entries;
    std::u16string names;
    for (const auto& entry : visible_entries_) if (selected_.contains(entry->name)) {
      entries.emplace_back(entry->name, entry->directory);
      if (entries.size() <= 8) { if (!names.empty()) names += u"\n"; names += base::UTF8ToUTF16(entry->name); }
    }
    if (entries.size() > 8) names += u"\n… (" + base::NumberToString16(entries.size()) + u")";
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog->SetTitle(Text(IDS_CRFILES_NETWORK_DELETE)); dialog->SetModalType(ui::mojom::ModalType::kWindow);
    dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) | static_cast<int>(ui::mojom::DialogButton::kCancel));
    dialog->SetButtonLabel(ui::mojom::DialogButton::kOk, Text(IDS_CRFILES_NETWORK_DELETE));
    dialog->SetButtonLabel(ui::mojom::DialogButton::kCancel, Text(IDS_CRFILES_CANCEL));
    auto message = std::make_unique<views::Label>(Text(IDS_CRFILES_NETWORK_DELETE_CONFIRM) + u"\n" + names);
    message->SetMultiLine(true); message->SetMaximumWidth(440); message->SetBorder(views::CreateEmptyBorder(gfx::Insets(16)));
    dialog->SetContentsView(std::move(message));
    dialog->SetAcceptCallback(base::BindOnce([](base::WeakPtr<NetworkPage> page, std::string url,
                                              std::vector<std::pair<std::string, bool>> entries) {
      if (!page || page->busy_ || !page->connected_ || page->Url() != url) return;
      page->deleting_ = std::move(entries); page->delete_index_ = 0; page->Busy(true);
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(&NetworkPage::DeleteNext, page));
    }, weak_.GetWeakPtr(), Url(), std::move(entries)));
    ShowCrFilesDialog(std::move(dialog), GetWidget());
  }
  void DeleteNext() {
    if (delete_index_ == deleting_.size()) { deleting_.clear(); Busy(false); Load(share_, path_); return; }
    const auto& [name, directory] = deleting_[delete_index_]; Watchdog(); SetStatus(Text(IDS_CRFILES_NETWORK_DELETE) + u" · " + base::UTF8ToUTF16(name));
    connection_.service()->Mutate(share_, ChildPath(name), directory ? Mutation::kDeleteTree : Mutation::kDeleteFile, {},
        base::BindOnce([](base::WeakPtr<NetworkPage> page, Error error) {
          if (!page) return;
          page->deadline_.Stop();
          if (error != Error::kOk) { page->deleting_.clear(); page->Mutated(error); return; }
          ++page->delete_index_; page->DeleteNext();
        }, weak_.GetWeakPtr()));
  }
  std::string ChildPath(const std::string& name) const { return path_.empty() ? name : path_ + "/" + name; }
  void NewFolder() {
    if (connected_ && !busy_ && !share_.empty() && show_new_folder_) show_new_folder_.Run();
  }
  void FolderCreated(Error error) {
    auto done = std::move(create_folder_done_);
    deadline_.Stop(); Busy(false);
    if (error == Error::kOk) Load(share_, path_);
    else UpdateListingStatus();
    // The shared form retains its input on failure. Permission failures must
    // not replace it with an authentication popup.
    if (done) std::move(done).Run(error);
  }
  void EditEntry(Mutation operation, std::string name) {
    if (busy_ || !connected_ || share_.empty()) return;
    const bool remove = operation == Mutation::kDeleteFile || operation == Mutation::kDeleteDirectory;
    const int title = remove ? IDS_CRFILES_NETWORK_DELETE : operation == Mutation::kRename ? IDS_CRFILES_RENAME : IDS_CRFILES_NEW_FOLDER;
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog->SetTitle(Text(title)); dialog->SetModalType(ui::mojom::ModalType::kWindow);
    dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) | static_cast<int>(ui::mojom::DialogButton::kCancel));
    dialog->SetButtonLabel(ui::mojom::DialogButton::kOk, Text(title));
    dialog->SetButtonLabel(ui::mojom::DialogButton::kCancel, Text(IDS_CRFILES_CANCEL));
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
    views::Textfield* input = nullptr;
    if (remove) {
      auto* message = contents->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_NETWORK_DELETE_CONFIRM) + u"\n" + base::UTF8ToUTF16(name)));
      message->SetMultiLine(true); message->SetMaximumWidth(400);
    } else {
      input = contents->AddChildView(std::make_unique<views::Textfield>());
      input->SetAccessibleName(Text(IDS_CRFILES_NAME)); input->SetText(base::UTF8ToUTF16(name));
      input->SetDefaultWidthInChars(32); dialog->SetInitiallyFocusedView(input);
    }
    auto* error = contents->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_NETWORK_OPERATION_ERROR)));
    error->SetMultiLine(true); error->SetMaximumWidth(400); error->SetVisible(false);
    dialog->SetAcceptCallbackWithClose(base::BindRepeating(
        [](base::WeakPtr<NetworkPage> page, Mutation operation, std::string name, views::Textfield* input, views::Label* error) {
          if (!page || page->busy_ || !page->connected_) return true;
          const auto target = input ? base::UTF16ToUTF8(input->GetText()) : std::string();
          if (input && (target.empty() || target == "." || target == ".." || target.size() > 1024 ||
              !base::IsStringUTF8(target) || std::ranges::any_of(target, [](unsigned char c) { return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':'; }))) {
            input->SetInvalid(true); input->RequestFocus(); error->SetVisible(true); return false;
          }
          base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
              base::BindOnce(&NetworkPage::Mutate, page, operation, std::move(name), target));
          return true;
        }, weak_.GetWeakPtr(), operation, std::move(name), base::Unretained(input), base::Unretained(error)));
    dialog->SetContentsView(std::move(contents));
    ShowCrFilesDialog(std::move(dialog), GetWidget());
  }
  void Mutate(Mutation operation, std::string name, std::string target) {
    if (busy_ || !connected_ || share_.empty()) return;
    Busy(true); Watchdog(); SetStatus(Text(IDS_CRFILES_LOADING));
    connection_.service()->Mutate(share_, ChildPath(operation == Mutation::kCreateDirectory ? target : name), operation,
        operation == Mutation::kRename ? ChildPath(target) : std::string(),
        base::BindOnce(&NetworkPage::Mutated, weak_.GetWeakPtr()));
  }
  void Mutated(Error error) {
    deadline_.Stop(); Busy(false);
    if (error != Error::kOk) {
      SetStatus(Text(error == Error::kAlreadyExists ? IDS_CRFILES_NETWORK_EXISTS : IDS_CRFILES_NETWORK_OPERATION_ERROR));
      RequestCredentialsFor(error); return;
    }
    Load(share_, path_);
  }
  void Select(std::string name, bool directory) {
    if (busy_) return;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
        base::BindOnce(&NetworkPage::Selected, weak_.GetWeakPtr(), std::move(name), directory));
  }
  void Selected(std::string name, bool directory) {
    if (busy_ || !connected_) return;
    if (directory) { Load(share_.empty() ? name : share_, share_.empty() ? "" : ChildPath(name)); return; }
    pending_name_ = std::move(name); BeginDownload({});
  }
  void BeginDownload(base::FilePath destination) {
    Busy(true); SetStatus(Text(IDS_CRFILES_NETWORK_DOWNLOADING));
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PrepareDownload, pending_name_, std::move(destination)),
        base::BindOnce(&NetworkPage::Prepared, weak_.GetWeakPtr()));
  }
  void Prepared(std::shared_ptr<DownloadFile> file) {
    if (!file->file.IsValid()) { Busy(false); SetStatus(Text(IDS_CRFILES_NETWORK_SAVE_ERROR)); return; }
    download_ = std::move(file); Watchdog();
    connection_.service()->Download(share_, path_.empty() ? pending_name_ : path_ + "/" + pending_name_,
        std::move(download_->file), base::BindOnce(&NetworkPage::Downloaded, weak_.GetWeakPtr()));
  }
  void Downloaded(mojom::SmbDownloadPtr state) {
    deadline_.Stop();
    if (state->error != Error::kOk) {
      download_.reset(); Busy(false); SetStatus(Text(ErrorText(state->error))); RequestCredentialsFor(state->error); return;
    }
    if (!state->done) {
      SetStatus(Text(IDS_CRFILES_NETWORK_DOWNLOADING) + u" " +
          base::NumberToString16(state->total ? state->bytes * 100 / state->total : 0) + u"%");
      Watchdog();
      connection_.service()->ContinueDownload(base::BindOnce(&NetworkPage::Downloaded, weak_.GetWeakPtr()));
      return;
    }
    cancel_->SetVisible(false);
    const bool open = download_->open;
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PublishDownload, std::exchange(download_, {})),
        base::BindOnce([](base::WeakPtr<NetworkPage> page, bool open, base::FilePath path) {
          if (!page) return;
          page->Busy(false);
          page->SetStatus(path.empty() ? Text(IDS_CRFILES_NETWORK_SAVE_ERROR) :
              open ? Text(IDS_CRFILES_NETWORK_LOCAL_COPY) : std::u16string());
          if (open && !path.empty()) page->open_.Run(path);
        }, weak_.GetWeakPtr(), open));
  }
  base::RepeatingCallback<void(base::FilePath)> open_;
  base::RepeatingCallback<void(std::unique_ptr<ConnectedSmbSession>)> open_device_;
  base::RepeatingClosure changed_;
  base::RepeatingCallback<views::View*()> address_anchor_;
  base::RepeatingCallback<void(SmbCredentials, int)> open_location_;
  base::RepeatingCallback<void(int)> sorted_;
  base::RepeatingCallback<void(NetworkFooterState)> footer_changed_;
  base::RepeatingClosure show_new_folder_;
  base::OnceCallback<void(Error)> create_folder_done_;
  std::u16string status_text_;
  std::u16string drop_text_;
  std::optional<size_t> drop_target_;
  std::string drag_token_;
  NetworkDragModifiers drag_modifiers_;
  int bottom_inset_ = 0;
  SmbCredentials credentials_;
  mojom::SmbWriteAccess write_access_ = mojom::SmbWriteAccess::kUnknown;
  std::set<std::string> selected_, marquee_base_;
  std::optional<size_t> anchor_, cursor_;
  int sort_ = 2;
  bool clipboard_available_ = false;
  std::vector<std::pair<std::string, bool>> deleting_;
  size_t delete_index_ = 0;
  std::unique_ptr<ui::SimpleMenuModel> sort_menu_, split_menu_;
  bool device_mode_ = false, credentials_needed_ = false;
  bool icons_ = true, icon_loading_ = false;
  bool show_hidden_ = false, listing_limited_ = false;
  NetworkColumns columns_;
  int icon_size_ = 32;
  gfx::ImageSkia server_icon_;
  std::string connection_address_, credential_url_;
  std::vector<std::string> history_;
  int history_index_ = -1, pending_history_ = -1;
  SmbConnection connection_;
  bool running_ = false, connected_ = false, busy_ = false, discovering_ = false, refresh_requested_ = false;
  std::string server_address_, share_, path_, pending_name_;
  std::vector<mojom::SmbServerPtr> servers_;
  std::vector<mojom::SmbEntryPtr> entries_;
  std::vector<mojom::SmbEntryPtr> visible_entries_;
  uint64_t listing_generation_ = 0;
  std::string context_name_, context_url_;
  bool context_directory_ = false;
  std::unique_ptr<ui::SimpleMenuModel> menu_;
  std::unique_ptr<views::MenuRunner> menu_runner_;
  std::shared_ptr<DownloadFile> download_;
  std::unique_ptr<SmbConnection> authentication_connection_;
  bool authenticating_ = false;
  base::OneShotTimer authentication_deadline_;
  std::unique_ptr<views::BubbleDialogDelegate> connection_dialog_;
  std::unique_ptr<views::Widget> connection_widget_;
  raw_ptr<views::View> form_ = nullptr;
  raw_ptr<views::View> list_;
  raw_ptr<views::View> actions_;
  raw_ptr<views::ScrollView> scroll_;
  raw_ptr<views::Textfield> user_ = nullptr, password_ = nullptr, domain_ = nullptr;
  raw_ptr<views::Label> connection_status_ = nullptr;
  ConnectionMessageKind connection_message_kind_ = ConnectionMessageKind::kInfo;
  raw_ptr<views::Label> status_;
  raw_ptr<views::LabelButton> cancel_;
  base::RepeatingTimer poll_;
  base::OneShotTimer deadline_, empty_;
  base::WeakPtrFactory<NetworkPage> authentication_weak_{this};
  base::WeakPtrFactory<NetworkPage> weak_{this};
};
BEGIN_METADATA(NetworkPage)
END_METADATA
}
std::unique_ptr<views::View> CreateNetworkPage(base::RepeatingCallback<void(base::FilePath)> open,
    base::RepeatingCallback<void(std::unique_ptr<ConnectedSmbSession>)> open_device, base::RepeatingClosure changed,
    base::RepeatingCallback<views::View*()> address_anchor,
    base::RepeatingCallback<void(SmbCredentials, int)> open_location, base::RepeatingCallback<void(int)> sorted) {
  return std::make_unique<NetworkPage>(std::move(open), std::move(open_device), std::move(changed), std::move(address_anchor), std::move(open_location), std::move(sorted));
}
void SetNetworkPageLayout(views::View* page, bool icons, int icon_size, bool show_hidden, NetworkColumns columns, int sort) {
  static_cast<NetworkPage*>(page)->SetLayout(icons, icon_size, show_hidden, columns, sort);
}
bool NetworkPageKey(views::View* page, const ui::KeyEvent& event) { return static_cast<NetworkPage*>(page)->FileKey(event); }
void CopyNetworkPageSelection(views::View* page) { static_cast<NetworkPage*>(page)->Copy(); }
void PasteNetworkPage(views::View* page) { static_cast<NetworkPage*>(page)->Paste(); }
void SetNetworkPageFooter(views::View* page, base::RepeatingCallback<void(NetworkFooterState)> changed) { static_cast<NetworkPage*>(page)->SetFooter(std::move(changed)); }
void SetNetworkPageBottomInset(views::View* page, int height) { static_cast<NetworkPage*>(page)->SetBottomInset(height); }
void SetNetworkPageNewFolderHandler(views::View* page, base::RepeatingClosure show) { static_cast<NetworkPage*>(page)->SetNewFolderHandler(std::move(show)); }
void CreateNetworkFolder(views::View* page, std::u16string name, base::OnceCallback<void(mojom::SmbError)> done) {
  static_cast<NetworkPage*>(page)->CreateFolder(std::move(name), std::move(done));
}
void CancelNetworkPageOperation(views::View* page) { static_cast<NetworkPage*>(page)->CancelOperation(); }
std::optional<SmbCredentials> NetworkPageCredentials(views::View* page) { return static_cast<NetworkPage*>(page)->Credentials(); }
mojom::SmbWriteAccess NetworkPageWriteAccess(views::View* page) { return static_cast<NetworkPage*>(page)->WriteAccess(); }
void AdoptNetworkSession(views::View* page, std::unique_ptr<ConnectedSmbSession> session) { static_cast<NetworkPage*>(page)->AdoptSession(std::move(session)); }
void ConnectNetworkPage(views::View* page, SmbCredentials credentials) { static_cast<NetworkPage*>(page)->StartConnection(std::move(credentials)); }
bool NavigateNetworkPage(views::View* page, const std::string& address) { return static_cast<NetworkPage*>(page)->Navigate(address); }
std::string NetworkPageUrl(views::View* page) { return static_cast<NetworkPage*>(page)->Url(); }
bool NetworkPageCanGo(views::View* page, int delta) { return static_cast<NetworkPage*>(page)->CanGo(delta); }
void NetworkPageGo(views::View* page, int delta) { static_cast<NetworkPage*>(page)->Go(delta); }
void PromptNetworkPage(views::View* page, const std::string& address) { static_cast<NetworkPage*>(page)->Prompt(address); }
void RefreshNetworkPage(views::View* page) { static_cast<NetworkPage*>(page)->Refresh(); }
void StopNetworkPage(views::View* page) { static_cast<NetworkPage*>(page)->Stop(); }
}
