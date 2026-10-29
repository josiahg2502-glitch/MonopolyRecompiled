// A small UPnP Internet Gateway Device client: SSDP discovery, then SOAP calls to the router.
// Written out instead of using Windows' NATUPnP, which silently fails when the "SSDP Discovery"
// service is disabled (as it often is).
#include "upnp.h"

#include <chrono>
#include <optional>
#include <vector>

#include "netplay/wire.h"

namespace {

using namespace netplay::wire;

struct Url {
  std::string host;
  uint16_t port = 80;
  std::string path = "/";
};

std::optional<Url> ParseUrl(const std::string& text) {
  if (!text.starts_with("http://")) return std::nullopt;
  std::string rest = text.substr(7);
  size_t slash = rest.find('/');
  std::string hostport = rest.substr(0, slash);
  Url url;
  url.path = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = hostport.rfind(':');
  url.host = hostport.substr(0, colon);
  if (colon != std::string::npos) url.port = uint16_t(atoi(hostport.c_str() + colon + 1));
  if (url.host.empty() || !url.port) return std::nullopt;
  return url;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(tolower(uint8_t(c)));
  return s;
}

// Text between `open` and `close` after `from`; empty if absent.
std::string Between(const std::string& s, const std::string& open, const std::string& close, size_t from = 0) {
  size_t a = s.find(open, from);
  if (a == std::string::npos) return {};
  a += open.size();
  size_t b = s.find(close, a);
  return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

// One HTTP/1.1 request to the router; returns the status code (0 on failure) and the body.
int Http(const Url& url, const std::string& method, const std::string& headers, const std::string& body,
         std::string* response) {
  std::string error;
  SOCKET s = Connect(url.host, url.port, &error);
  if (s == INVALID_SOCKET) return 0;
  SetRecvTimeout(s, 4000);
  std::string req = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
                    "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + headers +
                    "\r\n" + body;
  std::string raw;
  if (SendAll(s, req.data(), req.size())) {
    char buf[4096];
    for (int n; raw.size() < (256u << 10) && (n = recv(s, buf, sizeof(buf), 0)) > 0;) raw.append(buf, n);
  }
  closesocket(s);
  size_t split = raw.find("\r\n\r\n");
  if (!raw.starts_with("HTTP/1.") || split == std::string::npos || raw.size() < 12) return 0;
  int status = atoi(raw.c_str() + 9);
  std::string head = Lower(raw.substr(0, split));
  std::string rest = raw.substr(split + 4);
  if (head.find("transfer-encoding: chunked") != std::string::npos) {
    response->clear();
    for (size_t p = 0;;) {
      size_t eol = rest.find("\r\n", p);
      if (eol == std::string::npos) break;
      size_t n = strtoul(rest.c_str() + p, nullptr, 16);
      if (!n || eol + 2 + n > rest.size()) break;
      response->append(rest, eol + 2, n);
      p = eol + 2 + n + 2;
    }
  } else {
    *response = std::move(rest);
  }
  return status;
}

struct Gateway {
  Url control;
  std::string service;
};

// Finds the router's WAN connection service by SSDP multicast from `local_ip`'s interface.
std::optional<Gateway> Discover(const std::string& local_ip) {
  if (!StartWinsock()) return std::nullopt;
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return std::nullopt;
  sockaddr_in local{};
  local.sin_family = AF_INET;
  inet_pton(AF_INET, local_ip.c_str(), &local.sin_addr);
  bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));  // answers come back on this interface
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&local.sin_addr), sizeof(local.sin_addr));
  DWORD ttl = 2;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  sockaddr_in group{};
  group.sin_family = AF_INET;
  group.sin_port = htons(1900);
  inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
  for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                         "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
    std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n") +
                         "MX: 2\r\nST: " + st + "\r\n\r\n";
    sendto(s, search.data(), int(search.size()), 0, reinterpret_cast<sockaddr*>(&group), sizeof(group));
  }
  std::vector<std::string> locations;
  for (auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);;) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    SetRecvTimeout(s, DWORD(left));
    char buf[2048];
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAETIMEDOUT) break;
      continue;
    }
    std::string reply(buf, n), lower = Lower(reply);
    size_t at = lower.find("\r\nlocation:");
    if (at == std::string::npos) continue;
    std::string loc = reply.substr(at + 11, reply.find("\r\n", at + 11) - at - 11);
    loc.erase(0, loc.find_first_not_of(' '));
    if (std::find(locations.begin(), locations.end(), loc) == locations.end()) locations.push_back(loc);
    if (locations.size() >= 1) break;  // routers answer both searches with the same device
  }
  closesocket(s);

  static const char* kServices[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                                    "urn:schemas-upnp-org:service:WANIPConnection:1",
                                    "urn:schemas-upnp-org:service:WANPPPConnection:1"};
  for (const auto& location : locations) {
    auto url = ParseUrl(location);
    std::string desc;
    if (!url || Http(*url, "GET", "", "", &desc) != 200) continue;
    for (const char* service : kServices) {
      size_t at = desc.find(std::string("<serviceType>") + service + "</serviceType>");
      if (at == std::string::npos) continue;
      std::string control = Between(desc, "<controlURL>", "</controlURL>", at);
      if (control.empty()) continue;
      Gateway g;
      g.service = service;
      if (auto absolute = ParseUrl(control)) {
        g.control = *absolute;
      } else {
        g.control = *url;
        g.control.path = control.starts_with("/") ? control : "/" + control;
      }
      return g;
    }
  }
  return std::nullopt;
}

