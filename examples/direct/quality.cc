#include "quality.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "api/audio/audio_frame.h"
#include "api/audio/audio_view.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/audio_format.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/environment/environment_factory.h"
#include "api/neteq/default_neteq_factory.h"
#include "api/neteq/neteq.h"
#include "api/rtp_headers.h"
#include "api/rtp_packet_info.h"
#include "api/units/timestamp.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "examples/direct/impair_schedule.h"
#include "rtc_base/buffer.h"
#include "system_wrappers/include/clock.h"

#if defined(WEBRTC_USE_LIQUIDCODEC)
#include "modules/audio_coding/codecs/liquidcodec/liquid_backend.h"
#include "modules/audio_coding/codecs/liquidcodec/liquid_codec.h"
#include "modules/audio_coding/codecs/liquidcodec/liquid_factory.h"
#include "modules/audio_coding/codecs/liquidcodec/liquid_stream.h"
#endif

namespace {
struct Pcm {
  int rate = 0;
  std::vector<int16_t> mono;
};

bool ReadWav(const char* path, Pcm* out) {
  if (!path || !out) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t hdr[12];
  if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) ||
      std::memcmp(hdr + 8, "WAVE", 4)) {
    std::fclose(f);
    return false;
  }
  int channels = 0;
  bool fmt_ok = false;
  bool data_ok = false;
  std::vector<int16_t> interleaved;
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
      out->rate = static_cast<int>(sr);
      channels = chans;
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
  out->mono.resize(interleaved.size() / static_cast<size_t>(channels));
  for (size_t i = 0; i < out->mono.size(); ++i) {
    int32_t sum = 0;
    for (int c = 0; c < channels; ++c) {
      sum += interleaved[i * static_cast<size_t>(channels) + static_cast<size_t>(c)];
    }
    out->mono[i] = static_cast<int16_t>(sum / channels);
  }
  return out->rate > 0 && !out->mono.empty();
}

bool WriteWav(const char* path, int rate, const int16_t* pcm, size_t n) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uint16_t channels = 1;
  const uint16_t bits = 16;
  const uint32_t data_bytes = static_cast<uint32_t>(n * 2);
  const uint32_t riff = 36 + data_bytes;
  const uint32_t rate_u = static_cast<uint32_t>(rate);
  const uint32_t byte_rate = rate_u * 2;
  const uint16_t block = 2;
  uint8_t h[44] = {};
  std::memcpy(h, "RIFF", 4);
  std::memcpy(h + 4, &riff, 4);
  std::memcpy(h + 8, "WAVE", 4);
  std::memcpy(h + 12, "fmt ", 4);
  uint32_t fmt = 16;
  std::memcpy(h + 16, &fmt, 4);
  uint16_t pcm_tag = 1;
  std::memcpy(h + 20, &pcm_tag, 2);
  std::memcpy(h + 22, &channels, 2);
  std::memcpy(h + 24, &rate_u, 4);
  std::memcpy(h + 28, &byte_rate, 4);
  std::memcpy(h + 32, &block, 2);
  std::memcpy(h + 34, &bits, 2);
  std::memcpy(h + 36, "data", 4);
  std::memcpy(h + 40, &data_bytes, 4);
  const bool ok = std::fwrite(h, 1, 44, f) == 44 &&
                  (n == 0 || std::fwrite(pcm, sizeof(int16_t), n, f) == n);
  std::fclose(f);
  return ok;
}

void ResampleMono(const int16_t* src, size_t src_n, int src_rate, int dst_rate,
                  std::vector<int16_t>* dst) {
  dst->clear();
  if (src_n == 0 || src_rate <= 0 || dst_rate <= 0) return;
  if (src_rate == dst_rate) {
    dst->assign(src, src + src_n);
    return;
  }
  const size_t src_10 = static_cast<size_t>(src_rate / 100);
  const size_t dst_10 = static_cast<size_t>(dst_rate / 100);
  if (src_10 == 0 || dst_10 == 0) return;
  webrtc::PushResampler<int16_t> rs(src_10, dst_10, 1);
  std::vector<int16_t> in(src_10);
  std::vector<int16_t> out(dst_10);
  for (size_t pos = 0; pos < src_n; pos += src_10) {
    std::fill(in.begin(), in.end(), 0);
    const size_t n = std::min(src_10, src_n - pos);
    std::memcpy(in.data(), src + pos, n * sizeof(int16_t));
    rs.Resample(webrtc::MonoView<const int16_t>(in.data(), src_10),
                webrtc::MonoView<int16_t>(out.data(), dst_10));
    dst->insert(dst->end(), out.begin(), out.end());
  }
}

