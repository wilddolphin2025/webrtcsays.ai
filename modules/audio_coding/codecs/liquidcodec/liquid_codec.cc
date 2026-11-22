#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "modules/audio_coding/codecs/liquidcodec/liquid_codec.h"

#include <algorithm>
#include <atomic>
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
  return PayloadBitrateBps(WirePack::kU16, 1);
}

int PayloadBytes(WirePack pack, int frames) {
  if (frames < 1 || frames > 4) return 0;
  if (pack == WirePack::kTight) {
    return (frames * kCodebooks * kTightBitsPerCode + 7) / 8;
  }
  return kPayloadHeaderBytes + frames * kU16BytesPerFrame;
}

double PayloadBitrateBps(WirePack pack, int frames) {
  const int bytes = PayloadBytes(pack, frames);
  if (bytes <= 0) return 0;
  return static_cast<double>(bytes) * 8.0 * kFramesPerSecond / static_cast<double>(frames);
}

void OnWireBitrateBps(int payload_bytes, double packets_per_second, int srtp_tag_bytes,
                      double* ipv4_udp_rtp_bps, double* with_srtp_bps) {
  const double packets = packets_per_second;
  const int headers = kIpHeaderBytes + kUdpHeaderBytes + kRtpHeaderBytes;
  if (ipv4_udp_rtp_bps) {
    *ipv4_udp_rtp_bps = (payload_bytes + headers) * 8.0 * packets;
  }
  if (with_srtp_bps) {
    *with_srtp_bps =
        (payload_bytes + headers + std::max(0, srtp_tag_bytes)) * 8.0 * packets;
  }
}

namespace {

void WriteBits(uint8_t* dst, int* bit, uint32_t value, int nbits) {
  for (int i = nbits - 1; i >= 0; --i) {
    if ((value >> i) & 1u) dst[*bit / 8] |= static_cast<uint8_t>(1u << (7 - (*bit % 8)));
    ++*bit;
  }
}

uint32_t ReadBits(const uint8_t* src, int* bit, int nbits) {
  uint32_t value = 0;
  for (int i = 0; i < nbits; ++i) {
    value <<= 1;
    if (src[*bit / 8] & (1u << (7 - (*bit % 8)))) value |= 1u;
    ++*bit;
  }
  return value;
}

}  // namespace

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

bool QuantizeCode(float raw, int codebook_size, int32_t* quantized, bool* clamped) {
  if (!quantized || codebook_size <= 1) return false;
  if (clamped) *clamped = false;
  const float lo = -1.f;
  // int8 rounding can land on codebook_size itself (2016.0). Allow one
  // extra unit and clamp it back to the last legal index.
  const float hi = static_cast<float>(codebook_size) + 1.f;
  if (!(raw >= lo && raw < hi)) return false;
  int value = static_cast<int>(std::lrintf(raw));
  if (value < 0 || value >= codebook_size) {
    if (clamped) *clamped = true;
    if (value < 0) value = 0;
    if (value >= codebook_size) value = codebook_size - 1;
  }
  *quantized = value;
  return true;
}

std::string EncoderModelForPrecision(const std::string& model, const std::string& precision) {
  if (precision != "int8" && precision != "fp16" && precision != "fp32") return model;
  const std::string token = "_" + precision;
  std::string out = model;
  for (const char* old : {"_int8", "_fp16", "_fp32"}) {
    if (token == old) continue;
    const size_t pos = out.find(old);
    if (pos == std::string::npos) continue;
    out.replace(pos, std::strlen(old), token);
    break;
  }
  return out;
}

namespace {
std::atomic<int> g_out_of_codebook{0};
std::atomic<int> g_clamped_codes{0};
}  // namespace

void NoteOutOfCodebook() { g_out_of_codebook.fetch_add(1, std::memory_order_relaxed); }
void NoteClampedCode() { g_clamped_codes.fetch_add(1, std::memory_order_relaxed); }
int ConsumeOutOfCodebook() { return g_out_of_codebook.exchange(0, std::memory_order_relaxed); }
int ConsumeClampedCodes() { return g_clamped_codes.exchange(0, std::memory_order_relaxed); }

uint64_t RtpHeaderBytesForOnWire(uint64_t reported_header_bytes, uint64_t packets) {
  if (packets == 0) return reported_header_bytes;
  const double per = static_cast<double>(reported_header_bytes) / static_cast<double>(packets);
  if (per >= 8.0 && per <= 20.0) return reported_header_bytes;
  return packets * 12;
}

double OnWireKbps(uint64_t payload_bytes, uint64_t reported_header_bytes, uint64_t packets,
                  double seconds, bool encrypted, bool ipv6) {
  if (seconds <= 0) return 0;
  uint64_t per_packet = 28;
  if (encrypted) per_packet += 16;
  if (ipv6) per_packet += 20;
  const uint64_t total = payload_bytes +
                         RtpHeaderBytesForOnWire(reported_header_bytes, packets) +
                         packets * per_packet;
  return static_cast<double>(total) * 8.0 / seconds / 1000.0;
}

