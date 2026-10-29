// MonopolyLauncher: choose Play / Host online / Join online, edit settings, then start
// monopoly.exe or monopoly_net.exe and come back when it closes.
//
// Settings the game understands live in monopoly.toml next to the exe (the game reads that file
// itself, so they also apply when monopoly.exe is started directly). Launcher-only state lives in
// launcher.toml. Rendering is Dear ImGui on Direct3D 9's fixed-function pipeline: no shaders.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d9.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "misc/cpp/imgui_stdlib.h"
#include "netplay/lobby.h"
#include "netplay/wire.h"
#include "dht.h"
#include "install.h"
#include "upnp.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

namespace fs = std::filesystem;

constexpr uint16_t kDefaultPort = netplay::kDefaultPort;
constexpr const char* kGameFiles[] = {"default.xex", "data.big", "movies.big", "movies2.big", "music.big"};

// ---- text and paths -----------------------------------------------------------------------------

std::string Utf8(std::wstring_view w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Wide(std::string_view s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

fs::path ExeDir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(buf).parent_path();
}

// Player names go into the lobby and into TOML: printable, no quotes or backslashes, <= 32 bytes.
// trim = false while the player is still typing (so spaces between words survive).
std::string SanitizeName(std::string_view in, bool trim = true) {
  std::string out;
  for (unsigned char c : in)
    if (c >= 0x20 && c != '"' && c != '\\') out += char(c);
  while (trim && !out.empty() && out.back() == ' ') out.pop_back();
  while (trim && !out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.size() > 32) {
    out.resize(32);
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) out.pop_back();
  }
  return out;
}

// Windows command-line quoting (the rules CommandLineToArgvW undoes).
std::wstring QuoteArg(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < a.size() && a[i] == L'\\') ++i, ++slashes;
    if (i == a.size()) {
      out.append(slashes * 2, L'\\');
      break;
    }
    out.append(a[i] == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    out += a[i];
  }
  return out + L"\"";
}

// ---- flat TOML ----------------------------------------------------------------------------------

// Reads and edits top-level `key = value` lines (the format the game's cvar loader saves);
// every other line is kept as it is.
class FlatToml {
 public:
  explicit FlatToml(fs::path path) : path_(std::move(path)) {
    std::ifstream in(path_);
    for (std::string line; std::getline(in, line);) lines_.push_back(line);
  }

  std::optional<std::string> Get(std::string_view key) const {
    for (size_t i = 0; i < TopLevelEnd(); ++i)
      if (auto v = ValueOf(lines_[i], key)) return v;
    return std::nullopt;
  }
  std::string GetString(std::string_view key, std::string fallback) const { return Get(key).value_or(fallback); }
  bool GetBool(std::string_view key, bool fallback) const {
    auto v = Get(key);
    return v ? *v == "true" : fallback;
  }
  int GetInt(std::string_view key, int fallback) const {
    auto v = Get(key);
    if (!v) return fallback;
    char* end = nullptr;
    long n = strtol(v->c_str(), &end, 10);
    return end && *end == '\0' && !v->empty() ? int(n) : fallback;
  }

  void Set(std::string_view key, const std::string& toml_value) {
    std::string line = std::string(key) + " = " + toml_value;
    for (size_t i = 0; i < TopLevelEnd(); ++i)
      if (ValueOf(lines_[i], key)) return void(lines_[i] = line);
    lines_.insert(lines_.begin(), line);  // above any [table], so the key stays top-level
  }
  void SetString(std::string_view key, std::string_view v) {
    std::string q = "\"";
    for (char c : v) q += (c == '"' || c == '\\') ? std::string{'\\', c} : std::string{c};
    Set(key, q + "\"");
  }
  void SetBool(std::string_view key, bool v) { Set(key, v ? "true" : "false"); }
  void SetInt(std::string_view key, int v) { Set(key, std::to_string(v)); }

  bool Save() const {
    std::ofstream out(path_, std::ios::trunc);
    for (auto& l : lines_) out << l << '\n';
    return bool(out);
  }

 private:
  size_t TopLevelEnd() const {
    for (size_t i = 0; i < lines_.size(); ++i) {
      auto p = lines_[i].find_first_not_of(" \t");
      if (p != std::string::npos && lines_[i][p] == '[') return i;
    }
    return lines_.size();
  }

  static std::optional<std::string> ValueOf(const std::string& line, std::string_view key) {
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos || line.compare(p, key.size(), key) != 0) return std::nullopt;
    p = line.find_first_not_of(" \t", p + key.size());
    if (p == std::string::npos || line[p] != '=') return std::nullopt;
    p = line.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) return std::string();
    if (line[p] == '"') {
      std::string out;
      for (size_t i = p + 1; i < line.size() && line[i] != '"'; ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
          char e = line[++i];
          out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        } else {
          out += line[i];
        }
      }
      return out;
    }
    std::string v = line.substr(p, line.find('#', p) - p);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    return v;
  }

  fs::path path_;
  std::vector<std::string> lines_;
};

// ---- settings -----------------------------------------------------------------------------------

struct Settings {
  bool fullscreen = true;
  bool vsync = true;
  int scale = 1;  // draw resolution scale: 1 = 1280x720
  std::string name;
  int port = kDefaultPort;
  std::string game_folder;  // empty: "game" next to the exe
  std::string join_address;
  std::string lobby_name;  // empty: "<name>'s game"
  bool public_lobby = true;  // announce hosted lobbies in Public games
  bool upnp = true;          // ask the router to forward the port
  int page = 0;
};

std::string DefaultName() {
  wchar_t buf[64];
  DWORD n = 64;
  std::string name = GetUserNameW(buf, &n) ? SanitizeName(Utf8(buf)) : "";
  return name.empty() ? "Player" : name;
}

Settings LoadSettings() {
  FlatToml game(ExeDir() / "monopoly.toml"), launcher(ExeDir() / "launcher.toml");
  Settings s;
  s.fullscreen = game.GetBool("fullscreen", true);
  s.vsync = game.GetBool("vsync", true);
  s.scale = std::clamp(game.GetInt("resolution_scale", 1), 1, 3);
  s.name = SanitizeName(game.GetString("net_name", ""));
  if (s.name.empty()) s.name = DefaultName();
  s.port = std::clamp(game.GetInt("net_port", kDefaultPort), 1, 65535);
  s.game_folder = launcher.GetString("game_folder", "");
  s.join_address = launcher.GetString("join_address", "");
  s.lobby_name = SanitizeName(launcher.GetString("lobby_name", ""));
  s.public_lobby = launcher.GetBool("public_lobby", true);
  s.upnp = launcher.GetBool("upnp", true);
  s.page = std::clamp(launcher.GetInt("page", 0), 0, 3);
  return s;
}

