// Copyright 2026 The Chromium Authors
#include "crFiles/crfiles_network.h"
#include "crFiles/crfiles_network_discovery.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include "mojo/public/cpp/bindings/associated_remote.h"
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
#include "base/numerics/byte_conversions.h"
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
constexpr size_t kMaxShares = 1000;
constexpr size_t kMaxTreeEntries = 100000;
constexpr uint32_t kQueryBytes = 64 * 1024;
using Purpose = mojom::SmbTransferPurpose;
using WatchObserver = mojo::PendingRemote<mojom::SmbDirectoryObserver>;
Error ControlError(uint32_t status) {
  const int error = nterror_to_errno(status);
  return error == EACCES || error == EPERM || error == EROFS ? Error::kAccessDenied : Error::kFailed;
}
bool UnsupportedControl(uint32_t status) {
  return status == SMB2_STATUS_NOT_SUPPORTED || status == SMB2_STATUS_INVALID_DEVICE_REQUEST ||
      status == SMB2_STATUS_NOT_IMPLEMENTED;
}
using ProgressRemote = mojo::PendingAssociatedRemote<mojom::SmbTransferProgress>;

class TransferReporter {
 public:
  explicit TransferReporter(ProgressRemote remote) : remote_(std::move(remote)) {}
  void Advanced(uint64_t bytes, uint64_t total) {
    const auto now = base::TimeTicks::Now();
    if (now < next_ && bytes != total) return;
    next_ = now + base::Milliseconds(200);
    remote_->Advanced(bytes, total);
  }
 private:
  mojo::AssociatedRemote<mojom::SmbTransferProgress> remote_;
  base::TimeTicks next_;
};
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
    const auto configured = Configure(server, user, password, domain);
    if (configured != Error::kOk) return Failed(configured);
    return List(std::move(share), std::move(path), false);
  }
  Error Configure(std::string& server, std::string& user, std::string& password, std::string& domain) {
    Reset();
    if (server.empty() || server.size() > 300 || user.size() > 256 || password.size() > 1024 ||
        domain.size() > 256 || user.find('\0') != std::string::npos ||
        password.find('\0') != std::string::npos || domain.find('\0') != std::string::npos ||
        std::ranges::any_of(server, [](unsigned char c) {
          return !base::IsAsciiAlphaNumeric(c) && c != '.' && c != '-' && c != '_' &&
              c != ':' && c != '[' && c != ']' && c != '%';
        })) return Error::kInvalidAddress;
    server_ = std::move(server); user_ = user.empty() ? "guest" : std::move(user);
    password_ = std::move(password); domain_ = std::move(domain);
    return Error::kOk;
  }
  mojom::SmbListingPtr List(std::string share, std::string path, bool complete) {
    if ((!share.empty() && !Name(share)) || (!path.empty() && (!Path(path) || share.empty())) || server_.empty())
      return Failed(Error::kInvalidAddress);
    const auto status = Mount(share.empty() ? "IPC$" : share);
    if (status != Error::kOk) return Failed(status);
    if (share.empty()) return Shares();
    CloseDirectory();
    strict_listing_ = complete;
    listed_ = 0;
    const auto opened = OpenExisting(path, true, directory_);
    if (opened != Error::kOk) return Failed(opened);
    const auto access = complete ? mojom::SmbWriteAccess::kUnknown : WriteAccess(path);
    auto result = NextPage();
    result->write_access = access;
    return result;
  }

  Error OpenExisting(const std::string& path, bool directory, smb2fh*& handle) {
    smb2_create_request request{};
    request.name = path.c_str();
    request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
    request.desired_access = (directory ? SMB2_FILE_LIST_DIRECTORY : SMB2_FILE_READ_DATA) | SMB2_FILE_READ_ATTRIBUTES;
    request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE | SMB2_FILE_SHARE_DELETE;
    request.create_disposition = SMB2_FILE_OPEN;
    request.create_options = (directory ? SMB2_FILE_DIRECTORY_FILE : SMB2_FILE_NON_DIRECTORY_FILE) | SMB2_FILE_OPEN_REPARSE_POINT;
    struct OpenResult { bool done = false; Error error = Error::kFailed; smb2_file_id id{}; } opened;
    auto* pdu = smb2_cmd_create_async(context_, &request,
        [](smb2_context*, int status, void* data, void* opaque) {
          auto& result = *static_cast<OpenResult*>(opaque);
          result.done = true;
          if (status) { result.error = Status(-nterror_to_errno(status)); return; }
          if (!data) return;
          const auto& reply = *static_cast<smb2_create_reply*>(data);
          base::span(result.id).copy_from(base::span(reply.file_id));
          result.error = (reply.file_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT) ?
              Error::kInvalidAddress : Error::kOk;
        }, &opened);
    if (!pdu) return Error::kFailed;
    smb2_queue_pdu(context_, pdu);
    if (!WaitForReply(opened.done)) return Error::kFailed;
    if (opened.error == Error::kInvalidAddress) {
      // The server granted a handle to a reparse point. Release it with the session.
      Reset(); return opened.error;
    }
    if (opened.error != Error::kOk) return opened.error;
    handle = smb2_fh_from_file_id(context_, &opened.id);
    if (!handle) { Reset(); return Error::kFailed; }
    return Error::kOk;
  }

  mojom::SmbListingPtr NextPage() {
    if (!directory_) return Failed();
    smb2_query_directory_request request{};
    request.file_information_class = SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION;
    base::span(request.file_id).copy_from(base::span(*smb2_get_file_id(directory_)));
    request.output_buffer_length = kQueryBytes;
    request.name = "*";
    struct Query { Session* session; mojom::SmbListingPtr page; bool done; };
    Query query{this, Failed(Error::kOk), false};
    auto* pdu = smb2_cmd_query_directory_async(context_, &request,
        [](smb2_context*, int status, void* data, void* opaque) {
          auto& query = *static_cast<Query*>(opaque);
          query.done = true;
          if (static_cast<uint32_t>(status) == SMB2_STATUS_NO_MORE_FILES) return;
          if (status || !data) {
            query.page->error = status ? Status(-nterror_to_errno(status)) : Error::kFailed;
            return;
          }
          query.session->DecodePage(*static_cast<smb2_query_directory_reply*>(data), *query.page);
        }, &query);
    if (!pdu) { CloseDirectory(); return Failed(); }
    smb2_queue_pdu(context_, pdu);
    if (!WaitForReply(query.done)) return Failed();
    if (!query.page->more) CloseDirectory();
    return std::move(query.page);
  }

  void DecodePage(const smb2_query_directory_reply& reply, mojom::SmbListing& page) {
    if (!reply.output_buffer || !reply.output_buffer_length || reply.output_buffer_length > kQueryBytes) {
      page.error = Error::kFailed; return;
    }
    // SAFETY: libsmb2 validates the response payload length before dispatching the PDU.
    auto remaining = UNSAFE_BUFFERS(base::span(reply.output_buffer, reply.output_buffer_length));
    while (!remaining.empty()) {
      constexpr size_t header = SMB2_FILEID_FULL_DIRECTORY_INFORMATION_SIZE;
      if (remaining.size() < header) { page.error = Error::kFailed; return; }
      const uint32_t next = base::U32FromLittleEndian(remaining.first<4>());
      if (next && (next < header || next % 8 || next >= remaining.size())) { page.error = Error::kFailed; return; }
      const auto record = remaining.first(next ? next : remaining.size());
      const uint32_t name_bytes = base::U32FromLittleEndian(record.subspan<60, 4>());
      if (name_bytes % 2 || name_bytes > record.size() - header) { page.error = Error::kFailed; return; }
      // The library exposes a NUL-terminated UTF-8 name. Reject embedded UTF-16
      // NULs so a recursive operation never uses a shortened server name.
      const auto encoded_name = record.subspan(header, name_bytes);
      for (size_t i = 0; i < encoded_name.size(); i += 2) {
        if (!encoded_name[i] && !encoded_name[i + 1]) { page.error = Error::kInvalidAddress; return; }
      }
      smb2_iovec vec{record.data(), record.size(), nullptr};
      smb2_fileidfulldirectoryinformation info{};
      if (smb2_decode_fileidfulldirectoryinformation(context_, &info, &vec) < 0 || !info.name) {
        page.error = Error::kFailed; return;
      }
      std::string name(info.name);
      free(const_cast<char*>(info.name));
      if (name != "." && name != "..") {
        if (!Name(name) || (info.file_attributes & SMB2_FILE_ATTRIBUTE_REPARSE_POINT)) {
          if (strict_listing_) { page.error = Error::kInvalidAddress; return; }
        } else if (listed_ >= kMaxTreeEntries) {
          page.limited = true;
          if (strict_listing_) page.error = Error::kTooLarge;
          return;
        } else {
          ++listed_;
          page.entries.push_back(mojom::SmbEntry::New(std::move(name),
              (info.file_attributes & SMB2_FILE_ATTRIBUTE_DIRECTORY) != 0, info.end_of_file,
              SmbTime(info.last_write_time.tv_sec, info.last_write_time.tv_usec * 1000),
              SmbTime(info.creation_time.tv_sec, info.creation_time.tv_usec * 1000)));
        }
      }
      if (!next) break;
      remaining = remaining.subspan(next);
    }
    page.more = true;
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
  Error Download(std::string share, std::string path, base::File destination, Purpose purpose, ProgressRemote remote) {
    CloseDirectory(); CloseFile();
    uint64_t bytes = 0;
    if (!destination.IsValid()) return Error::kLocalFile;
    if (!Name(share) || share == "IPC$" || !Path(path)) return Error::kInvalidAddress;
    const auto mounted = Mount(share);
    if (mounted != Error::kOk) return mounted;
    file_ = smb2_open(context_, path.c_str(), O_RDONLY);
    if (!file_) return Error::kFailed;
    smb2_stat_64 info{};
    if (smb2_fstat(context_, file_, &info) != 0 || info.smb2_type != SMB2_TYPE_FILE) {
      CloseFile(); return Error::kFailed;
    }
    const uint64_t total = info.smb2_size;
    if (total > std::numeric_limits<int64_t>::max() || (purpose == Purpose::kPreview && total > kSmbPreviewLimit)) { CloseFile(); return Error::kTooLarge; }
    TransferReporter progress(std::move(remote));
    std::vector<uint8_t> buffer(std::min<uint64_t>({total, 1024 * 1024, smb2_get_max_read_size(context_)}));
    while (bytes < total) {
      const auto count = static_cast<uint32_t>(std::min<uint64_t>(
          buffer.size(), total - bytes));
      if (!count) { CloseFile(); return Error::kFailed; }
      const int received = smb2_pread(context_, file_, buffer.data(), count, bytes);
      if (received <= 0 || static_cast<uint32_t>(received) > count) {
        CloseFile(); return Error::kFailed;
      }
      if (!destination.WriteAtCurrentPosAndCheck(base::span(buffer).first(static_cast<size_t>(received)))) {
        CloseFile(); return Error::kLocalFile;
      }
      bytes += received;
      progress.Advanced(bytes, total);
    }
    // Publish the download only after rechecking the source and flushing the destination.
    smb2_stat_64 final_info{};
    const bool valid = smb2_fstat(context_, file_, &final_info) == 0 && final_info.smb2_size == total &&
        final_info.smb2_mtime == info.smb2_mtime && final_info.smb2_mtime_nsec == info.smb2_mtime_nsec;
    const bool flushed = destination.Flush();
    CloseFile();
    if (!valid || !flushed) return Error::kFailed;
    return Error::kOk;
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
    CloseDirectory(); CloseFile();
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
    }
    return Error::kFailed;
  }
  mojom::SmbDeletionPtr DeleteTree(std::string share, std::string root) {
    CancelDelete();
    if (!Name(share) || share == "IPC$" || !Path(root))
      return mojom::SmbDeletion::New(Error::kInvalidAddress, 0, 0, false, true);
    delete_share_ = std::move(share);
    tree_.emplace_back(std::move(root), true);
    return ContinueDelete();
  }
  mojom::SmbDeletionPtr ContinueDelete() {
    auto state = [&](Error error, bool done) {
      auto result = mojom::SmbDeletion::New(error, tree_.size(), deleted_, deleting_, done);
      if (done) CancelDelete();
      return result;
    };
    if (tree_.empty()) return state(Error::kFailed, true);
    // A batch ends after a directory response or a bounded period of successful
    // deletes. The caller renews its inactivity deadline on each reply.
    while (!deleting_ && scan_index_ < tree_.size()) {
      if (!tree_[scan_index_].second) { ++scan_index_; continue; }
      const auto parent = tree_[scan_index_].first;
      auto page = directory_ ? NextPage() : List(delete_share_, parent, true);
      if (page->error != Error::kOk) return state(page->error, true);
      for (const auto& entry : page->entries) {
        auto child = parent + "/" + entry->name;
        if (tree_.size() >= kMaxTreeEntries || !Path(child)) return state(Error::kTooLarge, true);
        tree_.emplace_back(std::move(child), entry->directory);
      }
      if (!page->more) ++scan_index_;
      return state(Error::kOk, false);
    }
    deleting_ = true;
    const auto deadline = base::TimeTicks::Now() + base::Milliseconds(150);
    do {
      const auto& [path, directory] = tree_[tree_.size() - deleted_ - 1];
      const int status = directory ? smb2_rmdir(context_, path.c_str()) : smb2_unlink(context_, path.c_str());
      if (status) return state(Status(status), true);
      ++deleted_;
    } while (deleted_ < tree_.size() && base::TimeTicks::Now() < deadline);
    return state(Error::kOk, deleted_ == tree_.size());
  }
  void CancelDelete() {
    CloseDirectory();
    tree_.clear(); delete_share_.clear(); scan_index_ = 0; deleted_ = 0; deleting_ = false;
  }
  Error Upload(std::string share, std::string path, base::File source, Purpose purpose, ProgressRemote remote) {
    CloseDirectory(); CloseFile();
    uint64_t bytes = 0;
    if (!Name(share) || share == "IPC$" || !Path(path)) return Error::kInvalidAddress;
    base::File::Info info;
    if (!source.IsValid() || !source.GetInfo(&info) || info.is_directory || info.size < 0) return Error::kLocalFile;
    const uint64_t total = info.size;
    if (purpose == Purpose::kNearbyReceive && total > kSmbNearbyFileLimit) return Error::kTooLarge;
    const auto mounted = Mount(share);
    if (mounted != Error::kOk) return mounted;
    const auto opened = BeginUpload(path);
    if (opened != Error::kOk) return opened;
    TransferReporter progress(std::move(remote));
    std::vector<uint8_t> buffer(std::min<uint64_t>({total, 1024 * 1024, smb2_get_max_write_size(context_)}));
    auto fail = [this](Error error) { CloseFile(); return error; };
    while (bytes < total) {
      const size_t count = std::min<uint64_t>(buffer.size(), total - bytes);
      if (!count) return fail(Error::kFailed);
      const auto read = source.ReadAtCurrentPos(base::span(buffer).first(count));
      if (!read || *read != count) return fail(Error::kLocalFile);
      size_t offset = 0;
      while (offset < count) {
        const int written = smb2_pwrite(context_, file_, base::span(buffer).subspan(offset).data(), static_cast<uint32_t>(count - offset), bytes);
        if (written <= 0 || static_cast<size_t>(written) > count - offset) return fail(written < 0 ? Status(written) : Error::kFailed);
        offset += written; bytes += written;
        progress.Advanced(bytes, total);
      }
    }
    base::File::Info final_info;
    if (!source.GetInfo(&final_info) || final_info.size < 0 || static_cast<uint64_t>(final_info.size) != total || final_info.last_modified != info.last_modified) return fail(Error::kLocalFile);
    return PublishUpload(path);
  }

  Error CopyFile(std::string source_share, std::string source, std::string destination_share, std::string destination, ProgressRemote remote) {
    const bool same_share = base::EqualsCaseInsensitiveASCII(source_share, destination_share);
    if (!Name(source_share) || !Name(destination_share) || base::EqualsCaseInsensitiveASCII(source_share, "IPC$") ||
        base::EqualsCaseInsensitiveASCII(destination_share, "IPC$") || source.empty() || destination.empty() ||
        !Path(source) || !Path(destination) ||
        (same_share && base::EqualsCaseInsensitiveASCII(source, destination))) return Error::kInvalidAddress;
    CloseDirectory(); CloseFile();
    if (!copy_source_share_.empty() && !same_share && copy_source_share_ != source_share) Reset();
    const auto mounted = Mount(destination_share);
    if (mounted != Error::kOk) return mounted;
    uint32_t destination_tree = 0;
    if (smb2_get_tree_id_for_pdu(context_, nullptr, &destination_tree)) return Error::kFailed;
    uint32_t source_tree = destination_tree;
    auto error = Error::kOk;
    if (!same_share) {
      error = SelectCopySource(source_share);
      if (error != Error::kOk) return error;
      source_tree = copy_source_tree_;
    }
    smb2fh* input = nullptr;
    error = OpenExisting(source, false, input);
    if (!context_) return error;
    if (!SelectTree(destination_tree)) return Error::kFailed;
    if (error != Error::kOk) return error;
    error = BeginUpload(destination);
    if (error == Error::kOk) {
      TransferReporter progress(std::move(remote));
      error = CopyContents(input, source_tree, destination_tree, progress);
    }
    if (!SelectTree(source_tree)) return Error::kFailed;
    smb2_close(context_, input);
    if (!SelectTree(destination_tree)) return Error::kFailed;
    if (error != Error::kOk) { CloseFile(); return error; }
    return PublishUpload(destination);
  }

  // The parent normally terminates this dedicated worker when the pane hides.
  // Also release a pending watch if the parent exits and its Mojo pipe closes.
  void StopWatch() { stop_watch_.store(true); }
  void StartWatch(SmbCredentials login, WatchObserver endpoint) {
    mojo::Remote<mojom::SmbDirectoryObserver> observer(std::move(endpoint));
    const auto configured = Configure(login.location.server, login.user, login.password, login.domain);
    const bool permanent = configured != Error::kOk || Watch(login.location.share, login.location.path, *observer.get());
    AbandonConnection();
    if (!stop_watch_.load()) observer->Unavailable(permanent);
  }
 private:
  bool SelectTree(uint32_t tree) {
    if (context_ && !smb2_select_tree_id(context_, tree)) return true;
    AbandonConnection();
    return false;
  }
  Error SelectCopySource(const std::string& share) {
    if (copy_source_share_ == share) return SelectTree(copy_source_tree_) ? Error::kOk : Error::kFailed;
    // COPYCHUNK requires both opens in the same authenticated SMB session.
    // TREE_CONNECT adds the source share and selects it for subsequent requests.
    const auto unc = "\\\\" + server_ + "\\" + share;
    std::unique_ptr<smb2_utf16, decltype(&free)> path(smb2_utf8_to_utf16(unc.c_str()), &free);
    if (!path) return Error::kFailed;
    smb2_tree_connect_request request{};
    request.path_length = path->len * 2; request.path = path->val;
    struct Result { bool done = false; uint32_t status = SMB2_STATUS_UNSUCCESSFUL; } result;
    auto* pdu = smb2_cmd_tree_connect_async(context_, &request, [](smb2_context*, int status, void*, void* opaque) {
      auto& result = *static_cast<Result*>(opaque);
      result.status = status; result.done = true;
    }, &result);
    if (!pdu) return Error::kFailed;
    smb2_queue_pdu(context_, pdu);
    if (!WaitForReply(result.done)) return Error::kFailed;
    if (result.status) return ControlError(result.status);
    if (smb2_get_tree_id_for_pdu(context_, nullptr, &copy_source_tree_)) { AbandonConnection(); return Error::kFailed; }
    copy_source_share_ = share;
    return Error::kOk;
  }
  Error BeginUpload(const std::string& path) {
    smb2_stat_64 existing{};
    const int found = smb2_stat(context_, path.c_str(), &existing);
    if (found == 0) return Error::kAlreadyExists;
    if (found != -ENOENT) return Status(found);
    const auto slash = path.rfind('/');
    const auto temporary = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) +
        ".crFiles-upload-" + base::Uuid::GenerateRandomV4().AsLowercaseString();
    file_ = smb2_open(context_, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL);
    if (!file_) {
      const int error = nterror_to_errno(smb2_get_nterror(context_));
      return error ? Status(-error) : Error::kFailed;
    }
    upload_temporary_ = temporary;
    return Error::kOk;
  }
  Error PublishUpload(const std::string& path) {
    const int flushed = smb2_fsync(context_, file_);
    if (flushed < 0) { CloseFile(); return Status(flushed); }
    auto* file = std::exchange(file_, nullptr);
    const int closed = smb2_close(context_, file);
    if (closed < 0) { CloseFile(); return Status(closed); }
    // Publish only a complete file. The server performs a non-overwriting rename.
    const int renamed = smb2_rename(context_, upload_temporary_.c_str(), path.c_str());
    if (renamed < 0) { CloseFile(); return Status(renamed); }
    upload_temporary_.clear();
    return Error::kOk;
  }

  uint32_t CopyControl(smb2fh* handle, uint32_t code, base::span<uint8_t> input, base::span<uint8_t> output) {
    smb2_ioctl_request request{};
    request.ctl_code = code;
    base::span(request.file_id).copy_from(base::span(*smb2_get_file_id(handle)));
    request.flags = SMB2_0_IOCTL_IS_FSCTL;
    request.input = input.data(); request.input_count = input.size();
    struct Reply { uint32_t code; base::span<uint8_t> bytes; uint32_t status; bool done; };
    Reply reply{code, output, SMB2_STATUS_UNSUCCESSFUL, false};
    auto* pdu = smb2_cmd_ioctl_async(context_, &request,
        [](smb2_context* context, int status, void* data, void* opaque) {
          auto& reply = *static_cast<Reply*>(opaque);
          reply.done = true; reply.status = status;
          if (status) return;
          auto* response = static_cast<smb2_ioctl_reply*>(data);
          if (!response || !response->output_count || !response->output) {
            reply.status = SMB2_STATUS_UNSUCCESSFUL; return;
          }
          if (response->ctl_code == reply.code && response->output_count >= reply.bytes.size()) {
            // SAFETY: libsmb2 owns output_count decoded bytes for these two FSCTLs.
            reply.bytes.copy_from(UNSAFE_BUFFERS(base::span(
                static_cast<const uint8_t*>(response->output), reply.bytes.size())));
          } else reply.status = SMB2_STATUS_UNSUCCESSFUL;
          smb2_free_data(context, response->output);
        }, &reply);
    if (!pdu) return SMB2_STATUS_UNSUCCESSFUL;
    smb2_queue_pdu(context_, pdu);
    return WaitForReply(reply.done) ? reply.status : SMB2_STATUS_UNSUCCESSFUL;
  }
  Error CopyContents(smb2fh* input, uint32_t source_tree, uint32_t destination_tree, TransferReporter& progress) {
    if (!SelectTree(source_tree)) return Error::kFailed;
    smb2_stat_64 before{};
    if (smb2_fstat(context_, input, &before) || before.smb2_type != SMB2_TYPE_FILE) return Error::kFailed;
    // MS-SMB2 2.2.31.1: 24-byte source key, count and reserved word,
    // followed by one source offset, destination offset, length and reserved word.
    std::array<uint8_t, 56> request{};
    bool relay = copy_control_unavailable_;
    if (before.smb2_size && !relay) {
      const auto status = CopyControl(input, SMB2_FSCTL_SRV_REQUEST_RESUME_KEY, {}, base::span(request).first<24>());
      if (UnsupportedControl(status)) { relay = true; copy_control_unavailable_ = true; }
      else if (status) return ControlError(status);
    }
    base::span(request).subspan<24, 4>().copy_from(base::U32ToLittleEndian(1));
    if (!SelectTree(destination_tree)) return Error::kFailed;
    uint32_t chunk_size = copy_chunk_size_;
    std::vector<uint8_t> buffer;
    uint64_t offset = 0;
    while (offset < before.smb2_size) {
      const auto count = static_cast<uint32_t>(std::min<uint64_t>(chunk_size, before.smb2_size - offset));
      if (!relay) {
        base::span(request).subspan<32, 8>().copy_from(base::U64ToLittleEndian(offset));
        base::span(request).subspan<40, 8>().copy_from(base::U64ToLittleEndian(offset));
        base::span(request).subspan<48, 4>().copy_from(base::U32ToLittleEndian(count));
        std::array<uint8_t, 12> reply{};
        const auto status = CopyControl(file_, SMB2_FSCTL_SRV_COPYCHUNK_WRITE, request, reply);
        if (UnsupportedControl(status)) { relay = true; copy_control_unavailable_ = true; continue; }
        // The server may limit copies to one share or backing filesystem.
        if (status == SMB2_STATUS_NOT_SAME_DEVICE || (source_tree != destination_tree &&
            (status == SMB2_STATUS_OBJECT_NAME_NOT_FOUND || status == SMB2_STATUS_INVALID_HANDLE))) {
          relay = true; continue;
        }
        if (status == SMB2_STATUS_INVALID_PARAMETER && count > 1) {
          chunk_size = count / 2; continue;
        }
        if (status) return ControlError(status);
        copy_chunk_size_ = chunk_size;
        if (base::U32FromLittleEndian(base::span(reply).first<4>()) != 1 ||
            base::U32FromLittleEndian(base::span(reply).last<4>()) != count) return Error::kFailed;
        offset += count;
      } else {
        if (buffer.empty()) {
          chunk_size = std::min({chunk_size, smb2_get_max_read_size(context_), smb2_get_max_write_size(context_)});
          if (!chunk_size) return Error::kFailed;
          buffer.resize(std::min(count, chunk_size));
        }
        const auto requested = static_cast<uint32_t>(std::min<size_t>(count, buffer.size()));
        if (!SelectTree(source_tree)) return Error::kFailed;
        const int read = smb2_pread(context_, input, buffer.data(), requested, offset);
        if (read <= 0 || static_cast<uint32_t>(read) > requested) return Error::kFailed;
        if (!SelectTree(destination_tree)) return Error::kFailed;
        auto pending = base::span(buffer).first(static_cast<size_t>(read));
        while (!pending.empty()) {
          const int written = smb2_pwrite(context_, file_, pending.data(), pending.size(), offset);
          if (written <= 0 || static_cast<size_t>(written) > pending.size()) return Error::kFailed;
          pending = pending.subspan(static_cast<size_t>(written)); offset += written;
        }
      }
      progress.Advanced(offset, before.smb2_size);
    }
    if (!SelectTree(source_tree)) return Error::kFailed;
    smb2_stat_64 after{};
    return !smb2_fstat(context_, input, &after) && before.smb2_size == after.smb2_size &&
        before.smb2_mtime == after.smb2_mtime && before.smb2_mtime_nsec == after.smb2_mtime_nsec ?
        Error::kOk : Error::kFailed;
  }

  bool ServiceSocket(int milliseconds) {
#if BUILDFLAG(IS_WIN)
    WSAPOLLFD descriptor{smb2_get_fd(context_), static_cast<short>(smb2_which_events(context_)), 0};
    const int ready = WSAPoll(&descriptor, 1, milliseconds);
#else
    pollfd descriptor{smb2_get_fd(context_), static_cast<short>(smb2_which_events(context_)), 0};
    const int ready = poll(&descriptor, 1, milliseconds);
#endif
    return ready >= 0 && smb2_service(context_, descriptor.revents) >= 0;
  }
  bool WatchConnectionAlive() {
    struct Echo { bool done = false; bool ok = false; } echo;
    auto* pdu = smb2_cmd_echo_async(context_, [](smb2_context*, int status, void*, void* opaque) {
      if (static_cast<uint32_t>(status) == SMB2_STATUS_PENDING) return;
      auto& echo = *static_cast<Echo*>(opaque);
      echo.ok = status == 0; echo.done = true;
    }, &echo);
    if (!pdu) return false;
    smb2_queue_pdu(context_, pdu);
    return WaitForReply(echo.done) && echo.ok;
  }
  bool Watch(const std::string& share, const std::string& path, mojom::SmbDirectoryObserver& observer) {
    if (!Name(share) || share == "IPC$" || (!path.empty() && !Path(path)) || server_.empty()) return true;
    const auto mounted = Mount(share);
    if (mounted != Error::kOk) return mounted == Error::kAccessDenied;
    const auto opened = OpenExisting(path, true, directory_);
    if (opened != Error::kOk) return opened == Error::kAccessDenied || opened == Error::kInvalidAddress;
    // A quiet directory can remain pending indefinitely. Passthrough exposes
    // STATUS_PENDING so the first confirmed subscription can close the listing race.
    smb2_set_timeout(context_, 0);
    smb2_set_passthrough(context_, 1);
    bool armed = false;
    auto next_echo = base::TimeTicks::Now() + base::Minutes(1);
    while (!stop_watch_.load()) {
      smb2_change_notify_request request{};
      request.output_buffer_length = 16 * 1024;
      base::span(request.file_id).copy_from(base::span(*smb2_get_file_id(directory_)));
      request.completion_filter = SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_FILE_NAME |
          SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_DIR_NAME | SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_SIZE |
          SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_LAST_WRITE | SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_ATTRIBUTES;
      struct Result { mojom::SmbDirectoryObserver& observer; bool& armed; bool done; uint32_t status; };
      Result result{observer, armed, false, 0};
      auto* pdu = smb2_cmd_change_notify_async(context_, &request,
          [](smb2_context*, int status, void*, void* opaque) {
            auto& result = *static_cast<Result*>(opaque);
            if (static_cast<uint32_t>(status) == SMB2_STATUS_PENDING) {
              if (!result.armed) { result.armed = true; result.observer.Changed(); }
              return;
            }
            result.status = status; result.done = true;
          }, &result);
      if (!pdu) return false;
      smb2_queue_pdu(context_, pdu);
      while (!result.done && !stop_watch_.load() && ServiceSocket(1000)) {
        if (base::TimeTicks::Now() >= next_echo) {
          if (!WatchConnectionAlive()) { AbandonConnection(); return false; }
          next_echo = base::TimeTicks::Now() + base::Minutes(1);
        }
      }
      if (!result.done) {
        // Dispatch outstanding callbacks while result is alive, without waiting for CLOSE.
        AbandonConnection(); return false;
      }
      constexpr uint32_t kNotifyEnumDir = 0x0000010c;
      constexpr uint32_t kBufferOverflow = 0x80000005;
      if (result.status && result.status != kNotifyEnumDir && result.status != kBufferOverflow)
        return UnsupportedControl(result.status) || result.status == SMB2_STATUS_ACCESS_DENIED;
      armed = true;
      observer.Changed();
    }
    return false;
  }
  bool WaitForReply(bool& done) {
    const auto deadline = base::TimeTicks::Now() + base::Seconds(20);
    while (!done && base::TimeTicks::Now() < deadline && ServiceSocket(100)) {}
    // Destroy outstanding callbacks while their stack-owned results are alive.
    if (!done) { AbandonConnection(); return false; }
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
  void CloseDirectory() {
    if (directory_) smb2_close(context_, std::exchange(directory_, nullptr));
  }
  void CloseFile() {
    if (file_) { smb2_close(context_, file_); file_ = nullptr; }
    if (context_ && !upload_temporary_.empty()) smb2_unlink(context_, upload_temporary_.c_str());
    upload_temporary_.clear();
  }
  void AbandonConnection() {
    directory_ = nullptr; file_ = nullptr; upload_temporary_.clear();
    if (context_) smb2_destroy_context(std::exchange(context_, nullptr));
    share_.clear(); copy_source_share_.clear();
  }
  void Reset() {
    CloseDirectory();
    CloseFile();
    if (context_) { smb2_destroy_context(context_); context_ = nullptr; }
    share_.clear(); copy_source_share_.clear();
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
    copy_control_unavailable_ = false; copy_chunk_size_ = 1024 * 1024;
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
                if (result.listing->entries.size() >= kMaxShares) { result.listing->limited = true; break; }
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
  std::string copy_source_share_;
  uint32_t copy_source_tree_ = 0;
  std::atomic_bool stop_watch_{false};
  smb2_context* context_ = nullptr;
  smb2fh* directory_ = nullptr;
  bool strict_listing_ = false, copy_control_unavailable_ = false;
  uint32_t copy_chunk_size_ = 1024 * 1024;
  size_t listed_ = 0;
  std::vector<std::pair<std::string, bool>> tree_;
  std::string delete_share_;
  size_t scan_index_ = 0;
  uint32_t deleted_ = 0;
  bool deleting_ = false;
  smb2fh* file_ = nullptr;
  std::string server_, user_, password_, domain_, share_;
  std::string upload_temporary_;
};

class Service : public mojom::SmbService {
 public:
  Service(mojo::PendingReceiver<mojom::SmbService> receiver, base::OnceClosure quit)
      : receiver_(this, std::move(receiver)), worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE})), session_(std::make_shared<Session>()) {
    receiver_.set_disconnect_handler(std::move(quit));
  }
  ~Service() override { session_->StopWatch(); }
  void WatchDirectory(const std::string& server, const std::string& user, const std::string& password,
                      const std::string& domain, const std::string& share, const std::string& path,
                      WatchObserver observer) override {
    worker_->PostTask(FROM_HERE, base::BindOnce(&Session::StartWatch, session_,
        SmbCredentials{{server, share, path}, user, password, domain}, std::move(observer)));
  }
  void CopyFile(const std::string& source_share, const std::string& source, const std::string& destination_share, const std::string& destination,
                ProgressRemote progress, CopyFileCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Session::CopyFile, session_,
        source_share, source, destination_share, destination, std::move(progress)), std::move(callback));
  }
  void Discover(bool refresh, DiscoverCallback callback) override {
    if (!discovery_) discovery_ = std::make_unique<SmbDiscovery>(base::BindPostTaskToCurrentDefault(
        base::BindRepeating(&Service::Discovered, weak_.GetWeakPtr())));
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
  void NextPage(NextPageCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Session::NextPage, session_), std::move(callback));
  }
  void Download(const std::string& share, const std::string& path, base::File destination,
                Purpose purpose, ProgressRemote progress, DownloadCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Session::Download, session_, share, path,
        std::move(destination), purpose, std::move(progress)), std::move(callback));
  }
  void DeleteTree(const std::string& share, const std::string& path, DeleteTreeCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Session::DeleteTree, session_, share, path), std::move(callback));
  }
  void ContinueDelete(ContinueDeleteCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Session::ContinueDelete, session_), std::move(callback));
  }
  void CancelDelete(CancelDeleteCallback callback) override {
    worker_->PostTaskAndReply(FROM_HERE, base::BindOnce(&Session::CancelDelete, session_), std::move(callback));
  }
  void Mutate(const std::string& share, const std::string& path, mojom::SmbMutation operation,
              const std::string& target, MutateCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE,
        base::BindOnce(&Session::Mutate, session_, share, path, operation, target), std::move(callback));
  }
  void Upload(const std::string& share, const std::string& path, base::File source,
              Purpose purpose, ProgressRemote progress, UploadCallback callback) override {
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Session::Upload, session_, share, path,
        std::move(source), purpose, std::move(progress)), std::move(callback));
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
