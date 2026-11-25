// In-process CetaCall / Opus quality entry point.
// directcall --quality=offline|factory calls DirectQualityRun.
// The iOS direct.framework exports the same function. RobotPhone's XCTest
// (scripts/cetacall/ios/CetaCallQualityTests.mm) calls it and writes the
// CSV into the app Documents directory. The simulator has no ANE; pass
// compute "cpu" there and "ane" only on a device.

#ifndef EXAMPLES_DIRECT_QUALITY_H_
#define EXAMPLES_DIRECT_QUALITY_H_

#if defined(__GNUC__)
#define DIRECT_QUALITY_API __attribute__((visibility("default")))
#else
#define DIRECT_QUALITY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// stage: "offline" (codec round-trip, no WebRTC) or "factory" (AudioEncoder,
// AudioDecoder, and NetEq, no network).
// codec: "cetacall" or "opus".
// compute: "cpu", "ane", or "all" (cetacall decoder units).
// encoder_precision: "int8", "fp16", or "fp32".
// Returns 0 on a completed run, 3 when a model is intentionally absent
// (fp32 reference), 2 when this build has no CetaCall, 1 on failure.
// Writes quality_stats-style JSON to stats_json and PCM WAV to wav_out.
DIRECT_QUALITY_API int DirectQualityRun(const char* stage, const char* wav_in,
                                        const char* wav_out, const char* stats_json,
                                        const char* codec, const char* model_dir,
                                        const char* compute, const char* encoder_precision,
                                        int opus_bitrate_bps, int loss_pct, int jitter_ms);

// Jitter buffer for the factory stage's NetEq: max_delay_ms < 0 keeps the
// 500 ms default, 0 is no cap. directcall --quality passes --jitter-max-ms.
DIRECT_QUALITY_API void DirectQualitySetJitterBuffer(int max_delay_ms, int fast_accelerate);
// Encoder compute units for --quality runs: "cpu" (default), "ane", "all".
DIRECT_QUALITY_API void DirectQualitySetEncoderCompute(const char* units);

// Offline echo-canceller loop (directcall --quality=aec): render_wav plays
// through a simulated speaker-to-mic path into APM with AEC3; prints ERLE per
// second and writes aec_stats.json and aec_out.wav to out_dir.
DIRECT_QUALITY_API int DirectAecSelfTest(const char* render_wav, const char* out_dir);

#ifdef __cplusplus
}
#endif

#endif
