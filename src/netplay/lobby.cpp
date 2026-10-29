#include "lobby.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include "wire.h"

#include <iphlpapi.h>

namespace netplay {
namespace {

using namespace wire;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr uint8_t kWelcome = 21;  // h->c: u8 seat
constexpr uint8_t kState = 23;    // h->c: str lobby name, per seat: u8 present, u8 ready, str name
constexpr uint8_t kReady = 24;    // c->h: u8 ready
constexpr uint8_t kChat = 25;     // c->h: text; h->c: u8 seat (0xFF = notice), str name, text
constexpr uint8_t kStart = 26;    // h->c: launch the stream client now
// kLobbyHello (c->h: u32 kLobbyVersion, name) and kLobbyReject (h->c: reason) live in wire.h.

constexpr size_t kChatHistory = 100;
constexpr size_t kMaxChat = 200;

void PutStr(std::vector<uint8_t>& out, const std::string& s) {
  size_t n = std::min<size_t>(s.size(), 255);
  out.push_back(uint8_t(n));
  out.insert(out.end(), s.begin(), s.begin() + n);
}

bool GetStr(const std::vector<uint8_t>& in, size_t& p, std::string* s) {
  if (p >= in.size() || p + 1 + in[p] > in.size()) return false;
  s->assign(reinterpret_cast<const char*>(in.data() + p + 1), in[p]);
  p += 1 + in[p];
  return true;
}

std::vector<uint8_t> PackState(const LobbyState& st) {
  std::vector<uint8_t> out;
  PutStr(out, st.lobby_name);
  for (int i = 0; i < kMaxPlayers; ++i) {
    out.push_back(st.present[i]);
    out.push_back(st.ready[i]);
    PutStr(out, st.names[i]);
  }
  return out;
}

bool UnpackState(const std::vector<uint8_t>& in, LobbyState* st) {
  size_t p = 0;
  if (!GetStr(in, p, &st->lobby_name)) return false;
  for (int i = 0; i < kMaxPlayers; ++i) {
    if (p + 2 > in.size()) return false;
    st->present[i] = in[p] != 0;
    st->ready[i] = in[p + 1] != 0;
    p += 2;
    if (!GetStr(in, p, &st->names[i])) return false;
  }
  return true;
}

std::vector<uint8_t> PackChat(const ChatLine& line) {
  std::vector<uint8_t> out{uint8_t(line.seat < 0 ? 0xFF : line.seat)};
  PutStr(out, line.name);
  out.insert(out.end(), line.text.begin(), line.text.end());
  return out;
}

bool UnpackChat(const std::vector<uint8_t>& in, ChatLine* line) {
  if (in.empty()) return false;
  line->seat = in[0] == 0xFF ? -1 : in[0];
  size_t p = 1;
  if (!GetStr(in, p, &line->name)) return false;
  line->text = Sanitize(std::string(in.begin() + p, in.end()), kMaxChat, "");
  return true;
}

// ---------------------------------------------------------------------------------------------
// Lobby host

class LobbyHostImpl final : public LobbyHost {
 public:
  struct Peer {
    SOCKET socket = INVALID_SOCKET;
    int seat = 0;
    std::thread reader;
    std::mutex send_mutex;
    std::atomic<bool> joined{false};  // welcomed: may receive broadcasts
    std::atomic<bool> dead{false};
  };

  ~LobbyHostImpl() override { Shutdown(); }