bool ApplyNegotiatedPack(const std::string& pack, const std::string& fpp,
                         WirePack* wire_pack, int* frames_per_packet,
                         std::string* mismatch) {
  if (!wire_pack || !frames_per_packet) return false;
  std::string note;
  if (!pack.empty() && pack != "u16" && pack != "tight") {
    note = "negotiated pack \"" + pack + "\" is not u16 or tight";
  } else if (!pack.empty()) {
    const WirePack negotiated = pack == "tight" ? WirePack::kTight : WirePack::kU16;
    if (negotiated != *wire_pack) {
      note = std::string("pack local ") + (*wire_pack == WirePack::kTight ? "tight" : "u16") +
             " negotiated " + pack + "; using negotiated";
      *wire_pack = negotiated;
    }
  }
  if (!fpp.empty()) {
    const int n = std::atoi(fpp.c_str());
    if (n < 1 || n > 4) {
      if (!note.empty()) note += "; ";
      note += "negotiated fpp \"" + fpp + "\" is not 1..4";
    } else if (n != *frames_per_packet) {
      if (!note.empty()) note += "; ";
      note += "fpp local " + std::to_string(*frames_per_packet) + " negotiated " +
              std::to_string(n) + "; using negotiated";
      *frames_per_packet = n;
    }
  }
  if (mismatch) *mismatch = note;
  return note.empty();
}

std::string CompiledModelPath(const std::string& path) {
  const std::string suffix = ".mlpackage";
  if (path.size() >= suffix.size() &&
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return path.substr(0, path.size() - suffix.size()) + ".mlmodelc";
  }
  return path;
}

int StridedElementCount(const int* shape, const int* strides, int rank, int count) {
  if (rank <= 0 || shape == nullptr || strides == nullptr) {
    return count > 0 ? count : 0;
  }
  int extent = 1;
  for (int d = 0; d < rank; ++d) {
    if (shape[d] <= 0) continue;
    extent += (shape[d] - 1) * strides[d];
  }
  if (extent < 1) extent = 1;
  return extent > count ? extent : count;
}

bool GatherStrided(const void* src, const int* shape, const int* strides, int rank,
                   int elem_bytes, void* dense, int dense_count) {
  if (src == nullptr || dense == nullptr || elem_bytes <= 0 || rank < 0) return false;
  if (rank == 0) return dense_count == 0;
  if (shape == nullptr || strides == nullptr) return false;
  int expect = 1;
  for (int d = 0; d < rank; ++d) {
    if (shape[d] <= 0) return false;
    expect *= shape[d];
  }
  if (expect != dense_count) return false;
  int packed = 1;
  bool contiguous = true;
  for (int d = rank - 1; d >= 0; --d) {
    if (strides[d] != packed) contiguous = false;
    packed *= shape[d];
  }
  if (contiguous) {
    std::memcpy(dense, src, static_cast<size_t>(dense_count) * static_cast<size_t>(elem_bytes));
    return true;
  }
  const auto* in = static_cast<const uint8_t*>(src);
  auto* out = static_cast<uint8_t*>(dense);
  std::vector<int> idx(static_cast<size_t>(rank), 0);
  for (int written = 0; written < dense_count; ++written) {
    int offset = 0;
    for (int d = 0; d < rank; ++d) offset += idx[static_cast<size_t>(d)] * strides[d];
    std::memcpy(out + static_cast<size_t>(written) * static_cast<size_t>(elem_bytes),
                in + static_cast<size_t>(offset) * static_cast<size_t>(elem_bytes),
                static_cast<size_t>(elem_bytes));
    int dim = rank - 1;
    while (dim >= 0) {
      ++idx[static_cast<size_t>(dim)];
      if (idx[static_cast<size_t>(dim)] < shape[dim]) break;
      idx[static_cast<size_t>(dim)] = 0;
      --dim;
    }
  }
  return true;
}

