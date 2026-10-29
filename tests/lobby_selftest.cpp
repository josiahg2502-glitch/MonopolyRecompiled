// Loopback test of lobbies: seats, ready, chat, kick, full lobby, start handoff, discovery, join codes.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "netplay/lobby.h"
#include "netplay/wire.h"

using namespace std::chrono_literals;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

template <typename F>
bool WaitFor(F&& cond, std::chrono::milliseconds timeout = 3000ms) {
  auto end = std::chrono::steady_clock::now() + timeout;
  while (!cond()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

static int Present(const netplay::LobbyState& s) {
  int n = 0;
  for (bool p : s.present) n += p;
  return n;
}

int main() {
  // Join codes
  auto code = netplay::MakeJoinCode("203.0.113.7", 27360);
  CHECK(code.size() == 11 && code[5] == '-');
  auto back = netplay::ParseJoinCode(code);
  CHECK(back && back->first == "203.0.113.7" && back->second == 27360);
  std::string sloppy;
  for (char c : code) sloppy += c == '0' ? 'o' : char(tolower(c));
  CHECK(netplay::ParseJoinCode(sloppy) == back);
  CHECK(!netplay::ParseJoinCode("192.168.1.20"));
  CHECK(!netplay::ParseJoinCode("hello"));
  CHECK(!netplay::ParseJoinCode("ZZZZZ-ZZZZZ"));  // more than 48 bits
  std::printf("join code %s\n", code.c_str());

  const uint16_t port = 27362;
  std::string error;
  auto host = netplay::LobbyHost::Start(port, "Test lobby", "Hostie", &error);
  if (!host) std::fprintf(stderr, "%s\n", error.c_str());
  CHECK(host);

  auto a = netplay::LobbyClient::Connect("127.0.0.1", port, "Alice", &error);
  CHECK(a && a->seat() == 1);
  CHECK(WaitFor([&] { return Present(a->State()) == 2 && a->State().names[0] == "Hostie"; }));
  CHECK(a->State().lobby_name == "Test lobby");

  a->SetReady(true);
  CHECK(WaitFor([&] { return host->State().ready[1]; }));
  a->Say("hi all\x01");
  CHECK(WaitFor([&] {
    auto chat = host->Chat();
    return !chat.empty() && chat.back().seat == 1 && chat.back().name == "Alice" && chat.back().text == "hi all?";
  }));
  host->Say("welcome");
  CHECK(WaitFor([&] {
    auto chat = a->Chat();
    return !chat.empty() && chat.back().seat == 0 && chat.back().text == "welcome";
  }));

  auto b = netplay::LobbyClient::Connect("localhost", port, "Bob", &error);
  auto c = netplay::LobbyClient::Connect("127.0.0.1", port, "Carol", &error);
  CHECK(b && b->seat() == 2 && c && c->seat() == 3);
  auto d = netplay::LobbyClient::Connect("127.0.0.1", port, "Dave", &error);
  CHECK(!d && error.find("full") != std::string::npos);
  CHECK(WaitFor([&] { return Present(host->State()) == 4; }));
  CHECK(!b->Chat().empty());  // newcomers get the conversation so far

  host->Kick(2);
  std::string reason;
  CHECK(WaitFor([&] { return b->GetStatus(&reason) == netplay::LobbyClient::Status::kClosed; }));
  CHECK(reason.find("removed") != std::string::npos);
  CHECK(WaitFor([&] { return !host->State().present[2] && Present(a->State()) == 3; }));
  b.reset();
  std::printf("lobby: seats, ready, chat, kick ok\n");

  // Discovery sees the lobby (on this PC via loopback, and on the LAN address if there is one).
  auto responder = netplay::DiscoveryResponder::Start([&] {
    netplay::LobbyInfo info;
    info.port = port;
    info.lobby_name = "Test lobby";
    info.host_name = "Hostie";
    info.players = Present(host->State());
    return info;
  });
  CHECK(responder);
  auto found = netplay::FindLobbies(600ms);
  bool seen = false;
  for (auto& f : found) seen |= f.lobby_name == "Test lobby" && f.port == port && f.players == 3 && !f.in_game;
  CHECK(seen);
  std::printf("discovery: %zu lobby(s), first at %s\n", found.size(), found.empty() ? "-" : found[0].address.c_str());
  responder.reset();

  // Asking the lobby directly, as players browsing public games do.
  auto asked = netplay::QueryLobby("127.0.0.1", port, 2000ms);
  CHECK(asked && asked->lobby_name == "Test lobby" && asked->host_name == "Hostie" && asked->players == 3 &&
        !asked->in_game && asked->port == port);
  CHECK(!netplay::QueryLobby("127.0.0.1", 1, 500ms));  // nobody there: fails fast
  CHECK(Present(host->State()) == 3);                  // the query didn't take a seat

  // Start: clients are told, hang up, and the port is free for the game right away.
  std::thread starter([&] { host->StartGame(); });
  CHECK(WaitFor([&] { return a->GetStatus(nullptr) == netplay::LobbyClient::Status::kStarting; }));
  CHECK(WaitFor([&] { return c->GetStatus(nullptr) == netplay::LobbyClient::Status::kStarting; }));
  a.reset();
  c.reset();
  starter.join();
  auto game = netplay::Host::Start(port, "Hostie", &error);
  if (!game) std::fprintf(stderr, "%s\n", error.c_str());
  CHECK(game);
  std::printf("start handoff ok\n");

  // A launcher that reaches a running game is told to use the stream client.
  auto late = netplay::LobbyClient::Connect("127.0.0.1", port, "Late", &error);
  CHECK(!late && error == netplay::wire::kInGameReason);
  game->SetInfoReply([] {
    netplay::LobbyInfo info;
    info.port = port;
    info.lobby_name = "Test lobby";
    info.host_name = "Hostie";
    info.players = 2;
    info.in_game = true;
    return netplay::EncodeLobbyInfo(info);
  });
  auto playing = netplay::QueryLobby("127.0.0.1", port, 2000ms);
  CHECK(playing && playing->in_game && playing->players == 2);
  host.reset();

  std::printf("lobby selftest passed\n");
  return 0;
}
