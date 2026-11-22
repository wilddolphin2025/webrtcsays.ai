/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */
#include "signaling_log.h"

#include <mutex>

namespace {
// Leaked on purpose: no exit-time destructor.
std::mutex& Mu() {
  static std::mutex* mu = new std::mutex;
  return *mu;
}
DirectSignalingLogCallback g_cb = nullptr;
void* g_ctx = nullptr;
}  // namespace

extern "C" void DirectSetSignalingLogCallback(DirectSignalingLogCallback callback,
                                              void* ctx) {
  std::lock_guard<std::mutex> lock(Mu());
  g_cb = callback;
  g_ctx = ctx;
}

void DirectSignalingLog(const char* event, const std::string& text) {
  DirectSignalingLogCallback cb;
  void* ctx;
  {
    std::lock_guard<std::mutex> lock(Mu());
    cb = g_cb;
    ctx = g_ctx;
  }
  if (cb) cb(ctx, event, text.c_str());
}
