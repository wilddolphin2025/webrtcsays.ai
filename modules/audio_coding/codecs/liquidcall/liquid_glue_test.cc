#include <sys/stat.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "examples/direct/impair_schedule.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_codec.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_stream.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_tail.h"

namespace {

int g_fails = 0;

void Expect(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "ok  " << what << "\n";
  } else {
    std::cout << "FAIL " << what << "\n";
    ++g_fails;
  }
}

class MockBackend final : public webrtc::liquid::LiquidBackend {
 public:
  bool Start(const webrtc::liquid::ModelPaths&, const webrtc::liquid::ChunkConfig&,
             std::string*) override {
    return true;
  }
  bool EncodeWindow(const int16_t* pcm, const webrtc::liquid::ChunkConfig& chunk,
                    int32_t* codes_out, webrtc::liquid::WindowTiming* timing,
                    std::string*) override {
    const int16_t* news = pcm + chunk.ctx_frames * chunk.frame_samples;
    for (int f = 0; f < chunk.new_frames; ++f) {
      for (int q = 0; q < webrtc::liquid::kCodebooks; ++q) {
        const int sample = news[f * chunk.frame_samples];
        codes_out[f * webrtc::liquid::kCodebooks + q] =
            (sample + 1000 + q) % chunk.codebook_size;
      }
    }
    if (timing) {
      timing->window_ms = 1.5;
      timing->prediction_ms = 0.25;
    }
    ++encodes_;
    return true;
  }
  bool DecodeWindow(const int32_t* codes, const webrtc::liquid::ChunkConfig& chunk,
                    int16_t* pcm_out, webrtc::liquid::WindowTiming* timing,
                    std::string*) override {
    const int32_t* news = codes + chunk.ctx_frames * webrtc::liquid::kCodebooks;
    for (int f = 0; f < chunk.new_frames; ++f) {
      const int16_t value = static_cast<int16_t>(news[f * webrtc::liquid::kCodebooks]);
      for (int i = 0; i < chunk.frame_samples; ++i) {
        pcm_out[f * chunk.frame_samples + i] = value;
      }
    }
    if (timing) {
      timing->window_ms = 2.0;
      timing->prediction_ms = 0.5;
    }
    ++decodes_;
    return true;
  }
  void Reset() override {}
  const char* name() const override { return "mock"; }
  int encodes_ = 0;
  int decodes_ = 0;
};

