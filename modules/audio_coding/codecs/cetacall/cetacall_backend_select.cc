#include "modules/audio_coding/codecs/cetacall/cetacall_backend.h"

namespace webrtc {
namespace cetacall {

std::unique_ptr<CetaCallBackend> CreateCetaCallBackend() {
#if defined(WEBRTC_CETACALL_ONNX)
  return CreateOnnxBackend();
#else
  return CreateCoreMlBackend();
#endif
}

}  // namespace cetacall
}  // namespace webrtc
