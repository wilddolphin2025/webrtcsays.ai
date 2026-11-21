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

#include "rtc_base/network.h"
#include "rtc_base/ip_address.h"
#include <unistd.h> // For usleep
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <atomic>
#include <map>
#include <sstream>
#include <memory>
#include <cstdlib>
#include "rtc_base/time_utils.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"

#include "direct.h"
#include "option.h"
#include "video.h"
#include "status.h"

// String split from option.cc
std::vector<std::string> stringSplit(std::string input, std::string delimiter);

void DirectApplication::rtcInitialize() {
  webrtc::InitializeSSL();
}

void DirectApplication::rtcCleanup() {
  webrtc::CleanupSSL();
}

webrtc::IPAddress IPFromString(absl::string_view str) {
  webrtc::IPAddress ip;
  RTC_CHECK(webrtc::IPFromString(str, &ip));
  return ip;
}

#if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)
WhillatsLlama* DirectApplication::llama_ = nullptr;

void llamaCallback(bool success, const char* response, void* user_data) {
  if (success && response && *response && user_data) {
    DirectApplication* app = static_cast<DirectApplication*>(user_data);
    if(app) {
      // Safely handle the language string
      std::string language = webrtc::SpeechAudioDeviceFactory::GetLanguage();
      if (language.empty()) {
        language = "en";  // Default to English if empty
      }
      
      if (app->remote_agent() == "text-only") {
        if (app->IsConnected()) {
          std::string message;
          message.append("LLAMA:[").append(language).append("]").append(response);
          app->SendMessage(message);
        }
      } else if(app->remote_agent() == "audio") {
        webrtc::SpeechAudioDeviceFactory::SpeakText(response, language);
        if (app->IsConnected()) {
          std::string message;
          message.append("LLAMA:[").append(language).append("]").append(response);
          app->SendMessage(message);
        }
      } else {
        RTC_LOG(LS_WARNING) << "Llama notification arrived but peer is no longer connected. Dropping message: [" << language << "]" << response;
      }
    }
  }
}
#endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED

// DirectApplication Implementation
DirectApplication::DirectApplication(Options opts)
  : opts_(opts)
#if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)
    , llamaCallback_(llamaCallback, this)
#endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED
{
  // Threads will be created in Initialize() to support full teardown/re-init
  peer_connection_factory_ = nullptr;
}

void DirectApplication::StopThreads() {
  // Thread::Stop() joins, so it must never run on the thread being stopped.
  RTC_CHECK(!IsOwnedThread(webrtc::Thread::Current()));
  // Main first: it is the one that posts work to the other two.
  for (auto* th : {&main_thread_, &signaling_thread_, &network_thread_}) {
    if (*th) {
      (*th)->Stop();
      th->reset();
    }
  }
}

void DirectApplication::ReleaseCallResources() {
  // PeerConnection::Close() and the factory destructor block on the network
  // thread, so this must not run there.
  RTC_DCHECK(!network_thread() || !network_thread()->IsCurrent());

#if defined(WEBRTC_SPEECH_DEVICES) && LLAMA_NOTIFICATION_ENABLED
  if (opts_.llama && llama_) {
    RTC_LOG(LS_INFO) << "Stopping Llama device for ended call";
    llama_->stop();
    llama_ = nullptr;
  }
#endif

  RunOn(signaling_thread(), [this] {
    if (video_sink_) {
      if (video_track_) video_track_->RemoveSink(video_sink_.get());
      if (remote_video_track_) remote_video_track_->RemoveSink(video_sink_.get());
    }
    remote_video_track_ = nullptr;
    video_track_ = nullptr;
    if (peer_connection_) {
      peer_connection_->Close();
      peer_connection_ = nullptr;
    }
    echo_sources_.clear();
    // A fresh factory (and ADM) per call keeps the speech device state clean.
    peer_connection_factory_ = nullptr;
    network_manager_ = nullptr;
  });

  RunOn(worker_thread(), [this] {
    if (audio_device_module_) {
      audio_device_module_->StopPlayout();
      audio_device_module_->StopRecording();
      audio_device_module_->Terminate();
      audio_device_module_ = nullptr;
    }
  });
}

