/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#include "echo_stats.h"

#include <mutex>
#include <utility>

#include "api/scoped_refptr.h"

namespace {

struct EchoState {
  std::mutex mu;
  std::string path = "off";
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
};

EchoState& State() {
  static EchoState* state = new EchoState();  // never destroyed
  return *state;
}

class RememberingApmBuilder : public webrtc::AudioProcessingBuilderInterface {
 public:
  explicit RememberingApmBuilder(
      std::unique_ptr<webrtc::AudioProcessingBuilderInterface> inner)
      : inner_(std::move(inner)) {}

  webrtc::scoped_refptr<webrtc::AudioProcessing> Build(
      const webrtc::Environment& env) override {
    auto apm = inner_->Build(env);
    std::lock_guard<std::mutex> lock(State().mu);
    State().apm = apm;
    return apm;
  }

 private:
  std::unique_ptr<webrtc::AudioProcessingBuilderInterface> inner_;
};

}  // namespace

void DirectSetEchoPath(const std::string& path) {
  std::lock_guard<std::mutex> lock(State().mu);
  State().path = path;
}

std::string DirectEchoPath() {
  std::lock_guard<std::mutex> lock(State().mu);
  return State().path;
}

std::unique_ptr<webrtc::AudioProcessingBuilderInterface> DirectWrapApmBuilder(
    std::unique_ptr<webrtc::AudioProcessingBuilderInterface> inner) {
  return std::make_unique<RememberingApmBuilder>(std::move(inner));
}

bool DirectApmStats(webrtc::AudioProcessingStats* out) {
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
  {
    std::lock_guard<std::mutex> lock(State().mu);
    apm = State().apm;
  }
  if (!apm) return false;
  *out = apm->GetStatistics();
  return true;
}
