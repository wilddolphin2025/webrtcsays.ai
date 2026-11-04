# ROBOT_CALL test plan

Verifies the browser → AI robot call described in [ROBOT_CALL.md](ROBOT_CALL.md),
from a local build up to the deployment on `rtc.wilddolphin.us`. Each case
lists how to run it and what passing looks like. Phase 1 (direct path) must
pass before Phase 2 (LiveKit) work starts.

Status key: **Auto** = scripted, **Manual** = needs a person and a browser.

## 0. Prerequisites

| Item | Check |
|------|-------|
| Linux build of `directcall` from `156.0.0` (`./build.sh release whillats`) | `directcall --help` lists `--call_setup_timeout_ms` and `--tts_voice` |
| whillats `v0.4.0` built with `-DWHILLATS_PIPER=ON -DWHILLATS_POCKET_TTS=ON` (CMake ≥ 3.28) | `whillats_server` links `libpocket_tts` and `libonnxruntime` 1.23.2 (`ldd`) |
| Pocket TTS models | `third_party/pocket-tts-raven/tools/prepare_models.sh` finished; `POCKET_TTS_MODELS_DIR` set |
| Voice clips | `POCKET_TTS_VOICES_DIR` has ≥ 2 clips (6–15 s, one speaker each), with a consent record per clip |
| Browsers | Chrome, Firefox and Safari (desktop) plus Chrome Android and Safari iOS |
| Networks | Same LAN, home NAT, mobile carrier (4G/5G), and one UDP-blocked network (forces TURN/TLS on 5349) |

Run every Phase 1 case with `DIRECT_LOG_LEVEL=info` and keep the journal:
`journalctl -u directcall3-talkingface5 -f`.

## 1. Build and unit checks (Auto)

| ID | Case | Run | Pass |
|----|------|-----|------|
| B1 | M156 host build | `autoninja -C out/release directcall` | exit 0, `strings libdirect.* \| grep "source stamp"` shows 2026-09-26 |
| B2 | Speech build | build with `rtc_use_speech_audio_devices=true` | links on Linux (macOS needs `libwhillats.dylib`) |
| B3 | Pocket adapter | `c++ -std=c++17 -Isrc -Ithird_party/pocket-tts-raven/include test/pocket_voice_tts_test.cc src/pocket_voice_tts.cc && ./a.out` (in whillats) | all PASS: 1 kHz unity, 10 kHz < −40 dB, unsafe names rejected |
| B4 | whillats server smoke | start `whillats_server`, send `MSG_CONFIG` + `MSG_TTS_START` | log shows `[pocket_tts] ready` and `Piper TTS started` |

## 2. Signaling and teardown (Auto, loopback)

| ID | Case | Run | Pass |
|----|------|-----|------|
| T1 | Caller SIGTERM mid-call | `scripts/ci/test_direct_shutdown.sh out/release/directcall` | `PASS 1` … `PASS 4` |
| T2 | Caller SIGKILL | same script | callee logs `Ending call (signaling connection lost)` and takes the next call |
| T3 | Callee SIGTERM | same script | caller gets BYE, callee exits ≤ 10 s |
| T4 | Frozen caller (SIGSTOP) | same script | callee logs `Ending call (media disconnected)` within ICE timeout + `--media_grace_ms` |
| T5 | Browser vanishes from WS bridge | `scripts/ci/test_ws_bridge.py out/release/directcall cert.pem key.pem` | both PASS lines |
| T6 | Soak | run T1–T5 in a loop for 1 h | 0 failures, no core dumps, RSS of the callee flat (±10 %) |
| T7 | Second callee on same port | start two callees with the same `--websocket_port` | second fails to bind (no `SO_REUSEPORT` sharing) |

## 3. Browser call on loopback / LAN (Manual)

Serve `ws_test.html` locally, callee with `--websocket_signaling --talking_face=RobotPhoneLogo.jpeg --video`.

| ID | Case | Steps | Pass |
|----|------|-------|------|
| L1 | First call | Open page, Call | robot audio and talking face within 3 s; `PeerConnectionState -> connected` |
| L2 | Conversation | Ask 5 questions | each answered; answer language matches question language |
| L3 | Barge-in | Interrupt during a long answer | robot stops within ~0.5 s (no tail of the old sentence), answers the new question |
| L4 | Hang up | Press hangup | browser sends BYE, gets `200 OK`; callee `Ending call (remote BYE)` |
| L5 | Close tab mid-call | close tab | callee hangs up ≤ 1 s; a new tab can call immediately |
| L6 | Reload during setup | reload before media connects | callee frees itself; next call works |
| L7 | Busy | second tab calls while first is in a call | second gets `486 Busy Here`; first call unaffected |
| L8 | Setup timeout | block UDP + no TURN, Call | call ends after `--call_setup_timeout_ms` (45 s) with BYE |

