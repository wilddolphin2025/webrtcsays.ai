/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#ifndef EXAMPLES_DIRECT_REMOTE_CHOICE_H_
#define EXAMPLES_DIRECT_REMOTE_CHOICE_H_

#include <string>

// What can actually present remote pixels. kNone means no window is opened.
enum class RemoteWindowChoice {
  kNone = 0,
  kOpenVr,
  kSdl,
};

// `openvr_ready` means IVROverlay_028 loaded. `display_ready` means SDL2
// loaded and DISPLAY is set. A failed OpenVR request does not become an SDL
// window: that window is not a headset panel.
inline RemoteWindowChoice ChooseRemoteWindow(const std::string& requested,
                                             bool openvr_ready,
                                             bool display_ready) {
  if (requested == "none") return RemoteWindowChoice::kNone;
  if (requested == "openvr") {
    return openvr_ready ? RemoteWindowChoice::kOpenVr : RemoteWindowChoice::kNone;
  }
  if (requested == "sdl") {
    return display_ready ? RemoteWindowChoice::kSdl : RemoteWindowChoice::kNone;
  }
  if (openvr_ready) return RemoteWindowChoice::kOpenVr;
  if (display_ready) return RemoteWindowChoice::kSdl;
  return RemoteWindowChoice::kNone;
}

inline const char* RemoteWindowName(RemoteWindowChoice choice) {
  switch (choice) {
    case RemoteWindowChoice::kOpenVr:
      return "openvr";
    case RemoteWindowChoice::kSdl:
      return "sdl";
    case RemoteWindowChoice::kNone:
      return "none";
  }
  return "none";
}

#endif  // EXAMPLES_DIRECT_REMOTE_CHOICE_H_
