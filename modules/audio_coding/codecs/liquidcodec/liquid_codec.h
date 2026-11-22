/*
 * LiquidCodec glue for the macOS direct-call quality test.
 *
 * The Core ML encoder and the streaming decoder chunk live in
 * wilddolphin2025/liquidcodec-ane. This library does not convert either
 * model. It packetizes 12 GFSQ codes per 80 ms codec frame and drives
 * whatever chunk size the fetched manifest declares.
 *
 * Wire payload. Each code is in [0, 2016), which fits in 11 bits.
 * Information rate is 12 * log2(2016) * 12.5 ≈ 1646 bps.
 *
 * u16 (default), N frames in {1, 2, 3, 4}:
 *   u8  version = 1
 *   u8  frame_count = N
 *   u16 sequence of the first frame, little-endian
 *   N * 12 little-endian uint16 codes
 *   N=1 is 28 bytes (4 + 24). N=2 is 52. N=4 is 100.
 *   The sequence is kept so a missing packet can be counted in codec frames.
 *   RTP already has a sequence number; this one is the codec-frame index.
 *
 * tight, no payload header. RTP carries the sequence number and the timestamp,
 * and SDP fmtp `pack=tight` plus `fpp` fixes N. The length is also unique:
 *   N=1: 12*11 = 132 bits = 17 bytes (4 pad bits)
 *   N=2: 264 bits = 33 bytes
 *   N=4: 528 bits = 66 bytes
 * Bits are packed MSB first. Pad bits are zero.
 *
 * SDP clock rate is 48 kHz because NetEq only accepts 8/16/32/48 kHz.
 * The codec itself is 24 kHz mono. A packet of N frames is parsed into N
 * frames of 80 ms so each one stays inside NetEq's 120 ms limit.
 */

#ifndef MODULES_AUDIO_CODING_CODECS_LIQUIDCODEC_LIQUID_CODEC_H_
#define MODULES_AUDIO_CODING_CODECS_LIQUIDCODEC_LIQUID_CODEC_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace webrtc {
namespace liquid {

inline constexpr int kSampleRateHz = 24000;
inline constexpr int kWebRtcClockHz = 48000;
inline constexpr int kFrameSamples = 1920;  // 80 ms at 24 kHz
inline constexpr int kFrameMs = 80;
inline constexpr int kCodebooks = 12;
inline constexpr int kCodebookSize = 2016;  // 8*7*6*6
inline constexpr double kFramesPerSecond = 12.5;
inline constexpr int kPayloadVersion = 1;
inline constexpr int kPayloadHeaderBytes = 4;
inline constexpr int kU16BytesPerFrame = kCodebooks * 2;  // 24
inline constexpr int kPayloadBytes = kPayloadHeaderBytes + kU16BytesPerFrame;  // 28
inline constexpr int kTightBitsPerCode = 11;
inline constexpr int kIpHeaderBytes = 20;
inline constexpr int kUdpHeaderBytes = 8;
inline constexpr int kRtpHeaderBytes = 12;
inline constexpr int kSrtpTagBytes = 16;  // AEAD tag used by typical DTLS-SRTP

enum class WirePack { kU16, kTight };

int PayloadBytes(WirePack pack, int frames);
double PayloadBitrateBps(WirePack pack, int frames);
// payload + RTP + UDP + IPv4, and the same plus a 16-byte SRTP tag.
// packets_per_second is 12.5 / frames for LiquidCodec.
void OnWireBitrateBps(int payload_bytes, double packets_per_second,
                      double* ipv4_udp_rtp_bps, double* with_srtp_bps);
inline constexpr int kHidden = 512;
inline constexpr int kLatent = 48;
inline constexpr int kFft = 1280;
inline constexpr int kHop = 320;
inline constexpr int kBins = kFft / 2 + 1;  // 641
inline constexpr int kLinear = kBins * 2;   // 1282
inline constexpr int kUpsample = 6;         // 12.5 Hz -> 75 Hz
inline constexpr int kTrim = (kFft - kHop) / 2;  // 480 samples, 20 ms
inline constexpr char kPayloadName[] = "LIQUIDCODEC";
inline constexpr int kPreferredPayloadType = 96;

// Left context does not add delay. `new_frames` does: the sender must collect
// that many codec frames before the Core ML encoder can run, then the receiver
// collects the same number of RTP packets (paced one per 80 ms) before the
// decoder can run.
struct ChunkConfig {
  int sample_rate_hz = kSampleRateHz;
  int frame_samples = kFrameSamples;
  int codebooks = kCodebooks;
  int codebook_size = kCodebookSize;
  int ctx_frames = 20;
  int new_frames = 20;
  double frames_per_second = kFramesPerSecond;