  bool Start(uint16_t port, std::string lobby_name, std::string host_name, std::string* error) {
    state_.lobby_name = Sanitize(std::move(lobby_name), 40, "Monopoly");
    state_.present[0] = state_.ready[0] = true;
    state_.names[0] = Sanitize(std::move(host_name));
    listener_ = Listen(port, error);
    if (listener_ == INVALID_SOCKET) return false;
    acceptor_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  LobbyState State() override {
    std::lock_guard lock(state_mutex_);
    return state_;
  }

  std::vector<ChatLine> Chat() override {
    std::lock_guard lock(state_mutex_);
    return {chat_.begin(), chat_.end()};
  }

  void Say(const std::string& text) override { AddChat(0, text); }

  void Kick(int seat) override {
    for (auto& peer : Peers())
      if (peer->seat == seat && peer->joined) {
        static const std::string reason = "The host removed you from the lobby.";
        Send(*peer, kLobbyReject, reason.data(), reason.size());
        Abort(peer->socket);
      }
  }

  void StartGame() override {
    starting_ = true;
    Broadcast(kStart, nullptr, 0);
    // Clients hang up as soon as they hear kStart; closing first leaves TIME_WAIT on their side,
    // not on the port the game is about to listen on.
    for (auto end = Clock::now() + 1500ms; Clock::now() < end;) {
      bool any = false;
      for (auto& peer : Peers()) any |= !peer->dead;
      if (!any) break;
      std::this_thread::sleep_for(20ms);
    }
    Shutdown();
  }

 private:
  std::vector<std::shared_ptr<Peer>> Peers() {
    std::lock_guard lock(peers_mutex_);
    return peers_;
  }

  void Send(Peer& peer, uint8_t type, const void* data, size_t size) {
    std::lock_guard lock(peer.send_mutex);
    SendMsg(peer.socket, type, data, size);  // a failure surfaces as a disconnect in the reader
  }

  void Broadcast(uint8_t type, const void* data, size_t size) {
    for (auto& peer : Peers())
      if (peer->joined && !peer->dead) Send(*peer, type, data, size);
  }

  void BroadcastState() {
    auto bytes = PackState(State());
    Broadcast(kState, bytes.data(), bytes.size());
  }

  void AddChat(int seat, std::string text) {
    ChatLine line;
    {
      std::lock_guard lock(state_mutex_);
      line.seat = seat;
      line.text = Sanitize(std::move(text), kMaxChat, "");
      if (line.text.empty()) return;
      if (seat >= 0) line.name = state_.names[seat];
      chat_.push_back(line);
      if (chat_.size() > kChatHistory) chat_.pop_front();
    }
    auto bytes = PackChat(line);
    Broadcast(kChat, bytes.data(), bytes.size());
  }

  void AcceptLoop() {
    while (!stopped_) {
      SOCKET s = accept(listener_, nullptr, nullptr);
      if (s == INVALID_SOCKET) {
        if (!stopped_) std::this_thread::sleep_for(10ms);
        continue;
      }
      Reap();
      SetNoDelay(s);
      auto peer = std::make_shared<Peer>();
      peer->socket = s;
      std::lock_guard lock(peers_mutex_);
      peers_.push_back(peer);
      peer->reader = std::thread([this, peer] { ReadLoop(*peer); });
    }
  }

  void Reap() {
    std::vector<std::shared_ptr<Peer>> dead;
    {
      std::lock_guard lock(peers_mutex_);
      std::erase_if(peers_, [&](auto& peer) { return peer->dead && (dead.push_back(peer), true); });
    }
    for (auto& peer : dead) {
      if (peer->reader.joinable()) peer->reader.join();
      closesocket(peer->socket);
    }
  }

  void Shutdown() {
    if (stopped_.exchange(true)) return;
    closesocket(listener_);
    if (acceptor_.joinable()) acceptor_.join();
    std::vector<std::shared_ptr<Peer>> peers;
    {
      std::lock_guard lock(peers_mutex_);
      peers.swap(peers_);
    }
    for (auto& peer : peers) {
      Abort(peer->socket);
      if (peer->reader.joinable()) peer->reader.join();
      closesocket(peer->socket);
    }
  }

  void ReadLoop(Peer& peer) {
    uint8_t type;
    std::vector<uint8_t> payload;
    SetRecvTimeout(peer.socket, 5000);
    if (!ReadMessage(peer.socket, &type, &payload) || type != kLobbyHello || payload.size() < 4) {
      peer.dead = true;
      return;
    }
    SetRecvTimeout(peer.socket, 0);
    uint32_t version;
    memcpy(&version, payload.data(), 4);
    std::string name = Sanitize(std::string(payload.begin() + 4, payload.end()));
    std::string reject;
    {
      std::lock_guard lock(state_mutex_);
      if (version != kLobbyVersion) {
        reject = "This lobby is running a different version of the game.";
      } else if (starting_) {
        reject = "The game is already starting.";
      } else {
        for (int i = 1; i < kMaxPlayers && !peer.seat; ++i)
          if (!state_.present[i]) peer.seat = i;
        if (!peer.seat) reject = "The lobby is full.";
      }
      if (peer.seat) {
        state_.present[peer.seat] = true;
        state_.ready[peer.seat] = false;
        state_.names[peer.seat] = name;
      }
    }
    if (!reject.empty()) {
      Send(peer, kLobbyReject, reject.data(), reject.size());
      peer.dead = true;
      return;
    }
    uint8_t seat = uint8_t(peer.seat);
    Send(peer, kWelcome, &seat, 1);
    for (auto& line : Chat()) {  // the conversation so far
      auto bytes = PackChat(line);
      Send(peer, kChat, bytes.data(), bytes.size());
    }
    peer.joined = true;
    BroadcastState();
    AddChat(-1, name + " joined.");

    while (ReadMessage(peer.socket, &type, &payload)) {
      if (type == kReady && payload.size() == 1) {
        {
          std::lock_guard lock(state_mutex_);
          state_.ready[peer.seat] = payload[0] != 0;
        }
        BroadcastState();
      } else if (type == kChat) {
        AddChat(peer.seat, std::string(payload.begin(), payload.end()));
      }
    }
    {
      std::lock_guard lock(state_mutex_);
      state_.present[peer.seat] = state_.ready[peer.seat] = false;
      state_.names[peer.seat].clear();
    }
    if (!starting_) {
      BroadcastState();
      AddChat(-1, name + " left.");
    }
    peer.dead = true;  // last: Reap may join this thread once it is set
  }

  SOCKET listener_ = INVALID_SOCKET;
  std::thread acceptor_;
  std::atomic<bool> stopped_{false}, starting_{false};
  std::mutex peers_mutex_;
  std::vector<std::shared_ptr<Peer>> peers_;
  std::mutex state_mutex_;
  LobbyState state_;
  std::deque<ChatLine> chat_;
};

// ---------------------------------------------------------------------------------------------
// Lobby client

class LobbyClientImpl final : public LobbyClient {
 public:
  ~LobbyClientImpl() override {
    stopping_ = true;
    Abort(socket_);
    if (reader_.joinable()) reader_.join();
    closesocket(socket_);
  }

