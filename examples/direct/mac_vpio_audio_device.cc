/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

// AudioDeviceModuleImpl is behind this define; the library is built with the
// internal audio devices, the define just isn't exported to this target.
#ifndef WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE
#define WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE
#endif
#include "mac_vpio_audio_device.h"

#if defined(WEBRTC_MAC) && !defined(WEBRTC_IOS)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "api/make_ref_counted.h"
#include "modules/audio_device/audio_device_buffer.h"
#include "modules/audio_device/audio_device_generic.h"
#include "modules/audio_device/audio_device_impl.h"
#include "modules/audio_device/fine_audio_buffer.h"
#include "rtc_base/logging.h"

namespace {

constexpr UInt32 kOutputBus = 0;
constexpr UInt32 kInputBus = 1;
constexpr int kRate = 48000;
constexpr int k10ms = kRate / 100;
constexpr UInt32 kMaxFrames = 4096;

// Single producer (feeder thread), single consumer (VPIO render callback).
class SpscRing {
 public:
  explicit SpscRing(size_t capacity_pow2) : buf_(capacity_pow2), mask_(capacity_pow2 - 1) {}
  size_t Size() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
  size_t Free() const { return buf_.size() - Size(); }
  void Push(const int16_t* in, size_t n) {
    size_t w = w_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = in[i];
    w_.store(w + n, std::memory_order_release);
  }
  size_t Pop(int16_t* out, size_t n) {
    const size_t avail = Size();
    const size_t m = std::min(avail, n);
    size_t r = r_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < m; ++i) out[i] = buf_[(r + i) & mask_];
    r_.store(r + m, std::memory_order_release);
    return m;
  }
  void Clear() { r_.store(w_.load()); }

 private:
  std::vector<int16_t> buf_;
  size_t mask_;
  std::atomic<size_t> r_{0};
  std::atomic<size_t> w_{0};
};

class MacVpioAudioDevice : public webrtc::AudioDeviceGeneric {
 public:
  MacVpioAudioDevice() : ring_(1 << 14) {
    prebuffer_ms_ = 20;
    if (const char* v = std::getenv("CETACALL_VPIO_PREBUFFER_MS")) {
      const int ms = std::atoi(v);
      if (ms >= 0 && ms <= 200) prebuffer_ms_ = ms;
    }
    wake_ = dispatch_semaphore_create(0);
    record_scratch_.resize(kMaxFrames);
  }
  ~MacVpioAudioDevice() override { Terminate(); }

  int32_t ActiveAudioLayer(webrtc::AudioDeviceModule::AudioLayer& layer) const override {
    layer = webrtc::AudioDeviceModule::kPlatformDefaultAudio;
    return 0;
  }

  InitStatus Init() override {
    if (unit_) return InitStatus::OK;
    AudioComponentDescription d = {};
    d.componentType = kAudioUnitType_Output;
    d.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
    d.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &d);
    if (!comp || AudioComponentInstanceNew(comp, &unit_) != noErr) {
      unit_ = nullptr;
      std::fprintf(stderr, "VPIO: AudioComponentInstanceNew failed\n");
      return InitStatus::OTHER_ERROR;
    }
    UInt32 one = 1;
    if (!Set(kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, kInputBus, &one, sizeof(one), "enable input") ||
        !Set(kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, kOutputBus, &one, sizeof(one), "enable output")) {
      Dispose();
      return InitStatus::OTHER_ERROR;
    }
    AudioStreamBasicDescription f = {};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBytesPerPacket = 2;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 2;
    f.mChannelsPerFrame = 1;
    f.mBitsPerChannel = 16;
    if (!Set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kInputBus, &f, sizeof(f), "input format") ||
        !Set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputBus, &f, sizeof(f), "output format")) {
      Dispose();
      return InitStatus::OTHER_ERROR;
    }
    UInt32 max_frames = kMaxFrames;
    Set(kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames), nullptr);

    AURenderCallbackStruct render = {&MacVpioAudioDevice::OnRender, this};
    AURenderCallbackStruct input = {&MacVpioAudioDevice::OnInput, this};
    if (!Set(kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, kOutputBus, &render, sizeof(render), "render callback") ||
        !Set(kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, kInputBus, &input, sizeof(input), "input callback")) {
      Dispose();
      return InitStatus::OTHER_ERROR;
    }
