// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRFILES_SHARING_NEARBY_SOCKET_H_
#define CRFILES_SHARING_NEARBY_SOCKET_H_

#include <utility>
#include <string>
#include <vector>
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "base/compiler_specific.h"
#include "base/files/file_util.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#if BUILDFLAG(IS_MAC)
#include "net/base/network_interfaces.h"
#endif

namespace crfiles::nearby {
#if BUILDFLAG(IS_WIN)
using Socket = SOCKET;
using SocketLength = int;
using PollFd = WSAPOLLFD;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
inline constexpr int kSendFlags = 0;
inline int SocketError() { return WSAGetLastError(); }
inline bool RetrySocket() {
  const int error = SocketError();
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
}
inline bool InitializeSockets() {
  // Process lifetime: discovery and identity workers can outlive their UI.
  static const bool initialized = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}
inline void CloseSocket(Socket socket) { closesocket(socket); }
inline bool NonBlocking(Socket socket) {
  u_long one = 1;
  return ioctlsocket(socket, FIONBIO, &one) == 0;
}
#else
using Socket = int;
using SocketLength = socklen_t;
using PollFd = pollfd;
inline constexpr Socket kInvalidSocket = -1;
#if BUILDFLAG(IS_APPLE)
// Darwin suppresses SIGPIPE on the socket, including accepted connections.
inline constexpr int kSendFlags = 0;
#else
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#endif
inline int SocketError() { return errno; }
inline bool RetrySocket() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
}
inline bool InitializeSockets() { return true; }
inline void CloseSocket(Socket socket) { close(socket); }
#endif

#if BUILDFLAG(IS_APPLE)
inline bool ConfigureAppleSocket(Socket socket) {
  const int one = 1;
  return base::SetNonBlocking(socket) && base::SetCloseOnExec(socket) &&
      setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0;
}
#endif

class ScopedSocket {
 public:
  ScopedSocket() = default;
  explicit ScopedSocket(Socket socket) : socket_(socket) {}
  ~ScopedSocket() { reset(); }
  ScopedSocket(ScopedSocket&& other) noexcept
      : socket_(std::exchange(other.socket_, kInvalidSocket)) {}
  ScopedSocket& operator=(ScopedSocket&& other) noexcept {
    if (this != &other) reset(std::exchange(other.socket_, kInvalidSocket));
    return *this;
  }
  Socket get() const { return socket_; }
  bool is_valid() const { return socket_ != kInvalidSocket; }
  void reset(Socket socket = kInvalidSocket) {
    if (is_valid()) CloseSocket(socket_);
    socket_ = socket;
  }
 private:
  Socket socket_ = kInvalidSocket;
};

inline ScopedSocket CreateSocket(int type) {
  if (!InitializeSockets()) return {};
#if BUILDFLAG(IS_WIN)
  ScopedSocket result(WSASocketW(AF_INET, type, 0, nullptr, 0,
                               WSA_FLAG_NO_HANDLE_INHERIT));
  if (result.is_valid() && !NonBlocking(result.get())) result.reset();
  if (result.is_valid() && type == SOCK_STREAM) {
    const int one = 1;
    if (setsockopt(result.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&one), sizeof(one)) != 0) result.reset();
  }
#elif BUILDFLAG(IS_APPLE)
  ScopedSocket result(socket(AF_INET, type, 0));
  if (result.is_valid() && !ConfigureAppleSocket(result.get())) result.reset();
#else
  ScopedSocket result(socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
#endif
  return result;
}
inline ScopedSocket Accept(Socket listener, sockaddr* address, SocketLength* length) {
#if BUILDFLAG(IS_WIN)
  ScopedSocket result(accept(listener, address, length));
  if (result.is_valid() && (!NonBlocking(result.get()) ||
      !SetHandleInformation(reinterpret_cast<HANDLE>(result.get()), HANDLE_FLAG_INHERIT, 0)))
    result.reset();
#elif BUILDFLAG(IS_APPLE)
  ScopedSocket result(accept(listener, address, length));
  if (result.is_valid() && !ConfigureAppleSocket(result.get())) result.reset();
#else
  ScopedSocket result(accept4(listener, address, length, SOCK_NONBLOCK | SOCK_CLOEXEC));
#endif
  return result;
}
inline int Poll(PollFd* fd, unsigned count, int timeout) {
  if (!count) {
    base::PlatformThread::Sleep(base::Milliseconds(timeout));
    return 0;
  }
#if BUILDFLAG(IS_WIN)
  return WSAPoll(fd, count, timeout);
#else
  return poll(fd, count, timeout);
#endif
}
template <typename T>
int SetOption(Socket socket, int level, int option, const T& value) {
  return setsockopt(socket, level, option,
                   reinterpret_cast<const char*>(&value), sizeof(value));
}
inline int PendingError(Socket socket) {
  int error = 0;
  SocketLength size = sizeof(error);
  return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0
      ? error : SocketError();
}
enum class InterfaceType { kOther, kWifi, kEthernet };
struct LocalInterface {
  in_addr address;
  InterfaceType type;
};

