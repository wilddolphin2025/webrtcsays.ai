#ifndef MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_LIQUID_FACTORY_H_
#define MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_LIQUID_FACTORY_H_

#include <string>

#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/scoped_refptr.h"
#include "modules/audio_coding/codecs/liquidcall/audio_encoder_liquid.h"

namespace webrtc {

// Checks that the fetched model directory matches the manifest, then stores
// the runtime config the factories read. Does not convert models.
bool ConfigureLiquidRuntime(const liquid::RuntimeConfig& config, std::string* error);

scoped_refptr<AudioEncoderFactory> CreateLiquidAudioEncoderFactory();
scoped_refptr<AudioDecoderFactory> CreateLiquidAudioDecoderFactory();

}  // namespace webrtc

#endif
