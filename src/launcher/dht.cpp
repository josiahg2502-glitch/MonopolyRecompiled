#include "dht.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include "netplay/wire.h"

// SHA-1 of "MonopolyRecompiled public lobbies v1".
const uint8_t kPublicLobbiesKey[20] = {0x3F, 0xB3, 0xC4, 0x87, 0x8D, 0xD3, 0x37, 0x83, 0x1B, 0x86,
                                       0x2C, 0xF8, 0x35, 0xBE, 0x3F, 0xD5, 0x64, 0x5B, 0x6B, 0x07};

namespace {

using namespace netplay::wire;
using Clock = std::chrono::steady_clock;

constexpr const char* kBootstrap[][2] = {{"router.bittorrent.com", "6881"},
                                         {"dht.transmissionbt.com", "6881"},
                                         {"router.utorrent.com", "6881"},
                                         {"dht.libtorrent.org", "25401"}};
constexpr size_t kK = 8;           // closest nodes that must answer before a lookup is done
constexpr int kParallel = 10;      // queries in flight
constexpr int kMaxQueries = 400;
constexpr auto kQueryTimeout = std::chrono::milliseconds(1500);

// ---- bencode -------------------------------------------------------------------------------------

struct Ben {
  enum Type { kInt, kStr, kList, kDict } type = kStr;
  int64_t i = 0;
  std::string s;
  std::vector<Ben> list;
  std::vector<std::pair<std::string, Ben>> dict;
  const Ben* Get(std::string_view key) const {
    for (auto& [k, v] : dict)
      if (k == key) return &v;
    return nullptr;
  }
  std::string Str(std::string_view key) const {
    auto* v = Get(key);
    return v && v->type == kStr ? v->s : std::string();
  }
};

bool Parse(const std::string& in, size_t& p, Ben& out, int depth = 0) {
  if (p >= in.size() || depth > 8) return false;
  char c = in[p];
  if (c == 'i') {
    size_t e = in.find('e', p);
    if (e == std::string::npos) return false;
    out.type = Ben::kInt;
    out.i = strtoll(in.c_str() + p + 1, nullptr, 10);
    p = e + 1;
    return true;
  }
  if (c == 'l' || c == 'd') {
    out.type = c == 'l' ? Ben::kList : Ben::kDict;
    ++p;
    while (p < in.size() && in[p] != 'e') {
      if (out.type == Ben::kDict) {
        Ben key;
        if (!Parse(in, p, key, depth + 1) || key.type != Ben::kStr) return false;
        out.dict.emplace_back(key.s, Ben{});
        if (!Parse(in, p, out.dict.back().second, depth + 1)) return false;
      } else {
        out.list.emplace_back();
        if (!Parse(in, p, out.list.back(), depth + 1)) return false;
      }
    }
    return p++ < in.size();
  }
  if (c < '0' || c > '9') return false;
  size_t colon = in.find(':', p);
  if (colon == std::string::npos) return false;
  size_t n = strtoull(in.c_str() + p, nullptr, 10);
  if (colon + 1 + n > in.size()) return false;
  out.type = Ben::kStr;
  out.s = in.substr(colon + 1, n);
  p = colon + 1 + n;
  return true;
}

std::string Str(std::string_view s) { return std::to_string(s.size()) + ":" + std::string(s); }

// ---- lookup ------------------------------------------------------------------------------------

struct Node {
  std::string id;  // 20 bytes once known
  sockaddr_in addr{};
  enum State { kNew, kSent, kDone, kFailed } state = kNew;
  Clock::time_point sent;
  std::string token;
};

bool Closer(const std::string& a, const std::string& b, const uint8_t* key) {
  if (a.size() != 20) return false;  // unknown ids (bootstrap routers) sort last
  if (b.size() != 20) return true;
  for (int i = 0; i < 20; ++i) {
    uint8_t x = uint8_t(a[i]) ^ key[i], y = uint8_t(b[i]) ^ key[i];
    if (x != y) return x < y;
  }
  return false;
}

std::string AddrText(const sockaddr_in& a) {
  char text[INET_ADDRSTRLEN];
  return inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text)) ? text : "";
}

// A public IPv4 address and a port: private, loopback and multicast addresses in DHT answers are noise.
bool Routable(const sockaddr_in& a) {
  uint32_t ip = ntohl(a.sin_addr.s_addr);
  return a.sin_port != 0 && ip != 0 && (ip >> 24) != 127 && (ip >> 24) != 10 && (ip >> 20) != 0xAC1 &&
         (ip >> 16) != 0xC0A8 && (ip >> 24) < 224;
}

}  // namespace