void TestPayload() {
  int32_t codes[12];
  for (int i = 0; i < 12; ++i) codes[i] = 100 + i;
  uint8_t bytes[64];
  size_t len = 0;
  Expect(webrtc::liquid::PackFrame(codes, 7, 2016, bytes, sizeof(bytes), &len), "pack");
  Expect(len == 28, "payload is 28 bytes");
  int32_t back[12];
  uint16_t seq = 0;
  Expect(webrtc::liquid::UnpackFrame(bytes, len, 2016, back, &seq), "unpack");
  Expect(seq == 7, "sequence");
  Expect(std::memcmp(codes, back, sizeof(codes)) == 0, "codes roundtrip");
  codes[0] = 2016;
  Expect(!webrtc::liquid::PackFrame(codes, 1, 2016, bytes, sizeof(bytes), &len),
         "reject code 2016");

  Expect(webrtc::liquid::PayloadBytes(webrtc::liquid::WirePack::kU16, 1) == 28, "u16 n=1 is 28");
  Expect(webrtc::liquid::PayloadBytes(webrtc::liquid::WirePack::kU16, 2) == 52, "u16 n=2 is 52");
  Expect(webrtc::liquid::PayloadBytes(webrtc::liquid::WirePack::kU16, 4) == 100, "u16 n=4 is 100");
  Expect(webrtc::liquid::PayloadBytes(webrtc::liquid::WirePack::kTight, 1) == 17, "tight n=1 is 17");
  Expect(webrtc::liquid::PayloadBytes(webrtc::liquid::WirePack::kTight, 2) == 33, "tight n=2 is 33");
  Expect(webrtc::liquid::PayloadBytes(webrtc::liquid::WirePack::kTight, 4) == 66, "tight n=4 is 66");
  Expect(std::fabs(webrtc::liquid::PayloadBitrateBps(webrtc::liquid::WirePack::kTight, 1) - 1700.0) <
             0.1,
         "tight one-frame payload is 1700 bps");
  Expect(std::fabs(webrtc::liquid::PayloadBitrateBps(webrtc::liquid::WirePack::kTight, 2) - 1650.0) <
             0.1,
         "tight two-frame payload is 1650 bps");
  double plain = 0;
  double srtp = 0;
  webrtc::liquid::OnWireBitrateBps(17, 12.5, webrtc::liquid::kSrtpTagSha1_32Bytes, &plain,
                                   &srtp);
  Expect(std::fabs(plain - 5700.0) < 0.1, "tight n=1 IPv4/UDP/RTP is 5700 bps");
  Expect(std::fabs(srtp - 6100.0) < 0.1, "tight n=1 with 4-byte SHA1_32 SRTP tag is 6100 bps");
  webrtc::liquid::OnWireBitrateBps(28, 12.5, webrtc::liquid::kSrtpTagSha1_32Bytes, &plain,
                                   &srtp);
  Expect(std::fabs(srtp - 7200.0) < 0.1, "u16 n=1 with 4-byte SHA1_32 SRTP tag is 7200 bps");
  webrtc::liquid::OnWireBitrateBps(28, 12.5, webrtc::liquid::kSrtpTagSha1_80Bytes, &plain,
                                   &srtp);
  Expect(std::fabs(srtp - 7800.0) < 0.1, "u16 n=1 with 10-byte SHA1_80 SRTP tag is 7800 bps");

  for (int n : {1, 2, 3, 4}) {
    for (auto pack : {webrtc::liquid::WirePack::kU16, webrtc::liquid::WirePack::kTight}) {
      std::vector<int32_t> many(static_cast<size_t>(n * 12));
      for (int i = 0; i < n * 12; ++i) many[static_cast<size_t>(i)] = (i * 17 + 2015) % 2016;
      uint8_t buf[128];
      size_t out = 0;
      Expect(webrtc::liquid::PackPayload(pack, many.data(), n, 9, 2016, buf, sizeof(buf), &out),
             "pack multi");
      Expect(out == static_cast<size_t>(webrtc::liquid::PayloadBytes(pack, n)), "packed size");
      int32_t back_n[4 * 12];
      webrtc::liquid::WirePack got = webrtc::liquid::WirePack::kU16;
      int frames = 0;
      uint16_t seq_n = 99;
      Expect(webrtc::liquid::UnpackPayload(buf, out, 2016, &got, &frames, &seq_n, back_n),
             "unpack multi");
      Expect(got == pack && frames == n, "detected pack and frame count");
      const bool exact = std::memcmp(many.data(), back_n, many.size() * sizeof(int32_t)) == 0 &&
                         got == pack && frames == n;
      Expect(exact, "multi-frame codes roundtrip");
      std::cout << "QUALITY_PACK pack=" << (pack == webrtc::liquid::WirePack::kU16 ? "u16" : "tight")
                << " fpp=" << n << " bit_exact=" << (exact ? 1 : 0) << " bytes=" << out << "\n";
      if (pack == webrtc::liquid::WirePack::kU16) Expect(seq_n == 9, "u16 keeps the first sequence");
      if (pack == webrtc::liquid::WirePack::kTight) Expect(seq_n == 0, "tight has no sequence");
    }
  }
  int32_t edge[12];
  for (int i = 0; i < 12; ++i) edge[i] = (i % 2) ? 2015 : 0;
  uint8_t tight[32];
  size_t tight_len = 0;
  Expect(webrtc::liquid::PackPayload(webrtc::liquid::WirePack::kTight, edge, 1, 0, 2016, tight,
                                     sizeof(tight), &tight_len),
         "pack edge codes");
  int32_t edge_back[12];
  webrtc::liquid::WirePack edge_pack = webrtc::liquid::WirePack::kU16;
  int edge_n = 0;
  uint16_t edge_seq = 1;
  Expect(webrtc::liquid::UnpackPayload(tight, tight_len, 2016, &edge_pack, &edge_n, &edge_seq,
                                       edge_back),
         "unpack edge");
  Expect(std::memcmp(edge, edge_back, sizeof(edge)) == 0, "0 and 2015 survive 11-bit packing");
}

