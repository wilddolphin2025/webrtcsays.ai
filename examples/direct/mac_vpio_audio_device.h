/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

// macOS audio device on Apple's Voice Processing I/O unit (VPIO), the
// canceller FaceTime uses. The stock macOS ADM (CoreAudio HAL) has no echo
// cancellation of its own and relies on WebRTC's software AEC3. VPIO
// cancels against its own speaker output, so the reference is exactly what
// the speaker played, whatever NetEq did to the stream before it.

#ifndef EXAMPLES_DIRECT_MAC_VPIO_AUDIO_DEVICE_H_
#define EXAMPLES_DIRECT_MAC_VPIO_AUDIO_DEVICE_H_

#if defined(WEBRTC_MAC) && !defined(WEBRTC_IOS)

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/scoped_refptr.h"

// Returns an initialized ADM, or nullptr when the VPIO unit can't be
// created (the caller falls back to the HAL ADM plus AEC3).
webrtc::scoped_refptr<webrtc::AudioDeviceModule>
CreateMacVoiceProcessingAudioDeviceModule(const webrtc::Environment& env);

#endif  // WEBRTC_MAC && !WEBRTC_IOS
#endif  // EXAMPLES_DIRECT_MAC_VPIO_AUDIO_DEVICE_H_