bool CopyStrided(const void* src, const int* shape, const int* src_strides, void* dst,
                 const int* dst_strides, int rank, int elem_bytes) {
  if (!src || !dst || !shape || !src_strides || !dst_strides || elem_bytes <= 0 || rank <= 0) {
    return false;
  }
  int count = 1;
  for (int d = 0; d < rank; ++d) {
    if (shape[d] <= 0) return false;
    count *= shape[d];
  }
  bool src_contig = true;
  bool dst_contig = true;
  bool same = true;
  int packed = 1;
  for (int d = rank - 1; d >= 0; --d) {
    if (src_strides[d] != packed) src_contig = false;
    if (dst_strides[d] != packed) dst_contig = false;
    if (src_strides[d] != dst_strides[d]) same = false;
    packed *= shape[d];
  }
  if (same && src_contig && dst_contig) {
    std::memcpy(dst, src, static_cast<size_t>(count) * static_cast<size_t>(elem_bytes));
    return true;
  }
  const auto* in = static_cast<const uint8_t*>(src);
  auto* out = static_cast<uint8_t*>(dst);
  std::vector<int> index(static_cast<size_t>(rank), 0);
  for (int written = 0; written < count; ++written) {
    int src_off = 0;
    int dst_off = 0;
    for (int d = 0; d < rank; ++d) {
      src_off += index[static_cast<size_t>(d)] * src_strides[d];
      dst_off += index[static_cast<size_t>(d)] * dst_strides[d];
    }
    std::memcpy(out + static_cast<size_t>(dst_off) * static_cast<size_t>(elem_bytes),
                in + static_cast<size_t>(src_off) * static_cast<size_t>(elem_bytes),
                static_cast<size_t>(elem_bytes));
    int dim = rank - 1;
    while (dim >= 0) {
      ++index[static_cast<size_t>(dim)];
      if (index[static_cast<size_t>(dim)] < shape[dim]) break;
      index[static_cast<size_t>(dim)] = 0;
      --dim;
    }
  }
  return true;
}

bool PackPayload(WirePack pack, const int32_t* codes, int frames, uint16_t first_seq,
                 int codebook_size, uint8_t* dst, size_t cap, size_t* out_len) {
  const int bytes = PayloadBytes(pack, frames);
  if (!codes || !dst || !out_len || bytes <= 0 || cap < static_cast<size_t>(bytes)) return false;
  for (int i = 0; i < frames * kCodebooks; ++i) {
    if (codes[i] < 0 || codes[i] >= codebook_size || codes[i] >= (1 << kTightBitsPerCode)) {
      return false;
    }
  }
  std::memset(dst, 0, static_cast<size_t>(bytes));
  if (pack == WirePack::kU16) {
    dst[0] = static_cast<uint8_t>(kPayloadVersion);
    dst[1] = static_cast<uint8_t>(frames);
    WriteU16(dst + 2, first_seq);
    for (int i = 0; i < frames * kCodebooks; ++i) {
      WriteU16(dst + kPayloadHeaderBytes + i * 2, static_cast<uint16_t>(codes[i]));
    }
  } else {
    int bit = 0;
    for (int i = 0; i < frames * kCodebooks; ++i) {
      WriteBits(dst, &bit, static_cast<uint32_t>(codes[i]), kTightBitsPerCode);
    }
  }
  *out_len = static_cast<size_t>(bytes);
  return true;
}

bool UnpackPayload(const uint8_t* src, size_t len, int codebook_size, WirePack* pack,
                   int* frames, uint16_t* first_seq, int32_t* codes) {
  if (!src || !pack || !frames || !first_seq || !codes) return false;
  WirePack found = WirePack::kU16;
  int n = 0;
  bool matched = false;
  // Lengths are distinct: tight 17/33/50/66, u16 28/52/76/100 bytes.
  for (int candidate : {1, 2, 3, 4}) {
    if (static_cast<size_t>(PayloadBytes(WirePack::kTight, candidate)) == len) {
      found = WirePack::kTight;
      n = candidate;
      matched = true;
      break;
    }
    if (static_cast<size_t>(PayloadBytes(WirePack::kU16, candidate)) == len) {
      found = WirePack::kU16;
      n = candidate;
      matched = true;
      break;
    }
  }
  if (!matched) return false;
  if (found == WirePack::kU16) {
    if (src[0] != kPayloadVersion || src[1] != n) return false;
    ReadU16(src + 2, first_seq);
    for (int i = 0; i < n * kCodebooks; ++i) {
      uint16_t v = 0;
      ReadU16(src + kPayloadHeaderBytes + i * 2, &v);
      if (v >= codebook_size) return false;
      codes[i] = v;
    }
  } else {
    *first_seq = 0;
    int bit = 0;
    for (int i = 0; i < n * kCodebooks; ++i) {
      const uint32_t v = ReadBits(src, &bit, kTightBitsPerCode);
      if (v >= static_cast<uint32_t>(codebook_size)) return false;
      codes[i] = static_cast<int32_t>(v);
    }
  }
  *pack = found;
  *frames = n;
  return true;
}

bool PackFrame(const int32_t* codes12, uint16_t seq, int codebook_size,
               uint8_t* dst, size_t cap, size_t* out_len) {
  return PackPayload(WirePack::kU16, codes12, 1, seq, codebook_size, dst, cap, out_len);
}

bool UnpackFrame(const uint8_t* src, size_t len, int codebook_size,
                 int32_t* codes12, uint16_t* seq) {
  WirePack pack = WirePack::kU16;
  int frames = 0;
  if (!UnpackPayload(src, len, codebook_size, &pack, &frames, seq, codes12)) return false;
  return pack == WirePack::kU16 && frames == 1;
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
