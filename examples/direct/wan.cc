#include "wan.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>
#include <vector>

#include "json/json.h"
#include "rtc_base/logging.h"
#include "wsock.h"

namespace {

std::string Trim(std::string s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> Tokens(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream in(line);
  std::string t;
  while (in >> t) out.push_back(t);
  return out;
}

bool ParseJsonObject(const std::string& text, Json::Value* root) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errors;
  return reader->parse(text.data(), text.data() + text.size(), root, &errors);
}

std::string WriteJson(const Json::Value& root) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, root);
}

bool HasPrefix(const std::string& s, const char* prefix) {
  const size_t n = std::strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

}  // namespace

DirectWanClient::DirectWanClient(const Options& opts) : DirectPeer(opts) {}

DirectWanClient::~DirectWanClient() {
  CloseSocket();
  ws_.reset();
  if (ws_thread_) {
    ws_thread_->Stop();
    ws_thread_.reset();
  }
  Cleanup();
}

bool DirectWanClient::Connect(const std::string& host, int port,
                              const std::string& path,
                              const std::string& bearer_token) {
  CloseSocket();
  if (!ws_thread_) {
    ws_thread_ = webrtc::Thread::Create();
    if (!ws_thread_ || !ws_thread_->Start()) {
      ws_thread_.reset();
      RTC_LOG(LS_ERROR) << "WAN: could not start the WebSocket thread";
      return false;
    }
  }
  ws_ = std::make_shared<WebSocketClient>();
  ws_->set_network_thread(ws_thread_.get());
  ws_->set_allow_reconnect(true);
  ws_->set_reconnect_callback([this]() { OnReconnected(); });
  ws_->set_message_callback([this](const std::string& message) {
    OnWsMessage(message);
  });

  WebSocketClient::Config cfg;
  cfg.host = host;
  cfg.port = std::to_string(port);
  cfg.use_ssl = true;
  cfg.verify_tls = true;
  cfg.ca_file = opts_.ca_file;
  cfg.headers["path"] = path;
  if (!bearer_token.empty()) {
    cfg.headers["Authorization"] = "Bearer " + bearer_token;
  }
  if (!ws_->connect(cfg)) {
    RTC_LOG(LS_ERROR) << "WAN: WebSocket connect failed";
    return false;
  }
  if (!ws_->send_websocket_handshake_with_headers()) {
    RTC_LOG(LS_ERROR) << "WAN: WebSocket handshake failed";
    return false;
  }
  ws_->start_listening();
  return true;
}

bool DirectWanClient::Register(const std::string& nickname,
                               const std::string& device_token) {
  if (nickname.empty() || device_token.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    nickname_ = nickname;
    device_token_ = device_token;
  }
  SendRegister();
  return true;
}

void DirectWanClient::SendRegister() {
  std::string nickname;
  std::string token;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    nickname = nickname_;
    token = device_token_;
  }
  if (nickname.empty() || token.empty()) return;
  // Never log the device token.
  SendFrame("REGISTER " + nickname + " " + token);
}

bool DirectWanClient::Lookup(const std::string& nickname) {
  if (nickname.empty()) return false;
  SendFrame("LOOKUP " + nickname);
  return true;
}

bool DirectWanClient::Search(const std::string& prefix) {
  const std::string query = Trim(prefix);
  if (query.empty()) return false;
  SendFrame("SEARCH " + query);
  return true;
}

bool DirectWanClient::SendChatFrame(const std::string& frame) {
  if (frame.empty()) return false;
  auto ws = ws_;
  if (!ws || !ws->is_connected()) return false;
  ++chat_outstanding_;
  ws->send_message(frame);
  return true;
}

bool DirectWanClient::Call(const std::string& nickname) {
  if (nickname.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    caller_ = true;
    peer_name_ = nickname;
    call_id_.clear();
  }
  opts_.mode = "caller";
  SendFrame("CALL " + nickname);
  return true;
}

bool DirectWanClient::Accept() {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    id = call_id_;
    caller_ = false;
  }
  if (id.empty()) return false;
  opts_.mode = "callee";
  SendFrame("ACCEPT " + id);
  return true;
}

bool DirectWanClient::Reject() {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    id = call_id_;
  }
  if (id.empty()) return false;
  SendFrame("REJECT " + id);
  ResetCall();
  if (on_rejected) on_rejected(id);
  return true;
}