struct Stats {
  const char* status = "fail";
  const char* stage = "";
  const char* codec = "";
  const char* compute = "";
  const char* encoder = "";
  int loss_pct = 0;
  int jitter_ms = 0;
  int opus_bps = 0;
  int out_of_codebook = 0;
  int clamped = 0;
  int packets = 0;
  int dropped = 0;
  int pack_mismatches = 0;
  int64_t payload_bytes = 0;
  double seconds = 0;
  double onwire_kbps = 0;
  int algorithmic_ms = 0;
  double jitter_buffer_ms = 0;
  std::string detail;
};

void WriteStats(const char* path, const Stats& s) {
  if (!path || !path[0]) return;
  std::string detail = s.detail;
  for (char& c : detail) {
    if (c == '"' || c == '\n' || c == '\r') c = ' ';
  }
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fprintf(f,
               "{\n"
               "  \"status\": \"%s\",\n"
               "  \"stage\": \"%s\",\n"
               "  \"codec\": \"%s\",\n"
               "  \"compute\": \"%s\",\n"
               "  \"encoder\": \"%s\",\n"
               "  \"loss_pct\": %d,\n"
               "  \"jitter_ms\": %d,\n"
               "  \"opus_bps\": %d,\n"
               "  \"out_of_codebook\": %d,\n"
               "  \"clamped\": %d,\n"
               "  \"packets\": %d,\n"
               "  \"dropped\": %d,\n"
               "  \"pack_mismatches\": %d,\n"
               "  \"payload_bytes\": %lld,\n"
               "  \"seconds\": %.4f,\n"
               "  \"onwire_kbps\": %.4f,\n"
               "  \"algorithmic_ms\": %d,\n"
               "  \"jitter_buffer_ms\": %.2f,\n"
               "  \"detail\": \"%s\"\n"
               "}\n",
               s.status, s.stage, s.codec, s.compute, s.encoder, s.loss_pct, s.jitter_ms,
               s.opus_bps, s.out_of_codebook, s.clamped, s.packets, s.dropped, s.pack_mismatches,
               static_cast<long long>(s.payload_bytes), s.seconds, s.onwire_kbps, s.algorithmic_ms,
               s.jitter_buffer_ms, detail.c_str());
  std::fclose(f);
}

void PrintResult(const Stats& s) {
  std::printf(
      "QUALITY_RESULT status=%s stage=%s codec=%s compute=%s encoder=%s loss=%d jitter_ms=%d "
      "opus_bps=%d out_of_codebook=%d clamped=%d packets=%d dropped=%d onwire_kbps=%.3f "
      "algo_ms=%d jitter_buffer_ms=%.2f pack_mismatches=%d\n",
      s.status, s.stage, s.codec, s.compute, s.encoder, s.loss_pct, s.jitter_ms, s.opus_bps,
      s.out_of_codebook, s.clamped, s.packets, s.dropped, s.onwire_kbps, s.algorithmic_ms,
      s.jitter_buffer_ms, s.pack_mismatches);
  std::fflush(stdout);
}

double OnWire(int64_t payload, int packets, double seconds) {
  if (seconds <= 0 || packets <= 0) return 0;
  const double bytes = static_cast<double>(payload) + static_cast<double>(packets) * (12.0 + 28.0);
  return bytes * 8.0 / seconds / 1000.0;
}

