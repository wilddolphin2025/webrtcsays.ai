#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "modules/audio_coding/codecs/liquidcodec/liquid_factory.h"

#include <memory>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "api/environment/environment.h"
#include "api/make_ref_counted.h"
#include "modules/audio_coding/codecs/liquidcodec/audio_decoder_liquid.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {

class LiquidEncoderFactory : public AudioEncoderFactory {
 public:
  std::vector<AudioCodecSpec> GetSupportedEncoders() override {
    SdpAudioFormat format(liquid::kPayloadName, liquid::kWebRtcClockHz, 1);
    AudioCodecInfo info(liquid::kWebRtcClockHz, 1,
                        static_cast<int>(liquid::PayloadBitrateBps()));
    info.allow_comfort_noise = false;
    return {{format, info}};
  }

  std::optional<AudioCodecInfo> QueryAudioEncoder(
      const SdpAudioFormat& format) override {
    if (!absl::EqualsIgnoreCase(format.name, liquid::kPayloadName)) {
      return std::nullopt;
    }
    if (format.clockrate_hz != liquid::kWebRtcClockHz || format.num_channels != 1) {
      return std::nullopt;
    }
    AudioCodecInfo info(liquid::kWebRtcClockHz, 1,
                        static_cast<int>(liquid::PayloadBitrateBps()));
    info.allow_comfort_noise = false;
    return info;
  }

  std::unique_ptr<AudioEncoder> Create(const Environment& /*env*/,
                                      const SdpAudioFormat& format,
                                      Options options) override {
    if (!QueryAudioEncoder(format)) return nullptr;
    return std::make_unique<AudioEncoderLiquid>(options.payload_type, liquid::Runtime());
  }
};

class LiquidDecoderFactory : public AudioDecoderFactory {
 public:
  std::vector<AudioCodecSpec> GetSupportedDecoders() override {
    SdpAudioFormat format(liquid::kPayloadName, liquid::kWebRtcClockHz, 1);
    AudioCodecInfo info(liquid::kWebRtcClockHz, 1,
                        static_cast<int>(liquid::PayloadBitrateBps()));
    info.allow_comfort_noise = false;
    return {{format, info}};
  }

  bool IsSupportedDecoder(const SdpAudioFormat& format) override {
    return absl::EqualsIgnoreCase(format.name, liquid::kPayloadName) &&
           format.clockrate_hz == liquid::kWebRtcClockHz && format.num_channels == 1;
  }

  std::unique_ptr<AudioDecoder> Create(
      const Environment& /*env*/, const SdpAudioFormat& format,
      std::optional<AudioCodecPairId> /*codec_pair_id*/) override {
    if (!IsSupportedDecoder(format)) return nullptr;
    return std::make_unique<AudioDecoderLiquid>(liquid::Runtime());
  }
};

}  // namespace

bool ConfigureLiquidRuntime(const liquid::RuntimeConfig& config, std::string* error) {
  if (!config.chunk.Ok()) {
    if (error) *error = "LiquidCodec chunk config is invalid";
    return false;
  }
  const std::string enc =
      liquid::JoinPath(config.paths.model_dir, config.paths.encoder_model);
  const std::string dec =
      liquid::JoinPath(config.paths.model_dir, config.paths.decoder_model);
  if (!liquid::FileExists(enc)) {
    if (error) {
      *error = "LiquidCodec encoder model not found at " + enc +
               ". Fetch it with scripts/liquidcodec/fetch_models.sh "
               "(default repo wilddolphin2025/liquidcodec-ane). "
               "This branch does not convert the encoder.";
    }
    return false;
  }
  if (!liquid::FileExists(dec)) {
    if (error) {
      *error = "LiquidCodec decoder model not found at " + dec +
               ". Fetch models with scripts/liquidcodec/fetch_models.sh.";
    }
    return false;
  }
  if (config.paths.decoder_kind == "hidden") {
    const std::string tail = liquid::JoinPath(config.paths.model_dir, config.paths.tail);
    if (!liquid::FileExists(tail)) {
      if (error) *error = "LiquidCodec CPU tail not found at " + tail;
      return false;
    }
  }
  *liquid::MutableRuntime() = config;
  liquid::MutableRuntime()->hidden_tail = config.paths.decoder_kind != "pcm";
  RTC_LOG(LS_INFO) << liquid::DescribeLatency(config.chunk, config.paths.decoder_kind != "pcm",
                                             config.paths)
                          .summary;
  return true;
}

scoped_refptr<AudioEncoderFactory> CreateLiquidAudioEncoderFactory() {
  return make_ref_counted<LiquidEncoderFactory>();
}

scoped_refptr<AudioDecoderFactory> CreateLiquidAudioDecoderFactory() {
  return make_ref_counted<LiquidDecoderFactory>();
}

}  // namespace webrtc
