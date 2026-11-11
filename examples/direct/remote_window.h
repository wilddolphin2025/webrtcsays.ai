/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#ifndef EXAMPLES_DIRECT_REMOTE_WINDOW_H_
#define EXAMPLES_DIRECT_REMOTE_WINDOW_H_

#include <memory>
#include <string>

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

// Opens an OpenVR overlay when IVROverlay_028 is available, otherwise an SDL2
// window when DISPLAY is set. Returns null when nothing can show pixels.
// An SDL window is a desktop window. It is not a SteamVR compositor view.
// `frame_path` is used when requested is "app": the latest remote frame is
// written there for the fullscreen call app. No OpenVR overlay is created.
// `slot_w` and `slot_h` are the call-view video rectangle. When both are
// positive, frames are scaled into that size in this process so the app
// copies instead of resampling.
std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> StartRemoteVideoWindow(
    const std::string& requested, const std::string& frame_path, int slot_w, int slot_h);

#endif  // EXAMPLES_DIRECT_REMOTE_WINDOW_H_
