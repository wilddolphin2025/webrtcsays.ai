// ONNX Runtime CPU backend for the LiquidCall step encoder and the
// hidden-state decoder. The session I/O is the Core ML step:
//   audio, hist, k_cache, v_cache, conv_state, pos
//   -> codes, hist_out, k_cache_out, v_cache_out, conv_state_out, pos_out
// and the decoder lat/valid -> hidden, then the same fp32 CPU tail.
// NNAPI and every other execution provider are not requested.

#include "modules/audio_coding/codecs/liquidcall/liquid_backend.h"

#include "modules/audio_coding/codecs/liquidcall/liquid_tail.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "onnxruntime_c_api.h"

namespace webrtc {
namespace liquid {
namespace {

int ThreadCount() {
  const char* env = std::getenv("ORT_THREADS");
  if (!env || !*env) return 2;
  const int n = std::atoi(env);
  return n > 0 ? n : 2;
}

class OnnxBackend : public LiquidBackend {
 public:
  ~OnnxBackend() override { Release(); }

  bool Start(const ModelPaths& paths, const ChunkConfig& chunk, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    Release();
    paths_ = paths;
    chunk_ = chunk;
    const OrtApiBase* base = OrtGetApiBase();
    if (!base) {
      if (error) *error = "OrtGetApiBase returned null";
      return false;
    }
    api_ = base->GetApi(ORT_API_VERSION);
    if (!api_) {
      if (error) *error = "this ONNX Runtime does not provide API version " +
                          std::to_string(ORT_API_VERSION);
      return false;
    }
    if (!Ok(api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "cetacall", &env_), error, "CreateEnv")) {
      return false;
    }
    if (!Ok(api_->CreateSessionOptions(&opts_), error, "CreateSessionOptions")) return false;
    if (!Ok(api_->SetIntraOpNumThreads(opts_, ThreadCount()), error, "SetIntraOpNumThreads")) {
      return false;
    }
    if (!Ok(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_), error,
            "CreateCpuMemoryInfo")) {
      return false;
    }
    const std::string enc = JoinPath(paths.model_dir, paths.encoder_model);
    const std::string dec = JoinPath(paths.model_dir, paths.decoder_model);
    if (!Ok(api_->CreateSession(env_, enc.c_str(), opts_, &enc_), error, "encoder session")) {
      return false;
    }
    if (!Ok(api_->CreateSession(env_, dec.c_str(), opts_, &dec_), error, "decoder session")) {
      return false;
    }
    cache_ = paths.encoder_cache_frames > 0 ? paths.encoder_cache_frames : 1024;
    lookback_ = paths.stft_lookback_samples > 0 ? paths.stft_lookback_samples : 1120;
    step_ = paths.EncoderStepSamples(chunk.frame_samples);
    hist_.assign(static_cast<size_t>(lookback_), 0.f);
    k_.assign(static_cast<size_t>(3 * 2 * 64 * cache_), 0.f);
    v_.assign(k_.size(), 0.f);
    conv_.assign(static_cast<size_t>(5 * 512 * 6), 0.f);
    pos_.assign(1, 0.f);
    if (paths.decoder_kind == "hidden") {
      if (!tail_.Load(JoinPath(paths.model_dir, paths.tail), error)) return false;
    }
    return true;
  }

  bool EncodeWindow(const int16_t* pcm, const ChunkConfig& chunk, int32_t* codes_out,
                    WindowTiming* timing, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!api_ || !enc_ || !pcm || !codes_out) return false;
    const auto t0 = std::chrono::steady_clock::now();
    EnsureDft();
    std::vector<float> audio(static_cast<size_t>(step_));
    for (int i = 0; i < step_; ++i) {
      audio[static_cast<size_t>(i)] = static_cast<float>(pcm[i]) / 32768.f;
    }
    // Same waves as StepEncoder._embed. first is the reflect pad Liquid's
    // encode() uses on frame 0. later is the 1120-sample PCM history plus
    // this step. Both are fed every call; pos selects the embedding.
    constexpr int kLookahead = 480;
    constexpr int kFeat = 1923;
    std::vector<float> first(static_cast<size_t>(kLookahead + step_));
    for (int i = 0; i < kLookahead; ++i) first[static_cast<size_t>(i)] = audio[static_cast<size_t>(kLookahead - i)];
    std::memcpy(first.data() + kLookahead, audio.data(), static_cast<size_t>(step_) * sizeof(float));
    std::vector<float> later(static_cast<size_t>(lookback_ + step_));
    std::memcpy(later.data(), hist_.data(), hist_.size() * sizeof(float));
    std::memcpy(later.data() + lookback_, audio.data(), static_cast<size_t>(step_) * sizeof(float));
    std::vector<float> feat0(static_cast<size_t>(kFeat * 6));
    std::vector<float> feat1(static_cast<size_t>(kFeat * 8));
    DftFeatures(first.data(), static_cast<int>(first.size()), 6, feat0.data());
    DftFeatures(later.data(), static_cast<int>(later.size()), 8, feat1.data());

