# WebRTCSays stage-1 demo

Authenticated browser demo for room `room101` and callee `Slim`. This directory is the deployable bundle. It does not build or replace `directcall`, and it does not change coturn.

Nothing here was run against the production VPS. Install it on the host, by hand, after the checks below.

## In scope

- Cookie login, plus per-client `Authorization: Bearer` and `?token=` for clients that cannot set a header.
- `https://rtc.wilddolphin.us/demo/` for room101 / Slim.
- `directcall` under systemd, using the binary already installed plus the PR 18 signaling bind.
- Legacy exact `/ws` behind the same gate. `/ws/` (the signal gateway) stays as it is.
- Retire `/var/www/directcall/index.html` only after a root-only backup. That file is unused and contains static TURN credentials.

## Out of scope

- TURN REST credential minting, `use-auth-secret`, and rotating the live static TURN user.
- Steam login.
- An H.264 rebuild, and any edit under `examples/direct/` or the speech/codec tree.
- Running `scripts/deploy/deploy_directcall_remote.sh` or the Deploy DirectCall workflows. Those rewrite nginx and can restart coturn.

`/demo/turn` only forwards the static account DevOps injects on the host. A minted `<expiry>:<name>` username would be rejected by the current coturn, which has no `use-auth-secret`. The JSON says `mode: static`, `temporary: true`, `minted: false`. The static account remains temporary.

## Layout

| Path | Role |
|---|---|
| `auth/demo_auth.py` | Login and `auth_request` service |
| `auth/Dockerfile` | Image. Loopback publish is in the run command, not the image |
| `nginx/webrtcsays-demo-http.conf` | `log_format` and login rate zone. HTTP context |
| `nginx/webrtcsays-demo-locations.conf` | Locations to include in the rtc server only |
| `page/index.html` | Demo page. No TURN secret and no raw IP |
| `systemd/webrtcsays-demo.service` | Existing binary, loopback config copy |
| `host/` | Check-first helpers. They do not SSH |

## Secrets

The auth container runs as uid 8793. A root-owned mode `0600` file is not readable by that user. The directory is mode `0700` and the secret files are mode `0600`, both owned by uid 8793. `directcall.env` stays root-owned because systemd reads it as root; the auth process does not open it.

```bash
install -d -o 8793 -g 8793 -m 0700 /etc/webrtcsays-demo
openssl rand -hex 32 | install -o 8793 -g 8793 -m 0600 /dev/stdin /etc/webrtcsays-demo/session.key
# Replace these placeholders on the host. The service rejects the word REPLACE.
printf '%s\n' 'REPLACE_WITH_A_HOST_PASSWORD' | install -o 8793 -g 8793 -m 0600 /dev/stdin /etc/webrtcsays-demo/password
printf '%s\n' 'founder-iphone REPLACEWITHTHEHOSTTOKEN' | install -o 8793 -g 8793 -m 0600 /dev/stdin /etc/webrtcsays-demo/tokens
install -o 8793 -g 8793 -m 0600 deploy/webrtcsays-demo/config/turn.example.json /etc/webrtcsays-demo/turn.json
# then replace the username and credential with the existing static coturn account
DEMO_AUTH_CONF=/etc/webrtcsays-demo bash deploy/webrtcsays-demo/host/set-auth-perms.sh --apply
```

The service rejects placeholders (`REPLACE`, `EXAMPLE`, `<...>`). If `turn.json` is missing, asks for REST mode, or uses a numeric `<expiry>:` username, `/demo/turn` returns 503 and does not invent a credential. URIs must be hostnames (`turn.wilddolphin.us`), not raw or LAN addresses.

Tokens are the Bearer and query credentials. The shared password works only on the login form. Prefer Bearer. Query tokens exist for clients that cannot set a header. The browser page does not put a token in the WebSocket URL.

## Auth service

Build and publish on loopback only. `--init` is required so health-check children are reaped.

```bash
docker build -t webrtcsays-demo-auth:stage1 deploy/webrtcsays-demo/auth
bash deploy/webrtcsays-demo/host/install-auth.sh          # prints the command
DEMO_AUTH_INSTALL_CONFIRM=loopback-8793 \
  bash deploy/webrtcsays-demo/host/install-auth.sh --apply
curl -fsS http://127.0.0.1:8793/healthz
```

The container listens on `0.0.0.0:8793` inside the network namespace. Docker publishes `127.0.0.1:8793` only. Do not publish `0.0.0.0:8793`.

