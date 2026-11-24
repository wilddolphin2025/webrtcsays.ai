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
#include <deque>
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

// Leaked on purpose (no exit-time destructors).
std::mutex& VoiceMu() { static std::mutex* m = new std::mutex; return *m; }
std::string& VoiceStr() { static std::string* s = new std::string; return *s; }   // guarded by VoiceMu()
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

// Translated PCM from the local sidecar. The socket is drained on the encode
// thread; a short queue absorbs generation jitter, and the sidecar is expected
// to pace itself so this never holds seconds of audio.
class ReturnSource {
 public:
  explicit ReturnSource(int port) {
    if (port <= 0) return;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return;
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
      fprintf(stderr, "voice swap return: cannot bind 127.0.0.1:%d\n", port);
      close(fd_);
      fd_ = -1;
      return;
    }
    fprintf(stderr, "voice swap return: translated PCM <- 127.0.0.1:%d (48 kHz)\n", port);
  }
  ~ReturnSource() {
    if (fd_ >= 0) close(fd_);
  }
  bool active() const { return fd_ >= 0; }

  // Fills exactly n samples. Returns false (and leaves silence) on underrun.
  bool Read(int16_t* out, size_t n) {
    if (fd_ < 0 || !out || n == 0) return false;
    uint8_t buf[4 + 2 * 960];
    for (;;) {
      const ssize_t got = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
      if (got < 4) break;
      uint32_t rate = 0;
      std::memcpy(&rate, buf, 4);
      if (rate != 48000) {
        if (!warned_rate_) {
          fprintf(stderr, "voice swap return: ignoring %u Hz (need 48000)\n", rate);
          warned_rate_ = true;
        }
        continue;
      }
      const size_t samples = static_cast<size_t>(got - 4) / 2;
      const int16_t* pcm = reinterpret_cast<const int16_t*>(buf + 4);
      for (size_t i = 0; i < samples; ++i) q_.push_back(pcm[i]);
      while (q_.size() > kMaxSamples) q_.pop_front();
    }
    if (q_.size() < n) {
      std::fill(out, out + n, static_cast<int16_t>(0));
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      out[i] = q_.front();
      q_.pop_front();
    }
    return true;
  }

 private:
  static constexpr size_t kMaxSamples = 48000 / 2;  // 500 ms cap
  int fd_ = -1;
  bool warned_rate_ = false;
  std::deque<int16_t> q_;
};

// The encoder-input (post-APM) microphone, forwarded to the translation sidecar
// on 127.0.0.1:<port> as [uint32 sample_rate][int16 mono PCM, 10 ms] datagrams,
// so the Mac user's speech can be translated and returned via ReturnSource.
class MicSink {
 public:
  explicit MicSink(int port) {
    if (port <= 0) return;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return;
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(static_cast<uint16_t>(port));
    addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    fprintf(stderr, "voice swap mic: encoder input -> 127.0.0.1:%d\n", port);
  }
  ~MicSink() {
    if (fd_ >= 0) close(fd_);
  }
  void Send(int rate, const int16_t* pcm, size_t n) {
    if (fd_ < 0 || !pcm || n == 0) return;
    uint8_t buf[4 + 2 * 960];
    const uint32_t r = static_cast<uint32_t>(rate);
    for (size_t off = 0; off < n;) {
      const size_t m = std::min<size_t>(960, n - off);
      std::memcpy(buf, &r, 4);
      std::memcpy(buf + 4, pcm + off, m * 2);
      sendto(fd_, buf, 4 + m * 2, 0, reinterpret_cast<const sockaddr*>(&addr_), sizeof(addr_));
      off += m;
    }
  }

 private:
  int fd_ = -1;
  sockaddr_in addr_{};
};

