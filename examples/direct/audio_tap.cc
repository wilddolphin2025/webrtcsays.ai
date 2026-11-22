#include "audio_tap.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "api/audio_codecs/audio_decoder.h"
#include "api/audio_codecs/audio_encoder.h"
#include "api/environment/environment.h"
#include "api/make_ref_counted.h"
#include "common_audio/resampler/include/push_resampler.h"

namespace webrtc {
namespace {

std::mutex g_voice_mu;
std::string g_voice;              // guarded by g_voice_mu
std::atomic<int> g_voice_gen{0};

void EnsureDir(const std::string& dir) {
  if (dir.empty()) return;
  std::string cur;
  for (size_t i = 0; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
      if (!cur.empty()) mkdir(cur.c_str(), 0755);
      if (i == dir.size()) break;
    }
    cur.push_back(dir[i]);
  }
}

class PcmWav {
 public:
  ~PcmWav() { Close(); }
  bool Open(const std::string& path, int rate) {
    Close();
    file_ = std::fopen(path.c_str(), "wb+");
    if (!file_) return false;
    rate_ = rate;
    WriteHeader(0);
    return true;
  }
  void Write(const int16_t* p, size_t n) {
    if (!file_ || !p || n == 0) return;
    std::fwrite(p, sizeof(int16_t), n, file_);
    frames_ += static_cast<uint32_t>(n);
    std::fflush(file_);
  }
  void Close() {
    if (!file_) return;
    WriteHeader(frames_);
    std::fclose(file_);
    file_ = nullptr;
  }
  int64_t frames() const { return frames_; }

 private:
  void WriteHeader(uint32_t frames) {
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t data_bytes = frames * 2;
    const uint32_t riff = 36 + data_bytes;
    const uint32_t rate = static_cast<uint32_t>(rate_);
    const uint32_t byte_rate = rate * 2;
    const uint16_t block = 2;
    uint8_t h[44] = {};
    std::memcpy(h, "RIFF", 4);
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    uint32_t fmt = 16;
    std::memcpy(h + 16, &fmt, 4);
    uint16_t pcm = 1;
    std::memcpy(h + 20, &pcm, 2);
    std::memcpy(h + 22, &channels, 2);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &byte_rate, 4);
    std::memcpy(h + 32, &block, 2);
    std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data_bytes, 4);
    std::fseek(file_, 0, SEEK_SET);
    std::fwrite(h, 1, 44, file_);
    std::fseek(file_, 0, SEEK_END);
  }
  FILE* file_ = nullptr;
  int rate_ = 48000;
  uint32_t frames_ = 0;
};

bool ReadMonoWav(const std::string& path, int* rate, std::vector<int16_t>* mono) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t hdr[12];
  if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) ||
      std::memcmp(hdr + 8, "WAVE", 4)) {
    std::fclose(f);
    return false;
  }
  int channels = 0;
  int bits = 0;
  *rate = 0;
  std::vector<int16_t> interleaved;
  bool fmt_ok = false;
  bool data_ok = false;
  while (!data_ok) {
    uint8_t ch[8];
    if (std::fread(ch, 1, 8, f) != 8) break;
    uint32_t size = 0;
    std::memcpy(&size, ch + 4, 4);
    if (!std::memcmp(ch, "fmt ", 4)) {
      std::vector<uint8_t> fmt(size);
      if (std::fread(fmt.data(), 1, size, f) != size) break;
      uint16_t format = 0, chans = 0, bps = 0;
      uint32_t sr = 0;
      if (fmt.size() >= 16) {
        std::memcpy(&format, fmt.data(), 2);
        std::memcpy(&chans, fmt.data() + 2, 2);
        std::memcpy(&sr, fmt.data() + 4, 4);
        std::memcpy(&bps, fmt.data() + 14, 2);
      }
      if (format != 1 || bps != 16) {
        std::fclose(f);
        return false;
      }
      *rate = static_cast<int>(sr);
      channels = chans;
      bits = bps;
      fmt_ok = true;
    } else if (!std::memcmp(ch, "data", 4)) {
      if (!fmt_ok || channels < 1) {
        std::fclose(f);
        return false;
      }
      interleaved.resize(size / 2);
      if (!interleaved.empty() &&
          std::fread(interleaved.data(), 2, interleaved.size(), f) != interleaved.size()) {
        std::fclose(f);
        return false;
      }
      data_ok = true;
    } else if (std::fseek(f, static_cast<long>(size), SEEK_CUR) != 0) {
      break;
    }
    if (size & 1) std::fgetc(f);
  }
  std::fclose(f);
  if (!data_ok) return false;
  mono->resize(interleaved.size() / static_cast<size_t>(channels));
  for (size_t i = 0; i < mono->size(); ++i) {
    (*mono)[i] = interleaved[i * static_cast<size_t>(channels)];
  }
  (void)bits;
  return true;
}

