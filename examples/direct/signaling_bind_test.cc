// Standalone check for the signaling listen address. It does not link WebRTC.
#include "signaling_bind.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int failures = 0;

void Expect(bool cond, const char* name) {
  if (!cond) {
    std::fprintf(stderr, "FAIL %s\n", name);
    ++failures;
  }
}

in_addr_t Parse(const char* text) {
  struct in_addr parsed = {};
  if (inet_pton(AF_INET, text, &parsed) != 1) {
    std::fprintf(stderr, "bad fixture %s\n", text);
    std::exit(2);
  }
  return parsed.s_addr;
}

}  // namespace

int main() {
  in_addr_t addr = 0x11111111;
  std::string error = "stale";
  Expect(ResolveSignalingBind("", &addr, &error), "empty ok");
  Expect(addr == htonl(INADDR_ANY), "empty is any");
  Expect(error.empty(), "empty clears error");

  addr = 0x22222222;
  error = "stale";
  Expect(ResolveSignalingBind("0.0.0.0", &addr, &error), "any literal ok");
  Expect(addr == htonl(INADDR_ANY), "any literal");

  addr = 0x33333333;
  Expect(ResolveSignalingBind("127.0.0.1", &addr, &error), "loopback ok");
  Expect(addr == Parse("127.0.0.1"), "loopback value");
  Expect(addr != htonl(INADDR_ANY), "loopback is not any");

  const in_addr_t sentinel = 0x44444444;
  addr = sentinel;
  Expect(!ResolveSignalingBind("localhost", &addr, &error), "reject hostname");
  Expect(addr == sentinel, "hostname leaves addr");
  Expect(!error.empty(), "hostname explains");

  addr = sentinel;
  Expect(!ResolveSignalingBind("turn.wilddolphin.us", &addr, &error),
         "reject dns");
  Expect(addr == sentinel, "dns leaves addr");

  addr = sentinel;
  Expect(!ResolveSignalingBind("::1", &addr, &error), "reject ipv6");
  Expect(addr == sentinel, "ipv6 leaves addr");

  addr = sentinel;
  Expect(!ResolveSignalingBind("127.0.0.1 ", &addr, &error), "reject space");
  Expect(addr == sentinel, "space leaves addr");

  addr = sentinel;
  Expect(ResolveSignalingBind("10.1.2.3", &addr, &error), "unicast ok");
  Expect(addr == Parse("10.1.2.3"), "unicast value");

  Expect(!ResolveSignalingBind("127.0.0.1", nullptr, &error), "null out");

  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("signaling_bind ok\n");
  return 0;
}