class TapEncoder final : public AudioEncoder {
 public:
  TapEncoder(std::unique_ptr<AudioEncoder> inner, std::string record_dir, std::string inject,
             std::shared_ptr<ReturnSource> ret, std::shared_ptr<MicSink> mic)
      : inner_(std::move(inner)), record_dir_(std::move(record_dir)), inject_path_(std::move(inject)),
        ret_(std::move(ret)), mic_(std::move(mic)) {
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
    if (mic_) mic_->Send(SampleRateHz(), audio.data(), audio.size());
    if (ret_ && ret_->active()) {
      // The sidecar sends 48 kHz; resample to the encoder rate if the codec asks
      // for something else. On underrun this encodes silence, never the Mac mic.
      replaced.assign(audio.size(), 0);
      const int enc_rate = std::max(1, SampleRateHz());
      const size_t src_n = std::max<size_t>(1, (48000 * audio.size()) / static_cast<size_t>(enc_rate));
      std::vector<int16_t> src(src_n, 0);
      ret_->Read(src.data(), src.size());
      if (enc_rate == 48000 && src_n == audio.size()) {
        std::memcpy(replaced.data(), src.data(), audio.size() * sizeof(int16_t));
      } else {
        PushResampler<int16_t> rs;
        rs.Resample(std::span<const int16_t>(src.data(), src.size()),
                    std::span<int16_t>(replaced.data(), replaced.size()));
      }
      view = replaced;
    } else if (!inject_.empty() && inject_rate_ > 0) {
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
  std::shared_ptr<ReturnSource> ret_;
  std::shared_ptr<MicSink> mic_;
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
    // PCM always goes to the sidecar while it is attached, also for "none", so its Core ML engine stays warm
    // and a mid-call switch is heard at the next 80 ms block instead of after a cold start.
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
    // Local playout of the caller's own voice: on while no voice is active. A switch ramps it over 30 ms
    // (no click); the sidecar ramps the converted voice the same way.
    const float target = (mute_ && !voice_.empty()) ? 0.f : 1.f;
    if (gain_ == target) {
      if (target == 0.f) std::fill(p, p + n, static_cast<int16_t>(0));
      return;
    }
    const float step = 1.f / (0.03f * static_cast<float>(rate_));
    for (size_t i = 0; i < n; ++i) {
      gain_ = target > gain_ ? std::min(target, gain_ + step) : std::max(target, gain_ - step);
      p[i] = static_cast<int16_t>(static_cast<float>(p[i]) * gain_);
    }
  }

 private:
  PcmWav wav_;
  int rate_;
  bool mute_;
  int fd_ = -1;
  int seen_gen_ = -1;
  int since_ctl_ = 0;
  std::string voice_;
  float gain_ = 1.f;   // local playout gain, ramped on switches
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
                    std::string inject, int return_udp, int mic_udp)
      : inner_(std::move(inner)),
        record_dir_(std::move(record_dir)),
        inject_(std::move(inject)),
        ret_(std::make_shared<ReturnSource>(return_udp)),
        mic_(mic_udp > 0 ? std::make_shared<MicSink>(mic_udp) : nullptr) {}
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
    return std::make_unique<TapEncoder>(std::move(enc), record_dir_, inject_, ret_, mic_);
  }

 private:
  scoped_refptr<AudioEncoderFactory> inner_;
  std::string record_dir_;
  std::string inject_;
  std::shared_ptr<ReturnSource> ret_;
  std::shared_ptr<MicSink> mic_;
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
    const std::string& inject_wav, int voice_swap_return_udp, int voice_swap_mic_udp) {
  return make_ref_counted<TapEncoderFactory>(std::move(inner), record_dir, inject_wav,
                                             voice_swap_return_udp, voice_swap_mic_udp);
}

scoped_refptr<AudioDecoderFactory> CreateTappingAudioDecoderFactory(
    scoped_refptr<AudioDecoderFactory> inner, const std::string& record_dir, int voice_swap_udp,
    bool voice_swap_mute) {
  return make_ref_counted<TapDecoderFactory>(std::move(inner), record_dir, voice_swap_udp, voice_swap_mute);
}

void SetVoiceSwapVoice(const std::string& voice) {
  {
    std::lock_guard<std::mutex> lock(VoiceMu());
    VoiceStr() = SanitizeVoiceName(voice);
  }
  g_voice_gen.fetch_add(1, std::memory_order_release);
}

std::string GetVoiceSwapVoice() {
  std::lock_guard<std::mutex> lock(VoiceMu());
  return VoiceStr();
}

std::string SanitizeVoiceName(const std::string& voice) {
  std::string out;
  for (char c : voice) {
    if (out.size() >= 64) break;
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') out.push_back(c);
  }
  return out;
}

static std::vector<std::string> SplitVoiceList(const std::string& list) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i <= list.size(); ++i) {
    if (i == list.size() || list[i] == ',' || list[i] == ' ') {
      cur = SanitizeVoiceName(cur);
      if (!cur.empty() && std::find(out.begin(), out.end(), cur) == out.end()) out.push_back(cur);
      cur.clear();
      if (out.size() >= 16) break;
    } else {
      cur.push_back(list[i]);
    }
  }
  return out;
}

static std::string JoinVoiceList(const std::vector<std::string>& voices) {
  std::string out;
  for (const auto& v : voices) out += (out.empty() ? "" : ",") + v;
  return out;
}

