#include "modules/audio_coding/codecs/liquidcodec/liquid_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/stat.h>

namespace webrtc {
namespace liquid {
namespace {

bool ReadU16(const uint8_t* p, uint16_t* v) {
  *v = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
  return true;
}

void WriteU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void SkipWs(const std::string& s, size_t* i) {
  while (*i < s.size() && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' ||
                           s[*i] == '\r')) {
    ++*i;
  }
}

bool FindKey(const std::string& json, const char* key, size_t* value_at) {
  const std::string pattern = std::string("\"") + key + "\"";
  size_t pos = 0;
  while ((pos = json.find(pattern, pos)) != std::string::npos) {
    size_t i = pos + pattern.size();
    SkipWs(json, &i);
    if (i < json.size() && json[i] == ':') {
      ++i;
      SkipWs(json, &i);
      *value_at = i;
      return true;
    }
    pos += pattern.size();
  }
  return false;
}

bool ParseJsonStringAt(const std::string& json, size_t* at, std::string* out);

bool ParseJsonString(const std::string& json, size_t at, std::string* out) {
  size_t end = at;
  return ParseJsonStringAt(json, &end, out);
}

bool ParseJsonStringAt(const std::string& json, size_t* at, std::string* out) {
  size_t i = *at;
  if (i >= json.size() || json[i] != '"') return false;
  ++i;
  out->clear();
  while (i < json.size() && json[i] != '"') {
    if (json[i] == '\\' && i + 1 < json.size()) {
      out->push_back(json[i + 1]);
      i += 2;
      continue;
    }
    out->push_back(json[i]);
    ++i;
  }
  if (i >= json.size() || json[i] != '"') return false;
  *at = i + 1;
  return true;
}

bool ParseStringList(const std::string& json, const char* key,
                     std::vector<std::string>* out) {
  size_t at = 0;
  if (!FindKey(json, key, &at) || at >= json.size() || json[at] != '[') return false;
  ++at;
  out->clear();
  while (at < json.size() && json[at] != ']') {
    SkipWs(json, &at);
    if (at >= json.size() || json[at] == ']') break;
    if (json[at] == ',') {
      ++at;
      continue;
    }
    std::string value;
    if (!ParseJsonStringAt(json, &at, &value)) return false;
    out->push_back(value);
  }
  return true;
}

bool ParseJsonNumber(const std::string& json, size_t at, double* out) {
  char* end = nullptr;
  const char* begin = json.c_str() + at;
  *out = std::strtod(begin, &end);
  return end != begin;
}

void MaybeString(const std::string& json, const char* key, std::string* dest) {
  size_t at = 0;
  std::string value;
  if (FindKey(json, key, &at) && ParseJsonString(json, at, &value) &&
      !value.empty()) {
    *dest = value;
  }
}

void MaybeInt(const std::string& json, const char* key, int* dest) {
  size_t at = 0;
  double value = 0;
  if (FindKey(json, key, &at) && ParseJsonNumber(json, at, &value)) {
    *dest = static_cast<int>(value);
  }
}

void WriteWavHeader(FILE* f, int rate, uint32_t frames) {
  const uint16_t channels = 1;
  const uint16_t bits = 16;
  const uint32_t data_bytes = frames * channels * (bits / 8);
  const uint32_t riff_size = 36 + data_bytes;
  const uint32_t byte_rate = static_cast<uint32_t>(rate) * channels * bits / 8;
  const uint16_t block = channels * bits / 8;
  uint8_t h[44];
  std::memset(h, 0, sizeof(h));
  std::memcpy(h + 0, "RIFF", 4);
  std::memcpy(h + 4, &riff_size, 4);
  std::memcpy(h + 8, "WAVE", 4);
  std::memcpy(h + 12, "fmt ", 4);
  uint32_t fmt_size = 16;
  std::memcpy(h + 16, &fmt_size, 4);
  uint16_t audio_format = 1;
  std::memcpy(h + 20, &audio_format, 2);
  std::memcpy(h + 22, &channels, 2);
  uint32_t sample_rate = static_cast<uint32_t>(rate);
  std::memcpy(h + 24, &sample_rate, 4);
  std::memcpy(h + 28, &byte_rate, 4);
  std::memcpy(h + 32, &block, 2);
  std::memcpy(h + 34, &bits, 2);
  std::memcpy(h + 36, "data", 4);
  std::memcpy(h + 40, &data_bytes, 4);
  std::fseek(f, 0, SEEK_SET);
  std::fwrite(h, 1, 44, f);
}

}  // namespace

bool ChunkConfig::Ok() const {
  return sample_rate_hz == kSampleRateHz && frame_samples == kFrameSamples &&
         codebooks == kCodebooks && codebook_size > 1 &&
         codebook_size <= 65535 && ctx_frames >= 0 && new_frames >= 1 &&
         frames_per_second > 0;
}

LatencyReport DescribeLatency(const ChunkConfig& chunk, bool hidden_tail) {
  return DescribeLatency(chunk, hidden_tail, ModelPaths{});
}

LatencyReport DescribeLatency(const ChunkConfig& chunk, bool hidden_tail,
                              const ModelPaths& paths) {
  LatencyReport r;
  const bool stateful = paths.Stateful();
  r.encoder_lookahead_ms =
      paths.lookahead_samples > 0
          ? paths.lookahead_samples * 1000 / chunk.sample_rate_hz
          : 0;
  if (stateful) {
    // The step encoder emits one frame once that frame plus the real
    // lookahead have arrived. Later frames follow every 80 ms, so the
    // receiver's wait is just the spacing until its chunk is full.
    r.sender_collect_ms = kFrameMs + r.encoder_lookahead_ms;
    r.receiver_collect_ms = (chunk.new_frames - 1) * kFrameMs;
  } else {
    // A stateless window emits nothing until every new frame is collected,
    // then the packets are paced one per 80 ms.
    r.sender_collect_ms = chunk.new_frames * kFrameMs;
    r.receiver_collect_ms = (chunk.new_frames - 1) * kFrameMs;
  }
  r.ola_holdback_ms = hidden_tail ? (kTrim * 1000) / kSampleRateHz : 0;
  r.algorithmic_ms = r.sender_collect_ms + r.receiver_collect_ms + r.ola_holdback_ms;
  std::ostringstream o;
  o << "Calculated codec latency " << r.algorithmic_ms << " ms (sender collect "
    << r.sender_collect_ms << " ms";
  if (r.encoder_lookahead_ms > 0) {
    o << ", including " << r.encoder_lookahead_ms << " ms encoder lookahead";
  }
  o << " + receiver collect " << r.receiver_collect_ms << " ms";
  if (hidden_tail) {
    o << " + STFT tail holdback " << r.ola_holdback_ms << " ms";
  }
  o << "). Left context is " << chunk.ctx_frames
    << " frames of lookback and is not added. "
       "NetEq jitter and Core ML runtime are not included. new_frames="
    << chunk.new_frames << " ctx_frames=" << chunk.ctx_frames << ".";
  r.summary = o.str();
  return r;
}

double InformationBitrateBps(const ChunkConfig& chunk) {
  return static_cast<double>(chunk.codebooks) * std::log2(chunk.codebook_size) *
         chunk.frames_per_second;
}

double PayloadBitrateBps() {
  return static_cast<double>(kPayloadBytes) * 8.0 * kFramesPerSecond;
}

bool ParseManifest(const std::string& json,
                   ModelPaths* paths,
                   ChunkConfig* chunk,
                   std::string* error) {
  if (!paths || !chunk) {
    if (error) *error = "manifest output is null";
    return false;
  }
  MaybeString(json, "encoder_model", &paths->encoder_model);
  MaybeString(json, "decoder_model", &paths->decoder_model);
  MaybeString(json, "tail", &paths->tail);
  MaybeString(json, "decoder_kind", &paths->decoder_kind);
  MaybeString(json, "encoder_input", &paths->encoder_input);
  MaybeString(json, "encoder_output", &paths->encoder_output);
  MaybeString(json, "decoder_lat_input", &paths->decoder_lat_input);
  MaybeString(json, "decoder_valid_input", &paths->decoder_valid_input);
  MaybeString(json, "decoder_output", &paths->decoder_output);
  MaybeString(json, "encoder_window", &paths->encoder_window);
  MaybeString(json, "encoder_codes_layout", &paths->encoder_codes_layout);
  MaybeString(json, "compute", &paths->compute);
  MaybeString(json, "encoder_kind", &paths->encoder_kind);
  MaybeInt(json, "lookahead_samples", &paths->lookahead_samples);
  MaybeInt(json, "stft_lookback_samples", &paths->stft_lookback_samples);
  MaybeInt(json, "encoder_input_samples", &paths->encoder_input_samples);
  MaybeInt(json, "encoder_cache_frames", &paths->encoder_cache_frames);
  ParseStringList(json, "encoder_states_in", &paths->encoder_states_in);
  ParseStringList(json, "encoder_states_out", &paths->encoder_states_out);
  MaybeInt(json, "sample_rate_hz", &chunk->sample_rate_hz);
  MaybeInt(json, "frame_samples", &chunk->frame_samples);
  MaybeInt(json, "codebooks", &chunk->codebooks);
  MaybeInt(json, "codebook_size", &chunk->codebook_size);
  MaybeInt(json, "ctx_frames", &chunk->ctx_frames);
  MaybeInt(json, "new_frames", &chunk->new_frames);
  if (paths->decoder_kind != "hidden" && paths->decoder_kind != "pcm") {
    if (error) *error = "decoder_kind must be \"hidden\" or \"pcm\"";
    return false;
  }
  if (paths->encoder_window != "ctx+new" && paths->encoder_window != "new") {
    if (error) *error = "encoder_window must be \"ctx+new\" or \"new\"";
    return false;
  }
  if (paths->Stateful()) {
    if (paths->lookahead_samples <= 0) paths->lookahead_samples = 480;
    if (paths->encoder_input_samples <= 0) {
      paths->encoder_input_samples = chunk->frame_samples + paths->lookahead_samples;
    }
    if (paths->encoder_states_in.empty()) {
      paths->encoder_states_in = {"hist", "k_cache", "v_cache", "conv_state", "pos"};
      paths->encoder_states_out = {"hist_out", "k_cache_out", "v_cache_out",
                                   "conv_state_out", "pos_out"};
    }
    if (paths->encoder_states_in.size() != paths->encoder_states_out.size()) {
      if (error) *error = "encoder_states_in and encoder_states_out differ in length";
      return false;
    }
  }
  if (!chunk->Ok()) {
    if (error) {
      *error =
          "manifest chunk is not usable (need 24 kHz, 1920 samples/frame, "
          "12 codebooks, ctx_frames >= 0, new_frames >= 1)";
    }
    return false;
  }
  return true;
}

bool LoadManifestFile(const std::string& path,
                      ModelPaths* paths,
                      ChunkConfig* chunk,
                      std::string* error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) *error = "cannot open manifest " + path;
    return false;
  }
  std::string json;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
  std::fclose(f);
  return ParseManifest(json, paths, chunk, error);
}

