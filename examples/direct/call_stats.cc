/*
 *  (c) 2026, wilddolphin2025
 *  For WebRTCsays.ai project
 */

#include "call_stats.h"

#include <atomic>
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
    if (pair) {
      if (pair->current_round_trip_time) rtt_ms = *pair->current_round_trip_time * 1000.0;
      if (pair->local_candidate_id) {
        if (const auto* local =
                report->GetAs<webrtc::RTCLocalIceCandidateStats>(*pair->local_candidate_id)) {
          if (local->candidate_type) ice = *local->candidate_type;
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
    double jitter_buffer_ms = -1;

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
        if (in->jitter_buffer_delay && in->jitter_buffer_emitted_count &&
            *in->jitter_buffer_emitted_count > 0) {
          jitter_buffer_ms = (*in->jitter_buffer_delay / static_cast<double>(*in->jitter_buffer_emitted_count)) * 1000.0;
        }
        const auto* codec = CodecFor<webrtc::RTCCodecStats>(*report, in->codec_id);
        audio_out.str("codec", codec && codec->mime_type ? *codec->mime_type : "");
        audio_out.number("recv_bitrate_bps", bps, 0);
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
        audio_out.number("send_bitrate_bps", bps, 0);
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
    root.integer("concealment_events", concealment);
    root.integer("nack", nack);
    root.integer("pli", pli);
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
  Counter audio_out_bytes_;
  Counter video_in_bytes_;
  Counter video_out_bytes_;
  Counter video_enc_;
  Counter video_dec_;
  std::string audio_codec_;
  bool audio_out_has_codec_ = false;

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
                      std::function<webrtc::PeerConnectionInterface*()> peer_connection)
      : interval_sec_(interval_sec),
        origin_(origin),
        signaling_thread_(signaling_thread),
        peer_connection_(std::move(peer_connection)),
        collector_(webrtc::make_ref_counted<StatsCollector>()) {
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
    std::function<webrtc::PeerConnectionInterface*()> peer_connection) {
  return std::make_unique<DirectCallStatsImpl>(interval_sec, origin, signaling_thread,
                                               std::move(peer_connection));
}
