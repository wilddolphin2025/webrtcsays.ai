#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "modules/audio_coding/codecs/liquidcodec/liquid_backend.h"
#include "modules/audio_coding/codecs/liquidcodec/liquid_tail.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace liquid {
namespace {

void* RetainId(id obj) {
  if (!obj) return nullptr;
#if __has_feature(objc_arc)
  return (__bridge_retained void*)obj;
#else
  return (void*)[obj retain];
#endif
}

void ReleaseId(void* p) {
  if (!p) return;
#if __has_feature(objc_arc)
  (void)(__bridge_transfer id)p;
#else
  [(id)p release];
#endif
}

id ToId(void* p) {
#if __has_feature(objc_arc)
  return (__bridge id)p;
#else
  return (id)p;
#endif
}

uint16_t FloatToHalf(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000);
  int32_t exp = static_cast<int32_t>((bits >> 23) & 0xff) - 127 + 15;
  uint32_t mant = bits & 0x7fffff;
  if ((bits & 0x7fffffff) > 0x7f800000) return static_cast<uint16_t>(sign | 0x7e00);
  if (exp <= 0) {
    if (exp < -10) return sign;
    mant |= 0x800000;
    const uint32_t shift = static_cast<uint32_t>(1 - exp);
    uint32_t half = mant >> (shift + 13);
    if ((mant >> (shift + 12)) & 1) ++half;
    return static_cast<uint16_t>(sign | half);
  }
  if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00);
  uint16_t half = static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
  if (mant & 0x1000) ++half;
  return half;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t mant = h & 0x3ff;
  uint32_t bits = 0;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      exp = 127 - 15 + 1;
      while ((mant & 0x400) == 0) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x3ff;
      bits = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000 | (mant << 13);
  } else {
    bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  float out = 0;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

MLComputeUnits Units(const std::string& name) {
  if (name == "cpu") return MLComputeUnitsCPUOnly;
  if (name == "all") return MLComputeUnitsAll;
  return MLComputeUnitsCPUAndNeuralEngine;
}

std::string NsError(NSError* err) {
  if (!err) return "unknown Core ML error";
  return err.localizedDescription.UTF8String ? err.localizedDescription.UTF8String : "Core ML error";
}

// modelWithContentsOfURL rejects a .mlpackage ("Compile the model with Xcode
// or MLModel.compileModel(at:)"). Compile once and keep the .mlmodelc beside it.
bool PrepareModelPath(const std::string& path, std::string* load_path, std::string* error) {
  const std::string compiled = CompiledModelPath(path);
  if (compiled == path) {
    *load_path = path;
    return true;
  }
  if (FileExists(compiled)) {
    *load_path = compiled;
    return true;
  }
  @autoreleasepool {
    fprintf(stderr, "LiquidCodec compiling %s -> %s\n", path.c_str(), compiled.c_str());
    NSString* src = [NSString stringWithUTF8String:path.c_str()];
    NSURL* srcURL = [NSURL fileURLWithPath:src isDirectory:YES];
    NSError* err = nil;
    NSURL* tmp = [MLModel compileModelAtURL:srcURL error:&err];
    if (!tmp) {
      if (error) *error = "compile " + path + ": " + NsError(err);
      return false;
    }
    NSString* dst = [NSString stringWithUTF8String:compiled.c_str()];
    NSURL* dstURL = [NSURL fileURLWithPath:dst isDirectory:YES];
    NSFileManager* files = [NSFileManager defaultManager];
    NSError* moveErr = nil;
    if (![files moveItemAtURL:tmp toURL:dstURL error:&moveErr]) {
      moveErr = nil;
      if (![files copyItemAtURL:tmp toURL:dstURL error:&moveErr]) {
        if ([files fileExistsAtPath:dst]) {
          *load_path = compiled;
          return true;
        }
        if (error) *error = "save compiled model " + compiled + ": " + NsError(moveErr);
        return false;
      }
    }
  }
  *load_path = compiled;
  return true;
}

std::string JoinNames(NSDictionary<NSString*, MLFeatureDescription*>* map) {
  NSMutableString* s = [NSMutableString string];
  for (NSString* key in map) {
    if (s.length) [s appendString:@", "];
    [s appendString:key];
  }
  return s.length ? std::string(s.UTF8String) : std::string("(none)");
}

NSString* ResolveName(NSDictionary<NSString*, MLFeatureDescription*>* map,
                      const std::string& wanted, std::string* error, const char* what) {
  NSString* key = [NSString stringWithUTF8String:wanted.c_str()];
  if (map[key]) return key;
  if (map.count == 1) return map.allKeys.firstObject;
  if (error) {
    *error = std::string(what) + " \"" + wanted + "\" was not found. Model has: " +
             JoinNames(map);
  }
  return nil;
}

bool ShapeOf(MLFeatureDescription* fd, std::vector<int>* shape, std::string* error) {
  if (fd.type != MLFeatureTypeMultiArray || !fd.multiArrayConstraint) {
    if (error) *error = "Core ML feature is not a multiarray";
    return false;
  }
  shape->clear();
  for (NSNumber* n in fd.multiArrayConstraint.shape) shape->push_back(n.intValue);
  return true;
}

int Product(const std::vector<int>& shape) {
  int p = 1;
  for (int d : shape) {
    if (d < 0) return -1;
    p *= d;
  }
  return p;
}

MLMultiArray* MakeArray(const std::vector<int>& shape, MLMultiArrayDataType type, NSError** err) {
  NSMutableArray<NSNumber*>* dims = [NSMutableArray arrayWithCapacity:shape.size()];
  for (int d : shape) [dims addObject:@(d)];
  return [[MLMultiArray alloc] initWithShape:dims dataType:type error:err];
}

std::vector<int> StridesOf(MLMultiArray* arr);
std::vector<int> ShapeOfArray(MLMultiArray* arr);
int ElementBytes(MLMultiArrayDataType type);

void WriteFloatAt(MLMultiArray* arr, int offset, float value) {
  if (arr.dataType == MLMultiArrayDataTypeFloat16) {
    static_cast<uint16_t*>(arr.dataPointer)[offset] = FloatToHalf(value);
  } else if (arr.dataType == MLMultiArrayDataTypeFloat32) {
    static_cast<float*>(arr.dataPointer)[offset] = value;
  } else if (arr.dataType == MLMultiArrayDataTypeInt32) {
    static_cast<int32_t*>(arr.dataPointer)[offset] = static_cast<int32_t>(std::lrintf(value));
  } else if (arr.dataType == MLMultiArrayDataTypeDouble) {
    static_cast<double*>(arr.dataPointer)[offset] = value;
  }
}

void ZeroArray(MLMultiArray* arr) {
  if (!arr || !arr.dataPointer) return;
  const std::vector<int> shape = ShapeOfArray(arr);
  const std::vector<int> strides = StridesOf(arr);
  const int count = static_cast<int>(arr.count);
  int n = count;
  if (!shape.empty() && strides.size() == shape.size()) {
    n = StridedElementCount(shape.data(), strides.data(), static_cast<int>(shape.size()), count);
  }
  if (n > 0) {
    std::memset(arr.dataPointer, 0, static_cast<size_t>(n) * ElementBytes(arr.dataType));
  }
}

std::vector<float> ReadFloats(MLMultiArray* arr) {
  const std::vector<int> shape = ShapeOfArray(arr);
  const std::vector<int> strides = StridesOf(arr);
  const int count = static_cast<int>(arr.count);
  // ANE pads strides. Indexing with those strides walks past `count`, which
  // libc++ hardening reports as vector[] index out of bounds.
  int n = count;
  if (!shape.empty() && strides.size() == shape.size()) {
    n = StridedElementCount(shape.data(), strides.data(), static_cast<int>(shape.size()), count);
  }
  std::vector<float> out(static_cast<size_t>(std::max(n, 0)));
  if (arr.dataType == MLMultiArrayDataTypeFloat16) {
    const uint16_t* p = static_cast<const uint16_t*>(arr.dataPointer);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = HalfToFloat(p[i]);
  } else if (arr.dataType == MLMultiArrayDataTypeFloat32) {
    std::memcpy(out.data(), arr.dataPointer, static_cast<size_t>(n) * sizeof(float));
  } else if (arr.dataType == MLMultiArrayDataTypeDouble) {
    const double* p = static_cast<const double*>(arr.dataPointer);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = static_cast<float>(p[i]);
  } else if (arr.dataType == MLMultiArrayDataTypeInt32) {
    const int32_t* p = static_cast<const int32_t*>(arr.dataPointer);
    for (int i = 0; i < n; ++i) out[static_cast<size_t>(i)] = static_cast<float>(p[i]);
  }
  return out;
}

std::vector<int> StridesOf(MLMultiArray* arr) {
  std::vector<int> s;
  for (NSNumber* n in arr.strides) s.push_back(n.intValue);
  return s;
}

std::vector<int> ShapeOfArray(MLMultiArray* arr) {
  std::vector<int> s;
  for (NSNumber* n in arr.shape) s.push_back(n.intValue);
  return s;
}

// Row-major fill when the last dimension is the sample/time axis and every
// other dimension is 1. Returns false if the package uses a different layout.
bool WriteTimeMajor(MLMultiArray* arr, const float* src, int n, std::string* error) {
  const std::vector<int> shape = ShapeOfArray(arr);
  if (shape.empty() || shape.back() != n) {
    if (error) {
      *error = "Core ML time axis must be the last dimension and match the chunk";
    }
    return false;
  }
  for (size_t i = 0; i + 1 < shape.size(); ++i) {
    if (shape[i] != 1) {
      if (error) *error = "Core ML input has more than one non-time axis";
      return false;
    }
  }
  // MakeArray can pad the last stride. A dense memcpy then leaves the samples
  // the model reads (via strides) as zeros.
  const std::vector<int> strides = StridesOf(arr);
  const int step = strides.empty() ? 1 : std::max(1, strides.back());
  for (int i = 0; i < n; ++i) WriteFloatAt(arr, i * step, src[i]);
  return true;
}

bool ReadCodes(MLMultiArray* arr, int codebooks, int codebook_size, int new_frames,
               int ctx_frames, const std::string& layout, int frame_index, int32_t* dst,
               std::string* error) {
  const std::vector<int> shape = ShapeOfArray(arr);
  const std::vector<float> raw = ReadFloats(arr);
  int codebook_axis = -1;
  int time_axis = -1;
  const int time_full = ctx_frames + new_frames;
  for (int i = 0; i < static_cast<int>(shape.size()); ++i) {
    if (shape[static_cast<size_t>(i)] == codebooks && codebook_axis < 0) codebook_axis = i;
    else if (shape[static_cast<size_t>(i)] == new_frames ||
             shape[static_cast<size_t>(i)] == time_full) {
      time_axis = i;
    }
  }
  if (codebook_axis < 0 || time_axis < 0) {
    if (new_frames == codebooks) {
      // Both axes are the same length. Honor the manifest layout.
      int seen = 0;
      for (int i = 0; i < static_cast<int>(shape.size()); ++i) {
        if (shape[static_cast<size_t>(i)] != codebooks) continue;
        if (seen == 0) {
          if (layout == "time_codebook") time_axis = i;
          else codebook_axis = i;
        } else if (seen == 1) {
          if (layout == "time_codebook") codebook_axis = i;
          else time_axis = i;
        }
        ++seen;
      }
    }
  }
  if (codebook_axis < 0 || time_axis < 0) {
    if (error) *error = "cannot find codebook and time axes on the encoder output";
    return false;
  }
  const int time_len = shape[static_cast<size_t>(time_axis)];
  const int time0 = time_len == new_frames ? 0 : time_len - new_frames;
  const std::vector<int> strides = StridesOf(arr);
  for (int f = 0; f < new_frames; ++f) {
    for (int q = 0; q < codebooks; ++q) {
      std::vector<int> idx(shape.size(), 0);
      idx[static_cast<size_t>(time_axis)] = time0 + f;
      idx[static_cast<size_t>(codebook_axis)] = q;
      int offset = 0;
      for (size_t d = 0; d < idx.size(); ++d) {
        offset += idx[d] * strides[d];
      }
      const float sample = raw[static_cast<size_t>(offset)];
      int32_t code = 0;
      bool clamped = false;
      if (!QuantizeCode(sample, codebook_size, &code, &clamped)) {
        if (error) {
          *error = "encoder code outside the codebook frame=" + std::to_string(frame_index) +
                   " codebook=" +
                   std::to_string(q) + " raw=" + std::to_string(sample) +
                   " rounded=" + std::to_string(static_cast<int>(std::lrintf(sample))) +
                   " limit=" + std::to_string(codebook_size);
        }
        return false;
      }
      if (clamped) {
        NoteClampedCode();
        fprintf(stderr,
                "LiquidCodec encoder: clamped int8/fp16 code frame=%d codebook=%d raw=%.4f -> %d\n",
                frame_index, q, sample, code);
      }
      dst[f * codebooks + q] = code;
    }
  }
  return true;
}

int ElementBytes(MLMultiArrayDataType type) {
  switch (type) {
    case MLMultiArrayDataTypeFloat16:
      return 2;
    case MLMultiArrayDataTypeFloat32:
    case MLMultiArrayDataTypeInt32:
      return 4;
    case MLMultiArrayDataTypeDouble:
      return 8;
    default:
      return 4;
  }
}

struct KeptState {
  std::string in_name;
  std::string out_name;
  void* array = nullptr;
};

class CoreMlBackend final : public LiquidBackend {
 public:
  ~CoreMlBackend() override {
    ReleaseId(encoder_);
    ReleaseId(decoder_);
    for (KeptState& state : states_) ReleaseId(state.array);
  }

  const char* name() const override { return "coreml"; }
  int out_of_codebook() const override { return bad_codes_; }

  bool Start(const ModelPaths& paths, const ChunkConfig& chunk, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    paths_ = paths;
    chunk_ = chunk;
    const auto t0 = std::chrono::steady_clock::now();
    NSError* err = nil;
    MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
    cfg.computeUnits = Units(paths.compute);
    const std::string enc_src = JoinPath(paths.model_dir, paths.encoder_model);
    const std::string dec_src = JoinPath(paths.model_dir, paths.decoder_model);
    std::string enc_path;
    std::string dec_path;
    if (!PrepareModelPath(enc_src, &enc_path, error)) return false;
    if (!PrepareModelPath(dec_src, &dec_path, error)) return false;
    MLModel* enc = [MLModel modelWithContentsOfURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:enc_path.c_str()]]
                                    configuration:cfg
                                            error:&err];
    if (!enc) {
      if (error) *error = "encoder model failed to load: " + NsError(err);
      return false;
    }
    NSString* enc_in = ResolveName(enc.modelDescription.inputDescriptionsByName, paths.encoder_input,
                                   error, "encoder input");
    NSString* enc_out = ResolveName(enc.modelDescription.outputDescriptionsByName, paths.encoder_output,
                                    error, "encoder output");
    if (!enc_in || !enc_out) return false;
    enc_in_ = enc_in.UTF8String;
    enc_out_ = enc_out.UTF8String;
    std::vector<int> in_shape;
    if (!ShapeOf(enc.modelDescription.inputDescriptionsByName[enc_in], &in_shape, error)) return false;
    const bool stateful = paths.Stateful();
    const int samples =
        stateful ? paths.encoder_input_samples
                 : (paths.encoder_window == "new" ? chunk.new_frames : chunk.WindowFrames()) *
                       chunk.frame_samples;
    if (!in_shape.empty() && in_shape.back() > 0 && in_shape.back() != samples) {
      if (error) {
        *error = "encoder expects " + std::to_string(in_shape.back()) +
                 " samples in the last dimension but this manifest requires " +
                 std::to_string(samples) +
                 (stateful ? " (one frame plus lookahead_samples). " :
                             ". Pass --liquid-ctx and --liquid-new to match the fetched model. ") +
                 "This branch does not re-convert it.";
      }
      return false;
    }
    if (stateful) {
      for (KeptState& state : states_) ReleaseId(state.array);
      states_.clear();
      if (paths.encoder_states_in.size() != paths.encoder_states_out.size()) {
        if (error) *error = "encoder state input and output lists differ in length";
        return false;
      }
      for (size_t i = 0; i < paths.encoder_states_in.size(); ++i) {
        NSString* key = [NSString stringWithUTF8String:paths.encoder_states_in[i].c_str()];
        MLFeatureDescription* fd = enc.modelDescription.inputDescriptionsByName[key];
        if (!fd) {
          if (error) {
            *error = "encoder state \"" + paths.encoder_states_in[i] +
                     "\" was not found. Model inputs: " +
                     JoinNames(enc.modelDescription.inputDescriptionsByName);
          }
          return false;
        }
        std::vector<int> shape;
        if (!ShapeOf(fd, &shape, error)) return false;
        NSError* state_err = nil;
        MLMultiArray* arr = MakeArray(shape, fd.multiArrayConstraint.dataType, &state_err);
        if (!arr) {
          if (error) *error = NsError(state_err);
          return false;
        }
        std::memset(arr.dataPointer, 0,
                    static_cast<size_t>(arr.count) * ElementBytes(arr.dataType));
        KeptState kept;
        kept.in_name = paths.encoder_states_in[i];
        kept.out_name = paths.encoder_states_out[i];
        kept.array = RetainId(arr);
        states_.push_back(kept);
      }
    }
    MLModel* dec = [MLModel modelWithContentsOfURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:dec_path.c_str()]]
                                    configuration:cfg
                                            error:&err];
    if (!dec) {
      if (error) *error = "decoder model failed to load: " + NsError(err);
      return false;
    }
    if (paths.decoder_kind == "hidden") {
      NSString* lat = ResolveName(dec.modelDescription.inputDescriptionsByName, paths.decoder_lat_input,
                                  error, "decoder lat input");
      NSString* valid = ResolveName(dec.modelDescription.inputDescriptionsByName, paths.decoder_valid_input,
                                    error, "decoder valid input");
      NSString* hid = ResolveName(dec.modelDescription.outputDescriptionsByName, paths.decoder_output,
                                  error, "decoder output");
      if (!lat || !valid || !hid) return false;
      dec_lat_ = lat.UTF8String;
      dec_valid_ = valid.UTF8String;
      dec_out_ = hid.UTF8String;
      std::vector<int> lat_shape;
      if (!ShapeOf(dec.modelDescription.inputDescriptionsByName[lat], &lat_shape, error)) return false;
      if (!lat_shape.empty() && lat_shape.back() > 0 && lat_shape.back() != chunk.WindowFrames()) {
        if (error) {
          *error = "decoder lat last dimension is " + std::to_string(lat_shape.back()) +
                   " frames but ctx+new is " + std::to_string(chunk.WindowFrames()) +
                   ". Match --liquid-ctx/--liquid-new to the model. This branch does not re-convert it.";
        }
        return false;
      }
      if (!tail_.Load(JoinPath(paths.model_dir, paths.tail), error)) return false;
    } else {
      NSString* in_name = ResolveName(dec.modelDescription.inputDescriptionsByName,
                                     paths.decoder_lat_input, error, "decoder pcm input");
      NSString* hid = ResolveName(dec.modelDescription.outputDescriptionsByName, paths.decoder_output,
                                  error, "decoder pcm output");
      if (!in_name || !hid) return false;
      dec_lat_ = in_name.UTF8String;
      dec_out_ = hid.UTF8String;
    }
    encoder_ = RetainId(enc);
    decoder_ = RetainId(dec);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    fprintf(stderr,
            "LiquidCodec Core ML loaded in %.0f ms (compute=%s, ctx=%d, new=%d, encoder=%s, decoder=%s)\n",
            ms, paths.compute.c_str(), chunk.ctx_frames, chunk.new_frames, enc_path.c_str(),
            dec_path.c_str());
    RTC_LOG(LS_INFO) << "LiquidCodec models loaded in " << ms << " ms";
    return true;
  }

  bool EncodeWindow(const int16_t* pcm, const ChunkConfig& chunk, int32_t* codes_out,
                    WindowTiming* timing, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    const auto t0 = std::chrono::steady_clock::now();
    const bool stateful = paths_.Stateful();
    const int samples =
        stateful ? paths_.encoder_input_samples
                 : (paths_.encoder_window == "new" ? chunk.new_frames : chunk.WindowFrames()) *
                       chunk.frame_samples;
    const int offset = (!stateful && paths_.encoder_window == "new")
                           ? chunk.ctx_frames * chunk.frame_samples
                           : 0;
    const int frames_out = stateful ? 1 : chunk.new_frames;
    std::vector<float> audio(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
      audio[static_cast<size_t>(i)] =
          static_cast<float>(pcm[offset + i]) / 32768.f;
    }
    NSError* err = nil;
    MLModel* model = (MLModel*)ToId(encoder_);
    MLFeatureDescription* fd = model.modelDescription.inputDescriptionsByName[
        [NSString stringWithUTF8String:enc_in_.c_str()]];
    std::vector<int> shape;
    if (!ShapeOf(fd, &shape, error)) return false;
    if (!shape.empty() && shape.back() < 0) shape.back() = samples;
    MLMultiArray* arr = MakeArray(shape, fd.multiArrayConstraint.dataType, &err);
    if (!arr || !WriteTimeMajor(arr, audio.data(), samples, error)) {
      if (error && error->empty()) *error = NsError(err);
      return false;
    }
    MLFeatureValue* audio_value = [MLFeatureValue featureValueWithMultiArray:arr];
    NSMutableDictionary* features = [NSMutableDictionary dictionary];
    features[[NSString stringWithUTF8String:enc_in_.c_str()]] = audio_value;
    for (const KeptState& state : states_) {
      features[[NSString stringWithUTF8String:state.in_name.c_str()]] =
          [MLFeatureValue featureValueWithMultiArray:(MLMultiArray*)ToId(state.array)];
    }
    MLDictionaryFeatureProvider* provider =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:features error:&err];
    const auto p0 = std::chrono::steady_clock::now();
    id<MLFeatureProvider> out = [model predictionFromFeatures:provider error:&err];
    const auto p1 = std::chrono::steady_clock::now();
    if (!out) {
      if (error) *error = "encoder prediction failed: " + NsError(err);
      return false;
    }
    MLMultiArray* codes = [out featureValueForName:[NSString stringWithUTF8String:enc_out_.c_str()]]
                              .multiArrayValue;
    if (!codes || !ReadCodes(codes, chunk.codebooks, chunk.codebook_size, frames_out,
                             stateful ? 0 : chunk.ctx_frames, paths_.encoder_codes_layout,
                             frames_encoded_, codes_out, error)) {
      ++bad_codes_;
      NoteOutOfCodebook();
      fprintf(stderr, "LiquidCodec %s (count=%d). State reset to zero.\n",
              error && !error->empty() ? error->c_str() : "encoder code unreadable", bad_codes_);
      for (const KeptState& cleared : states_) {
        ZeroArray((MLMultiArray*)ToId(cleared.array));
      }
      ++frames_encoded_;
      return false;
    }
    ++frames_encoded_;
    for (const KeptState& state : states_) {
      MLMultiArray* updated =
          [out featureValueForName:[NSString stringWithUTF8String:state.out_name.c_str()]]
              .multiArrayValue;
      MLMultiArray* dest = (MLMultiArray*)ToId(state.array);
      if (!updated || !dest || updated.dataType != dest.dataType) {
        if (error) *error = "encoder state \"" + state.out_name + "\" did not match the stored tensor";
        return false;
      }
      const std::vector<int> state_shape = ShapeOfArray(updated);
      const std::vector<int> state_strides = StridesOf(updated);
      // The stored state is dense. An ANE state output is often padded, so a
      // memcpy of `count` elements keeps the wrong bytes and later frames
      // emit codes outside the codebook. The encode call then fails on every
      // tick and the callee sends nothing more.
      // Names are not `shape`/`strides`: this function already has `shape`
      // for the audio input, and -Wshadow is an error on Apple clang.
      const std::vector<int> dest_strides = StridesOf(dest);
      if (state_shape.empty() || state_strides.size() != state_shape.size() ||
          dest_strides.size() != state_shape.size() ||
          !CopyStrided(updated.dataPointer, state_shape.data(), state_strides.data(),
                       dest.dataPointer, dest_strides.data(),
                       static_cast<int>(state_shape.size()), ElementBytes(updated.dataType))) {
        if (error) *error = "encoder state \"" + state.out_name + "\" could not be copied";
        return false;
      }
    }
    for (int i = 0; i < frames_out * chunk.codebooks; ++i) {
      if (codes_out[i] < 0 || codes_out[i] >= chunk.codebook_size) {
        ++bad_codes_;
        NoteOutOfCodebook();
        if (bad_codes_ == 1 || bad_codes_ % 50 == 0) {
          fprintf(stderr,
                  "LiquidCodec encoder: code outside the codebook (count=%d). "
                  "State reset to zero; the call stays up.\n",
                  bad_codes_);
        }
        for (const KeptState& cleared : states_) {
          ZeroArray((MLMultiArray*)ToId(cleared.array));
        }
        if (error) *error = "encoder produced a code outside the codebook";
        return false;
      }
    }
    if (timing) {
      timing->window_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      timing->prediction_ms = std::chrono::duration<double, std::milli>(p1 - p0).count();
    }
    return true;
  }

  bool DecodeWindow(const int32_t* codes, const ChunkConfig& chunk, int16_t* pcm_out,
                    WindowTiming* timing, std::string* error) override {
    std::lock_guard<std::mutex> lock(mu_);
    const auto t0 = std::chrono::steady_clock::now();
    const int fr = chunk.WindowFrames();
    NSError* err = nil;
    MLModel* model = (MLModel*)ToId(decoder_);
    NSMutableDictionary* inputs = [NSMutableDictionary dictionary];
    if (paths_.decoder_kind == "hidden") {
      std::vector<float> lat(static_cast<size_t>(kLatent * fr));
      CodesToLatent(codes, fr, chunk.codebooks, lat.data());
      std::vector<float> valid(static_cast<size_t>(fr * kUpsample), 0.f);
      for (int i = 0; i < fr; ++i) {
        bool real = i >= chunk.ctx_frames;
        if (!real) {
          real = true;
          for (int q = 0; q < chunk.codebooks; ++q) {
            if (codes[i * chunk.codebooks + q] < 0) real = false;
          }
        }
        const float v = real ? 1.f : 0.f;
        for (int k = 0; k < kUpsample; ++k) valid[static_cast<size_t>(i * kUpsample + k)] = v;
      }
      if (!AddInput(model, dec_lat_, lat, inputs, error)) return false;
      if (!AddInput(model, dec_valid_, valid, inputs, error)) return false;
    } else {
      std::vector<float> as_float(static_cast<size_t>(fr * chunk.codebooks));
      for (size_t i = 0; i < as_float.size(); ++i) as_float[i] = static_cast<float>(codes[i]);
      const std::string input = dec_lat_.empty() ? paths_.decoder_lat_input : dec_lat_;
      if (!AddInput(model, input, as_float, inputs, error)) return false;
    }
    MLDictionaryFeatureProvider* provider =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:inputs error:&err];
    const auto p0 = std::chrono::steady_clock::now();
    id<MLFeatureProvider> out = [model predictionFromFeatures:provider error:&err];
    const auto p1 = std::chrono::steady_clock::now();
    if (!out) {
      if (error) *error = "decoder prediction failed: " + NsError(err);
      return false;
    }
    MLMultiArray* hidden =
        [out featureValueForName:[NSString stringWithUTF8String:dec_out_.c_str()]].multiArrayValue;
    if (!hidden) {
      if (error) *error = "decoder output missing";
      return false;
    }
    std::vector<float> pcm;
    if (paths_.decoder_kind == "pcm") {
      pcm = ReadFloats(hidden);
    } else {
      const int t75 = fr * kUpsample;
      const int keep = chunk.new_frames * kUpsample;
      const int c0 = chunk.ctx_frames * kUpsample;
      std::vector<float> raw = ReadFloats(hidden);
      const std::vector<int> shape = ShapeOfArray(hidden);
      const std::vector<int> strides = StridesOf(hidden);
      int hidden_axis = -1;
      int time_axis = -1;
      for (int i = 0; i < static_cast<int>(shape.size()); ++i) {
        if (shape[static_cast<size_t>(i)] == kHidden) hidden_axis = i;
        if (shape[static_cast<size_t>(i)] == t75) time_axis = i;
      }
      if (hidden_axis < 0 || time_axis < 0) {
        if (error) *error = "decoder hidden shape is not [512, ctx+new frames at 75 Hz]";
        return false;
      }
      std::vector<float> slice(static_cast<size_t>(kHidden * keep));
      for (int c = 0; c < kHidden; ++c) {
        for (int t = 0; t < keep; ++t) {
          std::vector<int> idx(shape.size(), 0);
          idx[static_cast<size_t>(hidden_axis)] = c;
          idx[static_cast<size_t>(time_axis)] = c0 + t;
          int offset = 0;
          for (size_t d = 0; d < idx.size(); ++d) offset += idx[d] * strides[d];
          slice[static_cast<size_t>(c * keep + t)] = raw[static_cast<size_t>(offset)];
        }
      }
      pcm = tail_.PushHidden(slice.data(), keep);
    }
    std::vector<int16_t> s16;
    s16.reserve(pcm.size());
    for (float x : pcm) {
      float y = x * 32767.f;
      if (y > 32767.f) y = 32767.f;
      if (y < -32768.f) y = -32768.f;
      s16.push_back(static_cast<int16_t>(std::lrintf(y)));
    }
    fifo_.insert(fifo_.end(), s16.begin(), s16.end());
    const int need = chunk.new_frames * chunk.frame_samples;
    if (static_cast<int>(fifo_.size()) < need) {
      fifo_.insert(fifo_.begin(), static_cast<size_t>(need - fifo_.size()), 0);
    }
    std::memcpy(pcm_out, fifo_.data(), static_cast<size_t>(need) * sizeof(int16_t));
    fifo_.erase(fifo_.begin(), fifo_.begin() + need);
    if (timing) {
      timing->window_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      timing->prediction_ms = std::chrono::duration<double, std::milli>(p1 - p0).count();
    }
    return true;
  }

  void Reset() override {
    std::lock_guard<std::mutex> lock(mu_);
    tail_.Reset();
    fifo_.clear();
    for (const KeptState& cleared : states_) {
      ZeroArray((MLMultiArray*)ToId(cleared.array));
    }
  }

 private:
  bool AddInput(MLModel* model, const std::string& name, const std::vector<float>& src,
                NSMutableDictionary* inputs, std::string* error) {
    NSError* err = nil;
    NSString* key = [NSString stringWithUTF8String:name.c_str()];
    MLFeatureDescription* fd = model.modelDescription.inputDescriptionsByName[key];
    if (!fd) {
      if (error) *error = "missing decoder input " + name;
      return false;
    }
    std::vector<int> shape;
    if (!ShapeOf(fd, &shape, error)) return false;
    if (Product(shape) > 0 && Product(shape) != static_cast<int>(src.size())) {
      if (error) *error = "decoder input " + name + " has " + std::to_string(Product(shape)) +
                          " elements, chunk produced " + std::to_string(src.size());
      return false;
    }
    MLMultiArray* arr = MakeArray(shape, fd.multiArrayConstraint.dataType, &err);
    if (!arr) {
      if (error) *error = NsError(err);
      return false;
    }
    const std::vector<int> in_shape = ShapeOfArray(arr);
    const std::vector<int> in_strides = StridesOf(arr);
    const int in_n = static_cast<int>(src.size());
    bool contiguous = in_strides.size() == in_shape.size() && !in_shape.empty();
    int packed = 1;
    for (int d = static_cast<int>(in_shape.size()) - 1; d >= 0 && contiguous; --d) {
      if (in_strides[static_cast<size_t>(d)] != packed) contiguous = false;
      packed *= in_shape[static_cast<size_t>(d)];
    }
    if (contiguous || in_strides.empty()) {
      for (int i = 0; i < in_n; ++i) WriteFloatAt(arr, i, src[static_cast<size_t>(i)]);
    } else {
      std::vector<int> index(in_shape.size(), 0);
      for (int written = 0; written < in_n; ++written) {
        int offset = 0;
        for (size_t d = 0; d < in_shape.size(); ++d) {
          offset += index[d] * in_strides[d];
        }
        WriteFloatAt(arr, offset, src[static_cast<size_t>(written)]);
        int dim = static_cast<int>(in_shape.size()) - 1;
        while (dim >= 0) {
          ++index[static_cast<size_t>(dim)];
          if (index[static_cast<size_t>(dim)] < in_shape[static_cast<size_t>(dim)]) break;
          index[static_cast<size_t>(dim)] = 0;
          --dim;
        }
      }
    }
    inputs[key] = [MLFeatureValue featureValueWithMultiArray:arr];
    return true;
  }

  std::mutex mu_;
  int bad_codes_ = 0;
  int frames_encoded_ = 0;
  ModelPaths paths_;
  ChunkConfig chunk_;
  void* encoder_ = nullptr;
  void* decoder_ = nullptr;
  std::string enc_in_, enc_out_, dec_lat_, dec_valid_, dec_out_;
  std::vector<KeptState> states_;
  LiquidTail tail_;
  std::vector<int16_t> fifo_;
};

}  // namespace

std::unique_ptr<LiquidBackend> CreateCoreMlBackend() {
  return std::make_unique<CoreMlBackend>();
}

}  // namespace liquid
}  // namespace webrtc