void DirectApplication::Cleanup() {
  if (IsOwnedThread(webrtc::Thread::Current())) {
    // Joining our own thread would deadlock; the destructor (which runs on
    // the owner thread) finishes the job.
    RTC_LOG(LS_WARNING) << "Cleanup() requested on an owned thread; deferring";
    SignalQuit();
    return;
  }
  if (cleaned_up_.exchange(true)) {
    return;
  }
  should_quit_ = true;
  RTC_LOG(LS_INFO) << "DirectApplication cleanup";

  // Main runs the call state machine (EndCall continuations, timers). Join it
  // first so none of that races with the teardown below; tasks posted to it
  // from now on are dropped.
  if (main_thread_) {
    main_thread_->Stop();
  }
  ShutdownInternal();
  ReleaseCallResources();
  RunOn(signaling_thread(), [this] { video_source_ = nullptr; });
  video_sink_.reset();
  RunOn(network_thread(), [this] { ReleaseSockets(); });

  StopThreads();
  socket_factory_.reset();
  pss_.reset();
}

void DirectApplication::Disconnect() {
  RTC_LOG(LS_INFO) << "Disconnecting active call";
  auto* current = webrtc::Thread::Current();
  if (current && current != main_thread() && IsOwnedThread(current)) {
    main_thread()->PostTask([this] { ReleaseCallResources(); });
    return;
  }
  ReleaseCallResources();
}

void DirectApplication::Run() {
  quit_event_.Wait(webrtc::Event::kForever);
  Cleanup();
}

void DirectApplication::RunOnBackgroundThread() {
  // Initialize() already started every thread; they pump their own queues.
  if (!main_thread_) {
    RTC_LOG(LS_WARNING) << "RunOnBackgroundThread called before Initialize – ignoring.";
  }
}

DirectApplication::~DirectApplication() {
  should_quit_ = true;
  Cleanup();
}

bool DirectApplication::Initialize() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  if (!env_) {
    env_ = webrtc::CreateEnvironment();
  }

  main_thread_ = webrtc::Thread::CreateWithSocketServer();
  DirectThreadSetName(main_thread(), "Main");
  signaling_thread_ = webrtc::Thread::Create();
  DirectThreadSetName(signaling_thread(), "Signaling");
  network_thread_ = std::make_unique<webrtc::Thread>(pss());
  DirectThreadSetName(network_thread(), "Network");

  if (!main_thread_->Start() || !signaling_thread_->Start() ||
      !network_thread_->Start()) {
    RTC_LOG(LS_ERROR) << "Failed to start threads";
    return false;
  }
  return true;
}

