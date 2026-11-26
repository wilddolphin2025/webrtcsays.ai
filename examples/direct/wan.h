/*
 * CetaBridge WAN direct-call client (protocol v0.1).
 *
 * One persistent WebSocket to wss://<host>/direct. CetaBridge only relays
 * signaling; WebRTC media stays peer-to-peer. This class reuses DirectPeer's
 * existing CETACALL/Opus offer/answer and audio setup and replaces the TCP
 * signaling with REGISTER/LOOKUP/CALL/ACCEPT/REJECT/HANGUP plus JSON
 * OFFER/ANSWER/ICE frames.
 */
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "direct.h"

class WebSocketClient;

class DIRECT_API DirectWanClient : public DirectPeer {
 public:
  explicit DirectWanClient(const Options& opts);
  ~DirectWanClient() override;

  /// Opens wss://host:port/path with Authorization: Bearer <token>.
  bool Connect(const std::string& host, int port, const std::string& path,
               const std::string& bearer_token);
  /// Sends REGISTER <nickname> <device_token>. The device token is never logged.
  bool Register(const std::string& nickname, const std::string& device_token);
  bool Lookup(const std::string& nickname);
  bool Call(const std::string& nickname);
  bool Accept();
  bool Reject();
  bool Hangup();
  /// Closes the /direct socket and stops reconnecting. The object stays alive.
  void Disconnect();

  bool IsRegistered() const;
  bool IsCaller() const;
  std::string call_id() const;
  std::string peer_name() const;

  // Signaling callbacks. They run on the framework's network/signaling
  // thread; the app must hop to the main queue before touching UIKit.
  std::function<void()> on_registered;
  std::function<void(const std::string&)> on_online;
  std::function<void(const std::string&)> on_offline;
  std::function<void(const std::string& call_id, const std::string& callee)> on_ringing;
  std::function<void(const std::string& call_id, const std::string& caller)> on_incoming;
  std::function<void(const std::string& call_id, const std::string& callee)> on_accepted;
  std::function<void(const std::string& call_id)> on_rejected;
  std::function<void(const std::string& call_id)> on_remote_hangup;
  std::function<void(const std::string& call_id)> on_timeout;
  std::function<void()> on_replaced;
  std::function<void(const std::string&)> on_error;
  std::function<void(const std::string&)> on_closed;
  std::function<void()> on_media_connected;
  std::function<void(const std::string& call_id, bool remote)> on_call_ended;

  // DirectPeer overrides.
  bool SendMessage(const std::string& message) override;
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

 protected:
  void OnCallEnded(bool notified_remote) override;

 private:
  void OnWsMessage(const std::string& frame);
  void OnReconnected();
  void SendRegister();
  void StartCallerOffer();
  void HandleOfferSdp(const std::string& sdp);
  void HandleAnswerSdp(const std::string& sdp);
  void HandleIce(const std::string& candidate, int mline_index);
  void SendJson(const char* verb, const std::string& sdp);
  void SendIceJson(const std::string& candidate, int mline_index);
  void SendFrame(const std::string& frame);
  void ResetCall();
  void CloseSocket();

  std::shared_ptr<WebSocketClient> ws_;
  mutable std::mutex state_mu_;
  bool registered_ = false;
  bool caller_ = false;
  std::string nickname_;
  std::string device_token_;
  std::string call_id_;
  std::string peer_name_;
};