Checks: constant-time token compare, login rate limit, HttpOnly/Secure/SameSite=Lax session cookie (12 h), CSRF on the login form, Origin check, no open redirect (`next` may only be `/demo/`, `/demo/index.html`, or `/demo/turn`). Logs are `METHOD path status`. Query strings, passwords, tokens, and cookies are not logged.

## WebSocket path

`directcall` speaks HTTP/1.1 Upgrade, then masked client text frames and unmasked server text frames. `HELLO:Slim`, `200 OK`, `INVITE`, `WAITING`, `OFFER`, `ANSWER`, `ICE`, `BYE`. There is no 2-byte length prefix on this socket. That prefix is only the raw TCP framing. The handshake ignores the URL path (`examples/direct/main.cc`).

nginx rewrites `/demo/ws` and legacy `/ws` to `GET / HTTP/1.1`, strips the query, and clears `Authorization`, `Cookie`, and `X-Demo-Token` before they reach the callee. `auth_request` runs first. A missing or bad credential is 401 and does not open an upstream socket. The access log format is `demo_noargs` (`$request_method` and `$uri` only) on the login, TURN, page, and WebSocket locations. Keep `error_log` at `warn` or `error`. Do not enable `debug` or `rewrite_log`; those levels can record a request line.

The page uses `wss://<location.host>/demo/ws` on HTTPS and refuses to connect when `location.hostname` is an IP address.

## nginx

Back up first. The helper defaults to a dry run. It comments only an unauthenticated `location = /ws` that proxies to port 3459, inserts one include above the babelfish include, and copies two new files. It does not reload nginx, delete `sites-enabled/default`, or edit other vhosts.

```bash
install -d -m 0700 /root/webrtcsays-demo-backups
TS=$(date +%Y%m%d%H%M%S)
nginx -T | sha256sum > /root/webrtcsays-demo-backups/nginx-T-$TS.sha256
ss -tlnup > /root/webrtcsays-demo-backups/ss-$TS.txt

python3 deploy/webrtcsays-demo/host/nginx_stage.py \
  --site /etc/nginx/sites-enabled/rtc.wilddolphin.us.conf \
  --nginx-dir /etc/nginx \
  --backup-dir /root/webrtcsays-demo-backups \
  --locations deploy/webrtcsays-demo/nginx/webrtcsays-demo-locations.conf \
  --http-conf deploy/webrtcsays-demo/nginx/webrtcsays-demo-http.conf

python3 deploy/webrtcsays-demo/host/nginx_stage.py --apply \
  --site /etc/nginx/sites-enabled/rtc.wilddolphin.us.conf \
  --nginx-dir /etc/nginx \
  --backup-dir /root/webrtcsays-demo-backups \
  --locations deploy/webrtcsays-demo/nginx/webrtcsays-demo-locations.conf \
  --http-conf deploy/webrtcsays-demo/nginx/webrtcsays-demo-http.conf

nginx -t && systemctl reload nginx
```

Do not copy the site file to `sites-enabled/*.bak` or `sites-available/*.bak`. nginx includes those directories, and the backup would be a second server. The helper writes only under `--backup-dir` (default `/root/webrtcsays-demo-backups`). When the site path is a symlink, it edits the target and leaves the symlink in place. The login rate-limit status `429` is set on the demo login and logout locations, not for every vhost.

Install the page after the auth container is healthy:

```bash
install -d -m 0755 /srv/webrtcsays-demo
install -m 0644 deploy/webrtcsays-demo/page/index.html /srv/webrtcsays-demo/index.html
```

Reload does not drop existing connections. `nginx -t` failure must not be followed by reload.

## directcall systemd

The unit starts the installed binary:

`/opt/wsai-m156/src/out/release/directcall --config /opt/directcall3-dev/config.demo.json`

Loopback signaling is the PR 18 patch (`69289a61`, `signaling_bind`). Install that patched binary before starting this unit. `address` only supplies the port the in-process bridge dials. It does not make `bind` use 127.0.0.1. `config.demo.json` sets both `address` to `127.0.0.1:3458` and `signaling_bind` to `127.0.0.1`. An unpatched binary ignores `signaling_bind` and keeps listening on `0.0.0.0`. ufw is not a substitute. Media ICE is unchanged. Do not edit the local whisper sources while installing this.

`EnvironmentFile=-/etc/webrtcsays-demo/directcall.env` is optional. The previous process may already be dead, in which case its environment cannot be captured. Write an empty root-owned mode `0600` file and continue. The installed config uses absolute paths. Do not reconstruct variables by editing whisper sources. If the live process was started through `run-directcall.sh`, point `ExecStart` at that script and keep the same config path and `EnvironmentFile`.

