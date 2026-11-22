/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// Unit tests for DecisionLogic class and derived classes.

#include "modules/audio_coding/neteq/decision_logic.h"

#include <memory>
#include <optional>
#include <utility>

#include "api/neteq/neteq.h"
#include "api/neteq/neteq_controller.h"
#include "api/neteq/tick_timer.h"
#include "modules/audio_coding/neteq/delay_manager.h"
#include "modules/audio_coding/neteq/mock/mock_buffer_level_filter.h"
#include "modules/audio_coding/neteq/mock/mock_delay_manager.h"
#include "modules/audio_coding/neteq/mock/mock_packet_arrival_history.h"
#include "test/create_test_field_trials.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace webrtc {

namespace {

using ::testing::_;
using ::testing::Return;

constexpr int kSampleRate = 8000;
constexpr int kSamplesPerMs = kSampleRate / 1000;
constexpr int kOutputSizeSamples = kSamplesPerMs * 10;
constexpr int kMinTimescaleInterval = 5;

NetEqController::NetEqStatus CreateNetEqStatus(NetEq::Mode last_mode,
                                               int current_delay_ms) {
  NetEqController::NetEqStatus status;
  status.play_dtmf = false;
  status.last_mode = last_mode;
  status.target_timestamp = 1234;
  status.generated_noise_samples = 0;
  status.expand_mutefactor = 0;
  status.packet_buffer_info.num_samples = current_delay_ms * kSamplesPerMs;
  status.packet_buffer_info.span_samples = current_delay_ms * kSamplesPerMs;
  status.packet_buffer_info.span_samples_wait_time =
      current_delay_ms * kSamplesPerMs;
  status.packet_buffer_info.dtx_or_cng = false;
  status.next_packet = {
      .timestamp = status.target_timestamp, .is_dtx = false, .is_cng = false};
  status.sync_buffer_samples = 0;
  return status;
}

}  // namespace

class DecisionLogicTest : public ::testing::Test {
 protected:
  DecisionLogicTest() {
    NetEqController::Config config;
    config.tick_timer = &tick_timer_;
    config.allow_time_stretching = true;
    config.max_packets_in_buffer = 200;
    config.base_min_delay_ms = 0;
    auto delay_manager = std::make_unique<MockDelayManager>(
        DelayManager::Config(CreateTestFieldTrials()), config.tick_timer);
    mock_delay_manager_ = delay_manager.get();
    auto buffer_level_filter = std::make_unique<MockBufferLevelFilter>();
    mock_buffer_level_filter_ = buffer_level_filter.get();
    auto packet_arrival_history =
        std::make_unique<MockPacketArrivalHistory>(&tick_timer_);
    mock_packet_arrival_history_ = packet_arrival_history.get();
    decision_logic_ = std::make_unique<DecisionLogic>(
        config, std::move(delay_manager), std::move(buffer_level_filter),
        std::move(packet_arrival_history));
    decision_logic_->SetSampleRate(kSampleRate, kOutputSizeSamples);
  }

  TickTimer tick_timer_;
  std::unique_ptr<DecisionLogic> decision_logic_;
  MockDelayManager* mock_delay_manager_;
  MockBufferLevelFilter* mock_buffer_level_filter_;
  MockPacketArrivalHistory* mock_packet_arrival_history_;
};

TEST_F(DecisionLogicTest, NormalOperation) {
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(100));
  EXPECT_CALL(*mock_packet_arrival_history_, GetDelayMs(_))
      .WillRepeatedly(Return(100));
  EXPECT_CALL(*mock_packet_arrival_history_, GetMaxDelayMs())
      .WillRepeatedly(Return(0));

  bool reset_decoder = false;
  tick_timer_.Increment(kMinTimescaleInterval + 1);
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kNormal, 100), &reset_decoder),
            NetEq::Operation::kNormal);
  EXPECT_FALSE(reset_decoder);
}