std::string JoinPath(const std::string& dir, const std::string& leaf) {
  if (leaf.empty()) return dir;
  if (!leaf.empty() && leaf[0] == '/') return leaf;
  if (dir.empty()) return leaf;
  if (dir.back() == '/') return dir + leaf;
  return dir + "/" + leaf;
}

bool FileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool PackFrame(const int32_t* codes12, uint16_t seq, int codebook_size,
               uint8_t* dst, size_t cap, size_t* out_len) {
  if (!codes12 || !dst || !out_len || cap < static_cast<size_t>(kPayloadBytes)) {
    return false;
  }
  for (int i = 0; i < kCodebooks; ++i) {
    if (codes12[i] < 0 || codes12[i] >= codebook_size) return false;
  }
  dst[0] = static_cast<uint8_t>(kPayloadVersion);
  dst[1] = 1;
  WriteU16(dst + 2, seq);
  for (int i = 0; i < kCodebooks; ++i) {
    WriteU16(dst + 4 + i * 2, static_cast<uint16_t>(codes12[i]));
  }
  *out_len = kPayloadBytes;
  return true;
}

bool UnpackFrame(const uint8_t* src, size_t len, int codebook_size,
                 int32_t* codes12, uint16_t* seq) {
  if (!src || !codes12 || !seq || len != static_cast<size_t>(kPayloadBytes)) {
    return false;
  }
  if (src[0] != kPayloadVersion || src[1] != 1) return false;
  ReadU16(src + 2, seq);
  for (int i = 0; i < kCodebooks; ++i) {
    uint16_t v = 0;
    ReadU16(src + 4 + i * 2, &v);
    if (v >= codebook_size) return false;
    codes12[i] = static_cast<int32_t>(v);
  }
  return true;
}