void TestLatency() {
  webrtc::liquid::ChunkConfig chunk;
  chunk.new_frames = 20;
  chunk.ctx_frames = 20;
  const auto big = webrtc::liquid::DescribeLatency(chunk, true);
  Expect(big.sender_collect_ms == 1600, "new=20 sender collect 1600 ms");
  Expect(big.receiver_collect_ms == 1520, "new=20 receiver collect 1520 ms");
  Expect(big.ola_holdback_ms == 20, "tail holdback 20 ms");
  Expect(big.algorithmic_ms == 3140, "new=20 algorithmic 3140 ms");
  chunk.new_frames = 1;
  const auto small = webrtc::liquid::DescribeLatency(chunk, true);
  Expect(small.sender_collect_ms == 80, "new=1 sender collect 80 ms");
  Expect(small.receiver_collect_ms == 0, "new=1 receiver collect 0");
  Expect(small.algorithmic_ms == 100, "new=1 algorithmic 100 ms");
  const auto pcm = webrtc::liquid::DescribeLatency(chunk, false);
  Expect(pcm.ola_holdback_ms == 0, "pcm decoder has no tail holdback");
  webrtc::liquid::ModelPaths step;
  step.encoder_kind = "stateful_io";
  step.lookahead_samples = 480;
  step.encoder_input_samples = 2400;
  chunk.new_frames = 1;
  const auto live = webrtc::liquid::DescribeLatency(chunk, true, step);
  Expect(live.encoder_lookahead_ms == 20, "encoder lookahead is 20 ms");
  Expect(live.sender_collect_ms == 100, "new=1 sender collect includes lookahead");
  Expect(live.receiver_collect_ms == 0, "new=1 stateful receiver collect");
  Expect(live.algorithmic_ms == 120, "new=1 stateful algorithmic latency is 120 ms");
  chunk.new_frames = 4;
  const auto wider = webrtc::liquid::DescribeLatency(chunk, true, step);
  Expect(wider.algorithmic_ms == 360, "new=4 stateful latency is 360 ms");
  const double info = webrtc::liquid::InformationBitrateBps(chunk);
  Expect(info > 1640.0 && info < 1650.0, "information bitrate about 1646 bps");
  Expect(std::fabs(webrtc::liquid::PayloadBitrateBps() - 2800.0) < 0.1,
         "payload bitrate 2800 bps");
}

