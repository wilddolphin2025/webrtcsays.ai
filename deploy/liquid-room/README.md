# LiquidCall room101 demo: www.wilddolphin.us/demo.html → rtc.wilddolphin.us → LiquidServer

Goal: the founder opens `https://www.wilddolphin.us/demo.html?room=room101#demo` in Chrome, signs in,
and talks to the LiquidCall iPhone app in room101. Phase B adds LiquidServer, which turns the iPhone's
LiquidCall audio (in the voice picked on the phone) into Opus for Chrome.

Nothing in this directory has been installed anywhere. DevOps installs it after the founder approves in
the DevOps chat. See `DEVOPS.md` for the exact steps and `SIGNALING.md` for the protocol LiquidServer
implements.

## What is live today (checked 2026-11-23 04:05 ET with unauthenticated curl only)

| URL | Today | Notes |
|---|---|---|
| `www.wilddolphin.us/demo.html` | 200, static "Demo Available on Demand" placeholder | No script, no socket. Separate static host (public_html). |
| `rtc.wilddolphin.us/demo/` | 302 → `/demo/login?next=/demo/` | Browser **caller** page for the Slim robot callee (`/demo/ws` → `directcall` on 127.0.0.1:3459). One caller at a time (`486`). |
| `rtc.wilddolphin.us/demo/ws`, `/ws` | 401 without a credential | Gate = `demo_auth.py` (cookie, Bearer, `?token=`). |
| `rtc.wilddolphin.us/demo/view/<room>` | 302 → `/demo/login?next=/demo/view/<room>` | Frame viewer: browser **answering** page. Live files = PR 22 `0f1a701` (receive only, no microphone). |
| `rtc.wilddolphin.us/demo/view/ws/<room>` | 401 without a credential | `frame-view-router` on 127.0.0.1:3461 (PR 22 router `9d8c9d2f`): pairs one native caller with one browser viewer per room and forwards the direct text protocol. |
| `rtc.wilddolphin.us/demo/turn` | 302/401 without a credential | Static temporary coturn account, `uris` on turn.wilddolphin.us. |

Live `demo_auth.py` = develop `389a84a2` + `deploy/frame-view/auth/demo-auth-view-paths.patch`
(sha256 `8711311d…`). This branch folds that patch in (same bytes as live) and adds the room paths.

## Phase A (no new server): iPhone (Opus) ⇄ Chrome through the frame-view router

```
Chrome  /demo/view/room101  ── VIEW ──▶  frame-view-router ◀── HELLO… ──  iPhone (caller)
             answers OFFER                  (pairs per room)               wss /demo/view/ws/room101, Bearer
                     ◀──────────── Opus, peer to peer (TURN on turn.wilddolphin.us) ────────────▶
```

- www: `www/demo.html` replaces the placeholder. It sends the visitor to
  `https://rtc.wilddolphin.us/demo/view/<room>` (`room` validated, default room101, `#demo` dropped,
  `via=room` selects phase B). The login keeps the room (`next=/demo/view/room101` is already allowed).
- rtc: no change for one-way audio (iPhone → Chrome). For two-way, install the PR 22 `290e925` viewer
  static files (Microphone button, auto-listen). DevOps already staged those
  (`viewer-290e925-rollout-plan.md`); they are awaiting the founder's approval.
- iPhone: **the installed app cannot do this yet.** It only dials a Mac over raw TCP
  (`DirectCaller::Connect(host, port)`, `--no-websocket_signaling`) and always uses
  `--audio-codec=liquidcall`, offering LIQUIDCALL only. `direct.framework` itself can do Opus
  (`--audio-codec=opus`) and WebSocket signaling (`--signal-url`, `--token-env`, `--turns-file`,
  `DirectCallerClient::Connect` → `ConnectSignalUrl`). The LiquidCall app needs a "Server room" call
  mode (owned by the LiquidCall bot). Chrome cannot decode LIQUIDCALL, so Phase A is Opus only and
  has **no voice swap**.

## Phase B: LiquidServer on its own VPS

```
Chrome  /demo/room/room101 (caller) ─┐                         ┌─ iPhone (caller), LIQUIDCALL 96 (+ Opus 111)
                                     ▼                         ▼
             rtc nginx /demo/room/ws/room101: auth, strip credentials, add X-Liquid-Gate
                                     ▼
             Caddy cb.wilddolphin.us/ws/room101 (rtc source IP only; ls. also accepted) → 127.0.0.1:7880
                                     ▼
             LiquidServer (Go/Pion): callee for both; LIQUIDCALL → decode → voice swap → Opus → Chrome
             media on udp/7882 (ICE mux) and tcp/7881, public IP, no TURN needed
```

- Both clients stay plain direct-protocol **callers** (HELLO → 200 OK → INVITE → WAITING → OFFER →
  ANSWER → ICE → BYE). LiquidServer plays the callee, like Slim. No new protocol for the page or the app.
- `page/index.html` is the Phase B browser page (`/demo/room/<room>`): audio only, joins with a click,
  shows codec, kbps, jitter, loss and RTT.
- The iPhone uses `wss://rtc.wilddolphin.us:443/demo/room/ws/room101` with its Bearer token, offers
  `LIQUIDCALL` first and Opus second, and keeps voice selection in SDP
  (`a=x-liquidcall-voice:<pt> <voice>`). LiquidServer sends Opus down to the iPhone until it has a
  server-side LIQUIDCALL encoder.
- SFU or MCU: a plain SFU is not enough because Chrome can't decode LIQUIDCALL and the swap needs
  decoded audio. LiquidServer forwards what it can (Opus → Opus, Liquid → Liquid with voice `me`, Opus
  down to the iPhone) and runs one decode → swap → Opus leg per Liquid speaker per Opus listener.
  It never mixes, so it is not a full MCU.

## Layout

| Path | Role |
|---|---|
| `SIGNALING.md` | Wire spec LiquidServer implements (transport, auth, messages, SDP, voice swap, routing) |
| `DEVOPS.md` | Exact install, check and rollback steps for both phases |
| `www/demo.html` | Static redirect page for www.wilddolphin.us |
| `page/index.html` | Phase B caller page for `/demo/room/<room>` |
| `nginx/liquid-room-locations.conf`, `nginx/liquid-room-http.conf` | Phase B rtc nginx locations and upstream |
| `nginx/liquid-gate.conf.example` | Shape of the root-only gate header include |
| `caddy/Caddyfile.liquidserver` | Phase B Caddy site on the LiquidServer VPS |
| `host/*.sh` | Dry-run-first install and check scripts |
| `tools/signaling_ref.py` | Executable reference of the signaling (no media) |
| `tests/` | `python3 -m unittest discover -s deploy/liquid-room/tests` (nginx, node and openssl needed for all of them) |

## Tests

```bash
python3 -m unittest discover -s deploy/liquid-room/tests -v
python3 -m unittest discover -s deploy/webrtcsays-demo/tests -v   # auth incl. new room paths
python3 -m unittest discover -s deploy/frame-view/tests -v
```