bool SaveSettings(const Settings& s) {
  FlatToml game(ExeDir() / "monopoly.toml"), launcher(ExeDir() / "launcher.toml");
  game.SetBool("fullscreen", s.fullscreen);
  game.SetBool("vsync", s.vsync);
  game.SetInt("resolution_scale", s.scale);
  game.SetString("net_name", s.name);
  game.SetInt("net_port", s.port);
  launcher.SetString("game_folder", s.game_folder);
  launcher.SetString("join_address", s.join_address);
  launcher.SetString("lobby_name", s.lobby_name);
  launcher.SetBool("public_lobby", s.public_lobby);
  launcher.SetBool("upnp", s.upnp);
  launcher.SetInt("page", s.page);
  return game.Save() && launcher.Save();
}

fs::path GameDir(const Settings& s) { return s.game_folder.empty() ? ExeDir() / "game" : fs::path(Wide(s.game_folder)); }

std::vector<std::string> MissingGameFiles(const fs::path& dir) {
  std::vector<std::string> missing;
  std::error_code ec;
  for (const char* f : kGameFiles)
    if (!fs::is_regular_file(dir / f, ec)) missing.push_back(f);
  return missing;
}

// "host", "host:port", or a bare IPv6 literal.
bool ValidJoinAddress(const std::string& a) {
  if (a.empty() || a.find_first_of(" \t\"") != std::string::npos) return false;
  auto colon = a.rfind(':');
  if (colon == std::string::npos || a.find(':') != colon) return true;
  std::string port = a.substr(colon + 1);
  if (colon == 0 || port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
    return false;
  int n = std::stoi(port);
  return n >= 1 && n <= 65535;
}

// IPv4 addresses other players can reach: adapters with a default gateway (the home network) first,
// virtual ones (WSL, Hyper-V) only when there's nothing else.
std::vector<std::string> LanAddresses() {
  std::vector<std::string> out, routed;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  }
  if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR) return out;
  for (auto* a = adapters; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
      uint8_t first = sin->sin_addr.S_un.S_un_b.s_b1, second = sin->sin_addr.S_un.S_un_b.s_b2;
      if (first == 169 && second == 254) continue;  // link-local: no DHCP, not reachable
      char text[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) (a->FirstGatewayAddress ? routed : out).push_back(text);
    }
  }
  return routed.empty() ? out : routed;
}

std::optional<fs::path> PickDiscImage(HWND owner) {
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return std::nullopt;
  std::optional<fs::path> result;
  COMDLG_FILTERSPEC types[] = {{L"Xbox 360 disc image (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
  dialog->SetFileTypes(2, types);
  dialog->SetTitle(L"Choose your Monopoly (Xbox 360) disc image");
  IShellItem* item = nullptr;
  if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      result = fs::path(path);
      CoTaskMemFree(path);
    }
    item->Release();
  }
  dialog->Release();
  return result;
}

std::optional<std::string> PickFolder(HWND owner) {
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return std::nullopt;
  std::optional<std::string> result;
  FILEOPENDIALOGOPTIONS options;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"Choose the folder with your Monopoly disc files");
  IShellItem* item = nullptr;
  if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      result = Utf8(path);
      CoTaskMemFree(path);
    }
    item->Release();
  }
  dialog->Release();
  return result;
}

// ---- Direct3D 9 ---------------------------------------------------------------------------------

IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_device = nullptr;
D3DPRESENT_PARAMETERS g_pp = {};
bool g_device_lost = false;

bool CreateDevice(HWND hwnd) {
  if (!(g_d3d = Direct3DCreate9(D3D_SDK_VERSION))) return false;
  g_pp.Windowed = TRUE;
  g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
  g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  for (DWORD vp : {D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING})
    if (SUCCEEDED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, vp, &g_pp, &g_device))) return true;
  return false;
}

void ResetDevice() {
  ImGui_ImplDX9_InvalidateDeviceObjects();
  if (g_device->Reset(&g_pp) == D3DERR_INVALIDCALL) IM_ASSERT(0);
  ImGui_ImplDX9_CreateDeviceObjects();
}

// The 256 px icon art, for the sidebar. Managed pool, so it survives device resets.
IDirect3DTexture9* LoadArt() {
  HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(100), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
  if (!res) return nullptr;
  auto* data = static_cast<const stbi_uc*>(LockResource(LoadResource(nullptr, res)));
  int w, h, n;
  stbi_uc* rgba = stbi_load_from_memory(data, int(SizeofResource(nullptr, res)), &w, &h, &n, 4);
  if (!rgba) return nullptr;
  IDirect3DTexture9* tex = nullptr;
  D3DLOCKED_RECT lr;
  if (SUCCEEDED(g_device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) &&
      SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const stbi_uc* s = rgba + (y * w + x) * 4;
        auto* d = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4;
        d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3];
      }
    tex->UnlockRect(0);
  }
  stbi_image_free(rgba);
  return tex;
}

// ---- launcher state -----------------------------------------------------------------------------

enum Page { kPlay, kHost, kJoin, kSettings };

struct Target {
  std::string address;
  uint16_t port = kDefaultPort;
};

// A join code, "host", "host:port" or a bare IPv6 literal.
std::optional<Target> ParseTarget(std::string text) {
  text.erase(0, text.find_first_not_of(" \t"));
  text.erase(text.find_last_not_of(" \t") + 1);
  if (auto code = netplay::ParseJoinCode(text); code && text.find('.') == std::string::npos)
    return Target{code->first, code->second};
  if (!ValidJoinAddress(text)) return std::nullopt;
  auto colon = text.rfind(':');
  if (colon == std::string::npos || text.find(':') != colon) return Target{text};
  return Target{text.substr(0, colon), uint16_t(std::stoi(text.substr(colon + 1)))};
}

// Runs work on a detached thread, so a slow connect or router query never blocks the window or
// its closing. The result is collected with Take(), or thrown away with Drop().
template <typename T>
class Background {
 public:
  template <typename F>
  void Run(F work) {
    auto slot = std::make_shared<Slot>();
    slot_ = slot;
    std::thread([slot, work = std::move(work)]() mutable {
      T value = work();
      std::lock_guard lock(slot->mutex);
      slot->value = std::move(value);
    }).detach();
  }
  bool Busy() const { return slot_ != nullptr; }
  std::optional<T> Take() {
    auto slot = slot_;
    if (!slot) return std::nullopt;
    std::optional<T> value;
    {
      std::lock_guard lock(slot->mutex);
      value.swap(slot->value);
    }
    if (value) slot_.reset();
    return value;
  }
  void Drop() { slot_.reset(); }

 private:
  struct Slot {
    std::mutex mutex;
    std::optional<T> value;
  };
  std::shared_ptr<Slot> slot_;
};

// Router (UPnP) calls take turns, so closing the port always follows the open it undoes; the
// launcher waits for outstanding ones before it exits.
std::mutex g_router_mutex;
std::atomic<int> g_router_jobs{0};