TEST_F(DecisionLogicTest, Accelerate) {
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(100));
  EXPECT_CALL(*mock_packet_arrival_history_, GetDelayMs(_))
      .WillRepeatedly(Return(150));
  EXPECT_CALL(*mock_packet_arrival_history_, GetMaxDelayMs())
      .WillRepeatedly(Return(0));

  bool reset_decoder = false;
  tick_timer_.Increment(kMinTimescaleInterval + 1);
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kNormal, 100), &reset_decoder),
            NetEq::Operation::kAccelerate);
  EXPECT_FALSE(reset_decoder);
}

TEST_F(DecisionLogicTest, FastAccelerate) {
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(100));
  EXPECT_CALL(*mock_packet_arrival_history_, GetDelayMs(_))
      .WillRepeatedly(Return(500));
  EXPECT_CALL(*mock_packet_arrival_history_, GetMaxDelayMs())
      .WillRepeatedly(Return(0));

  bool reset_decoder = false;
  tick_timer_.Increment(kMinTimescaleInterval + 1);
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kNormal, 100), &reset_decoder),
            NetEq::Operation::kFastAccelerate);
  EXPECT_FALSE(reset_decoder);
}

// One late packet in the arrival history raises the accelerate threshold to
// target + that delay; without a maximum delay NetEq keeps the buffer.
TEST_F(DecisionLogicTest, LateArrivalHistoryDefersAccelerate) {
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(60));
  EXPECT_CALL(*mock_packet_arrival_history_, GetDelayMs(_))
      .WillRepeatedly(Return(150));
  EXPECT_CALL(*mock_packet_arrival_history_, GetMaxDelayMs())
      .WillRepeatedly(Return(100));

  bool reset_decoder = false;
  tick_timer_.Increment(kMinTimescaleInterval + 1);
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kNormal, 100), &reset_decoder),
            NetEq::Operation::kNormal);
}

// With a maximum delay (low-latency mode) the playout delay is bounded by
// max(target, maximum) + 20 ms, so the same buffer is accelerated.
TEST_F(DecisionLogicTest, MaximumDelayBoundsPlayoutDelay) {
  ASSERT_TRUE(decision_logic_->SetMaximumDelay(80));
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(60));
  EXPECT_CALL(*mock_packet_arrival_history_, GetDelayMs(_))
      .WillRepeatedly(Return(150));
  EXPECT_CALL(*mock_packet_arrival_history_, GetMaxDelayMs())
      .WillRepeatedly(Return(100));

  bool reset_decoder = false;
  tick_timer_.Increment(kMinTimescaleInterval + 1);
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kNormal, 100), &reset_decoder),
            NetEq::Operation::kAccelerate);
}

// The cap clamps the target too: a 300 ms target becomes 80 ms.
TEST_F(DecisionLogicTest, MaximumDelayClampsTarget) {
  ASSERT_TRUE(decision_logic_->SetMaximumDelay(80));
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(300));
  EXPECT_EQ(decision_logic_->TargetLevelMs(), 80);
}

// With a cap, the first packet after a DTX gap doesn't feed the delay
// estimate; the packets that follow it do. Without a cap nothing changes.
TEST(DecisionLogicDtxTest, TalkspurtStartSkipsDelayEstimateWhenCapped) {
  TickTimer tick_timer;
  NetEqController::Config config;
  config.tick_timer = &tick_timer;
  config.allow_time_stretching = true;
  config.max_packets_in_buffer = 200;
  config.base_min_delay_ms = 0;
  auto delay_manager = std::make_unique<::testing::NiceMock<MockDelayManager>>(
      DelayManager::Config(CreateTestFieldTrials()), &tick_timer);
  DecisionLogic logic(config, std::move(delay_manager),
                      std::make_unique<::testing::NiceMock<MockBufferLevelFilter>>(),
                      std::make_unique<PacketArrivalHistory>(&tick_timer, 2000));
  logic.SetSampleRate(kSampleRate, kOutputSizeSamples);
  const size_t len = 20 * kSamplesPerMs;
  auto arrive = [&](uint32_t ts) {
    NetEqController::PacketArrivedInfo info = {};
    info.packet_length_samples = len;
    info.main_timestamp = ts;
    tick_timer.Increment(2);
    return logic.PacketArrived(kSampleRate, true, info);
  };
  ASSERT_TRUE(logic.SetMaximumDelay(80));
  arrive(0);
  EXPECT_TRUE(arrive(len).has_value());
  EXPECT_FALSE(arrive(len * 20).has_value());  // talkspurt after 360 ms of DTX
  EXPECT_TRUE(arrive(len * 21).has_value());

  ASSERT_TRUE(logic.SetMaximumDelay(0));
  EXPECT_TRUE(arrive(len * 40).has_value());  // uncapped: stock behavior
}

