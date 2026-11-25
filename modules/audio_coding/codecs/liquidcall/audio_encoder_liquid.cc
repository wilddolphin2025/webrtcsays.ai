#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "modules/audio_coding/codecs/liquidcall/audio_encoder_liquid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

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
  return static_cast<int>(liquid::PayloadBitrateBps(
      config_.pack, std::max(1, config_.frames_per_packet)));
}

void AudioEncoderLiquid::Reset() {
  if (stream_) stream_->Reset();
  pacer_.Reset();
  bundle_.clear();
  inject_pos_ = 0;
}

std::optional<std::pair<TimeDelta, TimeDelta>>
AudioEncoderLiquid::GetFrameLengthRange() const {
  const int ms = liquid::kFrameMs * std::max(1, config_.frames_per_packet);
  return {{TimeDelta::Millis(ms), TimeDelta::Millis(ms)}};
}

bool AudioEncoderLiquid::EnsureStarted(std::string* error) {
  if (backend_) return !failed_;
  backend_ = liquid::CreateLiquidBackend();
  if (!backend_) {
    if (error) *error = "LiquidCall backend is not linked";
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
  double plain = 0;
  double srtp = 0;
  const int fpp = std::max(1, config_.frames_per_packet);
  const int payload = liquid::PayloadBytes(config_.pack, fpp);
  const int tag = config_.srtp_tag_bytes;
  liquid::OnWireBitrateBps(payload, liquid::kFramesPerSecond / fpp, tag, &plain, &srtp);
  // The SRTP cipher is negotiated in DTLS after this runs, so this is the
  // expected figure; directcall prints the negotiated one when stats start.
  std::string srtp_text;
  if (tag < 0) {
    srtp_text = "SRTP tag not known yet";
  } else if (tag == 0) {
    srtp_text = "no SRTP";
  } else {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "expected with %d-byte SRTP tag (%s) %.0f bps", tag,
                  tag == liquid::kSrtpTagSha1_32Bytes   ? "SHA1_32"
                  : tag == liquid::kSrtpTagSha1_80Bytes ? "SHA1_80"
                                                        : "GCM",
                  srtp);
    srtp_text = buf;
  }
  fprintf(stderr,
          "LiquidCall pack=%s frames/packet=%d payload=%d bytes, information %.0f bps, "
          "payload %.0f bps, on-wire IPv4/UDP/RTP %.0f bps, %s, compute=%s\n",
          config_.pack == liquid::WirePack::kTight ? "tight" : "u16", fpp, payload,
          liquid::InformationBitrateBps(config_.chunk),
          liquid::PayloadBitrateBps(config_.pack, fpp), plain, srtp_text.c_str(),
          config_.paths.compute.c_str());
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
    fprintf(stderr, "LiquidCall inject %s (%d Hz, %zu samples)\n",
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
    RTC_LOG(LS_ERROR) << "LiquidCall encoder: " << error;
    fprintf(stderr, "LiquidCall encoder: %s\n", error.c_str());
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
  const bool voice = VoiceActive(pcm24, liquid::kSampleRateHz / 100);
  codec_wav_.Write(pcm24, liquid::kSampleRateHz / 100);
  stats_.samples_24k += liquid::kSampleRateHz / 100;

  std::vector<std::array<int32_t, liquid::kCodebooks>> frames;
  liquid::WindowTiming timing;
  if (!stream_->Push(pcm24, liquid::kSampleRateHz / 100, &frames, &timing, &error)) {
    RTC_LOG(LS_ERROR) << "LiquidCall encode window: " << error;
    fprintf(stderr, "LiquidCall encode window: %s\n", error.c_str());
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
  if (packet) bundle_.push_back(*packet);
  const int fpp = std::max(1, config_.frames_per_packet);
  if (static_cast<int>(bundle_.size()) < fpp) return info;

  std::vector<int32_t> packed(static_cast<size_t>(fpp * liquid::kCodebooks));
  for (int f = 0; f < fpp; ++f) {
    for (int c = 0; c < liquid::kCodebooks; ++c) {
      packed[static_cast<size_t>(f * liquid::kCodebooks + c)] =
          bundle_[static_cast<size_t>(f)].codes[static_cast<size_t>(c)];
    }
  }
  const uint32_t stamp = bundle_.front().timestamp;
  bundle_.clear();
  if (config_.dtx && !voice) {
    // Silence: the encoder keeps running so its state stays current, but no
    // packet goes out. The receiver's NetEq conceals the gap and fades out.
    ++dtx_skipped_packets_;
    info.speech = false;
    return info;
  }
  uint8_t bytes[128];
  size_t len = 0;
  if (!liquid::PackPayload(config_.pack, packed.data(), fpp,
                           static_cast<uint16_t>(stats_.packets & 0xffff),
                           config_.chunk.codebook_size, bytes, sizeof(bytes), &len)) {
    RTC_LOG(LS_ERROR) << "LiquidCall pack rejected a code";
    return info;
  }
  encoded->AppendData(bytes, len);
  info.encoded_bytes = len;
  info.encoded_timestamp = stamp;
  stats_.AddPacket(static_cast<int>(len));
  if (!config_.record_dir.empty() && stats_.packets % 50 == 0) {
    stats_.WriteJson(liquid::JoinPath(config_.record_dir, "encoder_stats.json"));
  }
  return info;
}

// Energy VAD. Speech when the 10 ms level is 9 dB over the tracked noise floor
// and above -55 dBFS. The hangover (400 ms) covers the 120 ms algorithmic
// lookahead plus word tails, so the packet that carries an onset is sent.
bool AudioEncoderLiquid::VoiceActive(const int16_t* pcm24, int n) {
  double sum = 0;
  for (int i = 0; i < n; ++i) sum += static_cast<double>(pcm24[i]) * pcm24[i];
  const double rms = std::sqrt(sum / std::max(1, n)) / 32768.0;
  const double db = 20.0 * std::log10(std::max(rms, 1e-6));
  // Floor follows quiet input quickly and loud input slowly.
  noise_floor_db_ += (db < noise_floor_db_ ? 0.3 : 0.002) * (db - noise_floor_db_);
  const bool loud = db > -55.0 && db > noise_floor_db_ + 9.0;
  ticks_since_voice_ = loud ? 0 : std::min(ticks_since_voice_ + 1, 1 << 20);
  return ticks_since_voice_ <= 40;
}

}  // namespace webrtc