struct Launcher {
  HWND hwnd = nullptr;
  float S = 1.0f;  // DPI scale
  ImFont* regular = nullptr;
  ImFont* bold = nullptr;
  IDirect3DTexture9* art = nullptr;

  Settings settings;
  std::vector<std::string> missing;  // game files not found in GameDir()
  double missing_checked = -10;
  std::vector<std::string> lan;  // this PC's addresses, best first

  std::string notice;  // shown above the page until the next launch
  bool notice_is_error = false;

  HANDLE child = nullptr;
  bool child_is_game = false;
  bool child_hosted = false;  // the game was started from our lobby: release the router port after

  // Online. A lobby is either hosted here or joined; responder must go before lobby_host (it reads it).
  std::unique_ptr<netplay::LobbyHost> lobby_host;
  std::unique_ptr<netplay::DiscoveryResponder> responder;
  std::unique_ptr<netplay::LobbyClient> lobby_client;
  Target lobby_target;  // the host to stream from once the game starts
  struct JoinResult {
    std::unique_ptr<netplay::LobbyClient> client;
    std::string error;
    Target target;
  };
  Background<JoinResult> joining;
  Background<std::vector<netplay::LobbyInfo>> scanning;
  std::vector<netplay::LobbyInfo> found;
  double scanned_at = -10;
  Background<RouterPort> router_opening;
  std::optional<RouterPort> router;
  uint16_t router_port = 0;  // nonzero while we hold (or are asking for) a router mapping
  std::string router_ip;     // the LAN address it points at
  std::unique_ptr<PublicAnnouncer> announcer;  // our lobby in Public games; kept while the game runs
  Background<std::pair<std::vector<netplay::LobbyInfo>, std::string>> fetching;
  std::vector<netplay::LobbyInfo> public_lobbies;
  std::string public_error;
  double fetched_at = -100;
  int join_tab = 0;  // 0: public games, 1: on your network
  std::string chat_input;
  size_t chat_seen = 0;
  bool ready = false;
  // Set when a page is picked with keyboard or controller: the page's main button takes focus,
  // since ImGui navigation only moves sideways between items on the same row.
  bool focus_page = false;

  // Installing from a disc image, into the "game" folder next to the launcher.
  std::shared_ptr<InstallProgress> install;

  void StartInstall() {
    auto image = PickDiscImage(hwnd);
    if (!image) return;
    install = std::make_shared<InstallProgress>();
    std::thread([p = install, image = *image] { InstallFromDiscImage(image, ExeDir() / "game", *p); }).detach();
  }

  void InstallStatus() {
    if (install->finished) {
      std::string error = [&] {
        std::lock_guard lock(install->mutex);
        return install->error;
      }();
      if (error.empty()) settings.game_folder.clear(), Save(), Notice("Game installed. Have fun!", false);
      else Notice(error, true);
      install.reset();
      missing_checked = -10;
      return;
    }
    double total = double(install->total), done = double(install->done);
    std::string file = install->File();
    char label[64];
    snprintf(label, sizeof(label), "%.0f%%", total > 0 ? 100 * done / total : 0.0);
    ImGui::TextUnformatted(file.empty() ? "Reading the disc image..." : ("Installing " + file + "...").c_str());
    ImGui::ProgressBar(total > 0 ? float(done / total) : 0.0f, ImVec2(-110 * S, 0), label);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) install->cancel = true;
  }

  bool InLobby() const { return lobby_host || lobby_client; }
  std::string LobbyName() const { return settings.lobby_name.empty() ? settings.name + "'s game" : settings.lobby_name; }

  void Save() {
    if (!SaveSettings(settings)) Notice("Couldn't save settings next to the launcher (is the folder read-only?).", true);
  }
  void Notice(std::string text, bool error) {
    notice = std::move(text);
    notice_is_error = error;
  }

  bool Launch(const wchar_t* exe_name, const std::wstring& args, bool is_game) {
    fs::path exe = ExeDir() / exe_name;
    std::wstring cmd = QuoteArg(exe.wstring()) + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
      Notice("Couldn't start " + Utf8(exe_name) + " (error " + std::to_string(GetLastError()) +
                 "). Keep the launcher in the same folder as the game.",
             true);
      return false;
    }
    CloseHandle(pi.hThread);
    child = pi.hProcess;
    child_is_game = is_game;
    child_hosted = false;
    notice.clear();
    ShowWindow(hwnd, SW_HIDE);
    return true;
  }

  std::wstring GameArgs(bool host, const std::string& lobby_name = {}) const {
    std::wstring args = host ? L"--net_host" : L"";
    if (!lobby_name.empty()) args += L" " + QuoteArg(L"--net_lobby=" + Wide(lobby_name));
    if (!settings.game_folder.empty()) {
      std::wstring dir = Wide(settings.game_folder);
      while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
      args += (args.empty() ? L"" : L" ") + QuoteArg(L"--game_data_root=" + dir);
    }
    return args;
  }

  void ChildExited() {
    DWORD code = 0;
    GetExitCodeProcess(child, &code);
    CloseHandle(child);
    child = nullptr;
    if (child_hosted) announcer.reset(), ReleaseRouterPort();
    // monopoly_net reports its own errors (exit code 1) in a message box.
    if (code != 0 && (child_is_game || code != 1)) {
      char hex[16];
      snprintf(hex, sizeof(hex), "0x%08lX", code);
      Notice(std::string(child_is_game ? "The game" : "The online client") + " closed unexpectedly (code " + hex +
                 "). Details are in the logs folder.",
             true);
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
  }

  void OpenLobby();
  void LeaveLobby();
  void StartHostedGame();
  void ReleaseRouterPort();
  void Join(Target target);
  void LaunchStream(const Target& t, int seat, int wait_seconds);
  void PollOnline();

  void Frame();
  void Sidebar();
  void PagePlay();
  void PageHost();
  void PageJoin();
  void PageLobby();
  void PageSettings();
  void GameFilesWarning();
  void NameField(const char* id);
  bool ActionButton(const char* label, bool enabled);
  void Heading(const char* text, const char* subtitle);
  void Label(const char* text);
  void Muted(const char* text);
};

const ImVec4 kCream = ImVec4(0.961f, 0.945f, 0.894f, 1);
const ImVec4 kInk = ImVec4(0.12f, 0.16f, 0.14f, 1);
const ImVec4 kMuted = ImVec4(0.36f, 0.42f, 0.38f, 1);
const ImVec4 kRed = ImVec4(0.827f, 0.184f, 0.184f, 1);
const ImVec4 kRedHover = ImVec4(0.898f, 0.224f, 0.208f, 1);
const ImVec4 kRedActive = ImVec4(0.65f, 0.10f, 0.10f, 1);
const ImVec4 kGreen = ImVec4(0.18f, 0.49f, 0.20f, 1);
const ImVec4 kSeatColors[netplay::kMaxPlayers] = {ImVec4(0.83f, 0.18f, 0.18f, 1), ImVec4(0.10f, 0.46f, 0.82f, 1),
                                                  ImVec4(0.18f, 0.49f, 0.20f, 1), ImVec4(0.85f, 0.58f, 0.05f, 1)};

