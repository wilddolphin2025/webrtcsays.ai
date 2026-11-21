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

scoped_refptr<AudioDecoderFactory> CreateTappingAudioDecoderFactory(
    scoped_refptr<AudioDecoderFactory> inner, const std::string& record_dir);

}  // namespace webrtc

#endif
