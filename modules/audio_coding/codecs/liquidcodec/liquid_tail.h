#ifndef MODULES_AUDIO_CODING_CODECS_LIQUIDCODEC_LIQUID_TAIL_H_
#define MODULES_AUDIO_CODING_CODECS_LIQUIDCODEC_LIQUID_TAIL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "modules/audio_coding/codecs/liquidcodec/liquid_codec.h"

namespace webrtc {
namespace liquid {

// CPU fp32 tail shared with the published Liquid-Codec decoder:
// linear 512 -> 1282, exp/polar, inverse real STFT (n_fft 1280, hop 320, Hann),
// overlap-add. The Core ML package stops at the 512-d hidden state.
class LiquidTail {
 public:
  bool Load(const std::string& path, std::string* error);
  void SetLinear(std::vector<float> weight_1282x512, std::vector<float> bias);

  // hid is channel-major (512, n75). Returns newly finalized PCM in about [-1, 1].
  std::vector<float> PushHidden(const float* hid, int n75);
  // y is (1282, n75), already the linear output. Used by PushHidden and tests.
  std::vector<float> PushLinear(const float* y, int n75);
  std::vector<float> Finalize();
  void Reset();

 private:
  void EnsureBasis();
  std::vector<float> Emit(bool finalize);

  std::vector<float> weight_;
  std::vector<float> bias_;
  std::vector<float> basis_;
  std::vector<float> window_;
  std::vector<float> acc_;
  std::vector<float> env_;
  int n75_ = 0;
  int emitted_ = kTrim;
};

}  // namespace liquid
}  // namespace webrtc

#endif
