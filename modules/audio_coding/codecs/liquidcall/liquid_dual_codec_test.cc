// LiquidCall offer: send CETACALL/48000/1 PT 96, and keep Opus/48000/2 PT 111
// so an answer that includes both can be decoded as Opus on the receive side.
// Opus-only (--audio-codec=opus, and the default which sets no preference)
// must not grow a CETACALL payload.

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <opus/opus.h>

#include "examples/direct/srtp_policy.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_codec.h"
#include "modules/audio_coding/codecs/liquidcall/liquid_offer.h"

namespace {

int g_fails = 0;

void Expect(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "ok  " << what << "\n";
  } else {
    std::cout << "FAIL " << what << "\n";
    ++g_fails;
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

bool Contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// Encode a 440 Hz tone and decode it the way the receive path does once the
// answer has selected Opus (PT 111, fmtp minptime=10;useinbandfec=1).
bool DecodeOpusTone(std::string* detail) {
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_VOIP, &err);
  if (!enc || err != OPUS_OK) {
    if (detail) *detail = "opus_encoder_create";
    return false;
  }
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
  OpusDecoder* dec = opus_decoder_create(48000, 2, &err);
  if (!dec || err != OPUS_OK) {
    opus_encoder_destroy(enc);
    if (detail) *detail = "opus_decoder_create";
    return false;
  }
  constexpr int kFrame = 960;  // 20 ms at 48 kHz, above minptime=10
  constexpr int kChannels = 2;
  bool heard = false;
  for (int frame = 0; frame < 8; ++frame) {
    std::vector<opus_int16> pcm(static_cast<size_t>(kFrame * kChannels));
    for (int i = 0; i < kFrame; ++i) {
      const double t = (frame * kFrame + i) / 48000.0;
      const auto sample = static_cast<opus_int16>(8000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * t));
      pcm[static_cast<size_t>(i * kChannels)] = sample;
      pcm[static_cast<size_t>(i * kChannels + 1)] = sample;
    }
    unsigned char packet[4000];
    const int nbytes = opus_encode(enc, pcm.data(), kFrame, packet, sizeof(packet));
    if (nbytes < 1) {
      if (detail) *detail = "opus_encode";
      opus_encoder_destroy(enc);
      opus_decoder_destroy(dec);
      return false;
    }
    std::vector<opus_int16> decoded(static_cast<size_t>(kFrame * kChannels));
    const int samples = opus_decode(dec, packet, nbytes, decoded.data(), kFrame, 0);
    if (samples != kFrame) {
      if (detail) *detail = "opus_decode samples";
      opus_encoder_destroy(enc);
      opus_decoder_destroy(dec);
      return false;
    }
    int peak = 0;
    for (opus_int16 s : decoded) {
      const int a = s < 0 ? -s : s;
      if (a > peak) peak = a;
    }
    if (frame >= 3 && peak > 1000) heard = true;
  }
  opus_encoder_destroy(enc);
  opus_decoder_destroy(dec);
  if (!heard && detail) *detail = "decoded Opus was silent";
  return heard;
}

void TestPreferences() {
  const auto def = webrtc::liquid::AudioCodecPreferenceNames("");
  Expect(def.empty(), "default audio codec sets no preference");
  const auto opus = webrtc::liquid::AudioCodecPreferenceNames("opus");
  Expect(opus.size() == 1 && opus[0] == "opus", "opus preference is opus only");
  Expect(webrtc::liquid::OpusOnlyOfferCodecLines("\n").find("CETACALL") == std::string::npos,
         "opus-only offer has no CETACALL");
  const auto opus_lines = webrtc::liquid::AudioCodecLinesFromSdp(webrtc::liquid::OpusOnlyOfferCodecLines());
  Expect(opus_lines.size() == 1 && opus_lines[0].payload_type == 111 &&
             webrtc::liquid::LowerCodecName(opus_lines[0].name) == "opus" &&
             opus_lines[0].clockrate_hz == 48000 && opus_lines[0].channels == 2 &&
             webrtc::liquid::OpusFmtpAccepted(opus_lines[0].fmtp),
         "opus-only offer is opus/48000/2 PT 111 with minptime=10;useinbandfec=1");

  const auto liquid = webrtc::liquid::AudioCodecPreferenceNames("liquidcall");
  Expect(liquid.size() == 2 && liquid[0] == "cetacall" && liquid[1] == "opus",
         "cetacall preference is CETACALL then Opus");
}