void TestManifest() {
  const char* json = R"({
    "ctx_frames": 20,
    "new_frames": 1,
    "encoder_model": "lc_enc_ane_int8.mlmodelc",
    "decoder_model": "lc_dec_ane_int8.mlmodelc",
    "decoder_kind": "hidden",
    "encoder_window": "ctx+new",
    "compute": "cpu"
  })";
  webrtc::liquid::ModelPaths paths;
  webrtc::liquid::ChunkConfig chunk;
  std::string error;
  Expect(webrtc::liquid::ParseManifest(json, &paths, &chunk, &error), error);
  Expect(chunk.ctx_frames == 20 && chunk.new_frames == 1, "manifest chunk");
  Expect(paths.encoder_model == "lc_enc_ane_int8.mlmodelc", "manifest encoder");
  Expect(paths.compute == "cpu", "manifest compute");
  Expect(!webrtc::liquid::ParseManifest("{\"decoder_kind\": \"nope\"}", &paths, &chunk, &error),
         "reject bad decoder_kind");
  const char* fork = R"({
    "ctx_frames": 20,
    "new_frames": 1,
    "encoder_window": "new",
    "encoder_kind": "stateful_io",
    "lookahead_samples": 480,
    "encoder_input_samples": 2400,
    "encoder_cache_frames": 4096,
    "encoder_model": "stream/lc_enc_step_int8.mlpackage",
    "encoder_states_in": ["hist", "k_cache", "v_cache", "conv_state", "pos"],
    "encoder_states_out": ["hist_out", "k_cache_out", "v_cache_out", "conv_state_out", "pos_out"]
  })";
  webrtc::liquid::ModelPaths live;
  webrtc::liquid::ChunkConfig live_chunk;
  Expect(webrtc::liquid::ParseManifest(fork, &live, &live_chunk, &error), error);
  Expect(live.Stateful() && live.lookahead_samples == 480, "fork manifest is stateful");
  Expect(live.encoder_input_samples == 2400, "fork step is 2400 samples");
  Expect(live.encoder_states_in.size() == 5 && live.encoder_states_in[0] == "hist",
         "fork state inputs");
  Expect(live.encoder_states_out.back() == "pos_out", "fork state outputs");
  Expect(live_chunk.new_frames == 1 && live_chunk.ctx_frames == 20, "fork decoder chunk");
  Expect(webrtc::liquid::CompiledModelPath("stream/lc_enc_step_int8.mlpackage") ==
             "stream/lc_enc_step_int8.mlmodelc",
         "mlpackage compiles to a sibling mlmodelc");
  Expect(webrtc::liquid::CompiledModelPath("stream/lc_dec_n1_int8.mlmodelc") ==
             "stream/lc_dec_n1_int8.mlmodelc",
         "mlmodelc path is unchanged");
  Expect(webrtc::liquid::CompiledModelPath("stream/not-a-model") == "stream/not-a-model",
         "other paths are unchanged");
  webrtc::liquid::WirePack pack = webrtc::liquid::WirePack::kU16;
  int fpp = 1;
  std::string mismatch;
  Expect(!webrtc::liquid::ApplyNegotiatedPack("tight", "2", &pack, &fpp, &mismatch),
         "negotiated pack differs");
  Expect(pack == webrtc::liquid::WirePack::kTight && fpp == 2,
         "negotiated pack and fpp replace the local flags");
  Expect(mismatch.find("using negotiated") != std::string::npos, "mismatch is logged");
  int32_t code = -1;
  bool clamped = false;
  Expect(webrtc::liquid::QuantizeCode(2015.6f, 2016, &code, &clamped) && clamped && code == 2015,
         "int8 2015.6 clamps to 2015");
  Expect(webrtc::liquid::QuantizeCode(2016.0f, 2016, &code, &clamped) && clamped && code == 2015,
         "int8 2016.0 clamps to the last index");
  Expect(!webrtc::liquid::QuantizeCode(2018.f, 2016, &code, &clamped),
         "a code far outside the codebook is rejected");
  Expect(webrtc::liquid::QuantizeCode(10.2f, 2016, &code, &clamped) && !clamped && code == 10,
         "an in-range code is not clamped");
  Expect(webrtc::liquid::EncoderModelForPrecision("stream/lc_enc_step_int8.mlmodelc", "fp16") ==
             "stream/lc_enc_step_fp16.mlmodelc",
         "fp16 swaps the int8 encoder name");
  Expect(webrtc::liquid::EncoderModelForPrecision("stream/lc_enc_step_int8.mlmodelc", "fp32") ==
             "stream/lc_enc_step_fp32.mlmodelc",
         "fp32 swaps the int8 encoder name");
  Expect(std::fabs(webrtc::liquid::OnWireKbps(28, 12, 1, 0.08, false, false) - 6.8) < 0.01,
         "one u16 frame is 6.8 kbps IPv4/UDP/RTP");
  Expect(webrtc::liquid::RtpHeaderBytesForOnWire(28, 1) == 12,
         "a header equal to the payload is not an RTP header");
}

