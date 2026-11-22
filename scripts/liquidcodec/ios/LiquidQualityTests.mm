// Drop this file into the RobotPhone test target on branch liquidcodec-sync.
// Link direct.framework and WebRTC.framework from
// <webrtcsays gclient>/out/ios-arm64 and copy out/ios-arm64/models into the
// app bundle as a folder named "models". The test writes quality.csv into
// the app Documents directory.
//
// Interface, exported by direct.framework:
//   int DirectQualityRun(const char* stage, const char* wav_in,
//                        const char* wav_out, const char* stats_json,
//                        const char* codec, const char* model_dir,
//                        const char* compute, const char* encoder_precision,
//                        int opus_bitrate_bps, int loss_pct, int jitter_ms);
// stage is "offline" or "factory". compute "cpu" on the simulator (no ANE)
// and "ane" on a device. Return 0 is a finished run, 3 is a skipped fp32
// model, 2 means LiquidCodec was not in the framework.
//
// This file is not built by webrtcsays gn. RobotPhone owns the Xcode target.

#import <XCTest/XCTest.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

extern "C" int DirectQualityRun(const char* stage, const char* wav_in, const char* wav_out,
                                const char* stats_json, const char* codec, const char* model_dir,
                                const char* compute, const char* encoder_precision,
                                int opus_bitrate_bps, int loss_pct, int jitter_ms);

@interface LiquidQualityTests : XCTestCase
@end

@implementation LiquidQualityTests

- (NSString*)documents {
  return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

- (NSString*)models {
  NSString* bundled = [[NSBundle mainBundle] pathForResource:@"models" ofType:nil];
  if (bundled.length > 0) return bundled;
  return [[self documents] stringByAppendingPathComponent:@"models"];
}

- (NSString*)vowelWav {
  // A short generated tone so the test does not depend on a checked-in recording.
  NSString* path = [[self documents] stringByAppendingPathComponent:@"vowel.wav"];
  if ([[NSFileManager defaultManager] fileExistsAtPath:path]) return path;
  FILE* f = fopen(path.UTF8String, "wb");
  XCTAssert(f != nullptr);
  const int rate = 24000;
  const int n = rate;
  const uint32_t data_bytes = static_cast<uint32_t>(n * 2);
  const uint32_t riff = 36 + data_bytes;
  uint8_t h[44] = {};
  memcpy(h, "RIFF", 4);
  memcpy(h + 4, &riff, 4);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  uint32_t fmt = 16;
  memcpy(h + 16, &fmt, 4);
  uint16_t tag = 1, ch = 1, bits = 16, block = 2;
  uint32_t byte_rate = rate * 2;
  memcpy(h + 20, &tag, 2);
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &rate, 4);
  memcpy(h + 28, &byte_rate, 4);
  memcpy(h + 32, &block, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &data_bytes, 4);
  fwrite(h, 1, 44, f);
  for (int i = 0; i < n; ++i) {
    double t = static_cast<double>(i) / rate;
    int16_t s = static_cast<int16_t>(8000 * sin(2 * 3.1415926535 * 110 * t));
    fwrite(&s, 2, 1, f);
  }
  fclose(f);
  return path;
}

- (void)appendCsv:(NSString*)line {
  NSString* csv = [[self documents] stringByAppendingPathComponent:@"quality.csv"];
  NSFileManager* fm = [NSFileManager defaultManager];
  if (![fm fileExistsAtPath:csv]) {
    [@"stage,compute,encoder,status,stats\n" writeToFile:csv atomically:YES encoding:NSUTF8StringEncoding error:nil];
  }
  NSFileHandle* handle = [NSFileHandle fileHandleForWritingAtPath:csv];
  [handle seekToEndOfFile];
  [handle writeData:[[line stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]];
  [handle closeFile];
  NSLog(@"LiquidQuality %@", line);
}

- (void)runStage:(NSString*)stage compute:(NSString*)compute encoder:(NSString*)encoder {
  NSString* docs = [self documents];
  NSString* wavOut = [docs stringByAppendingPathComponent:
      [NSString stringWithFormat:@"%@-%@-%@.wav", stage, compute, encoder]];
  NSString* stats = [docs stringByAppendingPathComponent:
      [NSString stringWithFormat:@"%@-%@-%@.json", stage, compute, encoder]];
  int rc = DirectQualityRun(stage.UTF8String, [self vowelWav].UTF8String, wavOut.UTF8String,
                            stats.UTF8String, "liquidcodec", [self models].UTF8String,
                            compute.UTF8String, encoder.UTF8String, 0, 0, 0);
  [self appendCsv:[NSString stringWithFormat:@"%@,%@,%@,%d,%@", stage, compute, encoder, rc, stats]];
  // 3 means the fp32 package is not in the model drop. That is a skip, not a failure.
  XCTAssert(rc == 0 || rc == 3, @"DirectQualityRun %d", rc);
}

- (void)testOfflineCpuInt8 {
  [self runStage:@"offline" compute:@"cpu" encoder:@"int8"];
}

- (void)testFactoryCpuInt8 {
  [self runStage:@"factory" compute:@"cpu" encoder:@"int8"];
}

- (void)testOfflineFp16Cpu {
  [self runStage:@"offline" compute:@"cpu" encoder:@"fp16"];
}

// Device only. The simulator has no Apple Neural Engine; this test is
// expected to fail or skip there. Do not select it for a simulator run.
- (void)testOfflineAneInt8 {
  [self runStage:@"offline" compute:@"ane" encoder:@"int8"];
}

@end
