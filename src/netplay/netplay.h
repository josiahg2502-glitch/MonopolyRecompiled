// Host-streamed online play: the host runs the game with 4 local player slots,
// remote players send pad input and receive the host's video/audio.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace netplay {

constexpr int kMaxPlayers = 4;  // slot 0 is always the host
constexpr uint16_t kDefaultPort = 27360;
constexpr uint32_t kProtocolVersion = 1;

// Wire/API mirror of XINPUT_GAMEPAD (X_INPUT_GAMEPAD on the guest).
struct Gamepad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
  bool operator==(const Gamepad&) const = default;
};

struct Roster {
  bool connected[kMaxPlayers] = {};
  std::string names[kMaxPlayers];
};

// A decoded or to-be-encoded video frame, tightly packed BGRA8.
struct Frame {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> bgra;
};

class Host {
 public:
  static std::unique_ptr<Host> Start(uint16_t port, std::string host_name, std::string* error);
  virtual ~Host() = default;

  // Latest input from the remote player in `slot` (1..3); false if nobody is there.
  virtual bool GetInput(int slot, Gamepad* out) = 0;
  // Encode and send one frame to every client. Call once per presented frame.
  virtual void SubmitVideo(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch) = 0;
  // Interleaved stereo int16 at 48 kHz.
  virtual void SubmitAudio(const int16_t* samples, uint32_t frames) = 0;
  virtual Roster GetRoster() = 0;
  // Remote players currently connected; capture work can be skipped while it's 0.
  virtual int PeerCount() const = 0;
};

class Client {
 public:
  struct Callbacks {
    std::function<void(const Frame&)> on_video;                       // network thread
    std::function<void(const int16_t*, uint32_t frames)> on_audio;    // network thread
    std::function<void(const Roster&)> on_roster;                     // network thread
    std::function<void(const std::string& reason)> on_disconnect;     // network thread
  };

  static std::unique_ptr<Client> Connect(const std::string& address, uint16_t port, std::string name,
                                         Callbacks callbacks, std::string* error);
  virtual ~Client() = default;

  virtual int slot() const = 0;  // 1..3, assigned by the host
  virtual void SendInput(const Gamepad& pad) = 0;
};

}  // namespace netplay
