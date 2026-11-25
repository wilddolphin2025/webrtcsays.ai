#ifndef MODULES_AUDIO_CODING_CODECS_CETACALL_LIQUID_BACKEND_H_
#define MODULES_AUDIO_CODING_CODECS_CETACALL_LIQUID_BACKEND_H_

#include <cstdint>
#include <memory>
#include <string>

#include "modules/audio_coding/codecs/cetacall/cetacall_codec.h"

namespace webrtc {
namespace cetacall {

// One Core ML (or test) call. The stream layer owns the chunk schedule.
// EncodeWindow PCM is mono int16 at 24 kHz.
//   encoder_window "ctx+new": (ctx+new) * frame_samples, left-padded with 0.
//   encoder_window "new": the same buffer is passed; the backend reads only
//   the last new_frames * frame_samples.
// codes_out is frame-major, new_frames * 12, values in [0, codebook_size).
//
// DecodeWindow codes are frame-major, (ctx+new) * 12.
// pcm_out receives exactly new_frames * frame_samples samples.
struct WindowTiming {
  double window_ms = 0;
  double prediction_ms = 0;
};

class CetaCallBackend {
 public:
  virtual ~CetaCallBackend() = default;
  virtual bool Start(const ModelPaths& paths, const ChunkConfig& chunk,
                     std::string* error) = 0;
  virtual bool EncodeWindow(const int16_t* pcm, const ChunkConfig& chunk,
                            int32_t* codes_out, WindowTiming* timing,
                            std::string* error) = 0;
  virtual bool DecodeWindow(const int32_t* codes, const ChunkConfig& chunk,
                            int16_t* pcm_out, WindowTiming* timing,
                            std::string* error) = 0;
  virtual void Reset() = 0;
  virtual const char* name() const = 0;
  // Codes the encoder refused (farther than one int8 step from the codebook).
  virtual int out_of_codebook() const { return 0; }
};

std::unique_ptr<CetaCallBackend> CreateCoreMlBackend();

}  // namespace cetacall
}  // namespace webrtc

#endif