bool DirectWanClient::Hangup() {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    id = call_id_;
  }
  if (id.empty()) {
    if (InCall()) EndCall("local hangup", /*notify_remote=*/false);
    return false;
  }
  SendFrame("HANGUP " + id);
  if (InCall()) {
    EndCall("local hangup", /*notify_remote=*/false);
  } else {
    ResetCall();
    if (on_call_ended) on_call_ended(id, false);
  }
  return true;
}

void DirectWanClient::Disconnect() {
  CloseSocket();
}

bool DirectWanClient::IsRegistered() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return registered_;
}

bool DirectWanClient::IsCaller() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return caller_;
}

std::string DirectWanClient::call_id() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return call_id_;
}

std::string DirectWanClient::peer_name() const {
  std::lock_guard<std::mutex> lock(state_mu_);
  return peer_name_;
}

void DirectWanClient::SendFrame(const std::string& frame) {
  auto ws = ws_;
  if (!ws || !ws->is_connected()) return;
  ws->send_message(frame);
}

void DirectWanClient::SendJson(const char* verb, const std::string& sdp) {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    id = call_id_;
  }
  if (id.empty()) return;
  Json::Value root;
  root["call_id"] = id;
  root["sdp"] = sdp;
  SendFrame(std::string(verb) + " " + WriteJson(root));
}

void DirectWanClient::SendIceJson(const std::string& candidate, int mline_index) {
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    id = call_id_;
  }
  if (id.empty()) return;
  std::fprintf(stderr, "[wan] local ice %d %s\n", mline_index, candidate.c_str());
  Json::Value root;
  root["call_id"] = id;
  root["candidate"] = candidate;
  root["sdpMid"] = "0";
  root["sdpMLineIndex"] = mline_index;
  SendFrame("ICE " + WriteJson(root));
}

bool DirectWanClient::SendMessage(const std::string& message) {
  if (HasPrefix(message, "OFFER:")) {
    SendJson("OFFER", message.substr(6));
    return true;
  }
  if (HasPrefix(message, "ANSWER:")) {
    SendJson("ANSWER", message.substr(7));
    return true;
  }
  if (HasPrefix(message, "ICE:")) {
    const std::string rest = message.substr(4);
    const size_t colon = rest.find(':');
    if (colon != std::string::npos) {
      int mline = std::atoi(rest.substr(0, colon).c_str());
      SendIceJson(rest.substr(colon + 1), mline);
    }
    return true;
  }
  if (message == "BYE" || message == "CANCEL") {
    std::string id;
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      id = call_id_;
    }
    if (!id.empty()) SendFrame("HANGUP " + id);
    return true;
  }
  // The /direct server has no HELLO/WAITING/200 OK dialogue for media.
  return true;
}

void DirectWanClient::StartCallerOffer() {
  opts_.mode = "caller";
  auto start = [this]() {
    // Never offer from a PeerConnection left over from the previous call:
    // its ICE ufrag and failed candidate pairs are stale.
    if (peer_connection_) ShutdownInternal();
    signaling_thread()->PostTask([this]() { this->Start(); });
  };
  if (main_thread()) {
    main_thread()->PostTask(start);
  } else {
    start();
  }
}

void DirectWanClient::HandleOfferSdp(const std::string& sdp) {
  opts_.mode = "callee";
  auto apply = [this, sdp]() {
    // A WAN offer starts a fresh call. Reusing the previous PeerConnection
    // makes the answer keep a stale ICE ufrag, so Android rejects its STUN
    // checks and ICE fails. Tear it down and start a new call generation.
    if (peer_connection_) ShutdownInternal();
    BeginCall();
    this->HandleMessage(nullptr, "OFFER:" + sdp, webrtc::SocketAddress());
  };
  if (main_thread()) {
    main_thread()->PostTask(apply);
  } else {
    apply();
  }
}

void DirectWanClient::HandleAnswerSdp(const std::string& sdp) {
  opts_.mode = "caller";
  this->HandleMessage(nullptr, "ANSWER:" + sdp, webrtc::SocketAddress());
}

