/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */
// Signaling event hook for apps that show an offer/answer/ICE log.
// direct.framework exports DirectSetSignalingLogCallback. Events:
//   "local-offer", "local-answer", "remote-offer", "remote-answer"  text = full SDP
//   "local-ice", "remote-ice"                                         text = "<mline>:<candidate>"
//   "signaling-state", "ice-state", "pc-state"                        text = state name
//   "message-out", "message-in"                                       text = control message
//                                                                     (INVITE/OK/BYE...; SDP
//                                                                     and ICE are reported above)
// The callback runs on a WebRTC thread; copy the strings and return quickly.
// The text is unredacted (includes ICE credentials) and is never logged by
// this hook itself; it only reaches a callback the app installed.

#ifndef EXAMPLES_DIRECT_SIGNALING_LOG_H_
#define EXAMPLES_DIRECT_SIGNALING_LOG_H_

#if defined(__GNUC__)
#define DIRECT_SIGNALING_API __attribute__((visibility("default")))
#else
#define DIRECT_SIGNALING_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DirectSignalingLogCallback)(void* ctx, const char* event,
                                           const char* text);

// Pass NULL to remove. Thread-safe.
DIRECT_SIGNALING_API void DirectSetSignalingLogCallback(
    DirectSignalingLogCallback callback, void* ctx);

#ifdef __cplusplus
}

#include <string>
// Internal: deliver one event to the installed callback (no-op when none).
void DirectSignalingLog(const char* event, const std::string& text);
#endif

#endif  // EXAMPLES_DIRECT_SIGNALING_LOG_H_
