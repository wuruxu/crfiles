// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_network_transfer.h"
#include <algorithm>
#include <deque>
#include <set>
#include <utility>
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#else
#include <sys/stat.h>
#endif
#include "crFiles/crfiles_dialog.h"
#include "crFiles/crfiles_file_operations.h"
#include "crFiles/grit/crfiles_strings.h"
#if BUILDFLAG(IS_MAC)
#include "crFiles/crfiles_mac.h"
#endif
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/file_info.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/window/dialog_delegate.h"
namespace crfiles {
namespace {
using Error = mojom::SmbError;
struct ClipboardState {
  std::shared_ptr<NetworkClipboard> files;
  std::optional<ui::ClipboardSequenceNumberToken> sequence;
};
ClipboardState& Clipboard() { static base::NoDestructor<ClipboardState> state; return *state; }
std::u16string Text(int id) { return l10n_util::GetStringUTF16(id); }
std::string Join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }
bool ValidName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.size() <= 1024 && base::IsStringUTF8(name) &&
      !std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':'; });
}
struct CopyItem { base::FilePath local; std::string relative; bool directory; };
struct CopyPlan { Error error = Error::kOk; std::vector<CopyItem> items; };
bool RegularLocalEntry(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  const auto attributes = GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  struct stat info;
  return lstat(path.value().c_str(), &info) == 0 && (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode));
