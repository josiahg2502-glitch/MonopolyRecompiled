#include "directory.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <variant>

#include "netplay/lobby.h"
#include "netplay/wire.h"

namespace {

// ---- minimal JSON (objects, arrays, strings, numbers, true/false/null) --------------------------

struct Json {
  std::variant<std::nullptr_t, bool, double, std::string, std::vector<Json>, std::map<std::string, Json>> v;
  const Json* Get(const std::string& key) const {
    auto* obj = std::get_if<std::map<std::string, Json>>(&v);
    if (!obj) return nullptr;
    auto it = obj->find(key);
    return it == obj->end() ? nullptr : &it->second;
  }
  std::string Str(const std::string& key) const {
    auto* j = Get(key);
    auto* s = j ? std::get_if<std::string>(&j->v) : nullptr;
    return s ? *s : std::string();
  }
  double Num(const std::string& key) const {
    auto* j = Get(key);
    auto* d = j ? std::get_if<double>(&j->v) : nullptr;
    return d ? *d : 0;
  }
  std::optional<bool> Bool(const std::string& key) const {
    auto* j = Get(key);
    auto* b = j ? std::get_if<bool>(&j->v) : nullptr;
    return b ? std::optional<bool>(*b) : std::nullopt;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& s) : s_(s) {}
  std::optional<Json> Parse() {
    auto v = Value(0);
    Skip();
    return v && p_ == s_.size() ? v : std::nullopt;
  }

 private:
  void Skip() {
    while (p_ < s_.size() && isspace(uint8_t(s_[p_]))) ++p_;
  }
  bool Eat(char c) {
    Skip();
    if (p_ < s_.size() && s_[p_] == c) return ++p_, true;
    return false;
  }
  std::optional<std::string> String() {
    if (!Eat('"')) return std::nullopt;
    std::string out;
    while (p_ < s_.size() && s_[p_] != '"') {
      char c = s_[p_++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p_ >= s_.size()) return std::nullopt;
      char e = s_[p_++];
      if (e == 'u') {
        if (p_ + 4 > s_.size()) return std::nullopt;
        unsigned cp = strtoul(s_.substr(p_, 4).c_str(), nullptr, 16);
        p_ += 4;
        if (cp >= 0xD800 && cp < 0xE000) cp = '?';  // shortcut: surrogate pairs become '?'; names rarely need them
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 63));
        else out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 63)), out += char(0x80 | (cp & 63));
      } else {
        out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
      }
    }
    if (p_ >= s_.size()) return std::nullopt;
    ++p_;
    return out;
  }
  std::optional<Json> Value(int depth) {
    if (depth > 16) return std::nullopt;
    Skip();
    if (p_ >= s_.size()) return std::nullopt;
    char c = s_[p_];
    if (c == '{') {
      ++p_;
      std::map<std::string, Json> obj;
      if (Eat('}')) return Json{obj};
      do {
        auto key = String();
        if (!key || !Eat(':')) return std::nullopt;
        auto val = Value(depth + 1);
        if (!val) return std::nullopt;
        obj[*key] = std::move(*val);
      } while (Eat(','));
      return Eat('}') ? std::optional<Json>(Json{std::move(obj)}) : std::nullopt;
    }
    if (c == '[') {
      ++p_;
      std::vector<Json> arr;
      if (Eat(']')) return Json{arr};
      do {
        auto val = Value(depth + 1);
        if (!val) return std::nullopt;
        arr.push_back(std::move(*val));
      } while (Eat(','));
      return Eat(']') ? std::optional<Json>(Json{std::move(arr)}) : std::nullopt;
    }
    if (c == '"') {
      auto str = String();
      return str ? std::optional<Json>(Json{*str}) : std::nullopt;
    }
    for (auto [word, value] : {std::pair{"true", Json{true}}, std::pair{"false", Json{false}}, std::pair{"null", Json{nullptr}}})
      if (s_.compare(p_, strlen(word), word) == 0) return p_ += strlen(word), value;
    char* end = nullptr;
    double d = strtod(s_.c_str() + p_, &end);
    if (end == s_.c_str() + p_) return std::nullopt;
    p_ = size_t(end - s_.c_str());
    return Json{d};
  }

  const std::string& s_;
  size_t p_ = 0;
};

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += char(c);
    else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else out += char(c);
  }
  return out + "\"";
}

std::string ListingJson(const ListingInfo& i) {
  return "{\"name\":" + Quote(i.name) + ",\"host\":" + Quote(i.host) + ",\"players\":" + std::to_string(i.players) +
         ",\"in_game\":" + (i.in_game ? "true" : "false") + ",\"port\":" + std::to_string(i.port) +
         ",\"version\":" + std::to_string(netplay::kLobbyVersion) + "}";
}

// ---- HTTP(S) through WinHTTP --------------------------------------------------------------------

struct Response {
  int status = 0;  // 0: no answer
  std::string body;
  std::string error;
};

std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

