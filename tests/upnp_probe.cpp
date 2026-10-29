// upnp_probe <this PC's LAN address> [port]: finds the router over UPnP and prints its internet
// address. Read-only: it asks for no port mapping.
#include <cstdio>
#include <cstdlib>

#include "launcher/upnp.h"

int main(int argc, char** argv) {
  if (argc < 2) return std::fprintf(stderr, "usage: upnp_probe <lan address> [port]\n"), 2;
  RouterPort r = OpenRouterPort(argc > 2 ? uint16_t(std::atoi(argv[2])) : 27360, argv[1], false);
  if (!r.error.empty()) return std::printf("error: %s\n", r.error.c_str()), 1;
  if (r.external_ip.empty()) return std::printf("router found; it didn't say its internet address\n"), 0;
  std::printf("router found; internet address %s (%s)\n", r.external_ip.c_str(),
              IsPrivateIPv4(r.external_ip) ? "private: another router or CGNAT in front" : "public");
  return 0;
}
