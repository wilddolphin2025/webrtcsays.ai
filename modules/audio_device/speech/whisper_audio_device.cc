/*
 *  (c) 2025, wilddolphin2025 
 *  For WebRTCsays.ai project
 *  https://github.com/wilddolphin2025
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <string>
#include <cstdio>
#include <thread>
#include <iomanip>
#include <memory>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <queue>

#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "rtc_base/thread.h"
#include "rtc_base/string_utils.h"
#include "api/task_queue/default_task_queue_factory.h"

#include "modules/audio_device/speech/speech_audio_device_factory.h"
#include "modules/audio_device/speech/whisper_audio_device.h"

#include "absl/synchronization/mutex.h"

//#define PLAY_WAV_ON_RECORD 1
//#define PLAY_WAV_ON_PLAY 1
#define LLAMA_ENABLED 1

namespace webrtc {

const int kRecordingFixedSampleRate = 16000;  // Whisper typically uses 16kHz
const size_t kRecordingNumChannels = 1;       // Mono for Whisper
const int kPlayoutFixedSampleRate = 16000;
const size_t kPlayoutNumChannels = 1;
const size_t kPlayoutBufferSize =
    kPlayoutFixedSampleRate / 100 * kPlayoutNumChannels * 2;
const size_t kRecordingBufferSize =
    kRecordingFixedSampleRate / 100 * kRecordingNumChannels * 2;

void ttsAudioCallback(bool success, const uint16_t* buffer, size_t buffer_size, void* user_data) {
  WhisperAudioDevice* audio_device = static_cast<WhisperAudioDevice*>(user_data);
  if (!audio_device) return;
  if(success) {
    RTC_LOG(LS_VERBOSE) << "Generated " << buffer_size << " audio samples (" 
      << buffer_size / 16000 << " s)";
    audio_device->SetTTSBuffer(buffer, buffer_size);
  }
}

void whisperResponseCallback(bool success, const char* response, void* user_data) {
  WhisperAudioDevice* audio_device = static_cast<WhisperAudioDevice*>(user_data);
  if (!audio_device) return;
  // Handle response here
  RTC_LOG(LS_INFO) << "Whisper response via callback: " << response;
  if(success) {
    if(audio_device->_llama_enabled)
      audio_device->askLlama(std::string(response));
    else  
      audio_device->speakText(std::string(response));
  }
}

void languageResponseCallback(bool success, const char* language, void* user_data) {
  WhisperAudioDevice* audio_device = static_cast<WhisperAudioDevice*>(user_data);
  if (!audio_device) return;
  // Only note the detection: overwriting the transcriber's "auto" setting
  // would freeze auto-detect on the first utterance's language.
  if (success && language && *language)
    SpeechAudioDeviceFactory::NoteDetectedLanguage(language);
}

void llamaResponseCallback(bool success, const char* response, void* user_data) {
  WhisperAudioDevice* audio_device = static_cast<WhisperAudioDevice*>(user_data);
  if (!audio_device || !success || !response) return;

  size_t len = strnlen(response, WhisperAudioDevice::kLlamaPendingBytes - 1);
  if (len == 0) return;

  // Pure C operations only - no C++ objects, no RTC_LOG, no std::string.
  // This callback runs on the libstdc++ Llama thread; any libc++ usage crashes.
  uint32_t head = audio_device->_llamaPendingHead.load(std::memory_order_relaxed);
  uint32_t tail = audio_device->_llamaPendingTail.load(std::memory_order_acquire);
  if (head - tail >= WhisperAudioDevice::kLlamaPendingSlots) return;  // full: drop
  char* slot = audio_device->_llamaPending[head % WhisperAudioDevice::kLlamaPendingSlots];
  memcpy(slot, response, len);
  slot[len] = '\0';
  audio_device->_llamaPendingHead.store(head + 1, std::memory_order_release);
}

void transcriptCallback(const char* text, bool is_final, const char* language, void* user_data) {
  // C-only for the same reason as llamaResponseCallback. Finals still reach the
  // LLM through whisperResponseCallback; here we only note that words were heard.
  WhisperAudioDevice* audio_device = static_cast<WhisperAudioDevice*>(user_data);
  if (audio_device && text && text[0])
    audio_device->_heardSpeech.fetch_add(1, std::memory_order_relaxed);
}

WhisperAudioDevice::WhisperAudioDevice(
    TaskQueueFactory* task_queue_factory)
    : _task_queue_factory(task_queue_factory),
      _ttsCallback(ttsAudioCallback, this),
      _whisperCallback(whisperResponseCallback, this),
      _languageCallback(languageResponseCallback, this),
      _llamaResponseCallback(llamaResponseCallback, this),
      _transcriptCallback(transcriptCallback, this)
{
}

WhisperAudioDevice::~WhisperAudioDevice() {
  // Stop any ongoing playout/recording to ensure no more callbacks after destruction.
  if (_playing) {
    StopPlayout();
  }
  if (_recording) {
    StopRecording();
  }

  // Guard shared resources against concurrent access.
  {
    absl::MutexLock lock(&_queueMutex);
    _ttsBuffer.clear();
    std::queue<std::string> empty;
    std::swap(_textQueue, empty);
  }

  // Free dynamically allocated buffers.
  delete[] _recordingBuffer;
  delete[] _playoutBuffer;
}

int32_t WhisperAudioDevice::ActiveAudioLayer(
    AudioDeviceModule::AudioLayer& audioLayer) const {
  if(audioLayer == AudioDeviceModule::kSpeechAudio)
    return 0;

  return -1;  
}

AudioDeviceGeneric::InitStatus WhisperAudioDevice::Init() {

  return InitStatus::OK;
}

int32_t WhisperAudioDevice::Terminate() {
  return 0;
}

bool WhisperAudioDevice::Initialized() const {
  return true;
}

// trim from start (in place)
inline void ltrim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
      return !std::isspace(ch);
  }));
}

// trim from end (in place)
inline void rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
      return !std::isspace(ch);
  }).base(), s.end());
}

void WhisperAudioDevice::speakText(const std::string& text) {
  if(_tts_enabled) {
    {
      absl::MutexLock lock(&_queueMutex);
      std::string s(text);
      rtrim(s);
      ltrim(s);
      _textQueue.push(s);
    }
    _queueCondition.notify_one();  // Inform one waiting thread that an item is available
  }
}

// Method to ask llama 
void WhisperAudioDevice::askLlama(const std::string& text) {
  if(_llama_enabled) {
    // The language callback fires before each final, so this is the utterance's language.
    std::string lang = SpeechAudioDeviceFactory::GetLanguage();
    RTC_LOG(LS_INFO) << "Asking llama [" << lang << "]: " << text;
    SpeechAudioDeviceFactory::llama()->askLlama(text.c_str(), lang.c_str());
  }  
}

//
// Recording
//

int16_t WhisperAudioDevice::RecordingDevices() {
  return 1;
}

int32_t WhisperAudioDevice::RecordingDeviceName(uint16_t index,
                                             char name[kAdmMaxDeviceNameSize],
                                             char guid[kAdmMaxGuidSize]) {
  const char* kName = "whisper_recording_device";
  const char* kGuid = "358f8c4d-9605-4d23-bf0a-17d346fafc6f";
  if (index < 1) {
    webrtc::strcpyn(name, kAdmMaxDeviceNameSize, kName);
    webrtc::strcpyn(guid, kAdmMaxGuidSize, kGuid);
    return 0;
  }
  return -1;
}

int32_t WhisperAudioDevice::SetRecordingDevice(uint16_t index) {
  if (index == 0) {
    return 0;
  }
  return -1;
}

int32_t WhisperAudioDevice::SetRecordingDevice(
    AudioDeviceModule::WindowsDeviceType device) {
  return 0;
}

int32_t WhisperAudioDevice::RecordingIsAvailable(bool& available) {
  available = true;
  return 0;
}

int32_t WhisperAudioDevice::InitRecording() {
  MutexLock lock(&mutex_);

  if (_recording) {
    return -1;
  }

  _recordingFramesIn10MS = static_cast<size_t>(kRecordingFixedSampleRate / 100);

  if (_ptrAudioBuffer) {
    _ptrAudioBuffer->SetRecordingSampleRate(kRecordingFixedSampleRate);
    _ptrAudioBuffer->SetRecordingChannels(kRecordingNumChannels);
  }

  return 0;
}

bool WhisperAudioDevice::RecordingIsInitialized() const {
  return _recordingFramesIn10MS != 0;
}

int32_t WhisperAudioDevice::InitMicrophone() {
  return 0;
}

bool WhisperAudioDevice::MicrophoneIsInitialized() const {
  return true;
}

int32_t WhisperAudioDevice::StartRecording() {
  {
    MutexLock lock(&mutex_);
    // The send path is what the browser hears. Start TTS here so the greeting
    // and later answers are spoken even if remote audio has not arrived yet.
    StartSpeechEngines();
  }

  _recording = true;

  // Allocate recording buffer
  if (!_recordingBuffer) {
    _recordingBuffer = new int8_t[kRecordingBufferSize];
  }

  // "RECORDING"
  #if defined(PLAY_WAV_ON_RECORD)
  if (!_wavFilename.empty()) {
    _recFile = FileWrapper::OpenReadOnly(_wavFilename);
    if (!_recFile.is_open()) {
      RTC_LOG(LS_ERROR) << "Failed to open 'recording' file: " << _wavFilename;
      _recording = false;
      delete[] _recordingBuffer;
      _recordingBuffer = NULL;
      return -1;
    }
  }
  #endif // defined(PLAY_WAV_ON_RECORD)

  speakText("Hello, I am ready.");

  _ptrThreadRec = webrtc::PlatformThread::SpawnJoinable(
      [this] {
        while (RecThreadProcess()) {
        }
      },
      "whisper_audio_module_capture_thread",
      webrtc::ThreadAttributes().SetPriority(webrtc::ThreadPriority::kRealtime));

  RTC_LOG(LS_INFO) << "Started Whisper recording";

  return 0;
}

bool WhisperAudioDevice::Recording() const {
  return _recording;
}

int32_t WhisperAudioDevice::StopRecording() {
  {
    MutexLock lock(&mutex_);
    _recording = false;
  }

  if (!_ptrThreadRec.empty())
    _ptrThreadRec.Finalize();

  MutexLock lock(&mutex_);
  _recordingFramesLeft = 0;
  if (_recordingBuffer) {
    delete[] _recordingBuffer;
    _recordingBuffer = NULL;
  }

  _recFile.Close();

  RTC_LOG(LS_INFO) << "Stopped 'recording'!";
  return 0;
}

void WhisperAudioDevice::SetTTSBuffer(const uint16_t* buffer, size_t buffer_size) {
    RTC_LOG(LS_INFO) << "[PLAY] SetTTSBuffer called with " << buffer_size << " samples";

    constexpr size_t kMaxBufferSamples = 1 << 20;

    absl::MutexLock lock(&_queueMutex);

    size_t current_size = _ttsBuffer.size();
    size_t new_size = current_size + buffer_size;

    if (new_size > kMaxBufferSamples) {
        RTC_LOG(LS_WARNING) << "[PLAY] TTS buffer would exceed max size (" << new_size << " > " << kMaxBufferSamples << "), truncating";
        buffer_size = kMaxBufferSamples - current_size;
        if (buffer_size == 0) {
            RTC_LOG(LS_ERROR) << "[PLAY] TTS buffer full, discarding new data";
            return;
        }
        new_size = kMaxBufferSamples;
    }

    _ttsBuffer.reserve(new_size);
    _ttsBuffer.insert(_ttsBuffer.end(), buffer, buffer + buffer_size);
    if (!_isSpeaking.exchange(true, std::memory_order_relaxed)) {
      _ttsStartedMillis.store(webrtc::TimeMillis(), std::memory_order_relaxed);
    }

    RTC_LOG(LS_INFO) << "[PLAY] TTS buffer updated, new size: " << _ttsBuffer.size();
}

void WhisperAudioDevice::interruptLlama() {
  // 1. Stop LLaMa generation
  if (_llama_device) {
    _llama_device->stop();
  }

  // 2. Drop pending LLaMa sentences (the recording thread owns the ring tail)
  _llamaDropPending.store(true, std::memory_order_release);

  // 3. Stop the synthesizer first so audio already in flight does not refill
  //    the buffer cleared below.
  if (_tts) {
    _tts->interrupt();
  }

  // 4. Clear TTS text queue
  {
    absl::MutexLock lock(&_queueMutex);
    std::queue<std::string> empty;
    std::swap(_textQueue, empty);

    // 5. Clear TTS audio buffer
    _ttsBuffer.clear();
    _ttsIndex = 0;
    _isSpeaking.store(false, std::memory_order_relaxed);
  }

  RTC_LOG(LS_INFO) << "Interrupted: LLaMa stopped, TTS queue and buffer cleared";

  // 6. Restart LLaMa so it can accept new requests
  if (_llama_device) {
    _llama_device->start();
  }
}

void WhisperAudioDevice::drainLlamaPending() {
  uint32_t head = _llamaPendingHead.load(std::memory_order_acquire);
  uint32_t tail = _llamaPendingTail.load(std::memory_order_relaxed);
  if (_llamaDropPending.exchange(false, std::memory_order_acq_rel)) {
    _llamaPendingTail.store(head, std::memory_order_release);
    return;
  }
  for (; tail != head; ++tail) {
    std::string resp(_llamaPending[tail % kLlamaPendingSlots]);
    _llamaPendingTail.store(tail + 1, std::memory_order_release);
    RTC_LOG(LS_INFO) << "Llama response: " << resp;
    speakText(resp);
  }
}

bool WhisperAudioDevice::RecThreadProcess() {
  if (!_recording) {
    return false;
  }
  int64_t currentTime = webrtc::TimeMillis();
  mutex_.Lock();

  // Check if it's time to process another 10ms chunk
  if (_lastCallRecordMillis == 0 || currentTime - _lastCallRecordMillis >= 10) {
    // Handle audio buffer playback
    {
      // Guard concurrent access to _ttsBuffer and _ttsIndex.
      absl::MutexLock qlock(& _queueMutex);

      if (!_ttsBuffer.empty()) {
        if (_ttsIndex >= _ttsBuffer.size()) {
          RTC_LOG(LS_INFO) << "Finished playing TTS buffer, resetting";
          _ttsIndex = 0;
          _ttsBuffer.clear();
          _isSpeaking.store(false, std::memory_order_relaxed);
          // Close mouth when done speaking
          auto* face = SpeechAudioDeviceFactory::talkingFace();
          if (face) face->feedAudio(nullptr, 0);
        } else {
          size_t remainingSamples = _ttsBuffer.size() - _ttsIndex;
          size_t samplesToCopy = std::min(_recordingFramesIn10MS, remainingSamples);

          if (samplesToCopy > 0 && _recordingBuffer != nullptr) {
            const int8_t* src = reinterpret_cast<const int8_t*>(&_ttsBuffer[_ttsIndex]);
            const int8_t* end = src + samplesToCopy * sizeof(short);
            std::copy(src, end, _recordingBuffer);

            // Feed this 10ms chunk to talking face for lip-sync
            auto* face = SpeechAudioDeviceFactory::talkingFace();
            if (face) {
              face->feedAudio(reinterpret_cast<const int16_t*>(&_ttsBuffer[_ttsIndex]),
                              samplesToCopy);
            }

            _ttsIndex += samplesToCopy;

            std::fill_n(
              _recordingBuffer + samplesToCopy * sizeof(short),
              (_recordingFramesIn10MS - samplesToCopy) * sizeof(short),
              int8_t(0)
            );
          }
        }
      }
    }

    drainLlamaPending();
    if (_ttsBuffer.empty()) {

      // Only process new text when current audio is finished
      bool shouldSynthesize = false;
      std::string textToSpeak;
      if (_tts_enabled) {
        absl::MutexLock lock(&_queueMutex);
        if (!_textQueue.empty()) {
          textToSpeak = _textQueue.front();
          _textQueue.pop();
          shouldSynthesize = true;
          RTC_LOG(LS_INFO) << "Popped text: " << textToSpeak << ", Remaining queue size: " << _textQueue.size();
        }
      }

      if (shouldSynthesize) {
        RTC_LOG(LS_INFO) << "Queueing TTS text: " << textToSpeak;
        SpeechAudioDeviceFactory::SpeakText(
            textToSpeak,
            SpeechAudioDeviceFactory::GetLanguage());
      } else {
        // Send silence for a full 10ms frame when there's nothing to play or synthesize.
        std::fill_n(
            _recordingBuffer,
            _recordingFramesIn10MS * sizeof(short),
            int8_t(0));

        mutex_.Unlock();
        _ptrAudioBuffer->SetRecordedBuffer(_recordingBuffer, _recordingFramesIn10MS);
        _ptrAudioBuffer->DeliverRecordedData();
        mutex_.Lock();
      }
    }

    // If we just processed TTS samples (buffer was not empty), deliver them.
    if (!_ttsBuffer.empty()) {
      mutex_.Unlock();
      _ptrAudioBuffer->SetRecordedBuffer(_recordingBuffer, _recordingFramesIn10MS);
      _ptrAudioBuffer->DeliverRecordedData();
      mutex_.Lock();
    }

    _lastCallRecordMillis = currentTime;
  }

  mutex_.Unlock();

  // Sleep out the rest of this 10ms period. Sleeping in the next iteration
  // added a full extra 10ms, so the sender ran at about half rate and the
  // receiver heard gaps where the missing samples should have been.
  int64_t deltaTimeMillis = webrtc::TimeMillis() - currentTime;
  if (deltaTimeMillis < 10) {
    webrtc::Thread::SleepMs(10 - deltaTimeMillis);
  }
  return true;
}

void WhisperAudioDevice::AttachAudioBuffer(AudioDeviceBuffer* audioBuffer) {
  MutexLock lock(&mutex_);
  _ptrAudioBuffer = audioBuffer;

  _ptrAudioBuffer->SetRecordingSampleRate(kRecordingFixedSampleRate);
  _ptrAudioBuffer->SetPlayoutSampleRate(kPlayoutFixedSampleRate);
  _ptrAudioBuffer->SetRecordingChannels(1);
  _ptrAudioBuffer->SetPlayoutChannels(1);
}

// 
// Playout block
// 

int16_t WhisperAudioDevice::PlayoutDevices() {
  return 1;
}

int32_t WhisperAudioDevice::PlayoutDeviceName(uint16_t index,
                                           char name[kAdmMaxDeviceNameSize],
                                           char guid[kAdmMaxGuidSize]) {
  const char* kName = "whisper_playout_device";
  const char* kGuid = "951ba178-fbd1-47d1-96be-965b17d56d5b";
  if (index < 1) {
    webrtc::strcpyn(name, kAdmMaxDeviceNameSize, kName);
    webrtc::strcpyn(guid, kAdmMaxGuidSize, kGuid);
    return 0;
  }
  return -1;
}

int32_t WhisperAudioDevice::SetPlayoutDevice(uint16_t index) {
  if (index == 0) {
    return 0;
  }
  return -1;
}

int32_t WhisperAudioDevice::SetPlayoutDevice(
    AudioDeviceModule::WindowsDeviceType device) {
  return -1;
}

void WhisperAudioDevice::StartSpeechEngines() {
  if (_engines_started) {
    return;
  }
  _engines_started = true;

  _tts = SpeechAudioDeviceFactory::CreateWhillatsTTS(_ttsCallback);
  if(_tts && _tts->start()) {
    SpeechAudioDeviceFactory::ApplyTTSVoice();
    _tts_enabled = true;
    RTC_LOG(LS_INFO) << "TTS enabled...";
  }

  {
    std::lock_guard<std::mutex> tlock(_transcriber_mutex);
    _whisper_transcriber = SpeechAudioDeviceFactory::CreateWhillatsTranscriber(_whisperCallback, _languageCallback);
    if (_whisper_transcriber) _whisper_transcriber->setTranscriptCallback(_transcriptCallback);
    if (_whisper_transcriber && _whisper_transcriber->start()) {
      _whisper_enabled = true;
      RTC_LOG(LS_INFO) << "Whisper enabled, model: " << SpeechAudioDeviceFactory::GetWhisperModelFilename() << "...";
    }
  }

  _llama_device = SpeechAudioDeviceFactory::CreateWhillatsLlama(_llamaResponseCallback);
  if(_llama_device &&  _llama_device->start()) {
    _llama_enabled = true;
    RTC_LOG(LS_INFO) << "Llama enabled, model: " << SpeechAudioDeviceFactory::GetLlamaModelFilename() << "...";
  }
}

int32_t WhisperAudioDevice::InitPlayout() {
  MutexLock lock(&mutex_);

  if (_playing) {
    return -1;
  }

  StartSpeechEngines();

  _playoutFramesIn10MS = static_cast<size_t>(kPlayoutFixedSampleRate / 100);

  if (_ptrAudioBuffer) {
    // Update webrtc audio buffer with the selected parameters
    _ptrAudioBuffer->SetPlayoutSampleRate(kPlayoutFixedSampleRate);
    _ptrAudioBuffer->SetPlayoutChannels(kPlayoutNumChannels);
  }

  return 0;
}

int32_t WhisperAudioDevice::PlayoutIsAvailable(bool& available) {
  available = true;
  return 0;
}

bool WhisperAudioDevice::PlayoutIsInitialized() const {
  return _playoutFramesIn10MS != 0;
}

int32_t WhisperAudioDevice::StartPlayout() {
  if (_playing) {
    return 0;
  }

  if (!PlayoutIsInitialized()) {
    InitPlayout();
  }

  _playing = true;
  _playoutFramesLeft = 0;

  if (!_playoutBuffer) {
    _playoutBuffer = new int8_t[kPlayoutBufferSize];
  }
  if (!_playoutBuffer) {
    _playing = false;
    return -1;
  }

  #if defined(PLAY_WAV_ON_PLAY)
  if (!_wavFilename.empty()) {
    _playFile = FileWrapper::OpenReadOnly(_wavFilename);
    if (!_playFile.is_open()) {
      RTC_LOG(LS_ERROR) << "Failed to open 'playout' file: " << _wavFilename;
      _playing = false;
      delete[] _playoutBuffer;
      _playoutBuffer = NULL;
      return -1;
    }
  }
  #endif // defined(PLAY_WAV_ON_PLAY)

  // "PLAYOUT"
  _ptrThreadPlay = webrtc::PlatformThread::SpawnJoinable(
      [this] {
        while (PlayThreadProcess()) {
        }
      },
      "webrtc_audio_module_play_thread",
      webrtc::ThreadAttributes().SetPriority(webrtc::ThreadPriority::kRealtime));

  RTC_LOG(LS_INFO) << "Started playout...";
  return 0;
}

int32_t WhisperAudioDevice::StopPlayout() {
  {
    MutexLock lock(&mutex_);
    _playing = false;
  }

  // stop playout thread first
  if (!_ptrThreadPlay.empty())
    _ptrThreadPlay.Finalize();

  if (_tts) {
    std::queue<std::string> empty;
    std::swap(_textQueue, empty);
    _tts->setCallback({nullptr, nullptr});
    _tts->stop();
    _tts = nullptr;
  }

  if (_llama_device) {
    _llama_device->setCallback({nullptr, nullptr});
    _llama_device->stop();
    _llama_device = nullptr;
  }

  {
    std::lock_guard<std::mutex> tlock(_transcriber_mutex);
    if (_whisper_transcriber) {
        _whisper_transcriber->setCallback({nullptr, nullptr});
        _whisper_transcriber->setLanguageCallback({nullptr, nullptr});
        _whisper_transcriber->setTranscriptCallback({nullptr, nullptr});
        _whisper_transcriber->stop();
        _whisper_transcriber = nullptr;
    }
  }

  // Reset TTS state for next call
  {
    absl::MutexLock lock(&_queueMutex);
    _ttsBuffer.clear();
    _ttsIndex = 0;
    std::queue<std::string> empty;
    std::swap(_textQueue, empty);
  }

  MutexLock lock(&mutex_);

  _playoutFramesLeft = 0;
  delete[] _playoutBuffer;
  _playoutBuffer = NULL;

  // Release cached helper devices so that the next AudioDevice instance will
  // recreate them with fresh callbacks.
  _engines_started = false;
  SpeechAudioDeviceFactory::ResetDevices();

  return 0;
}

bool WhisperAudioDevice::PlayThreadProcess() {
  if (!_playing) {
    return false;
  }

  int64_t currentTime = webrtc::TimeMillis();
  mutex_.Lock();

  if (_lastCallPlayoutMillis == 0 ||
      currentTime - _lastCallPlayoutMillis >= 10) {
    mutex_.Unlock();
    _ptrAudioBuffer->RequestPlayoutData(_playoutFramesIn10MS);
    mutex_.Lock();

    _playoutFramesLeft = _ptrAudioBuffer->GetPlayoutData(_playoutBuffer);
    RTC_DCHECK_EQ(_playoutFramesIn10MS, _playoutFramesLeft);

    #if defined(PLAY_WAV_ON_PLAY)
    if (_playFile.is_open()) {
      if (_playFile.Read(_playoutBuffer, kPlayoutBufferSize) > 0) {
        #if defined(DUMP_WAV_ON_PLAY)
        HexPrinter::Dump((const uint8_t*) _playoutBuffer, kPlayoutBufferSize);
        #endif
      } else {
        _playFile.Rewind();
      }
      if(_playFile.ReadEof())
        _playFile.Close();
    }
    #endif // defined(PLAY_WAV_ON_PLAY)

    // Barge-in: Whisper recognised remote speech while TTS is playing.
    {
      const uint32_t heard = _heardSpeech.load(std::memory_order_relaxed);
      if (heard != _heardSpeechSeen) {
        _heardSpeechSeen = heard;
        int64_t sinceLastInterrupt = currentTime - _lastInterruptMillis.load(std::memory_order_relaxed);
        int64_t ttsPlayDuration = currentTime - _ttsStartedMillis.load(std::memory_order_relaxed);
        if (_isSpeaking.load(std::memory_order_relaxed) &&
            sinceLastInterrupt > kInterruptCooldownMs && ttsPlayDuration > kMinTTSPlayMs) {
          RTC_LOG(LS_INFO) << "Barge-in: remote speech recognised after " << ttsPlayDuration
                           << " ms of TTS, interrupting LLaMa/TTS";
          _lastInterruptMillis.store(currentTime, std::memory_order_relaxed);
          mutex_.Unlock();
          interruptLlama();
          mutex_.Lock();
        }
      }
    }

    {
      std::lock_guard<std::mutex> tlock(_transcriber_mutex);
      // While TTS is playing, the browser mic hears that audio. Feeding it to
      // Whisper transcribes the robot's own voice and cuts the sentence off.
      if (_whisper_transcriber && !_isSpeaking.load(std::memory_order_relaxed)) {
        _whisper_transcriber->processAudioBuffer(reinterpret_cast<uint8_t*>(_playoutBuffer),
                                                kPlayoutBufferSize);
      }
    }
 
    _lastCallPlayoutMillis = currentTime;
  }

  _playoutFramesLeft = 0;
  mutex_.Unlock();

  int64_t deltaTimeMillis = webrtc::TimeMillis() - currentTime;
  if (deltaTimeMillis < 10) {
    webrtc::Thread::SleepMs(10 - deltaTimeMillis);
  }

  return true;
}

bool WhisperAudioDevice::Playing() const {
  return _playing;
}

int32_t WhisperAudioDevice::InitSpeaker() {
  return 0;
}

bool WhisperAudioDevice::SpeakerIsInitialized() const {
  return true;
}

//
// Pure virtual ooverrides
//

// Other required methods remain the same as in previous implementation
// (Dummy implementations for methods not specifically required)
int32_t WhisperAudioDevice::SpeakerVolumeIsAvailable(bool& /* available */) {
  return -1;
}
int32_t WhisperAudioDevice::SetSpeakerVolume(uint32_t /* volume */) {
  return -1;
}
int32_t WhisperAudioDevice::SpeakerVolume(uint32_t& /* volume */) const {
  return -1;
}
int32_t WhisperAudioDevice::MaxSpeakerVolume(uint32_t& /* maxVolume */) const {
  return -1;
}
int32_t WhisperAudioDevice::MinSpeakerVolume(uint32_t& /* minVolume */) const {
  return -1;
}
int32_t WhisperAudioDevice:: MicrophoneVolumeIsAvailable(bool& /* available */) {
  return -1;
}
int32_t WhisperAudioDevice::SetMicrophoneVolume(uint32_t /* volume */) {
  return -1;
}
int32_t WhisperAudioDevice::MicrophoneVolume(uint32_t& /* volume */) const {
  return -1;
}
int32_t WhisperAudioDevice::MaxMicrophoneVolume(uint32_t& /* maxVolume */) const {
  return -1;
}
int32_t WhisperAudioDevice::MinMicrophoneVolume(uint32_t& /* minVolume */) const {
  return -1;
}
int32_t WhisperAudioDevice::SpeakerMuteIsAvailable(bool& /* available */) {
  return -1;
}
int32_t WhisperAudioDevice::SetSpeakerMute(bool /* enable */) {
  return -1;
}
int32_t WhisperAudioDevice::SpeakerMute(bool& /* enabled */) const {
  return -1;
}
int32_t WhisperAudioDevice::MicrophoneMuteIsAvailable(bool& /* available */) {
  return -1;
}
int32_t WhisperAudioDevice::SetMicrophoneMute(bool /* enable */) {
  return -1;
}
int32_t WhisperAudioDevice::MicrophoneMute(bool& /* enabled */) const {
  return -1;
}
int32_t WhisperAudioDevice::StereoPlayoutIsAvailable(bool& /* available */) {
  return -1;
}
int32_t WhisperAudioDevice::SetStereoPlayout(bool /* enable */) {
  return -1;
}
int32_t WhisperAudioDevice::StereoPlayout(bool& /* enabled */) const {
  return -1;
}
int32_t WhisperAudioDevice::StereoRecordingIsAvailable(bool& /* available */) {
  return -1;
}
int32_t WhisperAudioDevice::SetStereoRecording(bool /* enable */) {
  return -1;
}
int32_t WhisperAudioDevice::StereoRecording(bool& /* enabled */) const {
  return -1;
}
int32_t WhisperAudioDevice::PlayoutDelay(uint16_t& delayMS) const {
  delayMS = _lastCallPlayoutMillis;
  return 0;
}

}  // namespace webrtc
