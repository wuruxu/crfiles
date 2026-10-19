// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_process_singleton.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/hash/hash.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/message_loop/message_pump_type.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "crFiles/crfiles_trash.h"
#include "mojo/public/cpp/platform/named_platform_channel.h"
#include "mojo/public/cpp/platform/platform_channel_endpoint.h"
#include "mojo/public/cpp/platform/platform_channel_server.h"

#if BUILDFLAG(IS_MAC)
#include "net/base/net_errors.h"
#include "net/socket/stream_socket.h"
#include "net/socket/unix_domain_client_socket_posix.h"
#include "net/socket/unix_domain_server_socket_posix.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/process/process_handle.h"
#include "base/win/scoped_handle.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#endif

namespace crfiles {
namespace {

constexpr int kConnectionAttempts = 30;
constexpr base::TimeDelta kConnectionRetryDelay = base::Milliseconds(50);
constexpr base::TimeDelta kListenRetryDelay = base::Milliseconds(100);

#if BUILDFLAG(IS_WIN)
std::string GetSingletonIdentity(const base::FilePath& data_directory) {
  const uint32_t path_hash =
      base::PersistentHash(data_directory.AsUTF8Unsafe());
  return "org.crfiles.ProcessSingleton." + base::NumberToString(path_hash);
}
#endif

#if BUILDFLAG(IS_MAC)
base::FilePath GetMacSocketPath(const base::FilePath& data_directory) {
  // Container Application Support paths can exceed sockaddr_un::sun_path.
  // Keep the lock in the data directory and the socket in the user's temp dir.
  base::FilePath temporary;
  if (!base::GetTempDir(&temporary)) return {};
  return temporary.AppendASCII("crfiles-" +
      base::NumberToString(base::PersistentHash(data_directory.AsUTF8Unsafe())) + ".sock");
}
#endif

#if !BUILDFLAG(IS_MAC)
mojo::NamedPlatformChannel::ServerName GetServerName(
    const base::FilePath& data_directory) {
#if BUILDFLAG(IS_LINUX)
  return mojo::NamedPlatformChannel::ServerNameFromUTF8(
      data_directory.Append(FILE_PATH_LITERAL("singleton.sock"))
          .AsUTF8Unsafe());
#else
  return mojo::NamedPlatformChannel::ServerNameFromUTF8(
      GetSingletonIdentity(data_directory));
#endif
}
#endif

class IpcServer : public base::RefCountedThreadSafe<IpcServer> {
 public:
  IpcServer(const base::FilePath& data_directory,
            scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner,
            base::RepeatingClosure activate_callback)
      :
#if BUILDFLAG(IS_MAC)
        socket_path_(GetMacSocketPath(data_directory).AsUTF8Unsafe()),
#else
        server_name_(GetServerName(data_directory)),
#endif
        ui_task_runner_(std::move(ui_task_runner)),
        activate_callback_(std::move(activate_callback)) {}

  void Start() {
    if (!shutting_down_) {
      Listen();
    }
  }

  void Shutdown() {
    shutting_down_ = true;
    if (server_) {
      server_->Stop();
      server_.reset();
    }
#if BUILDFLAG(IS_MAC)
    accepted_socket_.reset();
    unix_domain_server_.reset();
#endif
  }

 private:
  friend class base::RefCountedThreadSafe<IpcServer>;
  ~IpcServer() = default;

  void Listen() {
    if (shutting_down_) {
      return;
    }
#if BUILDFLAG(IS_MAC)
    ListenOnMac();
#else
    mojo::NamedPlatformChannel::Options options;
    options.server_name = server_name_;
#if BUILDFLAG(IS_WIN)
    options.enforce_uniqueness = false;
    options.max_clients = 1;
#endif
    mojo::NamedPlatformChannel channel(options);
    mojo::PlatformChannelServerEndpoint endpoint =
        channel.TakeServerEndpoint();
    if (!endpoint.is_valid()) {
      ScheduleListenRetry();
      return;
    }
    server_ = std::make_unique<mojo::PlatformChannelServer>();
    if (!server_->Listen(
            std::move(endpoint),
            base::BindOnce(&IpcServer::OnConnected,
                           base::RetainedRef(this)))) {
      server_.reset();
      ScheduleListenRetry();
    }
#endif
  }

#if BUILDFLAG(IS_MAC)
  void ListenOnMac() {
    base::DeleteFile(base::FilePath::FromUTF8Unsafe(socket_path_));
    unix_domain_server_ = std::make_unique<net::UnixDomainServerSocket>(
        base::BindRepeating(
            [](const net::UnixDomainServerSocket::Credentials& credentials) {
              return credentials.user_id == geteuid();
            }),
        false);
    if (unix_domain_server_->BindAndListen(socket_path_, 5) != net::OK) {
      unix_domain_server_.reset();
      ScheduleListenRetry();
      return;
    }
    AcceptMacConnection();
  }

