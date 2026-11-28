/*
 * CetaCall CETACALL token bridge for the Direct GATT path.
 */

#include "ble_token_audio.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>

#include "modules/audio_coding/codecs/cetacall/cetacall_backend.h"
#include "modules/audio_coding/codecs/cetacall/cetacall_codec.h"
#include "modules/audio_coding/codecs/cetacall/cetacall_stream.h"

namespace cetacall {
namespace {

void SetError(std::string* error, const char* text) {
  if (error) *error = text;
}

}  // namespace

struct BleTokenAudioCodec::Impl {
  std::unique_ptr<webrtc::cetacall::CetaCallBackend> backend;
  std::unique_ptr<webrtc::cetacall::StreamEncoder> encoder;
  std::unique_ptr<webrtc::cetacall::StreamDecoder> decoder;
  webrtc::cetacall::ModelPaths paths;
  webrtc::cetacall::ChunkConfig chunk;
  std::mutex encode_mutex;
  std::mutex decode_mutex;
  bool have_odd = false;
  int16_t odd = 0;
};

BleTokenAudioCodec::BleTokenAudioCodec(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

BleTokenAudioCodec::~BleTokenAudioCodec() = default;

std::unique_ptr<BleTokenAudioCodec> BleTokenAudioCodec::Create(
    const std::string& model_dir, std::string* error) {
  auto impl = std::make_unique<Impl>();
  if (!webrtc::cetacall::LoadManifestFile(
          webrtc::cetacall::ManifestPath(model_dir), &impl->paths,
          &impl->chunk, error)) {
    return nullptr;
  }
  impl->paths.model_dir = model_dir;
  impl->backend = webrtc::cetacall::CreateCoreMlBackend();
  if (!impl->backend) {
    SetError(error, "Core ML CetaCall backend is unavailable");
    return nullptr;
  }
  if (!impl->backend->Start(impl->paths, impl->chunk, error)) {
    return nullptr;
  }
  impl->encoder = std::make_unique<webrtc::cetacall::StreamEncoder>(
      impl->backend.get(), impl->chunk, impl->paths);
  impl->decoder = std::make_unique<webrtc::cetacall::StreamDecoder>(
      impl->backend.get(), impl->chunk);
  return std::unique_ptr<BleTokenAudioCodec>(
      new BleTokenAudioCodec(std::move(impl)));
}

bool BleTokenAudioCodec::Encode(
    const int16_t* pcm, size_t samples, int sample_rate_hz,
    std::vector<std::vector<uint8_t>>* payloads, std::string* error) {
  if (!pcm || samples == 0 || !payloads) return false;
  payloads->clear();
  std::vector<int16_t> in24;
  if (sample_rate_hz == 24000) {
    in24.assign(pcm, pcm + samples);
  } else if (sample_rate_hz == 48000) {
    in24.reserve(samples / 2 + 1);
    size_t i = 0;
    if (impl_->have_odd) {
      in24.push_back(static_cast<int16_t>((impl_->odd + pcm[0]) / 2));
      impl_->have_odd = false;
      i = 1;
    }
    for (; i + 1 < samples; i += 2) {
      in24.push_back(static_cast<int16_t>((pcm[i] + pcm[i + 1]) / 2));
    }
    if (i < samples) {
      impl_->odd = pcm[i];
      impl_->have_odd = true;
    }
  } else {
    SetError(error, "CetaCall BLE PCM must be 24 kHz or 48 kHz");
    return false;
  }
  if (in24.empty()) return true;

  std::lock_guard<std::mutex> lock(impl_->encode_mutex);
  std::vector<std::array<int32_t, webrtc::cetacall::kCodebooks>> frames;
  webrtc::cetacall::WindowTiming timing;
  if (!impl_->encoder->Push(in24.data(), in24.size(), &frames, &timing,
                            error)) {
    return false;
  }
  for (const auto& frame : frames) {
    std::vector<int32_t> codes(frame.begin(), frame.end());
    uint8_t bytes[128];
    size_t size = 0;
    if (!webrtc::cetacall::PackPayload(
            webrtc::cetacall::WirePack::kTight, codes.data(), 1, 0,
            impl_->chunk.codebook_size, bytes, sizeof(bytes), &size)) {
      SetError(error, "CetaCall tight token pack failed");
      return false;
    }
    payloads->emplace_back(bytes, bytes + size);
  }
  return true;
}

bool BleTokenAudioCodec::Decode(const uint8_t* payload, size_t size,
                                int output_rate_hz,
                                std::vector<int16_t>* pcm,
                                std::string* error) {
  if (!payload || size == 0 || !pcm) return false;
  pcm->clear();
  if (output_rate_hz != 24000 && output_rate_hz != 48000) {
    SetError(error, "CetaCall BLE output must be 24 kHz or 48 kHz");
    return false;
  }
  int32_t codes[4 * webrtc::cetacall::kCodebooks] = {};
  uint16_t sequence = 0;
  int frames = 0;
  webrtc::cetacall::WirePack pack = webrtc::cetacall::WirePack::kTight;
  if (!webrtc::cetacall::UnpackPayload(
          payload, size, impl_->chunk.codebook_size, &pack, &frames,
          &sequence, codes)) {
    SetError(error, "CetaCall BLE token unpack failed");
    return false;
  }
  if (frames <= 0 || frames > 4) {
    SetError(error, "CetaCall BLE token frame count is invalid");
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->decode_mutex);
  for (int f = 0; f < frames; ++f) {
    std::vector<int16_t> decoded;
    webrtc::cetacall::WindowTiming timing;
    if (!impl_->decoder->PushFrame(
            codes + f * webrtc::cetacall::kCodebooks, &decoded, &timing,
            error)) {
      return false;
    }
    if (output_rate_hz == 48000) {
      for (int16_t sample : decoded) {
        pcm->push_back(sample);
        pcm->push_back(sample);
      }
    } else {
      pcm->insert(pcm->end(), decoded.begin(), decoded.end());
    }
  }
  return true;
}

}  // namespace cetacall