inline std::vector<LocalInterface> LocalInterfaces(bool require_multicast) {
  std::vector<LocalInterface> result;
#if BUILDFLAG(IS_WIN)
  ULONG size = 16 * 1024;
  std::vector<unsigned char> storage(size);
  ULONG status = ERROR_BUFFER_OVERFLOW;
  IP_ADAPTER_ADDRESSES* addresses = nullptr;
  for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
    if (size < sizeof(IP_ADAPTER_ADDRESSES)) return result;
    storage.resize(size);
    // SAFETY: storage owns at least sizeof(IP_ADAPTER_ADDRESSES) bytes and
    // its allocation is suitably aligned for this Windows structure. The API
    // receives exactly storage.size() bytes as its capacity. Refresh the pointer
    // after each resize, and traverse the returned list only after NO_ERROR,
    // while storage remains alive and is no longer resized.
    addresses = UNSAFE_BUFFERS(reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data()));
    status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER, nullptr, addresses, &size);
  }
  if (status != NO_ERROR) return result;
  for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        (require_multicast && (adapter->Flags & IP_ADAPTER_NO_MULTICAST))) continue;
    InterfaceType type = InterfaceType::kOther;
    if (!require_multicast) {
      if (adapter->IfType == IF_TYPE_IEEE80211) {
        type = InterfaceType::kWifi;
      } else if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = adapter->Luid;
        if (GetIfEntry2(&row) == NO_ERROR && row.InterfaceAndOperStatusFlags.HardwareInterface)
          type = InterfaceType::kEthernet;
      }
    }
    for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
      if (item->Address.lpSockaddr && item->Address.lpSockaddr->sa_family == AF_INET)
        result.push_back({reinterpret_cast<sockaddr_in*>(item->Address.lpSockaddr)->sin_addr, type});
    }
  }
#else
#if BUILDFLAG(IS_MAC)
  // Chromium queries Darwin's interface media types. /sys/class/net is Linux
  // specific; interface names such as en0 do not identify Wi-Fi vs Ethernet.
  net::NetworkInterfaceList networks;
  if (!require_multicast) {
    net::GetNetworkList(&networks, net::INCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES);
  }
#endif
  ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) != 0) return result;
  for (auto* item = addresses; item; item = item->ifa_next) {
    if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
        !(item->ifa_flags & IFF_UP) ||
        (require_multicast && !(item->ifa_flags & IFF_MULTICAST)) ||
        (item->ifa_flags & IFF_LOOPBACK)) continue;
    InterfaceType type = InterfaceType::kOther;
    if (!require_multicast && item->ifa_name) {
#if BUILDFLAG(IS_MAC)
      for (const auto& network : networks) {
        if (network.name != item->ifa_name) continue;
        if (network.type == net::NetworkChangeNotifier::CONNECTION_WIFI)
          type = InterfaceType::kWifi;
        else if (network.type == net::NetworkChangeNotifier::CONNECTION_ETHERNET)
          type = InterfaceType::kEthernet;
        break;
      }
#else
      const auto interface_path = base::FilePath("/sys/class/net").Append(item->ifa_name);
      if (base::DirectoryExists(interface_path.Append("wireless")) ||
          base::PathExists(interface_path.Append("phy80211"))) {
        type = InterfaceType::kWifi;
      } else if (base::PathExists(interface_path.Append("device"))) {
        std::string hardware_type;
        if (base::ReadFileToStringWithMaxSize(interface_path.Append("type"), &hardware_type, 32) &&
            (hardware_type == "1\n" || hardware_type == "1"))
          type = InterfaceType::kEthernet;
      }
#endif
    }
    result.push_back({reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr, type});
  }
  freeifaddrs(addresses);
#endif
  return result;
}
inline std::vector<in_addr> LocalAddresses() {
  std::vector<in_addr> result;
  for (const auto& entry : LocalInterfaces(true)) result.push_back(entry.address);
  return result;
}
}  // namespace crfiles::nearby

#endif  // CRFILES_SHARING_NEARBY_SOCKET_H_
