# Frame viewer

A separate answering page and loopback room router. The Frame runs the existing `directcall` caller and the browser answers. The Slim robot routes stay on their current locations. No SFU.

Nothing in this directory was installed on the production VPS. DevOps reviews and installs it. Leave `scripts/deploy/deploy_directcall_remote.sh` unused.

## Depends on

| Change | Commit | What this needs from it |
|---|---|---|
| PR 17 auth gate, backup/secret ownership, and Babelfish palette | `1dfe20dbc1088e6f51f74e2baf99c1201d23d0e2` on `cursor/stage1-demo-auth-0e0e` | `auth_request /_demo_auth`, `@demo_login`, `demo_noargs`, cookie `wsdemo` with `Path=/`, login and call pages at 14px/1.4 |
| PR 18 loopback bind | `69289a61609c73b4a68b0244e43012e3c31d4c72` on `cursor/signaling-loopback-bind-0e0e` | Slim callee on `127.0.0.1:3459` only. The viewer router is a different process on `127.0.0.1:3461` |

Both pull requests are unmerged. The live host is still auth `5fe64695` plus host workarounds. Apply the steps below when installing PR 17 at `1dfe20dbc1088e6f51f74e2baf99c1201d23d0e2`, after `d715d3f97d` (backup and secret ownership) and `1dfe20dbc1` (palette). DevOps keeps the palette pages. This branch does not contain `deploy/webrtcsays-demo/`.

PR 17 `authenticated_client` allows `X-Original-URI` only when the path is exactly `/demo/`, `/demo/index.html`, `/demo/turn`, `/demo/ws`, or `/ws`. A viewer URI would be 401 with a valid cookie until the patch below is applied. `safe_next` allows only `/demo/`, `/demo/index.html`, and `/demo/turn`, so login would return to `/demo/` instead of the viewer.

From the PR 17 worktree at that commit:

```bash
patch -p1 < deploy/frame-view/auth/demo-auth-view-paths.patch
```

The patch adds three regexes and `view_path()`. It leaves the cookie flags, the login HTML, the call page, and the five existing paths as they are.

- Login `next` may also be `/demo/view/<room>` (`[A-Za-z0-9][A-Za-z0-9_-]{0,63}`). Room labels `ws` and `assets` are reserved.
- `X-Original-URI` may also be that page, `/demo/view/assets/view.css`, `/demo/view/assets/view.js`, or `/demo/view/ws/<room>`.
- `/`, `/ws/`, `/demo/ws/extra`, and `/demo/view/ws` stay rejected. A token in `next` stays rejected because `?` and `#` still fall back to `/demo/`.

Auth still accepts the signed `wsdemo` cookie, `Authorization: Bearer`, and `X-Demo-Token` (the query token nginx copied before clearing `$args`). nginx `X-Original-URI` is `$uri`. Secrets stay in `/etc/webrtcsays-demo`, mode `0600`, uid 8793. This repository has no secret files.

## Proposed public URLs

- Viewer page: `https://rtc.wilddolphin.us/demo/view/frame101`
- Viewer socket: `wss://rtc.wilddolphin.us:443/demo/view/ws/frame101`
- Router bind: `127.0.0.1:3461` only

The room is the last path label. `frame101` is the suggested room so it stays off Slim's `room101`. Two peers are paired only when both use that same label. A peer in another room cannot receive those messages. A second caller in the same room gets `486 Busy Here`.

## Frame command

Header-only bearer token. Omit `--token-query`. Omit `--codec=h264` until a browser in this room has negotiated H.264. Forcing H.264 made the current Slim robot send `BYE`.

```sh
./directcall \
  --mode=caller \
  --user_name=frame \
  --target_name=viewer \
  --room_name=frame101 \
  --encryption \
  --signal-url=wss://rtc.wilddolphin.us:443/demo/view/ws/frame101 \
  --token-file=/path/to/token \
  --stun=stun:stun.l.google.com:19302 \
  --turns-file=/etc/webrtcsays-demo/frame-turns.json \
  --res=1920x1080 \
  --fps=30 \
  --video-source=pattern \
  --stats=5 \
  --duration=60
```

`--turns-file` is optional. Copy `config/turns.example.json` on the host and replace the placeholders there. The process logs that TURN is configured and does not log the username or credential. `/demo/turn` remains the current static temporary account. This page does not mint REST credentials.

## What DevOps installs

1. Merge this PR on its own. Leave the palette commits on `cursor/stage1-demo-auth-0e0e`.
2. Apply `auth/demo-auth-view-paths.patch` on top of `1dfe20dbc1088e6f51f74e2baf99c1201d23d0e2`.
3. Include `nginx/frame-view-locations.conf` in the rtc server block, immediately after the PR 17 include, above the babelfish include and above `location /`.
4. Install the page beside the Slim page, in its own directory:

```bash
install -d -m 0755 /srv/webrtcsays-frame-view
install -m 0644 deploy/frame-view/page/index.html /srv/webrtcsays-frame-view/index.html
install -m 0644 deploy/frame-view/page/view.css /srv/webrtcsays-frame-view/view.css
install -m 0644 deploy/frame-view/page/view.js /srv/webrtcsays-frame-view/view.js
install -d -m 0755 /opt/webrtcsays-frame-view
install -m 0755 deploy/frame-view/router.py /opt/webrtcsays-frame-view/router.py
install -m 0644 deploy/frame-view/systemd/frame-view-router.service \
  /etc/systemd/system/frame-view-router.service
systemctl daemon-reload
systemctl enable --now frame-view-router.service
nginx -t && systemctl reload nginx
```

5. Confirm `ss` shows `127.0.0.1:3461` and that Slim is still `127.0.0.1:3459` after PR 18. Confirm `https://rtc.wilddolphin.us/demo/` still reaches Slim, and `/ws/` is unchanged.

The WebSocket location runs `auth_request` before `proxy_pass`, clears `Authorization`, `Cookie`, and the query, and sets `X-Demo-Auth: ok` itself. The router rejects any connection without that exact header, rejects query strings, and accepts only the room path. Access logs use `demo_noargs` (`$uri` only). Keep `error_log` at `warn` or `error`.

The viewer stylesheet repeats the palette tokens from `1dfe20dbc1` (`#1B2838`, `#C7D5E0`, `14px/1.4`) in `page/view.css`. Element ids on this page are `view-connect`, `view-disconnect`, `view-status`, `view-codec`, `view-remote`, and `view-log`.

## Browser

Open the viewer URL after the PR 17 login. Press Connect. The page does not call `getUserMedia`. Connect is the playback gesture for desktop Safari and iPhone. If playback is still blocked, tap the video. The page prefers H.264 and shows the codec `getStats` actually negotiated, including `profile-level-id` when present. TURN is fetched from the existing `/demo/turn` cookie endpoint. A 503 leaves the page on STUN.

## Checks already run here

`python3 -m unittest discover -s deploy/frame-view/tests -v` covers refused unauthenticated upgrades, query-string rejection, OFFER/ANSWER pairing, busy and reconnect, and cross-room isolation.

The auth patch was applied to a temporary copy of `1dfe20dbc1088e6f51f74e2baf99c1201d23d0e2`. `deploy/webrtcsays-demo/tests/test_auth.py` then passed its 18 tests, including the existing path allowlist and the Babelfish palette check. On that copy, a bearer token is accepted for `/demo/view/frame101`, `/demo/view/assets/view.css`, `/demo/view/assets/view.js`, and `/demo/view/ws/frame101`. `/demo/ws/extra`, `/ws/`, and a `next` value that contains `?` still return to the closed behavior. The nginx suite from that branch was not re-run here. No production host was contacted.
