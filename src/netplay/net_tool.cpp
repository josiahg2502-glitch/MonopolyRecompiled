// monopoly_net: join a hosted game as a remote player, or run a test-pattern host.
//   monopoly_net --join <address>[:port] [--name <name>] [--slot <1-3>] [--wait <seconds>]
//   monopoly_net --test-host [--port <port>] [--name <name>]
#include <windows.h>
#include <shellapi.h>
#include <xaudio2.h>
#include <xinput.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "netplay.h"

namespace {

constexpr wchar_t kClassName[] = L"MonopolyNet";

std::mutex g_frame_mutex;
netplay::Frame g_frame;
HWND g_window = nullptr;

// ---- audio --------------------------------------------------------------------------------------

class AudioOut final : public IXAudio2VoiceCallback {
 public:
  bool Start() {
    if (FAILED(XAudio2Create(&xaudio_, 0)) || FAILED(xaudio_->CreateMasteringVoice(&master_))) return false;
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    return SUCCEEDED(xaudio_->CreateSourceVoice(&voice_, &fmt, 0, 2.0f, this)) && SUCCEEDED(voice_->Start());
  }
  void Push(const int16_t* samples, uint32_t frames) {
    if (!voice_ || queued_ > 8) return;  // keep latency bounded: drop instead of piling up
    auto* copy = new int16_t[frames * 2];
    memcpy(copy, samples, frames * 4);
    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = frames * 4;
    buf.pAudioData = reinterpret_cast<const BYTE*>(copy);
    buf.pContext = copy;
    ++queued_;
    if (FAILED(voice_->SubmitSourceBuffer(&buf))) {
      --queued_;
      delete[] copy;
    }
  }
  ~AudioOut() {
    if (voice_) voice_->DestroyVoice();  // flushes buffers through OnBufferEnd
    if (xaudio_) xaudio_->Release();
  }

  void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override {
    delete[] static_cast<int16_t*>(context);
    --queued_;
  }
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
  void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}

 private:
  IXAudio2* xaudio_ = nullptr;
  IXAudio2MasteringVoice* master_ = nullptr;
  IXAudio2SourceVoice* voice_ = nullptr;
  std::atomic<int> queued_{0};
};

// ---- input --------------------------------------------------------------------------------------

netplay::Gamepad ReadLocalPad() {
  netplay::Gamepad pad;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {  // first connected controller
    XINPUT_STATE st{};
    if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
    pad.buttons = st.Gamepad.wButtons;
    pad.left_trigger = st.Gamepad.bLeftTrigger;
    pad.right_trigger = st.Gamepad.bRightTrigger;
    pad.thumb_lx = st.Gamepad.sThumbLX, pad.thumb_ly = st.Gamepad.sThumbLY;
    pad.thumb_rx = st.Gamepad.sThumbRX, pad.thumb_ry = st.Gamepad.sThumbRY;
    break;
  }
  if (GetForegroundWindow() != g_window) return pad;
  static constexpr struct { int vk; uint16_t button; } kKeys[] = {
      {VK_UP, XINPUT_GAMEPAD_DPAD_UP},     {VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
      {VK_RETURN, XINPUT_GAMEPAD_A},       {'Z', XINPUT_GAMEPAD_A},
      {VK_BACK, XINPUT_GAMEPAD_B},         {'X', XINPUT_GAMEPAD_B},
      {'C', XINPUT_GAMEPAD_X},             {'V', XINPUT_GAMEPAD_Y},
      {VK_SPACE, XINPUT_GAMEPAD_START},    {VK_ESCAPE, XINPUT_GAMEPAD_BACK},
      {'Q', XINPUT_GAMEPAD_LEFT_SHOULDER}, {'E', XINPUT_GAMEPAD_RIGHT_SHOULDER},
  };
  for (auto& k : kKeys)
    if (GetAsyncKeyState(k.vk) & 0x8000) pad.buttons |= k.button;
  return pad;
}

// ---- window -------------------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      std::lock_guard lock(g_frame_mutex);
      if (g_frame.width) {
        // Letterbox to the window, keeping the stream's aspect ratio.
        int cw = rc.right, ch = rc.bottom;
        int w = cw, h = int(int64_t(cw) * g_frame.height / g_frame.width);
        if (h > ch) h = ch, w = int(int64_t(ch) * g_frame.width / g_frame.height);
        BITMAPINFO bmi{};
        bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(g_frame.width), -LONG(g_frame.height), 1, 32, BI_RGB};
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, (cw - w) / 2, (ch - h) / 2, w, h, 0, 0, g_frame.width, g_frame.height,
                      g_frame.bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateMainWindow(const wchar_t* title) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  wc.lpszClassName = kClassName;
  RegisterClassW(&wc);
  RECT rc{0, 0, 1280, 720};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  return CreateWindowW(kClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                       rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
}

// Pumps messages; calls `tick` roughly every `ms` milliseconds. Returns when the window closes.
template <typename F>
void RunLoop(DWORD ms, F&& tick) {
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!tick()) return;
    MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
  }
}

std::wstring Widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n ? n - 1 : 0, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

void Fail(const std::string& message) {
  MessageBoxW(g_window, Widen(message).c_str(), L"Monopoly online", MB_ICONERROR);
}

// ---- modes --------------------------------------------------------------------------------------

