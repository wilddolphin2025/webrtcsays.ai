/*
 * Signaling listen address. Empty means the historical INADDR_ANY bind.
 * This chooses the TCP signaling socket and the WebSocket signaling socket
 * only. It is not an ICE or media setting.
 */
#ifndef EXAMPLES_DIRECT_SIGNALING_BIND_H_
#define EXAMPLES_DIRECT_SIGNALING_BIND_H_

#include <arpa/inet.h>
#include <netinet/in.h>

#include <string>

// On success writes the IPv4 address to bind, in network byte order.
// An empty value and "0.0.0.0" both mean INADDR_ANY, which is the behavior
// of binaries that do not know this setting. Any other value must be a
// dotted IPv4 address. Hostnames are rejected so a typo cannot resolve onto
// a public interface. On failure *addr is left unchanged.
inline bool ResolveSignalingBind(const std::string& signaling_bind,
                                 in_addr_t* addr,
                                 std::string* error) {
  if (error) {
    error->clear();
  }
  if (!addr) {
    if (error) {
      *error = "missing address";
    }
    return false;
  }
  if (signaling_bind.empty() || signaling_bind == "0.0.0.0") {
    *addr = htonl(INADDR_ANY);
    return true;
  }
  for (unsigned char ch : signaling_bind) {
    if (ch <= ' ' || ch >= 127) {
      if (error) {
        *error = "signaling_bind must be a dotted IPv4 address";
      }
      return false;
    }
  }
  struct in_addr parsed = {};
  if (inet_pton(AF_INET, signaling_bind.c_str(), &parsed) != 1) {
    if (error) {
      *error = "signaling_bind must be a dotted IPv4 address";
    }
    return false;
  }
  *addr = parsed.s_addr;
  return true;
}

#endif  // EXAMPLES_DIRECT_SIGNALING_BIND_H_