class TapEncoder final : public AudioEncoder {
 public:
  TapEncoder(std::unique_ptr<AudioEncoder> inner, std::string record_dir, std::string inject)
      : inner_(std::move(inner)), record_dir_(std::move(record_dir)), inject_path_(std::move(inject)) {
    if (!record_dir_.empty()) {
      EnsureDir(record_dir_);
      wav_.Open(record_dir_ + "/input_48k.wav", inner_->SampleRateHz());
    }
    if (!inject_path_.empty()) {
      if (!ReadMonoWav(inject_path_, &inject_rate_, &inject_)) {
        fprintf(stderr, "inject wav failed: %s\n", inject_path_.c_str());
      } else {
        fprintf(stderr, "inject wav %s at %d Hz\n", inject_path_.c_str(), inject_rate_);
      }
    }
  }
  ~TapEncoder() override { WriteStats(); }

  int SampleRateHz() const override { return inner_->SampleRateHz(); }
  size_t NumChannels() const override { return inner_->NumChannels(); }
  int RtpTimestampRateHz() const override { return inner_->RtpTimestampRateHz(); }
  size_t Num10MsFramesInNextPacket() const override { return inner_->Num10MsFramesInNextPacket(); }
  size_t Max10MsFramesInAPacket() const override { return inner_->Max10MsFramesInAPacket(); }
  int GetTargetBitrate() const override { return inner_->GetTargetBitrate(); }
  void Reset() override { inner_->Reset(); }
  bool SetFec(bool enable) override { return inner_->SetFec(enable); }
  bool SetDtx(bool enable) override { return inner_->SetDtx(enable); }
  bool GetDtx() const override { return inner_->GetDtx(); }
  bool SetApplication(Application application) override { return inner_->SetApplication(application); }
  void SetMaxPlaybackRate(int frequency_hz) override { inner_->SetMaxPlaybackRate(frequency_hz); }
  void OnReceivedUplinkPacketLossFraction(float v) override {
    inner_->OnReceivedUplinkPacketLossFraction(v);
  }
  void OnReceivedTargetAudioBitrate(int bps) override { inner_->OnReceivedTargetAudioBitrate(bps); }
  void OnReceivedUplinkAllocation(BitrateAllocationUpdate update) override {
    inner_->OnReceivedUplinkAllocation(update);
  }
  void OnReceivedRtt(int rtt_ms) override { inner_->OnReceivedRtt(rtt_ms); }
  void OnReceivedOverhead(size_t overhead) override { inner_->OnReceivedOverhead(overhead); }
  void SetReceiverFrameLengthRange(int min_ms, int max_ms) override {
    inner_->SetReceiverFrameLengthRange(min_ms, max_ms);
  }
  ANAStats GetANAStats() const override { return inner_->GetANAStats(); }
  std::optional<std::pair<TimeDelta, TimeDelta>> GetFrameLengthRange() const override {
    return inner_->GetFrameLengthRange();
  }

