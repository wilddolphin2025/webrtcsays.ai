/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

// Offline echo-canceller loop, no devices and no network:
//   render (far end as played) -> simulated speaker-to-mic echo path ->
//   APM (AEC3 + HPF + NS, as in a call) -> measured ERLE per second.
// Feed it the factory stage's quality_out.wav (NetEq output, accelerate and
// expand included) to check AEC3 against time-stretched playout.
// Knobs (env): AEC_TEST_DELAY_MS (40), AEC_TEST_GAIN_DB (-6, echo level vs
// render), AEC_TEST_JUMP_MS (0; echo delay change at half time, models an
// audio-device delay change), AEC_TEST_NS (1).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#include "common_audio/wav_file.h"
#include "quality.h"

namespace {

int EnvInt(const char* name, int def) {
  const char* v = std::getenv(name);
  return v && *v ? std::atoi(v) : def;
}

double Dbfs(double sum_sq, size_t n) {
  if (n == 0) return -120.0;
  const double p = sum_sq / static_cast<double>(n) / (32768.0 * 32768.0);
  return p > 1e-12 ? 10.0 * std::log10(p) : -120.0;
}

}  // namespace

extern "C" DIRECT_QUALITY_API int DirectAecSelfTest(const char* render_wav, const char* out_dir) {
  webrtc::WavReader reader(render_wav);
  const int rate = reader.sample_rate();
  const size_t ch = reader.num_channels();
  if (rate % 100 != 0 || ch < 1) {
    std::fprintf(stderr, "aec selftest: %s has rate %d\n", render_wav, rate);
    return 1;
  }
  std::vector<int16_t> inter(reader.num_samples());
  inter.resize(reader.ReadSamples(inter.size(), inter.data()));
  std::vector<int16_t> render(inter.size() / ch);
  for (size_t i = 0; i < render.size(); ++i) render[i] = inter[i * ch];

  const int delay_ms = EnvInt("AEC_TEST_DELAY_MS", 40);
  const int gain_db = EnvInt("AEC_TEST_GAIN_DB", -6);
  const int jump_ms = EnvInt("AEC_TEST_JUMP_MS", 0);
  const bool ns = EnvInt("AEC_TEST_NS", 1) != 0;
  const size_t frame = static_cast<size_t>(rate / 100);
  const size_t frames = render.size() / frame;

  // Room impulse response: direct path plus a 40 ms decaying tail.
  std::mt19937 rng(7);
  std::normal_distribution<double> gauss(0.0, 1.0);
  const size_t tail = static_cast<size_t>(rate * 40 / 1000);
  std::vector<double> ir(tail + 1);
  ir[0] = 1.0;
  for (size_t i = 1; i <= tail; ++i) {
    ir[i] = 0.25 * gauss(rng) * std::exp(-static_cast<double>(i) / (rate * 0.008));
  }
  double norm = 0;
  for (double v : ir) norm += v * v;
  const double g = std::pow(10.0, gain_db / 20.0) / std::sqrt(norm);
  for (double& v : ir) v *= g;

  auto echo_at = [&](size_t n, size_t delay) -> double {
    double acc = 0;
    for (size_t k = 0; k < ir.size(); ++k) {
      const size_t lag = delay + k;
      if (n >= lag) acc += ir[k] * render[n - lag];
    }
    return acc;
  };

  webrtc::AudioProcessing::Config cfg;
  cfg.echo_canceller.enabled = true;
  cfg.high_pass_filter.enabled = true;
  cfg.noise_suppression.enabled = ns;
  cfg.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
  auto apm = webrtc::BuiltinAudioProcessingBuilder(cfg).Build(webrtc::CreateEnvironment());
  if (!apm) return 1;
  const webrtc::StreamConfig sc(rate, 1);

  std::vector<int16_t> out(render.size(), 0);
  std::vector<int16_t> rbuf(frame), cbuf(frame), obuf(frame);
  std::normal_distribution<double> noise(0.0, 32768.0 * std::pow(10.0, -70.0 / 20.0));
  const std::string stats_path = std::string(out_dir) + "/aec_stats.json";
  FILE* js = std::fopen(stats_path.c_str(), "w");
  if (js) std::fprintf(js, "{\"render\":\"%s\",\"rate\":%d,\"delay_ms\":%d,\"gain_db\":%d,\"jump_ms\":%d,\"ns\":%d,\"seconds\":[",
                       render_wav, rate, delay_ms, gain_db, jump_ms, ns ? 1 : 0);
  double sec_r = 0, sec_e = 0, sec_o = 0;
  size_t sec_n = 0;
  double tot_e = 0, tot_o = 0, conv_e = 0, conv_o = 0;
  double worst = 1e9;
  int worst_t = -1;
  for (size_t f = 0; f < frames; ++f) {
    const size_t base = f * frame;
    const bool after_jump = jump_ms != 0 && f >= frames / 2;
    const size_t delay = static_cast<size_t>(rate * (delay_ms + (after_jump ? jump_ms : 0)) / 1000);
    bool render_active = false;
    double fr = 0, fe = 0, fo = 0;
    for (size_t i = 0; i < frame; ++i) {
      rbuf[i] = render[base + i];
      const double e = echo_at(base + i, delay);
      const double c = std::max(-32768.0, std::min(32767.0, e + noise(rng)));
      cbuf[i] = static_cast<int16_t>(c);
      fr += static_cast<double>(rbuf[i]) * rbuf[i];
      fe += c * c;
    }
    apm->ProcessReverseStream(rbuf.data(), sc, sc, rbuf.data());
    apm->set_stream_delay_ms(delay_ms);
    apm->ProcessStream(cbuf.data(), sc, sc, obuf.data());
    for (size_t i = 0; i < frame; ++i) {
      out[base + i] = obuf[i];
      fo += static_cast<double>(obuf[i]) * obuf[i];
    }
    render_active = Dbfs(fr, frame) > -45.0;
    if (render_active) {
      sec_e += fe;
      sec_o += fo;
      tot_e += fe;
      tot_o += fo;
      if (f >= 200) { conv_e += fe; conv_o += fo; }  // after 2 s
    }
    sec_r += fr;
    sec_n += frame;
    if ((f + 1) % 100 == 0) {
      const int t = static_cast<int>((f + 1) / 100);
      const double erle = sec_o > 0 ? 10.0 * std::log10(sec_e / sec_o) : 99.0;
      const auto st = apm->GetStatistics(true);
      std::fprintf(stderr,
                   "aec t=%ds render_dbfs=%.1f echo_dbfs=%.1f out_dbfs=%.1f erle_db=%.1f "
                   "apm_erle_db=%.1f apm_delay_ms=%d divergent=%.2f\n",
                   t, Dbfs(sec_r, sec_n), Dbfs(sec_e, sec_n), Dbfs(sec_o, sec_n),
                   sec_e > 0 ? erle : 0.0,
                   st.echo_return_loss_enhancement.value_or(-1),
                   st.delay_ms.value_or(-1), st.divergent_filter_fraction.value_or(-1));
      if (js) std::fprintf(js, "%s{\"t\":%d,\"erle_db\":%.2f,\"apm_delay_ms\":%d}", t > 1 ? "," : "", t,
                           sec_e > 0 ? erle : 0.0, st.delay_ms.value_or(-1));
      if (t > 2 && sec_e > 0 && erle < worst) { worst = erle; worst_t = t; }
      sec_r = sec_e = sec_o = 0;
      sec_n = 0;
    }
  }
  const double erle_all = tot_o > 0 ? 10.0 * std::log10(tot_e / tot_o) : 0;
  const double erle_conv = conv_o > 0 ? 10.0 * std::log10(conv_e / conv_o) : 0;
  std::fprintf(stderr,
               "aec summary: erle_db=%.1f erle_after_2s_db=%.1f worst_1s_erle_db=%.1f at t=%ds "
               "(delay %d ms, jump %d ms, echo %d dB)\n",
               erle_all, erle_conv, worst == 1e9 ? 0.0 : worst, worst_t, delay_ms, jump_ms, gain_db);
  if (js) {
    std::fprintf(js, "],\"erle_db\":%.2f,\"erle_after_2s_db\":%.2f,\"worst_1s_erle_db\":%.2f}\n",
                 erle_all, erle_conv, worst == 1e9 ? 0.0 : worst);
    std::fclose(js);
  }
  webrtc::WavWriter w(std::string(out_dir) + "/aec_out.wav", rate, 1);
  w.WriteSamples(out.data(), out.size());
  return 0;
}