DhtResult DhtLookup(const uint8_t key[20], uint16_t announce_port, std::chrono::milliseconds budget) {
  DhtResult result;
  if (!StartWinsock()) return result;
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return result;
  sockaddr_in any{};
  any.sin_family = AF_INET;
  bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
  SetRecvTimeout(s, 100);

  std::mt19937 rng{std::random_device{}()};
  std::string my_id(20, '\0');
  for (char& c : my_id) c = char(rng());
  const std::string ih(reinterpret_cast<const char*>(key), 20);

  std::vector<Node> nodes;
  auto add = [&](const sockaddr_in& a, const std::string& id) {
    if (!Routable(a) || nodes.size() > 2000) return;
    for (auto& n : nodes)
      if (n.addr.sin_addr.s_addr == a.sin_addr.s_addr && n.addr.sin_port == a.sin_port) return;
    Node n;
    n.addr = a;
    n.id = id;
    nodes.push_back(n);
  };
  for (auto& [host, port] : kBootstrap) {
    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &list) != 0) continue;
    for (addrinfo* a = list; a; a = a->ai_next) add(*reinterpret_cast<sockaddr_in*>(a->ai_addr), {});
    freeaddrinfo(list);
  }

  auto send_to = [&](const Node& n, const std::string& msg) {
    sendto(s, msg.data(), int(msg.size()), 0, reinterpret_cast<const sockaddr*>(&n.addr), sizeof(n.addr));
  };
  auto tid = [](size_t i) { return std::string{char(i >> 8), char(i & 0xFF)}; };
  std::map<std::string, int> ip_votes;
  std::vector<std::pair<std::string, uint16_t>> peers;
  int queries = 0;
  const auto deadline = Clock::now() + budget;

  for (;;) {
    auto now = Clock::now();
    for (auto& n : nodes)
      if (n.state == Node::kSent && now - n.sent > kQueryTimeout) n.state = Node::kFailed;
    std::vector<size_t> order(nodes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      bool boot_a = nodes[a].id.empty(), boot_b = nodes[b].id.empty();  // routers first, then by distance
      if (boot_a != boot_b) return boot_a;
      return Closer(nodes[a].id, nodes[b].id, key);
    });
    // Done when the k closest nodes we know of have all answered (or failed).
    size_t settled = 0, inflight = 0;
    bool open_close = false;
    for (size_t i : order) {
      if (nodes[i].id.empty()) {
        if (nodes[i].state == Node::kSent) ++inflight;
        continue;
      }
      if (settled >= kK) break;
      if (nodes[i].state == Node::kDone) ++settled;
      else if (nodes[i].state != Node::kFailed) open_close = true;
    }
    for (auto& n : nodes) inflight += !n.id.empty() && n.state == Node::kSent;
    if (now > deadline || queries >= kMaxQueries || (settled >= kK && !open_close)) break;
    if (!inflight && std::none_of(nodes.begin(), nodes.end(), [](auto& n) { return n.state == Node::kNew; })) break;
    for (size_t i : order) {
      if (inflight >= kParallel) break;
      Node& n = nodes[i];
      if (n.state != Node::kNew) continue;
      send_to(n, "d1:ad2:id20:" + my_id + "9:info_hash20:" + ih + "e1:q9:get_peers1:t2:" + tid(i) + "1:y1:qe");
      n.state = Node::kSent;
      n.sent = now;
      ++inflight, ++queries;
    }

    char buf[2048];
    sockaddr_in from{};
    int from_len = sizeof(from);
    int got = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (got <= 0) continue;
    std::string msg(buf, got);
    Ben root;
    size_t p = 0;
    if (!Parse(msg, p, root) || root.type != Ben::kDict) continue;
    std::string t = root.Str("t");
    if (t.size() != 2 || root.Str("y") != "r") continue;
    size_t i = (uint8_t(t[0]) << 8) | uint8_t(t[1]);
    if (i >= nodes.size() || nodes[i].addr.sin_addr.s_addr != from.sin_addr.s_addr) continue;
    const Ben* r = root.Get("r");
    if (!r || r->type != Ben::kDict) continue;
    Node& n = nodes[i];
    if (n.state == Node::kDone) continue;
    n.state = Node::kDone;
    ++result.responded;
    if (std::string id = r->Str("id"); id.size() == 20) n.id = id;
    n.token = r->Str("token");
    if (std::string ip = root.Str("ip"); ip.size() == 6) {  // BEP 42: how this node sees us
      sockaddr_in me{};
      memcpy(&me.sin_addr, ip.data(), 4);
      ++ip_votes[AddrText(me)];
    }
    if (const Ben* values = r->Get("values"); values && values->type == Ben::kList)
      for (auto& v : values->list)
        if (v.type == Ben::kStr && v.s.size() == 6) {
          sockaddr_in peer{};
          memcpy(&peer.sin_addr, v.s.data(), 4);
          memcpy(&peer.sin_port, v.s.data() + 4, 2);
          std::pair<std::string, uint16_t> entry{AddrText(peer), ntohs(peer.sin_port)};
          if (Routable(peer) && std::find(peers.begin(), peers.end(), entry) == peers.end()) peers.push_back(entry);
        }
    std::string compact = r->Str("nodes");
    for (size_t at = 0; at + 26 <= compact.size(); at += 26) {
      sockaddr_in a{};
      a.sin_family = AF_INET;
      memcpy(&a.sin_addr, compact.data() + at + 20, 4);
      memcpy(&a.sin_port, compact.data() + at + 24, 2);
      add(a, compact.substr(at, 20));
    }
  }

  if (announce_port) {
    std::vector<size_t> done;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].state == Node::kDone && nodes[i].id.size() == 20 && !nodes[i].token.empty()) done.push_back(i);
    std::sort(done.begin(), done.end(), [&](size_t a, size_t b) { return Closer(nodes[a].id, nodes[b].id, key); });
    if (done.size() > kK) done.resize(kK);
    for (size_t k = 0; k < done.size(); ++k)  // transaction ids 0xFFxx mark announcements
      send_to(nodes[done[k]], "d1:ad2:id20:" + my_id + "12:implied_porti0e9:info_hash20:" + ih + "4:porti" +
                                  std::to_string(announce_port) + "e5:token" + Str(nodes[done[k]].token) +
                                  "e1:q13:announce_peer1:t2:" + tid(0xFF00 | k) + "1:y1:qe");
    for (auto end = Clock::now() + std::chrono::milliseconds(1500); Clock::now() < end && result.announced < int(done.size());) {
      char buf[512];
      int got = recv(s, buf, sizeof(buf), 0);
      if (got <= 0) continue;
      Ben root;
      size_t p = 0;
      std::string msg(buf, got);
      if (Parse(msg, p, root) && root.type == Ben::kDict && root.Str("y") == "r" && root.Str("t").size() == 2 &&
          uint8_t(root.Str("t")[0]) == 0xFF)
        ++result.announced;
    }
  }
  closesocket(s);

  int best = 0;
  for (auto& [ip, votes] : ip_votes)
    if (votes > best) best = votes, result.external_ip = ip;
  result.peers = std::move(peers);
  return result;
}