bool DirectApplication::CreatePeerConnection() {
  RTC_LOG(LS_INFO) << "Creating peer connection";

  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
  if (!env_) {
    env_ = webrtc::CreateEnvironment();
  }

  if (!video_source_ && opts_.video && is_caller()) {
    RTC_LOG(LS_WARNING) << "Video source not set yet for caller with video enabled. This may cause black frames. Ensure SetVideoSource is called before CreatePeerConnection.";
  }

  if (opts_.encryption && !certificate_) {
    certificate_ = DirectLoadCertificateFromEnv(opts_);
    UpdateCertificateStats();
    if (certificate_stats_) {
      RTC_LOG(LS_INFO) << "Using certificate with fingerprint: "
                       << certificate_stats_->fingerprint << " and algorithm: "
                       << certificate_stats_->fingerprint_algorithm;
    }
  }

  webrtc::AudioDeviceModule::AudioLayer audio_layer =
      webrtc::AudioDeviceModule::kPlatformDefaultAudio;
#ifdef WEBRTC_SPEECH_DEVICES
  if (opts_.whisper && opts_.llama) {
    webrtc::SpeechAudioDeviceFactory::SetLlamaEnabled(true);
    webrtc::SpeechAudioDeviceFactory::SetLlamaModelFilename(opts_.llama_model);
    webrtc::SpeechAudioDeviceFactory::SetLlavaMMProjFilename(opts_.llava_mmproj);
    if (opts_.whisper_threads > 0) webrtc::SpeechAudioDeviceFactory::SetWhisperThreads(opts_.whisper_threads);
    if (opts_.llama_threads > 0) webrtc::SpeechAudioDeviceFactory::SetLlamaThreads(opts_.llama_threads);
    if (opts_.tts_threads > 0) webrtc::SpeechAudioDeviceFactory::SetTTSThreads(opts_.tts_threads);
#if LLAMA_NOTIFICATION_ENABLED
    llama_ = webrtc::SpeechAudioDeviceFactory::CreateWhillatsLlama(llamaCallback_);
#endif
  }

  if (opts_.whisper && !opts_.whisper_model.empty()) {
    audio_layer = webrtc::AudioDeviceModule::kSpeechAudio;
    webrtc::SpeechAudioDeviceFactory::SetWhisperEnabled(true);
    webrtc::SpeechAudioDeviceFactory::SetWhisperModelFilename(opts_.whisper_model);
    webrtc::SpeechAudioDeviceFactory::SetLanguage(opts_.language);
  }

  if (opts_.llama && !opts_.llama_llava_yuv.empty()) {
    webrtc::SpeechAudioDeviceFactory::SetYuvFilename(opts_.llama_llava_yuv,
      opts_.llama_llava_yuv_width, opts_.llama_llava_yuv_height);
  }

  if (!opts_.tts_voice.empty()) {
    webrtc::SpeechAudioDeviceFactory::SetTTSVoice(opts_.tts_voice);
  }

  if (!opts_.talking_face.empty()) {
    if (!webrtc::SpeechAudioDeviceFactory::talkingFace()) {
      webrtc::SpeechAudioDeviceFactory::SetTalkingFaceImage(opts_.talking_face);
    }
  }
#endif // WEBRTC_SPEECH_DEVICES

  if (opts_.audio == "dummy") {
    audio_layer = webrtc::AudioDeviceModule::kDummyAudio;
  }

  // The ADM is owned by the worker (== network) thread.
  RunOn(worker_thread(), [this, audio_layer] {
    if (audio_device_module_) {
      return;
    }
    audio_device_module_ = webrtc::CreateAudioDeviceModule(*env_, audio_layer);
    if (!audio_device_module_ || audio_device_module_->Init() != 0) {
      RTC_LOG(LS_ERROR) << "Audio layer " << audio_layer
                        << " unavailable, switching to DummyAudio";
      audio_device_module_ = webrtc::CreateAudioDeviceModule(
          *env_, webrtc::AudioDeviceModule::kDummyAudio);
      if (audio_device_module_) {
        audio_device_module_->Init();
      }
    }
  });
  if (!audio_device_module_) {
    RTC_LOG(LS_ERROR) << "Audio device module creation failed";
    return false;
  }

  if (!peer_connection_factory_) {
    auto network_manager =
        std::make_unique<webrtc::BasicNetworkManager>(*env_, pss());
    std::vector<std::string> ignore = {"docker0", "br0", "veth", "lo"};
    std::vector<webrtc::NetworkMask> vpn_masks;
    for (const auto& vpn_ip : vpns_) {
      ignore.push_back(vpn_ip);
      vpn_masks.emplace_back(IPFromString(vpn_ip), 32);
    }
    network_manager->set_network_ignore_list(ignore);
    if (!vpn_masks.empty()) {
      network_manager->set_vpn_list(vpn_masks);
    }
    network_manager_ = network_manager.get();

    webrtc::PeerConnectionFactoryDependencies deps;
    deps.network_thread = network_thread();
    deps.signaling_thread = signaling_thread();
    deps.env = *env_;
    deps.socket_factory = pss();
    deps.network_manager = std::move(network_manager);
    deps.adm = audio_device_module_;
    deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    deps.audio_processing_builder =
        std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();
    // H.264 first, then VP8 and VP9. AV1 and HEVC are not offered.
    deps.video_encoder_factory = std::make_unique<webrtc::VideoEncoderFactoryTemplate<
        webrtc::OpenH264EncoderTemplateAdapter,
        webrtc::LibvpxVp8EncoderTemplateAdapter,
        webrtc::LibvpxVp9EncoderTemplateAdapter>>();
    deps.video_decoder_factory = std::make_unique<webrtc::VideoDecoderFactoryTemplate<
        webrtc::OpenH264DecoderTemplateAdapter,
        webrtc::LibvpxVp8DecoderTemplateAdapter,
        webrtc::LibvpxVp9DecoderTemplateAdapter>>();
    webrtc::EnableMedia(deps);
    peer_connection_factory_ =
        webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  }

  if (!peer_connection_factory_) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return false;
  }

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  if (opts_.encryption) {
    if (!certificate_) {
      certificate_ = DirectLoadCertificateFromEnv(opts_);
      UpdateCertificateStats();
    }
    if (certificate_) {
      config.certificates.push_back(certificate_);
    } else {
      RTC_LOG(LS_ERROR) << "Failed to obtain certificate, WebRTC DTLS would not work";
    }
  } else {
    // Debug only: plain RTP.
    webrtc::PeerConnectionFactoryInterface::Options options;
    options.disable_encryption = true;
    peer_connection_factory_->SetOptions(options);
  }

  // Host, srflx and relay candidates so LAN calls connect directly.
  config.type = webrtc::PeerConnectionInterface::kAll;
  config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
  config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
  config.continual_gathering_policy =
      webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
  config.disable_ipv6_on_wifi = true;
  config.max_ipv6_networks = 0;
  config.port_allocator_config.flags |=
      webrtc::PORTALLOCATOR_ENABLE_ANY_ADDRESS_PORTS;

  for (const auto& stun_url : opts_.stun_urls) {
    std::string ice_error;
    if (!IceUrlUsesHostname(stun_url, ice_error)) {
      RTC_LOG(LS_ERROR) << ice_error;
      return false;
    }
    webrtc::PeerConnectionInterface::IceServer stun;
    stun.uri = stun_url;
    stun.urls.push_back(stun_url);
    stun.tls_cert_policy = webrtc::PeerConnectionInterface::kTlsCertPolicySecure;
    config.servers.push_back(stun);
    RTC_LOG(LS_INFO) << "STUN config: " << stun_url;
  }

  // turns: "uri;user;pass;uri2;user2;pass2;..." (semicolon-separated triples)
  if (!opts_.turns.empty()) {
    std::vector<std::string> parts = stringSplit(opts_.turns, ";");
    for (size_t i = 0; i + 2 < parts.size(); i += 3) {
      std::string ice_error;
      if (!IceUrlUsesHostname(parts[i], ice_error)) {
        RTC_LOG(LS_ERROR) << ice_error;
        return false;
      }
      webrtc::PeerConnectionInterface::IceServer turn;
      turn.uri = parts[i];
      turn.urls.push_back(parts[i]);
      turn.username = parts[i + 1];
      turn.password = parts[i + 2];
      turn.tls_cert_policy = webrtc::PeerConnectionInterface::kTlsCertPolicySecure;
      config.servers.push_back(turn);
      RTC_LOG(LS_INFO) << "TURN server configured, tls_verify=on";
    }
  }
  if (config.servers.empty()) {
    RTC_LOG(LS_WARNING) << "No STUN or TURN servers configured. Pass --stun=stun:host:port "
                           "and --turns=turns:host:5349?transport=tcp;user;pass";
  }

  webrtc::PeerConnectionDependencies pc_dependencies(this);
  auto pc_or = peer_connection_factory_->CreatePeerConnectionOrError(
      config, std::move(pc_dependencies));
  if (!pc_or.ok()) {
    RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: " << pc_or.error().message();
    return false;
  }
  peer_connection_ = pc_or.MoveValue();
  RTC_LOG(LS_INFO) << "PeerConnection created successfully.";

  if (video_source_) {
    if (webrtc::Thread::Current() == signaling_thread()) {
      AddVideoTrackIfSourceAvailable();
    } else {
      signaling_thread()->PostTask([this]() { AddVideoTrackIfSourceAvailable(); });
    }
  } else {
    RTC_LOG(LS_WARNING) << "No video source available to add as a track after peer connection creation. Video will not be sent.";
  }
  EnsureVideoSendActive();
  return true;
}