  bool Ok() const;
  int WindowFrames() const { return ctx_frames + new_frames; }
};

struct LatencyReport {
  int encoder_lookahead_ms = 0;
  int sender_collect_ms = 0;
  int receiver_collect_ms = 0;
  int ola_holdback_ms = 0;
  int algorithmic_ms = 0;
  std::string summary;
};

struct ModelPaths;

LatencyReport DescribeLatency(const ChunkConfig& chunk, bool hidden_tail);
LatencyReport DescribeLatency(const ChunkConfig& chunk, bool hidden_tail,
                              const ModelPaths& paths);

double InformationBitrateBps(const ChunkConfig& chunk);
double PayloadBitrateBps();  // u16, one frame: 2800 bps

struct ModelPaths {
  std::string model_dir;
  std::string encoder_model = "lc_enc_ane_int8.mlmodelc";
  std::string decoder_model = "lc_dec_ane_int8.mlmodelc";
  std::string tail = "lc_tail_f32.bin";
  std::string decoder_kind = "hidden";  // "hidden" (plus CPU tail) or "pcm"
  std::string encoder_input = "audio";
  std::string encoder_output = "codes";
  std::string decoder_lat_input = "lat";
  std::string decoder_valid_input = "valid";
  std::string decoder_output = "hidden";
  // "ctx+new": each encoder call sees left context plus new audio.
  // "new": each call sees only the new audio. With encoder_kind stateful_io
  // that audio is one frame plus lookahead_samples, and the model keeps state.
  std::string encoder_window = "ctx+new";
  // "codebook_time" (default) or "time_codebook", used only when both axes
  // have the same length.
  std::string encoder_codes_layout = "codebook_time";
  std::string compute = "ane";  // ane, cpu, all
  // Empty or "window": stateless chunk. "stateful_io": one-frame step plus
  // the state tensors named below. lookahead_samples > 0 also selects it.
  std::string encoder_kind;
  int lookahead_samples = 0;
  int stft_lookback_samples = 0;
  int encoder_input_samples = 0;
  int encoder_cache_frames = 0;
  std::vector<std::string> encoder_states_in;
  std::vector<std::string> encoder_states_out;

