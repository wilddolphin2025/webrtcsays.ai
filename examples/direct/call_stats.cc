/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#include "call_stats.h"
#include "echo_stats.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "api/units/time_delta.h"
#include "rtc_base/event.h"

namespace {

int64_t ElapsedMs(std::chrono::steady_clock::time_point origin,
                  std::chrono::steady_clock::time_point when) {
  if (origin.time_since_epoch().count() == 0 || when.time_since_epoch().count() == 0) {
    return -1;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(when - origin).count();
}

uint64_t RtpHeaderForOnWire(uint64_t reported, uint64_t packets) {
  if (packets == 0) return reported;
  const double per = static_cast<double>(reported) / static_cast<double>(packets);
  // 12-byte fixed header plus any header extensions (one-byte form: 4-byte
  // block header + elements, padded to 4). Earlier this capped at 20 bytes and
  // hid extension overhead.
  if (per >= 12.0 && per <= 80.0) return reported;
  return packets * 12;
}

// payload + RTP header + packets * (20 IPv4 + 8 UDP + SRTP tag [+ 20 IPv6]).
// SRTP tag: 10 bytes (AES_CM_128_HMAC_SHA1_80), 4 (_32), 16 (AEAD GCM), 0 plain.
double OnWireKbps(uint64_t payload, uint64_t header, uint64_t packets, double seconds,
                  int srtp_tag, bool ipv6) {
  if (seconds <= 0) return 0;
  uint64_t extra = 28 + static_cast<uint64_t>(std::max(0, srtp_tag));
  if (ipv6) extra += 20;
  const uint64_t total = payload + RtpHeaderForOnWire(header, packets) + packets * extra;
  return static_cast<double>(total) * 8.0 / seconds / 1000.0;
}

std::string ProfileLevelId(const std::string& fmtp) {
  const std::string key = "profile-level-id=";
  auto pos = fmtp.find(key);
  if (pos == std::string::npos) return "";
  pos += key.size();
  auto end = fmtp.find_first_of("; \t\r\n", pos);
  return fmtp.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

class Json {
 public:
  void begin() {
    out_ = "{";
    first_ = true;
  }
  void key(const char* name) {
    if (!first_) out_ += ',';
    first_ = false;
    out_ += '"';
    out_ += name;
    out_ += "\":";
  }
  void nul(const char* name) {
    key(name);
    out_ += "null";
  }
  void integer(const char* name, long long value) {
    key(name);
    out_ += std::to_string(value);
  }
  void number(const char* name, double value, int prec) {
    key(name);
    if (!std::isfinite(value)) {
      out_ += "null";
      return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, value);
    out_ += buf;
  }
  void str(const char* name, const std::string& value) {
    key(name);
    out_ += '"';
    for (unsigned char c : value) {
      if (c == '\\' || c == '"') out_ += '\\';
      if (c == '\n') {
        out_ += "\\n";
        continue;
      }
      out_ += static_cast<char>(c);
    }
    out_ += '"';
  }
  void raw(const char* name, const std::string& json_value) {
    key(name);
    out_ += json_value;
  }
  std::string end() {
    out_ += '}';
    return out_;
  }

 private:
  std::string out_;
  bool first_ = true;
};

struct Counter {
  uint64_t bytes = 0;
  double media_time_s = 0;
  uint64_t frames = 0;
  bool seen = false;
};

constexpr double kNoLevel = -999.0;

// totalAudioEnergy / totalSamplesDuration deltas give the mean power of the
// interval; reported in dB relative to full scale.
struct EnergyCounter {
  double energy = 0;
  double duration = 0;
  bool seen = false;
};

double LevelDbfs(EnergyCounter& prev, double energy, double duration) {
  double dbfs = kNoLevel;
  if (prev.seen && duration > prev.duration + 1e-6 && energy >= prev.energy) {
    const double power = (energy - prev.energy) / (duration - prev.duration);
    dbfs = power > 1e-12 ? 10.0 * std::log10(power) : -120.0;
  }
  prev.energy = energy;
  prev.duration = duration;
  prev.seen = true;
  return dbfs;
}

struct Snapshot {
  std::string json;
  std::string ice;
  std::string video_codec;
  std::string profile_level_id;
  std::string quality_limitation;
  int send_w = 0;
  int send_h = 0;
  int recv_w = 0;
  int recv_h = 0;
  double send_fps = 0;
  double recv_fps = 0;
  double send_bitrate = 0;
  double recv_bitrate = 0;
  double encode_ms = 0;
  double decode_ms = 0;
  std::string audio_line;
  std::string echo_line;
  long long frames_dropped = 0;
  long long nack = 0;
  long long pli = 0;
  double rtt_ms = -1;
};

class StatsCollector : public webrtc::RTCStatsCollectorCallback {
 public:
  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    Snapshot snap = Build(report);
    std::lock_guard<std::recursive_mutex> lock(mu_);
    last_ = std::move(snap);
    has_last_ = true;
    samples_.fetch_add(1);
  }

  void set_encrypted(bool encrypted) { encrypted_ = encrypted; }

  int samples() const { return samples_.load(); }

  Snapshot Last() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return last_;
  }

  bool has_last() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return has_last_;
  }