void DirectApplication::HandleMessage(webrtc::AsyncPacketSocket* socket,
                                      const std::string& message,
                                      const webrtc::SocketAddress& remote_addr) {
  RTC_LOG(LS_INFO) << "DirectApplication::HandleMessage: " << message;
  if (message.find("ICE:") == 0) {
    ice_candidates_received_++;
    SendMessage("ICE_ACK:" + std::to_string(ice_candidates_received_));

    // Send our ICE candidate if we haven't sent all
    if (ice_candidates_sent_ < kMaxIceCandidates) {
      ice_candidates_sent_++;
      SendMessage("ICE:" + std::to_string(ice_candidates_sent_));
    }
    // Start SDP exchange when ICE is complete
    else if (ice_candidates_received_ >= kMaxIceCandidates &&
             sdp_fragments_sent_ == 0) {
      sdp_fragments_sent_++;
      SendMessage("SDP:" + std::to_string(sdp_fragments_sent_));
    }
  } else if (message.find("SDP:") == 0) {
    sdp_fragments_received_++;
    SendMessage("SDP_ACK:" + std::to_string(sdp_fragments_received_));

    // Send our SDP fragment if we haven't sent all
    if (sdp_fragments_sent_ < kMaxSdpFragments) {
      sdp_fragments_sent_++;
      SendMessage("SDP:" + std::to_string(sdp_fragments_sent_));
    }
    // Send BYE when all exchanges are complete
    else if (sdp_fragments_received_ >= kMaxSdpFragments &&
             ice_candidates_received_ >= kMaxIceCandidates) {
      SendMessage(Msg::kBye);
    }
  } 
  #if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)  
  else if (message.find("LLAMA:") == 0) {
    std::string payload = message.substr(6);
    std::string language, text;
    std::regex re("^\\[([^\\]]+)\\](.*)$");
    std::smatch match;
    if (std::regex_match(payload, match, re)) {
      language = match[1].str();
      text = match[2].str();
    } else {
      language = "en";
      text = payload;
    }
    RTC_LOG(LS_INFO) << "Speaking: [" << language << "] " << text;
    webrtc::SpeechAudioDeviceFactory::NotifyText(text, language);
  } else if (message.find("WHISPER:") == 0) {
    std::string payload = message.substr(8);
    std::string language, text;
    std::regex re("^\\[([^\\]]+)\\](.*)$");
    std::smatch match;
    if (std::regex_match(payload, match, re)) {
      language = match[1].str();
      text = match[2].str();
    } else {
      language = "en";
      text = payload;
    }
    RTC_LOG(LS_INFO) << "Whispering: [" << language << "] " << text;
    webrtc::SpeechAudioDeviceFactory::AskLlama(text, language);
  } 
  #endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED
  else if (message == StatusCodes::kOk) {
    ShutdownInternal();            // close PeerConnection

    if (tcp_socket_) {             // <-- NEW
        tcp_socket_->Close();      // close FD
        tcp_socket_.reset();       // **remove from dispatcher**
    }
  }
}