void CodesToLatent(const int32_t* codes, int frames, int codebooks, float* lat) {
  static constexpr int kLevels[4] = {8, 7, 6, 6};
  static constexpr int kBasis[4] = {1, 8, 56, 336};
  static constexpr int kHalf[4] = {4, 3, 3, 3};
  for (int f = 0; f < frames; ++f) {
    for (int q = 0; q < codebooks; ++q) {
      int code = codes[f * codebooks + q];
      if (code < 0) code = 0;
      for (int d = 0; d < 4; ++d) {
        const int digit = (code / kBasis[d]) % kLevels[d];
        const float v =
            (static_cast<float>(digit - kHalf[d])) / static_cast<float>(kHalf[d]);
        lat[(q * 4 + d) * frames + f] = v;
      }
    }
  }
}

void CodecStats::AddPacket(int bytes) {
  if (packets == 0) {
    packet_bytes_min = bytes;
    packet_bytes_max = bytes;
  } else {
    packet_bytes_min = std::min(packet_bytes_min, bytes);
    packet_bytes_max = std::max(packet_bytes_max, bytes);
  }
  ++packets;
  payload_bytes += bytes;
}

void CodecStats::AddWindow(double window, double prediction) {
  window_ms.push_back(window);
  prediction_ms.push_back(prediction);
}