 private:
  template <typename T>
  static const T* CodecFor(const webrtc::RTCStatsReport& report,
                           const std::optional<std::string>& codec_id) {
    if (!codec_id) return nullptr;
    return report.GetAs<T>(*codec_id);
  }

  static double Bitrate(Counter& prev, uint64_t bytes, double dt) {
    double bps = 0;
    if (prev.seen && dt > 0 && bytes >= prev.bytes) {
      bps = static_cast<double>(bytes - prev.bytes) * 8.0 / dt;
    }
    prev.bytes = bytes;
    prev.seen = true;
    return bps;
  }

  // bytes/header/packets from RTCStats are totals since the call began; the
  // rate over the last interval needs their deltas. Dividing the totals by dt
  // grew the "on-wire" figure with call length (hundreds of kbps for a
  // ~7 kbps LiquidCall stream).
  struct WireCounter {
    uint64_t bytes = 0;
    uint64_t header = 0;
    uint64_t packets = 0;
    bool seen = false;
  };
  static double WireDeltaKbps(WireCounter& prev, uint64_t bytes, uint64_t header,
                              uint64_t packets, double dt, int srtp_tag, bool ipv6) {
    double kbps = 0;
    if (prev.seen && dt > 0 && bytes >= prev.bytes && header >= prev.header &&
        packets >= prev.packets) {
      kbps = OnWireKbps(bytes - prev.bytes, header - prev.header, packets - prev.packets, dt,
                        srtp_tag, ipv6);
    }
    prev.bytes = bytes;
    prev.header = header;
    prev.packets = packets;
    prev.seen = true;
    return kbps;
  }

  static double PerFrameMs(Counter& prev, double total_s, uint64_t frames, bool* have) {
    *have = false;
    double ms = 0;
    if (prev.seen && frames > prev.frames && total_s >= prev.media_time_s) {
      ms = (total_s - prev.media_time_s) / static_cast<double>(frames - prev.frames) * 1000.0;
      *have = true;
    } else if (frames > 0 && total_s > 0) {
      ms = total_s / static_cast<double>(frames) * 1000.0;
      *have = true;
    }
    prev.media_time_s = total_s;
    prev.frames = frames;
    prev.seen = true;
    return ms;
  }

  Snapshot Build(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
    const auto now = std::chrono::steady_clock::now();
    double dt = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      if (last_sample_.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<double>(now - last_sample_).count();
      }
      last_sample_ = now;
    }

    Snapshot snap;
    std::string ice = "none";
    double rtt_ms = -1;
    const webrtc::RTCIceCandidatePairStats* pair = nullptr;
    for (const auto* transport : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
      if (!transport->selected_candidate_pair_id) continue;
      pair = report->GetAs<webrtc::RTCIceCandidatePairStats>(*transport->selected_candidate_pair_id);
      if (pair) break;
    }
    if (!pair) {
      for (const auto* candidate : report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
        if (candidate->nominated.value_or(false) && candidate->state.value_or("") == "succeeded") {
          pair = candidate;
          break;
        }
      }
    }
    // Real SRTP auth tag from the negotiated cipher.
    int srtp_tag = encrypted_ ? 10 : 0;
    std::string srtp_cipher = encrypted_ ? "unknown" : "none";
    for (const auto* transport : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
      if (!transport->srtp_cipher) continue;
      srtp_cipher = *transport->srtp_cipher;
      if (srtp_cipher.find("_32") != std::string::npos) srtp_tag = 4;
      else if (srtp_cipher.find("GCM") != std::string::npos) srtp_tag = 16;
      else if (srtp_cipher.find("_80") != std::string::npos) srtp_tag = 10;
      break;
    }
    bool ipv6 = false;
    if (pair) {
      if (pair->current_round_trip_time) rtt_ms = *pair->current_round_trip_time * 1000.0;
      if (pair->local_candidate_id) {
        if (const auto* local =
                report->GetAs<webrtc::RTCLocalIceCandidateStats>(*pair->local_candidate_id)) {
          if (local->candidate_type) ice = *local->candidate_type;
          const std::string addr = local->address.value_or(local->ip.value_or(""));
          ipv6 = addr.find(':') != std::string::npos;
        }
      }
    }

    Json audio_out;
    audio_out.begin();
    Json video_out;
    video_out.begin();
    bool have_audio = false;
    bool have_video = false;
    long long nack = 0;
    long long pli = 0;
    long long concealment = 0;
    double jitter_buffer_ms = -1;      // this interval
    double jitter_buffer_avg_ms = -1;  // whole call
    double jitter_target_ms = -1;      // NetEq target, this interval
    double send_onwire_kbps = -1;
    double recv_onwire_kbps = -1;
    double send_payload_kbps = -1;
    double recv_payload_kbps = -1;
    long long send_packets = 0;
    long long recv_packets = 0;
    long long packets_lost = 0;
    double jitter_ms = -1;
    int algo_ms = -1;
    double m2e_ms = -1;
    double recv_dbfs = kNoLevel;

