/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

// Echo path bookkeeping for the stats line: which echo canceller the call
// uses and AEC3's own statistics when the software canceller runs.

#ifndef EXAMPLES_DIRECT_ECHO_STATS_H_
#define EXAMPLES_DIRECT_ECHO_STATS_H_

#include <memory>
#include <string>

#include "api/audio/audio_processing.h"
#include "api/audio/audio_processing_statistics.h"

// "vpio" (macOS/iOS voice-processing I/O unit, Apple's canceller),
// "aec3" (WebRTC software AEC3 in APM), or "off".
void DirectSetEchoPath(const std::string& path);
std::string DirectEchoPath();

// Wraps an APM builder so the stats loop can read the APM it built.
std::unique_ptr<webrtc::AudioProcessingBuilderInterface> DirectWrapApmBuilder(
    std::unique_ptr<webrtc::AudioProcessingBuilderInterface> inner);

// AEC3 statistics of the last APM built. Returns false when there is none.
bool DirectApmStats(webrtc::AudioProcessingStats* out);

#endif  // EXAMPLES_DIRECT_ECHO_STATS_H_
