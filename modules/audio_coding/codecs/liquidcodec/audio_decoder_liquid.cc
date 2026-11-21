#include "modules/audio_coding/codecs/liquidcodec/audio_decoder_liquid.h"

#include <cstring>

#include "rtc_base/logging.h"

namespace webrtc {

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
  have_seq_ = false;
  expected_seq_ = 0;
  std::memset(last_codes_, 0, sizeof(last_codes_));
}

int AudioDecoderLiquid::PacketDuration(const uint8_t* encoded,
                                      size_t encoded_len) const {
  int32_t codes[liquid::kCodebooks];
  uint16_t seq = 0;
  if (!liquid::UnpackFrame(encoded, encoded_len, config_.chunk.codebook_size, codes,
                           &seq)) {
    return liquid::kWebRtcClockHz * liquid::kFrameMs / 1000;
  }
  return liquid::kWebRtcClockHz * liquid::kFrameMs / 1000;
}

bool AudioDecoderLiquid::EnsureStarted() {
  if (backend_) return !failed_;
  backend_ = liquid::CreateCoreMlBackend();
  std::string error;
  if (!backend_ || !backend_->Start(config_.paths, config_.chunk, &error)) {
    failed_ = true;
    RTC_LOG(LS_ERROR) << "LiquidCodec decoder: " << error;
    fprintf(stderr, "LiquidCodec decoder: %s\n", error.c_str());
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

int AudioDecoderLiquid::DecodeInternal(const uint8_t* encoded, size_t encoded_len,
                                      int /*sample_rate_hz*/, int16_t* decoded,
                                      SpeechType* speech_type) {
  *speech_type = kSpeech;
  const int out48 = liquid::kWebRtcClockHz * liquid::kFrameMs / 1000;
  int32_t codes[liquid::kCodebooks];
  uint16_t seq = 0;
  if (!EnsureStarted() ||
      !liquid::UnpackFrame(encoded, encoded_len, config_.chunk.codebook_size, codes,
                           &seq)) {
    std::memset(decoded, 0, static_cast<size_t>(out48) * sizeof(int16_t));
    return out48;
  }
  std::vector<int16_t> produced;
  liquid::WindowTiming timing;
  std::string error;
  if (!have_seq_) {
    expected_seq_ = seq;
    have_seq_ = true;
  }
  int missing = 0;
  while (expected_seq_ != seq && missing < 4) {
    stream_->PushFrame(last_codes_, &produced, &timing, &error);
    expected_seq_ = static_cast<uint16_t>(expected_seq_ + 1);
    ++missing;
  }
  if (expected_seq_ != seq) expected_seq_ = seq;
  if (!stream_->PushFrame(codes, &produced, &timing, &error)) {
    RTC_LOG(LS_ERROR) << "LiquidCodec decode window: " << error;
    fprintf(stderr, "LiquidCodec decode window: %s\n", error.c_str());
  }
  std::memcpy(last_codes_, codes, sizeof(last_codes_));
  expected_seq_ = static_cast<uint16_t>(seq + 1);
  if (timing.window_ms > 0) stats_.AddWindow(timing.window_ms, timing.prediction_ms);
  if (!produced.empty()) {
    codec_wav_.Write(produced.data(), produced.size());
    stats_.samples_24k += static_cast<int64_t>(produced.size());
    fifo24_.insert(fifo24_.end(), produced.begin(), produced.end());
  }
  stats_.AddPacket(static_cast<int>(encoded_len));

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
                 std::span<int16_t>(decoded + i * (liquid::kWebRtcClockHz / 100),
                                    liquid::kWebRtcClockHz / 100));
  }
  if (!config_.record_dir.empty() && stats_.packets % 50 == 0) {
    stats_.WriteJson(liquid::JoinPath(config_.record_dir, "decoder_stats.json"));
  }
  return out48;
}

}  // namespace webrtc
