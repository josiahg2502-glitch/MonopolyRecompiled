// H.264 encode/decode through the Media Foundation software MFTs that ship with Windows.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "netplay.h"

namespace netplay {

class VideoEncoder {
 public:
  // width/height must be even; bitrate in bits per second.
  static std::unique_ptr<VideoEncoder> Create(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate);
  virtual ~VideoEncoder() = default;
  // Returns Annex B bytes for this frame (may be empty while the MFT buffers).
  virtual bool Encode(const uint8_t* bgra, uint32_t pitch, bool force_keyframe, std::vector<uint8_t>* out,
                      bool* keyframe) = 0;
};

class VideoDecoder {
 public:
  static std::unique_ptr<VideoDecoder> Create();
  virtual ~VideoDecoder() = default;
  // Feeds one access unit; fills `out` (cropped to width x height) when a picture comes out.
  virtual bool Decode(const uint8_t* data, size_t size, uint32_t width, uint32_t height, Frame* out) = 0;
};

// BT.601 limited range. NV12 here is a tight width x height Y plane followed by the UV plane.
void BgraToNv12(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint32_t height, uint8_t* nv12);
void Nv12ToBgra(const uint8_t* y, const uint8_t* uv, uint32_t stride, uint32_t width, uint32_t height,
                uint8_t* bgra);

// Media Foundation must be started once per process before Create().
bool StartMediaFoundation();

}  // namespace netplay
