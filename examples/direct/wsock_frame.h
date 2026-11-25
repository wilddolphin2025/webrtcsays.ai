#ifndef EXAMPLES_DIRECT_WSOCK_FRAME_H_
#define EXAMPLES_DIRECT_WSOCK_FRAME_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// A client-to-server WebSocket frame. RFC 6455 section 5.1 requires the mask
// bit on every frame a client sends. CetaBridge's gorilla/websocket reader
// closes the socket when a PING or PONG arrives unmasked, which is what the
// room client did: the server's 20 s PING (payload "ls") was answered with an
// unmasked PONG, and the server then closed.
inline std::vector<unsigned char> BuildMaskedClientFrame(
    uint8_t opcode, const unsigned char* payload, size_t length,
    const unsigned char mask[4]) {
  std::vector<unsigned char> frame;
  frame.reserve(14 + length);
  frame.push_back(static_cast<unsigned char>(0x80 | (opcode & 0x0F)));
  if (length <= 125) {
    frame.push_back(static_cast<unsigned char>(0x80 | length));
  } else if (length <= 65535) {
    frame.push_back(static_cast<unsigned char>(0x80 | 126));
    frame.push_back(static_cast<unsigned char>((length >> 8) & 0xFF));
    frame.push_back(static_cast<unsigned char>(length & 0xFF));
  } else {
    frame.push_back(static_cast<unsigned char>(0x80 | 127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<unsigned char>((static_cast<uint64_t>(length) >> shift) & 0xFF));
    }
  }
  frame.insert(frame.end(), mask, mask + 4);
  for (size_t i = 0; i < length; ++i) {
    frame.push_back(static_cast<unsigned char>(payload[i] ^ mask[i % 4]));
  }
  return frame;
}

struct WsCloseInfo {
  int code = 1005;  // 1005: no status code was present
  std::string reason;
};

inline WsCloseInfo CloseInfoFromPayload(const unsigned char* payload, size_t length) {
  WsCloseInfo info;
  if (payload && length >= 2) {
    info.code = (static_cast<int>(payload[0]) << 8) | static_cast<int>(payload[1]);
    info.reason.assign(reinterpret_cast<const char*>(payload + 2), length - 2);
  } else if (length == 1) {
    info.code = 1002;
    info.reason = "truncated close payload";
  }
  return info;
}

inline std::string WsPayloadPreview(const unsigned char* payload, size_t length) {
  std::string out;
  if (!payload) return out;
  for (size_t i = 0; i < length && i < 64; ++i) {
    unsigned char c = payload[i];
    if (c >= 32 && c < 127) {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\x%02x", c);
      out += buf;
    }
  }
  return out;
}

#endif  // EXAMPLES_DIRECT_WSOCK_FRAME_H_
