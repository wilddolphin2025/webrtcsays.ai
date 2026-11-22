#ifndef EXAMPLES_DIRECT_AUDIO_TAP_H_
#define EXAMPLES_DIRECT_AUDIO_TAP_H_

#include <string>

#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/scoped_refptr.h"

namespace webrtc {

// Wraps an encoder and decoder factory so a quality run can record the PCM
// that entered the encoder and the PCM that left the decoder, and so
// `--inject-wav` replaces the microphone for every codec (Opus and LiquidCodec).
scoped_refptr<AudioEncoderFactory> CreateTappingAudioEncoderFactory(
    scoped_refptr<AudioEncoderFactory> inner, const std::string& record_dir,
    const std::string& inject_wav);

// voice_swap_udp > 0: every decoded block is also sent to 127.0.0.1:voice_swap_udp as datagrams of
// [uint32 sample_rate][int16 mono PCM, <= 10 ms] for the voice-swap sidecar; voice_swap_mute then zeroes the
// block before playout so only the sidecar's converted voice is heard.
scoped_refptr<AudioDecoderFactory> CreateTappingAudioDecoderFactory(
    scoped_refptr<AudioDecoderFactory> inner, const std::string& record_dir, int voice_swap_udp = 0,
    bool voice_swap_mute = true);

}  // namespace webrtc

#endif
