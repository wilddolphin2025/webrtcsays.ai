#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "modules/audio_coding/codecs/liquidcall/liquid_factory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "api/environment/environment.h"
#include "api/make_ref_counted.h"
#include "modules/audio_coding/codecs/liquidcall/audio_decoder_liquid.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_offer.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace {

// LIQUIDCALL, or the pre-rename LIQUIDCODEC from an older peer.
bool IsLiquidPayloadName(absl::string_view name) {
  return absl::EqualsIgnoreCase(name, liquid::kPayloadName) ||
         absl::EqualsIgnoreCase(name, liquid::kLegacyPayloadName);
}

SdpAudioFormat MakeLiquidFormat() {
  SdpAudioFormat format(liquid::kPayloadName, liquid::kWebRtcClockHz, 1);
  const liquid::RuntimeConfig& runtime = liquid::Runtime();
  const std::string pack = runtime.pack == liquid::WirePack::kTight ? "tight" : "u16";
  format.parameters =
      CodecParameterMap(liquid::LiquidFmtp(pack, std::max(1, runtime.frames_per_packet), runtime.dtx));
  return format;
}

AudioCodecSpec MakeLiquidSpec() {
  AudioCodecInfo info(liquid::kWebRtcClockHz, 1, static_cast<int>(liquid::PayloadBitrateBps()));
  info.allow_comfort_noise = false;
  return {MakeLiquidFormat(), info};
}

// Send side: the decoder detects pack and frame count from the payload
// length, so each sender may pick freely. Use the lower-bitrate choice of the
// two peers: tight if either asks, the larger frames-per-packet, DTX if either
// asks. The app's setting then lowers the Mac's send rate too.
liquid::RuntimeConfig EncoderConfigFromFormat(const SdpAudioFormat& format) {
  liquid::RuntimeConfig config = liquid::Runtime();
  const auto pack = format.parameters.find("pack");
  const auto fpp = format.parameters.find("fpp");
  const auto dtx = format.parameters.find("usedtx");
  if (pack != format.parameters.end() && pack->second == "tight") {
    config.pack = liquid::WirePack::kTight;
  }
  // Frames per packet is the sender's own latency choice (each extra frame adds
  // 80 ms). The peer's fpp is informational; the decoder reads any 1-4 frames
  // from the payload length, so we never let the remote raise our ptime.
  if (fpp != format.parameters.end()) {
    const int n = std::atoi(fpp->second.c_str());
    if (n >= 1 && n <= 4 && n != config.frames_per_packet) {
      fprintf(stderr, "LiquidCall: peer prefers %d frames/packet; sending %d (local setting)\n", n,
              config.frames_per_packet);
    }
  }
  if (dtx != format.parameters.end() && dtx->second == "1") config.dtx = true;
  fprintf(stderr, "LiquidCall send: pack=%s frames/packet=%d (ptime %d ms) dtx=%s\n",
          config.pack == liquid::WirePack::kTight ? "tight" : "u16",
          config.frames_per_packet, config.frames_per_packet * liquid::kFrameMs,
          config.dtx ? "on" : "off");
  return config;
}

liquid::RuntimeConfig ConfigFromFormat(const SdpAudioFormat& format) {
  liquid::RuntimeConfig config = liquid::Runtime();
  std::string mismatch;
  const auto pack = format.parameters.find("pack");
  const auto fpp = format.parameters.find("fpp");
  liquid::ApplyNegotiatedPack(pack == format.parameters.end() ? "" : pack->second,
                              fpp == format.parameters.end() ? "" : fpp->second,
                              &config.pack, &config.frames_per_packet, &mismatch);
  if (!mismatch.empty()) {
    fprintf(stderr, "LiquidCall negotiated fmtp: %s\n", mismatch.c_str());
    RTC_LOG(LS_ERROR) << "LiquidCall negotiated fmtp: " << mismatch;
  }
  return config;
}

class LiquidEncoderFactory : public AudioEncoderFactory {
 public:
  explicit LiquidEncoderFactory(scoped_refptr<AudioEncoderFactory> inner)
      : inner_(std::move(inner)) {}

  std::vector<AudioCodecSpec> GetSupportedEncoders() override {
    std::vector<AudioCodecSpec> inner;
    if (inner_) inner = inner_->GetSupportedEncoders();
    // LIQUIDCALL, then the builtin list with Opus kept.
    return liquid::PrependLiquidCall(std::move(inner), MakeLiquidSpec(),
                                     [](const AudioCodecSpec& spec) { return spec.format.name; });
  }