#if defined(WEBRTC_USE_LIQUIDCODEC)
int RunOffline(const Pcm& in, const char* wav_out, const char* model_dir, const char* compute,
               const char* precision, Stats* stats) {
  webrtc::liquid::ConsumeOutOfCodebook();
  webrtc::liquid::ConsumeClampedCodes();
  std::vector<int16_t> pcm24;
  ResampleMono(in.mono.data(), in.mono.size(), in.rate, webrtc::liquid::kSampleRateHz, &pcm24);
  webrtc::liquid::RuntimeConfig cfg;
  cfg.paths.model_dir = model_dir ? model_dir : "";
  cfg.paths.compute = compute && compute[0] ? compute : "cpu";
  const std::string manifest =
      webrtc::liquid::JoinPath(cfg.paths.model_dir, "liquidcodec.json");
  std::string error;
  if (!webrtc::liquid::LoadManifestFile(manifest, &cfg.paths, &cfg.chunk, &error)) {
    stats->detail = error.empty() ? "manifest" : error;
    return 1;
  }
  cfg.paths.encoder_model =
      webrtc::liquid::EncoderModelForPrecision(cfg.paths.encoder_model, precision ? precision : "int8");
  cfg.paths.model_dir = model_dir;
  if (!webrtc::ConfigureLiquidRuntime(cfg, &error)) {
    stats->detail = error;
    if (precision && std::strcmp(precision, "fp32") == 0) return 3;
    return 1;
  }
  auto backend = webrtc::liquid::CreateCoreMlBackend();
  if (!backend || !backend->Start(cfg.paths, cfg.chunk, &error)) {
    stats->detail = error.empty() ? "backend start" : error;
    if (precision && std::strcmp(precision, "fp32") == 0) return 3;
    return 1;
  }
  const auto latency = webrtc::liquid::DescribeLatency(
      cfg.chunk, cfg.paths.decoder_kind != "pcm", cfg.paths);
  stats->algorithmic_ms = latency.algorithmic_ms;
  if (cfg.paths.lookahead_samples > 0) {
    pcm24.insert(pcm24.end(), static_cast<size_t>(cfg.paths.lookahead_samples), 0);
  }
  webrtc::liquid::StreamEncoder encoder(backend.get(), cfg.chunk, cfg.paths);
  std::vector<std::array<int32_t, webrtc::liquid::kCodebooks>> codes;
  webrtc::liquid::WindowTiming timing;
  if (!encoder.Push(pcm24.data(), pcm24.size(), &codes, &timing, &error)) {
    stats->detail = error.empty() ? "encode" : error;
    return 1;
  }
  stats->out_of_codebook = webrtc::liquid::ConsumeOutOfCodebook();
  stats->clamped = webrtc::liquid::ConsumeClampedCodes();
  stats->packets = static_cast<int>(codes.size());
  if (codes.empty()) {
    stats->detail = "encoder produced no codes";
    return 1;
  }
  uint8_t packed[128];
  int32_t back[48];
  for (size_t i = 0; i < codes.size(); ++i) {
    for (auto pack : {webrtc::liquid::WirePack::kU16, webrtc::liquid::WirePack::kTight}) {
      size_t n = 0;
      webrtc::liquid::WirePack got = pack;
      int frames = 0;
      uint16_t seq = 0;
      if (!webrtc::liquid::PackPayload(pack, codes[i].data(), 1, static_cast<uint16_t>(i),
                                      cfg.chunk.codebook_size, packed, sizeof(packed), &n) ||
          !webrtc::liquid::UnpackPayload(packed, n, cfg.chunk.codebook_size, &got, &frames, &seq,
                                        back) ||
          frames != 1 || std::memcmp(codes[i].data(), back, sizeof(int32_t) * 12) != 0) {
        ++stats->pack_mismatches;
      }
    }
  }
  webrtc::liquid::StreamDecoder decoder(backend.get(), cfg.chunk);
  std::vector<int16_t> decoded;
  for (const auto& frame : codes) {
    if (!decoder.PushFrame(frame.data(), &decoded, &timing, &error)) {
      stats->detail = error.empty() ? "decode" : error;
      return 1;
    }
  }
  if (decoded.empty() || !WriteWav(wav_out, webrtc::liquid::kSampleRateHz, decoded.data(),
                                  decoded.size())) {
    stats->detail = "no decoded audio";
    return 1;
  }
  stats->seconds = static_cast<double>(pcm24.size()) / webrtc::liquid::kSampleRateHz;
  stats->payload_bytes = static_cast<int64_t>(codes.size()) * 28;
  stats->onwire_kbps = OnWire(stats->payload_bytes, stats->packets, stats->seconds);
  stats->status = stats->pack_mismatches == 0 ? "ok" : "fail";
  return stats->pack_mismatches == 0 ? 0 : 1;
}
#endif