void ApplyStyle(float S) {
  ImGuiStyle& st = ImGui::GetStyle();
  ImGui::StyleColorsLight(&st);
  st.WindowPadding = ImVec2(0, 0);
  st.FramePadding = ImVec2(10, 7);
  st.ItemSpacing = ImVec2(10, 10);
  st.FrameRounding = 7;
  st.GrabRounding = 7;
  st.PopupRounding = 7;
  st.FrameBorderSize = 1;
  st.WindowBorderSize = 0;
  st.ChildBorderSize = 0;
  st.ScrollbarSize = 12;
  st.ScaleAllSizes(S);
  st.FontScaleDpi = S;
  st.FontSizeBase = 17;

  ImVec4* c = st.Colors;
  c[ImGuiCol_WindowBg] = kCream;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_Text] = kInk;
  c[ImGuiCol_TextDisabled] = kMuted;
  c[ImGuiCol_Border] = ImVec4(0.80f, 0.77f, 0.68f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_FrameBgHovered] = ImVec4(1, 0.99f, 0.96f, 1);
  c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_CheckMark] = kGreen;
  c[ImGuiCol_Button] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.89f, 0.95f, 0.89f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.90f, 0.80f, 1);
  c[ImGuiCol_Header] = ImVec4(0.89f, 0.95f, 0.89f, 1);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.89f, 0.95f, 0.89f, 1);
  c[ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.90f, 0.80f, 1);
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.18f, 0.49f, 0.20f, 0.25f);
  c[ImGuiCol_NavCursor] = kGreen;
  c[ImGuiCol_Tab] = ImVec4(0.91f, 0.88f, 0.80f, 1);
  c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_TabSelected] = ImVec4(1, 1, 1, 0.75f);
  c[ImGuiCol_TabSelectedOverline] = kRed;
  c[ImGuiCol_PlotHistogram] = kGreen;  // progress bars
}

void Launcher::Heading(const char* text, const char* subtitle) {
  ImGui::PushFont(bold, 27);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  if (subtitle) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", subtitle);
    ImGui::PopStyleColor();
  }
  ImGui::Dummy(ImVec2(0, 8 * S));
}

void Launcher::Label(const char* text) {
  ImGui::PushFont(bold, 15);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}

void Launcher::Muted(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// The page's main action, pinned to the bottom-right corner of the content area.
bool Launcher::ActionButton(const char* label, bool enabled) {
  ImVec2 size(200 * S, 50 * S);
  ImVec2 at = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
  ImGui::SetCursorScreenPos(ImVec2(at.x + avail.x - size.x, at.y + std::max(0.0f, avail.y - size.y)));
  ImGui::PushStyleColor(ImGuiCol_Button, kRed);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRedActive);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
  ImGui::PushStyleColor(ImGuiCol_Border, kRedActive);
  ImGui::PushFont(bold, 19);
  ImGui::BeginDisabled(!enabled);
  if (focus_page) ImGui::SetKeyboardFocusHere(), focus_page = false;
  bool clicked = ImGui::Button(label, size);
  ImGui::SetItemDefaultFocus();  // where the first keyboard/controller press lands
  ImGui::EndDisabled();
  ImGui::PopFont();
  ImGui::PopStyleColor(5);
  return clicked;
}

void Launcher::GameFilesWarning() {
  if (missing.empty()) return;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.99f, 0.92f, 0.91f, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * S, 12 * S));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * S);
  ImGui::BeginChild("missing", ImVec2(0, 0),
                    ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
  ImGui::PushStyleColor(ImGuiCol_Text, kRedActive);
  ImGui::PushFont(bold, 16);
  ImGui::TextUnformatted("Game files not found");
  ImGui::PopFont();
  ImGui::PopStyleColor();
  if (install) {
    InstallStatus();
  } else {
    ImGui::TextWrapped("Install the game from your own Monopoly (Xbox 360) disc image (.iso), or choose a folder "
                       "that already has its files (default.xex and the .big files).");
    if (ImGui::Button("Install from disc image...")) StartInstall();
    ImGui::SameLine();
    if (ImGui::Button("Choose folder...")) {
      if (auto dir = PickFolder(hwnd)) settings.game_folder = *dir, missing_checked = -10, Save();
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 4 * S));
}

void Launcher::PagePlay() {
  Heading("Play on this PC", "Up to four players on one screen. Add computer players in the game's setup.");
  GameFilesWarning();
  ImGui::TextWrapped(
      "Connect a controller for each player, then choose \"Play With: Multiple\" in the game. "
      "With one controller, players pass it around.");
  ImGui::Dummy(ImVec2(0, 4 * S));
  Muted(settings.fullscreen ? "Starts in fullscreen. Change this in Settings." : "Starts in a window. Change this in Settings.");
  if (ActionButton("Play", missing.empty())) Launch(L"monopoly.exe", GameArgs(false), true);
}

// A player name field: invalid characters are dropped as you type, spaces trimmed when done.
void Launcher::NameField(const char* id) {
  ImGui::SetNextItemWidth(280 * S);
  if (ImGui::InputText(id, &settings.name)) settings.name = SanitizeName(settings.name, false);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    settings.name = SanitizeName(settings.name);
    if (settings.name.empty()) settings.name = DefaultName();
    Save();
  }
}

void Launcher::PageHost() {
  if (InLobby()) return PageLobby();
  Heading("Host an online game", "Open a lobby, invite up to three friends, and start the game when everyone is ready.");
  GameFilesWarning();

  Label("Game name");
  ImGui::SetNextItemWidth(360 * S);
  std::string hint = settings.name + "'s game";
  if (ImGui::InputTextWithHint("##lobby", hint.c_str(), &settings.lobby_name))
    settings.lobby_name = SanitizeName(settings.lobby_name, false);
  if (ImGui::IsItemDeactivatedAfterEdit()) settings.lobby_name = SanitizeName(settings.lobby_name), Save();

  Label("Your name");
  ImGui::SameLine(304 * S);
  Label("Port");
  NameField("##name");
  ImGui::SameLine(304 * S);
  ImGui::SetNextItemWidth(110 * S);
  if (ImGui::InputInt("##port", &settings.port, 0)) settings.port = std::clamp(settings.port, 1, 65535);
  if (ImGui::IsItemDeactivatedAfterEdit()) Save();

  ImGui::Dummy(ImVec2(0, 2 * S));
  if (ImGui::Checkbox("List it in Public games, so anyone can find and join it", &settings.public_lobby)) Save();
  if (ImGui::Checkbox("Open the port on my router automatically (UPnP)", &settings.upnp)) Save();

  ImGui::Dummy(ImVec2(0, 4 * S));
  std::string port = std::to_string(settings.port);
  Muted(("Friends on your network see your game in their Join list. Over the internet, friends join with a code, "
         "or anyone from Public games (which shows your internet address to players looking for games). "
         "That needs TCP port " + port +
         " open on your router: the launcher asks the router for you, or forward it to this PC yourself.\n"
         "In the game, choose \"Play With: Multiple\" so every player gets their own controller.")
            .c_str());
  if (ActionButton("Open lobby", missing.empty())) OpenLobby();
}

