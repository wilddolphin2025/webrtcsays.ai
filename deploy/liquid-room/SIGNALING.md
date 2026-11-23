# LiquidServer signaling (direct protocol over WebSocket)

Status: spec for the LiquidServer MVP (room101: one LiquidCall iPhone and one Chrome browser).
Nothing here changes the direct protocol. LiquidServer plays the **callee** role that `directcall`
plays today, so the iPhone (`DirectCallerClient::ConnectSignalUrl`) and the browser page
(`page/index.html`) stay plain callers.

Sources this was read from (webrtcsays.ai):

| Behaviour | File @ commit |
|---|---|
| Caller state machine (HELLO retry, 200 OK, INVITE JSON, WAITING, ANSWER reassembly, status-code rejection, BYE ack) | `examples/direct/caller.cc` @ `237b8ea0cd` (`liquidcodec-ane`, tag `liquidcall-v0.2`) |
| Callee replies (HELLO → `200 OK`/`486`, INVITE → `WAITING`, BYE → `200 OK`) | `examples/direct/callee.cc`, `peer.cc` @ `237b8ea0cd` |
| WebSocket client handshake, `Authorization: Bearer`, masked frames, 5 s pings | `examples/direct/client.cc` (`connectToWebSocketUrl`), `wsock.cc`, `option.cc` (`ParseSignalUrl`, `LoadSignalToken`) @ `237b8ea0cd` |
| Server-side text-frame bridge (no length prefix, 15 s ping, 45 s idle) | `examples/direct/main.cc` @ `237b8ea0cd` |
| Message constants | `examples/direct/status.h` |
| Front-door auth (cookie, Bearer, `?token=`) | `deploy/webrtcsays-demo/auth/demo_auth.py`, `nginx/webrtcsays-demo-locations.conf` |
| Room pairing precedent | `deploy/frame-view/router.py` (live as PR 22 `9d8c9d2f`) |

Wire format words below (`MUST`, `SHOULD`, `MAY`) are used in the RFC 2119 sense.

## 1. Transport

```
iPhone / Chrome ──wss──▶ rtc.wilddolphin.us/demo/room/ws/<room>     (nginx: auth_request, strips credentials)
                 ──https─▶ ls.wilddolphin.us/ws/<room>              (Caddy on the LiquidServer VPS: source-IP allowlist)
                 ──ws────▶ 127.0.0.1:7880/ws/<room>                 (LiquidServer, loopback only)
media:  iPhone / Chrome ◀──DTLS-SRTP──▶ LiquidServer public IP udp/7882 (ICE UDP mux), tcp/7881 (ICE-TCP fallback)
```

- Public URL: `wss://rtc.wilddolphin.us/demo/room/ws/<room>`. `<room>` matches
  `^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`; `ws` is reserved. MVP room: `room101`.
- RFC 6455. Client frames are masked, server frames are not. **One signaling message per text
  frame.** There is no 2-byte length prefix on the WebSocket (that framing exists only on the raw TCP
  socket, `main.cc`). Server text frames MUST NOT start with control characters (the page strips
  leading `\x00-\x1f`, the iPhone does not).
- Max message 256 KiB. An SDP is about 1–4 KiB.
- Keepalive: the iPhone sends a ping 10 s after connect and then every 5 s; the server MUST answer
  with a pong carrying the same payload. The server SHOULD ping every 15–30 s (browsers auto-pong)
  and MUST drop a socket that has been silent for 120 s.
- Binary frames are ignored.

### 1.1 Authentication

Two hops, two checks. LiquidServer never sees a user credential.

