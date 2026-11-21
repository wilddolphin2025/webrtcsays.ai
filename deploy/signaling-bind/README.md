# Loopback signaling bind

There is no existing flag that makes directcall's signaling sockets listen on
127.0.0.1.

`address` is parsed for a port. `DirectCalleeClient::setupWebRTCListener`
then replaces it with `0.0.0.0:<port>` before `bind`. Both listeners pass
`INADDR_ANY`:

- TCP signaling (`examples/direct/callee.cc`), port from `address` (3458 on the demo)
- WebSocket signaling (`examples/direct/main.cc` `DirectWebSocketProxy`), `websocket_port` (3459)

A firewall rule is not the same thing. The sockets would still accept a
connection that reaches the host another way.

## Config

`signaling_bind` is optional. Omit it, or set `0.0.0.0`, and the binary keeps
the old all-interface listen.

```json
"address": "0.0.0.0:3458",
"websocket_port": 3459,
"signaling_bind": "127.0.0.1"
```

`signaling_bind` must be a dotted IPv4 address. A hostname is rejected and the
process exits before it listens, so a typo cannot fall open onto every
interface. The value binds only those two TCP sockets.

The in-process WebSocket bridge dials that same address. With `127.0.0.1` it
dials `127.0.0.1:3458`. With the historical default it still dials `127.0.0.1`,
which an `INADDR_ANY` listener accepts.

Changing `address` to `127.0.0.1:3458` without `signaling_bind` does not change
the bind. The stage-1 config copy that only rewrites `address` is not this fix.

## Media ICE

ICE candidates come from the WebRTC peer connection's UDP sockets and from
`publishAddressToSignalingServer`, which walks the host interfaces. Neither
path reads `signaling_bind`. This patch does not set a port allocator, a
network ignore mask, or a candidate filter. Public and LAN media addresses
stay as they are. Loopback is still skipped when that function advertises an
address, which is the existing behavior.

## What to build on the VPS

The running callee is the checkout at `97702ebe` (`fix(direct): do not
increment a volatile shutdown counter`) plus local edits in
`modules/audio_device/speech/whisper_audio_device.cc` and
`whisper_audio_device.h`. Do not check out `feat/webrtc-m156`. That branch now
contains merged PR #16 (H.264 caller, TLS hostname checks, call stats). Building
it would ship those unrelated changes and can disturb the whisper edits.

Stay on `97702ebe`. Apply only `signaling-bind-97702ebe.patch`. It does not
touch the whisper files, codecs, or `p2p/`.

```bash
cd /opt/wsai-m156/src
git rev-parse HEAD    # 97702ebe6a...
TS=$(date +%Y%m%d%H%M%S)
install -d -m 0700 /root/webrtcsays-demo-backups
cp -a out/release/directcall "/root/webrtcsays-demo-backups/directcall.$TS"
cp -a out/release/libdirect.so "/root/webrtcsays-demo-backups/libdirect.so.$TS"
patch -p1 --dry-run < signaling-bind-97702ebe.patch
patch -p1 < signaling-bind-97702ebe.patch
```

Confirm the dry-run ninja plan does not compile whisper, codecs, or ICE:

```bash
ninja -C out/release -n directcall
```

Then build. `directcall` links `examples:directlib` (`libdirect.so`). Ninja
recompiles the directcall translation units that include `option.h`
(`callee.cc`, `caller.cc`, `client.cc`, `peer.cc`, `direct.cc`, `option.cc`,
`wsock.cc`, `dirobjc.mm`, `main.cc`) and relinks `libdirect.so` and
`directcall`. It does not rebuild the WebRTC static libraries or the whisper
objects, because those sources do not include `option.h` and this patch does
not change them.

```bash
ninja -C out/release directcall
```

That is four to nine C++ files in `examples/direct/` plus two links. It is not
`gn gen` and it is not a WebRTC rebuild.

After the new binary is installed, logs must show:

```
Signaling TCP listening on 127.0.0.1:3458
WebSocket signaling listening on ws://127.0.0.1:3459
```

`ss -ltnp` for those ports must show `127.0.0.1`, not `0.0.0.0`. Rollback is
the backed-up `directcall` and `libdirect.so`, and removing `signaling_bind`
from the config.

## Test without the WebRTC tree

```bash
g++ -std=c++17 -Wall -Werror -I examples/direct \
  examples/direct/signaling_bind_test.cc -o /tmp/signaling_bind_test
/tmp/signaling_bind_test
```