  bool Connect(const std::string& address, uint16_t port, std::string name, std::string* error) {
    socket_ = wire::Connect(address, port, error);
    if (socket_ == INVALID_SOCKET) return false;
    name = Sanitize(std::move(name));
    std::vector<uint8_t> hello(4 + name.size());
    memcpy(hello.data(), &kLobbyVersion, 4);
    memcpy(hello.data() + 4, name.data(), name.size());
    if (!SendMsg(socket_, kLobbyHello, hello.data(), hello.size())) return *error = "connection lost", false;
    SetRecvTimeout(socket_, 5000);
    uint8_t type;
    std::vector<uint8_t> payload;
    if (!ReadMessage(socket_, &type, &payload)) return *error = "no answer from " + address, false;
    if (type == kLobbyReject) return *error = std::string(payload.begin(), payload.end()), false;
    if (type != kWelcome || payload.size() != 1 || !payload[0] || payload[0] >= kMaxPlayers)
      return *error = address + " isn't a Monopoly lobby", false;
    seat_ = payload[0];
    SetRecvTimeout(socket_, 0);
    reader_ = std::thread([this] { ReadLoop(); });
    return true;
  }

  int seat() const override { return seat_; }

  LobbyState State() override {
    std::lock_guard lock(mutex_);
    return state_;
  }

  std::vector<ChatLine> Chat() override {
    std::lock_guard lock(mutex_);
    return {chat_.begin(), chat_.end()};
  }

  Status GetStatus(std::string* reason) override {
    std::lock_guard lock(mutex_);
    if (reason) *reason = reason_;
    return status_;
  }

  void SetReady(bool ready) override {
    uint8_t v = ready;
    std::lock_guard lock(send_mutex_);
    SendMsg(socket_, kReady, &v, 1);
  }

  void Say(const std::string& text) override {
    std::string t = Sanitize(text, kMaxChat, "");
    if (t.empty()) return;
    std::lock_guard lock(send_mutex_);
    SendMsg(socket_, kChat, t.data(), t.size());
  }

