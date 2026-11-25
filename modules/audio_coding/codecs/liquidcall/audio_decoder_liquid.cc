#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "modules/audio_coding/codecs/liquidcall/audio_decoder_liquid.h"

#include <array>
#include <cstring>
#include <memory>

#include "rtc_base/logging.h"

namespace webrtc {
namespace {

class LiquidOneFrame final : public AudioDecoder::EncodedAudioFrame {
 public:
  LiquidOneFrame(AudioDecoderLiquid* owner, std::array<int32_t, liquid::kCodebooks> codes)
      : owner_(owner), codes_(codes) {}
  size_t Duration() const override {
    return static_cast<size_t>(liquid::kWebRtcClockHz * liquid::kFrameMs / 1000);
  }
  std::optional<DecodeResult> Decode(std::span<int16_t> decoded) const override {
    auto speech = AudioDecoder::kSpeech;
    const int n = owner_->DecodeOne(codes_.data(), decoded.data(), &speech);
    if (n < 0) return std::nullopt;
    return DecodeResult{static_cast<size_t>(n), speech};
  }

 private:
  AudioDecoderLiquid* owner_;
  std::array<int32_t, liquid::kCodebooks> codes_;
};

}  // namespace

AudioDecoderLiquid::AudioDecoderLiquid(liquid::RuntimeConfig config)
    : config_(std::move(config)) {}

AudioDecoderLiquid::~AudioDecoderLiquid() {
  if (!config_.record_dir.empty()) {
    stats_.WriteJson(liquid::JoinPath(config_.record_dir, "decoder_stats.json"));
  }
}

void AudioDecoderLiquid::Reset() {
  if (stream_) stream_->Reset();
  fifo24_.clear();
}

int AudioDecoderLiquid::PacketDuration(const uint8_t* encoded,
                                      size_t encoded_len) const {
  // Each parsed frame is 80 ms. Report one frame so a caller that sizes a
  // single decode buffer stays inside NetEq's 120 ms limit. ParsePayload
  // splits N-frame packets into N such frames.
  (void)encoded;
  (void)encoded_len;
  return liquid::kWebRtcClockHz * liquid::kFrameMs / 1000;
}

std::vector<AudioDecoder::ParseResult> AudioDecoderLiquid::ParsePayload(
    Buffer&& payload, uint32_t timestamp) {
  std::vector<ParseResult> results;
  int32_t codes[4 * liquid::kCodebooks];
  liquid::WirePack pack = liquid::WirePack::kU16;
  int frames = 0;
  uint16_t seq = 0;
  if (!liquid::UnpackPayload(payload.data(), payload.size(), config_.chunk.codebook_size, &pack,
                             &frames, &seq, codes)) {
    return results;
  }
  const uint32_t step = static_cast<uint32_t>(liquid::kWebRtcClockHz * liquid::kFrameMs / 1000);
  for (int i = 0; i < frames; ++i) {
    std::array<int32_t, liquid::kCodebooks> one{};
    for (int c = 0; c < liquid::kCodebooks; ++c) {
      one[static_cast<size_t>(c)] = codes[i * liquid::kCodebooks + c];
    }
    results.emplace_back(timestamp + static_cast<uint32_t>(i) * step, 0,
                         std::make_unique<LiquidOneFrame>(this, one));
  }
  return results;
}

bool AudioDecoderLiquid::EnsureStarted() {
  if (backend_) return !failed_;
  backend_ = liquid::CreateLiquidBackend();
  std::string error;
  if (!backend_ || !backend_->Start(config_.paths, config_.chunk, &error)) {
    failed_ = true;
    RTC_LOG(LS_ERROR) << "LiquidCall decoder: " << error;
    fprintf(stderr, "LiquidCall decoder: %s\n", error.c_str());
    return false;
  }
  stream_ = std::make_unique<liquid::StreamDecoder>(backend_.get(), config_.chunk);
  stats_.backend = backend_->name();
  stats_.compute = config_.paths.compute;
  stats_.chunk = config_.chunk;
  stats_.hidden_tail = config_.hidden_tail;
  if (!config_.record_dir.empty()) {
    std::string wav_error;
    codec_wav_.Open(liquid::JoinPath(config_.record_dir, "codec_out_24k.wav"),
                    liquid::kSampleRateHz, &wav_error);
  }
  return true;
}

int AudioDecoderLiquid::DecodeOne(const int32_t* codes12, int16_t* decoded48,
                                   SpeechType* speech_type) {
  *speech_type = kSpeech;
  const int out48 = liquid::kWebRtcClockHz * liquid::kFrameMs / 1000;
  if (!EnsureStarted() || !codes12) {
    std::memset(decoded48, 0, static_cast<size_t>(out48) * sizeof(int16_t));
    return out48;
  }
  std::vector<int16_t> produced;
  liquid::WindowTiming timing;
  std::string error;
  if (!stream_->PushFrame(codes12, &produced, &timing, &error)) {
    RTC_LOG(LS_ERROR) << "LiquidCall decode window: " << error;
    fprintf(stderr, "LiquidCall decode window: %s\n", error.c_str());
  }
  if (timing.window_ms > 0) stats_.AddWindow(timing.window_ms, timing.prediction_ms);
  if (!produced.empty()) {
    codec_wav_.Write(produced.data(), produced.size());
    stats_.samples_24k += static_cast<int64_t>(produced.size());
    fifo24_.insert(fifo24_.end(), produced.begin(), produced.end());
  }
  stats_.AddPacket(liquid::kPayloadBytes);

  const int need24 = liquid::kFrameSamples;
  int16_t frame24[liquid::kFrameSamples];
  if (static_cast<int>(fifo24_.size()) >= need24) {
    std::memcpy(frame24, fifo24_.data(), sizeof(frame24));
    fifo24_.erase(fifo24_.begin(), fifo24_.begin() + need24);
  } else {
    std::memset(frame24, 0, sizeof(frame24));
  }
  for (int i = 0; i < 8; ++i) {
    up_.Resample(std::span<const int16_t>(frame24 + i * (liquid::kSampleRateHz / 100),
                                          liquid::kSampleRateHz / 100),
                 std::span<int16_t>(decoded48 + i * (liquid::kWebRtcClockHz / 100),
                                    liquid::kWebRtcClockHz / 100));
  }
  if (!config_.record_dir.empty() && stats_.packets % 50 == 0) {
    stats_.WriteJson(liquid::JoinPath(config_.record_dir, "decoder_stats.json"));
  }
  return out48;
}

int AudioDecoderLiquid::DecodeInternal(const uint8_t* encoded, size_t encoded_len,
                                      int /*sample_rate_hz*/, int16_t* decoded,
                                      SpeechType* speech_type) {
  int32_t codes[4 * liquid::kCodebooks];
  liquid::WirePack pack = liquid::WirePack::kU16;
  int frames = 0;
  uint16_t seq = 0;
  if (!liquid::UnpackPayload(encoded, encoded_len, config_.chunk.codebook_size, &pack, &frames,
                             &seq, codes)) {
    *speech_type = kSpeech;
    const int out48 = liquid::kWebRtcClockHz * liquid::kFrameMs / 1000;
    std::memset(decoded, 0, static_cast<size_t>(out48) * sizeof(int16_t));
    return out48;
  }
  return DecodeOne(codes, decoded, speech_type);
}

}  // namespace webrtc