// wait_seconds > 0: the host's game is still starting (we came from a lobby), keep retrying.
int Join(std::string address, std::string name, int wanted_slot, int wait_seconds) {
  uint16_t port = netplay::kDefaultPort;
  // host:port or [ipv6]:port; a bare IPv6 literal uses the default port
  if (address.starts_with('[')) {
    size_t close = address.find(']');
    if (close != std::string::npos && address.size() > close + 2 && address[close + 1] == ':')
      port = uint16_t(std::stoi(address.substr(close + 2)));
    address = address.substr(1, close == std::string::npos ? std::string::npos : close - 1);
  } else if (auto colon = address.rfind(':'); colon != std::string::npos && address.find(':') == colon) {
    port = uint16_t(std::stoi(address.substr(colon + 1)));
    address.resize(colon);
  }
  g_window = CreateMainWindow(wait_seconds ? L"Monopoly online - waiting for the host's game to start..."
                                            : L"Monopoly online - connecting...");
  AudioOut audio;
  audio.Start();  // silent play is fine if there's no audio device

  std::atomic<bool> lost{false};
  std::string lost_reason;
  netplay::Client::Callbacks cb;
  cb.on_video = [](const netplay::Frame& f) {
    {
      std::lock_guard lock(g_frame_mutex);
      g_frame = f;
    }
    InvalidateRect(g_window, nullptr, FALSE);
  };
  cb.on_audio = [&](const int16_t* s, uint32_t n) { audio.Push(s, n); };
  cb.on_roster = [](const netplay::Roster& r) {
    std::string title = "Monopoly online -";
    for (int i = 0; i < netplay::kMaxPlayers; ++i)
      if (r.connected[i]) title += " P" + std::to_string(i + 1) + ":" + r.names[i];
    SetWindowTextW(g_window, Widen(title).c_str());
  };
  cb.on_disconnect = [&](const std::string& reason) {
    lost_reason = reason;
    lost = true;
  };

  std::string error;
  std::unique_ptr<netplay::Client> client;
  auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
  bool closed = false;
  RunLoop(500, [&] {
    client = netplay::Client::Connect(address, port, name, wanted_slot, cb, &error);
    return !client && std::chrono::steady_clock::now() < give_up && error.starts_with("could not connect");
  });
  if (!client) {
    MSG msg;  // the window may already be gone if the player closed it while waiting
    closed = !IsWindow(g_window) || PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE);
    return closed ? 0 : (Fail(error), 1);
  }
  RunLoop(8, [&] {
    if (lost) return Fail("Disconnected: " + lost_reason), false;
    client->SendInput(ReadLocalPad());
    return true;
  });
  return 0;
}

int TestHost(uint16_t port, std::string name) {
  g_window = CreateMainWindow(L"Monopoly online - test host");
  std::string error;
  auto host = netplay::Host::Start(port, name, &error);
  if (!host) return Fail(error), 1;

  // Each player's column lights up while they hold buttons, so a remote tester sees their input arrive.
  const uint32_t w = 1280, h = 720;
  netplay::Frame frame{w, h, std::vector<uint8_t>(w * h * 4)};
  std::vector<int16_t> tone(800 * 2);
  uint32_t t = 0;
  double phase = 0;
  RunLoop(16, [&] {
    ++t;
    netplay::Gamepad pads[netplay::kMaxPlayers];
    pads[0] = ReadLocalPad();
    bool any_a = pads[0].buttons & XINPUT_GAMEPAD_A;
    for (int s = 1; s < netplay::kMaxPlayers; ++s) {
      host->GetInput(s, &pads[s]);
      any_a |= (pads[s].buttons & XINPUT_GAMEPAD_A) != 0;
    }
    auto roster = host->GetRoster();
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t* p = &frame.bgra[(y * w + x) * 4];
        int s = x / (w / netplay::kMaxPlayers);
        bool lit = pads[s].buttons != 0;
        uint8_t base = roster.connected[s] ? 90 : 20;
        p[0] = uint8_t(lit ? 40 : base + ((x + t * 4) & 63));
        p[1] = uint8_t(lit ? 220 : base);
        p[2] = uint8_t(lit ? 40 : base + ((y + t * 2) & 63));
        p[3] = 255;
      }
    {
      std::lock_guard lock(g_frame_mutex);
      g_frame = frame;
    }
    InvalidateRect(g_window, nullptr, FALSE);
    host->SubmitVideo(frame.bgra.data(), w, h, w * 4);
    for (size_t i = 0; i < tone.size(); i += 2) {  // 440 Hz while anyone holds A
      tone[i] = tone[i + 1] = any_a ? int16_t(6000 * std::sin(phase)) : 0;
      phase += 2 * 3.14159265358979 * 440 / 48000;
    }
    host->SubmitAudio(tone.data(), 800);
    return true;
  });
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::string mode, address, name = "Player";
  uint16_t port = netplay::kDefaultPort;
  int slot = 0, wait = 0;
  for (int i = 1; i < argc; ++i) {
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, sizeof(buf), nullptr, nullptr);
    std::string arg = buf;
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) return {};
      WideCharToMultiByte(CP_UTF8, 0, wargv[++i], -1, buf, sizeof(buf), nullptr, nullptr);
      return buf;
    };
    if (arg == "--join") mode = "join", address = next();
    else if (arg == "--test-host") mode = "host";
    else if (arg == "--port") port = uint16_t(std::stoi(next()));
    else if (arg == "--name") name = next();
    else if (arg == "--slot") slot = std::atoi(next().c_str());
    else if (arg == "--wait") wait = std::atoi(next().c_str());
  }
  LocalFree(wargv);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (mode == "join" && !address.empty()) return Join(address, name, slot, wait);
  if (mode == "host") return TestHost(port, name);
  MessageBoxW(nullptr,
              L"monopoly_net --join <address>[:port] [--name <name>] [--slot <1-3>] [--wait <seconds>]\n"
              L"monopoly_net --test-host [--port <port>] [--name <name>]",
              L"Monopoly online", MB_ICONINFORMATION);
  return 1;
}
