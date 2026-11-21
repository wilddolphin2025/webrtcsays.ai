/*
 *  (c) 2025, wilddolphin2025
 *  For WebRTCsays.ai project
 *  https://github.com/wilddolphin2025
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <unistd.h>

#include <string>
#include <vector>
#include <climits>
#include <csignal>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <algorithm>
#include <cctype>
#include <regex>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>


#include "direct.h"
#include "option.h"
#include "signaling_bind.h"
#include "client.h"
//#include "room.h"

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_shutdown_count = 0;

// Graceful teardown (BYE, ack wait, thread joins) must finish within this or
// the process exits hard. Keep it below systemd's TimeoutStopSec.
constexpr int kShutdownDeadlineMs = 10000;

// Only async-signal-safe calls in here: write(2) and _exit(2).
extern "C" void signalHandler(int /*signal*/) {
    g_shutdown = 1;
    // C++20 deprecates ++ on a volatile. A plain read and write of
    // sig_atomic_t is the signal-safe form.
    const sig_atomic_t n = g_shutdown_count + 1;
    g_shutdown_count = n;
    if (n >= 2) {
        static const char kMsg[] = "\nSecond termination signal, exiting now\n";
        (void)!write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
        _exit(130);
    }
    static const char kMsg[] = "\nTermination signal received, hanging up...\n";
    (void)!write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
}

static void InstallSignalHandlers() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) {
        sigaction(sig, &sa, nullptr);
    }
    // Writes to a socket the peer already closed must fail with EPIPE
    // instead of killing the process.
    signal(SIGPIPE, SIG_IGN);
}

// Started once shutdown begins; exits hard if teardown wedges.
static void ArmShutdownWatchdog() {
    static std::atomic<bool> armed{false};
    if (armed.exchange(true)) return;
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownDeadlineMs));
        fprintf(stderr, "Graceful shutdown exceeded %d ms, forcing exit\n",
                kShutdownDeadlineMs);
        _exit(1);
    }).detach();
}

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