  void AcceptMacConnection() {
    const int result = unix_domain_server_->Accept(
        &accepted_socket_,
        base::BindOnce(&IpcServer::OnMacConnectionAccepted,
                       base::RetainedRef(this)));
    if (result != net::ERR_IO_PENDING) {
      OnMacConnectionAccepted(result);
    }
  }

  void OnMacConnectionAccepted(int result) {
    accepted_socket_.reset();
    if (shutting_down_) {
      return;
    }
    if (result == net::OK) {
      PostActivation();
      AcceptMacConnection();
      return;
    }
    unix_domain_server_.reset();
    ScheduleListenRetry();
  }
#endif

  void ScheduleListenRetry() {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&IpcServer::Listen, base::RetainedRef(this)),
        kListenRetryDelay);
  }

  void OnConnected(mojo::PlatformChannelEndpoint endpoint) {
    server_.reset();
    if (shutting_down_) {
      return;
    }
    if (endpoint.is_valid()) {
      PostActivation();
    }
    Listen();
  }

  void PostActivation() {
    ui_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::RepeatingClosure callback) { callback.Run(); },
            activate_callback_));
  }

#if BUILDFLAG(IS_MAC)
  const std::string socket_path_;
#else
  const mojo::NamedPlatformChannel::ServerName server_name_;
#endif
  const scoped_refptr<base::SingleThreadTaskRunner> ui_task_runner_;
  const base::RepeatingClosure activate_callback_;
  std::unique_ptr<mojo::PlatformChannelServer> server_;
#if BUILDFLAG(IS_MAC)
  std::unique_ptr<net::UnixDomainServerSocket> unix_domain_server_;
  std::unique_ptr<net::StreamSocket> accepted_socket_;
#endif
  bool shutting_down_ = false;
};

}  // namespace

class CrFilesProcessSingleton::Impl {
 public:
  explicit Impl(base::RepeatingClosure activate_callback)
      : activate_callback_(std::move(activate_callback)),
        ipc_thread_("crFilesProcessSingleton") {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    if (ipc_server_ && ipc_thread_.IsRunning()) {
      ipc_thread_.task_runner()->PostTask(
          FROM_HERE,
          base::BindOnce(&IpcServer::Shutdown, ipc_server_));
      ipc_thread_.Stop();
      ipc_server_.reset();
    }
#if BUILDFLAG(IS_MAC)
    if (is_primary_ && !data_directory_.empty()) {
      const auto path = GetMacSocketPath(data_directory_);
      if (!path.empty()) base::DeleteFile(path);
    }
#elif BUILDFLAG(IS_LINUX)
    if (is_primary_ && !data_directory_.empty()) {
      base::DeleteFile(
          data_directory_.Append(FILE_PATH_LITERAL("singleton.sock")));
    }
#elif BUILDFLAG(IS_WIN)
    if (is_primary_ && !data_directory_.empty()) {
      base::DeleteFile(
          data_directory_.Append(FILE_PATH_LITERAL("singleton.pid")));
    }
#endif
  }

