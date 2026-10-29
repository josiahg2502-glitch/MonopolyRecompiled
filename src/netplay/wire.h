// TCP framing and socket helpers shared by the game stream (netplay.cpp) and the lobby (lobby.cpp).
// A message is u32 payload length (little endian), u8 type, payload.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace netplay::wire {

// First message a launcher sends to join a lobby. A running game answers it with kInGameReason so
// the launcher knows to start the stream client instead.
constexpr uint8_t kLobbyHello = 20;
constexpr uint8_t kLobbyReject = 22;
inline const std::string kInGameReason = "in game";

constexpr uint32_t kMaxMessage = 8u << 20;

inline bool StartWinsock() {
  static const bool ok = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
}

inline bool SendAll(SOCKET s, const void* data, size_t size) {
  auto* p = static_cast<const char*>(data);
  while (size) {
    int n = send(s, p, int(std::min<size_t>(size, 1 << 20)), 0);
    if (n <= 0) return false;
    p += n, size -= size_t(n);
  }
  return true;
}

inline bool RecvAll(SOCKET s, void* data, size_t size) {
  auto* p = static_cast<char*>(data);
  while (size) {
    int n = recv(s, p, int(std::min<size_t>(size, 1 << 20)), 0);
    if (n <= 0) return false;
    p += n, size -= size_t(n);
  }
  return true;
}

inline std::vector<uint8_t> Pack(uint8_t type, const void* payload, size_t size) {
  std::vector<uint8_t> out(5 + size);
  uint32_t len = uint32_t(size);
  memcpy(out.data(), &len, 4);  // x86-64 only: little endian on the wire
  out[4] = type;
  if (size) memcpy(out.data() + 5, payload, size);
  return out;
}

inline bool ReadMessage(SOCKET s, uint8_t* type, std::vector<uint8_t>* payload) {
  uint8_t header[5];
  if (!RecvAll(s, header, 5)) return false;
  uint32_t len;
  memcpy(&len, header, 4);
  if (len > kMaxMessage) return false;
  *type = header[4];
  payload->resize(len);
  return len == 0 || RecvAll(s, payload->data(), len);
}

inline bool SendMsg(SOCKET s, uint8_t type, const void* payload, size_t size) {
  auto msg = Pack(type, payload, size);
  return SendAll(s, msg.data(), msg.size());
}

inline void SetNoDelay(SOCKET s) {
  BOOL on = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
  setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
}

// shutdown() alone doesn't wake a recv() blocked in another thread until the remote side closes.
inline void Abort(SOCKET s) {
  shutdown(s, SD_BOTH);
  CancelIoEx(reinterpret_cast<HANDLE>(s), nullptr);
}

inline void SetRecvTimeout(SOCKET s, DWORD ms) {
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

// Names and chat arrive from other players: no control characters, bounded length.
inline std::string Sanitize(std::string text, size_t max = 32, const char* fallback = "Player") {
  if (text.size() > max) text.resize(max);
  for (char& c : text)
    if (uint8_t(c) < 0x20) c = '?';
  return text.empty() ? fallback : text;
}

// Dual-stack (IPv4 + IPv6) listening socket.
inline SOCKET Listen(uint16_t port, std::string* error) {
  SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return *error = "socket() failed", INVALID_SOCKET;
  DWORD v6only = 0;
  setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);
  addr.sin6_addr = in6addr_any;
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 4) != 0) {
    closesocket(s);
    *error = "could not listen on port " + std::to_string(port) + " (is another game or lobby using it?)";
    return INVALID_SOCKET;
  }
  return s;
}

inline SOCKET Connect(const std::string& address, uint16_t port, std::string* error) {
  addrinfo hints{}, *list = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  if (getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &list) != 0)
    return *error = "could not find " + address, INVALID_SOCKET;
  SOCKET out = INVALID_SOCKET;
  for (addrinfo* a = list; a && out == INVALID_SOCKET; a = a->ai_next) {
    SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    if (connect(s, a->ai_addr, int(a->ai_addrlen)) == 0)
      out = s;
    else
      closesocket(s);
  }
  freeaddrinfo(list);
  if (out == INVALID_SOCKET) *error = "could not connect to " + address;
  else SetNoDelay(out);
  return out;
}

}  // namespace netplay::wire