```bash
python3 deploy/webrtcsays-demo/host/prepare_loopback_config.py \
  --src /opt/directcall3-dev/config.talkingface5.json \
  --dst /opt/directcall3-dev/config.demo.json
PREPARE_DEMO_CONFIG_CONFIRM=copy-loopback \
  python3 deploy/webrtcsays-demo/host/prepare_loopback_config.py --apply \
  --src /opt/directcall3-dev/config.talkingface5.json \
  --dst /opt/directcall3-dev/config.demo.json

# Optional. A missing pid writes an empty file and does not touch whisper sources.
DEMO_ENV_OUT=/etc/webrtcsays-demo/directcall.env \
  bash deploy/webrtcsays-demo/host/capture-directcall-env.sh --apply

install -m 0644 deploy/webrtcsays-demo/systemd/webrtcsays-demo.service \
  /etc/systemd/system/webrtcsays-demo.service
systemctl daemon-reload
# Stop a stray session process only when it is still running and the unit
# file and env file exist. Skip kill when the process is already gone.
systemctl enable --now webrtcsays-demo.service
```

Do not start the unit while an old process still holds 3459. Confirm `ss` shows `127.0.0.1:3458` and `127.0.0.1:3459` after start. If it shows `0.0.0.0`, the PR 18 binary is not the one running.

## Retire the old page

```bash
DEMO_PAGE_TARGET=/var/www/directcall/index.html \
DEMO_PAGE_BACKUP_DIR=/root/webrtcsays-demo-backups \
RETIRE_DEMO_PAGE_CONFIRM=backup-then-delete \
  bash deploy/webrtcsays-demo/host/retire-old-demo-page.sh
```

The backup is mode `0600` in a `0700` directory. The script does not print the file. It refuses to store the backup under a web root.

## Tests on the host, after install

1. `curl -sI https://rtc.wilddolphin.us/demo/` is 302 to `/demo/login?next=/demo/` and the Location has no token.
2. Wrong password returns 401 and no `wsdemo` cookie. The right password or client token returns 303 and `Set-Cookie` with `HttpOnly`, `Secure`, and `SameSite=Lax`.
3. `curl -sI https://rtc.wilddolphin.us/demo/ws` is 401. With `Authorization: Bearer …`, the upgrade is 101, `HELLO:Slim` returns `200 OK`, and the upstream request line is `GET / HTTP/1.1` without the token.
4. The same Bearer and `?token=` checks on legacy `/ws`. `/ws/`, `/`, `/babelfish/healthz`, and `/karaoke/healthz` still answer as they did before the reload.
5. `/demo/turn` without a credential is 302 or 401 and the body has no username or credential. With a credential it is JSON, `Cache-Control: no-store`, `mode` `static`, `minted` false, `temporary` true.
6. `?token=` does not appear in `/var/log/nginx/demo-auth.log` or `demo-ws.log`.
7. Browser call from Chrome, then iPhone Safari on cellular: tap Start call, room101, two-way audio. `BYE` ends it. TURN is best-effort until the static account is injected; the page stays on STUN if `/demo/turn` is 503.

Local checks, with no production access:

```bash
python3 -m unittest discover -s deploy/webrtcsays-demo/tests -v
```

## Rollback

1. nginx: copy the backup from `/root/webrtcsays-demo-backups` over the real site file (the symlink target when `sites-enabled` is a link), `nginx -t && systemctl reload nginx`. Do not leave a `.bak` in `sites-enabled` or `sites-available`. Remove the two new conf files if you want the tree unchanged. Do not delete other sites.
2. Auth container: `docker rm -f webrtcsays-demo-auth`.
3. Callee: `systemctl disable --now webrtcsays-demo`. Start the previous binary with `config.talkingface5.json` and the captured environment if the demo callee must go back to the session process. Do not run `deploy_directcall_remote.sh` to roll back.
4. Old demo page: leave it deleted. Restore the `0600` backup only into a non-web directory if you need the bytes. Do not put it back under `/var/www`.
5. coturn was not changed. There is no coturn rollback in this stage.

## Native clients

This stage does not patch the native client. When a client can set a host, port, path, and header, the signaling URL is `wss://rtc.wilddolphin.us/demo/ws` with `Authorization: Bearer <token>`. Otherwise use `wss://rtc.wilddolphin.us/demo/ws?token=<token>` and keep that URL out of logs. Fetch TURN with the same Bearer header. Use the hostname only.
