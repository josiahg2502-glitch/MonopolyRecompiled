#include "netplay_bridge.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <rex/audio/audio_driver.h>
#include <rex/cvar.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include "keystroke.h"

REXCVAR_DECLARE(int32_t, video_mode_height);

namespace monopoly {

// ---- input -------------------------------------------------------------------------------------

int RemoteInputDriver::SlotOf(DeviceId id) {
  for (int slot = 1; slot < netplay::kMaxPlayers; ++slot)
    if (id == SlotDevice(slot)) return slot;
  return 0;
}

// A slot stays plugged in (idle) after its player drops: unplugging would pause the game for
// everyone, and a rejoining player gets the lowest free slot back, i.e. usually their own.
void RemoteInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  auto roster = host_->GetRoster();
  for (int slot = 1; slot < netplay::kMaxPlayers; ++slot) {
    if (roster.connected[slot]) seen_[slot] = true;
    if (!seen_[slot]) continue;
    DeviceInfo info;
    info.id = SlotDevice(slot);
    info.name = "Online P" + std::to_string(slot + 1) +
                (roster.connected[slot] ? " (" + roster.names[slot] + ")" : " (disconnected)");
    out.push_back(info);
  }
}

X_RESULT RemoteInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  int slot = SlotOf(id);
  if (!slot || !seen_[slot]) return X_ERROR_DEVICE_NOT_CONNECTED;
  netplay::Gamepad pad;
  host_->GetInput(slot, &pad);  // leaves the idle pad when the player is away
  if (!(pad == last_[slot])) {
    last_[slot] = pad;
    ++packet_[slot];  // XInput only advances the packet number when the state changes
  }
  std::memset(out_state, 0, sizeof(*out_state));
  out_state->packet_number = packet_[slot];
  out_state->gamepad.buttons = pad.buttons;
  out_state->gamepad.left_trigger = pad.left_trigger;
  out_state->gamepad.right_trigger = pad.right_trigger;
  out_state->gamepad.thumb_lx = pad.thumb_lx;
  out_state->gamepad.thumb_ly = pad.thumb_ly;
  out_state->gamepad.thumb_rx = pad.thumb_rx;
  out_state->gamepad.thumb_ry = pad.thumb_ry;
  return X_ERROR_SUCCESS;
}

X_RESULT RemoteInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) {
  int slot = SlotOf(id);
  if (!slot || !seen_[slot]) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT RemoteInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out_keystroke) {
  int slot = SlotOf(id);
  if (!slot || !seen_[slot]) return X_ERROR_DEVICE_NOT_CONNECTED;
  netplay::Gamepad pad;
  host_->GetInput(slot, &pad);
  uint32_t held = pad.buttons | (pad.left_trigger > 30 ? 1u << 16 : 0) | (pad.right_trigger > 30 ? 1u << 17 : 0);
  if (!NextKeystroke(held, keys_down_[slot], out_keystroke)) return X_ERROR_EMPTY;
  out_keystroke->user_index = uint8_t(slot);
  return X_ERROR_SUCCESS;
}

void NetplayAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  for (auto& user : users_) user.clear();
  bool remote_held[kMaxGuestUsers] = {};
  for (const auto& d : devices)
    if (int slot = RemoteInputDriver::SlotOf(d.id)) {
      users_[slot].push_back(d.id);
      remote_held[slot] = true;
    }
  uint32_t next_local = 0;
  for (const auto& d : devices) {  // sorted by ordinal, so local pads keep their order
    if (RemoteInputDriver::SlotOf(d.id)) continue;
    if (d.synthetic) {
      users_[0].push_back(d.id);
      continue;
    }
    while (next_local < kMaxGuestUsers && next_local > 0 && remote_held[next_local]) ++next_local;
    if (next_local < kMaxGuestUsers) users_[next_local++].push_back(d.id);
  }
}

void NetplayAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index < kMaxGuestUsers) out = users_[user_index];
}

// ---- audio -------------------------------------------------------------------------------------