std::string ResolveVoiceSwap(const std::string& callee_option, const std::string& caller_request,
                             const std::string& available) {
  const std::string opt = SanitizeVoiceName(callee_option);
  if (opt == "off" || opt == "none") return "";
  if (!opt.empty() && opt != "auto") return opt;
  const std::string req = SanitizeVoiceName(caller_request);
  if (req.empty() || req == kVoiceMe || req == "none" || req == "off" || req == "auto") return "";
  const auto have = SplitVoiceList(available);
  if (have.empty()) return req;   // no list configured: trust the sidecar to know the voice
  return std::find(have.begin(), have.end(), req) != have.end() ? req : std::string();
}

// Range [begin, end) of the audio m-section and the LIQUIDCALL payload type in it ("" if not offered).
static bool AudioSection(const std::string& sdp, size_t* begin, size_t* end, std::string* pt) {
  size_t m = sdp.rfind("m=audio", 0) == 0 ? 0 : sdp.find("\nm=audio");
  if (m == std::string::npos) return false;
  if (sdp[m] == '\n') ++m;
  size_t e = sdp.find("\nm=", m);
  *begin = m;
  *end = e == std::string::npos ? sdp.size() : e + 1;
  pt->clear();
  const std::string sec = sdp.substr(m, *end - m);
  size_t r = 0;
  while ((r = sec.find("a=rtpmap:", r)) != std::string::npos) {
    r += 9;
    const size_t sp = sec.find(' ', r);
    if (sp == std::string::npos) break;
    if (sec.compare(sp + 1, 11, "LIQUIDCALL/") == 0) { *pt = sec.substr(r, sp - r); break; }
  }
  return true;
}

static std::string AddAudioLines(const std::string& sdp, const std::vector<std::string>& lines) {
  size_t b = 0, e = 0;
  std::string pt;
  if (lines.empty() || !AudioSection(sdp, &b, &e, &pt)) return sdp;
  if (pt.empty()) return sdp;   // not a LiquidCall call: nothing to negotiate
  const bool crlf = sdp.find("\r\n") != std::string::npos;
  std::string add;
  for (const auto& l : lines) {
    const size_t colon = l.find(':');
    add += l.substr(0, colon + 1) + pt + " " + l.substr(colon + 1) + (crlf ? "\r\n" : "\n");
  }
  std::string out = sdp;
  if (e == out.size() && !out.empty() && out.back() != '\n') out += crlf ? "\r\n" : "\n", e = out.size();
  out.insert(e, add);
  return out;
}

// Value of "<attr><pt> <value>" inside the audio m-section (pt not checked: one LIQUIDCALL payload per call).
static std::string AudioAttr(const std::string& sdp, const char* attr) {
  size_t b = 0, e = 0;
  std::string pt;
  if (!AudioSection(sdp, &b, &e, &pt)) return "";
  const size_t p = sdp.find(attr, b);
  if (p == std::string::npos || p >= e) return "";
  size_t v = p + std::strlen(attr);
  size_t eol = sdp.find_first_of("\r\n", v);
  if (eol == std::string::npos) eol = sdp.size();
  std::string val = sdp.substr(v, eol - v);
  const size_t sp = val.find(' ');
  return sp == std::string::npos ? val : val.substr(sp + 1);
}

std::string AddVoiceRequestToSdp(const std::string& sdp, const std::string& voice) {
  const std::string v = SanitizeVoiceName(voice);
  if (v.empty() || v == kVoiceMe || v == "none" || v == "off") return sdp;   // absent = no conversion
  return AddAudioLines(sdp, {std::string(kVoiceSdpAttr) + v});
}

std::string AddVoicesToSdp(const std::string& sdp, const std::string& voices, const std::string& active) {
  std::vector<std::string> list{kVoiceMe};
  for (const auto& v : SplitVoiceList(voices)) if (v != kVoiceMe) list.push_back(v);
  const std::string a = SanitizeVoiceName(active);
  return AddAudioLines(sdp, {std::string(kVoicesSdpAttr) + JoinVoiceList(list),
                             std::string(kVoiceSdpAttr) + (a.empty() ? kVoiceMe : a)});
}

std::string VoicesFromSdp(const std::string& sdp) {
  return JoinVoiceList(SplitVoiceList(AudioAttr(sdp, kVoicesSdpAttr)));
}

std::string VoiceFromSdp(const std::string& sdp) {
  return SanitizeVoiceName(AudioAttr(sdp, kVoiceSdpAttr));
}

}  // namespace webrtc