1. **Front door (rtc.wilddolphin.us, existing)**. `auth_request /_demo_auth` accepts, in order:
   `Authorization: Bearer <token>` (iPhone; tokens in `/etc/webrtcsays-demo/tokens`, e.g. client
   id `founder-iphone`), `?token=<token>` (fallback for clients that can't set a header; nginx copies
   it to `X-Demo-Token` and clears `$args`), or the `wsdemo` session cookie (browser, set by
   `/demo/login`). Bad or missing → `401` and no upstream socket. nginx then clears
   `Authorization`, `Cookie`, `X-Demo-Token` and the query string before proxying.
   `demo_auth.py` allows `X-Original-URI` `/demo/room/<room>` and `/demo/room/ws/<room>`
   (this PR).
2. **Gate (rtc → LiquidServer)**. nginx adds `X-Liquid-Gate: <64 hex>` from a root-only include
   (`/etc/nginx/liquid-gate.conf`, never in git). Caddy forwards `/ws/*` only from the rtc VPS
   address. LiquidServer MUST:
   - compare `X-Liquid-Gate` against the file named by `LIQUID_GATE_FILE` in constant time and answer
     `401` on mismatch, before the `101` upgrade;
   - answer `400` to any request target that contains `?`;
   - answer `404` unless the path is exactly `/ws/<room>` with the room regex above;
   - bind HTTP to `127.0.0.1` only;
   - never log the gate value, `Authorization`, `Cookie`, a query string, or SDP bodies
     (log the verb only, as `router.py` does).
   - (Later, not MVP) a direct mode where LiquidServer checks `Authorization: Bearer` itself against
     a tokens file with the same `<client-id> <token>` format as `/etc/webrtcsays-demo/tokens`.

The iPhone uses the same URL as the browser, with `--signal-url=wss://rtc.wilddolphin.us:443/demo/room/ws/room101`
and the token from `--token-file` or `DIRECT_SIGNAL_TOKEN` (`Authorization` header; leave
`token_in_query` off). `ParseSignalUrl` refuses IP-address hosts, so a hostname is required.

## 2. Messages

`C` = caller (iPhone or page), `S` = LiquidServer. Text is ASCII except SDP.

| # | Direction | Message | Server rule |
|---|---|---|---|
| 1 | C→S | `HELLO` or `HELLO:<name>` | Reply `200 OK`. The caller resends HELLO every 200 ms until it sees `200 OK` (`SendHelloWithRetry`), so answer every HELLO received before the INVITE and ignore HELLO after it. `<name>` is informational (`[A-Za-z0-9._-]{1,64}`, else ignore it). Room full → `486 Busy Here`. Server overloaded or shutting down → `480 Temporarily Unavailable`. |
| 2 | S→C | `200 OK` | Exactly this text. |
| 3 | C→S | `INVITE` or `INVITE:{json}` | Reply `WAITING` at once. JSON fields seen today: `agent` (string, `"audio"`/`"text"`), `encryption` (bool), `video` (bool), `room_name` (string), `language`, `llama_model`, `llava_mmproj` (strings, ignore). The room is the URL path. If `room_name` is present and differs from the path, reply `400 Bad Request` and close. Unknown fields MUST be ignored. INVITE before a `200 OK` → `400 Bad Request`. |
| 4 | S→C | `WAITING` | The caller now creates the PeerConnection and its offer (`DirectPeer::Start`). |
| 5 | C→S | `OFFER:<sdp>` | Whole SDP in one frame, CRLF line ends. Build the answer (§3) and send it in **one** frame. The iPhone keeps appending ANSWER fragments until the SDP parses, so a split answer only adds delay; never interleave other messages inside it. |
| 6 | S→C | `ANSWER:<sdp>` | Complete SDP. Candidates MAY be embedded (recommended: wait for gathering to finish; the server has one public host candidate per transport). |
| 7 | both | `ICE:<mline>:<candidate>` | `<mline>` = decimal `sdpMLineIndex`. `<candidate>` is the `candidate:…` attribute value without `a=`. The page sends `sdpMid` `"0"` implicitly. Queue candidates that arrive before the remote description. No end-of-candidates message exists. Do **not** send `ICE_ACK:` (a `directcall` callee habit; the page only logs it). |
| 8 | C→S | `OFFER:<sdp>` after an answer | Renegotiation on the same session (voice change, §4). Answer with `ANSWER:<sdp>`. It is not a new call. |
| 9 | either | `BYE` | Receiver replies `200 OK`, stops that participant's media, and removes it from the room. The caller acknowledges a server BYE with `200 OK` and closes; close the socket after that ack or after 2 s (`hangup_ack_timeout_ms`). |
| 10 | C→S | `CANCEL` | Before the ANSWER: treat as BYE, reply `200 OK`. |
| 11 | S→C | `400 Bad Request`, `480 Temporarily Unavailable`, `486 Busy Here` | Any text that matches `^\d+\s` other than the `200 OK` handshake/ack ends the iPhone call immediately (`StatusCodes::IsStatusCode`, "Not retrying"). Send these only to refuse a call, then close. |

