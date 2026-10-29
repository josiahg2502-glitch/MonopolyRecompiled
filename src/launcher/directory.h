// Client for the public lobby directory (server/lobby_directory.py): lists open lobbies anyone can
// join, and keeps our own lobby listed while it's open.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct PublicLobby {
  std::string id, name, host, address;
  uint16_t port = 0;
  int players = 0;
  bool in_game = false;
  uint32_t version = 0;
  std::optional<bool> reachable;  // the directory's check that the internet can reach the host
};

// Blocking; run off the UI thread. `server` is the directory's base URL, e.g. https://example.org
// (the /v1/lobbies path is added here).
std::vector<PublicLobby> FetchPublicLobbies(const std::string& server, std::string* error);

struct ListingInfo {
  std::string name, host;
  int players = 1;
  bool in_game = false;
  uint16_t port = 0;
  bool operator==(const ListingInfo&) const = default;
};

struct ListingStatus {
  bool listed = false;
  std::string address;            // our internet address as the directory saw it
  std::optional<bool> reachable;  // can the internet reach our port?
  std::string error;
};

// Lists our lobby and refreshes it until destroyed (then removes it). Talks to the directory on
// its own thread; destruction never waits for the network.
class PublicListing {
 public:
  PublicListing(std::string server, ListingInfo info);
  ~PublicListing();
  void Update(const ListingInfo& info);
  ListingStatus Status();

 private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};

// Listings still being removed from the directory; the launcher waits for them before exiting.
int PendingListings();

// The JSON subset the directory speaks, exposed for the self-test.
std::optional<std::vector<PublicLobby>> ParseLobbyList(const std::string& json);