 protected:
  EncodedInfo EncodeImpl(uint32_t rtp_timestamp, std::span<const int16_t> audio,
                         Buffer* encoded) override {
    std::vector<int16_t> replaced;
    std::span<const int16_t> view = audio;
    if (!inject_.empty() && inject_rate_ > 0) {
      replaced.assign(audio.size(), 0);
      const int src_n = std::max(1, inject_rate_ / 100);
      if (inject_pos_ + static_cast<size_t>(src_n) <= inject_.size()) {
        if (inject_rate_ == SampleRateHz() && static_cast<int>(audio.size()) == src_n) {
          std::memcpy(replaced.data(), inject_.data() + inject_pos_,
                      static_cast<size_t>(src_n) * sizeof(int16_t));
        } else {
          PushResampler<int16_t> rs;
          rs.Resample(std::span<const int16_t>(inject_.data() + inject_pos_, src_n),
                      std::span<int16_t>(replaced.data(), replaced.size()));
        }
        inject_pos_ += static_cast<size_t>(src_n);
      }
      view = replaced;
    }
    wav_.Write(view.data(), view.size());
    samples_ += static_cast<int64_t>(view.size());
    const size_t before = encoded->size();
    EncodedInfo info = inner_->Encode(rtp_timestamp, view, encoded);
    if (info.encoded_bytes > 0) {
      ++packets_;
      payload_bytes_ += static_cast<int64_t>(encoded->size() - before);
    }
    return info;
  }

 private:
  void WriteStats() {
    if (record_dir_.empty()) return;
    const double seconds = inner_->SampleRateHz() > 0
                               ? static_cast<double>(samples_) / inner_->SampleRateHz()
                               : 0;
    const double bps = seconds > 0 ? static_cast<double>(payload_bytes_) * 8.0 / seconds : 0;
    FILE* f = std::fopen((record_dir_ + "/tap_stats.json").c_str(), "wb");
    if (!f) return;
    std::fprintf(f,
                 "{\n  \"samples\": %lld,\n  \"packets\": %lld,\n  \"payload_bytes\": %lld,\n"
                 "  \"payload_bitrate_bps\": %.3f,\n  \"sample_rate_hz\": %d\n}\n",
                 static_cast<long long>(samples_), static_cast<long long>(packets_),
                 static_cast<long long>(payload_bytes_), bps, inner_->SampleRateHz());
    std::fclose(f);
  }

  std::unique_ptr<AudioEncoder> inner_;
  std::string record_dir_;
  std::string inject_path_;
  PcmWav wav_;
  std::vector<int16_t> inject_;
  int inject_rate_ = 0;
  size_t inject_pos_ = 0;
  int64_t samples_ = 0;
  int64_t packets_ = 0;
  int64_t payload_bytes_ = 0;
};

// Where decoded far-end PCM goes: the quality-run WAV and/or the voice-swap sidecar (UDP to 127.0.0.1).
class DecodedSink {
 public:
  DecodedSink(const std::string& record_dir, int rate, int udp_port, bool mute) : rate_(rate), mute_(mute) {
    if (!record_dir.empty()) {
      EnsureDir(record_dir);
      wav_.Open(record_dir + "/output_48k.wav", rate);
    }
    if (udp_port > 0) {
      fd_ = socket(AF_INET, SOCK_DGRAM, 0);
      if (fd_ >= 0) {
        fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);   // never block the decode thread
        std::memset(&addr_, 0, sizeof(addr_));
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(static_cast<uint16_t>(udp_port));
        addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::fprintf(stderr, "voice swap: decoded audio -> 127.0.0.1:%d (%d Hz)%s\n", udp_port, rate,
                     mute ? ", local playout muted" : "");
      }
    }
  }
  ~DecodedSink() {
    if (fd_ >= 0) close(fd_);
  }
  void Write(int16_t* p, size_t n) {
    if (!p || n == 0) return;
    wav_.Write(p, n);
    if (fd_ < 0) return;
    const int gen = g_voice_gen.load(std::memory_order_acquire);
    if (gen != seen_gen_ || ++since_ctl_ >= 100) {   // on change, and once a second so a restarted sidecar learns it
      if (gen != seen_gen_) {
        seen_gen_ = gen;
        voice_ = GetVoiceSwapVoice();
        std::fprintf(stderr, "voice swap: %s\n", voice_.empty() ? "off for this call" : ("converting to " + voice_).c_str());
      }
      since_ctl_ = 0;
      const std::string ctl = "VOICE:" + (voice_.empty() ? std::string("none") : voice_);
      sendto(fd_, ctl.data(), ctl.size(), 0, reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));
    }
    if (voice_.empty()) return;   // no conversion: play the caller as is
    const size_t chunk = static_cast<size_t>(rate_ / 100);   // 10 ms per datagram
    uint8_t buf[4 + 2 * 960];
    const uint32_t rate = static_cast<uint32_t>(rate_);
    for (size_t off = 0; off < n; off += chunk) {
      const size_t m = std::min(chunk, n - off);
      if (m > 960) break;
      std::memcpy(buf, &rate, 4);
      std::memcpy(buf + 4, p + off, m * 2);
      sendto(fd_, buf, 4 + m * 2, 0, reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));
    }
    if (mute_) std::fill(p, p + n, static_cast<int16_t>(0));
  }

 private:
  PcmWav wav_;
  int rate_;
  bool mute_;
  int fd_ = -1;
  int seen_gen_ = -1;
  int since_ctl_ = 0;
  std::string voice_;
  sockaddr_in addr_{};
};