 private:
  void ReadLoop() {
    uint8_t type;
    std::vector<uint8_t> payload;
    while (ReadMessage(socket_, &type, &payload)) {
      std::lock_guard lock(mutex_);
      if (type == kState) {
        UnpackState(payload, &state_);
      } else if (type == kChat) {
        ChatLine line;
        if (UnpackChat(payload, &line)) chat_.push_back(line);
        if (chat_.size() > kChatHistory) chat_.pop_front();
      } else if (type == kStart) {
        status_ = Status::kStarting;
      } else if (type == kLobbyReject) {
        reason_ = Sanitize(std::string(payload.begin(), payload.end()), 120, "");
      }
    }
    std::lock_guard lock(mutex_);
    if (status_ == Status::kInLobby) {
      status_ = Status::kClosed;
      if (reason_.empty()) reason_ = stopping_ ? "You left the lobby." : "The host closed the lobby.";
    }
  }

  SOCKET socket_ = INVALID_SOCKET;
  int seat_ = 0;
  std::thread reader_;
  std::atomic<bool> stopping_{false};
  std::mutex send_mutex_;
  std::mutex mutex_;
  LobbyState state_;
  std::deque<ChatLine> chat_;
  Status status_ = Status::kInLobby;
  std::string reason_;
};

// ---------------------------------------------------------------------------------------------
// Discovery: "MNPLY?" + u32 version asks; "MNPLY!" + u32 version, u32 session, u16 port, u8 players,
// u8 in_game, str lobby name, str host name answers.

constexpr char kQuery[6] = {'M', 'N', 'P', 'L', 'Y', '?'};
constexpr char kAnswer[6] = {'M', 'N', 'P', 'L', 'Y', '!'};

class DiscoveryResponderImpl final : public DiscoveryResponder {
 public:
  ~DiscoveryResponderImpl() override {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    closesocket(socket_);
  }

  bool Start(std::function<LobbyInfo()> info) {
    info_ = std::move(info);
    session_ = std::random_device{}();
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) return false;
    BOOL on = TRUE;  // a lobby and a game on one PC both answer
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDefaultPort);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    SetRecvTimeout(socket_, 200);
    thread_ = std::thread([this] { Loop(); });
    return true;
  }

 private:
  void Loop() {
    char buf[64];
    while (!stopping_) {
      sockaddr_storage from{};
      int from_len = sizeof(from);
      int n = recvfrom(socket_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n < 10 || memcmp(buf, kQuery, 6) != 0) continue;
      LobbyInfo info = info_();
      std::vector<uint8_t> out(kAnswer, kAnswer + 6);
      auto put = [&](const void* p, size_t size) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + size); };
      put(&kLobbyVersion, 4);
      put(&session_, 4);
      put(&info.port, 2);
      out.push_back(uint8_t(info.players));
      out.push_back(info.in_game);
      PutStr(out, info.lobby_name);
      PutStr(out, info.host_name);
      sendto(socket_, reinterpret_cast<const char*>(out.data()), int(out.size()), 0,
             reinterpret_cast<sockaddr*>(&from), from_len);
    }
  }

  SOCKET socket_ = INVALID_SOCKET;
  std::function<LobbyInfo()> info_;
  uint32_t session_ = 0;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
};

// Directed broadcast addresses of the active IPv4 adapters (255.255.255.255 only leaves one of them).
std::vector<in_addr> BroadcastAddresses() {
  std::vector<in_addr> out;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  }
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR) return out;
  for (auto* a = adapters; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      uint8_t bits = u->OnLinkPrefixLength;
      if (bits == 0 || bits > 30) continue;
      uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
      in_addr b;
      b.s_addr = htonl(ip | (0xFFFFFFFFu >> bits));
      out.push_back(b);
    }
  }
  return out;
}

}  // namespace

std::unique_ptr<LobbyHost> LobbyHost::Start(uint16_t port, std::string lobby_name, std::string host_name,
                                            std::string* error) {
  if (!StartWinsock()) return *error = "WSAStartup failed", nullptr;
  auto host = std::make_unique<LobbyHostImpl>();
  return host->Start(port, std::move(lobby_name), std::move(host_name), error) ? std::move(host) : nullptr;
}

std::unique_ptr<LobbyClient> LobbyClient::Connect(const std::string& address, uint16_t port, std::string name,
                                                  std::string* error) {
  if (!StartWinsock()) return *error = "WSAStartup failed", nullptr;
  auto client = std::make_unique<LobbyClientImpl>();
  return client->Connect(address, port, std::move(name), error) ? std::move(client) : nullptr;
}