## 4. Personalized voices (Manual)

| ID | Case | Steps | Pass |
|----|------|-------|------|
| V1 | Default voice | callee `--tts_voice=a.wav` | robot speaks in voice A |
| V2 | Voice per call | `INVITE:{"agent":"audio","voice":"b.wav"}` | robot speaks in voice B from the first sentence |
| V3 | Switch mid-call | send `{"type":"voice","name":"a.wav"}` | next sentence in voice A; first switch to a new clip ≤ 1 s extra delay, cached switch none |
| V4 | Unsafe / unknown names | send `../x.wav`, `/etc/passwd`, `.x.wav`, `missing.wav` | server logs `rejected voice`; robot keeps the current voice; no crash |
| V5 | Fallback language | ask in Russian or Chinese | answer spoken by Piper (Pocket covers en/es/fr/de/it/pt only) |
| V6 | Quality | 5 listeners rate similarity to the reference clip and naturalness (1–5) | mean ≥ 3.5 on both; no audible aliasing or clicks between chunks |
| V7 | Disclosure | page UI | page says the voice is synthetic |

## 5. Latency and capacity (Manual, measured on the VPS)

Measure end of user speech → first robot audio from the browser (record both
sides, or use `chrome://webrtc-internals` audio levels).

| ID | Case | Pass |
|----|------|------|
| P1 | Turn latency, 20 turns | median ≤ 1.8 s, p90 ≤ 2.5 s |
| P2 | Pocket first audio | `[pocket_tts]` first chunk ≤ 150 ms after sentence queued |
| P3 | CPU during a call | `whillats_server` + `directcall` leave ≥ 20 % CPU headroom |
| P4 | 30-minute call | no audio drift or growing delay; memory flat |
| P5 | Back-to-back calls | 50 calls of 1 min each | all connect; no leaked threads (`ls /proc/<pid>/task \| wc -l` stable) |

## 6. Deployment on rtc.wilddolphin.us (Manual)

| ID | Case | Pass |
|----|------|------|
| D1 | `/ws` reachable | `wss://rtc.wilddolphin.us/ws` upgrades (101), no 502 |
| D2 | Unit settings | `systemctl show directcall3-talkingface5 -p KillSignal,TimeoutStopUSec,Restart` = SIGTERM, 15 s, on-failure |
| D3 | Restart during a call | `systemctl restart` while a browser is in a call | browser sees BYE and WS close 1001; service back ≤ 10 s; new call works |
| D4 | Crash recovery | `kill -9` the main PID | systemd restarts it; browser call ends cleanly via WS close |
| D5 | TURN UDP | call from mobile carrier | ICE selects relay or srflx; audio both ways |
| D6 | TURN TLS only | call from UDP-blocked network | relay over 5349 works; audio both ways |
| D7 | Browser matrix | L1, L3, L4 on every browser in §0 | all pass |
| D8 | Nothing else exposed | `nmap` from outside | only 80/443/3478/5349 open (no 3456/3459/7880) |

## 7. Phase 2: robot in a LiveKit room (after `LiveKitPeer` exists)

| ID | Case | Pass |
|----|------|------|
| K1 | Robot joins | robot appears as `robot-<room>` with its own API key, not `signal` |
| K2 | 1 human + robot | same results as L1–L4 through LiveKit |
| K3 | 3 humans + robot | robot answers whoever spoke; no echo of its own voice into STT |
| K4 | Interpreter mode | Spanish speaker → others hear English in the speaker's cloned voice (with consent) |
| K5 | Robot leaves | robot's EndCall sends LeaveRequest; room keeps working for humans |
| K6 | 20-participant limit | 21st join rejected; robot unaffected |
| K7 | LiveKit restart | robot reconnects or exits cleanly; no zombie participant |
| K8 | API binding | 7880 not reachable from the public IP |

## Exit criteria

Phase 1 ships when §1–§4 and §6 pass, P1 and P4 pass, and there are no open
crash or hang bugs. Record results (date, build tag, pass/fail, notes) in the
release ticket for `156.0.0`.
