#ifndef MODULES_AUDIO_CODING_CODECS_CETACALL_LIQUID_FACTORY_H_
#define MODULES_AUDIO_CODING_CODECS_CETACALL_LIQUID_FACTORY_H_

#include <string>

#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/scoped_refptr.h"
#include "modules/audio_coding/codecs/cetacall/audio_encoder_cetacall.h"

namespace webrtc {

// Checks that the fetched model directory matches the manifest, then stores
// the runtime config the factories read. Does not convert models.
bool ConfigureCetaCallRuntime(const cetacall::RuntimeConfig& config, std::string* error);

// CETACALL only. Quality runs use this.
scoped_refptr<AudioEncoderFactory> CreateCetaCallAudioEncoderFactory();
scoped_refptr<AudioDecoderFactory> CreateCetaCallAudioDecoderFactory();

// CETACALL first, then every codec `builtin` already supports. Opus stays,
// so a cetacall call can send CETACALL and decode Opus. `builtin` is the
// factory from CreateBuiltinAudioEncoderFactory / CreateBuiltinAudioDecoderFactory.
scoped_refptr<AudioEncoderFactory> CreateCetaCallAudioEncoderFactory(
    scoped_refptr<AudioEncoderFactory> builtin);
scoped_refptr<AudioDecoderFactory> CreateCetaCallAudioDecoderFactory(
    scoped_refptr<AudioDecoderFactory> builtin);

}  // namespace webrtc

#endif
