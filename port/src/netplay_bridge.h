// Connects the game to netplay::Host: remote players drive guest users 1-3, and the host's
// presented frames and mixed audio stream out to them.
#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_driver.h>

#include "netplay/netplay.h"

namespace rex {
class Runtime;
}

namespace monopoly {
using namespace rex;
using namespace rex::input;

// One guest controller per connected remote player (slots 1-3).
class RemoteInputDriver final : public InputDriver {
 public:
  explicit RemoteInputDriver(std::shared_ptr<netplay::Host> host)
      : InputDriver(nullptr, 0), host_(std::move(host)) {}

  static DeviceId SlotDevice(int slot) { return static_cast<DeviceId>(0x4E455400u + slot); }  // "NET"
  static int SlotOf(DeviceId id);  // 0 if not a remote device

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) override { return X_ERROR_SUCCESS; }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  std::shared_ptr<netplay::Host> host_;
  std::atomic<bool> seen_[netplay::kMaxPlayers] = {};  // slot has had a player this session
  uint32_t packet_[netplay::kMaxPlayers] = {};
  uint32_t keys_down_[netplay::kMaxPlayers] = {};
  netplay::Gamepad last_[netplay::kMaxPlayers];
};

// Remote slot N is always guest user N. Local pads take user 0 first, then any user no remote
// player holds; keyboard/script/NOP devices feed user 0.
class NetplayAssignment final : public DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override;

 private:
  std::vector<std::vector<DeviceId>> users_ = std::vector<std::vector<DeviceId>>(kMaxGuestUsers);
};

// SDL output plus a copy of every mixed frame, folded to stereo, for the remote players.
class NetAudioSystem final : public audio::sdl::SDLAudioSystem {
 public:
  NetAudioSystem(runtime::FunctionDispatcher* dispatcher, std::shared_ptr<netplay::Host> host)
      : SDLAudioSystem(dispatcher), host_(std::move(host)) {}

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        audio::AudioDriver** out_driver) override;
  void DestroyDriver(audio::AudioDriver* driver) override;

 private:
  std::shared_ptr<netplay::Host> host_;
};

// Captures the guest output for the remote players at the stream rate.
class FrameCapture {
 public:
  FrameCapture(rex::Runtime* runtime, std::shared_ptr<netplay::Host> host);
  ~FrameCapture();

 private:
  void Loop();

  rex::Runtime* runtime_;
  std::shared_ptr<netplay::Host> host_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace monopoly
