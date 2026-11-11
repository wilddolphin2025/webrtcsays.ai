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

#include <arpa/inet.h>
#include <errno.h>  // For errno
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>  // For close

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>  // For memset
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "direct.h"
#include "signaling_bind.h"
#include "status.h"
#include "parser.h"

#if TARGET_OS_IOS || TARGET_OS_OSX
#include "bonjour.h"
#endif // #if TARGET_OS_IOS || TARGET_OS_OSX

#if defined(__APPLE__) && !defined(SO_REUSEPORT)
#define SO_REUSEPORT 0x0200
#endif

using webrtc::PeerConnectionInterface;

// DirectCallee Implementation
DirectCallee::DirectCallee(Options opts) : DirectPeer(opts) {
  std::string ip;
  local_port_ = 0;
  ParseIpAndPort(opts.address, ip, local_port_);
}

DirectCallee::~DirectCallee() {
  Cleanup();
}

bool DirectCallee::StartListening() {
  // Core listening setup logic as a callable
  auto listen_fn = [this]() -> bool {
    // Resolve once. A bad signaling_bind must not fall back to every interface.
    in_addr_t bind_addr = htonl(INADDR_ANY);
    std::string bind_error;
    if (!ResolveSignalingBind(opts_.signaling_bind, &bind_addr, &bind_error)) {
      RTC_LOG(LS_ERROR) << "signaling bind refused: " << bind_error;
      fprintf(stderr, "signaling bind refused: %s\n", bind_error.c_str());
      return false;
    }
    const int kMaxRetries = 10;
    const int kMaxRetryTimeMs = 30000; // 30 seconds total
    const int kInitialDelayMs = 100;   // Start with 100ms delay
    
    auto start_time = std::chrono::steady_clock::now();
    
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      // Check if we've exceeded the total timeout
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_time).count();
      if (elapsed >= kMaxRetryTimeMs) {
        RTC_LOG(LS_ERROR) << "Binding timeout exceeded (" << kMaxRetryTimeMs << "ms). Giving up.";
        break;
      }
      
      // Create raw socket
      int raw_socket = ::socket(AF_INET, SOCK_STREAM, 0);
      if (raw_socket < 0) {
        RTC_LOG(LS_ERROR) << "Failed to create socket, errno: "
                          << strerror(errno);
        return false;
      }

      // Set SO_REUSEADDR to allow address reuse
      int reuse = 1;
      if (::setsockopt(raw_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        RTC_LOG(LS_WARNING) << "Failed to set SO_REUSEADDR, errno: " << strerror(errno);
        // Continue anyway, this is not fatal
      }

#ifdef SO_REUSEPORT
      // macOS/IPV4 requires SO_REUSEPORT together with SO_REUSEADDR to rebind
      // to a port that is still in TIME_WAIT. This allows the listener to
      // restart immediately on the same port after a previous connection.
      if (::setsockopt(raw_socket, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        RTC_LOG(LS_WARNING) << "Failed to set SO_REUSEPORT, errno: " << strerror(errno);
      }
#endif

      // signaling_bind was resolved above. Empty keeps INADDR_ANY.
      // This socket is TCP signaling only; media ICE is unaffected.
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(local_port_);
      addr.sin_addr.s_addr = bind_addr;

      // Attempt to bind
      if (::bind(raw_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        int bind_errno = errno;
        RTC_LOG(LS_WARNING) << "Bind attempt " << (attempt + 1) << "/" << kMaxRetries 
                           << " failed, errno: " << strerror(bind_errno) 
                           << " (port " << local_port_ << ")";
        ::close(raw_socket);
        
        // If not "Address already in use", don't retry
        if (bind_errno != EADDRINUSE) {
          RTC_LOG(LS_ERROR) << "Non-recoverable bind error: " << strerror(bind_errno);
          return false;
        }
        
        // Calculate delay with exponential backoff (100ms, 200ms, 400ms, 800ms, 1600ms, then cap at 2000ms)
        int delay_ms = kInitialDelayMs * (1 << std::min(attempt, 4));
        delay_ms = std::min(delay_ms, 2000); // Cap at 2 seconds
        
        // Make sure we don't exceed total timeout
        if (elapsed + delay_ms >= kMaxRetryTimeMs) {
          delay_ms = kMaxRetryTimeMs - elapsed;
          if (delay_ms <= 0) {
            RTC_LOG(LS_ERROR) << "No time left for retry delay. Giving up.";
            break;
          }
        }
        
        RTC_LOG(LS_INFO) << "Retrying bind in " << delay_ms << "ms...";
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        continue;
      }
      
      // Bind succeeded, continue with socket setup
      char shown[INET_ADDRSTRLEN] = {};
      if (!inet_ntop(AF_INET, &addr.sin_addr, shown, sizeof(shown))) {
        std::snprintf(shown, sizeof(shown), "?");
      }
      fprintf(stderr, "Signaling TCP listening on %s:%d\n", shown, local_port_);
      RTC_LOG(LS_INFO) << "Bind succeeded on attempt " << (attempt + 1)
                       << " (" << shown << ":" << local_port_ << ")";

      // Retrieve the actual port assigned by the OS if port was 0
      socklen_t addrlen = sizeof(addr);
      if (getsockname(raw_socket, (struct sockaddr*)&addr, &addrlen) == 0) {
          local_port_ = ntohs(addr.sin_port);
      }

      // Listen
      if (::listen(raw_socket, 5) < 0) {
        RTC_LOG(LS_ERROR) << "Failed to listen, errno: " << strerror(errno);
        ::close(raw_socket);
        return false;
      }

      // Wrap the listening socket
      auto wrapped_socket = pss()->WrapSocket(raw_socket);
      if (!wrapped_socket) {
        RTC_LOG(LS_ERROR) << "Failed to wrap socket";
        ::close(raw_socket);
        return false;
      }

      listen_socket_ = std::make_unique<webrtc::AsyncTcpListenSocket>(
          *env_,
          std::unique_ptr<webrtc::Socket>(wrapped_socket));
      listen_socket_->SubscribeNewConnection(
          this, [this](webrtc::AsyncListenSocket* ls, webrtc::AsyncPacketSocket* s) {
            OnNewConnection(ls, s);
          });

      RTC_LOG(LS_INFO) << "Server listening on port " << local_port_;

      #if TARGET_OS_IOS || TARGET_OS_OSX
      // Bonjour advertisement
      if (opts_.bonjour && !opts_.bonjour_name.empty()) {
          if (AdvertiseBonjourService(opts_.bonjour_name, local_port_)) {
              RTC_LOG(LS_INFO) << "Bonjour advertised as '" << opts_.bonjour_name << "' on port " << local_port_;
          } else {
              RTC_LOG(LS_WARNING) << "Bonjour advertisement failed for '" << opts_.bonjour_name << "'";
          }
      }
      #endif // #if TARGET_OS_IOS || TARGET_OS_OSX
      return true;
    } // End of retry loop
    
    // If we get here, all retry attempts failed
    RTC_LOG(LS_ERROR) << "Failed to bind after " << kMaxRetries << " attempts within " 
                      << kMaxRetryTimeMs << "ms timeout. Giving up.";
    return false;
  };
  // If already on network thread, run directly to avoid BlockingCall DCHECK
  if (network_thread()->IsCurrent()) {
    return listen_fn();
  }
  return network_thread()->BlockingCall(std::move(listen_fn));
}

void DirectCallee::OnNewConnection(webrtc::AsyncListenSocket* listen_socket,
                                   webrtc::AsyncPacketSocket* new_socket) {
  if (!new_socket) {
    RTC_LOG(LS_ERROR) << "Callee: new socket is null";
    return;
  }
  // We own the accepted socket from here on.
  std::unique_ptr<webrtc::AsyncTCPSocket> client(
      static_cast<webrtc::AsyncTCPSocket*>(new_socket));
  RTC_LOG(LS_INFO) << "Connection accepted from "
                   << client->GetRemoteAddress().ToString();

  // One call at a time. tcp_socket_ is only cleared after the previous call's
  // media is fully released, so this also covers the teardown window.
  if (InCall() || tcp_socket_) {
    RTC_LOG(LS_INFO) << "Callee busy, rejecting connection";
    std::string busy_msg = StatusCodes::kBusyHere;
    client->Send(busy_msg.c_str(), busy_msg.size(),
                 webrtc::AsyncSocketPacketOptions());
    client->Close();
    return;
  }

  // Detect half-open signaling connections (peer vanished without FIN):
  // probe after 15 s idle, give up after 3 x 5 s.
  client->SetOption(webrtc::Socket::OPT_KEEPALIVE, 1);
  client->SetOption(webrtc::Socket::OPT_TCP_KEEPIDLE, 15);
  client->SetOption(webrtc::Socket::OPT_TCP_KEEPINTVL, 5);
  client->SetOption(webrtc::Socket::OPT_TCP_KEEPCNT, 3);

  webrtc::AsyncTCPSocket* raw = client.get();
  tcp_socket_ = std::move(client);

  raw->RegisterReceivedPacketCallback(
      [this, raw](webrtc::AsyncPacketSocket* socket,
                  const webrtc::ReceivedIpPacket& packet) {
        if (socket != raw) {
          return;
        }
        OnMessage(raw,
                  reinterpret_cast<const unsigned char*>(packet.payload().data()),
                  packet.payload().size(), packet.source_address());
      });
  raw->SubscribeCloseEvent(
      this, [this](webrtc::AsyncPacketSocket* socket, int err) {
        RTC_LOG(LS_INFO) << "Signaling socket closed, err=" << err;
        OnClose(socket);
      });
}

void DirectCallee::OnMessage(webrtc::AsyncPacketSocket* socket,
                             const unsigned char* data,
                             size_t len,
                             const webrtc::SocketAddress& remote_addr) {
  std::string message(reinterpret_cast<const char*>(data), len);
  RTC_LOG(LS_INFO) << "Callee received: " << message.substr(0, 120);

  if (message.rfind(Msg::kHello, 0) == 0) {
    SendMessage(InCall() ? StatusCodes::kBusyHere : StatusCodes::kOk);
  } else if (message.rfind(Msg::kBye, 0) == 0) {
    SendMessage(StatusCodes::kOk);
    if (InCall()) {
      EndCall("remote BYE", /*notify_remote=*/false);
    } else {
      OnCallEnded(false);
    }
  } else if (message.rfind(Msg::kCancel, 0) == 0) {
    RTC_LOG(LS_INFO) << "Received CANCEL from " << remote_addr.ToString();
    OnCancel(socket);
  } else if (message.rfind(StatusCodes::kOk, 0) == 0) {
    // Acknowledgement of our own BYE; the socket is already being closed.
    RTC_LOG(LS_INFO) << "Peer acknowledged hangup";
  } else {
    std::string cmd;
    Json::Value params;
    if (!ParseMessageLine(message, cmd, params)) {
      SendMessage(StatusCodes::kBadRequest);
    }
    HandleMessage(socket, message, remote_addr);
  }
}

void DirectCallee::OnCancel(webrtc::AsyncPacketSocket* socket) {
  if (socket != tcp_socket_.get()) {
    RTC_LOG(LS_WARNING) << "Received CANCEL on an unexpected socket.";
    return;
  }
  ResetCallStartedFlag();
  // Existing callers expect BYE (not 200 OK) as the CANCEL acknowledgement.
  SendMessage(Msg::kBye);
  if (InCall()) {
    EndCall("remote CANCEL", /*notify_remote=*/false);
  } else {
    OnCallEnded(false);
  }
}

void DirectCallee::OnCallEnded(bool notified_remote) {
  // Runs on main after media is released. Closing on the network thread
  // after the BYE / 200 OK that is already queued there flushes it first.
  network_thread()->PostTask([this] {
    if (tcp_socket_) {
      tcp_socket_->UnsubscribeCloseEvent(this);
      tcp_socket_->Close();
      tcp_socket_.reset();
    }
    connection_closed_event_.Set();
  });
}

void DirectCallee::OnClose(webrtc::AsyncPacketSocket* socket) {
  // Network thread. Destroying the socket inside its own close callback is
  // unsafe, so defer the reset.
  network_thread()->PostTask([this, socket] {
    if (tcp_socket_.get() == socket) {
      tcp_socket_.reset();
    }
    if (!InCall()) {
      connection_closed_event_.Set();
    }
  });
  if (InCall()) {
    EndCall("signaling connection lost", /*notify_remote=*/false);
  }
}

bool DirectCallee::SendMessage(const std::string& message) {
  return DirectPeer::SendMessage(message);
}

void DirectCallee::OnIceConnectionChange(
    PeerConnectionInterface::IceConnectionState new_state) {
  DirectPeer::OnIceConnectionChange(new_state);
}

void DirectCallee::ShutdownInternal() {
  DirectPeer::ShutdownInternal();
}
