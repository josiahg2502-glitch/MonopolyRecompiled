#include "video_codec.h"

#include <algorithm>

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace netplay {
namespace {

uint8_t Clip(int v) { return uint8_t(std::clamp(v, 0, 255)); }

void SetCodecValue(IMFTransform* mft, const GUID& key, VARTYPE vt, uint32_t value) {
  ComPtr<ICodecAPI> api;
  if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&api)))) return;
  VARIANT var;
  VariantInit(&var);
  var.vt = vt;
  if (vt == VT_BOOL)
    var.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  else
    var.ulVal = value;
  api->SetValue(&key, &var);  // optional knobs: an MFT that lacks one still works
}

ComPtr<IMFSample> MakeSample(DWORD size, ComPtr<IMFMediaBuffer>* buffer_out = nullptr) {
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(size, &buffer)) ||
      FAILED(sample->AddBuffer(buffer.Get())))
    return nullptr;
  if (buffer_out) *buffer_out = buffer;
  return sample;
}

// Pulls every output the MFT has ready. `on_sample` gets each produced sample.
template <typename F>
HRESULT Drain(IMFTransform* mft, F&& on_sample) {
  for (;;) {
    MFT_OUTPUT_STREAM_INFO info{};
    HRESULT hr = mft->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return hr;
    MFT_OUTPUT_DATA_BUFFER odb{};
    ComPtr<IMFSample> sample;
    if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
      sample = MakeSample(std::max<DWORD>(info.cbSize, 1));
      if (!sample) return E_OUTOFMEMORY;
      odb.pSample = sample.Get();
    }
    DWORD status = 0;
    hr = mft->ProcessOutput(0, 1, &odb, &status);
    if (odb.pEvents) odb.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
    if (FAILED(hr)) {
      if (!sample && odb.pSample) odb.pSample->Release();
      return hr;
    }
    on_sample(odb.pSample);
    if (!sample && odb.pSample) odb.pSample->Release();
  }
}

class MfEncoder final : public VideoEncoder {
 public:
  bool Init(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_))))
      return false;

    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(mft_->SetOutputType(0, out.Get(), 0))) return false;

    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return false;

    SetCodecValue(mft_.Get(), CODECAPI_AVLowLatencyMode, VT_BOOL, 1);
    SetCodecValue(mft_.Get(), CODECAPI_AVEncMPVGOPSize, VT_UI4, fps * 4);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  bool Encode(const uint8_t* bgra, uint32_t pitch, bool force_keyframe, std::vector<uint8_t>* out,
              bool* keyframe) override {
    out->clear();
    *keyframe = false;
    DWORD size = width_ * height_ * 3 / 2;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample = MakeSample(size, &buffer);
    if (!sample) return false;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return false;
    BgraToNv12(bgra, pitch, width_, height_, dst);
    buffer->Unlock();
    buffer->SetCurrentLength(size);
    const LONGLONG duration = 10'000'000LL / fps_;
    sample->SetSampleTime(LONGLONG(frame_index_++) * duration);
    sample->SetSampleDuration(duration);
    if (force_keyframe) SetCodecValue(mft_.Get(), CODECAPI_AVEncVideoForceKeyFrame, VT_UI4, 1);

    auto collect = [&](IMFSample* s) {
      ComPtr<IMFMediaBuffer> b;
      if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
      BYTE* p = nullptr;
      DWORD len = 0;
      if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
        out->insert(out->end(), p, p + len);
        b->Unlock();
      }
      if (MFGetAttributeUINT32(s, MFSampleExtension_CleanPoint, FALSE)) *keyframe = true;
    };

    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      if (FAILED(Drain(mft_.Get(), collect))) return false;
      hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    return SUCCEEDED(hr) && SUCCEEDED(Drain(mft_.Get(), collect));
  }

 private:
  ComPtr<IMFTransform> mft_;
  uint32_t width_ = 0, height_ = 0, fps_ = 60;
  uint64_t frame_index_ = 0;
};

