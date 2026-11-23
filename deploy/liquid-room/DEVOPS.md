# DevOps handoff: room101 demo (Phase A now, Phase B when LiquidServer is ready)

Every step needs the founder's approval in the DevOps chat first. Nothing here restarts Slim,
coturn, the frame-view router or the karaoke/babelfish services. No secret is printed by any script.

Source: webrtcsays.ai branch `liquidserver/demo-room101` (this PR). Paths below are relative to the
repo root.

---------------------------------------------------------------------------------------------------
## Phase A: www redirect (+ PR 22 viewer files for two-way audio)

### A1. www.wilddolphin.us/demo.html

| File | Destination | Owner/mode |
|---|---|---|
| `deploy/liquid-room/www/demo.html` | `<www web root>/demo.html` (replaces the "Demo Available on Demand" placeholder) | same as the other site pages, 0644 |

```bash
# On the www host, from a checkout or a copied deploy/liquid-room directory:
SITE_ROOT=~/public_html bash deploy/liquid-room/host/install-www-demo.sh                 # dry run, prints hashes
SITE_ROOT=~/public_html INSTALL_WWW_DEMO_CONFIRM=replace-demo-html \
  bash deploy/liquid-room/host/install-www-demo.sh                                        # backup to ~/site-backups, then install
```

Backups go to `~/site-backups` (0700), never `public_html`. Rollback: the script prints the
`install -m 0644 ~/site-backups/demo.html.<ts> ~/public_html/demo.html` line.

No rtc change is needed for the redirect: `/demo/view/<room>` and its login `next` are already live.

### A2. Two-way audio in the viewer (already staged by you)

Apply the existing plan `viewer-290e925-rollout-plan.md` (PR 22 `290e925`: `index.html`, `view.js`,
`view.css` into `/srv/webrtcsays-frame-view/`, no restarts). Without it the iPhone → Chrome direction
works and Chrome sends only the silent placeholder.

### A checks (no credentials)

```bash
bash deploy/liquid-room/host/check-phase-a.sh
```
- `https://www.wilddolphin.us/demo.html` contains `id="demo-redirect"` and the rtc origin.
- `https://rtc.wilddolphin.us/demo/view/room101` → 302 to `/demo/login?next=/demo/view/room101`.
- `/demo/view/ws/room101` and `/demo/ws` → 401.
- In Chrome (founder): open `https://www.wilddolphin.us/demo.html?room=room101#demo` → sign-in page →
  room101 viewer page shows "waiting". Then the founder taps Call in the iPhone app (once the app has
  the Server room mode). Nobody else places calls.

---------------------------------------------------------------------------------------------------
## Phase B: LiquidServer behind rtc (only after the founder approves phase 1 on the LiquidServer VPS)

Prerequisites: DNS `ls.wilddolphin.us A <liquidserver-vps IPv4>`; Caddy on the LiquidServer VPS; ufw
`80/tcp 443/tcp 7881/tcp 7882/udp` (22/tcp stays); the LiquidServer binary and unit from the
LiquidServer repo (env `LIQUID_HTTP_BIND=127.0.0.1:7880`, `LIQUID_GATE_FILE=/etc/liquidserver/gate`,
`LIQUID_PUBLIC_IP=<public IPv4>`, `LIQUID_ICE_UDP_PORT=7882`, `LIQUID_ICE_TCP_PORT=7881`). Until the
Go server exists, `deploy/liquid-room/tools/signaling_ref.py` (signaling only, no media) can stand in
on 127.0.0.1:7880 as user `liquid` to prove the path end to end.

### B1. Gate secret (rtc → LiquidServer)

```bash
# rtc.wilddolphin.us, root:
bash deploy/liquid-room/host/make-gate-secret.sh            # /root/liquid-gate.hex, 0600, prints only a hash prefix
# From the founder's Mac (the value goes through the pipe, never to a terminal):
scp deploy/liquid-room/host/install-gate-liquid.sh root@<liquidserver-vps>:/root/
ssh root@rtc.wilddolphin.us 'cat /root/liquid-gate.hex' | ssh root@<liquidserver-vps> 'bash /root/install-gate-liquid.sh'
# -> /etc/liquidserver/gate, root:liquid 0640. Compare the two printed sha256 prefixes.
```

### B2. Caddy on the LiquidServer VPS

| File | Destination |
|---|---|
| `deploy/liquid-room/caddy/Caddyfile.liquidserver` (with `RTC_VPS_IP_PLACEHOLDER` replaced) | `/etc/caddy/Caddyfile` (backup to `/root/backups/Caddyfile.<ts>`) |