    OrtValue* in[6] = {};
    OrtValue* out[5] = {};
    const int64_t feat0_shape[] = {1, kFeat, 6};
    const int64_t feat1_shape[] = {1, kFeat, 8};
    const int64_t cache_shape[] = {3, 2, 64, cache_};
    const int64_t conv_shape[] = {5, 512, 6};
    const int64_t pos_shape[] = {1};
    bool ok = MakeTensor(feat0.data(), feat0.size(), feat0_shape, 3, &in[0], error) &&
              MakeTensor(feat1.data(), feat1.size(), feat1_shape, 3, &in[1], error) &&
              MakeTensor(k_.data(), k_.size(), cache_shape, 4, &in[2], error) &&
              MakeTensor(v_.data(), v_.size(), cache_shape, 4, &in[3], error) &&
              MakeTensor(conv_.data(), conv_.size(), conv_shape, 3, &in[4], error) &&
              MakeTensor(pos_.data(), pos_.size(), pos_shape, 1, &in[5], error);
    const char* in_names[] = {"feat0", "feat1", "k_cache", "v_cache", "conv_state", "pos"};
    const char* out_names[] = {"codes", "k_cache_out", "v_cache_out", "conv_state_out", "pos_out"};
    const auto p0 = std::chrono::steady_clock::now();
    if (ok) {
      ok = Ok(api_->Run(enc_, nullptr, in_names, in, 6, out_names, 5, out), error, "encoder run");
    }
    const auto p1 = std::chrono::steady_clock::now();
    std::vector<float> k_out, v_out, conv_out, pos_out;
    float* codes = nullptr;
    if (ok) ok = Read(out[0], &codes, error, "codes");
    if (ok) {
      for (int i = 0; i < chunk.codebooks; ++i) {
        bool clamped = false;
        if (!QuantizeCode(codes[i], chunk.codebook_size, &codes_out[i], &clamped)) {
          if (error) *error = "encoder code outside the codebook";
          ok = false;
          break;
        }
      }
    }
    if (ok) ok = CopyOut(out[1], &k_out, k_.size(), error, "k_cache_out") &&
                 CopyOut(out[2], &v_out, v_.size(), error, "v_cache_out") &&
                 CopyOut(out[3], &conv_out, conv_.size(), error, "conv_state_out") &&
                 CopyOut(out[4], &pos_out, 1, error, "pos_out");
    ReleaseValues(in, 6);
    ReleaseValues(out, 5);
    if (!ok) {
      ZeroState();
      ++bad_codes_;
      return false;
    }
    const int frame = chunk.frame_samples;
    const int hist_at = frame - lookback_;
    if (hist_at >= 0 && hist_at + lookback_ <= step_) {
      std::memcpy(hist_.data(), audio.data() + hist_at, hist_.size() * sizeof(float));
    }
    k_.swap(k_out);
    v_.swap(v_out);
    conv_.swap(conv_out);
    pos_.swap(pos_out);
    if (timing) {
      timing->window_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      timing->prediction_ms = std::chrono::duration<double, std::milli>(p1 - p0).count();
    }
    return true;
  }