class MfDecoder final : public VideoDecoder {
 public:
  bool Init() {
    if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_))))
      return false;
    SetCodecValue(mft_.Get(), CODECAPI_AVLowLatencyMode, VT_BOOL, 1);
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(mft_->SetInputType(0, in.Get(), 0)) || !SelectOutputType()) return false;
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
  }

  bool Decode(const uint8_t* data, size_t size, uint32_t width, uint32_t height, Frame* out) override {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample = MakeSample(DWORD(size), &buffer);
    if (!sample) return false;
    BYTE* dst = nullptr;
    if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return false;
    memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(size));

    bool produced = false;
    auto convert = [&](IMFSample* s) {
      ComPtr<IMFMediaBuffer> b;
      if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
      BYTE* p = nullptr;
      DWORD len = 0;
      if (FAILED(b->Lock(&p, nullptr, &len))) return;
      const uint32_t w = std::min(width, out_width_), h = std::min(height, out_height_);
      if (len >= stride_ * out_height_ * 3 / 2 && w && h) {
        out->width = w;
        out->height = h;
        out->bgra.resize(size_t(w) * h * 4);
        Nv12ToBgra(p, p + size_t(stride_) * out_height_, stride_, w, h, out->bgra.data());
        produced = true;
      }
      b->Unlock();
    };

    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    for (int attempt = 0; attempt < 2; ++attempt) {
      HRESULT drained = Drain(mft_.Get(), convert);
      if (drained == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (!SelectOutputType()) return false;
        continue;
      }
      if (FAILED(drained)) return false;
      if (hr == MF_E_NOTACCEPTING) {
        hr = mft_->ProcessInput(0, sample.Get(), 0);
        continue;
      }
      break;
    }
    return SUCCEEDED(hr) && produced;
  }

 private:
  bool SelectOutputType() {
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      if (FAILED(mft_->GetOutputAvailableType(0, i, &type))) return false;
      GUID subtype{};
      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      if (subtype != MFVideoFormat_NV12) continue;
      if (FAILED(mft_->SetOutputType(0, type.Get(), 0))) return false;
      MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &out_width_, &out_height_);
      stride_ = MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, out_width_);
      return true;
    }
  }

  ComPtr<IMFTransform> mft_;
  UINT32 out_width_ = 0, out_height_ = 0, stride_ = 0;
};

}  // namespace

bool StartMediaFoundation() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
  return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

std::unique_ptr<VideoEncoder> VideoEncoder::Create(uint32_t width, uint32_t height, uint32_t fps,
                                                   uint32_t bitrate) {
  if (!width || !height || (width | height) & 1) return nullptr;
  auto enc = std::make_unique<MfEncoder>();
  return enc->Init(width, height, fps, bitrate) ? std::move(enc) : nullptr;
}

std::unique_ptr<VideoDecoder> VideoDecoder::Create() {
  auto dec = std::make_unique<MfDecoder>();
  return dec->Init() ? std::move(dec) : nullptr;
}

void BgraToNv12(const uint8_t* bgra, uint32_t pitch, uint32_t width, uint32_t height, uint8_t* nv12) {
  uint8_t* y_plane = nv12;
  uint8_t* uv_plane = nv12 + size_t(width) * height;
  for (uint32_t y = 0; y < height; y += 2) {
    const uint8_t* rows[2] = {bgra + size_t(y) * pitch, bgra + size_t(y + 1) * pitch};
    for (uint32_t x = 0; x < width; x += 2) {
      int sr = 0, sg = 0, sb = 0;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const uint8_t* p = rows[dy] + (x + dx) * 4;
          int b = p[0], g = p[1], r = p[2];
          y_plane[size_t(y + dy) * width + x + dx] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
          sr += r, sg += g, sb += b;
        }
      sr /= 4, sg /= 4, sb /= 4;
      uint8_t* uv = uv_plane + size_t(y / 2) * width + x;
      uv[0] = uint8_t(((-38 * sr - 74 * sg + 112 * sb + 128) >> 8) + 128);
      uv[1] = uint8_t(((112 * sr - 94 * sg - 18 * sb + 128) >> 8) + 128);
    }
  }
}

void Nv12ToBgra(const uint8_t* y_plane, const uint8_t* uv_plane, uint32_t stride, uint32_t width,
                uint32_t height, uint8_t* bgra) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int c = y_plane[size_t(y) * stride + x] - 16;
      const uint8_t* uv = uv_plane + size_t(y / 2) * stride + (x & ~1u);
      int d = uv[0] - 128, e = uv[1] - 128;
      uint8_t* p = bgra + (size_t(y) * width + x) * 4;
      p[0] = Clip((298 * c + 516 * d + 128) >> 8);
      p[1] = Clip((298 * c - 100 * d - 208 * e + 128) >> 8);
      p[2] = Clip((298 * c + 409 * e + 128) >> 8);
      p[3] = 255;
    }
  }
}

}  // namespace netplay
