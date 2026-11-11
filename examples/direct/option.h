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

#pragma once

#include <memory>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstdlib> // for std::getenv
#include <cstdio>  // for FILE, fopen, fclose
#include <fstream>
#include <sstream>
#include <future>
#include <algorithm>
#include <cstdlib> // for getenv

#include <sys/socket.h>
#include <netinet/tcp.h>

#ifndef DIRECT_EXPORT_H
#define DIRECT_EXPORT_H

#if defined(_MSC_VER)
    #define WHILLATS_EXPORT __declspec(dllexport)
    #define WHILLATS_IMPORT __declspec(dllimport)
#elif defined(__GNUC__)
    #define DIRECT_EXPORT __attribute__((visibility("default")))
    #define DIRECT_IMPORT __attribute__((visibility("default")))
#else
    #define DIRECT_EXPORT
    #define DIRECT_IMPORT
#endif

#ifdef DIRECT_BUILDING_DLL
    #define DIRECT_API DIRECT_EXPORT
#else
    #define DIRECT_API DIRECT_IMPORT
#endif

enum LoggingSeverity {
  AS_VERBOSE,
  AS_INFO,
  AS_WARNING,
  AS_ERROR,
  AS_NONE,
};

#ifdef RTC_BASE_LOGGING_H_

#define AS_VERBOSE LS_VERBOSE
#define AS_INFO LS_INFO
#define AS_WARNING LS_WARNING
#define AS_ERROR LS_ERROR

#define AS_NONE LS_NONE
#define APP_LOG(x) RTC_LOG(x)
#endif // RTC_BASE_LOGGING_H_

#endif // DIRECT_EXPORT_H 

// Command line options
struct Options {
    std::string mode{}; // default to empty, set in parseOptions
    bool encryption = true;
    bool whisper = true;
    bool llama = true;
    bool video = false;
    bool help = false;
    bool bonjour = false;
    std::string help_string{};
    std::string config_path{}; // Path to JSON config file
    std::string whisper_model{};
    std::string llama_model{};
    std::string llava_mmproj{};
    std::string webrtc_cert_path{};
    std::string webrtc_key_path{};
    std::string webrtc_speech_initial_playout_wav{};
    std::string llama_llava_yuv{};
    int llama_llava_yuv_width = 300;
    int llama_llava_yuv_height = 300;
    std::string address{};
    std::string turns{};
    std::string vpn{};
    std::string camera{};
    std::string talking_face{}; // Path to avatar image for animated face
    std::string bonjour_name{};
    std::string user_name{}; // Name of this user (for registration)
    std::string target_name{}; // Name of user to call (for caller mode)
    std::string room_name{}; // Room name to join
    std::string language{}; // Language to use for speech
    std::vector<std::string> whispers{}; // Whispers to use for speech
    int whisper_threads = 0; // 0 = auto (hardware_concurrency)
    int llama_threads = 0;   // 0 = auto (hardware_concurrency)
    int tts_threads = 0;     // 0 = auto (hardware_concurrency)
    bool websocket_signaling = true;  // expose raw WebSocket signaling endpoint
    int websocket_port = 3457;        // raw WebSocket signaling port
    int call_setup_timeout_ms = 45000;  // INVITE -> media connected
    int media_grace_ms = 10000;         // tolerated PeerConnection "disconnected"
    int hangup_ack_timeout_ms = 2000;   // BYE -> 200 OK before force-close
    std::string tts_voice{};            // default cloned voice (Pocket TTS clip name)
    // Public signaling. Hostnames only; port and path come from the URL.
    std::string signal_url{};           // wss://host:443/demo/ws
    std::string token_file{};           // bearer token file; never logged
    std::string token_env{"DIRECT_SIGNAL_TOKEN"};
    bool token_in_query = false;        // default: Authorization header only
    std::string turns_file{};           // JSON TURN account; credentials stay in the file
    std::string ca_file{};              // PEM bundle for TLS verification
    std::vector<std::string> stun_urls; // stun:host:port, hostnames only
    std::string video_codec{};          // empty = H.264 then VP8/VP9; "h264" forces H.264
    std::string video_source{};         // pattern, camera, screen, or image
    std::string image_file{};           // PNG still used when video_source is image
    std::string remote_window{"auto"};  // auto, openvr, sdl, app, or none
    std::string remote_frame{};         // rgba frame file for remote_window=app
    int video_width = 0;
    int video_height = 0;
    int video_fps = 0;                  // 1..60, 30 if video is on and unset
    int stats_interval_sec = 0;
    int call_duration_sec = 0;
    std::string audio{};                // "dummy" forces the fake audio device
    std::string parse_error{};
};

struct SignalEndpoint {
    bool ssl = true;
    std::string host;
    int port = 443;
    std::string path{"/"};
};

bool IsNumericHost(const std::string& host);
bool ParseSignalUrl(const std::string& url, SignalEndpoint& out, std::string& error);
bool IceUrlUsesHostname(const std::string& url, std::string& error);
std::string UrlEncode(const std::string& in);
// Reads --token-file or the env named by token_env. The returned secret must
// not be written to logs.
std::string LoadSignalToken(const Options& opts, std::string& error);
std::string AppendTokenQuery(std::string path, const std::string& token);

// Function to parse command line string to above options
DIRECT_API Options parseOptions(const char* argString);
Options parseOptions(const std::vector<std::string>& args);

bool DIRECT_API ParseIpAndPort(const std::string& ip_port, std::string& ip, int& port);

// Function to get command line options to a string, to print or speak
DIRECT_API std::string getUsage(const Options opts);

DIRECT_API void DirectSetLoggingLevel(LoggingSeverity level);

DIRECT_API webrtc::scoped_refptr<webrtc::RTCCertificate> DirectLoadCertificateFromEnv(Options opts);
DIRECT_API uint32_t DirectCreateRandomId();
DIRECT_API std::string DirectCreateRandomUuid();
DIRECT_API void DirectThreadSetName(webrtc::Thread* thread, const char* name);
DIRECT_API webrtc::IceCandidateInterface* DirectCreateIceCandidate(const char* sdp_mid,
    int sdp_mline_index,
    const char* sdp,
    webrtc::SdpParseError* error);
DIRECT_API std::unique_ptr<webrtc::SessionDescriptionInterface> DirectCreateSessionDescription(
    webrtc::SdpType type,
    const char* sdp,
    webrtc::SdpParseError* error_out);