void TestPaddedStrides() {
  // Decoder hidden on the ANE: shape [1, 512, 1575], time stride padded to 1600.
  // Dense count is 806400. The last index is 819174, which libc++ rejects if
  // ReadFloats only copies `count` elements.
  const int shape[] = {1, 512, 1575};
  const int strides[] = {819200, 1600, 1};
  const int count = 1 * 512 * 1575;
  const int last = 511 * 1600 + 1574;
  const int n = webrtc::liquid::StridedElementCount(shape, strides, 3, count);
  Expect(n == last + 1, "padded extent is one past the last strided index");
  Expect(last >= count, "last ANE index is past the dense count");
  std::vector<float> dense(static_cast<size_t>(count), 0.f);
  Expect(last >= static_cast<int>(dense.size()), "dense copy would be out of bounds");
  std::vector<float> padded(static_cast<size_t>(n), 0.f);
  padded[static_cast<size_t>(last)] = 3.5f;
  Expect(padded[static_cast<size_t>(last)] == 3.5f, "padded buffer holds the last index");

  const int packed_strides[] = {512 * 1575, 1575, 1};
  Expect(webrtc::liquid::StridedElementCount(shape, packed_strides, 3, count) == count,
         "packed strides stay at the dense count");

  // State feedback: a padded [2, 3] tensor gathers into a dense 6.
  const int st_shape[] = {2, 3};
  const int st_strides[] = {8, 1};
  float src[16] = {};
  src[0] = 1.f;
  src[1] = 2.f;
  src[2] = 3.f;
  src[8] = 4.f;
  src[9] = 5.f;
  src[10] = 6.f;
  float dst[6] = {};
  Expect(webrtc::liquid::GatherStrided(src, st_shape, st_strides, 2, sizeof(float), dst, 6),
         "gather padded state");
  Expect(dst[0] == 1.f && dst[1] == 2.f && dst[2] == 3.f && dst[3] == 4.f && dst[4] == 5.f &&
             dst[5] == 6.f,
         "padded state lands densely in order");
  float dense_src[6] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
  const int dense_strides[] = {3, 1};
  float dense_dst[6] = {};
  Expect(webrtc::liquid::GatherStrided(dense_src, st_shape, dense_strides, 2, sizeof(float),
                                      dense_dst, 6),
         "gather packed state");
  Expect(dense_dst[5] == 6.f, "packed state is copied");
}

void TestStatefulHold() {
  webrtc::liquid::ChunkConfig chunk;
  chunk.new_frames = 1;
  chunk.ctx_frames = 20;
  webrtc::liquid::ModelPaths paths;
  paths.encoder_kind = "stateful_io";
  paths.lookahead_samples = 480;
  paths.encoder_input_samples = 2400;
  class HoldMock final : public webrtc::liquid::LiquidBackend {
   public:
    bool Start(const webrtc::liquid::ModelPaths&, const webrtc::liquid::ChunkConfig&,
               std::string*) override {
      return true;
    }
    bool EncodeWindow(const int16_t* pcm, const webrtc::liquid::ChunkConfig&, int32_t* codes_out,
                      webrtc::liquid::WindowTiming*, std::string*) override {
      last.assign(pcm, pcm + 2400);
      ++calls;
      for (int q = 0; q < 12; ++q) codes_out[q] = 3;
      return true;
    }
    bool DecodeWindow(const int32_t*, const webrtc::liquid::ChunkConfig&, int16_t*,
                      webrtc::liquid::WindowTiming*, std::string*) override {
      return true;
    }
    void Reset() override {}
    const char* name() const override { return "hold"; }
    int calls = 0;
    std::vector<int16_t> last;
  } backend;
  webrtc::liquid::StreamEncoder encoder(&backend, chunk, paths);
  std::vector<int16_t> audio(2400);
  for (int i = 0; i < 2400; ++i) audio[static_cast<size_t>(i)] = static_cast<int16_t>(i);
  std::vector<std::array<int32_t, 12>> codes;
  webrtc::liquid::WindowTiming timing;
  std::string error;
  Expect(encoder.Push(audio.data(), 2399, &codes, &timing, &error) && codes.empty() &&
             backend.calls == 0,
         "stateful encoder waits for the 480-sample lookahead");
  Expect(encoder.Push(audio.data() + 2399, 1, &codes, &timing, &error), "lookahead arrived");
  Expect(backend.calls == 1 && codes.size() == 1, "one frame encoded, no reflect pad");
  Expect(backend.last.size() == 2400 && backend.last[0] == 0 && backend.last[1919] == 1919 &&
             backend.last[1920] == 1920 && backend.last[2399] == 2399,
         "step is the real frame plus the next 480 samples");
  std::vector<int16_t> more(1920);
  for (int i = 0; i < 1920; ++i) more[static_cast<size_t>(i)] = static_cast<int16_t>(2400 + i);
  codes.clear();
  Expect(encoder.Push(more.data(), more.size(), &codes, &timing, &error), "second frame");
  Expect(backend.calls == 2 && backend.last[0] == 1920 && backend.last[479] == 2399 &&
             backend.last[480] == 2400,
         "next step keeps the previous lookahead as the frame start");
}

