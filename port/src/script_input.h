// Scripted controller for unattended testing: replays timed button presses on guest user 0.
//
// Script lines: <start_ms> <button>[+<button>...] [hold_ms]   (# starts a comment)
// Buttons: A B X Y START BACK UP DOWN LEFT RIGHT LB RB LT RT, and LS_UP/LS_DOWN/LS_LEFT/LS_RIGHT
// for a fully deflected left stick. Times count from boot; a start of
// "+<ms>" (or just "+") counts from when the line is read, which is how lines appended to the
// file while the game runs act as live presses.
#pragma once

#include <chrono>
#include <ios>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>

namespace monopoly {
using namespace rex;
using namespace rex::input;

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  // Returns nullptr (and logs) if the script can't be read or parsed.
  static std::unique_ptr<ScriptInputDriver> Load(const std::string& path);

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId, X_INPUT_VIBRATION*) override {
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  struct Press {
    uint32_t start_ms, hold_ms;
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t thumb_lx, thumb_ly;
  };
  // Exposed for the self-check in script_input.cpp. "+" times are relative to now_ms.
  static bool Parse(const std::string& text, std::vector<Press>* out, std::string* error,
                    uint32_t now_ms);

 private:
  ScriptInputDriver() : InputDriver(nullptr, 0) {}
  Press Current();                  // everything held right now
  void PollFile(uint32_t now_ms);  // picks up appended lines

  std::mutex mutex_;
  std::vector<Press> presses_;
  std::string path_;
  std::streamoff read_pos_ = 0;
  uint32_t last_poll_ms_ = 0;
  uint32_t keys_down_ = 0;  // keystroke state already reported to the guest
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  uint32_t packet_ = 0;
};

}  // namespace monopoly