    auto note_first_audio = [&](uint64_t packets) {
      if (packets == 0) return;
      std::lock_guard<std::recursive_mutex> lock(mu_);
      if (first_audio_.time_since_epoch().count() == 0) first_audio_ = now;
    };

    for (const auto* in : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
      const std::string kind = in->kind.value_or("");
      if (kind != "audio" && kind != "video") continue;
      Counter& bytes_prev = (kind == "audio") ? audio_in_bytes_ : video_in_bytes_;
      const double bps = Bitrate(bytes_prev, in->bytes_received.value_or(0), dt);
      nack += in->nack_count.value_or(0);
      pli += in->pli_count.value_or(0);
      if (kind == "audio") {
        have_audio = true;
        note_first_audio(in->packets_received.value_or(0));
        concealment += static_cast<long long>(in->concealment_events.value_or(0));
        if (in->total_audio_energy && in->total_samples_duration) {
          recv_dbfs = LevelDbfs(recv_energy_, *in->total_audio_energy,
                                *in->total_samples_duration);
        }
        // jitterBufferDelay and jitterBufferTargetDelay are sums since the
        // call began; the buffer "now" is their delta over this interval.
        // The cumulative average lagged real changes by tens of seconds.
        if (in->jitter_buffer_delay && in->jitter_buffer_emitted_count &&
            *in->jitter_buffer_emitted_count > 0) {
          const uint64_t emitted = *in->jitter_buffer_emitted_count;
          const double delay = *in->jitter_buffer_delay;
          const double target = in->jitter_buffer_target_delay.value_or(0);
          jitter_buffer_avg_ms = delay / static_cast<double>(emitted) * 1000.0;
          jitter_buffer_ms = jitter_buffer_avg_ms;
          if (in->jitter_buffer_target_delay) {
            jitter_target_ms = target / static_cast<double>(emitted) * 1000.0;
          }
          if (jb_prev_.seen && emitted > jb_prev_.emitted && delay >= jb_prev_.delay) {
            const double n = static_cast<double>(emitted - jb_prev_.emitted);
            jitter_buffer_ms = (delay - jb_prev_.delay) / n * 1000.0;
            if (in->jitter_buffer_target_delay && target >= jb_prev_.target) {
              jitter_target_ms = (target - jb_prev_.target) / n * 1000.0;
            }
          }
          jb_prev_.delay = delay;
          jb_prev_.target = target;
          jb_prev_.emitted = emitted;
          jb_prev_.seen = true;
        }
        const auto* codec = CodecFor<webrtc::RTCCodecStats>(*report, in->codec_id);
        const uint64_t recv_bytes = in->bytes_received.value_or(0);
        const uint64_t recv_header = in->header_bytes_received.value_or(0);
        const double recv_onwire_kbps_now =
            WireDeltaKbps(audio_in_wire_, recv_bytes, recv_header,
                          in->packets_received.value_or(0), dt, srtp_tag, ipv6);
        std::string mime = codec && codec->mime_type ? *codec->mime_type : audio_codec_;
        for (char& ch : mime) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const int codec_algo_ms = mime.find("liquid") != std::string::npos ? 120 : 20;
        const double half_rtt = rtt_ms >= 0 ? rtt_ms / 2.0 : 0;
        const double jb = jitter_buffer_ms >= 0 ? jitter_buffer_ms : 0;
        const double m2e = 10.0 + codec_algo_ms + half_rtt + jb + 10.0;
        audio_out.str("codec", mime);
        audio_out.str("srtp_cipher", srtp_cipher);
        audio_out.integer("srtp_tag_bytes", srtp_tag);
        if (const uint64_t rp = in->packets_received.value_or(0)) {
          audio_out.number("recv_rtp_header_bytes_per_packet",
                           static_cast<double>(recv_header) / static_cast<double>(rp), 1);
        }
        audio_out.number("recv_bitrate_bps", bps, 0);
        audio_out.number("recv_payload_kbps", bps / 1000.0, 3);
        audio_out.number("recv_onwire_ipv4_udp_rtp_kbps", recv_onwire_kbps_now, 3);
        audio_out.integer("algorithmic_latency_ms", codec_algo_ms);
        audio_out.number("mouth_to_ear_est_ms", m2e, 1);
        if (jitter_buffer_ms >= 0) audio_out.number("jitter_buffer_ms", jitter_buffer_ms, 1);
        if (jitter_buffer_avg_ms >= 0) audio_out.number("jitter_buffer_avg_ms", jitter_buffer_avg_ms, 1);
        if (jitter_target_ms >= 0) audio_out.number("jitter_buffer_target_ms", jitter_target_ms, 1);
        recv_onwire_kbps = recv_onwire_kbps_now;
        recv_payload_kbps = bps / 1000.0;
        recv_packets = static_cast<long long>(in->packets_received.value_or(0));
        packets_lost = in->packets_lost.value_or(0);
        jitter_ms = in->jitter ? *in->jitter * 1000.0 : -1;
        algo_ms = codec_algo_ms;
        m2e_ms = m2e;
        audio_out.integer("recv_bytes", static_cast<long long>(in->bytes_received.value_or(0)));
        audio_out.integer("recv_packets", static_cast<long long>(in->packets_received.value_or(0)));
        audio_out.integer("recv_header_bytes",
                          static_cast<long long>(in->header_bytes_received.value_or(0)));
        audio_out.number("recv_jitter_ms", in->jitter ? *in->jitter * 1000.0 : -1, 2);
        audio_out.integer("recv_packets_lost", in->packets_lost.value_or(0));
        audio_out.integer("recv_packets", in->packets_received.value_or(0));
        audio_out.integer("concealed_samples", static_cast<long long>(in->concealed_samples.value_or(0)));
        audio_out.integer("concealment_events", static_cast<long long>(in->concealment_events.value_or(0)));
        audio_out.integer("nack", in->nack_count.value_or(0));
      } else {
        have_video = true;
        snap.recv_w = static_cast<int>(in->frame_width.value_or(0));
        snap.recv_h = static_cast<int>(in->frame_height.value_or(0));
        snap.recv_fps = in->frames_per_second.value_or(0);
        snap.recv_bitrate = bps;
        snap.frames_dropped = in->frames_dropped.value_or(0);
        bool have_dec = false;
        snap.decode_ms = PerFrameMs(video_dec_, in->total_decode_time.value_or(0),
                                    in->frames_decoded.value_or(0), &have_dec);
        const auto* codec = CodecFor<webrtc::RTCCodecStats>(*report, in->codec_id);
        if (codec && codec->mime_type) snap.video_codec = *codec->mime_type;
        if (codec && codec->sdp_fmtp_line) snap.profile_level_id = ProfileLevelId(*codec->sdp_fmtp_line);
        video_out.str("codec", snap.video_codec);
        video_out.str("profile_level_id", snap.profile_level_id);
        video_out.integer("recv_width", snap.recv_w);
        video_out.integer("recv_height", snap.recv_h);
        video_out.number("recv_fps", snap.recv_fps, 2);
        video_out.number("recv_bitrate_bps", bps, 0);
        if (have_dec) video_out.number("decode_ms", snap.decode_ms, 2);
        else video_out.nul("decode_ms");
        video_out.integer("frames_decoded", in->frames_decoded.value_or(0));
        video_out.integer("frames_dropped", snap.frames_dropped);
        video_out.number("recv_jitter_ms", in->jitter ? *in->jitter * 1000.0 : -1, 2);
        video_out.integer("recv_packets_lost", in->packets_lost.value_or(0));
        video_out.integer("nack", in->nack_count.value_or(0));
        video_out.integer("pli", in->pli_count.value_or(0));
        if (in->jitter_buffer_delay && in->jitter_buffer_emitted_count &&
            *in->jitter_buffer_emitted_count > 0) {
          video_out.number("jitter_buffer_ms",
                           (*in->jitter_buffer_delay / static_cast<double>(*in->jitter_buffer_emitted_count)) * 1000.0,
                           2);
        } else {
          video_out.nul("jitter_buffer_ms");
        }
      }
    }

