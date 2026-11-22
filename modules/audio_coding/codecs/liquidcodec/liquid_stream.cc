#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#include "modules/audio_coding/codecs/liquidcodec/liquid_stream.h"

#include <cstring>
#include <utility>

namespace webrtc {
namespace liquid {

StreamEncoder::StreamEncoder(LiquidBackend* backend, ChunkConfig chunk, ModelPaths paths)
    : backend_(backend), chunk_(chunk), paths_(std::move(paths)) {}

bool StreamEncoder::Push(const int16_t* pcm, size_t n,
                         std::vector<std::array<int32_t, kCodebooks>>* codes,
                         WindowTiming* timing, std::string* error) {
  if (!backend_ || !codes) return false;
  if (n && pcm) pending_.insert(pending_.end(), pcm, pcm + n);
  if (paths_.Stateful()) return PushStateful(codes, timing, error);
  return PushWindow(codes, timing, error);
}

bool StreamEncoder::PushStateful(std::vector<std::array<int32_t, kCodebooks>>* codes,
                                 WindowTiming* timing, std::string* error) {
  // One codec frame plus real lookahead. The lookahead is the next audio,
  // never a reflected copy. The model applies its own left pad on the first
  // call when the state tensors are zero.
  const int frame = chunk_.frame_samples;
  const int step = paths_.EncoderStepSamples(frame);
  const int lookahead = step - frame;
  if (lookahead < 0 || step <= 0) {
    if (error) *error = "stateful encoder step is shorter than one codec frame";
    return false;
  }
  while (static_cast<int>(pending_.size()) >= step) {
    std::vector<int32_t> out(kCodebooks);
    if (!backend_->EncodeWindow(pending_.data(), chunk_, out.data(), timing, error)) {
      // The backend zeros its state before returning. Drop this frame and
      // keep the call up. Returning false made every later 10 ms look like
      // a fatal encode error.
      pending_.erase(pending_.begin(), pending_.begin() + frame);
      return true;
    }
    std::array<int32_t, kCodebooks> one{};
    for (int c = 0; c < kCodebooks; ++c) one[static_cast<size_t>(c)] = out[static_cast<size_t>(c)];
    codes->push_back(one);
    pending_.erase(pending_.begin(), pending_.begin() + frame);
  }
  return true;
}

bool StreamEncoder::PushWindow(std::vector<std::array<int32_t, kCodebooks>>* codes,
                               WindowTiming* timing, std::string* error) {
  const int need = chunk_.new_frames * chunk_.frame_samples;
  const int ctx_samples = chunk_.ctx_frames * chunk_.frame_samples;
  if (static_cast<int>(pending_.size()) < need) return true;

  std::vector<int16_t> window(static_cast<size_t>(ctx_samples + need), 0);
  if (ctx_samples > 0) {
    const int have = static_cast<int>(history_.size());
    const int copy = have < ctx_samples ? have : ctx_samples;
    if (copy > 0) {
      std::memcpy(window.data() + (ctx_samples - copy),
                  history_.data() + (have - copy),
                  static_cast<size_t>(copy) * sizeof(int16_t));
    }
  }
  std::memcpy(window.data() + ctx_samples, pending_.data(),
              static_cast<size_t>(need) * sizeof(int16_t));

  std::vector<int32_t> out(static_cast<size_t>(chunk_.new_frames * kCodebooks));
  if (!backend_->EncodeWindow(window.data(), chunk_, out.data(), timing, error)) {
    return false;
  }
  for (int f = 0; f < chunk_.new_frames; ++f) {
    std::array<int32_t, kCodebooks> frame{};
    for (int c = 0; c < kCodebooks; ++c) {
      frame[static_cast<size_t>(c)] = out[static_cast<size_t>(f * kCodebooks + c)];
    }
    codes->push_back(frame);
  }
  history_.insert(history_.end(), pending_.begin(), pending_.begin() + need);
  if (static_cast<int>(history_.size()) > ctx_samples) {
    history_.erase(history_.begin(),
                   history_.end() - ctx_samples);
  }
  pending_.erase(pending_.begin(), pending_.begin() + need);
  return true;
}

void StreamEncoder::Reset() {
  history_.clear();
  pending_.clear();
  if (backend_) backend_->Reset();
}

StreamDecoder::StreamDecoder(LiquidBackend* backend, ChunkConfig chunk)
    : backend_(backend), chunk_(chunk) {}

bool StreamDecoder::PushFrame(const int32_t* codes12, std::vector<int16_t>* pcm,
                              WindowTiming* timing, std::string* error) {
  if (!backend_ || !pcm || !codes12) return false;
  pending_.insert(pending_.end(), codes12, codes12 + kCodebooks);
  ++frames_seen_;
  if (static_cast<int>(pending_.size()) < chunk_.new_frames * kCodebooks) return true;

  const int fr = chunk_.WindowFrames();
  // -1 marks left-context padding. A real all-zero code is still audio.
  std::vector<int32_t> window(static_cast<size_t>(fr * kCodebooks), -1);
  const int ctx_codes = chunk_.ctx_frames * kCodebooks;
  const int have = static_cast<int>(history_.size());
  const int copy = have < ctx_codes ? have : ctx_codes;
  if (copy > 0) {
    std::memcpy(window.data() + (ctx_codes - copy),
                history_.data() + (have - copy),
                static_cast<size_t>(copy) * sizeof(int32_t));
  }
  std::memcpy(window.data() + ctx_codes, pending_.data(),
              static_cast<size_t>(chunk_.new_frames * kCodebooks) * sizeof(int32_t));

  std::vector<int16_t> audio(static_cast<size_t>(chunk_.new_frames * chunk_.frame_samples));
  if (!backend_->DecodeWindow(window.data(), chunk_, audio.data(), timing, error)) {
    return false;
  }
  pcm->insert(pcm->end(), audio.begin(), audio.end());
  history_.insert(history_.end(), pending_.begin(), pending_.end());
  if (static_cast<int>(history_.size()) > ctx_codes) {
    history_.erase(history_.begin(), history_.end() - ctx_codes);
  }
  pending_.clear();
  return true;
}

void StreamDecoder::Reset() {
  history_.clear();
  pending_.clear();
  frames_seen_ = 0;
  if (backend_) backend_->Reset();
}

}  // namespace liquid
}  // namespace webrtc