  bool Stateful() const {
    return encoder_kind == "stateful_io" || lookahead_samples > 0;
  }
  int EncoderStepSamples(int frame_samples) const {
    if (encoder_input_samples > 0) return encoder_input_samples;
    return frame_samples + lookahead_samples;
  }
};

// Flat JSON object. Unknown keys are ignored. Missing keys keep `defaults`.
bool ParseManifest(const std::string& json,
                   ModelPaths* paths,
                   ChunkConfig* chunk,
                   std::string* error);

bool LoadManifestFile(const std::string& path,
                      ModelPaths* paths,
                      ChunkConfig* chunk,
                      std::string* error);

std::string JoinPath(const std::string& dir, const std::string& leaf);
bool FileExists(const std::string& path);
// Core ML loads .mlmodelc. A .mlpackage path maps to the sibling .mlmodelc
// (same directory, suffix replaced). Any other path is returned unchanged.
std::string CompiledModelPath(const std::string& path);

// Copy negotiated SDP fmtp onto pack and frames-per-packet. A difference is
// written into `mismatch` and the negotiated value wins. Returns false when
// the negotiated values differ or are invalid.
bool ApplyNegotiatedPack(const std::string& pack, const std::string& fpp,
                         WirePack* wire_pack, int* frames_per_packet,
                         std::string* mismatch);

// Nearest index in [0, codebook_size). Values within 1.0 outside that range
// are int8/fp16 rounding and are clamped. Farther values are rejected.
// `clamped` is set when the raw float was outside the legal integers.
bool QuantizeCode(float raw, int codebook_size, int32_t* quantized, bool* clamped);

// Swap an _int8 / _fp16 / _fp32 token in an encoder path. Other names are unchanged.
std::string EncoderModelForPrecision(const std::string& model, const std::string& precision);

// Process-wide counts for one quality run. Core ML increments them. Consume
// returns the count and resets it so the next run starts at zero.
void NoteOutOfCodebook();
void NoteClampedCode();
int ConsumeOutOfCodebook();
int ConsumeClampedCodes();

// getStats headerBytes is the RTP header only, and some builds report a value
// equal to the payload. Use it when it is 8..20 bytes/packet, otherwise 12.
// Transport is packets * (20 IPv4 + 8 UDP), plus 16 when SRTP is on, plus 20
// when the IP header is IPv6. Result is kbps over `seconds`.
uint64_t RtpHeaderBytesForOnWire(uint64_t reported_header_bytes, uint64_t packets);
double OnWireKbps(uint64_t payload_bytes, uint64_t reported_header_bytes, uint64_t packets,
                  double seconds, bool encrypted, bool ipv6);

// Elements a strided tensor occupies. `count` is the dense product of the
// shape. ANE MLMultiArray strides are often padded, so the last index is
// past `count`. extent = 1 + sum((shape[d] - 1) * strides[d]), and the
// buffer must hold max(extent, count) elements.
int StridedElementCount(const int* shape, const int* strides, int rank, int count);
// Copy a strided tensor into a dense buffer of `dense_count` elements.
// Contiguous strides are a memcpy. Returns false when the shape product
// is not `dense_count`.
bool GatherStrided(const void* src, const int* shape, const int* strides, int rank,
                   int elem_bytes, void* dense, int dense_count);
// Copy one logical tensor between two strided buffers of the same shape.
bool CopyStrided(const void* src, const int* shape, const int* src_strides, void* dst,
                 const int* dst_strides, int rank, int elem_bytes);

bool PackFrame(const int32_t* codes12, uint16_t seq, int codebook_size,
               uint8_t* dst, size_t cap, size_t* out_len);
bool UnpackFrame(const uint8_t* src, size_t len, int codebook_size,
                 int32_t* codes12, uint16_t* seq);
// `codes` is frame-major, `frames` * 12. Tight ignores `first_seq` on pack
// and returns 0 from unpack. The format is recognized from the length.
bool PackPayload(WirePack pack, const int32_t* codes, int frames, uint16_t first_seq,
                 int codebook_size, uint8_t* dst, size_t cap, size_t* out_len);
bool UnpackPayload(const uint8_t* src, size_t len, int codebook_size, WirePack* pack,
                   int* frames, uint16_t* first_seq, int32_t* codes);

// codes: frame-major, `frames` * codebooks. lat: channel-major (48, frames).
void CodesToLatent(const int32_t* codes, int frames, int codebooks, float* lat);

struct CodecStats {
  int64_t packets = 0;
  int64_t payload_bytes = 0;
  int64_t samples_24k = 0;
  int packet_bytes_min = 0;
  int packet_bytes_max = 0;
  std::vector<double> window_ms;
  std::vector<double> prediction_ms;
  double load_ms = 0;
  std::string compute;
  std::string backend;
  ChunkConfig chunk;
  bool hidden_tail = true;
  int lookahead_samples = 0;
  bool stateful_encoder = false;

  void AddPacket(int bytes);
  void AddWindow(double window, double prediction);
  double PayloadBitrateBps() const;
  double Mean(const std::vector<double>& v) const;
  std::string ToJson() const;
  bool WriteJson(const std::string& path) const;
};

class WavWriter {
 public:
  WavWriter() = default;
  ~WavWriter();
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  bool Open(const std::string& path, int sample_rate_hz, std::string* error);
  void Write(const int16_t* pcm, size_t n);
  void Close();
  bool ok() const { return file_ != nullptr; }

 private:
  void* file_ = nullptr;
  int rate_ = 0;
  uint32_t frames_ = 0;
  std::string path_;
};

struct WavData {
  int sample_rate_hz = 0;
  int channels = 0;
  std::vector<int16_t> mono;
};

bool ReadWav(const std::string& path, WavData* out, std::string* error);

// Emits one 80 ms packet per 80 ms of input once codes for that frame exist.
// Codes that a chunked model returns in a burst stay queued so NetEq sees a
// steady 80 ms stream instead of a multi-second burst.
struct PacedPacket {
  uint32_t timestamp = 0;
  std::array<int32_t, kCodebooks> codes{};
};

class FramePacer {
 public:
  explicit FramePacer(int frames_per_packet_10ms = 8);

  std::optional<PacedPacket> Tick(uint32_t rtp_timestamp,
                                  const int32_t* new_codes,
                                  int nframes);
  void Reset();
  int queued() const { return static_cast<int>(slots_.size()); }

 private:
  struct Slot {
    uint32_t timestamp = 0;
    bool ready = false;
    std::array<int32_t, kCodebooks> codes{};
  };
  int ticks_per_frame_;
  int ticks_ = 0;
  uint32_t frame_ts_ = 0;
  std::vector<Slot> slots_;
};

}  // namespace liquid
}  // namespace webrtc

#endif  // MODULES_AUDIO_CODING_CODECS_LIQUIDCODEC_LIQUID_CODEC_H_