void TestOfferLines() {
  Expect(webrtc::liquid::kPreferredPayloadType == webrtc::liquid::kLiquidCallPayloadType,
         "frozen CETACALL payload type is 96");
  const auto tight = webrtc::liquid::LiquidFmtp("tight", 1, false);
  Expect(tight.count("usedtx") == 0, "DTX stays off the fmtp by default");
  Expect(webrtc::liquid::FormatFmtp(tight) == "fpp=1;pack=tight", "default fmtp is fpp=1;pack=tight");
  const auto dtx = webrtc::liquid::LiquidFmtp("u16", 4, true);
  Expect(webrtc::liquid::FormatFmtp(dtx) == "fpp=4;pack=u16;usedtx=1",
         "u16 fpp=4 dtx fmtp keeps usedtx");

  const std::string offer = webrtc::liquid::LiquidCallOfferCodecLines("tight", 1, false);
  Expect(Contains(offer, "m=audio 9 UDP/TLS/RTP/SAVPF 96 111"), "offer m-line is PT 96 then 111");
  Expect(Contains(offer, "a=rtpmap:96 CETACALL/48000/1"), "offer rtpmap CETACALL/48000/1");
  Expect(Contains(offer, "a=fmtp:96 fpp=1;pack=tight"), "offer liquidcall fmtp");
  Expect(!Contains(offer, "usedtx"), "default offer has no usedtx");
  Expect(Contains(offer, "a=rtpmap:111 opus/48000/2"), "offer rtpmap opus/48000/2");
  Expect(Contains(offer, "a=fmtp:111 minptime=10;useinbandfec=1"), "offer opus fmtp");

  const auto lines = webrtc::liquid::AudioCodecLinesFromSdp(offer);
  Expect(lines.size() == 2, "offer has two audio codecs");
  Expect(lines[0].payload_type == 96 && lines[0].name == "CETACALL" && lines[0].channels == 1,
         "first offered codec is CETACALL PT 96 (the one we send)");
  Expect(lines[1].payload_type == 111 && webrtc::liquid::OpusFmtpAccepted(lines[1].fmtp),
         "second offered codec is Opus PT 111");
}

void TestAnswerDecodesOpus() {
  // The server answers with both payload types and may list Opus first.
  // Our receive side still decodes PT 111. The send side stays PT 96 because
  // that is the first codec in our offer.
  const std::string answer =
      "m=audio 9 UDP/TLS/RTP/SAVPF 111 96\r\n"
      "a=rtpmap:111 opus/48000/2\r\n"
      "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
      "a=rtpmap:96 CETACALL/48000/1\r\n"
      "a=fmtp:96 fpp=1;pack=tight\r\n";
  const auto codecs = webrtc::liquid::AudioCodecLinesFromSdp(answer);
  const auto* opus = webrtc::liquid::FindCodecByPayload(codecs, 111);
  const auto* liquid = webrtc::liquid::FindCodecByPayload(codecs, 96);
  Expect(opus && liquid, "answer contains PT 111 and PT 96");
  Expect(opus && webrtc::liquid::LowerCodecName(opus->name) == "opus" && opus->clockrate_hz == 48000 &&
             opus->channels == 2 && webrtc::liquid::OpusFmtpAccepted(opus->fmtp),
         "answer Opus fmtp minptime=10;useinbandfec=1 is accepted");
  Expect(liquid && liquid->name == "CETACALL" && liquid->channels == 1, "answer still has CETACALL");
  // A reordered fmtp, which some stacks write, is the same contract.
  Expect(webrtc::liquid::OpusFmtpAccepted("useinbandfec=1; minptime=10"),
         "opus fmtp parameter order does not matter");

  const auto offered = webrtc::liquid::AudioCodecLinesFromSdp(
      webrtc::liquid::LiquidCallOfferCodecLines("tight", 1, false));
  Expect(!offered.empty() && offered[0].payload_type == 96, "send payload stays 96");

  std::string detail;
  Expect(DecodeOpusTone(&detail), detail.empty() ? "receive side decodes Opus PT 111" : detail);

  // The wrapped factory lists CETACALL and then the builtin codecs, Opus included.
  const std::vector<std::string> builtin = {"opus", "G722", "PCMU", "cetacall"};
  const auto wrapped = webrtc::liquid::PrependLiquidCall(
      builtin, std::string("CETACALL"), [](const std::string& name) { return name; });
  Expect(!wrapped.empty() && wrapped[0] == "CETACALL", "factory lists CETACALL first");
  bool saw_opus = false;
  bool saw_legacy = false;
  for (const auto& name : wrapped) {
    if (name == "opus") saw_opus = true;
    if (name == "cetacall") saw_legacy = true;
  }
  Expect(saw_opus, "factory decoder list keeps Opus");
  Expect(!saw_legacy, "factory lists CETACALL once");
}