  std::optional<AudioCodecInfo> QueryAudioEncoder(
      const SdpAudioFormat& format) override {
    if (IsLiquidPayloadName(format.name)) {
      if (format.clockrate_hz != liquid::kWebRtcClockHz || format.num_channels != 1) {
        return std::nullopt;
      }
      AudioCodecInfo info(liquid::kWebRtcClockHz, 1,
                          static_cast<int>(liquid::PayloadBitrateBps()));
      info.allow_comfort_noise = false;
      return info;
    }
    if (inner_) return inner_->QueryAudioEncoder(format);
    return std::nullopt;
  }

  std::unique_ptr<AudioEncoder> Create(const Environment& env,
                                      const SdpAudioFormat& format,
                                      Options options) override {
    if (IsLiquidPayloadName(format.name)) {
      if (!QueryAudioEncoder(format)) return nullptr;
      return std::make_unique<AudioEncoderLiquid>(options.payload_type,
                                                 EncoderConfigFromFormat(format));
    }
    if (inner_) return inner_->Create(env, format, options);
    return nullptr;
  }

 private:
  scoped_refptr<AudioEncoderFactory> inner_;
};

class LiquidDecoderFactory : public AudioDecoderFactory {
 public:
  explicit LiquidDecoderFactory(scoped_refptr<AudioDecoderFactory> inner)
      : inner_(std::move(inner)) {}

  std::vector<AudioCodecSpec> GetSupportedDecoders() override {
    std::vector<AudioCodecSpec> inner;
    if (inner_) inner = inner_->GetSupportedDecoders();
    return liquid::PrependLiquidCall(std::move(inner), MakeLiquidSpec(),
                                     [](const AudioCodecSpec& spec) { return spec.format.name; });
  }

  bool IsSupportedDecoder(const SdpAudioFormat& format) override {
    if (IsLiquidPayloadName(format.name)) {
      return format.clockrate_hz == liquid::kWebRtcClockHz && format.num_channels == 1;
    }
    return inner_ && inner_->IsSupportedDecoder(format);
  }

  std::unique_ptr<AudioDecoder> Create(
      const Environment& env, const SdpAudioFormat& format,
      std::optional<AudioCodecPairId> codec_pair_id) override {
    if (IsLiquidPayloadName(format.name)) {
      if (!IsSupportedDecoder(format)) return nullptr;
      return std::make_unique<AudioDecoderLiquid>(ConfigFromFormat(format));
    }
    if (inner_) return inner_->Create(env, format, codec_pair_id);
    return nullptr;
  }

 private:
  scoped_refptr<AudioDecoderFactory> inner_;
};

}  // namespace

bool ConfigureLiquidRuntime(const liquid::RuntimeConfig& config, std::string* error) {
  if (!config.chunk.Ok()) {
    if (error) *error = "LiquidCall chunk config is invalid";
    return false;
  }
  const std::string enc =
      liquid::JoinPath(config.paths.model_dir, config.paths.encoder_model);
  const std::string dec =
      liquid::JoinPath(config.paths.model_dir, config.paths.decoder_model);
  if (!liquid::FileExists(enc)) {
    if (error) {
      *error = "LiquidCall encoder model not found at " + enc +
               ". Fetch it with scripts/liquidcall/fetch_models.sh "
               "(default repo wilddolphin2025/liquidcodec-ane). "
               "This branch does not convert the encoder.";
    }
    return false;
  }
  if (!liquid::FileExists(dec)) {
    if (error) {
      *error = "LiquidCall decoder model not found at " + dec +
               ". Fetch models with scripts/liquidcall/fetch_models.sh.";
    }
    return false;
  }
  if (config.paths.decoder_kind == "hidden") {
    const std::string tail = liquid::JoinPath(config.paths.model_dir, config.paths.tail);
    if (!liquid::FileExists(tail)) {
      if (error) *error = "LiquidCall CPU tail not found at " + tail;
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
  return make_ref_counted<LiquidEncoderFactory>(nullptr);
}

scoped_refptr<AudioDecoderFactory> CreateLiquidAudioDecoderFactory() {
  return make_ref_counted<LiquidDecoderFactory>(nullptr);
}

scoped_refptr<AudioEncoderFactory> CreateLiquidAudioEncoderFactory(
    scoped_refptr<AudioEncoderFactory> builtin) {
  return make_ref_counted<LiquidEncoderFactory>(std::move(builtin));
}

scoped_refptr<AudioDecoderFactory> CreateLiquidAudioDecoderFactory(
    scoped_refptr<AudioDecoderFactory> builtin) {
  return make_ref_counted<LiquidDecoderFactory>(std::move(builtin));
}

}  // namespace webrtc
