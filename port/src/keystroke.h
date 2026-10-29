// XInputGetKeystroke emulation from polled button state: one press/release event per call.
#pragma once

#include <cstdint>
#include <cstring>

#include <rex/input/input.h>

namespace monopoly {

// `held` is the button mask with bits 16/17 standing for LT/RT; `reported` is the state already
// sent to the guest and is updated. Returns false when nothing changed (X_ERROR_EMPTY).
inline bool NextKeystroke(uint32_t held, uint32_t& reported, rex::input::X_INPUT_KEYSTROKE* out) {
  static constexpr uint16_t kVk[18] = {
      0x5810, 0x5811, 0x5812, 0x5813, 0x5814, 0x5815, 0x5816, 0x5817,  // dpad, start, back, thumbs
      0x5805, 0x5804, 0, 0,                                            // LB, RB, guide, unused
      0x5800, 0x5801, 0x5802, 0x5803,                                  // A B X Y
      0x5806, 0x5807};                                                 // LT RT
  for (int bit = 0; bit < 18; ++bit) {
    uint32_t mask = 1u << bit;
    if (((held ^ reported) & mask) == 0 || !kVk[bit]) continue;
    reported ^= mask;
    std::memset(out, 0, sizeof(*out));
    out->virtual_key = kVk[bit];
    out->flags = (held & mask) ? 0x0001 : 0x0002;  // XINPUT_KEYSTROKE_KEYDOWN / KEYUP
    return true;
  }
  return false;
}

}  // namespace monopoly
