#include "modules/audio_coding/codecs/liquidcodec/audio_encoder_liquid.h"

#include <cstring>

#include "rtc_base/logging.h"

namespace webrtc {
namespace liquid {
namespace {
RuntimeConfig g_runtime;
}  // namespace

RuntimeConfig* MutableRuntime() { return &g_runtime; }
const RuntimeConfig& Runtime() { return g_runtime; }
}  // namespace liquid

AudioEncoderLiquid::AudioEncoderLiquid(int payload_type, liquid::RuntimeConfig config)
    : payload_type_(payload_type),
      config_(std::move(config)),
      pacer_(8) {}

AudioEncoderLiquid::~AudioEncoderLiquid() {
  if (!config_.record_dir.empty()) {
    stats_.WriteJson(liquid::JoinPath(config_.record_dir, "encoder_stats.json"));
  }
}

int AudioEncoderLiquid::GetTargetBitrate() const {
  return static_cast<int>(liquid::PayloadBitrateBps());
}

void AudioEncoderLiquid::Reset() {
  if (stream_) stream_->Reset();
  pacer_.Reset();
  inject_pos_ = 0;
}

std::optional<std::pair<TimeDelta, TimeDelta>>
AudioEncoderLiquid::GetFrameLengthRange() const {
  return {{TimeDelta::Millis(liquid::kFrameMs), TimeDelta::Millis(liquid::kFrameMs)}};
}

bool AudioEncoderLiquid::EnsureStarted(std::string* error) {
  if (backend_) return !failed_;
  backend_ = liquid::CreateCoreMlBackend();
  if (!backend_) {
    if (error) *error = "Core ML backend is not linked";
    failed_ = true;
    return false;
  }
  if (!backend_->Start(config_.paths, config_.chunk, error)) {
    failed_ = true;
    return false;
  }
  stream_ = std::make_unique<liquid::StreamEncoder>(backend_.get(), config_.chunk, config_.paths);
  stats_.backend = backend_->name();
  stats_.compute = config_.paths.compute;
  stats_.chunk = config_.chunk;
  stats_.hidden_tail = config_.hidden_tail;
  stats_.lookahead_samples = config_.paths.lookahead_samples;
  stats_.stateful_encoder = config_.paths.Stateful();
  const liquid::LatencyReport latency =
      liquid::DescribeLatency(config_.chunk, config_.hidden_tail, config_.paths);
  RTC_LOG(LS_INFO) << latency.summary;
  fprintf(stderr, "%s\n", latency.summary.c_str());
  fprintf(stderr,
          "LiquidCodec payload %d bytes/frame, information %.0f bps, "
          "payload %.0f bps, compute=%s\n",
          liquid::kPayloadBytes, liquid::InformationBitrateBps(config_.chunk),
          liquid::PayloadBitrateBps(), config_.paths.compute.c_str());
  if (!config_.record_dir.empty()) {
    std::string wav_error;
    codec_wav_.Open(liquid::JoinPath(config_.record_dir, "codec_in_24k.wav"),
                    liquid::kSampleRateHz, &wav_error);
  }
  if (!config_.inject_wav.empty()) {
    liquid::WavData wav;
    if (!liquid::ReadWav(config_.inject_wav, &wav, error)) {
      failed_ = true;
      return false;
    }
    if (wav.sample_rate_hz != 16000 && wav.sample_rate_hz != 24000 &&
        wav.sample_rate_hz != 48000) {
      if (error) *error = "inject wav must be 16, 24, or 48 kHz";
      failed_ = true;
      return false;
    }
    inject_ = std::move(wav.mono);
    inject_rate_ = wav.sample_rate_hz;
    fprintf(stderr, "LiquidCodec inject %s (%d Hz, %zu samples)\n",
            config_.inject_wav.c_str(), inject_rate_, inject_.size());
  }
  return true;
}

bool AudioEncoderLiquid::TakeInject(int16_t* dst24, std::string* error) {
  const int src_10ms = inject_rate_ / 100;
  if (inject_pos_ + static_cast<size_t>(src_10ms) > inject_.size()) {
    std::memset(dst24, 0, liquid::kSampleRateHz / 100 * sizeof(int16_t));
    return true;
  }
  if (inject_rate_ == liquid::kSampleRateHz) {
    std::memcpy(dst24, inject_.data() + inject_pos_,
                src_10ms * sizeof(int16_t));
    inject_pos_ += static_cast<size_t>(src_10ms);
    return true;
  }
  std::vector<int16_t> src(inject_.begin() + static_cast<std::ptrdiff_t>(inject_pos_),
                           inject_.begin() + static_cast<std::ptrdiff_t>(inject_pos_ + src_10ms));
  inject_pos_ += static_cast<size_t>(src_10ms);
  PushResampler<int16_t> resampler;
  std::vector<int16_t> dst(liquid::kSampleRateHz / 100);
  resampler.Resample(std::span<const int16_t>(src.data(), src.size()),
                     std::span<int16_t>(dst.data(), dst.size()));
  std::memcpy(dst24, dst.data(), dst.size() * sizeof(int16_t));
  (void)error;
  return true;
}

AudioEncoder::EncodedInfo AudioEncoderLiquid::EncodeImpl(
    uint32_t rtp_timestamp, std::span<const int16_t> audio, Buffer* encoded) {
  EncodedInfo info;
  info.encoded_timestamp = rtp_timestamp;
  info.payload_type = payload_type_;
  info.encoder_type = CodecType::kOther;
  std::string error;
  if (!EnsureStarted(&error)) {
    RTC_LOG(LS_ERROR) << "LiquidCodec encoder: " << error;
    fprintf(stderr, "LiquidCodec encoder: %s\n", error.c_str());
    return info;
  }
  int16_t pcm24[liquid::kSampleRateHz / 100];
  if (!config_.inject_wav.empty()) {
    if (!TakeInject(pcm24, &error)) return info;
  } else {
    int16_t dst[liquid::kSampleRateHz / 100];
    down_.Resample(std::span<const int16_t>(audio.data(), audio.size()),
                   std::span<int16_t>(dst, liquid::kSampleRateHz / 100));
    std::memcpy(pcm24, dst, sizeof(dst));
  }
  codec_wav_.Write(pcm24, liquid::kSampleRateHz / 100);
  stats_.samples_24k += liquid::kSampleRateHz / 100;

  std::vector<std::array<int32_t, liquid::kCodebooks>> frames;
  liquid::WindowTiming timing;
  if (!stream_->Push(pcm24, liquid::kSampleRateHz / 100, &frames, &timing, &error)) {
    RTC_LOG(LS_ERROR) << "LiquidCodec encode window: " << error;
    fprintf(stderr, "LiquidCodec encode window: %s\n", error.c_str());
    return info;
  }
  if (timing.window_ms > 0) stats_.AddWindow(timing.window_ms, timing.prediction_ms);

  std::vector<int32_t> flat;
  flat.reserve(frames.size() * liquid::kCodebooks);
  for (const auto& frame : frames) {
    flat.insert(flat.end(), frame.begin(), frame.end());
  }
  const auto packet = pacer_.Tick(rtp_timestamp, flat.empty() ? nullptr : flat.data(),
                                  static_cast<int>(frames.size()));
  if (!packet) return info;

  uint8_t bytes[liquid::kPayloadBytes];
  size_t len = 0;
  if (!liquid::PackFrame(packet->codes.data(),
                         static_cast<uint16_t>(stats_.packets & 0xffff),
                         config_.chunk.codebook_size, bytes, sizeof(bytes), &len)) {
    RTC_LOG(LS_ERROR) << "LiquidCodec pack rejected a code";
    return info;
  }
  encoded->AppendData(bytes, len);
  info.encoded_bytes = len;
  info.encoded_timestamp = packet->timestamp;
  stats_.AddPacket(static_cast<int>(len));
  if (!config_.record_dir.empty() && stats_.packets % 50 == 0) {
    stats_.WriteJson(liquid::JoinPath(config_.record_dir, "encoder_stats.json"));
  }
  return info;
}

}  // namespace webrtc