    for (const auto* outb : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
      const std::string kind = outb->kind.value_or("");
      if (kind != "audio" && kind != "video") continue;
      Counter& bytes_prev = (kind == "audio") ? audio_out_bytes_ : video_out_bytes_;
      const double bps = Bitrate(bytes_prev, outb->bytes_sent.value_or(0), dt);
      nack += outb->nack_count.value_or(0);
      pli += outb->pli_count.value_or(0);
      const webrtc::RTCRemoteInboundRtpStreamStats* remote = nullptr;
      for (const auto* candidate : report->GetStatsOfType<webrtc::RTCRemoteInboundRtpStreamStats>()) {
        if (candidate->local_id && *candidate->local_id == outb->id()) {
          remote = candidate;
          break;
        }
        if (!remote && candidate->kind && *candidate->kind == kind) remote = candidate;
      }
      if (kind == "audio") {
        have_audio = true;
        if (!audio_out_has_codec_) {
          const auto* codec = CodecFor<webrtc::RTCCodecStats>(*report, outb->codec_id);
          if (codec && codec->mime_type && !codec->mime_type->empty()) {
            audio_codec_ = *codec->mime_type;
          }
        }
        const uint64_t send_bytes = outb->bytes_sent.value_or(0);
        const uint64_t send_header = outb->header_bytes_sent.value_or(0);
        const double send_payload_bps = bps;
        const uint64_t send_pk = outb->packets_sent.value_or(0);
        if (audio_out_wire_.seen && dt > 0 && send_pk >= audio_out_wire_.packets) {
          const uint64_t dp = send_pk - audio_out_wire_.packets;
          audio_out.number("send_packets_per_s", static_cast<double>(dp) / dt, 2);
          if (dp > 0) {
            const double pay = static_cast<double>(send_bytes - audio_out_wire_.bytes) / dp;
            const double hdr = static_cast<double>(send_header - audio_out_wire_.header) / dp;
            audio_out.number("send_payload_bytes_per_packet", pay, 1);
            audio_out.number("send_rtp_header_bytes_per_packet", hdr, 1);
            // LiquidCall payload sizes identify pack and frames per packet.
            int fpp = 0;
            const int ipay = static_cast<int>(pay + 0.5);
            const int tight[] = {17, 33, 50, 66};
            const int u16[] = {28, 52, 76, 100};
            for (int i = 0; i < 4; ++i) {
              if (ipay == tight[i] || ipay == u16[i]) fpp = i + 1;
            }
            if (fpp > 0 && !onwire_printed_ && srtp_cipher != "unknown" && hdr < 16.0) {
              // Negotiated transport, once per call. The encoder's startup
              // line cannot know the SRTP cipher; this one uses it.
              onwire_printed_ = true;
              const int rtp = static_cast<int>(hdr + 0.5);
              const int ip = ipv6 ? 48 : 28;
              const int total = ipay + rtp + ip + srtp_tag;
              std::fprintf(stderr,
                           "On-wire (negotiated): payload %d B + RTP %d B + %s %d B + SRTP tag %d B "
                           "(%s) = %d B x %.1f pkt/s = %.0f bps\n",
                           ipay, rtp, ipv6 ? "IPv6/UDP" : "IPv4/UDP", ip, srtp_tag,
                           srtp_cipher.c_str(), total, 12.5 / fpp, total * 8.0 * 12.5 / fpp);
            }
            if (fpp > 0) {
              audio_out.integer("send_frames_per_packet", fpp);
              audio_out.integer("send_ptime_ms", fpp * 80);
              const double expected = 12.5 / fpp;
              const double active = std::min(1.0, static_cast<double>(dp) / dt / expected);
              audio_out.number("send_dtx_pct", (1.0 - active) * 100.0, 0);
            }
          } else {
            audio_out.number("send_dtx_pct", 100.0, 0);
          }
        }
        const double send_onwire_kbps_now =
            WireDeltaKbps(audio_out_wire_, send_bytes, send_header, send_pk, dt, srtp_tag, ipv6);
        audio_out.number("send_bitrate_bps", send_payload_bps, 0);
        audio_out.number("send_payload_kbps", send_payload_bps / 1000.0, 3);
        audio_out.number("send_onwire_ipv4_udp_rtp_kbps", send_onwire_kbps_now, 3);
        send_onwire_kbps = send_onwire_kbps_now;
        send_payload_kbps = send_payload_bps / 1000.0;
        send_packets = static_cast<long long>(outb->packets_sent.value_or(0));
        audio_out.integer("send_bytes", static_cast<long long>(send_bytes));
        audio_out.integer("send_packets", static_cast<long long>(outb->packets_sent.value_or(0)));
        audio_out.integer("send_header_bytes",
                          static_cast<long long>(outb->header_bytes_sent.value_or(0)));
        audio_out.integer("send_packets_lost", remote && remote->packets_lost ? *remote->packets_lost : 0);
        audio_out.number("send_jitter_ms", remote && remote->jitter ? *remote->jitter * 1000.0 : -1, 2);
        audio_out.integer("nack", outb->nack_count.value_or(0));
        audio_out_has_codec_ = true;
      } else {
        have_video = true;
        snap.send_w = static_cast<int>(outb->frame_width.value_or(0));
        snap.send_h = static_cast<int>(outb->frame_height.value_or(0));
        snap.send_fps = outb->frames_per_second.value_or(0);
        snap.send_bitrate = bps;
        snap.quality_limitation = outb->quality_limitation_reason.value_or("none");
        bool have_enc = false;
        snap.encode_ms = PerFrameMs(video_enc_, outb->total_encode_time.value_or(0),
                                    outb->frames_encoded.value_or(0), &have_enc);
        const auto* codec = CodecFor<webrtc::RTCCodecStats>(*report, outb->codec_id);
        if (snap.video_codec.empty() && codec && codec->mime_type) snap.video_codec = *codec->mime_type;
        if (snap.profile_level_id.empty() && codec && codec->sdp_fmtp_line) {
          snap.profile_level_id = ProfileLevelId(*codec->sdp_fmtp_line);
        }
        video_out.str("codec", snap.video_codec);
        video_out.str("profile_level_id", snap.profile_level_id);
        video_out.integer("send_width", snap.send_w);
        video_out.integer("send_height", snap.send_h);
        video_out.number("send_fps", snap.send_fps, 2);
        video_out.number("send_bitrate_bps", bps, 0);
        if (have_enc) video_out.number("encode_ms", snap.encode_ms, 2);
        else video_out.nul("encode_ms");
        video_out.integer("frames_encoded", outb->frames_encoded.value_or(0));
        video_out.str("quality_limitation", snap.quality_limitation);
        video_out.integer("send_packets_lost", remote && remote->packets_lost ? *remote->packets_lost : 0);
        video_out.number("send_jitter_ms", remote && remote->jitter ? *remote->jitter * 1000.0 : -1, 2);
        video_out.integer("send_nack", outb->nack_count.value_or(0));
        video_out.integer("send_pli", outb->pli_count.value_or(0));
        if (outb->total_encode_time) video_out.number("total_encode_time_s", *outb->total_encode_time, 3);
      }
    }

