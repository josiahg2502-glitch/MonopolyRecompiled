// Live check against the public BitTorrent DHT: announce under a test-only key (so nothing shows up
// in the real public games list), then look the key up again and find ourselves.
#include <cstdio>
#include <cstdlib>
#include <random>

#include "launcher/dht.h"

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

// SHA-1 of "MonopolyRecompiled dht selftest".
const uint8_t kTestKey[20] = {0x56, 0x3B, 0x25, 0x03, 0x82, 0xEC, 0x31, 0xFE, 0xE3, 0x58,
                              0x8A, 0x73, 0x2C, 0xCC, 0x87, 0x0B, 0xC8, 0x46, 0x68, 0xAF};

int main() {
  uint16_t port = uint16_t(40000 + std::random_device{}() % 20000);
  DhtResult a = DhtLookup(kTestKey, port, std::chrono::seconds(15));
  std::printf("announce: %d nodes answered, %d accepted, we are %s, %zu peers already there\n", a.responded,
              a.announced, a.external_ip.c_str(), a.peers.size());
  CHECK(a.responded > 0 && a.announced > 0 && !a.external_ip.empty());

  DhtResult b = DhtLookup(kTestKey, 0, std::chrono::seconds(15));
  bool found = false;
  for (auto& [ip, p] : b.peers) found |= ip == a.external_ip && p == port;
  std::printf("lookup: %d nodes answered, %zu peers, ourselves %s\n", b.responded, b.peers.size(),
              found ? "found" : "missing");
  CHECK(found);
  std::printf("dht selftest passed\n");
  return 0;
}