int Soap(const Gateway& g, const std::string& action, const std::string& args, std::string* response) {
  std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                     action + " xmlns:u=\"" + g.service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
  std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + g.service + "#" + action + "\"\r\n";
  return Http(g.control, "POST", headers, body, response);
}

std::string MappingKey(uint16_t port) {
  return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
         "</NewExternalPort><NewProtocol>TCP</NewProtocol>";
}

}  // namespace

RouterPort OpenRouterPort(uint16_t port, const std::string& local_ip, bool map) {
  RouterPort result;
  auto gateway = Discover(local_ip);
  if (!gateway) {
    result.error = "Your router didn't answer (UPnP may be turned off in its settings).";
    return result;
  }
  std::string response;
  if (Soap(*gateway, "GetExternalIPAddress", "", &response) == 200)
    result.external_ip = Between(response, "<NewExternalIPAddress>", "</NewExternalIPAddress>");
  if (!map) return result;

  Soap(*gateway, "DeletePortMapping", MappingKey(port), &response);  // a stale one from an earlier session
  for (int lease : {0, 86400}) {  // some routers only take timed leases
    std::string args = MappingKey(port) + "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort>" +
                       "<NewInternalClient>" + local_ip + "</NewInternalClient><NewEnabled>1</NewEnabled>" +
                       "<NewPortMappingDescription>Monopoly online</NewPortMappingDescription>" +
                       "<NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
    if (Soap(*gateway, "AddPortMapping", args, &response) == 200) {
      result.open = true;
      return result;
    }
  }
  std::string code = Between(response, "<errorCode>", "</errorCode>");
  result.error = "Your router refused to open port " + std::to_string(port) + (code.empty() ? "" : " (UPnP error " + code + ")") + ".";
  return result;
}

void CloseRouterPort(uint16_t port, const std::string& local_ip) {
  std::string response;
  if (auto gateway = Discover(local_ip)) Soap(*gateway, "DeletePortMapping", MappingKey(port), &response);
}

bool IsPrivateIPv4(const std::string& ip) {
  in_addr a;
  if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return true;
  uint32_t v = ntohl(a.s_addr);
  return (v >> 24) == 10 || (v >> 20) == (172u << 4 | 1) || (v >> 16) == (192u << 8 | 168) ||
         (v >> 22) == (100u << 2 | 1) || (v >> 24) == 127 || v == 0;
}