namespace {

class TeeAudioDriver final : public audio::AudioDriver {
 public:
  TeeAudioDriver(memory::Memory* memory, audio::AudioDriver* inner, std::shared_ptr<netplay::Host> host)
      : AudioDriver(memory), inner_(inner), host_(std::move(host)) {}

  audio::AudioDriver* inner() const { return inner_; }

  // Guest frames are 256 samples x 6 channels (FL FR C LFE SL SR), channel after channel,
  // big-endian float.
  void SubmitFrame(uint32_t samples_ptr) override {
    if (host_->PeerCount()) {
      const auto* in = memory_->TranslateVirtual<const uint32_t*>(samples_ptr);
      auto sample = [&](int channel, int i) {
        uint32_t bits = __builtin_bswap32(in[channel * kSamples + i]);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
      };
      int16_t* out = &pending_[pending_frames_ * kSamples * 2];
      for (int i = 0; i < kSamples; ++i) {
        float c = 0.7071f * sample(2, i);
        float l = sample(0, i) + c + 0.7071f * sample(4, i);
        float r = sample(1, i) + c + 0.7071f * sample(5, i);
        out[i * 2] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767.0f);
        out[i * 2 + 1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767.0f);
      }
      // Four guest frames (~21 ms) per network message.
      if (++pending_frames_ == kBatch) {
        host_->SubmitAudio(pending_, kSamples * kBatch);
        pending_frames_ = 0;
      }
    }
    inner_->SubmitFrame(samples_ptr);
  }

 private:
  static constexpr int kSamples = 256, kBatch = 4;
  audio::AudioDriver* inner_;
  std::shared_ptr<netplay::Host> host_;
  int16_t pending_[kSamples * 2 * kBatch];
  int pending_frames_ = 0;
};

}  // namespace

X_STATUS NetAudioSystem::CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                      audio::AudioDriver** out_driver) {
  audio::AudioDriver* inner = nullptr;
  X_STATUS status = SDLAudioSystem::CreateDriver(index, semaphore, &inner);
  if (XFAILED(status)) return status;
  *out_driver = new TeeAudioDriver(memory(), inner, host_);
  return status;
}

void NetAudioSystem::DestroyDriver(audio::AudioDriver* driver) {
  auto* tee = static_cast<TeeAudioDriver*>(driver);
  SDLAudioSystem::DestroyDriver(tee->inner());
  delete tee;
}

// ---- video -------------------------------------------------------------------------------------

FrameCapture::FrameCapture(rex::Runtime* runtime, std::shared_ptr<netplay::Host> host)
    : runtime_(runtime), host_(std::move(host)), thread_([this] { Loop(); }) {}

FrameCapture::~FrameCapture() {
  stop_ = true;
  thread_.join();
}

void FrameCapture::Loop() {
  using namespace std::chrono;
  ui::RawImage image;
  auto next = steady_clock::now();
  while (!stop_) {
    next += milliseconds(33);
    std::this_thread::sleep_until(next);
    if (!host_->PeerCount()) continue;
    auto* graphics = runtime_->graphics_system();
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    // shortcut: CaptureGuestOutput allocates a readback buffer and waits on the GPU each call;
    // keep a persistent readback ring in the presenter if this shows up in frame times.
    if (!presenter || !presenter->CaptureGuestOutput(image)) continue;
    // The guest frontbuffer is tile-aligned (720 -> 736 rows); the rest is padding.
    image.height = std::min<uint32_t>(image.height, uint32_t(REXCVAR_GET(video_mode_height)));
    for (uint32_t y = 0; y < image.height; ++y) {  // RGBX -> BGRA in place
      uint8_t* p = image.data.data() + y * image.stride;
      for (uint32_t x = 0; x < image.width; ++x, p += 4) {
        std::swap(p[0], p[2]);
        p[3] = 255;
      }
    }
    host_->SubmitVideo(image.data.data(), image.width, image.height, uint32_t(image.stride));
  }
}

}  // namespace monopoly
