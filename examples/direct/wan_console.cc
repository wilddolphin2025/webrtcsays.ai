// Console CetaBridge WAN (/direct) callee. Registers a nickname, auto-accepts
// one call and keeps the media running until Ctrl-C. Used to test the Android
// WAN client against the same DirectWanClient the iPhone uses.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "wan.h"
#include "option.h"
#include "rtc_base/logging.h"

namespace {

std::string Arg(int argc, char** argv, const char* key, const std::string& fallback) {
  const std::string prefix = std::string("--") + key + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
  }
  return fallback;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string DeviceToken(const std::string& path) {
  std::string token = ReadFile(path);
  while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
  if (token.size() >= 32) return token;
  // One stable token per console install; the server binds it to the nickname.
  token.clear();
  const char* hex = "0123456789abcdef";
  unsigned seed = 0x9e3779b9u;
  const std::string host = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  for (char c : host) seed = seed * 131u + static_cast<unsigned char>(c);
  for (int i = 0; i < 64; ++i) {
    seed = seed * 1103515245u + 12345u;
    token.push_back(hex[(seed >> 16) & 0xF]);
  }
  std::ofstream out(path);
  out << token;
  return token;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string host = Arg(argc, argv, "host", "cb.wilddolphin.us");
  const std::string name = Arg(argc, argv, "name", "Mac-WAN");
  const std::string token = Arg(argc, argv, "token", "");
  const std::string turns = Arg(argc, argv, "turns-file", "");
  const std::string ca = Arg(argc, argv, "ca-file", "");
  const std::string cert = Arg(argc, argv, "cert", "");
  const std::string key = Arg(argc, argv, "key", "");
  const std::string token_path = Arg(argc, argv, "device-token-file", "/tmp/cetacall-wan-device-token");
  if (token.empty()) {
    std::fprintf(stderr, "usage: %s --token=<room token> [--name=Mac-WAN] [--turns-file=...] [--ca-file=...]\n", argv[0]);
    return 2;
  }

  std::string options =
      "--mode=callee --encryption --no-video --no-whisper --no-llama --audio=dummy "
      "--audio-codec=opus --stun=stun:stun.l.google.com:19302";
  if (!turns.empty()) options += " --turns-file=" + turns;
  if (!ca.empty()) options += " --ca-file=" + ca;
  if (!cert.empty()) options += " --webrtc_cert_path=" + cert + " --webrtc_key_path=" + key;

  Options opts = parseOptions(options.c_str());
  if (!opts.parse_error.empty()) {
    std::fprintf(stderr, "options: %s\n", opts.parse_error.c_str());
    return 1;
  }

  DirectWanClient::rtcInitialize();
  DirectSetLoggingLevel(static_cast<LoggingSeverity>(webrtc::LS_INFO));
  DirectWanClient client(opts);

  client.on_registered = []() { std::printf("REGISTERED\n"); std::fflush(stdout); };
  client.on_online = [](const std::string& peer) { std::printf("ONLINE %s\n", peer.c_str()); };
  client.on_offline = [](const std::string& peer) { std::printf("OFFLINE %s\n", peer.c_str()); };
  client.on_incoming = [&client](const std::string& id, const std::string& peer) {
    std::printf("INCOMING %s from %s -> ACCEPT\n", id.c_str(), peer.c_str());
    std::fflush(stdout);
    client.Accept();
  };
  client.on_ringing = [](const std::string& id, const std::string& peer) {
    std::printf("RINGING %s %s\n", id.c_str(), peer.c_str());
  };
  client.on_accepted = [](const std::string& id, const std::string& peer) {
    std::printf("ACCEPTED %s %s\n", id.c_str(), peer.c_str());
  };
  client.on_media_connected = []() { std::printf("MEDIA CONNECTED\n"); std::fflush(stdout); };
  client.on_rejected = [](const std::string& id) { std::printf("REJECTED %s\n", id.c_str()); };
  client.on_remote_hangup = [](const std::string& id) { std::printf("REMOTE HANGUP %s\n", id.c_str()); };
  client.on_timeout = [](const std::string& id) { std::printf("TIMEOUT %s\n", id.c_str()); };
  client.on_replaced = []() { std::printf("REPLACED\n"); };
  client.on_error = [](const std::string& why) { std::printf("ERROR %s\n", why.c_str()); };
  client.on_closed = [](const std::string& why) { std::printf("CLOSED %s\n", why.c_str()); };
  client.on_call_ended = [](const std::string& id, bool remote) {
    std::printf("CALL ENDED %s remote=%d\n", id.c_str(), remote ? 1 : 0);
    std::fflush(stdout);
  };

  if (!client.Initialize()) {
    std::fprintf(stderr, "Initialize failed\n");
    return 1;
  }
  if (!client.Connect(host, 443, "/direct", token)) {
    std::fprintf(stderr, "Connect failed\n");
    return 1;
  }
  const std::string device = DeviceToken(token_path);
  if (!client.Register(name, device)) {
    std::fprintf(stderr, "Register failed\n");
    return 1;
  }
  std::printf("REGISTERING %s on wss://%s/direct\n", name.c_str(), host.c_str());
  std::fflush(stdout);

  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