class TapFrame final : public AudioDecoder::EncodedAudioFrame {
 public:
  TapFrame(std::unique_ptr<EncodedAudioFrame> inner, DecodedSink* sink)
      : inner_(std::move(inner)), sink_(sink) {}
  size_t Duration() const override { return inner_->Duration(); }
  bool IsDtxPacket() const override { return inner_->IsDtxPacket(); }
  std::optional<DecodeResult> Decode(std::span<int16_t> decoded) const override {
    const auto result = inner_->Decode(decoded);
    if (result && sink_) sink_->Write(decoded.data(), result->num_decoded_samples);
    return result;
  }

 private:
  std::unique_ptr<EncodedAudioFrame> inner_;
  DecodedSink* sink_;
};

class TapDecoder final : public AudioDecoder {
 public:
  TapDecoder(std::unique_ptr<AudioDecoder> inner, std::string record_dir, int udp_port, bool mute)
      : inner_(std::move(inner)),
        record_dir_(std::move(record_dir)),
        sink_(record_dir_, inner_->SampleRateHz(), udp_port, mute) {}
  int SampleRateHz() const override { return inner_->SampleRateHz(); }
  size_t Channels() const override { return inner_->Channels(); }
  void Reset() override {}
  int PacketDuration(const uint8_t* encoded, size_t len) const override {
    return inner_->PacketDuration(encoded, len);
  }
  bool PacketHasFec(const uint8_t* encoded, size_t len) const override {
    return inner_->PacketHasFec(encoded, len);
  }
  std::vector<ParseResult> ParsePayload(Buffer&& payload, uint32_t timestamp) override {
    auto parsed = inner_->ParsePayload(std::move(payload), timestamp);
    for (auto& item : parsed) {
      if (item.frame) {
        item.frame = std::make_unique<TapFrame>(std::move(item.frame), &sink_);
      }
    }
    return parsed;
  }

 protected:
  int DecodeInternal(const uint8_t* encoded, size_t encoded_len, int sample_rate_hz,
                     int16_t* decoded, SpeechType* speech_type) override {
    const int duration = inner_->PacketDuration(encoded, encoded_len);
    const size_t max_bytes = duration > 0
                                 ? static_cast<size_t>(duration) * Channels() * sizeof(int16_t)
                                 : static_cast<size_t>(sample_rate_hz) * Channels() * sizeof(int16_t);
    const int n = inner_->Decode(encoded, encoded_len, sample_rate_hz, max_bytes, decoded,
                                 speech_type);
    if (n > 0) sink_.Write(decoded, static_cast<size_t>(n));
    return n;
  }

