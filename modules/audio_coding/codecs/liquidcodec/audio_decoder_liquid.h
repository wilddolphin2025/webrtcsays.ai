#ifndef MODULES_AUDIO_CODING_CODECS_LIQUIDCODEC_AUDIO_DECODER_LIQUID_H_
#define MODULES_AUDIO_CODING_CODECS_LIQUIDCODEC_AUDIO_DECODER_LIQUID_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "api/audio_codecs/audio_decoder.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "modules/audio_coding/codecs/liquidcodec/audio_encoder_liquid.h"

namespace webrtc {

class AudioDecoderLiquid final : public AudioDecoder {
 public:
  explicit AudioDecoderLiquid(liquid::RuntimeConfig config);
  ~AudioDecoderLiquid() override;

  int SampleRateHz() const override { return liquid::kWebRtcClockHz; }
  size_t Channels() const override { return 1; }
  void Reset() override;
  int PacketDuration(const uint8_t* encoded, size_t encoded_len) const override;

 protected:
  int DecodeInternal(const uint8_t* encoded, size_t encoded_len, int sample_rate_hz,
                     int16_t* decoded, SpeechType* speech_type) override;

 private:
  bool EnsureStarted();

  liquid::RuntimeConfig config_;
  std::unique_ptr<liquid::LiquidBackend> backend_;
  std::unique_ptr<liquid::StreamDecoder> stream_;
  liquid::CodecStats stats_;
  liquid::WavWriter codec_wav_;
  std::vector<int16_t> fifo24_;
  int32_t last_codes_[liquid::kCodebooks] = {};
  uint16_t expected_seq_ = 0;
  bool have_seq_ = false;
  PushResampler<int16_t> up_;
  bool failed_ = false;
};

}  // namespace webrtc

#endif