double CodecStats::PayloadBitrateBps() const {
  if (samples_24k <= 0) return 0;
  const double seconds =
      static_cast<double>(samples_24k) / static_cast<double>(kSampleRateHz);
  if (seconds <= 0) return 0;
  return static_cast<double>(payload_bytes) * 8.0 / seconds;
}

double CodecStats::Mean(const std::vector<double>& v) const {
  if (v.empty()) return 0;
  double s = 0;
  for (double x : v) s += x;
  return s / static_cast<double>(v.size());
}

std::string CodecStats::ToJson() const {
  ModelPaths paths;
  paths.lookahead_samples = lookahead_samples;
  if (stateful_encoder) paths.encoder_kind = "stateful_io";
  const LatencyReport latency = DescribeLatency(chunk, hidden_tail, paths);
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(3);
  o << "{\n";
  o << "  \"codec\": \"LIQUIDCODEC\",\n";
  o << "  \"backend\": \"" << backend << "\",\n";
  o << "  \"compute\": \"" << compute << "\",\n";
  o << "  \"sample_rate_hz\": " << chunk.sample_rate_hz << ",\n";
  o << "  \"webrtc_clock_hz\": " << kWebRtcClockHz << ",\n";
  o << "  \"codebooks\": " << chunk.codebooks << ",\n";
  o << "  \"codebook_size\": " << chunk.codebook_size << ",\n";
  o << "  \"ctx_frames\": " << chunk.ctx_frames << ",\n";
  o << "  \"new_frames\": " << chunk.new_frames << ",\n";
  o << "  \"information_bitrate_bps\": " << InformationBitrateBps(chunk) << ",\n";
  o << "  \"nominal_payload_bitrate_bps\": " << PayloadBitrateBps() << ",\n";
  o << "  \"packets\": " << packets << ",\n";
  o << "  \"payload_bytes\": " << payload_bytes << ",\n";
  o << "  \"packet_bytes_min\": " << packet_bytes_min << ",\n";
  o << "  \"packet_bytes_max\": " << packet_bytes_max << ",\n";
  o << "  \"payload_bitrate_bps\": " << PayloadBitrateBps() << ",\n";
  o << "  \"samples_24k\": " << samples_24k << ",\n";
  o << "  \"model_load_ms\": " << load_ms << ",\n";
  o << "  \"window_ms_mean\": " << Mean(window_ms) << ",\n";
  o << "  \"window_calls\": " << window_ms.size() << ",\n";
  o << "  \"prediction_ms_mean\": " << Mean(prediction_ms) << ",\n";
  o << "  \"algorithmic_latency_ms\": " << latency.algorithmic_ms << ",\n";
  o << "  \"encoder_lookahead_ms\": " << latency.encoder_lookahead_ms << ",\n";
  o << "  \"sender_collect_ms\": " << latency.sender_collect_ms << ",\n";
  o << "  \"receiver_collect_ms\": " << latency.receiver_collect_ms << ",\n";
  o << "  \"ola_holdback_ms\": " << latency.ola_holdback_ms << ",\n";
  o << "  \"latency_note\": \"" << latency.summary << "\"\n";
  o << "}\n";
  return o.str();
}

