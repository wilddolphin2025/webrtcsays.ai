#include "impair.h"

#include <cstdio>
#include <deque>
#include <utility>
#include <vector>

#include "api/audio_codecs/audio_encoder.h"
#include "api/environment/environment.h"
#include "api/make_ref_counted.h"
#include "examples/direct/impair_schedule.h"
#include "rtc_base/buffer.h"

namespace webrtc {
namespace {

struct HeldPacket {
  int due_ms = 0;
  AudioEncoder::EncodedInfo info;
  std::vector<uint8_t> payload;
};

class ImpairEncoder final : public AudioEncoder {
 public:
  ImpairEncoder(std::unique_ptr<AudioEncoder> inner, int loss_pct, int jitter_ms)
      : inner_(std::move(inner)), loss_pct_(loss_pct), jitter_ms_(jitter_ms) {
    fprintf(stderr,
            "impair in-process loss=%d%% jitter_ms=%d pattern=0,half,full (no pf/dnctl)\n",
            loss_pct_, jitter_ms_);
  }

  int SampleRateHz() const override { return inner_->SampleRateHz(); }
  size_t NumChannels() const override { return inner_->NumChannels(); }
  int RtpTimestampRateHz() const override { return inner_->RtpTimestampRateHz(); }
  size_t Num10MsFramesInNextPacket() const override { return inner_->Num10MsFramesInNextPacket(); }
  size_t Max10MsFramesInAPacket() const override { return inner_->Max10MsFramesInAPacket(); }
  int GetTargetBitrate() const override { return inner_->GetTargetBitrate(); }
  void Reset() override {
    inner_->Reset();
    held_.clear();
    media_ms_ = 0;
    packet_index_ = 0;
  }
  bool SetFec(bool enable) override { return inner_->SetFec(enable); }
  bool SetDtx(bool enable) override { return inner_->SetDtx(enable); }
  bool GetDtx() const override { return inner_->GetDtx(); }
  bool SetApplication(Application application) override { return inner_->SetApplication(application); }
  void SetMaxPlaybackRate(int frequency_hz) override { inner_->SetMaxPlaybackRate(frequency_hz); }
  void OnReceivedUplinkPacketLossFraction(float v) override {
    inner_->OnReceivedUplinkPacketLossFraction(v);
  }
  void OnReceivedTargetAudioBitrate(int bps) override { inner_->OnReceivedTargetAudioBitrate(bps); }
  void OnReceivedUplinkAllocation(BitrateAllocationUpdate update) override {
    inner_->OnReceivedUplinkAllocation(update);
  }
  void OnReceivedRtt(int rtt_ms) override { inner_->OnReceivedRtt(rtt_ms); }
  void OnReceivedOverhead(size_t overhead) override { inner_->OnReceivedOverhead(overhead); }
  void SetReceiverFrameLengthRange(int min_ms, int max_ms) override {
    inner_->SetReceiverFrameLengthRange(min_ms, max_ms);
  }
  ANAStats GetANAStats() const override { return inner_->GetANAStats(); }
  std::optional<std::pair<TimeDelta, TimeDelta>> GetFrameLengthRange() const override {
    return inner_->GetFrameLengthRange();
  }

 protected:
  EncodedInfo EncodeImpl(uint32_t rtp_timestamp, std::span<const int16_t> audio,
                         Buffer* encoded) override {
    media_ms_ += 10;
    const size_t before = encoded->size();
    EncodedInfo produced = inner_->Encode(rtp_timestamp, audio, encoded);
    if (produced.encoded_bytes > 0 && encoded->size() > before) {
      const size_t n = encoded->size() - before;
      HeldPacket held;
      held.due_ms = media_ms_ + quality::DelayMs(packet_index_, jitter_ms_);
      held.info = produced;
      held.info.encoded_bytes = n;
      held.payload.assign(encoded->data() + before, encoded->data() + encoded->size());
      encoded->SetSize(before);
      if (!quality::DropPacket(packet_index_, loss_pct_)) {
        held_.push_back(std::move(held));
      }
      ++packet_index_;
    }
    if (!held_.empty() && held_.front().due_ms <= media_ms_) {
      HeldPacket due = std::move(held_.front());
      held_.pop_front();
      encoded->AppendData(due.payload.data(), due.payload.size());
      return due.info;
    }
    EncodedInfo empty;
    empty.encoded_bytes = 0;
    empty.encoded_timestamp = rtp_timestamp;
    empty.payload_type = produced.payload_type;
    return empty;
  }

 private:
  std::unique_ptr<AudioEncoder> inner_;
  int loss_pct_ = 0;
  int jitter_ms_ = 0;
  int media_ms_ = 0;
  int packet_index_ = 0;
  std::deque<HeldPacket> held_;
};

class ImpairEncoderFactory : public AudioEncoderFactory {
 public:
  ImpairEncoderFactory(scoped_refptr<AudioEncoderFactory> inner, int loss_pct, int jitter_ms)
      : inner_(std::move(inner)), loss_pct_(loss_pct), jitter_ms_(jitter_ms) {}
  std::vector<AudioCodecSpec> GetSupportedEncoders() override {
    return inner_->GetSupportedEncoders();
  }
  std::optional<AudioCodecInfo> QueryAudioEncoder(const SdpAudioFormat& format) override {
    return inner_->QueryAudioEncoder(format);
  }
  std::unique_ptr<AudioEncoder> Create(const Environment& env, const SdpAudioFormat& format,
                                      Options options) override {
    auto enc = inner_->Create(env, format, options);
    if (!enc) return nullptr;
    return std::make_unique<ImpairEncoder>(std::move(enc), loss_pct_, jitter_ms_);
  }

 private:
  scoped_refptr<AudioEncoderFactory> inner_;
  int loss_pct_ = 0;
  int jitter_ms_ = 0;
};

}  // namespace

scoped_refptr<AudioEncoderFactory> CreateImpairingAudioEncoderFactory(
    scoped_refptr<AudioEncoderFactory> inner, int loss_pct, int jitter_ms) {
  return make_ref_counted<ImpairEncoderFactory>(std::move(inner), loss_pct, jitter_ms);
}

}  // namespace webrtc