struct Scheduled {
  int64_t arrival_ms = 0;
  uint16_t seq = 0;
  uint32_t timestamp = 0;
  webrtc::Buffer payload;
};

int RunFactory(const Pcm& in, const char* wav_out, const char* codec, const char* model_dir,
               const char* compute, const char* precision, int opus_bps, int loss_pct,
               int jitter_ms, Stats* stats) {
  const bool liquid = std::strcmp(codec, "liquidcodec") == 0;
#if !defined(WEBRTC_USE_LIQUIDCODEC)
  if (liquid) {
    stats->detail = "this build has no LiquidCodec";
    return 2;
  }
#else
  if (liquid) {
    webrtc::liquid::ConsumeOutOfCodebook();
    webrtc::liquid::ConsumeClampedCodes();
    webrtc::liquid::RuntimeConfig cfg;
    cfg.paths.model_dir = model_dir ? model_dir : "";
    cfg.paths.compute = compute && compute[0] ? compute : "cpu";
    std::string error;
    const std::string manifest =
        webrtc::liquid::JoinPath(cfg.paths.model_dir, "liquidcodec.json");
    if (!webrtc::liquid::LoadManifestFile(manifest, &cfg.paths, &cfg.chunk, &error)) {
      stats->detail = error.empty() ? "manifest" : error;
      return 1;
    }
    cfg.paths.encoder_model = webrtc::liquid::EncoderModelForPrecision(
        cfg.paths.encoder_model, precision ? precision : "int8");
    cfg.paths.model_dir = model_dir;
    if (!webrtc::ConfigureLiquidRuntime(cfg, &error)) {
      stats->detail = error;
      if (precision && std::strcmp(precision, "fp32") == 0) return 3;
      return 1;
    }
    stats->algorithmic_ms = webrtc::liquid::DescribeLatency(
                                cfg.chunk, cfg.paths.decoder_kind != "pcm", cfg.paths)
                                .algorithmic_ms;
  }
#endif
  if (!liquid) stats->algorithmic_ms = 20;

  std::unique_ptr<webrtc::Clock> clock_owner =
      std::make_unique<webrtc::SimulatedClock>(webrtc::Timestamp::Seconds(10000));
  webrtc::SimulatedClock* clock = static_cast<webrtc::SimulatedClock*>(clock_owner.get());
  webrtc::EnvironmentFactory env_factory;
  env_factory.Set(std::move(clock_owner));
  webrtc::Environment env = env_factory.Create();

  webrtc::scoped_refptr<webrtc::AudioEncoderFactory> enc_factory;
  webrtc::scoped_refptr<webrtc::AudioDecoderFactory> dec_factory;
  webrtc::SdpAudioFormat format("opus", 48000, 1);
  int channels = 1;
  if (liquid) {
#if defined(WEBRTC_USE_LIQUIDCODEC)
    enc_factory = webrtc::CreateLiquidAudioEncoderFactory();
    dec_factory = webrtc::CreateLiquidAudioDecoderFactory();
    format = webrtc::SdpAudioFormat(webrtc::liquid::kPayloadName, webrtc::liquid::kWebRtcClockHz, 1);
    format.parameters["pack"] = "u16";
    format.parameters["fpp"] = "1";
#else
    stats->detail = "this build has no LiquidCodec";
    return 2;
#endif
  } else {
    enc_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    dec_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    format.parameters["stereo"] = "0";
    format.parameters["useinbandfec"] = "0";
  }

  webrtc::AudioEncoderFactory::Options opt;
  opt.payload_type = 111;
  std::unique_ptr<webrtc::AudioEncoder> encoder = enc_factory->Create(env, format, opt);
  if (!encoder && !liquid) {
    format.num_channels = 2;
    channels = 2;
    encoder = enc_factory->Create(env, format, opt);
  }
  if (!encoder) {
    stats->detail = "encoder factory returned null";
    return 1;
  }
  if (!liquid && opus_bps > 0) encoder->OnReceivedTargetAudioBitrate(opus_bps);

  webrtc::NetEq::Config neteq_config;
  neteq_config.sample_rate_hz = 48000;
  neteq_config.max_delay_ms = 500;
  webrtc::DefaultNetEqFactory neteq_factory;
  std::unique_ptr<webrtc::NetEq> neteq = neteq_factory.Create(env, neteq_config, dec_factory);
  if (!neteq || !neteq->RegisterPayloadType(111, format)) {
    stats->detail = "neteq codec registration failed";
    return 1;
  }

  std::vector<int16_t> pcm48;
  ResampleMono(in.mono.data(), in.mono.size(), in.rate, 48000, &pcm48);
  const int frame = 480;
  const int64_t t0 = clock->CurrentTime().ms();
  int64_t now = t0;
  uint32_t rtp_ts = 0;
  uint16_t seq = 1;
  int packet_index = 0;
  std::deque<Scheduled> pending;
  std::vector<int16_t> decoded;
  size_t pos = 0;
  int guard = 0;
  const int extra_frames = (stats->algorithmic_ms + jitter_ms) / 10 + 40;
  const int guard_limit = static_cast<int>(pcm48.size() / frame) + extra_frames + 80;
  const size_t want = pcm48.size() + static_cast<size_t>(extra_frames) * static_cast<size_t>(frame) / 2;
  while (guard++ < guard_limit &&
         (pos < pcm48.size() || !pending.empty() || decoded.size() < want)) {
    if (pos < pcm48.size()) {
      std::vector<int16_t> frame_pcm(static_cast<size_t>(frame * channels));
      for (int i = 0; i < frame; ++i) {
        const int16_t s = pos + static_cast<size_t>(i) < pcm48.size() ? pcm48[pos + static_cast<size_t>(i)] : 0;
        for (int c = 0; c < channels; ++c) frame_pcm[static_cast<size_t>(i * channels + c)] = s;
      }
      webrtc::Buffer encoded;
      auto info = encoder->Encode(rtp_ts, frame_pcm, &encoded);
      if (info.encoded_bytes > 0 && encoded.size() > 0) {
        if (webrtc::quality::DropPacket(packet_index, loss_pct)) {
          ++stats->dropped;
        } else {
          Scheduled item;
          item.arrival_ms = (now - t0) + webrtc::quality::DelayMs(packet_index, jitter_ms);
          item.seq = seq++;
          item.timestamp = info.encoded_timestamp;
          item.payload.AppendData(encoded.data(), encoded.size());
          pending.push_back(std::move(item));
          stats->payload_bytes += static_cast<int64_t>(info.encoded_bytes);
        }
        ++packet_index;
        ++stats->packets;
      }
      pos += static_cast<size_t>(frame);
      rtp_ts += static_cast<uint32_t>(frame);
    }
    const int64_t media_ms = now - t0;
    while (!pending.empty() && pending.front().arrival_ms <= media_ms) {
      Scheduled item = std::move(pending.front());
      pending.pop_front();
      webrtc::RTPHeader header;
      header.payloadType = 111;
      header.sequenceNumber = item.seq;
      header.timestamp = item.timestamp;
      header.markerBit = false;
      const webrtc::Timestamp arrival = webrtc::Timestamp::Millis(t0 + item.arrival_ms);
      neteq->InsertPacket(header,
                          std::span<const uint8_t>(item.payload.data(), item.payload.size()),
                          webrtc::RtpPacketInfo(header, arrival));
    }
    webrtc::AudioFrame audio;
    bool muted = false;
    if (neteq->GetAudio(&audio, &muted) == webrtc::NetEq::kOK) {
      const size_t n = audio.samples_per_channel_;
      const int ch = std::max(1, static_cast<int>(audio.num_channels_));
      if (muted || n == 0 || audio.data() == nullptr) {
        decoded.insert(decoded.end(), frame, 0);
      } else {
        for (size_t i = 0; i < n; ++i) decoded.push_back(audio.data()[i * static_cast<size_t>(ch)]);
      }
    }
    clock->AdvanceTimeMilliseconds(10);
    now += 10;
    if (pos >= pcm48.size() && pending.empty() && decoded.size() >= want) break;
  }
  if (decoded.empty() || !WriteWav(wav_out, 48000, decoded.data(), decoded.size())) {
    stats->detail = "neteq produced no audio";
    return 1;
  }
  webrtc::NetEqNetworkStatistics net;
  neteq->NetworkStatistics(&net);
  stats->jitter_buffer_ms = net.mean_waiting_time_ms;
#if defined(WEBRTC_USE_LIQUIDCODEC)
  if (liquid) {
    stats->out_of_codebook = webrtc::liquid::ConsumeOutOfCodebook();
    stats->clamped = webrtc::liquid::ConsumeClampedCodes();
  }
#endif
  stats->seconds = static_cast<double>(pcm48.size()) / 48000.0;
  const int sent = stats->packets - stats->dropped;
  stats->onwire_kbps = OnWire(stats->payload_bytes, sent, stats->seconds);
  stats->status = "ok";
  return 0;
}

}  // namespace