bool CodecStats::WriteJson(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const std::string json = ToJson();
  const bool ok = std::fwrite(json.data(), 1, json.size(), f) == json.size();
  std::fclose(f);
  return ok;
}

WavWriter::~WavWriter() { Close(); }

bool WavWriter::Open(const std::string& path, int sample_rate_hz, std::string* error) {
  Close();
  FILE* f = std::fopen(path.c_str(), "wb+");
  if (!f) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  file_ = f;
  rate_ = sample_rate_hz;
  frames_ = 0;
  path_ = path;
  WriteWavHeader(f, rate_, 0);
  return true;
}

void WavWriter::Write(const int16_t* pcm, size_t n) {
  FILE* f = static_cast<FILE*>(file_);
  if (!f || n == 0 || !pcm) return;
  std::fwrite(pcm, sizeof(int16_t), n, f);
  frames_ += static_cast<uint32_t>(n);
  std::fflush(f);
}

void WavWriter::Close() {
  FILE* f = static_cast<FILE*>(file_);
  if (!f) return;
  WriteWavHeader(f, rate_, frames_);
  std::fclose(f);
  file_ = nullptr;
}

bool ReadWav(const std::string& path, WavData* out, std::string* error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) *error = "cannot open wav " + path;
    return false;
  }
  uint8_t hdr[12];
  if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 ||
      std::memcmp(hdr + 8, "WAVE", 4) != 0) {
    std::fclose(f);
    if (error) *error = "not a RIFF WAVE file: " + path;
    return false;
  }
  int rate = 0;
  int channels = 0;
  int bits = 0;
  std::vector<int16_t> interleaved;
  bool have_fmt = false;
  bool have_data = false;
  while (!have_data) {
    uint8_t ch[8];
    if (std::fread(ch, 1, 8, f) != 8) break;
    uint32_t size = 0;
    std::memcpy(&size, ch + 4, 4);
    if (std::memcmp(ch, "fmt ", 4) == 0) {
      std::vector<uint8_t> fmt(size);
      if (std::fread(fmt.data(), 1, size, f) != size) break;
      uint16_t format = 0;
      uint16_t chans = 0;
      uint16_t bps = 0;
      uint32_t sr = 0;
      if (fmt.size() >= 16) {
        std::memcpy(&format, fmt.data(), 2);
        std::memcpy(&chans, fmt.data() + 2, 2);
        std::memcpy(&sr, fmt.data() + 4, 4);
        std::memcpy(&bps, fmt.data() + 14, 2);
      }
      if (format != 1 || bps != 16) {
        std::fclose(f);
        if (error) *error = "wav must be 16-bit PCM: " + path;
        return false;
      }
      rate = static_cast<int>(sr);
      channels = static_cast<int>(chans);
      bits = bps;
      have_fmt = true;
    } else if (std::memcmp(ch, "data", 4) == 0) {
      if (!have_fmt || channels < 1) {
        std::fclose(f);
        if (error) *error = "wav data before fmt: " + path;
        return false;
      }
      const size_t samples = size / sizeof(int16_t);
      interleaved.resize(samples);
      if (samples &&
          std::fread(interleaved.data(), sizeof(int16_t), samples, f) != samples) {
        std::fclose(f);
        if (error) *error = "short wav data: " + path;
        return false;
      }
      have_data = true;
    } else {
      if (std::fseek(f, static_cast<long>(size), SEEK_CUR) != 0) break;
    }
    if (size & 1) std::fgetc(f);
  }
  std::fclose(f);
  if (!have_data) {
    if (error) *error = "wav has no data chunk: " + path;
    return false;
  }
  out->sample_rate_hz = rate;
  out->channels = channels;
  out->mono.clear();
  const size_t frames = interleaved.size() / static_cast<size_t>(channels);
  out->mono.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    out->mono[i] = interleaved[i * static_cast<size_t>(channels)];
  }
  (void)bits;
  return true;
}