void TestLiquidCallRoomSrtp() {
  // Caller and callee, direct TCP and the room WebSocket, all use
  // CreatePeerConnection, which installs PolicyFor(). DtlsTransportInternalImpl
  // copies SuiteIds() into srtp_ciphers_ and will not call
  // SSL_CTX_set_tlsext_use_srtp if that vector is empty.
  const direct_srtp::Policy room =
      direct_srtp::PolicyFor("liquidcall", /*srtp_tag32=*/-1, /*encryption=*/true);
  const std::vector<int> suites = direct_srtp::SuiteIds(room);
  Expect(!suites.empty(), "liquidcall room SRTP list is non-empty");
  Expect(suites.front() == direct_srtp::kSha1_32, "liquidcall offers SHA1_32 first");
  bool has_32 = false;
  bool has_80 = false;
  bool has_gcm = false;
  for (int id : suites) {
    if (id == direct_srtp::kSha1_32) has_32 = true;
    if (id == direct_srtp::kSha1_80) has_80 = true;
    if (id == direct_srtp::kAeadAes128Gcm || id == direct_srtp::kAeadAes256Gcm) has_gcm = true;
  }
  Expect(has_32, "liquidcall SRTP list contains SHA1_32");
  Expect(!has_80 && !has_gcm, "liquidcall does not offer SHA1_80 or GCM ahead of SHA1_32");
  const auto tcp = direct_srtp::PolicyFor("liquidcall", -1, true);
  Expect(direct_srtp::SuiteIds(tcp) == suites, "direct TCP uses the same SRTP list as the room path");
  const auto opus = direct_srtp::SuiteIds(direct_srtp::PolicyFor("opus", -1, true));
  Expect(!opus.empty() && opus.front() == direct_srtp::kSha1_80, "opus keeps SHA1_80");
  const auto plain = direct_srtp::SuiteIds(direct_srtp::PolicyFor("liquidcall", -1, false));
  Expect(plain.empty(), "encryption off installs no SRTP profiles");
  const std::string profile = direct_srtp::BoringSslProfileString(suites);
  Expect(profile == "SRTP_AES128_CM_SHA1_32",
         "BoringSSL profile string is SRTP_AES128_CM_SHA1_32");
  Expect(profile.find("HMAC") == std::string::npos,
         "profile string does not use the IANA HMAC spelling");
  Expect(direct_srtp::ProfilesOrSha1_32("") == "SRTP_AES128_CM_SHA1_32",
         "empty SRTP list falls back to SHA1_32");
  Expect(direct_srtp::ProfilesOrSha1_32(profile) == profile,
         "a configured profile string is kept");
}

void TestWiring(const std::string& root) {
  if (root.empty()) {
    std::cout << "ok  skipped source wiring check (no LIQUID_REPO_ROOT)\n";
    return;
  }
  const std::string direct = ReadFile(root + "/examples/direct/direct.cc");
  const std::string option = ReadFile(root + "/examples/direct/option.h");
  const std::string factory = ReadFile(root + "/modules/audio_coding/codecs/liquidcall/liquid_factory.cc");
  Expect(Contains(direct, "CreateLiquidAudioEncoderFactory(audio_encoder_factory)"),
         "liquidcall mode wraps the builtin encoder factory");
  Expect(Contains(direct, "CreateLiquidAudioDecoderFactory(audio_decoder_factory)"),
         "liquidcall mode wraps the builtin decoder factory");
  Expect(Contains(direct, "AudioCodecPreferenceNames"),
         "codec preferences come from AudioCodecPreferenceNames");
  Expect(Contains(direct, "direct_srtp::PolicyFor"),
         "peer connection installs the shared SRTP policy");
  Expect(Contains(direct, "SSL_PROTOCOL_DTLS_12"),
         "peer connection caps DTLS at 1.2");
  const std::string dtls = ReadFile(root + "/p2p/dtls/dtls_transport.cc");
  const std::string ssl = ReadFile(root + "/rtc_base/openssl_stream_adapter.cc");
  Expect(Contains(dtls, "installing SRTP_AES128_CM_SHA1_32") ||
             Contains(dtls, "profiles=SRTP_AES128_CM_SHA1_32 fallback=1"),
         "empty DTLS SRTP list installs SHA1_32");
  Expect(Contains(ssl, "SetupSSLContext role=%s"),
         "SSL_CTX setup logs the role");
  Expect(Contains(ssl, "BeginSSL role=%s"),
         "SSL object setup logs the role");
  Expect(Contains(ssl, "SRTP_AES128_CM_SHA1_32"),
         "SSL setup has the SHA1_32 fallback profile");
  Expect(Contains(option, "bool liquid_dtx = false"), "DTX default stays off");
  Expect(Contains(option, "kLiquidJitterMaxMs = 80"), "80 ms jitter cap unchanged");
  Expect(Contains(factory, "PrependLiquidCall"), "factories prepend CETACALL and keep the inner list");
  const std::string tap = ReadFile(root + "/examples/direct/audio_tap.h");
  if (Contains(tap, "kVoiceSdpAttr")) {
    Expect(Contains(tap, "a=x-liquidcall-voice:"), "voice attribute text is unchanged");
    Expect(Contains(tap, "a=x-liquidcall-voices:"), "voices attribute text is unchanged");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* root_env = std::getenv("LIQUID_REPO_ROOT");
  const std::string root = root_env ? root_env : (argc > 1 ? argv[1] : "");
  TestPreferences();
  TestOfferLines();
  TestAnswerDecodesOpus();
  TestLiquidCallRoomSrtp();
  TestWiring(root);
  if (g_fails) {
    std::cout << g_fails << " failure(s)\n";
    return 1;
  }
  std::cout << "liquidcall dual-codec tests passed\n";
  return 0;
}