  CrFilesProcessSingleton::Result NotifyOtherProcessOrCreate() {
    const auto trash_root = TrashRoot();
    data_directory_ = trash_root.empty() ? base::FilePath() : trash_root.DirName();
    if (data_directory_.empty() ||
        !base::CreateDirectory(data_directory_)) {
      LOG(ERROR) << "Unable to create the crFiles data directory";
      return CrFilesProcessSingleton::Result::kError;
    }

#if BUILDFLAG(IS_MAC)
    if (GetMacSocketPath(data_directory_).empty()) {
      LOG(ERROR) << "Unable to resolve the crFiles singleton socket directory";
      return CrFilesProcessSingleton::Result::kError;
    }
#endif
    const LockResult lock_result = TryAcquireLock();
    if (lock_result == LockResult::kError) {
      return CrFilesProcessSingleton::Result::kError;
    }
    if (lock_result == LockResult::kAlreadyLocked) {
      return NotifyExistingInstance()
                 ? CrFilesProcessSingleton::Result::kNotifiedExistingInstance
                 : CrFilesProcessSingleton::Result::kError;
    }

    is_primary_ = true;
#if BUILDFLAG(IS_WIN)
    if (!base::WriteFile(
            data_directory_.Append(FILE_PATH_LITERAL("singleton.pid")),
            base::NumberToString(base::GetCurrentProcId()))) {
      LOG(WARNING) << "Unable to write the crFiles singleton process ID";
    }
#endif
    if (!ipc_thread_.StartWithOptions(
            base::Thread::Options(base::MessagePumpType::IO, 0))) {
      LOG(ERROR) << "Unable to start the crFiles singleton IPC thread";
      return CrFilesProcessSingleton::Result::kError;
    }
    ipc_server_ = base::MakeRefCounted<IpcServer>(
        data_directory_,
        base::SingleThreadTaskRunner::GetCurrentDefault(),
        activate_callback_);
    ipc_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindOnce(&IpcServer::Start, ipc_server_));
    return CrFilesProcessSingleton::Result::kPrimary;
  }

 private:
  enum class LockResult {
    kAcquired,
    kAlreadyLocked,
    kError,
  };

  LockResult TryAcquireLock() {
#if BUILDFLAG(IS_WIN)
    const std::wstring mutex_name =
        L"Local\\" + base::UTF8ToWide(GetSingletonIdentity(data_directory_));
    ::SetLastError(ERROR_SUCCESS);
    base::win::ScopedHandle mutex(
        ::CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (!mutex.is_valid()) {
      PLOG(ERROR) << "Unable to create the crFiles singleton mutex";
      return LockResult::kError;
    }
    const bool already_locked = ::GetLastError() == ERROR_ALREADY_EXISTS;
    if (!already_locked) {
      mutex_ = std::move(mutex);
      return LockResult::kAcquired;
    }
    return LockResult::kAlreadyLocked;
#else
    const base::FilePath lock_path =
        data_directory_.Append(FILE_PATH_LITERAL("singleton.lock"));
    base::ScopedFD lock_fd(HANDLE_EINTR(open(
        lock_path.value().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)));
    if (!lock_fd.is_valid()) {
      PLOG(ERROR) << "Unable to open the crFiles singleton lock";
      return LockResult::kError;
    }
    if (HANDLE_EINTR(flock(lock_fd.get(), LOCK_EX | LOCK_NB)) == 0) {
      lock_fd_ = std::move(lock_fd);
      return LockResult::kAcquired;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return LockResult::kAlreadyLocked;
    }
    PLOG(ERROR) << "Unable to lock the crFiles singleton lock";
    return LockResult::kError;
#endif
  }

  bool NotifyExistingInstance() {
#if !BUILDFLAG(IS_MAC)
    const mojo::NamedPlatformChannel::ServerName server_name =
        GetServerName(data_directory_);
#endif
    for (int attempt = 0; attempt < kConnectionAttempts; ++attempt) {
#if BUILDFLAG(IS_WIN)
      std::string process_id_text;
      unsigned process_id = 0;
      if (base::ReadFileToString(
              data_directory_.Append(FILE_PATH_LITERAL("singleton.pid")),
              &process_id_text) &&
          base::StringToUint(process_id_text, &process_id)) {
        ::AllowSetForegroundWindow(process_id);
      }
#endif
#if BUILDFLAG(IS_MAC)
      net::UnixDomainClientSocket socket(
          GetMacSocketPath(data_directory_).AsUTF8Unsafe(),
          false);
      if (socket.Connect(base::DoNothing()) == net::OK) {
        return true;
      }
#else
      mojo::PlatformChannelEndpoint endpoint =
          mojo::NamedPlatformChannel::ConnectToServer(server_name);
      if (endpoint.is_valid()) {
        return true;
      }
#endif
      base::PlatformThread::Sleep(kConnectionRetryDelay);
    }
    LOG(ERROR) << "Unable to notify the existing crFiles instance";
    return false;
  }

  const base::RepeatingClosure activate_callback_;
  base::FilePath data_directory_;
  base::Thread ipc_thread_;
  scoped_refptr<IpcServer> ipc_server_;
  bool is_primary_ = false;
#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle mutex_;
#else
  base::ScopedFD lock_fd_;
#endif
};

CrFilesProcessSingleton::CrFilesProcessSingleton(
    base::RepeatingClosure activate_callback)
    : impl_(std::make_unique<Impl>(std::move(activate_callback))) {}

CrFilesProcessSingleton::~CrFilesProcessSingleton() = default;

CrFilesProcessSingleton::Result
CrFilesProcessSingleton::NotifyOtherProcessOrCreate() {
  return impl_->NotifyOtherProcessOrCreate();
}

}  // namespace crfiles