void Launcher::PageJoin() {
  if (InLobby()) return PageLobby();
  Heading("Join a game", "Pick a public game or one on your network, or enter the code or address a friend sent you.");

  if (ImGui::BeginTabBar("lists")) {
    if (ImGui::BeginTabItem("Public games")) join_tab = 0, ImGui::EndTabItem();
    if (ImGui::BeginTabItem("On your network")) join_tab = 1, ImGui::EndTabItem();
    ImGui::EndTabBar();
  }
  struct Row {
    std::string name, detail;
    Target target;
    bool full;
  };
  auto detail = [](const std::string& host, int players, bool in_game) {
    return "Hosted by " + host + "  |  " + std::to_string(players) + " of 4 players  |  " +
           (in_game ? "Playing" : "In the lobby");
  };
  std::vector<Row> rows;
  std::string empty;
  if (join_tab == 0) {
    for (auto& l : public_lobbies)  // every one of these answered just now, so it's reachable
      rows.push_back({l.lobby_name, detail(l.host_name, l.players, l.in_game), {l.address, l.port},
                      l.players >= netplay::kMaxPlayers});
    empty = !public_error.empty() ? "Couldn't search for public games: " + public_error + "."
            : fetched_at < 0      ? "Searching for public games (this takes a few seconds)..."
                                  : "No public games right now. Host one and list it for everyone!";
  } else {
    for (auto& g : found)
      rows.push_back({g.lobby_name, detail(g.host_name, g.players, g.in_game), {g.address, g.port},
                      g.players >= netplay::kMaxPlayers});
    empty = scanned_at < 0 ? "Looking for games..." : "No games found on your network yet. Still looking...";
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.7f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * S, 8 * S));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * S);
  ImGui::BeginChild("found", ImVec2(0, 170 * S), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
  if (rows.empty()) Muted(empty.c_str());
  for (size_t i = 0; i < rows.size(); ++i) {
    ImGui::PushID(int(i));
    ImGui::BeginGroup();
    ImGui::PushFont(bold, 17);
    ImGui::TextUnformatted(rows[i].name.c_str());
    ImGui::PopFont();
    Muted(rows[i].detail.c_str());
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 90 * S);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * S);
    ImGui::BeginDisabled(joining.Busy() || rows[i].full);
    if (focus_page) ImGui::SetKeyboardFocusHere(), focus_page = false;
    if (ImGui::Button("Join", ImVec2(90 * S, 0))) Join(rows[i].target);
    if (i == 0) ImGui::SetItemDefaultFocus();
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  Label("Join with a code or address");
  auto target = ParseTarget(settings.join_address);
  ImGui::SetNextItemWidth(-100 * S);
  bool enter = ImGui::InputTextWithHint("##address", "e.g. 6B01R-GETQ0 or 192.168.1.20", &settings.join_address,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemDeactivatedAfterEdit()) Save();
  ImGui::SameLine();
  ImGui::BeginDisabled(!target || joining.Busy());
  if ((ImGui::Button("Join##code", ImVec2(-FLT_MIN, 0)) || enter) && target) Join(*target);
  ImGui::EndDisabled();
  if (!settings.join_address.empty() && !target) {
    ImGui::PushStyleColor(ImGuiCol_Text, kRedActive);
    ImGui::TextUnformatted("That isn't a join code or an address.");
    ImGui::PopStyleColor();
  }

  Label("Your name");
  NameField("##name");
  if (joining.Busy()) Muted("Connecting...");
}

