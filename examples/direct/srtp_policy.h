#ifndef EXAMPLES_DIRECT_SRTP_POLICY_H_
#define EXAMPLES_DIRECT_SRTP_POLICY_H_

// SRTP profiles offered on a DTLS handshake. DtlsTransportInternalImpl copies
// this list into its srtp_ciphers_ and passes it to
// SSLStreamAdapter::SetDtlsSrtpCryptoSuites. An empty list skips
// SSL_CTX_set_tlsext_use_srtp, so the ServerHello has no use_srtp extension.
//
// The same CreatePeerConnection runs for the caller and the callee, and for
// direct TCP and the room WebSocket. LiquidCall's frozen interface is
// AES_CM_128_HMAC_SHA1_32 only. SHA1_80 is not offered with it: when we are
// the DTLS server, the peer's ClientHello order wins, and Pion lists SHA1_80
// ahead of SHA1_32. Offering 80 would select 80. Pion and our own clients
// both accept SHA1_32, so 32 alone is the profile that is actually negotiated.

#include <string>
#include <vector>

namespace direct_srtp {

inline constexpr int kSha1_80 = 0x0001;  // webrtc::kSrtpAes128CmSha1_80
inline constexpr int kSha1_32 = 0x0002;  // webrtc::kSrtpAes128CmSha1_32
inline constexpr int kAeadAes128Gcm = 0x0007;
inline constexpr int kAeadAes256Gcm = 0x0008;

struct Policy {
  bool sha1_32 = false;
  bool sha1_80 = true;
  bool gcm = true;
};

// srtp_tag32: -1 auto (on for liquidcall), 0 off, 1 on.
// encryption false is plain RTP: no DTLS-SRTP profiles.
inline Policy PolicyFor(const std::string& audio_codec, int srtp_tag32, bool encryption) {
  Policy policy;
  if (!encryption) {
    policy.sha1_32 = false;
    policy.sha1_80 = false;
    policy.gcm = false;
    return policy;
  }
  const bool tag32 = srtp_tag32 < 0 ? audio_codec == "liquidcall" : srtp_tag32 != 0;
  if (tag32) {
    policy.sha1_32 = true;
    policy.sha1_80 = false;
    policy.gcm = false;
    return policy;
  }
  policy.sha1_32 = false;
  policy.sha1_80 = true;
  policy.gcm = true;
  return policy;
}

// Same order as CryptoOptions::GetSupportedDtlsSrtpCryptoSuites with
// prefer_gcm left false: SHA1_32, then SHA1_80, then GCM.
// BoringSSL's SSL_CTX_set_tlsext_use_srtp names. Not the IANA
// "AES_CM_128_HMAC_SHA1_32" spelling.
inline const char* BoringSslProfileName(int id) {
  switch (id) {
    case kSha1_80:
      return "SRTP_AES128_CM_SHA1_80";
    case kSha1_32:
      return "SRTP_AES128_CM_SHA1_32";
    case kAeadAes128Gcm:
      return "SRTP_AEAD_AES_128_GCM";
    case kAeadAes256Gcm:
      return "SRTP_AEAD_AES_256_GCM";
    default:
      return nullptr;
  }
}

inline std::string BoringSslProfileString(const std::vector<int>& ids) {
  std::string out;
  for (int id : ids) {
    const char* name = BoringSslProfileName(id);
    if (!name) return {};
    if (!out.empty()) out += ":";
    out += name;
  }
  return out;
}

// An empty profile string makes BoringSSL omit use_srtp from the ClientHello
// and the ServerHello. The handshake still completes. DTLS must not start
// that way: the product profile is SHA1_32.
inline std::string ProfilesOrSha1_32(const std::string& configured) {
  if (!configured.empty()) return configured;
  return "SRTP_AES128_CM_SHA1_32";
}

inline std::vector<int> SuiteIds(const Policy& policy) {
  std::vector<int> suites;
  if (policy.sha1_32) suites.push_back(kSha1_32);
  if (policy.sha1_80) suites.push_back(kSha1_80);
  if (policy.gcm) {
    suites.push_back(kAeadAes256Gcm);
    suites.push_back(kAeadAes128Gcm);
  }
  return suites;
}

}  // namespace direct_srtp

#endif