Messages never used by LiquidServer: `ADDRESS:`, `USERS:`, `REGISTER:`, `FACE:`, `LLAMA:`, `VOICE:`.
`VOICE:` is the TTS clip selector of the robot callee and MUST NOT be reused for voice swap; voice
selection lives in SDP only.

### 2.1 Typical sequences

Browser (page/index.html):
```
C: HELLO
S: 200 OK
C: INVITE:{"agent":"audio","room_name":"room101","video":false}
S: WAITING
C: OFFER:v=0…m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8 9 126…a=rtpmap:111 opus/48000/2…
S: ANSWER:v=0…m=audio 7882 UDP/TLS/RTP/SAVPF 111…a=candidate:… 203.0.113.10 7882 typ host…
C: ICE:0:candidate:… typ srflx …          (trickle, any time)
…  media  …
C: BYE
S: 200 OK
```

iPhone (LiquidCall, `--audio-codec=liquidcall`, voice Female):
```
C: HELLO            (retried every 200 ms until answered)
S: 200 OK
C: INVITE:{"agent":"audio","encryption":true,"video":false,"room_name":"room101"}
S: WAITING
C: OFFER:…m=audio 9 UDP/TLS/RTP/SAVPF 96 111…a=rtpmap:96 LIQUIDCALL/48000/1…a=fmtp:96 pack=tight;fpp=1
        …a=x-liquidcall-voice:96 female…
S: ANSWER:…m=audio 7882 UDP/TLS/RTP/SAVPF 96 111…a=fmtp:96 pack=tight;fpp=1
        …a=x-liquidcall-voices:96 me,female,male…a=x-liquidcall-voice:96 female…
…  user picks "My voice"  …
C: OFFER:…a=x-liquidcall-voice:96 me…
S: ANSWER:…a=x-liquidcall-voice:96 me…
C: BYE
S: 200 OK
```

## 3. Answer rules (SDP)

- Bundle and `rtcp-mux` are required; LiquidServer answers one transport.
- DTLS-SRTP only (no SDES). Offer `a=setup:actpass`; the server answers `active` or `passive`.
  SRTP profiles: accept `SRTP_AES128_CM_HMAC_SHA1_32` (the iPhone prefers it for LIQUIDCALL, 4-byte tag),
  `SRTP_AES128_CM_HMAC_SHA1_80` and `SRTP_AEAD_AES_128_GCM`. pion/dtls supports all three.
- Audio header extensions: answer only `urn:ietf:params:rtp-hdrext:sdes:mid`. The iPhone strips the
  others for LIQUIDCALL anyway.
- Audio RTCP: the iPhone sends reports every 10 s for LIQUIDCALL; do not treat 10 s without RTCP as dead.
- Video m-lines: the MVP answers them with port 0 (rejected).
- Codec choice per participant (audio m-line), first match wins:
  1. `LIQUIDCALL/48000/1` (also accept the legacy name `LIQUIDCODEC` from pre-rename builds) →
     the participant is a **Liquid** sender. Answer it with the offered PT (96 today) and
     `a=fmtp:<pt> pack=tight;fpp=1`. If the offer also lists `opus/48000/2`, answer that PT too,
     after LIQUIDCALL, so the server can send Opus down to the iPhone (§5).
  2. `opus/48000/2` → **Opus** participant. Answer `a=fmtp:111 minptime=10;useinbandfec=1`.
  3. Neither → `400 Bad Request`, then close.