    if (have_audio && !audio_codec_.empty()) {
      // mime type is filled from inbound when present; outbound may have set audio_codec_.
    }
    if (have_audio) {
      // Rebuild is awkward; stamp codec if the inbound side left it empty by
      // rewriting only when we stored one from outbound and inbound was empty.
    }

    // Echo: the level we send (after the echo canceller) next to the level we
    // play. When the far end talks and nobody talks here, send should stay
    // near the noise floor; a send level that follows recv is echo leaking.
    double send_dbfs = kNoLevel;
    double erl_db = kNoLevel;
    double erle_db = kNoLevel;
    const auto audio_sources = report->GetStatsOfType<webrtc::RTCAudioSourceStats>();
    if (!audio_sources.empty()) {
      const auto* src = audio_sources.front();
      if (src->total_audio_energy && src->total_samples_duration) {
        send_dbfs = LevelDbfs(send_energy_, *src->total_audio_energy,
                              *src->total_samples_duration);
      }
      if (src->echo_return_loss) erl_db = *src->echo_return_loss;
      if (src->echo_return_loss_enhancement) erle_db = *src->echo_return_loss_enhancement;
    }
    const std::string echo_path = DirectEchoPath();
    webrtc::AudioProcessingStats apm;
    const bool have_apm = echo_path == "aec3" && DirectApmStats(&apm);
    if (have_apm) {
      if (erl_db == kNoLevel && apm.echo_return_loss) erl_db = *apm.echo_return_loss;
      if (erle_db == kNoLevel && apm.echo_return_loss_enhancement)
        erle_db = *apm.echo_return_loss_enhancement;
    }
    if (have_audio) {
      audio_out.str("aec", echo_path);
      if (send_dbfs != kNoLevel) audio_out.number("send_level_dbfs", send_dbfs, 1);
      if (recv_dbfs != kNoLevel) audio_out.number("recv_level_dbfs", recv_dbfs, 1);
      if (erl_db != kNoLevel) audio_out.number("echo_return_loss_db", erl_db, 1);
      if (erle_db != kNoLevel) audio_out.number("echo_return_loss_enhancement_db", erle_db, 1);
      if (have_apm && apm.delay_ms) audio_out.integer("aec_delay_ms", *apm.delay_ms);
      if (have_apm && apm.divergent_filter_fraction)
        audio_out.number("aec_divergent_filter_fraction", *apm.divergent_filter_fraction, 3);
      char eline[384];
      auto fmt = [](double v, char* buf, size_t n) {
        if (v == kNoLevel) std::snprintf(buf, n, "na");
        else std::snprintf(buf, n, "%.1f", v);
      };
      char sb[16], rb[16], lb[16], eb[16];
      fmt(send_dbfs, sb, sizeof(sb));
      fmt(recv_dbfs, rb, sizeof(rb));
      fmt(erl_db, lb, sizeof(lb));
      fmt(erle_db, eb, sizeof(eb));
      std::snprintf(eline, sizeof(eline),
                    "echo aec=%s send_dbfs=%s recv_dbfs=%s erl_db=%s erle_db=%s "
                    "aec_delay_ms=%d divergent=%.3f\n",
                    echo_path.c_str(), sb, rb, lb, eb,
                    have_apm && apm.delay_ms ? *apm.delay_ms : -1,
                    have_apm && apm.divergent_filter_fraction ? *apm.divergent_filter_fraction : -1.0);
      snap.echo_line = eline;
    }