#endif
}
CopyPlan LocalPlan(const std::vector<base::FilePath>& roots) {
  CopyPlan plan;
  std::deque<CopyItem> pending;
  std::set<std::string> names;
  for (const auto& path : roots) {
    const auto name = path.BaseName().AsUTF8Unsafe();
    if (!path.IsAbsolute() || !ValidName(name) || !names.insert(name).second) { plan.error = Error::kInvalidAddress; return plan; }
    pending.push_back({path, name, false});
  }
  while (!pending.empty()) {
    auto item = std::move(pending.front()); pending.pop_front();
    base::File::Info info;
    if (!RegularLocalEntry(item.local) || !base::GetFileInfo(item.local, &info) || info.is_symbolic_link) { plan.error = Error::kLocalFile; break; }
    if (plan.items.size() >= 100000 || item.relative.size() > 4096 ||
        (!info.is_directory && (info.size < 0 || info.size > int64_t{2} * 1024 * 1024 * 1024))) { plan.error = Error::kTooLarge; break; }
    item.directory = info.is_directory;
    plan.items.push_back(item);
    if (!item.directory) continue;
    base::FileEnumerator files(item.local, false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES | base::FileEnumerator::SHOW_SYM_LINKS,
        base::FilePath::StringType(), base::FileEnumerator::FolderSearchPolicy::ALL,
        base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (auto path = files.Next(); !path.empty(); path = files.Next()) {
      auto name = path.BaseName().AsUTF8Unsafe();
      if (!ValidName(name)) { plan.error = Error::kInvalidAddress; return plan; }
      if (pending.size() + plan.items.size() >= 100000) { plan.error = Error::kTooLarge; return plan; }
      pending.push_back({path, Join(item.relative, name), false});
    }
    if (files.GetError() != base::File::FILE_OK) { plan.error = Error::kLocalFile; break; }
  }
  return plan;
}
struct CopyCache {
  base::ScopedTempDir directory;
  ~CopyCache() {
    if (directory.IsValid()) base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
        base::BindOnce([](base::FilePath path) { base::DeletePathRecursively(path); }, directory.Take()));
  }
};
class CopyJob : public std::enable_shared_from_this<CopyJob> {
 public:
  CopyJob(std::shared_ptr<const NetworkClipboard> source, std::vector<base::FilePath> paths,
      std::optional<SmbCredentials> destination, base::FilePath local, base::OnceClosure changed, bool move = false)
      : source_(std::move(source)), roots_(std::move(paths)), destination_(std::move(destination)),
        local_destination_(std::move(local)), changed_(std::move(changed)), move_(move) {}
  ~CopyJob() { source_worker_.Stop(); destination_worker_.Stop(); }
  void Show(views::Widget* parent) {
    auto dialog = std::make_unique<views::DialogDelegate>();
    dialog_ = dialog.get();
    dialog->SetTitle(Text(move_ ? IDS_CRFILES_HISTORY_MOVE : IDS_CRFILES_HISTORY_COPY)); dialog->SetModalType(ui::mojom::ModalType::kWindow);
    dialog->SetButtons(static_cast<int>(ui::mojom::DialogButton::kCancel));
    dialog->SetButtonLabel(ui::mojom::DialogButton::kCancel, Text(IDS_CRFILES_CANCEL));
    dialog->SetSingleButtonWidthRatio(0.182);
    auto contents = std::make_unique<views::View>();
    contents->SetLayoutManager(std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));
    label_ = contents->AddChildView(std::make_unique<views::Label>(Text(IDS_CRFILES_LOADING)));
    label_->SetMultiLine(true); label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    label_->SetMaximumWidth(480); label_->SetPreferredSize(gfx::Size(480, 100));
    dialog->SetContentsView(std::move(contents));
    dialog->SetCancelCallback(base::BindOnce([](std::shared_ptr<CopyJob> job) { job->Cancel(); }, shared_from_this()));
    dialog->SetCloseCallback(base::BindOnce([](std::shared_ptr<CopyJob> job) { job->Cancel(); }, shared_from_this()));
    ShowCrFilesDialog(std::move(dialog), parent);
    widget_ = dialog_->GetWidget()->GetWeakPtr();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(&CopyJob::Start, weak_.GetWeakPtr()));
  }
 private:
  void Cancel() {
    if (finished_) return;
    finished_ = true; timer_.Stop(); weak_.InvalidateWeakPtrs();
    source_worker_.Stop(); destination_worker_.Stop();
    if (changed_) std::move(changed_).Run();
  }
  void Watch() { timer_.Start(FROM_HERE, base::Seconds(60), base::BindOnce(&CopyJob::Finish, weak_.GetWeakPtr(), Error::kFailed)); }
  void Start() {
    if (source_ && destination_ && !ValidNetworkTransferDestination(*source_, *destination_)) {
      Finish(Error::kInvalidAddress); return;
    }
    if (move_) {
      if (!source_ || !destination_ || !CanMoveNetworkSelection(*source_, *destination_)) {
        Finish(Error::kInvalidAddress); return;
      }
      for (const auto& entry : source_->entries) items_.push_back({{}, entry->name, entry->directory});
      ConnectDestination(); return;
    }
    if (source_) {
      if (source_->credentials.location.share.empty() || source_->entries.empty()) { Finish(Error::kInvalidAddress); return; }
      cache_ = std::make_shared<CopyCache>();
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
          base::BindOnce([](std::shared_ptr<CopyCache> cache) { return cache->directory.CreateUniqueTempDir(); }, cache_),
          base::BindOnce(&CopyJob::CacheReady, weak_.GetWeakPtr()));
    } else {
      if (roots_.empty() || !destination_) { Finish(Error::kInvalidAddress); return; }
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
          base::BindOnce(&LocalPlan, roots_), base::BindOnce(&CopyJob::Planned, weak_.GetWeakPtr()));
    }
  }
  void CacheReady(bool ready) {
    if (!ready) { Finish(Error::kLocalFile); return; }
    if (!source_worker_.Start(base::BindOnce(&CopyJob::Finish, weak_.GetWeakPtr(), Error::kFailed))) { Finish(Error::kFailed); return; }
    const auto& c = source_->credentials; Watch();
    source_worker_.service()->Connect(c.location.server, c.user, c.password, c.domain, c.location.share, c.location.path,
        base::BindOnce(&CopyJob::SourceConnected, weak_.GetWeakPtr()));
  }
  void SourceConnected(mojom::SmbListingPtr listing) {
    timer_.Stop();
    if (listing->error != Error::kOk) { Finish(listing->error); return; }
    std::set<std::string> names;
    for (const auto& entry : source_->entries) {
      if (!ValidName(entry->name) || !names.insert(entry->name).second) { Finish(Error::kInvalidAddress); return; }
      items_.push_back({cache_->directory.GetPath().Append(base::FilePath::FromUTF8Unsafe(entry->name)), entry->name, entry->directory});
      roots_.push_back(items_.back().local);
      if (entry->directory) folders_.push_back(entry->name);
    }
    Enumerate();
  }
  void Enumerate() {
    if (folders_.empty()) { index_ = 0; DownloadNext(); return; }
    auto folder = std::move(folders_.front()); folders_.pop_front();
    const auto& location = source_->credentials.location; Watch();
    source_worker_.service()->List(location.share, Join(location.path, folder), true,
        base::BindOnce(&CopyJob::Enumerated, weak_.GetWeakPtr(), folder));
  }
  void Enumerated(std::string folder, mojom::SmbListingPtr listing) {
    timer_.Stop();
    if (listing->error != Error::kOk || listing->limited) { Finish(listing->error == Error::kOk ? Error::kTooLarge : listing->error); return; }
    std::set<std::string> names;
    for (const auto& entry : listing->entries) {
      const auto relative = Join(folder, entry->name);
      if (!ValidName(entry->name) || !names.insert(entry->name).second) { Finish(Error::kInvalidAddress); return; }
      if (items_.size() >= 100000 || relative.size() > 4096 || (!entry->directory && entry->size > uint64_t{2} * 1024 * 1024 * 1024)) { Finish(Error::kTooLarge); return; }
      items_.push_back({cache_->directory.GetPath().Append(base::FilePath::FromUTF8Unsafe(relative)), relative, entry->directory});
      if (entry->directory) folders_.push_back(relative);
    }
    Enumerate();
  }
  void Progress(uint64_t bytes = 0, uint64_t total = 0) {
    label_->SetText(Text(move_ ? IDS_CRFILES_HISTORY_MOVE : IDS_CRFILES_HISTORY_COPY) + u" " + base::NumberToString16(index_ + 1) + u" / " +
        base::NumberToString16(items_.size()) + u"\n" + base::UTF8ToUTF16(items_[index_].relative) +
        (total ? u"\n" + base::NumberToString16(bytes * 100 / total) + u"%" : std::u16string()));
  }
  void DownloadNext() {
    if (index_ == items_.size()) {
      source_worker_.Stop();
      if (destination_) { index_ = 0; ConnectDestination(); }
      else Publish();
      return;
    }
    Progress(); const auto item = items_[index_];
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
        base::BindOnce([](std::shared_ptr<CopyCache> cache, CopyItem item) {
          // Exclusive creation rejects aliases (including case collisions).
          if (item.directory) return std::pair<bool, base::File>(!base::PathExists(item.local) && base::CreateDirectory(item.local), base::File());
          uint32_t flags = base::File::FLAG_CREATE | base::File::FLAG_WRITE;
#if BUILDFLAG(IS_WIN)
          flags |= base::File::FLAG_WIN_SHARE_DELETE;
#endif
          return std::pair<bool, base::File>(true, base::File(item.local, flags));
        }, cache_, item), base::BindOnce(&CopyJob::DownloadPrepared, weak_.GetWeakPtr()));
  }
  void DownloadPrepared(std::pair<bool, base::File> result) {
    if (!result.first) { Finish(Error::kLocalFile); return; }
    if (items_[index_].directory) { ++index_; DownloadNext(); return; }
    if (!result.second.IsValid()) { Finish(Error::kLocalFile); return; }
    const auto& location = source_->credentials.location; Watch();
    source_worker_.service()->Download(location.share, Join(location.path, items_[index_].relative), std::move(result.second),
        base::BindOnce(&CopyJob::Downloaded, weak_.GetWeakPtr()));
  }
  void Downloaded(mojom::SmbDownloadPtr state) {
    timer_.Stop();
    if (state->error != Error::kOk) { Finish(state->error); return; }
    if (state->done) { ++index_; DownloadNext(); return; }
    Progress(state->bytes, state->total); Watch();
    source_worker_.service()->ContinueDownload(base::BindOnce(&CopyJob::Downloaded, weak_.GetWeakPtr()));
  }
  void Planned(CopyPlan plan) {
    if (plan.error != Error::kOk) { Finish(plan.error); return; }
    items_ = std::move(plan.items); ConnectDestination();
  }
  void ConnectDestination() {
    if (!destination_ || destination_->location.share.empty()) { Finish(Error::kInvalidAddress); return; }
    if (!destination_worker_.Start(base::BindOnce(&CopyJob::Finish, weak_.GetWeakPtr(), Error::kFailed))) { Finish(Error::kFailed); return; }
    const auto& c = *destination_; Watch();
    destination_worker_.service()->Connect(c.location.server, c.user, c.password, c.domain, c.location.share, c.location.path,
        base::BindOnce(&CopyJob::DestinationConnected, weak_.GetWeakPtr()));
  }
  void DestinationConnected(mojom::SmbListingPtr listing) {
    timer_.Stop();
    if (listing->error != Error::kOk) { Finish(listing->error); return; }
    if (move_) MoveNext();
    else UploadNext();
  }
  void MoveNext() {
    if (index_ == items_.size()) { Finish(Error::kOk); return; }
    Progress(); Watch();
    const auto& from = source_->credentials.location;
    const auto& to = destination_->location;
    destination_worker_.service()->Mutate(to.share, Join(from.path, items_[index_].relative),
        mojom::SmbMutation::kRename, Join(to.path, items_[index_].relative),
        base::BindOnce(&CopyJob::Moved, weak_.GetWeakPtr()));
  }
  void Moved(Error error) {
    timer_.Stop();
    if (error != Error::kOk) { Finish(error); return; }
    ++index_; MoveNext();
  }
  void UploadNext() {
    if (index_ == items_.size()) { Finish(Error::kOk); return; }
    Progress(); const auto& item = items_[index_]; const auto& location = destination_->location;
    if (item.directory) {
      Watch();
      destination_worker_.service()->Mutate(location.share, Join(location.path, item.relative), mojom::SmbMutation::kCreateDirectory, {},
          base::BindOnce(&CopyJob::Created, weak_.GetWeakPtr()));
    } else {
      base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
          base::BindOnce([](std::shared_ptr<CopyCache> cache, base::FilePath path) {
            if (!RegularLocalEntry(path)) return base::File();
            uint32_t flags = base::File::FLAG_OPEN | base::File::FLAG_READ;
#if BUILDFLAG(IS_WIN)
            flags |= base::File::FLAG_WIN_SHARE_DELETE;
#endif
            return base::File(path, flags);
          }, cache_, item.local), base::BindOnce(&CopyJob::UploadPrepared, weak_.GetWeakPtr()));
    }
  }
  void Created(Error error) {
    timer_.Stop();
    if (error != Error::kOk) { Finish(error); return; }
    ++index_; UploadNext();
  }
  void UploadPrepared(base::File file) {
    if (!file.IsValid()) { Finish(Error::kLocalFile); return; }
    const auto& location = destination_->location; Watch();
    destination_worker_.service()->Upload(location.share, Join(location.path, items_[index_].relative), std::move(file),
        base::BindOnce(&CopyJob::Uploaded, weak_.GetWeakPtr()));
  }
  void Uploaded(mojom::SmbDownloadPtr state) {
    timer_.Stop();
    if (state->error != Error::kOk) { Finish(state->error); return; }
    if (state->done) { ++index_; UploadNext(); return; }
    Progress(state->bytes, state->total); Watch();
    destination_worker_.service()->ContinueUpload(base::BindOnce(&CopyJob::Uploaded, weak_.GetWeakPtr()));
  }
  void Publish() {
    // The existing local transfer implementation publishes without replacement.
    // Its final filesystem operation is not interruptible.
    dialog_->SetButtonEnabled(ui::mojom::DialogButton::kCancel, false);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
        base::BindOnce([](std::shared_ptr<CopyCache> cache, std::vector<base::FilePath> roots, base::FilePath destination) {
          for (const auto& root : roots) {
            const auto target = destination.Append(root.BaseName());
            if (base::PathExists(target) || base::IsLink(target)) return Error::kAlreadyExists;
          }
          const auto result = TransferLocalFiles(std::move(roots), std::move(destination), false, "smb-copy");
          return result.failed ? Error::kLocalFile : Error::kOk;
        }, cache_, roots_, local_destination_), base::BindOnce([](base::WeakPtr<CopyJob> job, Error error) {
          if (job) job->Finish(error);
        }, weak_.GetWeakPtr()));
  }
  void Finish(Error error) {
    if (finished_) return;
    finished_ = true; timer_.Stop(); source_worker_.Stop(); destination_worker_.Stop();
    weak_.InvalidateWeakPtrs();
    if (changed_) std::move(changed_).Run();
    if (error == Error::kOk) { if (widget_) widget_->Close(); return; }
    const int message = error == Error::kAlreadyExists ? IDS_CRFILES_NETWORK_EXISTS :
        error == Error::kAccessDenied ? IDS_CRFILES_NETWORK_ERROR :
        error == Error::kTooLarge ? IDS_CRFILES_NETWORK_TOO_LARGE : IDS_CRFILES_NETWORK_OPERATION_ERROR;
    label_->SetText(Text(message) + (index_ < items_.size() ? u"\n" + base::UTF8ToUTF16(items_[index_].relative) : std::u16string()));
    dialog_->SetButtonEnabled(ui::mojom::DialogButton::kCancel, true);
    dialog_->SetButtonLabel(ui::mojom::DialogButton::kCancel, Text(IDS_CRFILES_CLOSE));
  }
  std::shared_ptr<const NetworkClipboard> source_;
  std::vector<base::FilePath> roots_;
  std::optional<SmbCredentials> destination_;
  base::FilePath local_destination_;
  base::OnceClosure changed_;
  std::shared_ptr<CopyCache> cache_;
  std::vector<CopyItem> items_;
  std::deque<std::string> folders_;
  size_t index_ = 0;
  bool finished_ = false;
  const bool move_;
  SmbConnection source_worker_, destination_worker_;
  raw_ptr<views::DialogDelegate> dialog_ = nullptr;
  raw_ptr<views::Label> label_ = nullptr;
  base::WeakPtr<views::Widget> widget_;
  base::OneShotTimer timer_;
  base::WeakPtrFactory<CopyJob> weak_{this};
};
}
bool CanMoveNetworkSelection(const NetworkClipboard& source, const SmbCredentials& destination) {
  const auto& from = source.credentials;
  return !from.location.share.empty() &&
      base::EqualsCaseInsensitiveASCII(from.location.server, destination.location.server) &&
      base::EqualsCaseInsensitiveASCII(from.location.share, destination.location.share) &&
      from.user == destination.user && from.domain == destination.domain && from.password == destination.password;
}
bool ValidNetworkTransferDestination(const NetworkClipboard& source, const SmbCredentials& destination) {
  const auto& from = source.credentials.location;
  const auto& to = destination.location;
  if (source.entries.empty() || source.entries.size() > 100000 || from.share.empty() || to.share.empty() ||
      !ParseSmbLocation(from.Url()) || !ParseSmbLocation(to.Url())) return false;
  const bool same_share = base::EqualsCaseInsensitiveASCII(from.server, to.server) &&
      base::EqualsCaseInsensitiveASCII(from.share, to.share);
  if (same_share && base::EqualsCaseInsensitiveASCII(from.path, to.path)) return false;
  std::set<std::string> names;
  for (const auto& entry : source.entries) {
    if (!entry || !ValidName(entry->name) || !names.insert(base::ToLowerASCII(entry->name)).second) return false;
    const auto path = Join(from.path, entry->name);
    if (path.size() > 4096 || Join(to.path, entry->name).size() > 4096) return false;
    if (same_share && entry->directory && (base::EqualsCaseInsensitiveASCII(path, to.path) ||
        base::StartsWith(to.path, path + "/", base::CompareCase::INSENSITIVE_ASCII))) return false;
  }
  return true;
}
void MoveNetworkSelection(views::Widget* parent, std::shared_ptr<const NetworkClipboard> source,
    SmbCredentials destination, base::OnceClosure changed) {
  std::make_shared<CopyJob>(std::move(source), std::vector<base::FilePath>(), std::move(destination),
      base::FilePath(), std::move(changed), true)->Show(parent);
}
void CopyNetworkFiles(SmbCredentials credentials, std::vector<mojom::SmbEntryPtr> entries) {
  if (entries.empty()) return;
  std::u16string urls;
  for (const auto& entry : entries) {
    auto location = credentials.location; location.path = Join(location.path, entry->name);
    if (!urls.empty()) urls += u"\n";
    urls += base::UTF8ToUTF16(location.Url());
  }
  { ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste); writer.WriteText(urls); }
  auto& state = Clipboard(); state.files = std::make_shared<NetworkClipboard>();
  state.files->credentials = std::move(credentials); state.files->entries = std::move(entries);
  state.sequence = ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(ui::ClipboardBuffer::kCopyPaste);
}
std::shared_ptr<const NetworkClipboard> NetworkFileClipboard() {
  auto& state = Clipboard();
  if (!state.sequence || *state.sequence != ui::Clipboard::GetForCurrentThread()->GetSequenceNumber(ui::ClipboardBuffer::kCopyPaste)) {
    state.files.reset(); state.sequence.reset();
  }
  return state.files;
}
void ReadLocalFileClipboard(base::OnceCallback<void(std::vector<base::FilePath>)> callback) {
#if BUILDFLAG(IS_MAC)
  // Preserve the security-scoped access used by the ordinary file clipboard.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce(std::move(callback), ReadFileClipboardMac()));
#else
  ui::Clipboard::GetForCurrentThread()->ReadFilenames(ui::ClipboardBuffer::kCopyPaste, std::nullopt,
      base::BindOnce([](base::OnceCallback<void(std::vector<base::FilePath>)> callback, std::vector<ui::FileInfo> files) {
        std::vector<base::FilePath> paths;
        for (const auto& file : files) if (file.path.IsAbsolute()) paths.push_back(file.path);
        std::move(callback).Run(std::move(paths));
      }, std::move(callback)));
#endif
}
void CopyNetworkSelection(views::Widget* parent, std::shared_ptr<const NetworkClipboard> remote_source,
    std::vector<base::FilePath> local_sources, std::optional<SmbCredentials> remote_destination,
    base::FilePath local_destination, base::OnceClosure changed) {
  std::make_shared<CopyJob>(std::move(remote_source), std::move(local_sources), std::move(remote_destination),
      std::move(local_destination), std::move(changed))->Show(parent);
}
}
