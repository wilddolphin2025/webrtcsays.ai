#include "modules/audio_coding/codecs/liquidcodec/liquid_tail.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace webrtc {
namespace liquid {
namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

bool LiquidTail::Load(const std::string& path, std::string* error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) *error = "cannot open tail " + path;
    return false;
  }
  const size_t n = static_cast<size_t>(kLinear) * (static_cast<size_t>(kHidden) + 1);
  std::vector<float> all(n);
  const size_t got = std::fread(all.data(), sizeof(float), n, f);
  std::fclose(f);
  if (got != n) {
    if (error) *error = "tail file is the wrong size: " + path;
    return false;
  }
  weight_.assign(all.begin(), all.begin() + kLinear * kHidden);
  bias_.assign(all.begin() + kLinear * kHidden, all.end());
  EnsureBasis();
  return true;
}

void LiquidTail::SetLinear(std::vector<float> weight_1282x512,
                           std::vector<float> bias) {
  weight_ = std::move(weight_1282x512);
  bias_ = std::move(bias);
  EnsureBasis();
}

void LiquidTail::EnsureBasis() {
  if (!basis_.empty()) return;
  basis_.assign(static_cast<size_t>(kFft) * kLinear, 0.f);
  window_.assign(kFft, 0.f);
  for (int t = 0; t < kFft; ++t) {
    window_[static_cast<size_t>(t)] =
        0.5f - 0.5f * std::cos(2.f * kPi * static_cast<float>(t) /
                               static_cast<float>(kFft));
  }
  for (int t = 0; t < kFft; ++t) {
    for (int k = 0; k < kBins; ++k) {
      const float ck = (k == 0 || k == kBins - 1) ? 1.f : 2.f;
      const float ang = 2.f * kPi * static_cast<float>(k * t) /
                        static_cast<float>(kFft);
      const float w = window_[static_cast<size_t>(t)];
      const size_t row = static_cast<size_t>(t) * kLinear;
      basis_[row + static_cast<size_t>(k)] = ck * std::cos(ang) / kFft * w;
      basis_[row + static_cast<size_t>(kBins + k)] =
          (k == 0 || k == kBins - 1) ? 0.f : -ck * std::sin(ang) / kFft * w;
    }
  }
}

std::vector<float> LiquidTail::PushHidden(const float* hid, int n75) {
  if (!hid || n75 <= 0 || weight_.size() != static_cast<size_t>(kLinear * kHidden) ||
      bias_.size() != static_cast<size_t>(kLinear)) {
    return {};
  }
  std::vector<float> y(static_cast<size_t>(kLinear) * static_cast<size_t>(n75));
  for (int t = 0; t < n75; ++t) {
    for (int o = 0; o < kLinear; ++o) {
      double acc = bias_[static_cast<size_t>(o)];
      const float* row = weight_.data() + static_cast<size_t>(o) * kHidden;
      for (int i = 0; i < kHidden; ++i) {
        acc += static_cast<double>(row[i]) *
               static_cast<double>(hid[static_cast<size_t>(i) * n75 + t]);
      }
      y[static_cast<size_t>(o) * n75 + t] = static_cast<float>(acc);
    }
  }
  return PushLinear(y.data(), n75);
}

std::vector<float> LiquidTail::PushLinear(const float* y, int n75) {
  if (!y || n75 <= 0) return {};
  EnsureBasis();
  const int old = n75_;
  n75_ += n75;
  const int out_len = (n75_ - 1) * kHop + kFft;
  if (static_cast<int>(acc_.size()) < out_len) {
    acc_.resize(out_len, 0.f);
    env_.resize(out_len, 0.f);
  }
  std::vector<float> real(kBins);
  std::vector<float> imag(kBins);
  std::vector<float> frame(kFft);
  for (int t = 0; t < n75; ++t) {
    for (int k = 0; k < kBins; ++k) {
      const float logmag = y[static_cast<size_t>(k) * n75 + t];
      const float ang = y[static_cast<size_t>(kBins + k) * n75 + t];
      const float mag = std::exp(logmag);
      real[static_cast<size_t>(k)] = mag * std::cos(ang);
      imag[static_cast<size_t>(k)] = mag * std::sin(ang);
    }
    for (int i = 0; i < kFft; ++i) {
      const float* row = basis_.data() + static_cast<size_t>(i) * kLinear;
      double s = 0;
      for (int k = 0; k < kBins; ++k) {
        s += static_cast<double>(row[k]) * real[static_cast<size_t>(k)];
        s += static_cast<double>(row[kBins + k]) * imag[static_cast<size_t>(k)];
      }
      frame[static_cast<size_t>(i)] = static_cast<float>(s);
    }
    const int at = (old + t) * kHop;
    for (int i = 0; i < kFft; ++i) {
      acc_[static_cast<size_t>(at + i)] += frame[static_cast<size_t>(i)];
      const float w = window_[static_cast<size_t>(i)];
      env_[static_cast<size_t>(at + i)] += w * w;
    }
  }
  return Emit(false);
}

std::vector<float> LiquidTail::Finalize() { return Emit(true); }

void LiquidTail::Reset() {
  acc_.clear();
  env_.clear();
  n75_ = 0;
  emitted_ = kTrim;
}

std::vector<float> LiquidTail::Emit(bool finalize) {
  if (n75_ <= 0) return {};
  const int ready = finalize ? ((n75_ - 1) * kHop + kFft - kTrim) : (n75_ * kHop);
  if (ready <= emitted_) return {};
  std::vector<float> out;
  out.reserve(static_cast<size_t>(ready - emitted_));
  for (int i = emitted_; i < ready; ++i) {
    const float e = env_[static_cast<size_t>(i)];
    out.push_back(e > 1e-8f ? acc_[static_cast<size_t>(i)] / e : 0.f);
  }
  emitted_ = ready;
  return out;
}

}  // namespace liquid
}  // namespace webrtc
