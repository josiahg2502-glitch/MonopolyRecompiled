// Loopback test of the netplay module: join/slots/reject, input uplink, H.264 video and audio downlink.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "netplay/netplay.h"
#include "netplay/video_codec.h"

using namespace std::chrono_literals;

#define STEP(s) std::fprintf(stderr, "step: %s\n", s)
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

template <typename F>
bool WaitFor(F&& cond, std::chrono::milliseconds timeout = 3000ms) {
  auto end = std::chrono::steady_clock::now() + timeout;
  while (!cond()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

static void TestColorRoundTrip() {
  const uint32_t w = 4, h = 2;
  uint8_t bgra[w * h * 4], back[w * h * 4], nv12[w * h * 3 / 2];
  for (uint32_t i = 0; i < w * h; ++i) bgra[i * 4 + 0] = 40, bgra[i * 4 + 1] = 160, bgra[i * 4 + 2] = 220, bgra[i * 4 + 3] = 255;
  netplay::BgraToNv12(bgra, w * 4, w, h, nv12);
  netplay::Nv12ToBgra(nv12, nv12 + w * h, w, w, h, back);
  for (uint32_t i = 0; i < w * h * 4; ++i) CHECK(std::abs(int(bgra[i]) - int(back[i])) <= 3);
}

int main() {
  TestColorRoundTrip();
  STEP("color");

  const uint16_t port = 27361;
  std::string error;
  auto host = netplay::Host::Start(port, "Host", &error);
  if (!host) std::fprintf(stderr, "%s\n", error.c_str());
  CHECK(host);

  std::mutex mutex;
  int frames = 0;
  netplay::Frame last;
  std::atomic<uint32_t> audio_frames{0};
  std::atomic<int> roster_count{0};
  netplay::Client::Callbacks cb;
  cb.on_video = [&](const netplay::Frame& f) {
    std::lock_guard lock(mutex);
    ++frames;
    last = f;
  };
  cb.on_audio = [&](const int16_t*, uint32_t n) { audio_frames += n; };
  cb.on_roster = [&](const netplay::Roster& r) {
    int n = 0;
    for (bool c : r.connected) n += c;
    roster_count = n;
  };

  auto c1 = netplay::Client::Connect("127.0.0.1", port, "Alice", cb, &error);
  if (!c1) std::fprintf(stderr, "%s\n", error.c_str());
  CHECK(c1 && c1->slot() == 1);
  STEP("connected");
  CHECK(WaitFor([&] { return roster_count == 2; }));

  netplay::Gamepad pad;
  pad.buttons = 0x1234;
  pad.thumb_lx = -32768;
  c1->SendInput(pad);
  netplay::Gamepad got;
  CHECK(WaitFor([&] { return host->GetInput(1, &got) && got == pad; }));
  CHECK(!host->GetInput(2, &got));
  STEP("input");

  // Synthetic 320x240 frame: left half red, right half blue.
  const uint32_t w = 320, h = 240;
  std::vector<uint8_t> image(w * h * 4);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* p = &image[(y * w + x) * 4];
      bool left = x < w / 2;
      p[0] = left ? 0 : 255, p[1] = 0, p[2] = left ? 255 : 0, p[3] = 255;
    }
  std::vector<int16_t> audio(480 * 2, 1000);
  auto end = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < end) {
    host->SubmitVideo(image.data(), w, h, w * 4);
    host->SubmitAudio(audio.data(), 480);
    std::this_thread::sleep_for(16ms);
  }
  CHECK(WaitFor([&] {
    std::lock_guard lock(mutex);
    return frames >= 10;
  }));
  {
    std::lock_guard lock(mutex);
    CHECK(last.width == w && last.height == h);
    const uint8_t* left = &last.bgra[(120 * w + 40) * 4];
    const uint8_t* right = &last.bgra[(120 * w + 280) * 4];
    CHECK(left[2] > 200 && left[0] < 60);
    CHECK(right[0] > 200 && right[2] < 60);
    std::printf("video: %d frames decoded\n", frames);
  }
  CHECK(audio_frames >= 480 * 50);
  STEP("av");

  auto c2 = netplay::Client::Connect("localhost", port, "Bob", {}, &error);
  auto c3 = netplay::Client::Connect("127.0.0.1", port, "Carol", {}, &error);
  CHECK(c2 && c2->slot() == 2 && c3 && c3->slot() == 3);
  auto c4 = netplay::Client::Connect("127.0.0.1", port, "Dave", {}, &error);
  CHECK(!c4 && error.find("full") != std::string::npos);
  CHECK(WaitFor([&] { return roster_count == 4; }));
  STEP("full");

  c2.reset();
  CHECK(WaitFor([&] { return !host->GetRoster().connected[2] && roster_count == 3; }));
  auto c5 = netplay::Client::Connect("127.0.0.1", port, "Erin", {}, &error);
  CHECK(c5 && c5->slot() == 2);
  STEP("rejoin");

  std::atomic<bool> dropped{false};
  netplay::Client::Callbacks cb6;
  cb6.on_disconnect = [&](const std::string&) { dropped = true; };
  c5.reset();
  STEP("c5 gone");
  auto c6 = netplay::Client::Connect("127.0.0.1", port, "Finn", cb6, &error);
  CHECK(c6);
  STEP("c6");
  host.reset();
  STEP("host gone");
  CHECK(WaitFor([&] { return dropped.load(); }));

  std::printf("netplay selftest passed\n");
  return 0;
}