std::vector<netplay::LobbyInfo> FindPublicLobbies(std::string* error) {
  DhtResult dht = DhtLookup(kPublicLobbiesKey, 0, std::chrono::seconds(8));
  if (!dht.responded) {
    *error = "couldn't reach the peer-to-peer network (is UDP blocked on this network?)";
    return {};
  }
  error->clear();
  if (dht.peers.size() > 64) dht.peers.resize(64);
  // Ask every announced host at once; only those that answer are open. A host on this PC (or behind
  // our own router, which may not loop back) answers on the loopback address instead.
  std::vector<std::future<std::optional<netplay::LobbyInfo>>> asks;
  for (auto& [address, port] : dht.peers)
    asks.push_back(std::async(std::launch::async, [address, port, mine = address == dht.external_ip] {
      auto info = netplay::QueryLobby(address, port, std::chrono::milliseconds(2500));
      if (!info && mine) info = netplay::QueryLobby("127.0.0.1", port, std::chrono::milliseconds(1000));
      return info;  // its address is the one that answered, so joining uses it too
    }));
  std::vector<netplay::LobbyInfo> found;
  for (auto& ask : asks)
    if (auto info = ask.get())
      if (std::none_of(found.begin(), found.end(), [&](auto& f) { return f.session == info->session; }))
        found.push_back(*info);
  return found;
}

// ---- staying announced --------------------------------------------------------------------------

struct PublicAnnouncer::Shared {
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
  Status status;
};

PublicAnnouncer::PublicAnnouncer(uint16_t port) : shared_(std::make_shared<Shared>()) {
  std::thread([s = shared_, port] {
    for (;;) {
      DhtResult r = DhtLookup(kPublicLobbiesKey, port, std::chrono::seconds(12));
      std::unique_lock lock(s->mutex);
      if (s->stop) return;
      s->status.announced = r.announced > 0;
      if (!r.external_ip.empty()) s->status.external_ip = r.external_ip;
      s->status.error = r.responded ? (r.announced ? "" : "no node accepted the announcement")
                                    : "couldn't reach the peer-to-peer network (is UDP blocked?)";
      // Entries last about 30 minutes; refresh well before, or retry soon after a failure.
      s->cv.wait_for(lock, r.announced ? std::chrono::minutes(15) : std::chrono::seconds(30), [&] { return s->stop; });
      if (s->stop) return;
    }
  }).detach();
}

PublicAnnouncer::~PublicAnnouncer() {
  std::lock_guard lock(shared_->mutex);
  shared_->stop = true;
  shared_->cv.notify_all();
}

PublicAnnouncer::Status PublicAnnouncer::GetStatus() {
  std::lock_guard lock(shared_->mutex);
  return shared_->status;
}