FramePacer::FramePacer(int frames_per_packet_10ms)
    : ticks_per_frame_(frames_per_packet_10ms > 0 ? frames_per_packet_10ms : 8) {}

std::optional<PacedPacket> FramePacer::Tick(uint32_t rtp_timestamp,
                                            const int32_t* new_codes,
                                            int nframes) {
  if (ticks_ == 0) frame_ts_ = rtp_timestamp;
  ++ticks_;
  const bool boundary = ticks_ >= ticks_per_frame_;
  if (boundary) {
    Slot slot;
    slot.timestamp = frame_ts_;
    slots_.push_back(slot);
    ticks_ = 0;
  }
  if (nframes > 0 && new_codes) {
    int assigned = 0;
    for (Slot& slot : slots_) {
      if (slot.ready) continue;
      if (assigned >= nframes) break;
      for (int c = 0; c < kCodebooks; ++c) {
        slot.codes[static_cast<size_t>(c)] =
            new_codes[assigned * kCodebooks + c];
      }
      slot.ready = true;
      ++assigned;
    }
  }
  int ready = 0;
  for (const Slot& slot : slots_) {
    if (slot.ready) ++ready;
  }
  // A stateless chunk returns many frames at one boundary. Those stay paced
  // at 80 ms. A single ready frame (the streaming encoder, whose lookahead
  // arrives between boundaries) is sent on the tick it becomes ready.
  if (slots_.empty() || !slots_.front().ready) return std::nullopt;
  if (ready > 1 && !boundary) return std::nullopt;
  PacedPacket packet;
  packet.timestamp = slots_.front().timestamp;
  packet.codes = slots_.front().codes;
  slots_.erase(slots_.begin());
  return packet;
}

void FramePacer::Reset() {
  ticks_ = 0;
  frame_ts_ = 0;
  slots_.clear();
}

}  // namespace liquid
}  // namespace webrtc
