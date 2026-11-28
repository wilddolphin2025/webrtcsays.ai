/*
 * CetaCall CETACALL token bridge for the Direct GATT path.
 *
 * This is deliberately independent of the WebRTC PeerConnection. It exposes
 * the same StreamEncoder/StreamDecoder used by a normal call so a BLE token
 * transport can send tight 17-byte, 80 ms CETACALL payloads.
 */

#ifndef EXAMPLES_DIRECT_BLE_TOKEN_AUDIO_H_
#define EXAMPLES_DIRECT_BLE_TOKEN_AUDIO_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#if defined(DIRECT_BUILDING_DLL)
#define DIRECT_API __declspec(dllexport)
#else
#define DIRECT_API __declspec(dllimport)
#endif
#else
#define DIRECT_API __attribute__((visibility("default")))
#endif

namespace cetacall {

class DIRECT_API BleTokenAudioCodec {
 public:
  static std::unique_ptr<BleTokenAudioCodec> Create(
      const std::string& model_dir,
      std::string* error);
  ~BleTokenAudioCodec();

  BleTokenAudioCodec(const BleTokenAudioCodec&) = delete;
  BleTokenAudioCodec& operator=(const BleTokenAudioCodec&) = delete;

  // Mono PCM at 24000 or 48000 Hz. Appends one or more tight CETACALL
  // payloads (17 bytes per 80 ms frame).
  bool Encode(const int16_t* pcm,
              size_t samples,
              int sample_rate_hz,
              std::vector<std::vector<uint8_t>>* payloads,
              std::string* error);

  // One tight/u16 CETACALL payload. Output PCM is 24000 or 48000 Hz mono.
  bool Decode(const uint8_t* payload,
              size_t size,
              int output_rate_hz,
              std::vector<int16_t>* pcm,
              std::string* error);

 private:
  struct Impl;
  explicit BleTokenAudioCodec(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cetacall

#endif  // EXAMPLES_DIRECT_BLE_TOKEN_AUDIO_H_