void Launcher::PageLobby() {
  const bool hosting = lobby_host != nullptr;
  netplay::LobbyState st = hosting ? lobby_host->State() : lobby_client->State();
  std::vector<netplay::ChatLine> chat = hosting ? lobby_host->Chat() : lobby_client->Chat();
  const int me = hosting ? 0 : lobby_client->seat();
  bool all_ready = true;
  for (int i = 1; i < netplay::kMaxPlayers; ++i) all_ready &= !st.present[i] || st.ready[i];

  std::string subtitle = hosting ? "Your lobby is open. Start the game when everyone is ready."
                                 : "Hosted by " + st.names[0] + ". The host starts the game when everyone is ready.";
  Heading(st.lobby_name.empty() ? "Lobby" : st.lobby_name.c_str(), subtitle.c_str());

  // Players
  const float column = 290 * S;
  ImGui::BeginGroup();
  Label("Players");
  for (int i = 0; i < netplay::kMaxPlayers; ++i) {
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImVec4 color = st.present[i] ? kSeatColors[i] : ImVec4(0.75f, 0.73f, 0.66f, 1);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 38 * S, p.y + h), ImGui::GetColorU32(color), 6 * S);
    char badge[3] = {'P', char('1' + i), 0};
    ImGui::PushFont(bold, 15);
    ImVec2 ts = ImGui::CalcTextSize(badge);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (38 * S - ts.x) / 2, p.y + (h - ts.y) / 2), IM_COL32_WHITE, badge);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(38 * S, h));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (st.present[i]) {
      ImGui::PushFont(bold, 17);
      ImGui::TextUnformatted((st.names[i] + (i == me ? " (you)" : "")).c_str());
      ImGui::PopFont();
      ImGui::SameLine();
      if (i == 0) ImGui::TextColored(kMuted, "Host");
      else if (st.ready[i]) ImGui::TextColored(kGreen, "Ready");
      else ImGui::TextColored(kMuted, "Not ready");
      if (hosting && i > 0) {
        ImGui::SameLine(column - 70 * S);
        if (ImGui::SmallButton("Remove")) lobby_host->Kick(i);
      }
    } else {
      ImGui::TextColored(kMuted, "Open seat");
    }
    ImGui::PopID();
  }
  ImGui::EndGroup();

  // Invite (host) or controls (players)
  ImGui::SameLine(column + 24 * S);
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
  if (hosting) {
    Label("Invite friends");
    std::string port = std::to_string(settings.port);
    PublicAnnouncer::Status as = announcer ? announcer->GetStatus() : PublicAnnouncer::Status{};
    // A join code needs our internet address: from the router, or as the peer-to-peer network saw us.
    std::string public_ip;
    if (router && !router->external_ip.empty() && !IsPrivateIPv4(router->external_ip)) public_ip = router->external_ip;
    else if (!as.external_ip.empty() && !IsPrivateIPv4(as.external_ip)) public_ip = as.external_ip;
    if (!public_ip.empty()) {
      std::string code = netplay::MakeJoinCode(public_ip, uint16_t(settings.port));
      ImGui::PushFont(bold, 22);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(code.c_str());
      ImGui::PopFont();
      ImGui::SameLine();
      if (ImGui::SmallButton("Copy##code")) ImGui::SetClipboardText(code.c_str());
    }
    std::string forward = "forward TCP port " + port + " to " + (lan.empty() ? std::string("this PC") : lan.front()) +
                          " on your router";
    bool router_open = router && router->open;
    // The router reported a private internet address: another router or the provider's NAT is in front.
    bool double_nat = router && !router->external_ip.empty() && IsPrivateIPv4(router->external_ip);
    if (router_opening.Busy()) {
      Muted(("Asking your router to open port " + port + "...").c_str());
    } else if (router_open && !double_nat) {
      Muted(("Port " + port + " is open on your router" +
             (public_ip.empty() ? ". Send friends your public IP address to join with." : ": friends anywhere can join with this code."))
                .c_str());
    } else if (router_open) {
      Muted(("Your router opened port " + port + ", but it sits behind another router or your provider's network, "
             "so friends over the internet probably can't reach you.").c_str());
    } else {
      std::string why = router && !router->error.empty() ? router->error + " " : "";
      Muted((why + "For friends over the internet, " + forward +
             (public_ip.empty() ? " and send them your public IP address." : "."))
                .c_str());
    }
    if (announcer) {
      Muted(!as.error.empty() && !as.announced ? ("Couldn't list the game publicly: " + as.error + ".").c_str()
            : !as.announced                    ? "Listing the game in Public games..."
                                               : "Listed in Public games: players who can reach your port can join.");
    }
    if (!lan.empty()) {
      ImGui::Dummy(ImVec2(0, 2 * S));
      Muted(("On your network, friends see \"" + st.lobby_name + "\" in their Join list (or use " + lan.front() +
             ").")
                .c_str());
    }
  } else {
    Label("Controls");
    Muted("A controller works as on the console. On the keyboard: arrow keys move, Enter or Z = A, "
          "Backspace or X = B, C = X, V = Y, Q / E = bumpers, Space = Start, Esc = Back.");
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();

  // Chat
  ImGui::Dummy(ImVec2(0, 4 * S));
  Label("Chat");
  const float bottom = ImGui::GetFrameHeightWithSpacing() + 64 * S +
                       (hosting && !all_ready ? ImGui::GetTextLineHeightWithSpacing() : 0);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * S, 8 * S));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * S);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * S, 3 * S));
  ImGui::BeginChild("chat", ImVec2(0, std::max(70 * S, ImGui::GetContentRegionAvail().y - bottom)),
                    ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushTextWrapPos(0);
  for (auto& line : chat) {
    if (line.seat < 0) {
      ImGui::TextColored(kMuted, "%s", line.text.c_str());
      continue;
    }
    ImGui::PushFont(bold, 17);
    ImGui::TextColored(kSeatColors[line.seat % netplay::kMaxPlayers], "%s:", line.name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextUnformatted(line.text.c_str());
  }
  ImGui::PopTextWrapPos();
  if (chat.size() != chat_seen) ImGui::SetScrollHereY(1.0f), chat_seen = chat.size();
  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
  ImGui::SetNextItemWidth(-90 * S);
  bool send = ImGui::InputTextWithHint("##say", "Say something...", &chat_input, ImGuiInputTextFlags_EnterReturnsTrue);
  if (send) ImGui::SetKeyboardFocusHere(-1);  // keep typing after Enter
  ImGui::SameLine();
  send |= ImGui::Button("Send", ImVec2(-FLT_MIN, 0));
  if (send && !chat_input.empty()) {
    if (hosting) lobby_host->Say(chat_input);
    else lobby_client->Say(chat_input);
    chat_input.clear();
  }
  if (hosting && !all_ready) Muted("Waiting for everyone to be ready.");

  // Leave (left) and the main action (right) on the bottom row.
  ImVec2 at = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
  float y = at.y + std::max(0.0f, avail.y - 50 * S);
  ImGui::SetCursorScreenPos(ImVec2(at.x, y + 4 * S));
  if (ImGui::Button("Leave lobby", ImVec2(150 * S, 42 * S))) return LeaveLobby();
  ImGui::SetCursorScreenPos(ImVec2(at.x, y));
  if (hosting) {
    if (ActionButton("Start game", all_ready && missing.empty())) StartHostedGame();
  } else if (ActionButton(ready ? "Not ready" : "Ready", true)) {
    ready = !ready;
    lobby_client->SetReady(ready);
  }
}

void Launcher::PageSettings() {
  Heading("Settings", "Saved as you change them. They take effect the next time the game starts.");
  Label("Display");
  if (ImGui::Checkbox("Fullscreen", &settings.fullscreen)) Save();
  if (ImGui::Checkbox("Vertical sync", &settings.vsync)) Save();
  static const char* kScales[] = {"Native (1280 x 720)", "2x (2560 x 1440)", "3x (3840 x 2160)"};
  ImGui::SetNextItemWidth(260 * S);
  int scale_index = settings.scale - 1;
  if (ImGui::Combo("Rendering resolution", &scale_index, kScales, IM_ARRAYSIZE(kScales)))
    settings.scale = scale_index + 1, Save();

  ImGui::Dummy(ImVec2(0, 10 * S));
  Label("Game files");
  ImGui::TextWrapped("%s", Utf8(GameDir(settings).wstring()).c_str());
  if (missing.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
    ImGui::TextUnformatted("All game files found.");
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kRedActive);
    ImGui::TextUnformatted("Some game files are missing.");
    ImGui::PopStyleColor();
  }
  if (install) {
    InstallStatus();
  } else {
    if (ImGui::Button("Install from disc image...")) StartInstall();
    ImGui::SameLine();
    if (ImGui::Button("Choose folder...")) {
      if (auto dir = PickFolder(hwnd)) settings.game_folder = *dir, missing_checked = -10, Save();
    }
    if (!settings.game_folder.empty()) {
      ImGui::SameLine();
      if (ImGui::Button("Use the game folder next to the launcher")) settings.game_folder.clear(), missing_checked = -10, Save();
    }
  }

  ImGui::Dummy(ImVec2(0, 10 * S));
  Label("Troubleshooting");
  fs::path logs = ExeDir() / "logs";
  std::error_code ec;
  ImGui::BeginDisabled(!fs::is_directory(logs, ec));
  if (ImGui::Button("Open logs folder")) ShellExecuteW(hwnd, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  ImGui::EndDisabled();
}

// ---- online -------------------------------------------------------------------------------------

void Launcher::OpenLobby() {
  std::string error;
  uint16_t port = uint16_t(settings.port);
  lobby_host = netplay::LobbyHost::Start(port, LobbyName(), settings.name, &error);
  if (!lobby_host) return Notice("Couldn't open the lobby: " + error + ".", true);
  responder = netplay::DiscoveryResponder::Start([host = lobby_host.get(), port] {
    netplay::LobbyInfo info;
    auto st = host->State();
    for (bool p : st.present) info.players += p;
    info.port = port;
    info.lobby_name = st.lobby_name;
    info.host_name = st.names[0];
    return info;
  });
  notice.clear();
  chat_seen = 0;
  focus_page = ImGui::GetIO().NavVisible;  // controller: straight to "Start game"
  if (settings.public_lobby) announcer = std::make_unique<PublicAnnouncer>(port);
  lan = LanAddresses();
  if (lan.empty()) return;
  router.reset();
  router_ip = lan.front();
  if (settings.upnp) router_port = port;  // only then is there a mapping to remove later
  ++g_router_jobs;
  router_opening.Run([port, ip = router_ip, map = settings.upnp] {
    RouterPort r;
    {
      std::lock_guard lock(g_router_mutex);
      r = OpenRouterPort(port, ip, map);  // without map, just learn our internet address for the code
    }
    --g_router_jobs;
    return r;
  });
}

void Launcher::ReleaseRouterPort() {
  router_opening.Drop();
  router.reset();
  if (!router_port) return;
  ++g_router_jobs;
  std::thread([port = router_port, ip = router_ip] {
    {
      std::lock_guard lock(g_router_mutex);  // after any open still in flight
      CloseRouterPort(port, ip);
    }
    --g_router_jobs;
  }).detach();
  router_port = 0;
}

void Launcher::LeaveLobby() {
  responder.reset();  // reads lobby_host
  lobby_host.reset();
  lobby_client.reset();
  announcer.reset();
  ready = false;
  chat_input.clear();
  ReleaseRouterPort();
}

void Launcher::StartHostedGame() {
  auto st = lobby_host->State();
  lobby_host->StartGame();  // up to 1.5 s while friends' launchers hang up
  responder.reset();
  lobby_host.reset();
  chat_input.clear();
  // The announcement stays up while the game runs: the game itself answers players browsing public
  // games, and late joiners go straight into it.
  if (!Launch(L"monopoly.exe", GameArgs(true, st.lobby_name), true)) return announcer.reset(), ReleaseRouterPort();
  child_hosted = true;
}

void Launcher::Join(Target target) {
  if (joining.Busy()) return;
  notice.clear();
  joining.Run([target, name = settings.name] {
    JoinResult r;
    r.target = target;
    r.client = netplay::LobbyClient::Connect(target.address, target.port, name, &r.error);
    return r;
  });
}

void Launcher::LaunchStream(const Target& t, int seat, int wait_seconds) {
  bool ipv6 = t.address.find(':') != std::string::npos;
  std::string addr = t.port == kDefaultPort ? t.address
                                            : (ipv6 ? "[" + t.address + "]" : t.address) + ":" + std::to_string(t.port);
  std::wstring args = L"--join " + QuoteArg(Wide(addr)) + L" --name " + QuoteArg(Wide(settings.name));
  if (seat) args += L" --slot " + std::to_wstring(seat);
  if (wait_seconds) args += L" --wait " + std::to_wstring(wait_seconds);
  Launch(L"monopoly_net.exe", args, false);
}

void Launcher::PollOnline() {
  if (auto r = joining.Take()) {
    if (r->client) {
      lobby_client = std::move(r->client);
      lobby_target = r->target;
      ready = false;
      chat_seen = 0;
      focus_page = ImGui::GetIO().NavVisible;  // controller: straight to "Ready"
    } else if (r->error == netplay::wire::kInGameReason) {
      LaunchStream(r->target, 0, 0);  // already playing: join the game directly
    } else {
      Notice("Couldn't join: " + r->error + ".", true);
    }
  }
  if (lobby_client) {
    std::string reason;
    auto status = lobby_client->GetStatus(&reason);
    if (status == netplay::LobbyClient::Status::kStarting) {
      int seat = lobby_client->seat();
      lobby_client.reset();  // hang up first: the host hands this port to the game
      ready = false;
      LaunchStream(lobby_target, seat, 90);
    } else if (status == netplay::LobbyClient::Status::kClosed) {
      lobby_client.reset();
      ready = false;
      Notice(reason, true);
    }
  }
  if (auto r = router_opening.Take()) router = std::move(*r);
  if (settings.page == kJoin && !InLobby()) {  // and public games, which take a few seconds to find
    if (auto r = fetching.Take()) {
      public_lobbies = std::move(r->first), public_error = std::move(r->second), fetched_at = ImGui::GetTime();
    } else if (!fetching.Busy() && ImGui::GetTime() - fetched_at > 30) {
      fetching.Run([] {
        std::string error;
        auto lobbies = FindPublicLobbies(&error);
        return std::make_pair(std::move(lobbies), std::move(error));
      });
    }
  }
  if (settings.page == kJoin && !InLobby()) {  // keep the list of games on the network fresh
    if (auto r = scanning.Take()) found = std::move(*r), scanned_at = ImGui::GetTime();
    else if (!scanning.Busy() && ImGui::GetTime() - scanned_at > 3)
      scanning.Run([] { return netplay::FindLobbies(std::chrono::milliseconds(700)); });
  }
}

void Launcher::Sidebar() {
  const float width = 250 * S;
  // Drawn straight into the root window (not a child window) so keyboard and controller navigation
  // can move between the sidebar and the page. The page's padding is the root window's padding,
  // so lift the clip rect to reach the window edges here.
  ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
  ImGui::PushClipRect(p, ImVec2(p.x + sz.x, p.y + sz.y), false);
  ImGui::BeginGroup();
  ImGui::GetWindowDrawList()->AddRectFilledMultiColor(p, ImVec2(p.x + width, p.y + sz.y), IM_COL32(205, 45, 42, 255),
                                                      IM_COL32(205, 45, 42, 255), IM_COL32(150, 18, 22, 255),
                                                      IM_COL32(150, 18, 22, 255));
  const float art = 120 * S;
  ImGui::SetCursorPos(ImVec2((width - art) / 2, 28 * S));
  if (this->art) ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)this->art), ImVec2(art, art));
  else ImGui::Dummy(ImVec2(art, art));

  auto centered = [&](const char* text, ImFont* font, float size, ImVec4 color) {
    ImGui::PushFont(font, size);
    ImGui::SetCursorPosX((width - ImGui::CalcTextSize(text).x) / 2);
    ImGui::TextColored(color, "%s", text);
    ImGui::PopFont();
  };
  ImGui::Dummy(ImVec2(0, 2 * S));
  centered("MONOPOLY", bold, 30, ImVec4(1, 1, 1, 1));
  centered("Xbox 360 edition for PC", regular, 15, ImVec4(1, 0.88f, 0.86f, 1));
  centered("v" MONOPOLY_VERSION, regular, 13, ImVec4(1, 0.78f, 0.76f, 1));
  ImGui::Dummy(ImVec2(0, 18 * S));

  static const char* kPages[] = {"Play", "Host online", "Join online", "Settings"};
  ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20 * S, 0));
  // Out of keyboard/controller navigation, which otherwise wanders between the sidebar and the page
  // (ImGui only moves sideways to items on the same row). Pages switch with bumpers / Ctrl+PgUp/PgDn.
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  for (int i = 0; i < 4; ++i) {
    bool selected = settings.page == i;
    ImGui::SetCursorPosX(16 * S);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? kCream : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? kCream : ImVec4(1, 1, 1, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, selected ? kCream : ImVec4(1, 1, 1, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_Text, selected ? kRedActive : ImVec4(1, 1, 1, 1));
    ImGui::PushFont(bold, 18);
    ImGui::BeginDisabled(!selected && (InLobby() || joining.Busy()));  // leave the lobby first
    if (ImGui::Button(kPages[i], ImVec2(width - 32 * S, 46 * S))) {
      if (!selected) settings.page = i, Save();
      focus_page = ImGui::GetIO().NavVisible;
    }
    ImGui::EndDisabled();
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
  }
  ImGui::PopItemFlag();
  ImGui::PopStyleVar(3);
  const char* hint = "LB / RB or Ctrl+PgUp / PgDn: switch pages";
  ImGui::PushFont(regular, 13);
  ImGui::SetCursorPos(ImVec2((width - ImGui::CalcTextSize(hint).x) / 2, sz.y - 34 * S));
  ImGui::TextColored(ImVec4(1, 0.82f, 0.80f, 1), "%s", hint);
  ImGui::PopFont();
  ImGui::EndGroup();
  ImGui::PopClipRect();
}