void TestLatent() {
  int32_t codes[12] = {};
  codes[0] = 1;
  float lat[48];
  webrtc::liquid::CodesToLatent(codes, 1, 12, lat);
  Expect(std::fabs(lat[0] - (-0.75f)) < 1e-6, "code 1 dim0");
  Expect(std::fabs(lat[1] - (-1.f)) < 1e-6, "code 1 dim1");
  int32_t zeros[12] = {};
  webrtc::liquid::CodesToLatent(zeros, 1, 12, lat);
  bool all_neg = true;
  for (float v : lat) {
    if (std::fabs(v - (-1.f)) > 1e-6) all_neg = false;
  }
  Expect(all_neg, "code 0 is the bottom FSQ level");
}

void TestPacer() {
  webrtc::liquid::FramePacer pacer(8);
  webrtc::liquid::ChunkConfig chunk;
  chunk.new_frames = 4;
  chunk.ctx_frames = 2;
  MockBackend backend;
  webrtc::liquid::StreamEncoder encoder(&backend, chunk);
  int emitted = 0;
  uint32_t first_ts = 0;
  bool have_first = false;
  uint32_t prev_ts = 0;
  for (int frame = 0; frame < 4; ++frame) {
    for (int tick = 0; tick < 8; ++tick) {
      const uint32_t ts = static_cast<uint32_t>((frame * 8 + tick) * 480);
      std::vector<std::array<int32_t, 12>> codes;
      webrtc::liquid::WindowTiming timing;
      std::string error;
      if (tick == 7) {
        std::vector<int16_t> frame_pcm(1920, static_cast<int16_t>(1000 + frame));
        Expect(encoder.Push(frame_pcm.data(), frame_pcm.size(), &codes, &timing, &error),
               "stream push");
      }
      std::vector<int32_t> flat;
      for (const auto& c : codes) flat.insert(flat.end(), c.begin(), c.end());
      const auto packet = pacer.Tick(ts, flat.empty() ? nullptr : flat.data(),
                                     static_cast<int>(codes.size()));
      if (packet) {
        if (!have_first) {
          first_ts = packet->timestamp;
          have_first = true;
        } else {
          Expect(packet->timestamp == prev_ts + 3840, "paced timestamps step 80 ms");
        }
        prev_ts = packet->timestamp;
        ++emitted;
      } else if (frame < 3) {
        Expect(codes.empty(), "no codes before the chunk completes");
      }
    }
  }
  Expect(backend.encodes_ == 1, "one encode window for four frames");
  Expect(emitted == 1, "first packet leaves when the chunk is ready");
  Expect(have_first && first_ts == 0, "first packet keeps the first frame timestamp");
  for (int frame = 0; frame < 3; ++frame) {
    for (int tick = 0; tick < 8; ++tick) {
      const uint32_t ts = static_cast<uint32_t>((40 + frame * 8 + tick) * 480);
      const auto packet = pacer.Tick(ts, nullptr, 0);
      if (packet) {
        Expect(packet->timestamp == prev_ts + 3840, "drain timestamp");
        prev_ts = packet->timestamp;
        ++emitted;
      }
    }
  }
  Expect(emitted == 4, "four paced packets for four frames");
}

void TestDecodeChunk() {
  webrtc::liquid::ChunkConfig chunk;
  chunk.new_frames = 2;
  chunk.ctx_frames = 3;
  MockBackend backend;
  webrtc::liquid::StreamDecoder decoder(&backend, chunk);
  std::vector<int16_t> audio;
  webrtc::liquid::WindowTiming timing;
  std::string error;
  int32_t frame[12] = {};
  frame[0] = 42;
  Expect(decoder.PushFrame(frame, &audio, &timing, &error) && audio.empty(),
         "decoder waits for the chunk");
  frame[0] = 43;
  Expect(decoder.PushFrame(frame, &audio, &timing, &error), "second frame decodes");
  Expect(audio.size() == static_cast<size_t>(2 * 1920), "chunk audio length");
  Expect(audio.front() == 42 && audio[1920] == 43, "decoded frames stay in order");
  Expect(backend.decodes_ == 1, "one decode window");
}