    Json root;
    root.begin();
    root.str("event", "stats");
    root.number("t", 0, 1);  // replaced by the owner with the real age
    root.str("ice", ice);
    if (rtt_ms >= 0) root.number("rtt_ms", rtt_ms, 1);
    else root.nul("rtt_ms");
    if (have_audio) {
      if (audio_codec_.size() && audio_out_has_codec_) {
        // inbound already wrote codec when it was present
      }
      root.raw("audio", audio_out.end());
    } else {
      root.nul("audio");
    }
    if (have_video) root.raw("video", video_out.end());
    else root.nul("video");
    if (jitter_buffer_ms >= 0) root.number("jitter_buffer_ms", jitter_buffer_ms, 2);
    else root.nul("jitter_buffer_ms");
    if (jitter_target_ms >= 0) root.number("jitter_buffer_target_ms", jitter_target_ms, 2);
    root.integer("concealment_events", concealment);
    root.integer("nack", nack);
    root.integer("pli", pli);
    if (have_audio) {
      char line[512];
      std::snprintf(line, sizeof(line),
                    "audio onwire=payload+rtp+ip/udp send_ipv4_udp_rtp_kbps=%.3f send_payload_kbps=%.3f "
                    "recv_ipv4_udp_rtp_kbps=%.3f "
                    "recv_payload_kbps=%.3f send_packets=%lld recv_packets=%lld lost=%lld "
                    "jitter_ms=%.2f jitter_buffer_ms=%.2f jitter_target_ms=%.1f rtt_ms=%.1f algo_ms=%d "
                    "mouth_to_ear_est_ms=%.1f\n",
                    send_onwire_kbps, send_payload_kbps, recv_onwire_kbps, recv_payload_kbps,
                    send_packets, recv_packets, packets_lost, jitter_ms, jitter_buffer_ms,
                    jitter_target_ms, rtt_ms, algo_ms, m2e_ms);
      snap.audio_line = line;
    }
    snap.ice = ice;
    snap.rtt_ms = rtt_ms;
    snap.nack = nack;
    snap.pli = pli;
    snap.json = root.end();
    return snap;
  }

  mutable std::recursive_mutex mu_;
  Snapshot last_;
  bool has_last_ = false;
  std::atomic<int> samples_{0};
  std::chrono::steady_clock::time_point last_sample_{};
  std::chrono::steady_clock::time_point first_audio_{};
  Counter audio_in_bytes_;
  Counter audio_in_total_;
  Counter audio_out_bytes_;
  Counter audio_out_total_;
  struct JitterCounter {
    double delay = 0;
    double target = 0;
    uint64_t emitted = 0;
    bool seen = false;
  };
  JitterCounter jb_prev_;
  EnergyCounter send_energy_;
  EnergyCounter recv_energy_;
  bool onwire_printed_ = false;
  WireCounter audio_in_wire_;
  WireCounter audio_out_wire_;
  Counter video_in_bytes_;
  Counter video_out_bytes_;
  Counter video_enc_;
  Counter video_dec_;
  std::string audio_codec_;
  bool audio_out_has_codec_ = false;
  bool encrypted_ = false;

  friend class DirectCallStatsImpl;
  std::chrono::steady_clock::time_point FirstAudio() const {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    return first_audio_;
  }
};

