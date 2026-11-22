#ifndef MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_LIQUID_STREAM_H_
#define MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_LIQUID_STREAM_H_

#include <cstdint>
#include <vector>

#include "modules/audio_coding/codecs/liquidcall/liquid_backend.h"

namespace webrtc {
namespace liquid {

class StreamEncoder {
 public:
  StreamEncoder(LiquidBackend* backend, ChunkConfig chunk, ModelPaths paths = {});

  // Append 24 kHz samples. Completed code frames are appended to `codes`
  // (each entry is one frame of 12 codes, in time order).
  bool Push(const int16_t* pcm, size_t n, std::vector<std::array<int32_t, kCodebooks>>* codes,
            WindowTiming* timing, std::string* error);
  void Reset();

 private:
  bool PushStateful(std::vector<std::array<int32_t, kCodebooks>>* codes, WindowTiming* timing,
                    std::string* error);
  bool PushWindow(std::vector<std::array<int32_t, kCodebooks>>* codes, WindowTiming* timing,
                  std::string* error);

  LiquidBackend* backend_;
  ChunkConfig chunk_;
  ModelPaths paths_;
  std::vector<int16_t> history_;
  std::vector<int16_t> pending_;
};

class StreamDecoder {
 public:
  StreamDecoder(LiquidBackend* backend, ChunkConfig chunk);

  // One codec frame. Appends 24 kHz samples that this frame unlocked.
  // A chunked model returns nothing until `new_frames` have arrived, then
  // `new_frames` * frame_samples samples.
  bool PushFrame(const int32_t* codes12, std::vector<int16_t>* pcm,
                 WindowTiming* timing, std::string* error);
  void Reset();
  int frames_seen() const { return frames_seen_; }

 private:
  LiquidBackend* backend_;
  ChunkConfig chunk_;
  std::vector<int32_t> history_;
  std::vector<int32_t> pending_;
  int frames_seen_ = 0;
};

}  // namespace liquid
}  // namespace webrtc

#endif
