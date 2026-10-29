// Public games without a server. Hosts announce themselves in the BitTorrent "mainline" DHT (BEP 5),
// the public peer-to-peer network torrent clients use to find each other, under a fixed key that
// only this port uses; players look the key up and then ask each host directly what it's hosting.
// Only addresses are exchanged, never files.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "netplay/lobby.h"

struct DhtResult {
  std::vector<std::pair<std::string, uint16_t>> peers;  // hosts announced under the key
  std::string external_ip;  // our internet address, as the DHT nodes saw it (BEP 42)
  int responded = 0;        // DHT nodes that answered; 0 means the network couldn't be reached
  int announced = 0;        // nodes that accepted our announcement
};

extern const uint8_t kPublicLobbiesKey[20];

// Blocking for up to `budget`. announce_port != 0 also announces this PC on that TCP port.
DhtResult DhtLookup(const uint8_t key[20], uint16_t announce_port, std::chrono::milliseconds budget);

// Every public lobby that answers right now (each host is asked directly, which also proves it's
// reachable). Blocking, several seconds.
std::vector<netplay::LobbyInfo> FindPublicLobbies(std::string* error);

// Keeps this PC announced (re-announcing every 15 minutes) until destroyed. DHT entries can't be
// withdrawn; they expire on their own, and players only see hosts that answer.
class PublicAnnouncer {
 public:
  struct Status {
    bool announced = false;
    std::string external_ip;
    std::string error;
  };
  explicit PublicAnnouncer(uint16_t port);
  ~PublicAnnouncer();
  Status GetStatus();

 private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};