```bash
RTC_VPS_IP=<rtc.wilddolphin.us IPv4> bash deploy/liquid-room/host/install-liquid-caddy.sh            # validate only
RTC_VPS_IP=<rtc.wilddolphin.us IPv4> INSTALL_CADDY_CONFIRM=liquidserver-site \
  bash deploy/liquid-room/host/install-liquid-caddy.sh
```
`/ws/*` is reverse-proxied to 127.0.0.1:7880 only from the rtc address; `/healthz` answers `ok`;
everything else is 404. No access log (it would record `X-Liquid-Gate`).

### B3. rtc.wilddolphin.us: auth paths, page, nginx

| File | Destination | Notes |
|---|---|---|
| `deploy/webrtcsays-demo/auth/demo_auth.py` | the auth image (rebuild like the frame-viewer deploy, new tag e.g. `webrtcsays-demo-auth:stage1-room-<commit>`) | Diff vs live (`8711311d…`): adds `ROOM_PAGE_RE`, `ROOM_WS_RE`, `room_path()`, and `/demo/room/<room>` as a login `next`. Nothing else. Recreate the container with the live `docker inspect` run arguments (loopback 127.0.0.1:8793, uid 8793, `--init`, read-only, same `/etc/webrtcsays-demo` mount). Existing WebSockets are unaffected; new logins fail for ~1 s. |
| `deploy/liquid-room/page/index.html` | `/srv/liquid-room/index.html` 0644 | via script |
| `deploy/liquid-room/nginx/liquid-room-locations.conf` | `/etc/nginx/snippets/liquid-room-locations.conf` 0644 | via script |
| `deploy/liquid-room/nginx/liquid-room-http.conf` | `/etc/nginx/conf.d/liquid-room-http.conf` 0644 | via script; needs the DNS record, else `nginx -t` fails (safe) |
| generated | `/etc/nginx/liquid-gate.conf` root 0600 | `proxy_set_header X-Liquid-Gate "<hex>";` from `/root/liquid-gate.hex` |
| one line | rtc site file, right after the frame-view include | `include /etc/nginx/snippets/liquid-room-locations.conf;` |

```bash
bash deploy/liquid-room/host/install-room-rtc.sh                                   # dry run: shows site file and anchor line
INSTALL_ROOM_CONFIRM=room-locations bash deploy/liquid-room/host/install-room-rtc.sh
nginx -t && systemctl reload nginx                                                 # never reload if -t fails
```
The script backs up the site file to `/root/backups/liquid-room-<ts>/` with `rollback.sh`, keeps no
`.bak` in `sites-enabled`, and inserts the include once (idempotent).

### B checks

```bash
bash deploy/liquid-room/host/check-phase-b.sh        # from the box or the Mac (not from rtc)
```
- `/demo/room/room101` → 302 to `/demo/login?next=/demo/room/room101`; `/demo/room/ws/room101` → 401.
- `https://ls.wilddolphin.us/healthz` → 200; `/ws/room101` from a non-rtc address → 404.
- On rtc, as root, with the founder's token file (never echo it):
  `curl -s -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer $(cat <tokenfile>)" -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' https://rtc.wilddolphin.us/demo/room/ws/room101` → `101`.
- `grep -c token= /var/log/nginx/demo-room.log` → 0. `/demo/`, `/demo/ws`, `/demo/view/*`, `/ws/`, `/`,
  `/babelfish/healthz`, `/karaoke/healthz` answer as before the reload.
- Media check is the founder's: Chrome on `/demo/room/room101`, Join room, then he taps Call on the iPhone.

### B rollback
1. rtc: `/root/backups/liquid-room-<ts>/rollback.sh` (restores the site file, removes the 3 files, `nginx -t && reload`).
   Auth: re-run the previous image tag with the same arguments.
2. LiquidServer VPS: restore `/root/backups/Caddyfile.<ts>`, `systemctl reload caddy`; stop the LiquidServer unit.
3. www (phase A): restore `~/site-backups/demo.html.<ts>`.

## Local tests already run (box, no production access)

```
python3 -m unittest discover -s deploy/liquid-room/tests -v        # 26 tests, incl. real nginx + TLS upstream, node page tests
python3 -m unittest discover -s deploy/webrtcsays-demo/tests -v    # auth + nginx; the deploy-script contract test needs scripts/ (sparse checkout)
python3 -m unittest discover -s deploy/frame-view/tests -v
```