void TestWavAndTail(const std::string& dir) {
  const std::string wav_path = dir + "/round.wav";
  webrtc::liquid::WavWriter writer;
  std::string error;
  Expect(writer.Open(wav_path, 24000, &error), "wav open");
  int16_t tone[480];
  for (int i = 0; i < 480; ++i) tone[i] = static_cast<int16_t>(i - 240);
  writer.Write(tone, 480);
  writer.Close();
  webrtc::liquid::WavData data;
  Expect(webrtc::liquid::ReadWav(wav_path, &data, &error), error.empty() ? "wav read" : error);
  Expect(data.sample_rate_hz == 24000 && data.mono.size() == 480, "wav shape");
  Expect(data.mono[10] == tone[10], "wav sample");

  constexpr int n75 = 8;
  std::vector<float> y(static_cast<size_t>(webrtc::liquid::kLinear * n75), -8.f);
  for (int t = 0; t < n75; ++t) {
    y[static_cast<size_t>(0 * n75 + t)] = 0.f;
    y[static_cast<size_t>(1 * n75 + t)] = 0.f;
    y[static_cast<size_t>(webrtc::liquid::kBins * n75 + t)] = 0.f;
    y[static_cast<size_t>((webrtc::liquid::kBins + 1) * n75 + t)] = 0.5f;
  }
  webrtc::liquid::LiquidTail tail;
  auto pcm = tail.PushLinear(y.data(), n75);
  auto rest = tail.Finalize();
  pcm.insert(pcm.end(), rest.begin(), rest.end());
  const std::string bin = dir + "/tail_case.bin";
  FILE* f = std::fopen(bin.c_str(), "wb");
  Expect(f != nullptr, "tail case file");
  if (f) {
    const int32_t n = n75;
    const int32_t np = static_cast<int32_t>(pcm.size());
    std::fwrite(&n, 4, 1, f);
    std::fwrite(y.data(), sizeof(float), y.size(), f);
    std::fwrite(&np, 4, 1, f);
    std::fwrite(pcm.data(), sizeof(float), pcm.size(), f);
    std::fclose(f);
  }
  Expect(pcm.size() == static_cast<size_t>(n75 * webrtc::liquid::kHop),
         "tail length matches trimmed ISTFT");
}

}  // namespace

void TestImpairSchedule() {
  using webrtc::quality::DelayMs;
  using webrtc::quality::DropPacket;
  Expect(!DropPacket(0, 0) && DelayMs(2, 0) == 0, "zero impair is identity");
  Expect(DropPacket(0, 10) && DropPacket(9, 10) && !DropPacket(10, 10),
         "10% drops the first 10 packets of each 100");
  Expect(DelayMs(0, 20) == 0 && DelayMs(1, 20) == 10 && DelayMs(2, 20) == 20 &&
             DelayMs(3, 20) == 0,
         "jitter delay cycles 0, half, full");
  int drops = 0;
  for (int i = 0; i < 100; ++i) drops += DropPacket(i, 3) ? 1 : 0;
  Expect(drops == 3, "3% drops 3 of every 100 packets");
}

int main() {
  const char* dir = std::getenv("LIQUID_TEST_DIR");
  const std::string out = dir ? dir : "scripts/liquidcall/out";
  mkdir(out.c_str(), 0755);
  TestPayload();
  TestImpairSchedule();
  TestLatency();
  TestManifest();
  TestPaddedStrides();
  TestStatefulHold();
  TestLatent();
  TestPacer();
  TestDecodeChunk();
  TestWavAndTail(out);
  if (g_fails) {
    std::cout << g_fails << " failed\n";
    return 1;
  }
  std::cout << "all glue tests passed\n";
  return 0;
}
