#include "modules/audio_coding/codecs/liquidcall/liquid_backend.h"

namespace webrtc {
namespace liquid {

std::unique_ptr<LiquidBackend> CreateLiquidBackend() {
#if defined(WEBRTC_LIQUID_ONNX)
  return CreateOnnxBackend();
#else
  return CreateCoreMlBackend();
#endif
}

}  // namespace liquid
}  // namespace webrtc