bool DirectApplication::SendMessage(const std::string& message) {
  // The signaling socket lives on the network thread; hop there so sends from
  // observer callbacks stay ordered and never race with socket teardown.
  if (network_thread() && !network_thread()->IsCurrent()) {
    network_thread()->PostTask([this, message] { SendMessage(message); });
    return true;
  }
  if (!tcp_socket_) {
    RTC_LOG(LS_WARNING) << "Dropping message, signaling socket is closed: "
                        << message.substr(0, 32);
    return false;
  }
  RTC_LOG(LS_INFO) << "Sending message: " << message.substr(0, 120);
  int sent = tcp_socket_->Send(message.c_str(), message.length(),
                               webrtc::AsyncSocketPacketOptions());
  if (sent <= 0) {
    RTC_LOG(LS_ERROR) << "Failed to send message, error: " << tcp_socket_->GetError();
    return false;
  }
  return true;
}

webrtc::Socket* DirectApplication::WrapSocket(int s) {
  return pss()->WrapSocket(s);
}

webrtc::Socket* DirectApplication::CreateSocket(int family, int type) {
  return pss()->CreateSocket(family, type);
}

void DirectApplication::OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                           const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
    RTC_LOG(LS_INFO) << "Track added: " << receiver->track()->id() << " Kind: " << receiver->track()->kind();

    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        RTC_LOG(LS_INFO) << "Video track added for " << (is_caller() ? "caller" : "callee");
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(receiver->track().get());
        
#if defined(WEBRTC_SPEECH_DEVICES) && defined(LLAMA_NOTIFICATION_ENABLED)
        // Always create video sink for remote video tracks to prevent freezing
        if(!video_sink_) {
          RTC_LOG(LS_INFO) << "Initializing video sink for remote video track...";
          video_sink_ = std::make_unique<webrtc::LlamaVideoRenderer>();
          ((webrtc::LlamaVideoRenderer*)video_sink_.get())->set_is_llama(opts_.llama);
        }