class FinalStats : public webrtc::RTCStatsCollectorCallback {
 public:
  FinalStats(webrtc::scoped_refptr<StatsCollector> inner, webrtc::Event* done)
      : inner_(std::move(inner)), done_(done) {}
  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    inner_->OnStatsDelivered(report);
    if (done_) done_->Set();
  }
  webrtc::scoped_refptr<StatsCollector> inner_;
  webrtc::Event* done_ = nullptr;
};

class DirectCallStatsImpl : public DirectCallStats {
 public:
  DirectCallStatsImpl(int interval_sec,
                      std::chrono::steady_clock::time_point origin,
                      webrtc::Thread* signaling_thread,
                      std::function<webrtc::PeerConnectionInterface*()> peer_connection,
                      bool encrypted)
      : interval_sec_(interval_sec),
        origin_(origin),
        signaling_thread_(signaling_thread),
        peer_connection_(std::move(peer_connection)),
        collector_(webrtc::make_ref_counted<StatsCollector>()) {
    collector_->set_encrypted(encrypted);
    if (interval_sec_ > 0) {
      thread_ = std::thread([this] { Loop(); });
    }
  }

  ~DirectCallStatsImpl() override { Summarize(); }

  void OnIceConnected() override {
    std::lock_guard<std::mutex> lock(mu_);
    if (ice_at_.time_since_epoch().count() == 0) ice_at_ = std::chrono::steady_clock::now();
  }

