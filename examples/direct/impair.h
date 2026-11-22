#ifndef EXAMPLES_DIRECT_IMPAIR_H_
#define EXAMPLES_DIRECT_IMPAIR_H_

#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/scoped_refptr.h"

namespace webrtc {

// Wraps every encoder the inner factory creates. A dropped packet is not
// returned from Encode, so the RTP timestamp jumps and NetEq sees a gap.
// A delayed packet is returned on a later Encode with its original
// encoded_timestamp, so it arrives late relative to capture time.
scoped_refptr<AudioEncoderFactory> CreateImpairingAudioEncoderFactory(
    scoped_refptr<AudioEncoderFactory> inner, int loss_pct, int jitter_ms);

}  // namespace webrtc

#endif
