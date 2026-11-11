/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#ifndef EXAMPLES_DIRECT_SDP_DIAG_H_
#define EXAMPLES_DIRECT_SDP_DIAG_H_

#include <cctype>
#include <sstream>
#include <string>

// Signaling and SDP lines safe to print. ICE usernames, passwords, candidates,
// fingerprints, and anything after a token-like prefix are omitted.
inline std::string RedactedSignalingLine(const std::string& raw) {
  const auto nl = raw.find_first_of("\r\n");
  const std::string first = nl == std::string::npos ? raw : raw.substr(0, nl);
  auto starts = [&](const char* prefix) {
    return first.rfind(prefix, 0) == 0;
  };
  if (starts("OFFER:") || starts("ANSWER:")) {
    const char* kind = starts("OFFER:") ? "OFFER" : "ANSWER";
    std::ostringstream out;
    out << kind << " <sdp " << raw.size() << " bytes; codecs only>";
    std::istringstream lines(raw);
    std::string line;
    int shown = 0;
    while (std::getline(lines, line) && shown < 24) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("m=", 0) == 0 || line.rfind("a=rtpmap:", 0) == 0 ||
          line.rfind("a=fmtp:", 0) == 0 || line.rfind("a=mid:", 0) == 0 ||
          line.rfind("a=sendrecv", 0) == 0 || line.rfind("a=sendonly", 0) == 0 ||
          line.rfind("a=recvonly", 0) == 0 || line.rfind("a=inactive", 0) == 0) {
        out << "\n  " << line;
        ++shown;
      }
    }
    return out.str();
  }
  if (starts("ICE:")) return "ICE <candidate redacted>";
  if (first.size() > 180) return first.substr(0, 32) + " <truncated>";
  return first;
}

#endif  // EXAMPLES_DIRECT_SDP_DIAG_H_
