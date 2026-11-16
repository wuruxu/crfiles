// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_network.h"
#include "crFiles/crfiles_network_discovery.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <utility>
#include <string_view>
#include "base/check.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/platform/platform_channel.h"
#include "mojo/public/cpp/system/invitation.h"
#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#else
#include <poll.h>
#endif
#include "smb2/smb2.h"
#include "smb2/smb2-errors.h"
#include "smb2/libsmb2.h"
#include "smb2/libsmb2-raw.h"
#include "smb2/libsmb2-dcerpc-srvsvc.h"

namespace crfiles {
namespace {
using Error = mojom::SmbError;
constexpr size_t kMaxEntries = 1000;
constexpr uint64_t kMaxDownload = uint64_t{2} * 1024 * 1024 * 1024;
base::Time SmbTime(uint64_t seconds, uint64_t nanoseconds) {
  // Missing dates and out-of-range server values must not appear as real dates.
  // Limit calendar dates to year 9999 before converting unsigned values.
  if (!seconds || seconds > 253402300799ULL || nanoseconds >= 1000000000) return {};
  return base::Time::UnixEpoch() + base::Seconds(static_cast<int64_t>(seconds)) +
      base::Microseconds(static_cast<int64_t>(nanoseconds / 1000));
}
bool Name(const std::string& name) {
  return !name.empty() && name.size() <= 1024 && name != "." && name != ".." &&
      base::IsStringUTF8(name) && !std::ranges::any_of(name, [](unsigned char c) {
        return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':';
      });
}
bool Path(const std::string& path) {
  if (path.size() > 4096 || path.starts_with('/') || path.ends_with('/')) return false;
  for (const auto& part : base::SplitString(path, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL))
    if (!Name(part)) return false;
  return true;
}
Error Status(int error) {
  return error == 0 ? Error::kOk : error == -EEXIST ? Error::kAlreadyExists :
      error == -EACCES || error == -EPERM || error == -EROFS ? Error::kAccessDenied : Error::kFailed;
}
mojom::SmbListingPtr Failed(Error error = Error::kFailed) {
  auto result = mojom::SmbListing::New(); result->error = error; return result;
}

// All library calls and output writes run on one blocking sequence in this
// child. The IO sequence remains free to service discovery and pipe teardown.
class Session {
 public:
  ~Session() { Reset(); }
  mojom::SmbListingPtr Connect(std::string server, std::string user, std::string password,
                             std::string domain, std::string share, std::string path) {
    Reset();
    if (server.empty() || server.size() > 300 || user.size() > 256 || password.size() > 1024 ||
        domain.size() > 256 || user.find('\0') != std::string::npos ||
        password.find('\0') != std::string::npos || domain.find('\0') != std::string::npos ||
        std::ranges::any_of(server, [](unsigned char c) {
          return !base::IsAsciiAlphaNumeric(c) && c != '.' && c != '-' && c != '_' &&
              c != ':' && c != '[' && c != ']' && c != '%';
        })) return Failed(Error::kInvalidAddress);
    server_ = std::move(server); user_ = user.empty() ? "guest" : std::move(user);
    password_ = std::move(password); domain_ = std::move(domain);
    return List(std::move(share), std::move(path), false);
  }
  mojom::SmbListingPtr List(std::string share, std::string path, bool complete) {
    if ((!share.empty() && !Name(share)) || (!path.empty() && (!Path(path) || share.empty())) || server_.empty())
      return Failed(Error::kInvalidAddress);
    const auto status = Mount(share.empty() ? "IPC$" : share);
    if (status != Error::kOk) return Failed(status);
    if (share.empty()) return Shares();
    auto* directory = smb2_opendir(context_, path.c_str());
    if (!directory) return Failed();
    auto result = Failed(Error::kOk);
    while (const auto* entry = smb2_readdir(context_, directory)) {
      if (entry->name && (std::string_view(entry->name) == "." || std::string_view(entry->name) == "..")) continue;
      if (!entry->name || !Name(entry->name) ||
          (entry->st.smb2_type != SMB2_TYPE_DIRECTORY && entry->st.smb2_type != SMB2_TYPE_FILE)) {
        if (complete) { result->error = Error::kInvalidAddress; break; }
        continue;
      }
      if (result->entries.size() >= (complete ? 100000 : kMaxEntries)) {
        result->limited = true; if (complete) result->error = Error::kTooLarge; break;
      }
      result->entries.push_back(mojom::SmbEntry::New(entry->name,
          entry->st.smb2_type == SMB2_TYPE_DIRECTORY, entry->st.smb2_size,
          SmbTime(entry->st.smb2_mtime, entry->st.smb2_mtime_nsec),
          SmbTime(entry->st.smb2_btime, entry->st.smb2_btime_nsec)));
    }
    smb2_closedir(context_, directory);
    if (!complete && result->error == Error::kOk) result->write_access = WriteAccess(path);
    return result;
  }
  Error CheckWritable(std::string share, std::string path) {
    if (!Name(share) || share == "IPC$" || (!path.empty() && !Path(path))) return Error::kInvalidAddress;
    const auto mounted = Mount(share);
    if (mounted != Error::kOk) return mounted;
    // A real create/close/delete probe detects read-only filesystems as well as
    // ACLs. Delete-on-close also cleans up if this worker loses its connection.
    const auto probe = (path.empty() ? std::string() : path + "/") +
        ".crFiles-write-check-" + base::Uuid::GenerateRandomV4().AsLowercaseString();
    return CheckAccess(probe, true);
  }
  mojom::SmbDownloadPtr Download(std::string share, std::string path, base::File destination) {
    CloseFile(); bytes_ = 0; total_ = 0;
    if (!destination.IsValid()) return DownloadState(Error::kLocalFile, true);
    if (!Name(share) || share == "IPC$" || !Path(path)) return DownloadState(Error::kInvalidAddress, true);
    const auto mounted = Mount(share);
    if (mounted != Error::kOk) return DownloadState(mounted, true);
    file_ = smb2_open(context_, path.c_str(), O_RDONLY);
    if (!file_) return DownloadState(Error::kFailed, true);
    smb2_stat_64 info{};
    if (smb2_fstat(context_, file_, &info) != 0 || info.smb2_type != SMB2_TYPE_FILE) {
      CloseFile(); return DownloadState(Error::kFailed, true);
    }
    total_ = info.smb2_size; modified_ = info.smb2_mtime; modified_nsec_ = info.smb2_mtime_nsec;
    if (total_ > kMaxDownload) { CloseFile(); return DownloadState(Error::kTooLarge, true); }
    output_ = std::move(destination);
    return Continue();
  }
  mojom::SmbDownloadPtr Continue() {
    if (!file_ || !output_.IsValid()) return DownloadState(Error::kFailed, true);
    if (bytes_ < total_) {
      std::array<uint8_t, 256 * 1024> buffer;
      const auto count = static_cast<uint32_t>(std::min<uint64_t>(
          std::min<size_t>(buffer.size(), smb2_get_max_read_size(context_)), total_ - bytes_));
      const int received = smb2_pread(context_, file_, buffer.data(), count, bytes_);
      if (received <= 0 || static_cast<uint32_t>(received) > count) {
        CloseFile(); return DownloadState(Error::kFailed, true);
      }
      if (!output_.WriteAtCurrentPosAndCheck(base::span(buffer).first(static_cast<size_t>(received)))) {
        CloseFile(); return DownloadState(Error::kLocalFile, true);
      }
      bytes_ += received;
    }
    const bool done = bytes_ == total_;
    if (done) {
      // Detect common concurrent modifications instead of publishing a partial
      // or mixed-size download. The copy never writes back to the server.
      smb2_stat_64 info{};
      const bool valid = smb2_fstat(context_, file_, &info) == 0 && info.smb2_size == total_ &&
          info.smb2_mtime == modified_ && info.smb2_mtime_nsec == modified_nsec_;
      const bool flushed = output_.Flush();
      CloseFile();
      if (!valid || !flushed) return DownloadState(Error::kFailed, true);
    }
    return DownloadState(Error::kOk, done);
  }
  Error Mutate(std::string share, std::string path, mojom::SmbMutation operation, std::string target) {
    if (!Name(share) || !Path(path) || share == "IPC$" ||
        (operation == mojom::SmbMutation::kRename && !Path(target))) return Error::kInvalidAddress;
    // A rename may now cross directories within this tree connection. Reject
    // moving an entry into its own subtree before asking the SMB server.
    if (operation == mojom::SmbMutation::kRename &&
        base::StartsWith(target, path + "/", base::CompareCase::INSENSITIVE_ASCII)) return Error::kInvalidAddress;
    const auto mounted = Mount(share);
    if (mounted != Error::kOk) return mounted;
    CloseFile();
    switch (operation) {
      case mojom::SmbMutation::kCreateDirectory: return Status(smb2_mkdir(context_, path.c_str()));
      case mojom::SmbMutation::kEnsureDirectory: {
        const int created = smb2_mkdir(context_, path.c_str());
        if (created != -EEXIST) return Status(created);
        smb2_stat_64 info{};
        return smb2_stat(context_, path.c_str(), &info) == 0 && info.smb2_type == SMB2_TYPE_DIRECTORY ?
            Error::kOk : Error::kAlreadyExists;
      }
      // libsmb2 sets replace_if_exist=0: renaming never overwrites a target.
      case mojom::SmbMutation::kRename: return Status(smb2_rename(context_, path.c_str(), target.c_str()));
      case mojom::SmbMutation::kDeleteFile: return Status(smb2_unlink(context_, path.c_str()));
      case mojom::SmbMutation::kDeleteDirectory: return Status(smb2_rmdir(context_, path.c_str()));
      case mojom::SmbMutation::kDeleteTree: return DeleteTree(share, path);
    }
    return Error::kFailed;
  }
  Error DeleteTree(const std::string& share, const std::string& root) {
    std::vector<std::pair<std::string, bool>> entries{{root, true}};
    // Finish enumeration before deleting anything; never operate on a truncated
    // UI listing. Reverse traversal deletes children before their directories.
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!entries[i].second) continue;
      const auto path = entries[i].first;
      auto listing = List(share, path, true);
      if (listing->error != Error::kOk || listing->limited) return listing->error == Error::kOk ? Error::kTooLarge : listing->error;
      for (const auto& entry : listing->entries) {
        auto child = path + "/" + entry->name;
        if (entries.size() >= 100000 || !Path(child)) return Error::kTooLarge;
        entries.emplace_back(std::move(child), entry->directory);
      }
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      const int result = it->second ? smb2_rmdir(context_, it->first.c_str()) : smb2_unlink(context_, it->first.c_str());
      if (result != 0) return Status(result);
    }
    return Error::kOk;
  }
  mojom::SmbDownloadPtr Upload(std::string share, std::string path, base::File source) {
    CloseFile(); bytes_ = 0; total_ = 0;
    if (!Name(share) || share == "IPC$" || !Path(path)) return DownloadState(Error::kInvalidAddress, true);
    base::File::Info info;
    if (!source.IsValid() || !source.GetInfo(&info) || info.is_directory || info.size < 0) return DownloadState(Error::kLocalFile, true);
    total_ = info.size; source_modified_ = info.last_modified;
    if (total_ > kMaxDownload) return DownloadState(Error::kTooLarge, true);
    const auto mounted = Mount(share);
    if (mounted != Error::kOk) return DownloadState(mounted, true);
    smb2_stat_64 existing{};
    const int found = smb2_stat(context_, path.c_str(), &existing);
    if (found == 0) return DownloadState(Error::kAlreadyExists, true);
    if (found != -ENOENT) return DownloadState(Status(found), true);
    const auto slash = path.rfind('/');
    const auto temporary = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) +
        ".crFiles-upload-" + base::Uuid::GenerateRandomV4().AsLowercaseString();
    file_ = smb2_open(context_, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL);
    if (!file_) {
      const int error = nterror_to_errno(smb2_get_nterror(context_));
      return DownloadState(error ? Status(-error) : Error::kFailed, true);
    }
    upload_temporary_ = temporary; upload_target_ = std::move(path); output_ = std::move(source);
    return ContinueUpload();
  }
  mojom::SmbDownloadPtr ContinueUpload() {
    if (!file_ || !output_.IsValid() || upload_temporary_.empty()) return DownloadState(Error::kFailed, true);
    auto fail = [this](Error error) { CloseFile(); return DownloadState(error, true); };
    if (bytes_ < total_) {
      std::array<uint8_t, 256 * 1024> buffer;
      const size_t count = std::min<uint64_t>(std::min<size_t>(buffer.size(), smb2_get_max_write_size(context_)), total_ - bytes_);
      if (!count) return fail(Error::kFailed);
      const auto read = output_.ReadAtCurrentPos(base::span(buffer).first(count));
      if (!read || *read != count) return fail(Error::kLocalFile);
      size_t offset = 0;
      while (offset < count) {
        const int written = smb2_pwrite(context_, file_, base::span(buffer).subspan(offset).data(), static_cast<uint32_t>(count - offset), bytes_);
        if (written <= 0 || static_cast<size_t>(written) > count - offset) return fail(written < 0 ? Status(written) : Error::kFailed);
        offset += written; bytes_ += written;
      }
    }
    if (bytes_ < total_) return DownloadState(Error::kOk, false);
    base::File::Info info;
    if (!output_.GetInfo(&info) || info.size < 0 || static_cast<uint64_t>(info.size) != total_ || info.last_modified != source_modified_) return fail(Error::kLocalFile);
    const int flushed = smb2_fsync(context_, file_);
    if (flushed < 0) return fail(Status(flushed));
    auto* file = std::exchange(file_, nullptr);
    const int closed = smb2_close(context_, file);
    if (closed < 0) return fail(Status(closed));
    // Publish only a complete file. The server performs a non-overwriting rename.
    const int renamed = smb2_rename(context_, upload_temporary_.c_str(), upload_target_.c_str());
    if (renamed < 0) return fail(Status(renamed));
    upload_temporary_.clear(); upload_target_.clear(); output_.Close();
    return DownloadState(Error::kOk, true);
  }
 private:
  bool WaitForReply(bool& done) {
    const auto deadline = base::TimeTicks::Now() + base::Seconds(20);
    while (!done && base::TimeTicks::Now() < deadline) {
#if BUILDFLAG(IS_WIN)
      WSAPOLLFD descriptor{smb2_get_fd(context_), static_cast<short>(smb2_which_events(context_)), 0};
      const int ready = WSAPoll(&descriptor, 1, 100);
#else
      pollfd descriptor{smb2_get_fd(context_), static_cast<short>(smb2_which_events(context_)), 0};
      const int ready = poll(&descriptor, 1, 100);
#endif
      if (ready < 0 || smb2_service(context_, descriptor.revents) < 0) break;
    }
    // Destroy outstanding callbacks while their stack-owned results are alive.
    if (!done) { Reset(); return false; }
    return true;
  }
  Error CheckAccess(const std::string& path, bool probe) {
    struct Result { bool done = false; int status = 0; smb2_file_id id{}; } result;
    smb2_create_request request{};
    request.name = path.c_str();
    request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    request.desired_access = probe ? SMB2_FILE_WRITE_DATA | SMB2_DELETE : SMB2_FILE_ADD_FILE;
    request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
    request.create_disposition = probe ? SMB2_FILE_CREATE : SMB2_FILE_OPEN;
    request.create_options = probe ? SMB2_FILE_NON_DIRECTORY_FILE | SMB2_FILE_DELETE_ON_CLOSE : SMB2_FILE_DIRECTORY_FILE;
    auto* pdu = smb2_cmd_create_async(context_, &request,
        [](smb2_context*, int status, void* data, void* opaque) {
          auto& result = *static_cast<Result*>(opaque);
          result.status = status; result.done = true;
          if (!status && data) base::span(result.id).copy_from(base::span(static_cast<smb2_create_reply*>(data)->file_id));
        }, &result);
    if (!pdu) return Error::kFailed;
    smb2_queue_pdu(context_, pdu);
    if (!WaitForReply(result.done)) return Error::kFailed;
    if (result.status) return Status(-nterror_to_errno(result.status));
    auto* handle = smb2_fh_from_file_id(context_, &result.id);
    if (!handle) { Reset(); return Error::kFailed; }
    const auto error = Status(smb2_close(context_, handle));
    if (error != Error::kOk) Reset();
    return error;
  }
  mojom::SmbWriteAccess WriteAccess(const std::string& path) {
    const auto error = CheckAccess(path, false);
    return error == Error::kOk ? mojom::SmbWriteAccess::kWritable :
        error == Error::kAccessDenied ? mojom::SmbWriteAccess::kReadOnly : mojom::SmbWriteAccess::kUnknown;
  }
  mojom::SmbDownloadPtr DownloadState(Error error, bool done) {
    return mojom::SmbDownload::New(error, bytes_, total_, done);
  }
  void CloseFile() {
    if (file_) { smb2_close(context_, file_); file_ = nullptr; }
    if (context_ && !upload_temporary_.empty()) smb2_unlink(context_, upload_temporary_.c_str());
    upload_temporary_.clear(); upload_target_.clear();
    output_.Close();
  }
  void Reset() {
    CloseFile();
    if (context_) { smb2_destroy_context(context_); context_ = nullptr; }
    share_.clear();
  }
  Error Mount(const std::string& share) {
    if (context_ && share_ == share) return Error::kOk;
    Reset();
    context_ = smb2_init_context();
    if (!context_) return Error::kFailed;
    smb2_set_timeout(context_, 15);
    smb2_set_security_mode(context_, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_user(context_, user_.c_str());
    smb2_set_password(context_, password_.c_str());
    smb2_set_domain(context_, domain_.c_str());
    const int status = smb2_connect_share(context_, server_.c_str(), share.c_str(), user_.c_str());
    if (status != 0) {
      auto error = Status(status);
      // libsmb2 maps LOGON_FAILURE to ECONNREFUSED. Preserve authentication
      // failures so the UI prompts for credentials only when appropriate.
      switch (static_cast<uint32_t>(smb2_get_nterror(context_))) {
        case SMB2_STATUS_LOGON_FAILURE:
        case SMB2_STATUS_WRONG_PASSWORD:
        case SMB2_STATUS_NO_SUCH_USER:
        case SMB2_STATUS_PASSWORD_MUST_CHANGE:
        case SMB2_STATUS_INVALID_WORKSTATION:
          error = Error::kAccessDenied; break;
        default: break;
      }
      Reset(); return error;
    }
    share_ = share;
    return Error::kOk;
  }
  mojom::SmbListingPtr Shares() {
    struct Result { bool done = false; mojom::SmbListingPtr listing = Failed(); } result;
    const int started = smb2_share_enum_async(context_, SHARE_INFO_1,
        [](smb2_context* context, int status, void* data, void* opaque) {
          auto& result = *static_cast<Result*>(opaque);
          result.done = true; result.listing->error = Status(status);
          // SRVSVC can return Win32 ERROR_ACCESS_DENIED directly, while pipe
          // errors can still carry an NTSTATUS rather than a negative errno.
          if (status == 5 || static_cast<uint32_t>(status) == SMB2_STATUS_ACCESS_DENIED)
            result.listing->error = Error::kAccessDenied;
          if (status == 0 && data) {
            const auto& shares = static_cast<srvsvc_NetrShareEnum_rep*>(data)->ses.ShareInfo.Level1;
            if (shares.Buffer && shares.Buffer->share_info_1) {
              const size_t count = std::min(shares.EntriesRead, shares.Buffer->max_count);
              // SAFETY: libsmb2's dcerpc_carray_coder allocates max_count
              // SHARE_INFO_1 elements. The span is bounded by that allocation
              // and EntriesRead, and is consumed before smb2_free_data below.
              const auto entries = UNSAFE_BUFFERS(base::span<const srvsvc_SHARE_INFO_1>(
                  shares.Buffer->share_info_1, count));
              for (const auto& entry : entries) {
                if ((entry.type & 3) != SHARE_TYPE_DISKTREE || !entry.netname.utf8 || !Name(entry.netname.utf8)) continue;
                if (result.listing->entries.size() >= kMaxEntries) { result.listing->limited = true; break; }
                result.listing->entries.push_back(mojom::SmbEntry::New(entry.netname.utf8, true, 0, base::Time(), base::Time()));
              }
            }
          }
          if (data) smb2_free_data(context, data);
        }, &result);
    if (started < 0) return Failed(Status(started));
    const auto deadline = base::TimeTicks::Now() + base::Seconds(20);
    while (!result.done && base::TimeTicks::Now() < deadline) {
#if BUILDFLAG(IS_WIN)
      WSAPOLLFD descriptor{smb2_get_fd(context_), static_cast<short>(smb2_which_events(context_)), 0};
      const int ready = WSAPoll(&descriptor, 1, 100);
#else
      pollfd descriptor{smb2_get_fd(context_), static_cast<short>(smb2_which_events(context_)), 0};
      const int ready = poll(&descriptor, 1, 100);
#endif
      if (ready < 0 || smb2_service(context_, descriptor.revents) < 0) break;
    }
    if (!result.done) { Reset(); return Failed(); }
    return std::move(result.listing);
  }
  smb2_context* context_ = nullptr;
  smb2fh* file_ = nullptr;
  base::File output_;
  std::string server_, user_, password_, domain_, share_;
  std::string upload_temporary_, upload_target_;
  base::Time source_modified_;
  uint64_t bytes_ = 0, total_ = 0, modified_ = 0, modified_nsec_ = 0;
};

