// Headless remote player for end-to-end tests against a hosting game.
//   net_probe <address> <port> <out.bmp> <seconds> [<ms>:<buttons hex> ...]
// Joins, holds each button mask from its time until the next entry, and after <seconds> writes
// the last received frame to <out.bmp>. Prints frame/audio counts; exits non-zero if no video.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "netplay/netplay.h"

static void WriteBmp(const char* path, const netplay::Frame& f) {
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof(ih);
  ih.biWidth = LONG(f.width);
  ih.biHeight = -LONG(f.height);  // top-down
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = DWORD(fh.bfOffBits + f.bgra.size());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
  out.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
  out.write(reinterpret_cast<const char*>(f.bgra.data()), std::streamsize(f.bgra.size()));
}

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: net_probe <address> <port> <out.bmp> <seconds> [<ms>:<buttons hex> ...]\n");
    return 2;
  }
  struct Step {
    int ms;
    uint16_t buttons;
  };
  std::vector<Step> steps;
  for (int i = 5; i < argc; ++i) {
    std::string s = argv[i];
    auto colon = s.find(':');
    if (colon == std::string::npos) return std::fprintf(stderr, "bad step %s\n", argv[i]), 2;
    steps.push_back({std::atoi(s.substr(0, colon).c_str()), uint16_t(std::strtoul(s.c_str() + colon + 1, nullptr, 16))});
  }

  std::mutex mutex;
  netplay::Frame last;
  std::atomic<int> frames{0};
  std::atomic<uint32_t> audio{0};
  netplay::Client::Callbacks cb;
  cb.on_video = [&](const netplay::Frame& f) {
    std::lock_guard lock(mutex);
    last = f;
    ++frames;
  };
  cb.on_audio = [&](const int16_t*, uint32_t n) { audio += n; };
  std::string error;
  auto client = netplay::Client::Connect(argv[1], uint16_t(std::atoi(argv[2])), "Probe", cb, &error);
  if (!client) return std::fprintf(stderr, "connect failed: %s\n", error.c_str()), 1;
  std::printf("joined as slot %d\n", client->slot());

  auto start = std::chrono::steady_clock::now();
  auto end = start + std::chrono::seconds(std::atoi(argv[4]));
  while (std::chrono::steady_clock::now() < end) {
    int now = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    netplay::Gamepad pad;
    for (auto& s : steps)
      if (now >= s.ms) pad.buttons = s.buttons;
    client->SendInput(pad);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  std::lock_guard lock(mutex);
  std::printf("video frames %d (%ux%u), audio samples %u\n", frames.load(), last.width, last.height, audio.load());
  if (!frames) return 1;
  WriteBmp(argv[3], last);
  return 0;
}