namespace {

static std::string Base64Encode(const uint8_t* data, size_t len) {
  static constexpr char kB64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                 (static_cast<uint32_t>(data[i + 1]) << 8) |
                 static_cast<uint32_t>(data[i + 2]);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(kB64[(n >> 6) & 63]);
    out.push_back(kB64[n & 63]);
    i += 3;
  }
  const size_t rem = len - i;
  if (rem == 1) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                 (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(kB64[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

static std::array<uint8_t, 20> Sha1Digest(const std::string& msg) {
  auto rol = [](uint32_t v, int b) { return (v << b) | (v >> (32 - b)); };
  uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;
  std::vector<uint8_t> data(msg.begin(), msg.end());
  data.push_back(0x80);
  while ((data.size() % 64) != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xEFCDAB89;
  uint32_t h2 = 0x98BADCFE;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xC3D2E1F0;

  for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      size_t o = chunk + i * 4;
      w[i] = (static_cast<uint32_t>(data[o]) << 24) |
             (static_cast<uint32_t>(data[o + 1]) << 16) |
             (static_cast<uint32_t>(data[o + 2]) << 8) |
             static_cast<uint32_t>(data[o + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0, k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::array<uint8_t, 20> out{};
  auto wr = [&out](int idx, uint32_t v) {
    out[idx] = static_cast<uint8_t>((v >> 24) & 0xff);
    out[idx + 1] = static_cast<uint8_t>((v >> 16) & 0xff);
    out[idx + 2] = static_cast<uint8_t>((v >> 8) & 0xff);
    out[idx + 3] = static_cast<uint8_t>(v & 0xff);
  };
  wr(0, h0);
  wr(4, h1);
  wr(8, h2);
  wr(12, h3);
  wr(16, h4);
  return out;
}

class DirectWebSocketProxy {
 public:
  DirectWebSocketProxy(std::string tcp_host, int tcp_port, int ws_port,
                       std::string bind_host)
      : tcp_host_(std::move(tcp_host)),
        tcp_port_(tcp_port),
        ws_port_(ws_port),
        bind_host_(std::move(bind_host)) {}

  ~DirectWebSocketProxy() { Stop(); }

  bool Start() {
    if (running_.load()) return true;
    running_.store(true);
    server_thread_ = std::thread([this]() { ServeLoop(); });
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
      shutdown(listen_fd_, SHUT_RDWR);  // wakes accept()
    }
    if (server_thread_.joinable()) server_thread_.join();
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
    // Client loops poll running_ every 200 ms and say BYE on the way out.
    std::vector<ClientThread> clients;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients.swap(client_threads_);
    }
    for (auto& c : clients) {
      c.thread.join();
    }
  }

 private:
  static std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
  }

  static bool ReadExactly(int fd, void* out, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
      ssize_t r = recv(fd, p + got, n - got, 0);
      if (r <= 0) return false;
      got += static_cast<size_t>(r);
    }
    return true;
  }

  static std::string ComputeWebSocketAccept(const std::string& key) {
    static constexpr const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string in = key + kGuid;
    const auto digest = Sha1Digest(in);
    return Base64Encode(digest.data(), digest.size());
  }

  static bool SendWsTextFrame(int fd, const std::string& text) {
    std::vector<uint8_t> frame;
    frame.reserve(2 + 8 + text.size());
    frame.push_back(0x81);  // FIN + text
    const size_t len = text.size();
    if (len <= 125) {
      frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 65535) {
      frame.push_back(126);
      frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
      frame.push_back(static_cast<uint8_t>(len & 0xff));
    } else {
      frame.push_back(127);
      for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xff));
    }
    frame.insert(frame.end(), text.begin(), text.end());
    return SendAll(fd, frame.data(), frame.size());
  }

  // send() may write partially under backpressure (large SDP blobs).
  static bool SendAll(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (len > 0) {
      ssize_t n = send(fd, p, len, kSendFlags);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      p += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  static bool SendDirectFrame(int fd, const std::string& msg) {
    if (msg.size() > 65535) return false;
    uint8_t hdr[2] = {static_cast<uint8_t>((msg.size() >> 8) & 0xff),
                      static_cast<uint8_t>(msg.size() & 0xff)};
    return SendAll(fd, hdr, sizeof(hdr)) && SendAll(fd, msg.data(), msg.size());
  }

  static bool SendWsControlFrame(int fd, uint8_t opcode, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.reserve(2 + payload.size());
    frame.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0f)));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0x7f));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return SendAll(fd, frame.data(), frame.size());
  }

  static bool ReadWsFrame(int fd, uint8_t& opcode, std::string& text_payload) {
    uint8_t hdr[2];
    if (!ReadExactly(fd, hdr, 2)) return false;
    opcode = static_cast<uint8_t>(hdr[0] & 0x0f);
    const bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = static_cast<uint64_t>(hdr[1] & 0x7f);
    if (len == 126) {
      uint8_t ex[2];
      if (!ReadExactly(fd, ex, 2)) return false;
      len = (static_cast<uint64_t>(ex[0]) << 8) | static_cast<uint64_t>(ex[1]);
    } else if (len == 127) {
      uint8_t ex[8];
      if (!ReadExactly(fd, ex, 8)) return false;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<uint64_t>(ex[i]);
    }
    std::array<uint8_t, 4> mask{0, 0, 0, 0};
    if (masked) {
      if (!ReadExactly(fd, mask.data(), mask.size())) return false;
    }
    if (len > (1u << 20)) return false;  // limit to 1 MiB
    std::vector<uint8_t> payload(static_cast<size_t>(len));
    if (len > 0 && !ReadExactly(fd, payload.data(), payload.size())) return false;
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    }
    text_payload.assign(payload.begin(), payload.end());
    return true;
  }

  static bool ExtractJsonString(const std::string& json, const std::string& key, std::string& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"((?:\\\\.|[^\\\\\"])*)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re) || m.size() < 2) return false;
    out = m[1].str();
    return true;
  }

  static bool ExtractJsonInt(const std::string& json, const std::string& key, int& out) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch m;
    if (!std::regex_search(json, m, re) || m.size() < 2) return false;
    out = std::atoi(m[1].str().c_str());
    return true;
  }

  static std::string JsonToDirectMessage(const std::string& in) {
    std::string type;
    if (!ExtractJsonString(in, "type", type)) return in;
    if (type == "offer" || type == "answer") {
      std::string sdp;
      ExtractJsonString(in, "sdp", sdp);
      if (sdp.empty()) return "";
      return (type == "offer" ? "OFFER:" : "ANSWER:") + sdp;
    }
    if (type == "ice") {
      std::string candidate;
      int mline = 0;
      ExtractJsonString(in, "candidate", candidate);
      ExtractJsonInt(in, "sdpMLineIndex", mline);
      if (candidate.empty()) return "";
      return "ICE:" + std::to_string(mline) + ":" + candidate;
    }
    if (type == "hangup") return "BYE";
    if (type == "voice") {
      std::string name;
      ExtractJsonString(in, "name", name);
      return name.empty() ? "" : ("VOICE:" + name);
    }
    if (type == "cancel") return "CANCEL";
    if (type == "face") {
      std::string data;
      ExtractJsonString(in, "data", data);
      return data.empty() ? "" : ("FACE:" + data);
    }
    if (type == "raw") {
      std::string msg;
      ExtractJsonString(in, "message", msg);
      return msg;
    }
    return in;
  }

  static std::string MaybeJsonToDirect(const std::string& in) {
    std::string msg = Trim(in);
    if (msg.empty()) return msg;
    if (!msg.empty() && msg.front() == '{') return JsonToDirectMessage(msg);
    return msg;
  }

  int ConnectToDirectTcp() const {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(tcp_port_));
    if (inet_pton(AF_INET, tcp_host_.c_str(), &sa.sin_addr) != 1) {
      close(fd);
      return -1;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  bool PerformHandshake(int client_fd) const {
    std::string req;
    std::array<char, 4096> buf{};
    while (req.find("\r\n\r\n") == std::string::npos) {
      ssize_t n = recv(client_fd, buf.data(), buf.size(), 0);
      if (n <= 0) return false;
      req.append(buf.data(), static_cast<size_t>(n));
      if (req.size() > (1u << 20)) return false;
    }

    std::istringstream iss(req);
    std::string line;
    std::string ws_key;
    while (std::getline(iss, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto pos = line.find(':');
      if (pos == std::string::npos) continue;
      std::string h = line.substr(0, pos);
      std::string v = Trim(line.substr(pos + 1));
      std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (h == "sec-websocket-key") ws_key = v;
    }
    if (ws_key.empty()) return false;
    const std::string accept = ComputeWebSocketAccept(ws_key);
    std::ostringstream out;
    out << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
    const std::string resp = out.str();
    return SendAll(client_fd, resp.data(), resp.size());
  }

  void HandleClient(int client_fd) {
    // Bound the HTTP upgrade so a silent client cannot pin this thread.
    struct timeval handshake_tv = {5, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &handshake_tv, sizeof(handshake_tv));
    if (!PerformHandshake(client_fd)) {
      close(client_fd);
      return;
    }
    struct timeval no_tv = {0, 0};
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));

    int direct_fd = ConnectToDirectTcp();
    if (direct_fd < 0) {
      SendWsTextFrame(client_fd, "480 Temporarily Unavailable");
      SendWsControlFrame(client_fd, 0x8, {0x03, 0xF3});  // 1011
      close(client_fd);
      return;
    }

    // Browsers answer pings with pongs; anything from the browser counts as
    // liveness. A tab that vanished (sleep, network loss) stops answering.
    constexpr auto kPingInterval = std::chrono::seconds(15);
    constexpr auto kIdleTimeout = std::chrono::seconds(45);
    auto last_rx = std::chrono::steady_clock::now();
    auto last_ping = last_rx;

    // Who ended the session decides what each side is told.
    enum class End { kRunning, kBrowserGone, kBrowserClosed, kDirectClosed, kStopping };
    End end = End::kRunning;
    bool bye_seen = false;  // BYE already crossed the bridge in either direction

    std::vector<uint8_t> direct_buf;
    direct_buf.reserve(65536);
    while (end == End::kRunning) {
      if (!running_.load() || g_shutdown) {
        end = End::kStopping;
        break;
      }
      auto now = std::chrono::steady_clock::now();
      if (now - last_rx > kIdleTimeout) {
        fprintf(stderr, "[ws] browser idle for %llds, dropping\n",
                static_cast<long long>(kIdleTimeout.count()));
        end = End::kBrowserGone;
        break;
      }
      if (now - last_ping > kPingInterval) {
        SendWsControlFrame(client_fd, 0x9, {});
        last_ping = now;
      }

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(client_fd, &rfds);
      FD_SET(direct_fd, &rfds);
      const int maxfd = std::max(client_fd, direct_fd);
      struct timeval tv = {0, 200000};
      const int sel = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
      if (sel < 0) {
        if (errno == EINTR) continue;
        end = End::kBrowserGone;
        break;
      }
      if (sel == 0) continue;

      if (FD_ISSET(client_fd, &rfds)) {
        uint8_t opcode = 0;
        std::string ws_text;
        if (!ReadWsFrame(client_fd, opcode, ws_text)) {
          end = End::kBrowserGone;
          break;
        }
        last_rx = std::chrono::steady_clock::now();
        if (opcode == 0x8) {
          end = End::kBrowserClosed;
          break;
        }
        if (opcode == 0x9) {
          std::vector<uint8_t> echo(ws_text.begin(), ws_text.end());
          SendWsControlFrame(client_fd, 0xA, echo);
          continue;
        }
        if (opcode != 0x1) continue;  // pong / binary
        std::string direct_msg = MaybeJsonToDirect(ws_text);
        if (direct_msg.empty()) continue;
        if (direct_msg == "BYE" || direct_msg == "CANCEL") bye_seen = true;
        if (!SendDirectFrame(direct_fd, direct_msg)) {
          end = End::kDirectClosed;
          break;
        }
      }

      if (FD_ISSET(direct_fd, &rfds)) {
        std::array<char, 65536> b{};
        const ssize_t n = recv(direct_fd, b.data(), b.size(), 0);
        if (n <= 0) {
          end = End::kDirectClosed;
          break;
        }
        direct_buf.insert(direct_buf.end(), b.begin(), b.begin() + n);
        while (direct_buf.size() >= 2) {
          const uint16_t mlen = (static_cast<uint16_t>(direct_buf[0]) << 8) |
                                static_cast<uint16_t>(direct_buf[1]);
          if (direct_buf.size() < static_cast<size_t>(2 + mlen)) break;
          std::string msg(direct_buf.begin() + 2, direct_buf.begin() + 2 + mlen);
          direct_buf.erase(direct_buf.begin(), direct_buf.begin() + 2 + mlen);
          if (msg == "BYE") bye_seen = true;
          if (!SendWsTextFrame(client_fd, msg)) {
            end = End::kBrowserGone;
            break;
          }
        }
      }
    }

    // Browser side vanished or we are stopping: tell directcall so it tears
    // the call down now instead of waiting for ICE to time out.
    if (end != End::kDirectClosed && !bye_seen) {
      SendDirectFrame(direct_fd, "BYE");
    }
    // Close the WebSocket properly so the page sees code 1000 / 1001.
    if (end == End::kDirectClosed) {
      SendWsControlFrame(client_fd, 0x8, {0x03, 0xE8});  // 1000 normal
    } else if (end == End::kStopping) {
      SendWsControlFrame(client_fd, 0x8, {0x03, 0xE9});  // 1001 going away
    } else if (end == End::kBrowserClosed) {
      SendWsControlFrame(client_fd, 0x8, {});  // echo close
    }
    shutdown(direct_fd, SHUT_WR);
    close(direct_fd);
    close(client_fd);
  }

  void ServeLoop() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;
    int one = 1;
    // No SO_REUSEPORT: a second directcall must fail to bind instead of
    // silently sharing the port and stealing browser connections.
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    in_addr_t bind_addr = htonl(INADDR_ANY);
    std::string bind_error;
    if (!ResolveSignalingBind(bind_host_, &bind_addr, &bind_error)) {
      fprintf(stderr, "WebSocket signaling bind refused: %s\n", bind_error.c_str());
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(ws_port_));
    sa.sin_addr.s_addr = bind_addr;
    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0 ||
        listen(listen_fd_, 16) != 0) {
      fprintf(stderr, "WebSocket signaling: cannot listen on port %d: %s\n",
              ws_port_, strerror(errno));
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    char shown[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &sa.sin_addr, shown, sizeof(shown))) {
      std::snprintf(shown, sizeof(shown), "?");
    }
    std::cerr << "WebSocket signaling listening on ws://" << shown << ":" << ws_port_
              << " -> " << tcp_host_ << ":" << tcp_port_ << "\n";

    while (running_.load() && !g_shutdown) {
      int cfd = accept(listen_fd_, nullptr, nullptr);
      if (cfd < 0) {
        if (!running_.load() || g_shutdown) break;
        if (errno == EINTR || errno == ECONNABORTED) continue;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }
      std::lock_guard<std::mutex> lock(clients_mutex_);
      // Reap finished client threads so the list does not grow forever.
      for (auto it = client_threads_.begin(); it != client_threads_.end();) {
        if (it->done->load()) {
          it->thread.join();
          it = client_threads_.erase(it);
        } else {
          ++it;
        }
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      client_threads_.push_back(
          {std::thread([this, cfd, done]() {
             HandleClient(cfd);
             done->store(true);
           }),
           done});
    }
  }

  std::string tcp_host_;
  int tcp_port_ = 3456;
  int ws_port_ = 3457;
  std::string bind_host_;
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread server_thread_;
  struct ClientThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::mutex clients_mutex_;
  std::vector<ClientThread> client_threads_;
};

}  // namespace