extern "C" int DirectQualityRun(const char* stage, const char* wav_in, const char* wav_out,
                                const char* stats_json, const char* codec, const char* model_dir,
                                const char* compute, const char* encoder_precision,
                                int opus_bitrate_bps, int loss_pct, int jitter_ms) {
  Stats stats;
  stats.stage = stage ? stage : "";
  stats.codec = codec ? codec : "";
  stats.compute = compute ? compute : "";
  stats.encoder = encoder_precision ? encoder_precision : "";
  stats.loss_pct = loss_pct;
  stats.jitter_ms = jitter_ms;
  stats.opus_bps = opus_bitrate_bps;
  auto finish = [&](int rc) {
    if (rc == 3) stats.status = "skipped";
    else if (rc != 0) stats.status = "fail";
    WriteStats(stats_json, stats);
    PrintResult(stats);
    if (!stats.detail.empty()) std::fprintf(stderr, "quality: %s\n", stats.detail.c_str());
    return rc;
  };
  if (!stage || (std::strcmp(stage, "offline") != 0 && std::strcmp(stage, "factory") != 0)) {
    stats.detail = "stage must be offline or factory";
    return finish(1);
  }
  if (!wav_in || !wav_out || !wav_out[0]) {
    stats.detail = "wav_in and wav_out are required";
    return finish(1);
  }
  Pcm in;
  if (!ReadWav(wav_in, &in)) {
    stats.detail = "could not read wav";
    return finish(1);
  }
  const bool liquid = !codec || std::strcmp(codec, "liquidcodec") == 0 || std::strcmp(codec, "") == 0;
  if (!liquid && std::strcmp(codec, "opus") != 0) {
    stats.detail = "codec must be liquidcodec or opus";
    return finish(1);
  }
  stats.codec = liquid ? "liquidcodec" : "opus";
  if (std::strcmp(stage, "offline") == 0) {
    if (!liquid) {
      stats.detail = "offline round-trip is LiquidCodec only; opus starts at stage 3";
      return finish(3);
    }
#if defined(WEBRTC_USE_LIQUIDCODEC)
    return finish(RunOffline(in, wav_out, model_dir, compute, encoder_precision, &stats));
#else
    stats.detail = "offline LiquidCodec needs the Apple liquidcodec build";
    return finish(2);
#endif
  }
  return finish(RunFactory(in, wav_out, stats.codec, model_dir, compute, encoder_precision,
                           opus_bitrate_bps, loss_pct, jitter_ms, &stats));
}