 private:
  std::unique_ptr<AudioDecoder> inner_;
  std::string record_dir_;
  DecodedSink sink_;
};

class TapEncoderFactory : public AudioEncoderFactory {
 public:
  TapEncoderFactory(scoped_refptr<AudioEncoderFactory> inner, std::string record_dir,
                    std::string inject)
      : inner_(std::move(inner)),
        record_dir_(std::move(record_dir)),
        inject_(std::move(inject)) {}
  std::vector<AudioCodecSpec> GetSupportedEncoders() override {
    return inner_->GetSupportedEncoders();
  }
  std::optional<AudioCodecInfo> QueryAudioEncoder(const SdpAudioFormat& format) override {
    return inner_->QueryAudioEncoder(format);
  }
  std::unique_ptr<AudioEncoder> Create(const Environment& env, const SdpAudioFormat& format,
                                      Options options) override {
    auto enc = inner_->Create(env, format, options);
    if (!enc) return nullptr;
    return std::make_unique<TapEncoder>(std::move(enc), record_dir_, inject_);
  }

 private:
  scoped_refptr<AudioEncoderFactory> inner_;
  std::string record_dir_;
  std::string inject_;
};

class TapDecoderFactory : public AudioDecoderFactory {
 public:
  TapDecoderFactory(scoped_refptr<AudioDecoderFactory> inner, std::string record_dir, int udp_port, bool mute)
      : inner_(std::move(inner)), record_dir_(std::move(record_dir)), udp_port_(udp_port), mute_(mute) {}
  std::vector<AudioCodecSpec> GetSupportedDecoders() override {
    return inner_->GetSupportedDecoders();
  }
  bool IsSupportedDecoder(const SdpAudioFormat& format) override {
    return inner_->IsSupportedDecoder(format);
  }
  std::unique_ptr<AudioDecoder> Create(const Environment& env, const SdpAudioFormat& format,
                                      std::optional<AudioCodecPairId> pair) override {
    auto dec = inner_->Create(env, format, pair);
    if (!dec) return nullptr;
    return std::make_unique<TapDecoder>(std::move(dec), record_dir_, udp_port_, mute_);
  }

 private:
  scoped_refptr<AudioDecoderFactory> inner_;
  std::string record_dir_;
  int udp_port_;
  bool mute_;
};

}  // namespace

scoped_refptr<AudioEncoderFactory> CreateTappingAudioEncoderFactory(
    scoped_refptr<AudioEncoderFactory> inner, const std::string& record_dir,
    const std::string& inject_wav) {
  return make_ref_counted<TapEncoderFactory>(std::move(inner), record_dir, inject_wav);
}

scoped_refptr<AudioDecoderFactory> CreateTappingAudioDecoderFactory(
    scoped_refptr<AudioDecoderFactory> inner, const std::string& record_dir, int voice_swap_udp,
    bool voice_swap_mute) {
  return make_ref_counted<TapDecoderFactory>(std::move(inner), record_dir, voice_swap_udp, voice_swap_mute);
}

void SetVoiceSwapVoice(const std::string& voice) {
  {
    std::lock_guard<std::mutex> lock(g_voice_mu);
    g_voice = SanitizeVoiceName(voice);
  }
  g_voice_gen.fetch_add(1, std::memory_order_release);
}

std::string GetVoiceSwapVoice() {
  std::lock_guard<std::mutex> lock(g_voice_mu);
  return g_voice;
}

std::string SanitizeVoiceName(const std::string& voice) {
  std::string out;
  for (char c : voice) {
    if (out.size() >= 64) break;
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') out.push_back(c);
  }
  return out;
}

std::string ResolveVoiceSwap(const std::string& callee_option, const std::string& caller_request) {
  const std::string opt = SanitizeVoiceName(callee_option);
  if (opt == "off" || opt == "none") return "";
  if (!opt.empty() && opt != "auto") return opt;
  const std::string req = SanitizeVoiceName(caller_request);
  if (req == "none" || req == "off" || req == "auto") return "";
  return req;
}

}  // namespace webrtc
