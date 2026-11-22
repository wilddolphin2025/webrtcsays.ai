// Deterministic packet loss and jitter for the quality ladder.
// No clock and no root. The same rule is used by the in-process NetEq loop
// and by the live directcall encoder wrapper.
//
// Loss: packet_index % 100 < loss_pct. 3% drops packets 0, 1, and 2 of every
// hundred. Jitter: delay cycles through 0, jitter/2, and jitter milliseconds.

#ifndef EXAMPLES_DIRECT_IMPAIR_SCHEDULE_H_
#define EXAMPLES_DIRECT_IMPAIR_SCHEDULE_H_

namespace webrtc {
namespace quality {

inline bool DropPacket(int packet_index, int loss_pct) {
  if (loss_pct <= 0 || packet_index < 0) return false;
  if (loss_pct >= 100) return true;
  return (packet_index % 100) < loss_pct;
}

inline int DelayMs(int packet_index, int jitter_ms) {
  if (jitter_ms <= 0 || packet_index < 0) return 0;
  return (packet_index % 3) * (jitter_ms / 2);
}

}  // namespace quality
}  // namespace webrtc

#endif