  bool DecodeWindow(const int32_t* codes, const ChunkConfig& chunk, int16_t* pcm_out,
                    WindowTiming* timing, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!api_ || !dec_ || !codes || !pcm_out) return false;
    const auto t0 = std::chrono::steady_clock::now();
    const int fr = chunk.WindowFrames();
    const int t75 = fr * kUpsample;
    const int keep = chunk.new_frames * kUpsample;
    const int c0 = chunk.ctx_frames * kUpsample;
    std::vector<float> lat(static_cast<size_t>(kLatent * fr));
    CodesToLatent(codes, fr, chunk.codebooks, lat.data());
    std::vector<float> valid(static_cast<size_t>(t75), 0.f);
    for (int i = 0; i < fr; ++i) {
      bool real = i >= chunk.ctx_frames;
      if (!real) {
        real = true;
        for (int q = 0; q < chunk.codebooks; ++q) {
          if (codes[i * chunk.codebooks + q] < 0) real = false;
        }
      }
      if (real) {
        for (int k = 0; k < kUpsample; ++k) valid[static_cast<size_t>(i * kUpsample + k)] = 1.f;
      }
    }
    OrtValue* in[2] = {};
    OrtValue* out = nullptr;
    const int64_t lat_shape[] = {1, kLatent, 1, fr};
    const int64_t valid_shape[] = {1, 1, 1, t75};
    bool ok = MakeTensor(lat.data(), lat.size(), lat_shape, 4, &in[0], error) &&
              MakeTensor(valid.data(), valid.size(), valid_shape, 4, &in[1], error);
    const char* in_names[] = {"lat", "valid"};
    const char* out_names[] = {"hidden"};
    const auto p0 = std::chrono::steady_clock::now();
    if (ok) ok = Ok(api_->Run(dec_, nullptr, in_names, in, 2, out_names, 1, &out), error, "decoder run");
    const auto p1 = std::chrono::steady_clock::now();
    float* hidden = nullptr;
    if (ok) ok = Read(out, &hidden, error, "hidden");
    std::vector<float> pcm;
    if (ok) {
      // hidden is (1, 512, 1, T) channel-major over time.
      std::vector<float> slice(static_cast<size_t>(kHidden * keep));
      for (int c = 0; c < kHidden; ++c) {
        for (int t = 0; t < keep; ++t) {
          slice[static_cast<size_t>(c * keep + t)] =
              hidden[static_cast<size_t>(c * t75 + c0 + t)];
        }
      }
      pcm = tail_.PushHidden(slice.data(), keep);
    }
    if (in[0]) api_->ReleaseValue(in[0]);
    if (in[1]) api_->ReleaseValue(in[1]);
    if (out) api_->ReleaseValue(out);
    if (!ok) return false;
    std::vector<int16_t> s16;
    s16.reserve(pcm.size());
    for (float x : pcm) {
      float y = x * 32767.f;
      if (y > 32767.f) y = 32767.f;
      if (y < -32768.f) y = -32768.f;
      s16.push_back(static_cast<int16_t>(std::lrintf(y)));
    }
    fifo_.insert(fifo_.end(), s16.begin(), s16.end());
    const int need = chunk.new_frames * chunk.frame_samples;
    if (static_cast<int>(fifo_.size()) < need) {
      fifo_.insert(fifo_.begin(), static_cast<size_t>(need - fifo_.size()), 0);
    }
    std::memcpy(pcm_out, fifo_.data(), static_cast<size_t>(need) * sizeof(int16_t));
    fifo_.erase(fifo_.begin(), fifo_.begin() + need);
    if (timing) {
      timing->window_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      timing->prediction_ms = std::chrono::duration<double, std::milli>(p1 - p0).count();
    }
    return true;
  }

  void Reset() override {
    std::lock_guard<std::mutex> lock(mu_);
    ZeroState();
    tail_.Reset();
    fifo_.clear();
  }

  const char* name() const override { return "onnx-cpu"; }
  int out_of_codebook() const override { return bad_codes_; }

 private:
  bool Ok(OrtStatus* st, std::string* error, const char* what) {
    if (!st) return true;
    if (error) *error = std::string(what) + ": " + api_->GetErrorMessage(st);
    api_->ReleaseStatus(st);
    return false;
  }

  bool MakeTensor(float* data, size_t n, const int64_t* shape, size_t rank, OrtValue** out,
                  std::string* error) {
    return Ok(api_->CreateTensorWithDataAsOrtValue(mem_, data, n * sizeof(float), shape, rank,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, out),
              error, "tensor");
  }

  bool Read(OrtValue* value, float** data, std::string* error, const char* what) {
    if (!value) {
      if (error) *error = std::string(what) + " missing";
      return false;
    }
    void* p = nullptr;
    if (!Ok(api_->GetTensorMutableData(value, &p), error, what)) return false;
    *data = static_cast<float*>(p);
    return *data != nullptr;
  }

  bool CopyOut(OrtValue* value, std::vector<float>* dst, size_t n, std::string* error,
               const char* what) {
    float* p = nullptr;
    if (!Read(value, &p, error, what)) return false;
    dst->assign(p, p + n);
    return true;
  }

  void ReleaseValues(OrtValue** values, int n) {
    for (int i = 0; i < n; ++i) {
      if (values[i]) api_->ReleaseValue(values[i]);
      values[i] = nullptr;
    }
  }

  void EnsureDft() {
    if (!dft_win_.empty()) return;
    constexpr int kN = 1280;
    dft_win_.resize(kN);
    for (int t = 0; t < kN; ++t) {
      dft_win_[static_cast<size_t>(t)] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * t / kN);
    }
  }

  // In-place DFT, length factored by 2 and 5 (1280 = 2^8 * 5). Sign matches numpy.fft.
  static void Fft(std::vector<std::complex<double>>* a) {
    const int n = static_cast<int>(a->size());
    if (n <= 1) return;
    const int rad = (n % 2 == 0) ? 2 : 5;
    const int m = n / rad;
    std::vector<std::vector<std::complex<double>>> sub(
        static_cast<size_t>(rad), std::vector<std::complex<double>>(static_cast<size_t>(m)));
    for (int r = 0; r < rad; ++r) {
      for (int i = 0; i < m; ++i) sub[static_cast<size_t>(r)][static_cast<size_t>(i)] = (*a)[static_cast<size_t>(i * rad + r)];
      Fft(&sub[static_cast<size_t>(r)]);
    }
    for (int k = 0; k < n; ++k) {
      std::complex<double> sum = 0;
      const int i = k % m;
      for (int r = 0; r < rad; ++r) {
        const double ang = -2.0 * 3.14159265358979323846 * r * k / n;
        sum += sub[static_cast<size_t>(r)][static_cast<size_t>(i)] * std::complex<double>(std::cos(ang), std::sin(ang));
      }
      (*a)[static_cast<size_t>(k)] = sum;
    }
  }

  // feat is (1923, T), channel-major, matching the ONNX input (1, 1923, T).
  void DftFeatures(const float* wave, int n, int frames, float* feat) {
    constexpr int kN = 1280;
    constexpr int kHop = 320;
    constexpr int kBins = 641;
    (void)n;
    for (int frame = 0; frame < frames; ++frame) {
      const float* x = wave + frame * kHop;
      std::vector<std::complex<double>> spec(kN);
      for (int t = 0; t < kN; ++t) {
        spec[static_cast<size_t>(t)] = static_cast<double>(x[t]) * dft_win_[static_cast<size_t>(t)];
      }
      Fft(&spec);
      for (int k = 0; k < kBins; ++k) {
        const double re = spec[static_cast<size_t>(k)].real();
        const double im = spec[static_cast<size_t>(k)].imag();
        const double mag = std::sqrt(re * re + im * im);
        const double den = mag < 1e-5 ? 1e-5 : mag;
        feat[static_cast<size_t>(k * frames + frame)] = static_cast<float>(std::log1p(mag));
        feat[static_cast<size_t>((kBins + k) * frames + frame)] = static_cast<float>(re / den);
        feat[static_cast<size_t>((2 * kBins + k) * frames + frame)] = static_cast<float>(im / den);
      }
    }
  }

  void ZeroState() {
    std::fill(hist_.begin(), hist_.end(), 0.f);
    std::fill(k_.begin(), k_.end(), 0.f);
    std::fill(v_.begin(), v_.end(), 0.f);
    std::fill(conv_.begin(), conv_.end(), 0.f);
    std::fill(pos_.begin(), pos_.end(), 0.f);
  }

  void Release() {
    if (!api_) return;
    if (enc_) api_->ReleaseSession(enc_);
    if (dec_) api_->ReleaseSession(dec_);
    if (opts_) api_->ReleaseSessionOptions(opts_);
    if (mem_) api_->ReleaseMemoryInfo(mem_);
    if (env_) api_->ReleaseEnv(env_);
    enc_ = dec_ = nullptr;
    opts_ = nullptr;
    mem_ = nullptr;
    env_ = nullptr;
  }

  std::mutex mu_;
  const OrtApi* api_ = nullptr;
  OrtEnv* env_ = nullptr;
  OrtSessionOptions* opts_ = nullptr;
  OrtMemoryInfo* mem_ = nullptr;
  OrtSession* enc_ = nullptr;
  OrtSession* dec_ = nullptr;
  ModelPaths paths_;
  ChunkConfig chunk_;
  int cache_ = 1024;
  int lookback_ = 1120;
  int step_ = 2400;
  int bad_codes_ = 0;
  std::vector<float> hist_, k_, v_, conv_, pos_;
  std::vector<double> dft_win_;
  LiquidTail tail_;
  std::vector<int16_t> fifo_;
};

}  // namespace

std::unique_ptr<LiquidBackend> CreateOnnxBackend() {
  return std::make_unique<OnnxBackend>();
}

}  // namespace liquid
}  // namespace webrtc