#endif //WEBRTC_SPEECH_DEVICES && LLAMA_NOTIFICATION_ENABLED
        
        if (video_sink_) {
            RTC_LOG(LS_INFO) << "Attaching video sink to track: " << receiver->track()->id();
            video_track->AddOrUpdateSink(video_sink_.get(), webrtc::VideoSinkWants());
            
            // Store reference to remote video track to prevent disconnection
            remote_video_track_ = video_track;
            
            RTC_LOG(LS_INFO) << "Video sink attached successfully";
            // --- Echo video back to remote peer ---
            if (!video_source_) {
                RTC_LOG(LS_INFO) << "Creating EchoVideoTrackSource and outgoing track to echo remote video.";
                auto echo_source = webrtc::make_ref_counted<webrtc::EchoVideoTrackSource>();
                // Register echo_source as sink to remote video track
                // echo sink disabled: dangling pointer causes SEGV in IncomingVideoStream
                // video_track->AddOrUpdateSink(echo_source.get(), webrtc::VideoSinkWants());

                // Set as our local video source and add outgoing track
                SetVideoSource(echo_source);
            } else {
                // To avoid casts and RTTI, always create a dedicated EchoVideoTrackSource
                // for echoing the remote video back, without touching the existing
                // local video_source_ (which may be camera etc.).
                auto echo_source = webrtc::make_ref_counted<webrtc::EchoVideoTrackSource>();
                // echo sink disabled: dangling pointer causes SEGV in IncomingVideoStream
                // video_track->AddOrUpdateSink(echo_source.get(), webrtc::VideoSinkWants());
                // Optionally store echo_source if you want to keep it alive explicitly,
                // but since AddOrUpdateSink doesn't retain it, we need to hold the ref.
                echo_sources_.push_back(echo_source);  // Add vector<scoped_refptr<EchoVideoTrackSource>> echo_sources_ to class
            }
            RTC_LOG(LS_INFO) << "Receiver: Video track state: " << video_track->state();
            RTC_LOG(LS_INFO) << "Receiver: Video track enabled: " << (video_track->enabled() ? "true" : "false");
        } else {
            RTC_LOG(LS_ERROR) << "Video sink is still nullptr, cannot attach to track: " << receiver->track()->id();
        }
    } else if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        RTC_LOG(LS_INFO) << "Audio track added.";
        // Handle audio track if needed
    }
}

void DirectApplication::OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    RTC_LOG(LS_INFO) << "Track removed: " << receiver->track()->id();
    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        if (video_sink_ && remote_video_track_) {
            RTC_LOG(LS_INFO) << "Removing video sink from remote track for " << (is_caller() ? "caller" : "callee");
            remote_video_track_->RemoveSink(video_sink_.get());
        }
        // Clear remote video track reference
        remote_video_track_ = nullptr;
        RTC_LOG(LS_INFO) << "Remote video track reference cleared";
    }
}

// Store an external video source (injected by host) for use when creating video tracks.
bool DirectApplication::SetVideoSource(
    webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source) {
  if (signaling_thread() && webrtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask([this, video_source]() {
        // Safeguard: Prevent overwriting a live source with null during active connection
        if (video_source_ && video_source_->state() == webrtc::MediaSourceInterface::kLive && 
            !video_source && peer_connection_ && 
            peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
            RTC_LOG(LS_WARNING) << "Attempt to overwrite a live video source with null during active connection. Ignoring to prevent video freezing.";
            return;
        }
        video_source_ = video_source;
        AddVideoTrackIfSourceAvailable();
    });
    return true; // deferred
  }

  // Safeguard: Prevent overwriting a live source with null during active connection
  if (video_source_ && video_source_->state() == webrtc::MediaSourceInterface::kLive && 
      !video_source && peer_connection_ && 
      peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
    RTC_LOG(LS_WARNING) << "Attempt to overwrite a live video source with null during active connection. Ignoring to prevent video freezing.";
    return true;
  }
  video_source_ = video_source;
  AddVideoTrackIfSourceAvailable();
  RTC_LOG(LS_INFO) << "Video source set for " << (is_caller() ? "caller" : "callee");
  return true;
}

