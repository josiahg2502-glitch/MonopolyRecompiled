// Opening the game port on the home router with UPnP. Both calls block for a few seconds while the
// router is found; run them off the UI thread.
#pragma once

#include <cstdint>
#include <string>

struct RouterPort {
  bool open = false;        // the port is forwarded to this PC
  std::string external_ip;  // the router's internet address, when it told us
  std::string error;
};

// map = false only asks the router for its internet address.
RouterPort OpenRouterPort(uint16_t port, const std::string& local_ip, bool map = true);
void CloseRouterPort(uint16_t port, const std::string& local_ip);

// 10/8, 172.16/12, 192.168/16, 100.64/10 (carrier-grade NAT): not reachable from the internet.
bool IsPrivateIPv4(const std::string& ip);
