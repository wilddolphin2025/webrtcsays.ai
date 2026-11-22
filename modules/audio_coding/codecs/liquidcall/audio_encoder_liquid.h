#ifndef MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_AUDIO_ENCODER_LIQUID_H_
#define MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_AUDIO_ENCODER_LIQUID_H_

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "api/audio_codecs/audio_encoder.h"
#include "api/units/time_delta.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_codec.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_stream.h"

namespace webrtc {
namespace liquid {

struct RuntimeConfig {
  ModelPaths paths;
  ChunkConfig chunk;
  std::string record_dir;
  std::string inject_wav;
  bool hidden_tail = true;
  WirePack pack = WirePack::kU16;
  int frames_per_packet = 1;
  // DTX: skip packets while the input is silent (energy VAD with an adaptive
  // noise floor and a hangover that covers the codec lookahead).
  bool dtx = false;
  // SRTP auth tag the transport is set up to offer: 4 (SHA1_32, used when
  // both peers allow it), 10 (SHA1_80), 0 (no encryption), -1 unknown. Only
  // the startup estimate uses it; the stats print the negotiated cipher.
  int srtp_tag_bytes = -1;
};

// Process-wide settings copied from directcall before the peer connection
// factory is created. The macOS build reads them when the codec is negotiated.
RuntimeConfig* MutableRuntime();
const RuntimeConfig& Runtime();

}  // namespace liquid

class AudioEncoderLiquid final : public AudioEncoder {
 public:
  AudioEncoderLiquid(int payload_type, liquid::RuntimeConfig config);
  ~AudioEncoderLiquid() override;

  int SampleRateHz() const override { return liquid::kWebRtcClockHz; }
  size_t NumChannels() const override { return 1; }
  size_t Num10MsFramesInNextPacket() const override {
    return static_cast<size_t>(8 * std::max(1, config_.frames_per_packet));
  }
  size_t Max10MsFramesInAPacket() const override {
    return static_cast<size_t>(8 * std::max(1, config_.frames_per_packet));
  }
  int GetTargetBitrate() const override;
  void Reset() override;
  std::optional<std::pair<TimeDelta, TimeDelta>> GetFrameLengthRange()
      const override;

 protected:
  EncodedInfo EncodeImpl(uint32_t rtp_timestamp,
                         std::span<const int16_t> audio,
                         Buffer* encoded) override;

 private:
  bool EnsureStarted(std::string* error);
  bool TakeInject(int16_t* dst24, std::string* error);

  int payload_type_;
  liquid::RuntimeConfig config_;
  std::unique_ptr<liquid::LiquidBackend> backend_;
  std::unique_ptr<liquid::StreamEncoder> stream_;
  liquid::FramePacer pacer_;
  std::vector<liquid::PacedPacket> bundle_;
  liquid::CodecStats stats_;
  liquid::WavWriter codec_wav_;
  PushResampler<int16_t> down_;
  std::vector<int16_t> inject_;
  size_t inject_pos_ = 0;
  int inject_rate_ = 0;
  bool failed_ = false;
  // DTX state, in 10 ms ticks.
  bool VoiceActive(const int16_t* pcm24, int n);
  double noise_floor_db_ = -60.0;
  int ticks_since_voice_ = 1 << 20;
  long long dtx_skipped_packets_ = 0;
};

}  // namespace webrtc

#endif
