#ifndef MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_LIQUID_OFFER_H_
#define MODULES_AUDIO_CODING_CODECS_LIQUIDCALL_LIQUID_OFFER_H_

// Offer shape for --audio-codec=liquidcall. The frozen wire contract is
// CETACALL/48000/1 payload type 96, Opus/48000/2 payload type 111, and
// fmtp pack=tight|u16, fpp=1..4, usedtx=1 only when DTX is on.
// Opus stays in the offer so this client can decode Opus sent back by the server.
// Parameter order matches WebRTC's std::map fmtp writer (alphabetical).

#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace webrtc {
namespace liquid {

inline constexpr int kLiquidCallPayloadType = 96;
inline constexpr int kOpusPayloadType = 111;

inline std::string LowerCodecName(std::string name) {
  for (char& c : name) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return name;
}

inline bool IsLiquidCallName(const std::string& name) {
  const std::string lower = LowerCodecName(name);
  return lower == "cetacall";  // SDP name only; no older names (Nov 25)
}

// "" (default) is empty: do not call SetCodecPreferences, so the builtin
// offer is unchanged. "opus" is Opus only. "cetacall" (or the older option
// spelling "liquidcall") is CETACALL then Opus. Names are matched to SDP codec names.
inline std::vector<std::string> AudioCodecPreferenceNames(const std::string& audio_codec) {
  if (audio_codec == "cetacall" || audio_codec == "liquidcall") return {"cetacall", "opus"};
  if (audio_codec == "opus") return {"opus"};
  return {};
}

// fmtp for CETACALL. usedtx is omitted unless DTX is on (the default is off).
inline std::map<std::string, std::string> LiquidFmtp(const std::string& pack, int fpp, bool dtx) {
  std::map<std::string, std::string> params;
  params["pack"] = pack == "u16" ? "u16" : "tight";
  params["fpp"] = std::to_string(fpp < 1 ? 1 : fpp);
  if (dtx) params["usedtx"] = "1";
  return params;
}

inline std::string FormatFmtp(const std::map<std::string, std::string>& params) {
  std::string out;
  for (const auto& entry : params) {
    if (!out.empty()) out += ";";
    out += entry.first + "=" + entry.second;
  }
  return out;
}

// Codec lines of a liquidcall offer. WebRTC also writes session lines
// (c=, a=rtcp-mux, a=mid, DTLS, ICE) and a=rtcp-fb:111 transport-cc because
// the builtin Opus spec sets supports_network_adaption. Those are unchanged.
inline std::string LiquidCallOfferCodecLines(const std::string& pack, int fpp, bool dtx,
                                             const std::string& nl = "\r\n") {
  const std::string fmtp = FormatFmtp(LiquidFmtp(pack, fpp, dtx));
  std::string s;
  s += "m=audio 9 UDP/TLS/RTP/SAVPF 96 111" + nl;
  s += "a=rtpmap:96 CETACALL/48000/1" + nl;
  s += "a=fmtp:96 " + fmtp + nl;
  s += "a=rtpmap:111 opus/48000/2" + nl;
  s += "a=fmtp:111 minptime=10;useinbandfec=1" + nl;
  return s;
}

// What --audio-codec=opus (and --opus-bitrate) already offers: Opus only.
inline std::string OpusOnlyOfferCodecLines(const std::string& nl = "\r\n") {
  std::string s;
  s += "m=audio 9 UDP/TLS/RTP/SAVPF 111" + nl;
  s += "a=rtpmap:111 opus/48000/2" + nl;
  s += "a=fmtp:111 minptime=10;useinbandfec=1" + nl;
  return s;
}

// CETACALL first, then every inner codec that is not LiquidCall.
// An inner Opus entry stays, so the decoder factory can still create it.
template <typename T, typename NameOf>
std::vector<T> PrependLiquidCall(std::vector<T> inner, T liquid, NameOf name_of) {
  std::vector<T> out;
  out.reserve(inner.size() + 1);
  out.push_back(std::move(liquid));
  for (auto& item : inner) {
    if (!IsLiquidCallName(name_of(item))) out.push_back(std::move(item));
  }
  return out;
}

struct SdpAudioCodecLine {
  int payload_type = -1;
  std::string name;
  int clockrate_hz = 0;
  int channels = 0;
  std::string fmtp;
};

inline std::string TrimCopy(std::string s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// Audio codecs in one SDP (offer or answer). Payload types come from the
// audio m-line; rtpmap and fmtp fill in the rest. Unknown attributes are ignored.
inline std::vector<SdpAudioCodecLine> AudioCodecLinesFromSdp(const std::string& sdp) {
  std::vector<SdpAudioCodecLine> codecs;
  const size_t mpos = sdp.find("m=audio ");
  if (mpos == std::string::npos) return codecs;
  const size_t eol = sdp.find_first_of("\r\n", mpos);
  const std::string mline = sdp.substr(mpos, eol == std::string::npos ? std::string::npos : eol - mpos);
  // m=audio <port> <proto> <pt>...
  std::vector<std::string> fields;
  std::string cur;
  for (size_t i = 0; i <= mline.size(); ++i) {
    if (i == mline.size() || mline[i] == ' ') {
      if (!cur.empty()) fields.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(mline[i]);
    }
  }
  for (size_t i = 3; i < fields.size(); ++i) {
    SdpAudioCodecLine line;
    line.payload_type = std::atoi(fields[i].c_str());
    codecs.push_back(line);
  }
  for (size_t i = 0; i < sdp.size();) {
    const size_t end = sdp.find_first_of("\r\n", i);
    const std::string line = sdp.substr(i, end == std::string::npos ? std::string::npos : end - i);
    i = end == std::string::npos ? sdp.size() : end + 1;
    if (sdp.compare(i, 1, "\n") == 0 && end != std::string::npos && sdp[end] == '\r') ++i;
    if (line.compare(0, 9, "a=rtpmap:") == 0) {
      const size_t space = line.find(' ', 9);
      if (space == std::string::npos) continue;
      const int pt = std::atoi(line.substr(9, space - 9).c_str());
      const std::string rest = line.substr(space + 1);
      const size_t slash1 = rest.find('/');
      const size_t slash2 = slash1 == std::string::npos ? std::string::npos : rest.find('/', slash1 + 1);
      for (auto& codec : codecs) {
        if (codec.payload_type != pt) continue;
        codec.name = slash1 == std::string::npos ? rest : rest.substr(0, slash1);
        if (slash1 != std::string::npos) {
          const size_t clock_end = slash2 == std::string::npos ? rest.size() : slash2;
          codec.clockrate_hz = std::atoi(rest.substr(slash1 + 1, clock_end - slash1 - 1).c_str());
        }
        if (slash2 != std::string::npos) codec.channels = std::atoi(rest.substr(slash2 + 1).c_str());
      }
    } else if (line.compare(0, 7, "a=fmtp:") == 0) {
      const size_t space = line.find(' ', 7);
      if (space == std::string::npos) continue;
      const int pt = std::atoi(line.substr(7, space - 7).c_str());
      for (auto& codec : codecs) {
        if (codec.payload_type == pt) codec.fmtp = TrimCopy(line.substr(space + 1));
      }
    }
  }
  return codecs;
}

// Server Opus fmtp. Extra parameters are fine. Both keys must be present.
inline bool OpusFmtpAccepted(const std::string& fmtp) {
  bool minptime = false;
  bool fec = false;
  std::string cur;
  for (size_t i = 0; i <= fmtp.size(); ++i) {
    if (i == fmtp.size() || fmtp[i] == ';') {
      const std::string param = TrimCopy(cur);
      cur.clear();
      const size_t eq = param.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = TrimCopy(param.substr(0, eq));
      const std::string value = TrimCopy(param.substr(eq + 1));
      if (key == "minptime" && value == "10") minptime = true;
      if (key == "useinbandfec" && value == "1") fec = true;
    } else {
      cur.push_back(fmtp[i]);
    }
  }
  return minptime && fec;
}

inline const SdpAudioCodecLine* FindCodecByPayload(const std::vector<SdpAudioCodecLine>& codecs, int pt) {
  for (const auto& codec : codecs) {
    if (codec.payload_type == pt) return &codec;
  }
  return nullptr;
}

}  // namespace liquid
}  // namespace webrtc

#endif
