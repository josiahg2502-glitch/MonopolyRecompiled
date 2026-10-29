// Installing the game from the player's own Xbox 360 disc image (XDVDFS .iso): copies default.xex
// and the .big archives out of it. Runs on a background thread; progress is read by the UI.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

struct InstallProgress {
  std::atomic<uint64_t> done{0}, total{0};  // bytes
  std::atomic<bool> cancel{false}, finished{false};
  std::mutex mutex;
  std::string file;   // being copied
  std::string error;  // set when finished without success

  std::string File() {
    std::lock_guard lock(mutex);
    return file;
  }
};

// Blocking. Leaves no partial files behind: each file is written under a temporary name first.
bool InstallFromDiscImage(const std::filesystem::path& image, const std::filesystem::path& dest, InstallProgress& p);