bool DirectApplication::SetVideoSink(
    std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> video_sink) {
  // Ensure assignment happens on the signaling thread for safety.
  if (webrtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->BlockingCall([this, &video_sink]() {
      video_sink_ = std::move(video_sink);
    });
  } else {
      video_sink_ = std::move(video_sink);
  }

  RTC_LOG(LS_INFO) << "Video sink set for " << (is_caller() ? "caller" : "callee");
  return true;
}

// Method to add video track if source is available and peer connection exists
void DirectApplication::AddVideoTrackIfSourceAvailable() {
  if (!video_source_ || !peer_connection_) return;

  if (webrtc::Thread::Current() != signaling_thread()) {
    signaling_thread()->PostTask([this]() { AddVideoTrackIfSourceAvailable(); });
    return;
  }

  RTC_LOG(LS_INFO) << "Adding video track to existing peer connection for " << (is_caller() ? "caller" : "callee");
  RTC_LOG(LS_INFO) << "Video source state before adding track: " << (video_source_->state() == webrtc::MediaSourceInterface::kLive ? "Live" : "Not Live");
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(
      peer_connection_factory_->CreateVideoTrack(video_source_, "video_track"));
  video_track_ = video_track;
  video_track_->set_enabled(true);
  auto result = peer_connection_->AddTrack(video_track, {"stream1"});
  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: " << result.error().message();
  } else {
    RTC_LOG(LS_INFO) << "Video track added successfully to PeerConnection";
    RTC_LOG(LS_INFO) << "Sender: Video track enabled: " << (video_track_->enabled() ? "true" : "false");
    if (video_source_->state() == webrtc::MediaSourceInterface::kLive && video_track_->enabled()) {
      RTC_LOG(LS_INFO) << "Sender: Video source is live and track is enabled. Frames should be sent to remote peer.";
    } else {
      RTC_LOG(LS_WARNING) << "Sender: Video source or track state issue. Black frames may be sent to remote peer.";
    }
    // Safeguard: Ensure video send is active with the current source
    EnsureVideoSendActive();
    ConfigureOutgoingVideo();
  }
}

namespace {
std::string LowerName(std::string name) {
  for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return name;
}

int CodecRank(const webrtc::RtpCodecCapability& codec, const std::string& force) {
  const std::string name = LowerName(codec.name);
  if (name == "rtx" || name == "red" || name == "ulpfec" || name == "flexfec-03") return 100;
  if (!force.empty() && name != force) return 1000;
  if (name == "av1" || name == "h265" || name == "hev1" || name == "hvc1") return 1000;
  if (name == "h264") {
    int score = 0;
    auto mode = codec.parameters.find("packetization-mode");
    auto profile = codec.parameters.find("profile-level-id");
    if (mode == codec.parameters.end() || mode->second != "1") score += 10;
    if (profile != codec.parameters.end() && profile->second != "42e01f") score += 1;
    return score;
  }
  if (!force.empty()) return 1000;
  if (name == "vp8") return 20;
  if (name == "vp9") return 30;
  return 1000;
}

std::vector<webrtc::RtpCodecCapability> OrderVideoCodecs(
    const std::vector<webrtc::RtpCodecCapability>& caps,
    const std::string& force) {
  std::vector<webrtc::RtpCodecCapability> media;
  std::vector<int> payload_types;
  for (const auto& codec : caps) {
    const int rank = CodecRank(codec, force);
    const std::string name = LowerName(codec.name);
    if (rank >= 100 || name == "rtx" || name == "red" || name == "ulpfec" || name == "flexfec-03") {
      continue;
    }
    media.push_back(codec);
    if (codec.preferred_payload_type) payload_types.push_back(*codec.preferred_payload_type);
  }
  std::stable_sort(media.begin(), media.end(), [&](const auto& a, const auto& b) {
    return CodecRank(a, force) < CodecRank(b, force);
  });
  for (const auto& codec : caps) {
    const std::string name = LowerName(codec.name);
    if (name != "rtx" && name != "red" && name != "ulpfec" && name != "flexfec-03") continue;
    if (name == "rtx") {
      auto apt = codec.parameters.find("apt");
      if (apt == codec.parameters.end()) continue;
      const int apt_pt = std::atoi(apt->second.c_str());
      if (std::find(payload_types.begin(), payload_types.end(), apt_pt) == payload_types.end()) {
        continue;
      }
    }
    media.push_back(codec);
  }
  bool has_media = false;
  for (const auto& codec : media) {
    const std::string name = LowerName(codec.name);
    if (name != "rtx" && name != "red" && name != "ulpfec" && name != "flexfec-03") {
      has_media = true;
      break;
    }
  }
  if (!has_media) return {};
  return media;
}
}  // namespace