#if defined(MAC_OS_VERSION_14_0)
    if (__builtin_available(macOS 14.0, *)) {
      // Don't turn other apps down while a call runs.
      AUVoiceIOOtherAudioDuckingConfiguration duck = {false, kAUVoiceIOOtherAudioDuckingLevelMin};
      Set(kAUVoiceIOProperty_OtherAudioDuckingConfiguration, kAudioUnitScope_Global, 0, &duck, sizeof(duck), nullptr);
    }
#endif
    OSStatus st = AudioUnitInitialize(unit_);
    if (st != noErr) {
      std::fprintf(stderr, "VPIO: AudioUnitInitialize failed (%d)\n", static_cast<int>(st));
      Dispose();
      return InitStatus::OTHER_ERROR;
    }
    Float64 hw_latency = 0;
    UInt32 size = sizeof(hw_latency);
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &hw_latency, &size) == noErr) {
      unit_latency_ms_ = static_cast<int>(hw_latency * 1000.0 + 0.5);
    }
    initialized_ = true;
    return InitStatus::OK;
  }

  int32_t Terminate() override {
    StopPlayout();
    StopRecording();
    Dispose();
    initialized_ = false;
    return 0;
  }
  bool Initialized() const override { return initialized_; }

  int16_t PlayoutDevices() override { return 1; }
  int16_t RecordingDevices() override { return 1; }
  int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                            char guid[webrtc::kAdmMaxGuidSize]) override {
    if (index != 0) return -1;
    std::snprintf(name, webrtc::kAdmMaxDeviceNameSize, "Default output (Voice Processing I/O)");
    if (guid) guid[0] = 0;
    return 0;
  }
  int32_t RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize],
                              char guid[webrtc::kAdmMaxGuidSize]) override {
    if (index != 0) return -1;
    std::snprintf(name, webrtc::kAdmMaxDeviceNameSize, "Default input (Voice Processing I/O)");
    if (guid) guid[0] = 0;
    return 0;
  }
  int32_t SetPlayoutDevice(uint16_t index) override { return index == 0 ? 0 : -1; }
  int32_t SetPlayoutDevice(webrtc::AudioDeviceModule::WindowsDeviceType) override { return 0; }
  int32_t SetRecordingDevice(uint16_t index) override { return index == 0 ? 0 : -1; }
  int32_t SetRecordingDevice(webrtc::AudioDeviceModule::WindowsDeviceType) override { return 0; }

  int32_t PlayoutIsAvailable(bool& available) override { available = true; return 0; }
  int32_t InitPlayout() override { play_initialized_ = initialized_; return initialized_ ? 0 : -1; }
  bool PlayoutIsInitialized() const override { return play_initialized_; }
  int32_t RecordingIsAvailable(bool& available) override { available = true; return 0; }
  int32_t InitRecording() override { rec_initialized_ = initialized_; return initialized_ ? 0 : -1; }
  bool RecordingIsInitialized() const override { return rec_initialized_; }

  int32_t StartPlayout() override {
    if (!play_initialized_) return -1;
    if (playing_) return 0;
    ring_.Clear();
    started_play_ = false;
    underruns_ = 0;
    audio_buffer_->StartPlayout();
    playing_ = true;
    feeder_stop_ = false;
    feeder_ = std::thread([this] { FeedLoop(); });
    return StartUnitIfNeeded();
  }
  int32_t StopPlayout() override {
    if (!playing_) return 0;
    playing_ = false;
    feeder_stop_ = true;
    dispatch_semaphore_signal(wake_);
    if (feeder_.joinable()) feeder_.join();
    if (audio_buffer_) audio_buffer_->StopPlayout();
    StopUnitIfIdle();
    if (underruns_.load() > 0) {
      std::fprintf(stderr, "VPIO: %d playout underruns this call\n", underruns_.load());
    }
    return 0;
  }
  bool Playing() const override { return playing_; }

  int32_t StartRecording() override {
    if (!rec_initialized_) return -1;
    if (recording_) return 0;
    fine_->ResetRecord();
    audio_buffer_->StartRecording();
    recording_ = true;
    return StartUnitIfNeeded();
  }
  int32_t StopRecording() override {
    if (!recording_) return 0;
    recording_ = false;
    if (audio_buffer_) audio_buffer_->StopRecording();
    StopUnitIfIdle();
    return 0;
  }
  bool Recording() const override { return recording_; }

  int32_t InitSpeaker() override { return 0; }
  bool SpeakerIsInitialized() const override { return true; }
  int32_t InitMicrophone() override { return 0; }
  bool MicrophoneIsInitialized() const override { return true; }
  int32_t SpeakerVolumeIsAvailable(bool& available) override { available = false; return 0; }
  int32_t SetSpeakerVolume(uint32_t) override { return -1; }
  int32_t SpeakerVolume(uint32_t&) const override { return -1; }
  int32_t MaxSpeakerVolume(uint32_t&) const override { return -1; }
  int32_t MinSpeakerVolume(uint32_t&) const override { return -1; }
  int32_t MicrophoneVolumeIsAvailable(bool& available) override { available = false; return 0; }
  int32_t SetMicrophoneVolume(uint32_t) override { return -1; }
  int32_t MicrophoneVolume(uint32_t&) const override { return -1; }
  int32_t MaxMicrophoneVolume(uint32_t&) const override { return -1; }
  int32_t MinMicrophoneVolume(uint32_t&) const override { return -1; }
  int32_t SpeakerMuteIsAvailable(bool& available) override { available = false; return 0; }
  int32_t SetSpeakerMute(bool) override { return -1; }
  int32_t SpeakerMute(bool&) const override { return -1; }
  int32_t MicrophoneMuteIsAvailable(bool& available) override { available = false; return 0; }
  int32_t SetMicrophoneMute(bool) override { return -1; }
  int32_t MicrophoneMute(bool&) const override { return -1; }
  int32_t StereoPlayoutIsAvailable(bool& available) override { available = false; return 0; }
  int32_t SetStereoPlayout(bool enable) override { return enable ? -1 : 0; }
  int32_t StereoPlayout(bool& enabled) const override { enabled = false; return 0; }
  int32_t StereoRecordingIsAvailable(bool& available) override { available = false; return 0; }
  int32_t SetStereoRecording(bool enable) override { return enable ? -1 : 0; }
  int32_t StereoRecording(bool& enabled) const override { enabled = false; return 0; }

  int32_t PlayoutDelay(uint16_t& delay_ms) const override {
    delay_ms = static_cast<uint16_t>(ring_.Size() * 1000 / kRate + unit_latency_ms_);
    return 0;
  }

  // VPIO is the canceller. WebRTC turns its software AEC3 off when this is
  // available and enabled, the same as iOS.
  bool BuiltInAECIsAvailable() const override { return true; }
  bool BuiltInAGCIsAvailable() const override { return true; }
  bool BuiltInNSIsAvailable() const override { return false; }
  int32_t EnableBuiltInAEC(bool enable) override {
    if (!unit_) return -1;
    UInt32 bypass = enable ? 0 : 1;
    return Set(kAUVoiceIOProperty_BypassVoiceProcessing, kAudioUnitScope_Global, kInputBus, &bypass, sizeof(bypass), "bypass") ? 0 : -1;
  }
  int32_t EnableBuiltInAGC(bool enable) override {
    if (!unit_) return -1;
    UInt32 v = enable ? 1 : 0;
    return Set(kAUVoiceIOProperty_VoiceProcessingEnableAGC, kAudioUnitScope_Global, kInputBus, &v, sizeof(v), "agc") ? 0 : -1;
  }
  int32_t EnableBuiltInNS(bool) override { return -1; }
  int32_t GetPlayoutUnderrunCount() const override { return underruns_.load(); }

  void AttachAudioBuffer(webrtc::AudioDeviceBuffer* buffer) override {
    audio_buffer_ = buffer;
    buffer->SetRecordingSampleRate(kRate);
    buffer->SetPlayoutSampleRate(kRate);
    buffer->SetRecordingChannels(1);
    buffer->SetPlayoutChannels(1);
    fine_ = std::make_unique<webrtc::FineAudioBuffer>(buffer);
  }

  int unit_latency_ms() const { return unit_latency_ms_; }
  int prebuffer_ms() const { return prebuffer_ms_; }

 private:
  bool Set(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el,
           const void* data, UInt32 size, const char* what) {
    OSStatus st = AudioUnitSetProperty(unit_, id, scope, el, data, size);
    if (st != noErr && what) {
      std::fprintf(stderr, "VPIO: set %s failed (%d)\n", what, static_cast<int>(st));
    }
    return st == noErr;
  }

  int32_t StartUnitIfNeeded() {
    if (unit_running_) return 0;
    OSStatus st = AudioOutputUnitStart(unit_);
    if (st != noErr) {
      std::fprintf(stderr, "VPIO: AudioOutputUnitStart failed (%d)\n", static_cast<int>(st));
      return -1;
    }
    unit_running_ = true;
    return 0;
  }
  void StopUnitIfIdle() {
    if (unit_running_ && !playing_ && !recording_) {
      AudioOutputUnitStop(unit_);
      unit_running_ = false;
    }
  }
  void Dispose() {
    if (unit_) {
      if (unit_running_) AudioOutputUnitStop(unit_);
      unit_running_ = false;
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
    }
  }

  // Decoding (NetEq + the CetaCall Core ML decoder) runs here, not on the
  // real-time I/O thread, so a slow decode drains the ring instead of
  // stalling the speaker.
  void FeedLoop() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::vector<int16_t> block(k10ms);
    while (!feeder_stop_.load()) {
      const size_t want = static_cast<size_t>(last_io_frames_.load()) +
                          static_cast<size_t>(prebuffer_ms_) * kRate / 1000;
      while (!feeder_stop_.load() && ring_.Size() < want && ring_.Free() >= k10ms) {
        if (audio_buffer_->RequestPlayoutData(k10ms) <= 0) break;
        audio_buffer_->GetPlayoutData(block.data());
        ring_.Push(block.data(), k10ms);
        started_play_.store(true);
      }
      dispatch_semaphore_wait(wake_, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_MSEC));
    }
  }

  static OSStatus OnRender(void* ctx, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                           UInt32, UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<MacVpioAudioDevice*>(ctx);
    auto* out = static_cast<int16_t*>(io->mBuffers[0].mData);
    const size_t n = std::min<size_t>(frames, io->mBuffers[0].mDataByteSize / 2);
    self->last_io_frames_.store(static_cast<int>(n));
    size_t got = 0;
    if (self->playing_.load()) {
      got = self->ring_.Pop(out, n);
      if (got < n && self->started_play_.load()) self->underruns_.fetch_add(1);
    }
    if (got < n) std::memset(out + got, 0, (n - got) * 2);
    if (got == 0 && flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    dispatch_semaphore_signal(self->wake_);
    return noErr;
  }

  static OSStatus OnInput(void* ctx, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts,
                          UInt32 bus, UInt32 frames, AudioBufferList*) {
    auto* self = static_cast<MacVpioAudioDevice*>(ctx);
    if (frames > kMaxFrames) return noErr;
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 1;
    abl.mBuffers[0].mDataByteSize = frames * 2;
    abl.mBuffers[0].mData = self->record_scratch_.data();
    OSStatus st = AudioUnitRender(self->unit_, flags, ts, bus, frames, &abl);
    if (st != noErr || !self->recording_.load()) return st;
    self->fine_->DeliverRecordedData(
        std::span<const int16_t>(self->record_scratch_.data(), frames),
        self->unit_latency_ms_);
    return noErr;
  }

  AudioUnit unit_ = nullptr;
  webrtc::AudioDeviceBuffer* audio_buffer_ = nullptr;
  std::unique_ptr<webrtc::FineAudioBuffer> fine_;
  SpscRing ring_;
  std::vector<int16_t> record_scratch_;
  dispatch_semaphore_t wake_;
  std::thread feeder_;
  std::atomic<bool> feeder_stop_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> recording_{false};
  std::atomic<bool> started_play_{false};
  std::atomic<int> last_io_frames_{512};
  std::atomic<int> underruns_{0};
  bool initialized_ = false;
  bool play_initialized_ = false;
  bool rec_initialized_ = false;
  bool unit_running_ = false;
  int unit_latency_ms_ = 0;
  int prebuffer_ms_ = 20;
};

}  // namespace

webrtc::scoped_refptr<webrtc::AudioDeviceModule>
CreateMacVoiceProcessingAudioDeviceModule(const webrtc::Environment& env) {
  auto device = std::make_unique<MacVpioAudioDevice>();
  MacVpioAudioDevice* raw = device.get();
  auto adm = webrtc::make_ref_counted<webrtc::AudioDeviceModuleImpl>(
      env, webrtc::AudioDeviceModule::kPlatformDefaultAudio, std::move(device),
      /*create_detached=*/false);
  if (adm->AttachAudioBuffer() != 0 || adm->Init() != 0) {
    return nullptr;
  }
  std::fprintf(stderr,
               "Audio device: macOS Voice Processing I/O (Apple echo canceller + AGC, "
               "WebRTC AEC3 off), 48000 Hz mono, unit latency %d ms, playout prebuffer %d ms\n",
               raw->unit_latency_ms(), raw->prebuffer_ms());
  return adm;
}

#endif  // WEBRTC_MAC && !WEBRTC_IOS