- Payload format (from `liquid_codec.h` / `audio_decoder_liquid.cc`): 24 kHz mono, 80 ms frames,
  RTP clock 48000, one frame = 3840 ticks. `tight` = 12 × 11-bit codes per frame, MSB first, zero
  pad, no header (17/33/66 B for 1/2/4 frames). `u16` = `u8 ver=1, u8 N, u16 LE frame seq, N×12 u16
  LE codes` (28 B for N=1). The receiver infers pack and N from the payload length; each sender
  keeps its own settings. `usedtx=1` means silence gaps with no packets.

## 4. Voice swap negotiation (SDP only)

Attributes live in the audio m-section and are keyed to the LIQUIDCALL payload type:

| SDP | Meaning |
|---|---|
| offer `a=x-liquidcall-voice:<pt> <voice>` | The iPhone asks that **its own outgoing voice** be converted to `<voice>`. Absent, unknown, or `me` = no conversion. |
| answer `a=x-liquidcall-voices:<pt> me,female,male` | Voices the server has loaded. Only list voices that are ready. |
| answer `a=x-liquidcall-voice:<pt> <active>` | The voice the server really applies. If `<voice>` isn't loaded, answer `me` (fall back to no swap; never fail the call). |

Voice ids: `me`, `female` (Piper `en_US-amy-medium`), `male` (Piper `en_US-lessac-medium`).
A change during the call is a re-offer (message 8). The server switches at the next 80 ms frame
boundary and MUST NOT restart DTLS/ICE. Opus participants never carry these attributes; they hear
the converted voice.

## 5. Media routing in a room (MVP)

| From → to | Server work |
|---|---|
| Liquid → Opus (iPhone → Chrome) | Depacketize LIQUIDCALL → decode (int8 decoder, CPU) → 24 kHz PCM → voice swap if active voice ≠ `me` → resample 48 kHz → Opus encode 20 ms → RTP to Chrome. This is the one MCU-style leg. |
| Opus → Liquid (Chrome → iPhone) | MVP: forward Opus RTP as is (rewrite SSRC/PT/seq/timestamp) when the iPhone offered Opus as a second codec; the iPhone's NetEq decodes any negotiated PT. Later: Opus → PCM → LIQUIDCALL encode. If the iPhone offered LIQUIDCALL only, the MVP sends nothing down and logs it. |
| Liquid → Liquid | Forward RTP untouched (SFU) when the speaker's active voice is `me`. With a voice active: decode → swap → LIQUIDCALL encode (later; MVP rooms have one iPhone). |
| Opus → Opus | Forward (SFU). |

RTCP: terminate per leg. Send receiver reports and NACK/PLI per leg; never pass a browser RTCP
packet to the iPhone.

## 6. Limits and timers

| Item | Value |
|---|---|
| Participants per room | 2 in the MVP (third HELLO → `486 Busy Here`) |
| Rooms | 8 |
| HELLO → OFFER | 45 s, then `480 Temporarily Unavailable` and close (matches `call_setup_timeout_ms`) |
| Idle socket | 120 s |
| BYE ack wait | 2 s |

## 7. Conformance

`tools/signaling_ref.py` is an executable reference of §1.1 (gate, path, query) and §2 (state
machine, rooms, busy, BYE/CANCEL, renegotiation, voice fallback in SDP). It has no media.
`tests/test_signaling_ref.py` drives it with the exact iPhone and browser message sequences
above. The Go server SHOULD pass the same vectors (`tests/vectors.json`).