Response Request(const std::string& method, const std::string& url, const std::string& body = {},
                 const std::string& token = {}) {
  Response r;
  std::wstring wurl = Wide(url);
  wchar_t host[256], path[2048];
  URL_COMPONENTS uc{sizeof(uc)};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return r.error = "the lobby server address isn't a valid URL", r;
  struct Handle {
    HINTERNET h;
    ~Handle() {
      if (h) WinHttpCloseHandle(h);
    }
  };
  Handle session{WinHttpOpen(L"MonopolyLauncher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h) return r.error = "WinHTTP isn't available", r;
  WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 5000);
  Handle connect{WinHttpConnect(session.h, host, uc.nPort, 0)};
  Handle req{connect.h ? WinHttpOpenRequest(connect.h, Wide(method).c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                       : nullptr};
  std::wstring headers = L"Content-Type: application/json\r\n";
  if (!token.empty()) headers += L"Authorization: Bearer " + Wide(token) + L"\r\n";
  if (!req.h ||
      !WinHttpSendRequest(req.h, headers.c_str(), DWORD(-1), body.empty() ? WINHTTP_NO_REQUEST_DATA : (void*)body.data(),
                          DWORD(body.size()), DWORD(body.size()), 0) ||
      !WinHttpReceiveResponse(req.h, nullptr))
    return r.error = "couldn't reach the lobby server", r;
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  r.status = int(status);
  for (DWORD avail = 0; WinHttpQueryDataAvailable(req.h, &avail) && avail && r.body.size() < (1u << 20);) {
    std::string chunk(avail, '\0');
    DWORD got = 0;
    if (!WinHttpReadData(req.h, chunk.data(), avail, &got) || !got) break;
    r.body.append(chunk, 0, got);
  }
  return r;
}

std::string Endpoint(std::string server) {
  while (!server.empty() && server.back() == '/') server.pop_back();
  return server + "/v1/lobbies";
}

}  // namespace

std::optional<std::vector<PublicLobby>> ParseLobbyList(const std::string& text) {
  auto root = JsonParser(text).Parse();
  const Json* list = root ? root->Get("lobbies") : nullptr;
  auto* arr = list ? std::get_if<std::vector<Json>>(&list->v) : nullptr;
  if (!arr) return std::nullopt;
  std::vector<PublicLobby> out;
  for (const Json& j : *arr) {
    PublicLobby l;
    l.id = j.Str("id");
    l.name = netplay::wire::Sanitize(j.Str("name"), 40, "Monopoly");
    l.host = netplay::wire::Sanitize(j.Str("host"));
    l.address = j.Str("address");
    double port = j.Num("port");
    if (l.address.empty() || port < 1 || port > 65535) continue;
    l.port = uint16_t(port);
    l.players = int(j.Num("players"));
    l.in_game = j.Bool("in_game").value_or(false);
    l.version = uint32_t(j.Num("version"));
    l.reachable = j.Bool("reachable");
    out.push_back(std::move(l));
  }
  return out;
}

std::vector<PublicLobby> FetchPublicLobbies(const std::string& server, std::string* error) {
  Response r = Request("GET", Endpoint(server));
  if (r.status != 200) {
    *error = r.error.empty() ? "the lobby server answered " + std::to_string(r.status) : r.error;
    return {};
  }
  auto lobbies = ParseLobbyList(r.body);
  if (!lobbies) *error = "the lobby server sent something unexpected";
  return lobbies.value_or(std::vector<PublicLobby>{});
}

// ---- keeping our lobby listed --------------------------------------------------------------------

std::atomic<int> g_pending_listings{0};

int PendingListings() { return g_pending_listings; }

struct PublicListing::Shared {
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false, changed = true;
  ListingInfo info;
  ListingStatus status;
};

PublicListing::PublicListing(std::string server, ListingInfo info) : shared_(std::make_shared<Shared>()) {
  shared_->info = std::move(info);
  ++g_pending_listings;
  std::thread([s = shared_, url = Endpoint(server)] {
    std::string id, token;
    for (;;) {
      ListingInfo info;
      {
        std::unique_lock lock(s->mutex);
        s->cv.wait_for(lock, std::chrono::seconds(20), [&] { return s->stop || s->changed; });
        if (s->stop) break;
        s->changed = false;
        info = s->info;
      }
      Response r;
      if (!id.empty()) {
        r = Request("PUT", url + "/" + id, ListingJson(info), token);
        if (r.status == 404) id.clear();  // expired (e.g. the PC slept): list it again
      }
      if (id.empty()) {
        r = Request("POST", url, ListingJson(info));
        auto j = r.status == 201 ? JsonParser(r.body).Parse() : std::nullopt;
        if (j) id = j->Str("id"), token = j->Str("token");
      }
      auto j = (r.status == 200 || r.status == 201) ? JsonParser(r.body).Parse() : std::nullopt;
      bool checking;
      {
        std::lock_guard lock(s->mutex);
        s->status.listed = j.has_value() && !id.empty();
        if (j) {
          s->status.address = j->Str("address");
          s->status.reachable = j->Bool("reachable");
          s->status.error.clear();
        } else {
          auto err = r.status ? JsonParser(r.body).Parse() : std::nullopt;
          std::string why = err ? err->Str("error") : "";
          s->status.error = !r.error.empty() ? r.error
                            : !why.empty()   ? "the lobby server said: " + why
                                             : "the lobby server answered " + std::to_string(r.status);
        }
        // The directory is still testing whether the internet can reach us: ask again shortly.
        checking = s->status.listed && !s->status.reachable.has_value();
        if (checking) s->changed = true;
      }
      if (checking) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!id.empty()) Request("DELETE", url + "/" + id, {}, token);
    --g_pending_listings;
  }).detach();
}

PublicListing::~PublicListing() {
  std::lock_guard lock(shared_->mutex);
  shared_->stop = true;
  shared_->cv.notify_all();
}

void PublicListing::Update(const ListingInfo& info) {
  std::lock_guard lock(shared_->mutex);
  if (info == shared_->info) return;
  shared_->info = info;
  shared_->changed = true;
  shared_->cv.notify_all();
}

ListingStatus PublicListing::Status() {
  std::lock_guard lock(shared_->mutex);
  return shared_->status;
}