void DirectWanClient::HandleIce(const std::string& candidate, int mline_index) {
  std::fprintf(stderr, "[wan] remote ice %d %s\n", mline_index, candidate.c_str());
  this->HandleMessage(nullptr,
                      "ICE:" + std::to_string(mline_index) + ":" + candidate,
                      webrtc::SocketAddress());
}

void DirectWanClient::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  std::fprintf(stderr, "[wan] media state %d\n", static_cast<int>(new_state));
  DirectPeer::OnConnectionChange(new_state);
  if (new_state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected &&
      on_media_connected) {
    on_media_connected();
  }
}

void DirectWanClient::OnCallEnded(bool notified_remote) {
  DirectPeer::OnCallEnded(notified_remote);
  std::string id;
  {
    std::lock_guard<std::mutex> lock(state_mu_);
    id = call_id_;
  }
  if (on_call_ended) on_call_ended(id, notified_remote);
  ResetCall();
}

void DirectWanClient::ResetCall() {
  std::lock_guard<std::mutex> lock(state_mu_);
  call_id_.clear();
  peer_name_.clear();
  caller_ = false;
}

void DirectWanClient::CloseSocket() {
  auto ws = ws_;
  if (!ws) return;
  ws->set_allow_reconnect(false);
  ws->set_reconnect_callback(nullptr);
  ws->disconnect();
  std::lock_guard<std::mutex> lock(state_mu_);
  registered_ = false;
}

void DirectWanClient::OnReconnected() {
  SendRegister();
}

