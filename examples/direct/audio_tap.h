#ifndef EXAMPLES_DIRECT_AUDIO_TAP_H_
#define EXAMPLES_DIRECT_AUDIO_TAP_H_

#include <string>

#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/scoped_refptr.h"

namespace webrtc {

// Wraps an encoder and decoder factory so a quality run can record the PCM
// that entered the encoder and the PCM that left the decoder, and so
// `--inject-wav` replaces the microphone for every codec (Opus and CetaCall).
// voice_swap_return_udp > 0: accept [uint32 sample_rate][int16 mono PCM, <= 10 ms]
// datagrams from the translation sidecar on 127.0.0.1:<port> and encode them
// instead of the microphone. The stream is 48 kHz mono; underruns encode silence.
// voice_swap_mic_udp > 0: also send the original encoder input (the processed
// microphone) to 127.0.0.1:<port> in the same datagram format, before replacement.
scoped_refptr<AudioEncoderFactory> CreateTappingAudioEncoderFactory(
    scoped_refptr<AudioEncoderFactory> inner, const std::string& record_dir,
    const std::string& inject_wav, int voice_swap_return_udp = 0, int voice_swap_mic_udp = 0);

// voice_swap_udp > 0: every decoded block is also sent to 127.0.0.1:voice_swap_udp as datagrams of
// [uint32 sample_rate][int16 mono PCM, <= 10 ms] for the voice-swap sidecar; voice_swap_mute then zeroes the
// block before playout so only the sidecar's converted voice is heard.
scoped_refptr<AudioDecoderFactory> CreateTappingAudioDecoderFactory(
    scoped_refptr<AudioDecoderFactory> inner, const std::string& record_dir, int voice_swap_udp = 0,
    bool voice_swap_mute = true);

// Voice the voice-swap sidecar should speak for the current call: "" = no conversion (decoded audio is
// played as is and not sent). Process-wide; the callee sets it from the caller's INVITE or its own
// --voice-swap override. Tap decoders send "VOICE:<voice|none>" to the sidecar when it changes.
void SetVoiceSwapVoice(const std::string& voice);
std::string GetVoiceSwapVoice();
// Callee policy: the voice to use given our --voice-swap option, the caller's request (the offer's
// a=x-cetacall-voice) and the voices our sidecar has (--voice-swap-voices, comma list). A non-auto option wins;
// a request we don't have (or none) means no conversion. An empty list accepts any request.
std::string ResolveVoiceSwap(const std::string& callee_option, const std::string& caller_request,
                             const std::string& available = std::string());
// Voice negotiation, SDP only (audio m-section, keyed to the CETACALL payload type <pt>):
//   offer  a=x-cetacall-voice:<pt> female           caller's choice; absent or "me" = no conversion
//   answer a=x-cetacall-voices:<pt> me,female,male  callee's preloaded voices (only with a sidecar)
//          a=x-cetacall-voice:<pt> female           callee's active voice ("me" when it lacks the one asked)
// WebRTC ignores unknown media attributes, so peers without voice support are unaffected.
inline constexpr char kVoiceMe[] = "me";
inline constexpr char kVoiceSdpAttr[] = "a=x-cetacall-voice:";
inline constexpr char kVoicesSdpAttr[] = "a=x-cetacall-voices:";
std::string AddVoiceRequestToSdp(const std::string& sdp, const std::string& voice);
std::string AddVoicesToSdp(const std::string& sdp, const std::string& voices, const std::string& active);
std::string VoicesFromSdp(const std::string& sdp);   // "" when absent
std::string VoiceFromSdp(const std::string& sdp);    // "" when absent
// Keep [A-Za-z0-9_.-], at most 64 chars (the value travels in signaling JSON and to the sidecar).
std::string SanitizeVoiceName(const std::string& voice);

}  // namespace webrtc

#endif
