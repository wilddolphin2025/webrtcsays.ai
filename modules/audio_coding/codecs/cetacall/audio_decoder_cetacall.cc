#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "modules/audio_coding/codecs/cetacall/audio_decoder_cetacall.h"

#include <array>
#include <cstring>
#include <memory>

#include "rtc_base/logging.h"

namespace webrtc {
namespace {

class LiquidOneFrame final : public AudioDecoder::EncodedAudioFrame {
 public:
  LiquidOneFrame(AudioDecoderCetaCall* owner, std::array<int32_t, cetacall::kCodebooks> codes)
      : owner_(owner), codes_(codes) {}
  size_t Duration() const override {
    return static_cast<size_t>(cetacall::kWebRtcClockHz * cetacall::kFrameMs / 1000);
  }
  std::optional<DecodeResult> Decode(std::span<int16_t> decoded) const override {
    auto speech = AudioDecoder::kSpeech;
    const int n = owner_->DecodeOne(codes_.data(), decoded.data(), &speech);
    if (n < 0) return std::nullopt;
    return DecodeResult{static_cast<size_t>(n), speech};
  }

 private:
  AudioDecoderCetaCall* owner_;
  std::array<int32_t, cetacall::kCodebooks> codes_;
};

}  // namespace

AudioDecoderCetaCall::AudioDecoderCetaCall(cetacall::RuntimeConfig config)
    : config_(std::move(config)) {}

AudioDecoderCetaCall::~AudioDecoderCetaCall() {
  if (!config_.record_dir.empty()) {
    stats_.WriteJson(cetacall::JoinPath(config_.record_dir, "decoder_stats.json"));
  }
}

void AudioDecoderCetaCall::Reset() {
  if (stream_) stream_->Reset();
  fifo24_.clear();
}

int AudioDecoderCetaCall::PacketDuration(const uint8_t* encoded,
                                      size_t encoded_len) const {
  // Each parsed frame is 80 ms. Report one frame so a caller that sizes a
  // single decode buffer stays inside NetEq's 120 ms limit. ParsePayload
  // splits N-frame packets into N such frames.
  (void)encoded;
  (void)encoded_len;
  return cetacall::kWebRtcClockHz * cetacall::kFrameMs / 1000;
}

std::vector<AudioDecoder::ParseResult> AudioDecoderCetaCall::ParsePayload(
    Buffer&& payload, uint32_t timestamp) {
  std::vector<ParseResult> results;
  int32_t codes[4 * cetacall::kCodebooks];
  cetacall::WirePack pack = cetacall::WirePack::kU16;
  int frames = 0;
  uint16_t seq = 0;
  if (!cetacall::UnpackPayload(payload.data(), payload.size(), config_.chunk.codebook_size, &pack,
                             &frames, &seq, codes)) {
    return results;
  }
  const uint32_t step = static_cast<uint32_t>(cetacall::kWebRtcClockHz * cetacall::kFrameMs / 1000);
  for (int i = 0; i < frames; ++i) {
    std::array<int32_t, cetacall::kCodebooks> one{};
    for (int c = 0; c < cetacall::kCodebooks; ++c) {
      one[static_cast<size_t>(c)] = codes[i * cetacall::kCodebooks + c];
    }
    results.emplace_back(timestamp + static_cast<uint32_t>(i) * step, 0,
                         std::make_unique<LiquidOneFrame>(this, one));
  }
  return results;
}

bool AudioDecoderCetaCall::EnsureStarted() {
  if (backend_) return !failed_;
  backend_ = cetacall::CreateCetaCallBackend();
  std::string error;
  if (!backend_ || !backend_->Start(config_.paths, config_.chunk, &error)) {
    failed_ = true;
    RTC_LOG(LS_ERROR) << "CetaCall decoder: " << error;
    fprintf(stderr, "CetaCall decoder: %s\n", error.c_str());
    return false;
  }
  stream_ = std::make_unique<cetacall::StreamDecoder>(backend_.get(), config_.chunk);
  stats_.backend = backend_->name();
  stats_.compute = config_.paths.compute;
  stats_.chunk = config_.chunk;
  stats_.hidden_tail = config_.hidden_tail;
  if (!config_.record_dir.empty()) {
    std::string wav_error;
    codec_wav_.Open(cetacall::JoinPath(config_.record_dir, "codec_out_24k.wav"),
                    cetacall::kSampleRateHz, &wav_error);
  }
  return true;
}

int AudioDecoderCetaCall::DecodeOne(const int32_t* codes12, int16_t* decoded48,
                                   SpeechType* speech_type) {
  *speech_type = kSpeech;
  const int out48 = cetacall::kWebRtcClockHz * cetacall::kFrameMs / 1000;
  if (!EnsureStarted() || !codes12) {
    std::memset(decoded48, 0, static_cast<size_t>(out48) * sizeof(int16_t));
    return out48;
  }
  std::vector<int16_t> produced;
  cetacall::WindowTiming timing;
  std::string error;
  if (!stream_->PushFrame(codes12, &produced, &timing, &error)) {
    RTC_LOG(LS_ERROR) << "CetaCall decode window: " << error;
    fprintf(stderr, "CetaCall decode window: %s\n", error.c_str());
  }
  if (timing.window_ms > 0) stats_.AddWindow(timing.window_ms, timing.prediction_ms);
  if (!produced.empty()) {
    codec_wav_.Write(produced.data(), produced.size());
    stats_.samples_24k += static_cast<int64_t>(produced.size());
    fifo24_.insert(fifo24_.end(), produced.begin(), produced.end());
  }
  stats_.AddPacket(cetacall::kPayloadBytes);

  const int need24 = cetacall::kFrameSamples;
  int16_t frame24[cetacall::kFrameSamples];
  if (static_cast<int>(fifo24_.size()) >= need24) {
    std::memcpy(frame24, fifo24_.data(), sizeof(frame24));
    fifo24_.erase(fifo24_.begin(), fifo24_.begin() + need24);
  } else {
    std::memset(frame24, 0, sizeof(frame24));
  }
  for (int i = 0; i < 8; ++i) {
    up_.Resample(std::span<const int16_t>(frame24 + i * (cetacall::kSampleRateHz / 100),
                                          cetacall::kSampleRateHz / 100),
                 std::span<int16_t>(decoded48 + i * (cetacall::kWebRtcClockHz / 100),
                                    cetacall::kWebRtcClockHz / 100));
  }
  if (!config_.record_dir.empty() && stats_.packets % 50 == 0) {
    stats_.WriteJson(cetacall::JoinPath(config_.record_dir, "decoder_stats.json"));
  }
  return out48;
}

int AudioDecoderCetaCall::DecodeInternal(const uint8_t* encoded, size_t encoded_len,
                                      int /*sample_rate_hz*/, int16_t* decoded,
                                      SpeechType* speech_type) {
  int32_t codes[4 * cetacall::kCodebooks];
  cetacall::WirePack pack = cetacall::WirePack::kU16;
  int frames = 0;
  uint16_t seq = 0;
  if (!cetacall::UnpackPayload(encoded, encoded_len, config_.chunk.codebook_size, &pack, &frames,
                             &seq, codes)) {
    *speech_type = kSpeech;
    const int out48 = cetacall::kWebRtcClockHz * cetacall::kFrameMs / 1000;
    std::memset(decoded, 0, static_cast<size_t>(out48) * sizeof(int16_t));
    return out48;
  }
  return DecodeOne(codes, decoded, speech_type);
}

}  // namespace webrtc