std::unique_ptr<DiscoveryResponder> DiscoveryResponder::Start(std::function<LobbyInfo()> info) {
  if (!StartWinsock()) return nullptr;
  auto responder = std::make_unique<DiscoveryResponderImpl>();
  return responder->Start(std::move(info)) ? std::move(responder) : nullptr;
}

std::vector<LobbyInfo> FindLobbies(std::chrono::milliseconds timeout) {
  std::vector<LobbyInfo> found;
  if (!StartWinsock()) return found;
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return found;
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
  std::vector<uint8_t> query(kQuery, kQuery + 6);
  query.insert(query.end(), reinterpret_cast<const uint8_t*>(&kLobbyVersion),
               reinterpret_cast<const uint8_t*>(&kLobbyVersion) + 4);
  auto targets = BroadcastAddresses();
  in_addr any;
  any.s_addr = htonl(INADDR_BROADCAST);
  targets.push_back(any);
  any.s_addr = htonl(INADDR_LOOPBACK);
  targets.push_back(any);
  for (in_addr t : targets) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kDefaultPort);
    to.sin_addr = t;
    sendto(s, reinterpret_cast<const char*>(query.data()), int(query.size()), 0, reinterpret_cast<sockaddr*>(&to),
           sizeof(to));
  }
  for (auto end = Clock::now() + timeout;;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
    if (left <= 0) break;
    SetRecvTimeout(s, DWORD(left));
    uint8_t buf[600];
    sockaddr_in from{};
    int from_len = sizeof(from);
    int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAETIMEDOUT) break;
      continue;  // e.g. WSAECONNRESET from an unanswered loopback query
    }
    std::vector<uint8_t> in(buf, buf + n);
    uint32_t version;
    LobbyInfo info;
    if (n < 18 || memcmp(buf, kAnswer, 6) != 0) continue;
    memcpy(&version, buf + 6, 4);
    memcpy(&info.session, buf + 10, 4);
    memcpy(&info.port, buf + 14, 2);
    info.players = buf[16];
    info.in_game = buf[17] != 0;
    size_t p = 18;
    if (version != kLobbyVersion || !GetStr(in, p, &info.lobby_name) || !GetStr(in, p, &info.host_name)) continue;
    info.lobby_name = Sanitize(info.lobby_name, 40, "Monopoly");
    info.host_name = Sanitize(info.host_name);
    char text[INET_ADDRSTRLEN];
    info.address = inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text)) ? text : "";
    bool loopback = (ntohl(from.sin_addr.s_addr) >> 24) == 127;
    auto same = std::find_if(found.begin(), found.end(), [&](auto& f) { return f.session == info.session; });
    if (same == found.end()) found.push_back(info);
    else if (!loopback) same->address = info.address;  // prefer the address other PCs can use too
  }
  closesocket(s);
  return found;
}

// ---- join codes: 48 bits (IPv4, port) as 10 Crockford base32 digits ------------------------------

namespace {
constexpr char kDigits[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
}

std::string MakeJoinCode(const std::string& ipv4, uint16_t port) {
  in_addr a;
  if (inet_pton(AF_INET, ipv4.c_str(), &a) != 1) return {};
  uint64_t v = (uint64_t(ntohl(a.s_addr)) << 16) | port;
  std::string code;
  for (int i = 0; i < 10; ++i) {
    if (i == 5) code += '-';
    code += kDigits[(v >> (45 - 5 * i)) & 31];
  }
  return code;
}

std::optional<std::pair<std::string, uint16_t>> ParseJoinCode(const std::string& code) {
  uint64_t v = 0;
  int digits = 0;
  for (char c : code) {
    if (c == '-' || c == ' ') continue;
    c = char(toupper(uint8_t(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const char* d = strchr(kDigits, c);
    if (!d || !c || ++digits > 10) return std::nullopt;
    v = v << 5 | uint64_t(d - kDigits);
  }
  if (digits != 10 || v >> 48 || !(v & 0xFFFF)) return std::nullopt;
  in_addr a;
  a.s_addr = htonl(uint32_t(v >> 16));
  char text[INET_ADDRSTRLEN];
  if (!inet_ntop(AF_INET, &a, text, sizeof(text))) return std::nullopt;
  return std::make_pair(std::string(text), uint16_t(v & 0xFFFF));
}

}  // namespace netplay