TEST_F(DecisionLogicTest, PreemptiveExpand) {
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(100));
  EXPECT_CALL(*mock_packet_arrival_history_, GetDelayMs(_))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(*mock_packet_arrival_history_, GetMaxDelayMs())
      .WillRepeatedly(Return(0));

  bool reset_decoder = false;
  tick_timer_.Increment(kMinTimescaleInterval + 1);
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kNormal, 100), &reset_decoder),
            NetEq::Operation::kPreemptiveExpand);
  EXPECT_FALSE(reset_decoder);
}

TEST_F(DecisionLogicTest, PostponeDecodeAfterExpand) {
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(500));

  // Below 50% target delay threshold.
  bool reset_decoder = false;
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kExpand, 200), &reset_decoder),
            NetEq::Operation::kExpand);
  EXPECT_FALSE(reset_decoder);

  // Above 50% target delay threshold.
  EXPECT_EQ(decision_logic_->GetDecision(
                CreateNetEqStatus(NetEq::Mode::kExpand, 250), &reset_decoder),
            NetEq::Operation::kNormal);
  EXPECT_FALSE(reset_decoder);
}

TEST_F(DecisionLogicTest, TimeStrechComfortNoise) {
  EXPECT_CALL(*mock_delay_manager_, TargetDelayMs())
      .WillRepeatedly(Return(500));

  {
    bool reset_decoder = false;
    // Below target window.
    auto status = CreateNetEqStatus(NetEq::Mode::kCodecInternalCng, 200);
    status.generated_noise_samples = 400 * kSamplesPerMs;
    status.next_packet->timestamp =
        status.target_timestamp + 400 * kSamplesPerMs;
    EXPECT_EQ(decision_logic_->GetDecision(status, &reset_decoder),
              NetEq::Operation::kCodecInternalCng);
    EXPECT_FALSE(reset_decoder);
  }

  {
    bool reset_decoder = false;
    // Above target window.
    auto status = CreateNetEqStatus(NetEq::Mode::kCodecInternalCng, 600);
    status.generated_noise_samples = 200 * kSamplesPerMs;
    status.next_packet->timestamp =
        status.target_timestamp + 400 * kSamplesPerMs;
    EXPECT_EQ(decision_logic_->GetDecision(status, &reset_decoder),
              NetEq::Operation::kNormal);
    EXPECT_FALSE(reset_decoder);
  }
}

TEST_F(DecisionLogicTest, CngTimeout) {
  auto status = CreateNetEqStatus(NetEq::Mode::kCodecInternalCng, 0);
  status.next_packet = std::nullopt;
  status.generated_noise_samples = kSamplesPerMs * 500;
  bool reset_decoder = false;
  EXPECT_EQ(decision_logic_->GetDecision(status, &reset_decoder),
            NetEq::Operation::kCodecInternalCng);
  status.generated_noise_samples = kSamplesPerMs * 1010;
  EXPECT_EQ(decision_logic_->GetDecision(status, &reset_decoder),
            NetEq::Operation::kExpand);
}

}  // namespace webrtc