void DirectWanClient::OnWsMessage(const std::string& raw) {
  const std::string frame = Trim(raw);
  if (frame.empty()) return;

  // The WebSocket client reports a dropped link (and a failed reconnect)
  // through the same message callback. Tell the app so it can clear the stale
  // client and re-register on the new interface instead of showing a dead
  // "Registered" state after Wi-Fi -> LTE.
  if (frame == "DISCONNECTED" || frame == "RECONNECT_FAILED") {
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      registered_ = false;
    }
    if (on_closed) on_closed(frame);
    return;
  }

  // Chat v0.1 replies and errors. Keep them away from call signaling; a
  // chat error must never close the /direct socket.
  const bool chat_reply =
      HasPrefix(frame, "CAPS OK") || frame == "IDENTITY OK" ||
      HasPrefix(frame, "IDENTITY-RESP ") || HasPrefix(frame, "MSG ") ||
      HasPrefix(frame, "ACK ") || HasPrefix(frame, "MSGS ") ||
      HasPrefix(frame, "STATE ") || frame == "PUSH OK";
  const bool chat_error =
      HasPrefix(frame, "404 Unknown peer") ||
      HasPrefix(frame, "409 IDENTITY CONFLICT") ||
      HasPrefix(frame, "413 Message Too Long") ||
      HasPrefix(frame, "503 Chat Unavailable") ||
      HasPrefix(frame, "503 Inbox Full") ||
      HasPrefix(frame, "480 Temporarily Unavailable");
  const bool chat_bad_request = HasPrefix(frame, "400 Bad Request");
  // A chat 400 (for example a bad SYNC cursor) must never tear down an active
  // call. While a call is up, route 400 to the chat callback only.
  if (chat_reply || chat_error ||
      (chat_bad_request && (chat_outstanding_.load() > 0 || InCall()))) {
    if (chat_outstanding_.load() > 0) --chat_outstanding_;
    if (on_chat_frame) on_chat_frame(frame);
    return;
  }

  if (HasPrefix(frame, "OFFER ")) {
    Json::Value root;
    if (ParseJsonObject(frame.substr(6), &root) && root["sdp"].isString()) {
      HandleOfferSdp(root["sdp"].asString());
    }
    return;
  }
  if (HasPrefix(frame, "ANSWER ")) {
    Json::Value root;
    if (ParseJsonObject(frame.substr(7), &root) && root["sdp"].isString()) {
      HandleAnswerSdp(root["sdp"].asString());
    }
    return;
  }
  if (HasPrefix(frame, "ICE ")) {
    Json::Value root;
    if (ParseJsonObject(frame.substr(4), &root) && root["candidate"].isString()) {
      int mline = root["sdpMLineIndex"].isInt() ? root["sdpMLineIndex"].asInt() : 0;
      HandleIce(root["candidate"].asString(), mline);
    }
    return;
  }

  if (frame == "200 OK") {
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      registered_ = true;
    }
    if (on_registered) on_registered();
    return;
  }
  if (HasPrefix(frame, "RESULTS")) {
    const std::string body = Trim(frame.substr(std::strlen("RESULTS")));
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= body.size()) {
      const size_t comma = body.find(',', start);
      std::string one = Trim(body.substr(start, comma == std::string::npos
                                                   ? std::string::npos
                                                   : comma - start));
      if (!one.empty()) names.push_back(std::move(one));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (on_results) on_results(names);
    return;
  }
  if (HasPrefix(frame, "ONLINE ")) {
    if (on_online) on_online(Trim(frame.substr(7)));
    return;
  }
  if (HasPrefix(frame, "OFFLINE ")) {
    if (on_offline) on_offline(Trim(frame.substr(8)));
    return;
  }
  if (HasPrefix(frame, "RINGING ")) {
    std::vector<std::string> t = Tokens(frame);
    if (t.size() >= 3) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        call_id_ = t[1];
        peer_name_ = t[2];
        caller_ = true;
      }
      if (on_ringing) on_ringing(t[1], t[2]);
    }
    return;
  }
  if (HasPrefix(frame, "INCOMING ")) {
    std::vector<std::string> t = Tokens(frame);
    if (t.size() >= 3) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        call_id_ = t[1];
        peer_name_ = t[2];
        caller_ = false;
      }
      if (on_incoming) on_incoming(t[1], t[2]);
    }
    return;
  }
  if (HasPrefix(frame, "ACCEPTED ")) {
    std::vector<std::string> t = Tokens(frame);
    if (t.size() >= 3) {
      {
        std::lock_guard<std::mutex> lock(state_mu_);
        call_id_ = t[1];
        peer_name_ = t[2];
        caller_ = true;
      }
      if (on_accepted) on_accepted(t[1], t[2]);
      StartCallerOffer();
    }
    return;
  }
  if (HasPrefix(frame, "REJECTED ")) {
    std::vector<std::string> t = Tokens(frame);
    const std::string id = t.size() >= 2 ? t[1] : "";
    if (on_rejected) on_rejected(id);
    ResetCall();
    return;
  }
  if (HasPrefix(frame, "HANGUP ")) {
    std::vector<std::string> t = Tokens(frame);
    const std::string id = t.size() >= 2 ? t[1] : "";
    const bool active = InCall();
    if (active) {
      EndCall("remote hangup", /*notify_remote=*/false);
    } else {
      ResetCall();
      if (on_call_ended) on_call_ended(id, true);
    }
    if (on_remote_hangup) on_remote_hangup(id);
    return;
  }
  if (HasPrefix(frame, "TIMEOUT ")) {
    std::vector<std::string> t = Tokens(frame);
    const std::string id = t.size() >= 2 ? t[1] : "";
    if (InCall()) EndCall("ring timeout", /*notify_remote=*/false);
    else {
      ResetCall();
      if (on_call_ended) on_call_ended(id, false);
    }
    if (on_timeout) on_timeout(id);
    return;
  }
  if (frame == "REPLACED") {
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      registered_ = false;
    }
    if (on_replaced) on_replaced();
    if (on_closed) on_closed(frame);
    CloseSocket();
    return;
  }

  const bool fatal =
      HasPrefix(frame, "409 ") ||
      HasPrefix(frame, "400 Nickname Required") ||
      HasPrefix(frame, "400 Invalid Nickname") ||
      HasPrefix(frame, "400 Device Token Required") ||
      HasPrefix(frame, "400 Invalid Device Token") ||
      HasPrefix(frame, "480 Temporarily Unavailable");
  if (fatal) {
    {
      std::lock_guard<std::mutex> lock(state_mu_);
      registered_ = false;
    }
    if (on_error) on_error(frame);
    if (on_closed) on_closed(frame);
    CloseSocket();
    return;
  }
  if (HasPrefix(frame, "480 Offline") || HasPrefix(frame, "486 ") ||
      HasPrefix(frame, "400 Bad Request") || HasPrefix(frame, "429") ||
      HasPrefix(frame, "400 ")) {
    if (on_error) on_error(frame);
    return;
  }
  RTC_LOG(LS_INFO) << "WAN: ignoring frame";
}
