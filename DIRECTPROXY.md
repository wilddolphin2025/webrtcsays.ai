# DIRECTPROXY

`directproxy` adds native browser signaling support to `directcall` by exposing a WebSocket endpoint while preserving the original direct app-to-app TCP behavior.

## Overview

- Existing direct signaling remains available over TCP (`directcall` listener, default `3456`).
- A WebSocket endpoint (default `3457`) is exposed for browser clients.
- The WS side is bridged into the existing direct protocol path, so call logic stays in `directcall`.
- Browser and native clients use the same message vocabulary (`HELLO`, `INVITE`, `WAITING`, `OFFER`, `ANSWER`, `ICE`, `BYE`, `CANCEL`, etc.).

## Ports and Transport

- Direct signaling TCP: `127.0.0.1:3456` (default in examples)
- WebSocket signaling: `ws://<host>:3457`

The WS proxy uses text frames on the browser side and forwards messages to direct TCP with a 2-byte big-endian length prefix expected by direct signaling internals.

## Configuration

New `directcall` options:

- `--websocket_signaling` / `--no-websocket_signaling`
- `--websocket_port=<port>`
- `--talking_face=<path_to_image>`

JSON config equivalents are supported in `directcall.config.example.json`.

## Direct Protocol Messages

Common messages:

- `HELLO:<target_id>`
- `200 OK`
- `400 Bad Request`
- `480 Temporarily Unavailable`
- `486 Busy Here`
- `INVITE:<json_or_payload>`
- `WAITING`
- `OFFER:<sdp>`
- `ANSWER:<sdp>`
- `ICE:<mline>:<candidate>`
- `BYE`
- `CANCEL`

Optional/legacy side messages still recognized by protocol family:

- `REGISTER:<id>:<room>`
- `USERS`
- `ADDRESS:<id>:ip:port`
- `WHISPER[xx]<text>`
- `LLAMA[xx]<text>`
- `FACE:<base64 image>` — replace the talking-face avatar
- `VOICE:<clip>` — switch the robot's TTS voice mid-call (see below)

## Ending a Call

Either side may hang up at any time; the rules are symmetric.

| Situation | Sender | Receiver |
|-----------|--------|----------|
| Hang up an established call | `BYE` | replies `200 OK`, tears down |
| Abort during setup | `CANCEL` | replies `BYE` (legacy ack), tears down |
| Hangup acknowledged | — | sender closes the TCP connection on `200 OK`/`BYE`, or after `--hangup_ack_timeout_ms` (2 s) |

Teardown also starts without any message when:

- the signaling TCP connection closes or resets (the peer process died);
- TCP keepalive gives up (peer vanished without FIN: 15 s idle + 3 × 5 s probes);
- the PeerConnection reports `failed`, or stays `disconnected` longer than
  `--media_grace_ms` (10 s) — a frozen laptop or lost network;
- media is not connected within `--call_setup_timeout_ms` (45 s) of `INVITE`.

`directcall` handles `SIGTERM`, `SIGINT` and `SIGHUP` the same way: it sends
`BYE` to the active peer, waits for the acknowledgement, joins its threads and
exits within 10 s. A second signal exits immediately. `systemctl stop` therefore
hangs calls up cleanly instead of leaving the remote side to time out.

The WebSocket bridge pings the browser every 15 s and drops it after 45 s of
silence. When the browser side goes away for any reason it sends `BYE` to
`directcall` on the browser's behalf; when `directcall` closes, the browser gets
a WebSocket close frame (1000, or 1001 during shutdown).

`scripts/ci/test_direct_shutdown.sh` and `scripts/ci/test_ws_bridge.py` exercise
these paths end to end on loopback.

## Personalized Voices

With a whillats build that has `WHILLATS_POCKET_TTS`, the robot speaks with a
voice cloned from a 6–15 s reference clip (Kyutai Pocket TTS). Clips live in
`POCKET_TTS_VOICES_DIR` on the server; peers can only pick clips by bare file
name, never by path.

- Per call: `INVITE:{"agent":"audio","voice":"grandpa.wav"}`
- Mid-call: `VOICE:grandpa.wav` (JSON bridge: `{"type":"voice","name":"grandpa.wav"}`)
- Default: `--tts_voice=grandpa.wav` or `"tts_voice"` in the config file

Only clone voices you have the speaker's documented consent for, and tell
listeners that the voice is synthetic (Kyutai's acceptable-use policy).

## Browser Call Flow (Expected Sequence)

1. Browser connects to `ws://host:3457`
2. Browser sends `HELLO:<target>`
3. Server replies `200 OK`
4. Browser sends `INVITE:{"agent":"audio","room_name":"room101","video":true}`
5. Server replies `WAITING`
6. Browser creates offer, sends `OFFER:<sdp>`
7. Server replies `ANSWER:<sdp>`
8. Both sides exchange `ICE:<mline>:<candidate>`
9. Session ends with `BYE` or `CANCEL`

## Talking Face Mode

When built with speech/talking-face support and started with:

`--talking_face=/path/to/image`

the callee can provide a synthetic video source generated from the image and speech activity. If unavailable, code can fall back to synthetic echo video source (depending on runtime branch behavior).

## Sample Browser Page

A complete sample is provided at:

- `ws_test.html`

It includes:

- local/remote audio+video elements
- strict direct protocol sequencing (`HELLO -> 200 OK -> INVITE -> WAITING -> OFFER`)
- SDP offer normalization for this branch
- ICE exchange
- remote video state probes

Minimal JS signaling skeleton:

```html
<script>
const ws = new WebSocket("ws://127.0.0.1:3457");
ws.onopen = () => ws.send("HELLO:Slim");
ws.onmessage = async (e) => {
  const msg = String(e.data || "");
  if (msg.startsWith("200 OK")) {
    ws.send('INVITE:{"agent":"audio","room_name":"room101","video":true}');
  } else if (msg.startsWith("WAITING")) {
    // createOffer + setLocalDescription
    // ws.send(`OFFER:${offerSdp}`);
  } else if (msg.startsWith("ANSWER:")) {
    // setRemoteDescription(answer)
  } else if (msg.startsWith("ICE:")) {
    // addIceCandidate(...)
  }
};
</script>
```

## Run Example

Start callee with WS signaling and talking face:

```bash
./out/directproxy_whillats/directcall \
  --mode=callee \
  --user_name=Slim \
  --room_name=room101 \
  --encryption \
  --video \
  --webrtc_cert_path=cert.pem \
  --webrtc_key_path=key.pem \
  --websocket_signaling \
  --websocket_port=3457 \
  --talking_face="/absolute/path/RobotPhoneLogo.jpeg" \
  127.0.0.1:3456
```

Then open:

- `http://127.0.0.1:8000/ws_test.html`