  void Summarize() override {
    bool expected = false;
    if (!summarized_.compare_exchange_strong(expected, true)) return;
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (signaling_thread_ && !signaling_thread_->IsCurrent() &&
        !signaling_thread_->IsQuitting()) {
      webrtc::Event done;
      auto gate = webrtc::make_ref_counted<FinalStats>(collector_, &done);
      signaling_thread_->PostTask([this, gate] {
        auto* pc = peer_connection_ ? peer_connection_() : nullptr;
        if (!pc) {
          if (gate->done_) gate->done_->Set();
          return;
        }
        pc->GetStats(gate.get());
      });
      done.Wait(webrtc::TimeDelta::Seconds(2));
    }
    const Snapshot snap = collector_->Last();
    std::chrono::steady_clock::time_point ice_at;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ice_at = ice_at_;
    }
    const auto first_audio = collector_->FirstAudio();
    Json summary;
    summary.begin();
    summary.str("event", "summary");
    summary.integer("ice_connected_ms", ElapsedMs(origin_, ice_at));
    summary.integer("first_audio_ms", ElapsedMs(origin_, first_audio));
    summary.str("ice", snap.ice.empty() ? "none" : snap.ice);
    if (snap.rtt_ms >= 0) summary.number("rtt_ms", snap.rtt_ms, 1);
    else summary.nul("rtt_ms");
    summary.str("video_codec", snap.video_codec);
    summary.str("profile_level_id", snap.profile_level_id);
    summary.integer("send_width", snap.send_w);
    summary.integer("send_height", snap.send_h);
    summary.number("send_fps", snap.send_fps, 2);
    summary.number("send_bitrate_bps", snap.send_bitrate, 0);
    summary.number("encode_ms", snap.encode_ms, 2);
    summary.str("quality_limitation", snap.quality_limitation.empty() ? "none" : snap.quality_limitation);
    summary.integer("recv_width", snap.recv_w);
    summary.integer("recv_height", snap.recv_h);
    summary.number("recv_fps", snap.recv_fps, 2);
    summary.number("recv_bitrate_bps", snap.recv_bitrate, 0);
    summary.number("decode_ms", snap.decode_ms, 2);
    summary.integer("frames_dropped", snap.frames_dropped);
    summary.integer("nack", snap.nack);
    summary.integer("pli", snap.pli);
    const std::string line = summary.end();
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    std::fprintf(stderr,
                 "summary ice=%s rtt_ms=%.1f ice_connected_ms=%lld first_audio_ms=%lld "
                 "video=%s profile=%s send=%dx%d@%.1ffps encode_ms=%.2f limitation=%s "
                 "recv=%dx%d@%.1ffps decode_ms=%.2f dropped=%lld\n",
                 snap.ice.empty() ? "none" : snap.ice.c_str(),
                 snap.rtt_ms,
                 static_cast<long long>(ElapsedMs(origin_, ice_at)),
                 static_cast<long long>(ElapsedMs(origin_, first_audio)),
                 snap.video_codec.c_str(),
                 snap.profile_level_id.c_str(),
                 snap.send_w, snap.send_h, snap.send_fps, snap.encode_ms,
                 snap.quality_limitation.empty() ? "none" : snap.quality_limitation.c_str(),
                 snap.recv_w, snap.recv_h, snap.recv_fps, snap.decode_ms,
                 static_cast<long long>(snap.frames_dropped));
    if (!snap.audio_line.empty()) std::fputs(snap.audio_line.c_str(), stderr);
    if (!snap.echo_line.empty()) std::fputs(snap.echo_line.c_str(), stderr);
  }

 private:
  void Loop() {
    while (!stop_.load()) {
      for (int i = 0; i < interval_sec_ * 10 && !stop_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (stop_.load()) break;
      if (!signaling_thread_ || signaling_thread_->IsQuitting()) continue;
      const int before = collector_->samples();
      signaling_thread_->PostTask([this] {
        if (stop_.load()) return;
        if (auto* pc = peer_connection_ ? peer_connection_() : nullptr) {
          pc->GetStats(collector_.get());
        }
      });
      for (int i = 0; i < 20 && collector_->samples() == before && !stop_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (collector_->has_last()) {
        Snapshot snap = collector_->Last();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - origin_).count();
        std::string body = snap.json;
        const std::string token = "\"t\":0.0";
        const auto pos = body.find(token);
        if (pos != std::string::npos) {
          char repl[64];
          std::snprintf(repl, sizeof(repl), "\"t\":%.1f", t);
          body.replace(pos, token.size(), repl);
        }
        std::chrono::steady_clock::time_point ice_at;
        {
          std::lock_guard<std::mutex> lock(mu_);
          ice_at = ice_at_;
        }
        Json extra;
        extra.begin();
        extra.integer("ice_connected_ms", ElapsedMs(origin_, ice_at));
        extra.integer("first_audio_ms", ElapsedMs(origin_, collector_->FirstAudio()));
        std::string tail = extra.end();
        if (!body.empty() && body.back() == '}' && tail.size() > 2) {
          body.pop_back();
          body += ',';
          body += tail.substr(1);
        }
        std::fwrite(body.data(), 1, body.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        if (!snap.audio_line.empty()) {
          std::fputs(snap.audio_line.c_str(), stderr);
        }
        if (!snap.echo_line.empty()) {
          std::fputs(snap.echo_line.c_str(), stderr);
        }
      }
    }
  }

  int interval_sec_;
  std::chrono::steady_clock::time_point origin_;
  webrtc::Thread* signaling_thread_;
  std::function<webrtc::PeerConnectionInterface*()> peer_connection_;
  webrtc::scoped_refptr<StatsCollector> collector_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> summarized_{false};
  std::mutex mu_;
  std::chrono::steady_clock::time_point ice_at_{};
};

}  // namespace

std::unique_ptr<DirectCallStats> DirectCallStats::Create(
    int interval_sec,
    std::chrono::steady_clock::time_point origin,
    webrtc::Thread* signaling_thread,
    std::function<webrtc::PeerConnectionInterface*()> peer_connection,
    bool encrypted) {
  return std::make_unique<DirectCallStatsImpl>(interval_sec, origin, signaling_thread,
                                               std::move(peer_connection), encrypted);
}
