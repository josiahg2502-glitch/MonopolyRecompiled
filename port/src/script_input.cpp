#include "script_input.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <sstream>

#include <rex/logging.h>

#include "keystroke.h"

namespace monopoly {
namespace {

constexpr auto kScriptDevice = static_cast<rex::input::DeviceId>(0x53435249);  // "SCRI"

struct ButtonName {
  const char* name;
  uint16_t mask;
};
constexpr ButtonName kButtons[] = {
    {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
    {"START", 0x0010}, {"BACK", 0x0020}, {"LB", 0x0100},   {"RB", 0x0200},
    {"A", 0x1000},     {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},
};

}  // namespace

bool ScriptInputDriver::Parse(const std::string& text, std::vector<Press>* out, std::string* error,
                              uint32_t now_ms) {
  std::istringstream lines(text);
  std::string line;
  for (int number = 1; std::getline(lines, line); ++number) {
    line = line.substr(0, line.find('#'));
    std::istringstream fields(line);
    std::string start, combo;
    Press press{0, 120, 0, 0, 0, 0, 0};
    if (!(fields >> start)) continue;  // blank or comment
    try {
      press.start_ms = start[0] == '+' ? now_ms + (start.size() > 1 ? std::stoul(start.substr(1)) : 0)
                                       : std::stoul(start);
    } catch (const std::exception&) {
      return *error = "line " + std::to_string(number) + ": bad time " + start, false;
    }
    if (!(fields >> combo)) return *error = "line " + std::to_string(number) + ": missing button", false;
    fields >> press.hold_ms;
    std::istringstream names(combo);
    for (std::string name; std::getline(names, name, '+');) {
      if (name == "LT") { press.left_trigger = 255; continue; }
      if (name == "RT") { press.right_trigger = 255; continue; }
      // Left stick fully deflected: LS_UP / LS_DOWN / LS_LEFT / LS_RIGHT.
      if (name == "LS_UP") { press.thumb_ly = 32767; continue; }
      if (name == "LS_DOWN") { press.thumb_ly = -32768; continue; }
      if (name == "LS_LEFT") { press.thumb_lx = -32768; continue; }
      if (name == "LS_RIGHT") { press.thumb_lx = 32767; continue; }
      bool known = false;
      for (auto& b : kButtons)
        if (name == b.name) press.buttons |= b.mask, known = true;
      if (!known) return *error = "line " + std::to_string(number) + ": unknown button " + name, false;
    }
    out->push_back(press);
  }
  return true;
}

std::unique_ptr<ScriptInputDriver> ScriptInputDriver::Load(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    REXLOG_ERROR("input_script: cannot open {}", path);
    return nullptr;
  }
  std::stringstream text;
  text << file.rdbuf();
  std::unique_ptr<ScriptInputDriver> driver(new ScriptInputDriver());
  std::string error;
  if (!Parse(text.str(), &driver->presses_, &error, 0)) {
    REXLOG_ERROR("input_script {}: {}", path, error);
    return nullptr;
  }
  driver->path_ = path;
  driver->read_pos_ = std::streamoff(text.str().size());
  REXLOG_INFO("input_script: {} presses from {} (appended lines are picked up live)",
              driver->presses_.size(), path);
  return driver;
}

// Lines appended to the script while the game runs are added as they arrive; "+<ms>" times are
// relative to arrival, so a tester can press buttons by appending e.g. "+ A".
void ScriptInputDriver::PollFile(uint32_t now_ms) {
  if (now_ms - last_poll_ms_ < 100) return;
  last_poll_ms_ = now_ms;
  std::ifstream file(path_, std::ios::binary);
  if (!file.seekg(read_pos_)) return;
  std::string tail((std::istreambuf_iterator<char>(file)), {});
  auto end = tail.rfind('\n');
  if (end == std::string::npos) return;  // wait for a complete line
  tail.resize(end + 1);
  read_pos_ += std::streamoff(tail.size());
  std::string error;
  if (!Parse(tail, &presses_, &error, now_ms)) REXLOG_ERROR("input_script {}: {}", path_, error);
}

void ScriptInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  rex::input::DeviceInfo info;
  info.id = kScriptDevice;
  info.name = "Input script";
  info.synthetic = true;  // merges into guest user 0
  out.push_back(info);
}

ScriptInputDriver::Press ScriptInputDriver::Current() {
  std::lock_guard lock(mutex_);  // guest threads poll input concurrently
  auto now_ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start_).count());
  PollFile(now_ms);
  Press held{0, 0, 0, 0, 0, 0, 0};
  for (auto& p : presses_) {
    if (now_ms >= p.start_ms && now_ms < p.start_ms + p.hold_ms) {
      held.buttons |= p.buttons;
      held.left_trigger |= p.left_trigger;
      held.right_trigger |= p.right_trigger;
      if (p.thumb_lx) held.thumb_lx = p.thumb_lx;
      if (p.thumb_ly) held.thumb_ly = p.thumb_ly;
    }
  }
  return held;
}

X_RESULT ScriptInputDriver::GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  Press held = Current();
  std::memset(out_state, 0, sizeof(*out_state));
  out_state->packet_number = ++packet_;
  out_state->gamepad.buttons = held.buttons;
  out_state->gamepad.left_trigger = held.left_trigger;
  out_state->gamepad.right_trigger = held.right_trigger;
  out_state->gamepad.thumb_lx = held.thumb_lx;
  out_state->gamepad.thumb_ly = held.thumb_ly;
  return X_ERROR_SUCCESS;
}

// Menus read XInputGetKeystroke, so presses and releases are also reported as key events.
X_RESULT ScriptInputDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                                               X_INPUT_KEYSTROKE* out_keystroke) {
  if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  Press held = Current();
  uint32_t now = held.buttons | (held.left_trigger ? 1u << 16 : 0) | (held.right_trigger ? 1u << 17 : 0);
  std::lock_guard lock(mutex_);
  return NextKeystroke(now, keys_down_, out_keystroke) ? X_ERROR_SUCCESS : X_ERROR_EMPTY;
}

X_RESULT ScriptInputDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

// Self-check of the parser; runs once at startup in debug builds.
[[maybe_unused]] static const bool kParserChecked = [] {
  std::vector<ScriptInputDriver::Press> p;
  std::string err;
  bool ok = ScriptInputDriver::Parse("# boot\n1000 A\n2000 START+LB 300 # menu\n\n3000 RT\n+ B\n+50 X 10",
                                     &p, &err, 500);
  assert(ok && p.size() == 5);
  assert(p[3].start_ms == 500 && p[3].buttons == 0x2000);
  assert(p[4].start_ms == 550 && p[4].hold_ms == 10);
  ok = ScriptInputDriver::Parse("0 LS_LEFT+A", &p, &err, 0);
  assert(ok && p.back().thumb_lx == -32768 && p.back().buttons == 0x1000);
  assert(p[0].start_ms == 1000 && p[0].buttons == 0x1000 && p[0].hold_ms == 120);
  assert(p[1].buttons == (0x0010 | 0x0100) && p[1].hold_ms == 300);
  assert(p[2].right_trigger == 255 && p[2].buttons == 0);
  assert(!ScriptInputDriver::Parse("5 Z", &p, &err, 0));
  assert(!ScriptInputDriver::Parse("+x A", &p, &err, 0));
  return ok;
}();

}  // namespace monopoly