void Launcher::Frame() {
  PollOnline();
  // Bumpers or Ctrl+PgUp/PgDn flip through the pages (not while in a lobby: leave it first).
  int flip = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) - ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
  if (ImGui::GetIO().KeyCtrl)
    flip += ImGui::IsKeyPressed(ImGuiKey_PageDown, false) - ImGui::IsKeyPressed(ImGuiKey_PageUp, false);
  if (flip && !InLobby() && !joining.Busy()) settings.page = (settings.page + 4 + flip) % 4, focus_page = true, Save();
  if (ImGui::GetTime() - missing_checked > 2) missing = MissingGameFiles(GameDir(settings)), missing_checked = ImGui::GetTime();

  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * S, 30 * S));  // the page's margins
  ImGui::Begin("##root", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();
  Sidebar();
  ImGui::SetCursorPos(ImVec2(250 * S + 36 * S, 30 * S));
  ImGui::BeginGroup();  // the page: lines start at its left edge
  if (!notice.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, notice_is_error ? kRedActive : kGreen);
    ImGui::TextWrapped("%s", notice.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss")) notice.clear();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4 * S));
  }
  switch (settings.page) {
    case kPlay: PagePlay(); break;
    case kHost: PageHost(); break;
    case kJoin: PageJoin(); break;
    default: PageSettings(); break;
  }
  ImGui::EndGroup();
  focus_page = false;  // pages without a main button
  ImGui::End();
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // no Alt menu beep
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  ImGui_ImplWin32_EnableDpiAwareness();
  Launcher app;
  app.S = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  app.settings = LoadSettings();

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"MonopolyLauncher";
  RegisterClassExW(&wc);
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT rc{0, 0, LONG(960 * app.S), LONG(620 * app.S)};
  AdjustWindowRect(&rc, style, FALSE);
  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  app.hwnd = CreateWindowW(wc.lpszClassName, L"Monopoly", style, work.left + (work.right - work.left - w) / 2,
                           work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, instance, nullptr);
  if (!app.hwnd || !CreateDevice(app.hwnd)) {
    MessageBoxW(nullptr, L"Couldn't start Direct3D 9 for the launcher window.", L"Monopoly", MB_ICONERROR);
    return 1;
  }

  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  ApplyStyle(app.S);
  wchar_t windir[MAX_PATH];
  GetWindowsDirectoryW(windir, MAX_PATH);
  fs::path fonts = fs::path(windir) / "Fonts";
  std::error_code ec;
  auto font = [&](const char* file) -> ImFont* {
    fs::path path = fonts / file;
    return fs::is_regular_file(path, ec) ? io.Fonts->AddFontFromFileTTF(Utf8(path.wstring()).c_str(), 17) : nullptr;
  };
  app.regular = font("segoeui.ttf");
  if (!app.regular) app.regular = io.Fonts->AddFontDefault();
  app.bold = font("segoeuib.ttf");
  if (!app.bold) app.bold = app.regular;

  ImGui_ImplWin32_Init(app.hwnd);
  ImGui_ImplDX9_Init(g_device);
  app.art = LoadArt();
  ShowWindow(app.hwnd, show);
  UpdateWindow(app.hwnd);

  for (bool done = false; !done;) {
    // While the game runs the launcher is hidden: sleep until the child exits or a message arrives.
    if (app.child) {
      DWORD r = MsgWaitForMultipleObjects(1, &app.child, FALSE, INFINITE, QS_ALLINPUT);
      if (r == WAIT_OBJECT_0) app.ChildExited();
    } else if (IsIconic(app.hwnd)) {
      WaitMessage();
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done || app.child || IsIconic(app.hwnd)) continue;

    if (g_device_lost) {
      HRESULT hr = g_device->TestCooperativeLevel();
      if (hr == D3DERR_DEVICELOST) {
        Sleep(10);
        continue;
      }
      if (hr == D3DERR_DEVICENOTRESET) ResetDevice();
      g_device_lost = false;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.Frame();
    ImGui::EndFrame();
    g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_RGBA(245, 241, 228, 255), 1.0f, 0);
    if (g_device->BeginScene() >= 0) {
      ImGui::Render();
      ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
      g_device->EndScene();
    }
    if (g_device->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST) g_device_lost = true;
  }

  app.LeaveLobby();
  if (app.install) {  // closing mid-install: stop, and let it remove its partial file
    app.install->cancel = true;
    for (int i = 0; i < 100 && !app.install->finished; ++i) Sleep(50);
  }
  for (int i = 0; i < 200 && g_router_jobs; ++i) Sleep(50);  // let the router forget our port
  if (app.art) app.art->Release();
  ImGui_ImplDX9_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  if (g_device) g_device->Release();
  if (g_d3d) g_d3d->Release();
  DestroyWindow(app.hwnd);
  CoUninitialize();
  return 0;
}
