/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#ifndef EXAMPLES_DIRECT_PATTERN_VIDEO_H_
#define EXAMPLES_DIRECT_PATTERN_VIDEO_H_

#include <memory>

#include "api/scoped_refptr.h"
#include "pc/test/fake_video_track_source.h"

// Pumps I420 frames into a FakeVideoTrackSource at a fixed rate.
// `screen` grabs the X11 root window when DISPLAY is set and falls back to
// a moving color-bar pattern when it is not (headless Steam Frame).
class DirectVideoPump {
 public:
  virtual ~DirectVideoPump() = default;
  virtual void Stop() = 0;
};

std::unique_ptr<DirectVideoPump> StartDirectVideoPump(
    webrtc::scoped_refptr<webrtc::FakeVideoTrackSource> source,
    int width,
    int height,
    int fps,
    bool screen);

#endif  // EXAMPLES_DIRECT_PATTERN_VIDEO_H_
