// Standalone check for the masked client PONG. Not in examples/BUILD.gn.
//   c++ -std=c++17 -Iexamples/direct examples/direct/wsock_frame_test.cc -o /tmp/wsock_frame_test && /tmp/wsock_frame_test

#include "wsock_frame.h"

#include <cstdio>
#include <cstdlib>

static void Expect(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "FAIL %s\n", what);
  std::exit(1);
}

int main() {
  const unsigned char payload[] = {'l', 's'};
  const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
  const std::vector<unsigned char> pong =
      BuildMaskedClientFrame(0xA, payload, 2, mask);
  Expect(pong.size() == 8, "pong size");
  Expect(pong[0] == 0x8A, "pong opcode");
  Expect(pong[1] == (0x80 | 2), "pong mask bit and length");
  Expect(pong[2] == 0x11 && pong[5] == 0x44, "pong mask key");
  Expect((pong[6] ^ pong[2]) == 'l' && (pong[7] ^ pong[3]) == 's', "pong echoes ls");

  const std::vector<unsigned char> ping =
      BuildMaskedClientFrame(0x9, nullptr, 0, mask);
  Expect(ping.size() == 6, "empty ping size");
  Expect(ping[0] == 0x89 && ping[1] == 0x80, "empty ping is masked");

  const unsigned char close_bytes[] = {0x03, 0xEA, 'b', 'y', 'e'};
  const WsCloseInfo close = CloseInfoFromPayload(close_bytes, 5);
  Expect(close.code == 1002, "close code 1002");
  Expect(close.reason == "bye", "close reason");
  Expect(CloseInfoFromPayload(nullptr, 0).code == 1005, "close without a code");
  Expect(WsPayloadPreview(payload, 2) == "ls", "preview");

  std::puts("ok");
  return 0;
}