void DirectApplication::ConfigureOutgoingVideo() {
  if (!peer_connection_ || !peer_connection_factory_ || !opts_.video) return;
  const auto caps =
      peer_connection_factory_->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs;
  auto preferred = OrderVideoCodecs(caps, opts_.video_codec);
  const int width = opts_.video_width > 0 ? opts_.video_width : 1920;
  const int height = opts_.video_height > 0 ? opts_.video_height : 1080;
  const int fps = opts_.video_fps > 0 ? opts_.video_fps : 30;
  const int max_bps = std::max(300000, width * height * fps / 12);
  for (const auto& transceiver : peer_connection_->GetTransceivers()) {
    if (!transceiver || transceiver->media_type() != webrtc::MediaType::VIDEO) continue;
    if (!preferred.empty()) {
      std::span<webrtc::RtpCodecCapability> codec_span(preferred.data(), preferred.size());
      auto err = transceiver->SetCodecPreferences(codec_span);
      if (!err.ok()) {
        RTC_LOG(LS_ERROR) << "SetCodecPreferences failed: " << err.message();
      } else {
        RTC_LOG(LS_INFO) << "Video codecs start with " << preferred.front().name
                         << (opts_.video_codec.empty() ? " (H.264 preferred)" : " (forced)");
        fprintf(stderr, "video codecs: %s%s\n", preferred.front().name.c_str(),
                opts_.video_codec.empty() ? " then VP8/VP9" : " only");
      }
    } else if (!opts_.video_codec.empty()) {
      RTC_LOG(LS_ERROR) << "Requested video codec is not built into this binary";
      fprintf(stderr, "video codec %s is not available in this build\n", opts_.video_codec.c_str());
    }
    auto sender = transceiver->sender();
    if (!sender) continue;
    auto params = sender->GetParameters();
    params.degradation_preference =
        webrtc::DegradationPreference::MAINTAIN_FRAMERATE_AND_RESOLUTION;
    if (!params.encodings.empty()) {
      params.encodings[0].max_framerate = static_cast<double>(fps);
      params.encodings[0].scale_resolution_down_by = 1.0;
      params.encodings[0].scale_resolution_down_to = webrtc::Resolution{width, height};
      params.encodings[0].max_bitrate_bps = max_bps;
      params.encodings[0].min_bitrate_bps = std::max(150000, max_bps / 8);
    }
    auto set = sender->SetParameters(params);
    if (!set.ok()) {
      RTC_LOG(LS_WARNING) << "Video sender SetParameters: " << set.message();
    } else {
      fprintf(stderr, "video send target %dx%d @ %d fps, max %d bps, degradation=maintain\n",
              width, height, fps, max_bps);
    }
  }
}

// New method to ensure video send is active with the current source
void DirectApplication::EnsureVideoSendActive() {
  if (video_track_ && video_source_ && peer_connection_) {
    RTC_LOG(LS_INFO) << "Ensuring video send is active for SSRC associated with track.";
    
    // Check if video source is live and track is enabled
    if (video_source_->state() == webrtc::MediaSourceInterface::kLive) {
      RTC_LOG(LS_INFO) << "Video source is live, ensuring it remains active for sending.";
      
      // Ensure video track is enabled
      if (!video_track_->enabled()) {
        RTC_LOG(LS_WARNING) << "Video track was disabled, re-enabling to prevent freezing.";
        video_track_->set_enabled(true);
      }
      
      // Log current connection state for debugging
      if (peer_connection_->ice_connection_state() == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
        RTC_LOG(LS_INFO) << "ICE connected - video should be flowing normally.";
      }
      
    } else {
      RTC_LOG(LS_WARNING) << "Video source is not live, cannot ensure active video send.";
    }
  } else {
    RTC_LOG(LS_WARNING) << "Cannot ensure video send active: missing track (" << (video_track_ ? "present" : "missing") 
                        << "), source (" << (video_source_ ? "present" : "missing") 
                        << "), or peer connection (" << (peer_connection_ ? "present" : "missing") << ").";
  }
}