class Service : public mojom::SmbService {
 public:
  Service(mojo::PendingReceiver<mojom::SmbService> receiver, base::OnceClosure quit)
      : receiver_(this, std::move(receiver)), worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE})), session_(std::make_shared<Session>()) {
    receiver_.set_disconnect_handler(std::move(quit));
    discovery_ = std::make_unique<SmbDiscovery>(base::BindPostTaskToCurrentDefault(
        base::BindRepeating(&Service::Discovered, weak_.GetWeakPtr())));
  }
  void Discover(bool refresh, DiscoverCallback callback) override {
    if (refresh) LOG(INFO) << "SMB worker refresh requested; cached server count=" << peers_.size();
    if (refresh) discovery_->Refresh();
    std::vector<mojom::SmbServerPtr> servers;
    for (const auto& peer : peers_) servers.push_back(mojom::SmbServer::New(peer.name, peer.address, peer.port));
    std::move(callback).Run(std::move(servers));
  }
  void Connect(const std::string& server, const std::string& user, const std::string& password,
               const std::string& domain, const std::string& share, const std::string& path, ConnectCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::Connect, session_, server, user, password, domain, share, path), std::move(callback));
  }
  void List(const std::string& share, const std::string& path, bool complete, ListCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::List, session_, share, path, complete), std::move(callback));
  }
  void Download(const std::string& share, const std::string& path, base::File destination, DownloadCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::Download, session_, share, path, std::move(destination)), std::move(callback));
  }
  void ContinueDownload(ContinueDownloadCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::Continue, session_), std::move(callback));
  }
  void Mutate(const std::string& share, const std::string& path, mojom::SmbMutation operation,
              const std::string& target, MutateCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::Mutate, session_, share, path, operation, target), std::move(callback));
  }
  void Upload(const std::string& share, const std::string& path, base::File source, UploadCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::Upload, session_, share, path, std::move(source)), std::move(callback));
  }
  void ContinueUpload(ContinueUploadCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::ContinueUpload, session_), std::move(callback));
  }
  void CheckWritable(const std::string& share, const std::string& path, CheckWritableCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::CheckWritable, session_, share, path), std::move(callback));
  }
 private:
  void Discovered(std::vector<SmbPeer> peers) {
    LOG(INFO) << "SMB worker cached server count=" << peers.size();
    peers_ = std::move(peers);
  }
  mojo::Receiver<mojom::SmbService> receiver_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  std::shared_ptr<Session> session_;
  std::unique_ptr<SmbDiscovery> discovery_;
  std::vector<SmbPeer> peers_;
  base::WeakPtrFactory<Service> weak_{this};
};
}
int RunSmbDiscovery() {
#if BUILDFLAG(IS_WIN)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
  // Exercise the same implementation as Network without a window, IPC or
  // persisted preferences. This command only discovers; it never authenticates.
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::IO);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFilesSMBDiscovery");
  auto peers = std::make_shared<std::vector<SmbPeer>>();
  printf("Discovering _smb._tcp.local for 12 seconds...\n");
  fflush(stdout);
  {
    base::RunLoop loop;
    SmbDiscovery discovery(base::BindPostTaskToCurrentDefault(base::BindRepeating(
        [](std::shared_ptr<std::vector<SmbPeer>> peers, std::vector<SmbPeer> found) {
          *peers = std::move(found);
          printf("SMB servers: %zu\n", peers->size());
          for (const auto& peer : *peers)
            printf("%s\t%s:%u\t%s\n", peer.name.c_str(), peer.address.c_str(),
                   static_cast<unsigned>(peer.port), peer.host.c_str());
          fflush(stdout);
        }, peers)));
    base::OneShotTimer timer;
    timer.Start(FROM_HERE, base::Seconds(12), loop.QuitClosure());
    loop.Run();
  }
  printf("Discovery complete: %zu SMB servers.\n", peers->size());
  fflush(stdout);
  base::ThreadPoolInstance::Get()->Shutdown();
  return peers->empty() ? 2 : 0;
}
int RunSmbProcess() {
#if BUILDFLAG(IS_WIN)
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::IO);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFilesSMB");
  base::Thread ipc_thread("crFilesSMBIPC");
  CHECK(ipc_thread.StartWithOptions(base::Thread::Options(base::MessagePumpType::IO, 0)));
  mojo::core::ScopedIPCSupport ipc(ipc_thread.task_runner(), mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
  auto endpoint = mojo::PlatformChannel::RecoverPassedEndpointFromCommandLine(*base::CommandLine::ForCurrentProcess());
  if (!endpoint.is_valid()) return 1;
  auto pipe = mojo::IncomingInvitation::AcceptIsolated(std::move(endpoint));
  if (!pipe.is_valid()) return 1;
  base::RunLoop loop;
  {
    Service service(mojo::PendingReceiver<mojom::SmbService>(std::move(pipe)), loop.QuitClosure());
    loop.Run();
  }
  base::ThreadPoolInstance::Get()->Shutdown();
  return 0;
}
}
