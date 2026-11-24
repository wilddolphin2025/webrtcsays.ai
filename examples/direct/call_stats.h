/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#ifndef EXAMPLES_DIRECT_CALL_STATS_H_
#define EXAMPLES_DIRECT_CALL_STATS_H_

#include <chrono>
#include <functional>
#include <memory>

#include "api/peer_connection_interface.h"

#include <string>

// --stats-jsonl: every GetStats report (interval and final) appended as one JSON
// line {"t_us","final","stats":[...]} with the audio-relevant RTCStats objects
// (inbound-rtp, outbound-rtp, remote-inbound-rtp, candidate-pair, media-playout,
// media-source, transport) in their W3C JSON form. Empty path disables it.
void DirectCallStatsSetJsonlPath(const std::string& path);

class DirectCallStats {
 public:
  virtual ~DirectCallStats() = default;
  virtual void OnIceConnected() = 0;
  virtual void Summarize() = 0;

  static std::unique_ptr<DirectCallStats> Create(
      int interval_sec,
      std::chrono::steady_clock::time_point origin,
      webrtc::Thread* signaling_thread,
      std::function<webrtc::PeerConnectionInterface*()> peer_connection,
      bool encrypted);
};

#endif  // EXAMPLES_DIRECT_CALL_STATS_H_