int main(int argc, char* argv[]) {

  Options opts;
  std::string options;
  if (argc == 1) {
    opts.help = true;
  } else {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (const auto& piece : args)
      options += (piece + " ");
    opts = parseOptions(options.c_str());
  }

  if (opts.help) {
    auto usage = opts.help_string;
    // Print usage to stderr instead for command-line tools
    fprintf(stderr, "%s\n", usage.c_str());
    return 1;
  }
  if (!opts.parse_error.empty()) {
    fprintf(stderr, "%s\n", opts.parse_error.c_str());
    return 1;
  }

  InstallSignalHandlers();

  if (const char* level = getenv("DIRECT_LOG_LEVEL")) {
    const std::string l(level);
    // libdirect only exports DirectSetLoggingLevel; its enum mirrors
    // webrtc::LoggingSeverity.
    DirectSetLoggingLevel(static_cast<LoggingSeverity>(
        l == "verbose" ? webrtc::LS_VERBOSE
        : l == "info"  ? webrtc::LS_INFO
        : l == "error" ? webrtc::LS_ERROR
                       : webrtc::LS_WARNING));
  }
  DirectApplication::rtcInitialize();

  // Parse server address and room from options
  if (opts.call_duration_sec > 0) {
    const int seconds = opts.call_duration_sec;
    std::thread([seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      if (!g_shutdown) {
        fprintf(stderr, "\nDuration of %d s reached, hanging up\n", seconds);
        g_shutdown = 1;
      }
    }).detach();
  }

  std::string room_name = opts.room_name.empty() ? "room101" : opts.room_name;  // Use provided room or default
  // Ensure opts.room_name is set so that callee/caller register correctly
  opts.room_name = room_name;
   
  // Validate required parameters for name-based calling
  if (opts.user_name.empty()) {
    fprintf(stderr, "Error: --user_name is required for name-based calling\n");
    return 1;
  }
  
  if (opts.mode == "caller" && opts.target_name.empty()) {
    fprintf(stderr, "Error: --target_name is required when mode is caller\n");
    return 1;
  }

  std::shared_ptr<DirectCalleeClient> callee;
  std::shared_ptr<DirectCallerClient> caller;
  std::unique_ptr<DirectWebSocketProxy> ws_proxy;

  {
    in_addr_t signaling_addr = 0;
    std::string signaling_error;
    if (!ResolveSignalingBind(opts.signaling_bind, &signaling_addr, &signaling_error)) {
      fprintf(stderr, "Invalid signaling_bind: %s\n", signaling_error.c_str());
      return 1;
    }
  }

  if (opts.websocket_signaling && (opts.mode == "callee" || opts.mode == "both")) {
    std::string callee_ip;
    int callee_port = 3456;
    if (!opts.address.empty()) {
      std::string parsed_ip;
      int parsed_port = 0;
      if (ParseIpAndPort(opts.address, parsed_ip, parsed_port) && parsed_port > 0) {
        callee_port = parsed_port;
      }
    }
    // The bridge dials the TCP listener. INADDR_ANY accepts 127.0.0.1.
    // A specific signaling_bind, such as 127.0.0.1, accepts only that address.
    std::string dial_host = "127.0.0.1";
    if (!opts.signaling_bind.empty() && opts.signaling_bind != "0.0.0.0") {
      dial_host = opts.signaling_bind;
    }
    ws_proxy = std::make_unique<DirectWebSocketProxy>(
        dial_host, callee_port, opts.websocket_port, opts.signaling_bind);
    ws_proxy->Start();
  }
  
  if (opts.mode == "callee" or opts.mode == "both") {
    int session_count = 0;
    while (!g_shutdown) {
      if (!callee) {
        fprintf(stderr, "Callee loop entered, g_shutdown=%d\n", g_shutdown);
        session_count++;
        fprintf(stderr, "Starting callee session #%d\n", session_count);
        callee = std::make_shared<DirectCalleeClient>(opts);

        if (!callee->Initialize()) {
          fprintf(stderr, "failed to initialize callee\n");
          return 1;
        }
        if (!callee->StartListening()) {
          fprintf(stderr, "Failed to start listening\n");
          return 1;
        }
        callee->RunOnBackgroundThread();

        // Preload AI model files into OS page cache on a background thread
        // so the first call doesn't stall on disk I/O
        if (session_count == 1) {
          std::thread([opts]() {
            auto preload = [](const std::string& path, const char* name) {
              if (path.empty()) return;
              FILE* f = fopen(path.c_str(), "rb");
              if (!f) return;
              // Heap buffer: secondary threads get 512 KiB stacks on macOS.
              std::vector<char> buf(1 << 20);
              while (fread(buf.data(), 1, buf.size(), f) > 0) {}
              fclose(f);
              fprintf(stderr, "Preloaded %s into page cache: %s\n", name, path.c_str());
            };
            preload(opts.llama_model, "llama");
            preload(opts.whisper_model, "whisper");
          }).detach();
        }
      }

      // Prepare new session and wait for CANCEL or Ctrl+C
      callee->ResetConnectionClosedEvent();
      fprintf(stderr, "Callee ready for incoming connections in room %s...\n", room_name.c_str());
      
      while (!g_shutdown) {
        // Check shutdown more frequently with shorter timeout
        if (callee->WaitUntilConnectionClosed(200)) {
          fprintf(stderr, "Callee session #%d ended (connection closed/failed), restarting listener\n", session_count);
          break;
        }
        
        // Check shutdown flag multiple times per second
        if (g_shutdown) {
          fprintf(stderr, "Shutdown requested during callee session #%d\n", session_count);
          break;
        }
      }

      if (!g_shutdown) {
        fprintf(stderr, "Keeping callee listener alive for next connection\n");
        callee->ResetConnectionClosedEvent();
        continue;
      }

      if (g_shutdown && callee) {
        ArmShutdownWatchdog();
        if (callee->InCall()) {
          // Tell the remote side before the process goes away.
          fprintf(stderr, "Shutdown: hanging up active call\n");
          callee->ResetConnectionClosedEvent();
          callee->EndCall("shutting down", /*notify_remote=*/true);
          callee->WaitUntilConnectionClosed(opts.hangup_ack_timeout_ms + 500);
        }
        callee->SignalQuit();
      }

      callee.reset();
      
      if (!g_shutdown) {
        fprintf(stderr, "Preparing to restart callee in 2 seconds...\n");
        // Check for shutdown during sleep as well
        for (int i = 0; i < 20 && !g_shutdown; i++) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }
  }

  if(opts.mode == "caller" or opts.mode == "both") {
    int caller_session_count = 0;
    while (!g_shutdown) {
      caller_session_count++;
      fprintf(stderr, "Starting caller session #%d\n", caller_session_count);
      
      caller = std::make_shared<DirectCallerClient>(opts);
      
      // Set the target user to call
      if (!opts.target_name.empty()) {
        caller->SetTargetUser(opts.target_name);
        fprintf(stderr, "Caller will target user: %s\n", opts.target_name.c_str());
      }
      
      if (!caller->Initialize()) {
        fprintf(stderr, "failed to initialize caller\n");
        return 1;
      }
      if (!caller->Connect()) {
        fprintf(stderr, "failed to connect caller to room %s\n", room_name.c_str());
        return 1;
      }
      caller->RunOnBackgroundThread();
      fprintf(stderr, "Caller connected to room %s\n", room_name.c_str());
      
      // Wait for connection to close or shutdown signal
      while (!g_shutdown) {
        // Check shutdown more frequently with shorter timeout
        if (caller->WaitUntilConnectionClosed(200)) {
          fprintf(stderr, "Caller session #%d ended (connection closed/failed), restarting connection\n", caller_session_count);
          break;
        }
        
        // Check shutdown flag multiple times per second
        if (g_shutdown) {
          fprintf(stderr, "Shutdown requested during caller session #%d\n", caller_session_count);
          break;
        }
      }
      
      if (g_shutdown && caller) {
        ArmShutdownWatchdog();
        fprintf(stderr, "Shutdown: hanging up caller\n");
        caller->ResetConnectionClosedEvent();
        caller->Disconnect();
        // Wait for the callee's 200 OK (or the ack timeout) so BYE is
        // actually delivered before the threads are joined.
        caller->DirectPeer::WaitUntilConnectionClosed(opts.hangup_ack_timeout_ms + 500);
      }

      caller.reset();
      
      if (!g_shutdown) {
        fprintf(stderr, "Preparing to restart caller in 2 seconds...\n");
        // Check for shutdown during sleep as well
        for (int i = 0; i < 20 && !g_shutdown; i++) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }
  }

  // Both callee and caller now have their own loops, so we just wait for shutdown
  if (opts.mode != "caller" && opts.mode != "callee" && opts.mode != "both") {
    while (!g_shutdown) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      
      // Additional shutdown check for stuck scenarios
      if (g_shutdown_count >= 2) {
        fprintf(stderr, "Aggressive shutdown - breaking main loop\n");
        break;
      }
    }
  }

  // Sessions were torn down inside their loops; release anything left.
  fprintf(stderr, "Starting cleanup...\n");
  ArmShutdownWatchdog();
  if (caller) {
    caller->Disconnect();
    caller->DirectPeer::WaitUntilConnectionClosed(opts.hangup_ack_timeout_ms + 500);
    caller.reset();
  }
  if (callee) {
    callee->SignalQuit();
    callee.reset();
  }
  if (ws_proxy) {
    ws_proxy->Stop();
  }

  DirectApplication::rtcCleanup();
  fprintf(stderr, "Shutdown complete\n");
  return 0;
}
