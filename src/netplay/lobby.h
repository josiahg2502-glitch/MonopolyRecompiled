// Pre-game lobbies, run by the launchers before the host starts the game.
//
// The host's launcher opens a LobbyHost on the game port; friends' launchers join with a LobbyClient,
// see who's there, chat and mark themselves ready. When the host starts, every client is told to
// launch the stream client (asking for its lobby seat) and the host's launcher hands the port to
// the game. Lobbies on the local network answer DiscoveryResponder broadcasts.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "netplay.h"

namespace netplay {

constexpr uint32_t kLobbyVersion = 1;

struct LobbyState {
  std::string lobby_name;
  bool present[kMaxPlayers] = {};  // seat 0 is the host
  bool ready[kMaxPlayers] = {};
  std::string names[kMaxPlayers];
};

struct ChatLine {
  int seat = -1;  // -1: lobby notice ("Bob joined")
  std::string name, text;
};

class LobbyHost {
 public:
  static std::unique_ptr<LobbyHost> Start(uint16_t port, std::string lobby_name, std::string host_name,
                                          std::string* error);
  virtual ~LobbyHost() = default;

  virtual LobbyState State() = 0;
  virtual std::vector<ChatLine> Chat() = 0;  // most recent last
  virtual void Say(const std::string& text) = 0;
  virtual void Kick(int seat) = 0;
  // Tells every player the game is starting, gives their launchers a moment to hang up, then
  // closes the port so the game can open it.
  virtual void StartGame() = 0;
};

class LobbyClient {
 public:
  enum class Status { kInLobby, kStarting, kClosed };

  // Fails with error == wire::kInGameReason ("in game") when the host is already playing.
  static std::unique_ptr<LobbyClient> Connect(const std::string& address, uint16_t port, std::string name,
                                              std::string* error);
  virtual ~LobbyClient() = default;

  virtual int seat() const = 0;  // 1..3
  virtual LobbyState State() = 0;
  virtual std::vector<ChatLine> Chat() = 0;
  virtual Status GetStatus(std::string* reason) = 0;  // reason: why the lobby closed
  virtual void SetReady(bool ready) = 0;
  virtual void Say(const std::string& text) = 0;
};

// ---- finding games on the local network --------------------------------------------------------

struct LobbyInfo {
  std::string address;  // where the answer came from
  uint16_t port = kDefaultPort;
  std::string lobby_name, host_name;
  int players = 0;
  bool in_game = false;  // the host is already playing; join with the stream client directly
  uint32_t session = 0;  // random per host, to merge answers that arrive over several routes
};

// Answers discovery broadcasts (UDP, kDefaultPort) for as long as it exists.
class DiscoveryResponder {
 public:
  static std::unique_ptr<DiscoveryResponder> Start(std::function<LobbyInfo()> info);
  virtual ~DiscoveryResponder() = default;
};

// Asks the local network (and this PC) who is hosting; waits `timeout` for answers.
std::vector<LobbyInfo> FindLobbies(std::chrono::milliseconds timeout);

// The LobbyInfo wire format shared by discovery answers and direct queries. Encoding stamps this
// process's session; decoding rejects other versions of the lobby protocol.
std::vector<uint8_t> EncodeLobbyInfo(const LobbyInfo& info);
std::optional<LobbyInfo> DecodeLobbyInfo(const uint8_t* data, size_t size);

// Asks the host at address:port directly (TCP) what it's hosting; nullopt if it doesn't answer in
// time. Lobbies and running games both answer, so this also proves the host is reachable.
std::optional<LobbyInfo> QueryLobby(const std::string& address, uint16_t port, std::chrono::milliseconds timeout);

// ---- join codes ---------------------------------------------------------------------------------

// A short code for an IPv4 address and port, e.g. "1C3KQ-7Y2MA", easier to pass around than an IP.
std::string MakeJoinCode(const std::string& ipv4, uint16_t port);
// Address and port from a join code; nullopt if `code` isn't one.
std::optional<std::pair<std::string, uint16_t>> ParseJoinCode(const std::string& code);

}  // namespace netplay
