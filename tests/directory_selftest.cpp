// Lobby directory client: JSON parsing, and (given a server URL) listing, refresh and removal.
//   directory_selftest                      parser checks only
//   directory_selftest http://127.0.0.1:27380   also against a running server/lobby_directory.py
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "launcher/directory.h"

using namespace std::chrono_literals;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

int main(int argc, char** argv) {
  auto list = ParseLobbyList(R"({"lobbies": [
      {"id": "a1", "name": "Ann's \"game\"", "host": "Ann", "players": 2, "in_game": false,
       "address": "203.0.113.7", "port": 27360, "version": 1, "reachable": true},
      {"id": "b2", "name": "Café", "host": "B", "players": 1, "in_game": true,
       "address": "198.51.100.1", "port": 1234, "version": 1, "reachable": null},
      {"id": "bad", "name": "no port", "address": "1.2.3.4", "port": 0}]})");
  CHECK(list && list->size() == 2);
  CHECK((*list)[0].name == "Ann's \"game\"" && (*list)[0].players == 2 && (*list)[0].reachable == true);
  CHECK((*list)[1].name == "Caf\xc3\xa9" && (*list)[1].in_game && !(*list)[1].reachable.has_value());
  CHECK(!ParseLobbyList("{\"lobbies\": [ }"));
  CHECK(!ParseLobbyList("[]"));
  CHECK(ParseLobbyList("{\"lobbies\": []}")->empty());
  std::printf("json ok\n");

  if (argc < 2) return 0;
  std::string server = argv[1], error;
  {
    PublicListing listing(server, {"Selftest lobby", "Tester", 1, false, 27399});
    auto end = std::chrono::steady_clock::now() + 10s;
    while (!listing.Status().listed && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(50ms);
    auto status = listing.Status();
    if (!status.error.empty()) std::fprintf(stderr, "%s\n", status.error.c_str());
    CHECK(status.listed && !status.address.empty());
    auto found = FetchPublicLobbies(server, &error);
    bool seen = false;
    for (auto& l : found) seen |= l.name == "Selftest lobby" && l.port == 27399;
    CHECK(seen && error.empty());

    listing.Update({"Selftest lobby", "Tester", 3, true, 27399});
    end = std::chrono::steady_clock::now() + 10s;
    for (seen = false; !seen && std::chrono::steady_clock::now() < end; std::this_thread::sleep_for(100ms))
      for (auto& l : FetchPublicLobbies(server, &error)) seen |= l.name == "Selftest lobby" && l.players == 3 && l.in_game;
    CHECK(seen);
    // Nothing listens on 27399, so the directory finds it unreachable.
    end = std::chrono::steady_clock::now() + 15s;
    while (listing.Status().reachable != false && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(100ms);
    CHECK(listing.Status().reachable == false);
  }
  auto end = std::chrono::steady_clock::now() + 10s;
  for (bool gone = false; !gone; std::this_thread::sleep_for(100ms)) {
    gone = true;
    for (auto& l : FetchPublicLobbies(server, &error)) gone &= l.name != "Selftest lobby";
    CHECK(std::chrono::steady_clock::now() < end);
  }
  CHECK(FetchPublicLobbies("http://127.0.0.1:1", &error).empty() && !error.empty());
  std::printf("directory selftest passed\n");
  return 0;
}
