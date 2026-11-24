#!/usr/bin/env bash
# Phase B checks with no credentials. LIQUID_HOST defaults to cb.wilddolphin.us (ls. still accepted).
set -uo pipefail
fail() { echo "FAIL $*"; exit 1; }
code() { curl -s -o /dev/null -w '%{http_code}' -m 10 "$@"; }
host="${LIQUID_HOST:-cb.wilddolphin.us}"
[ "$(code https://rtc.wilddolphin.us/demo/room/room101)" = 302 ] || fail "room page is not behind login"
loc="$(curl -s -o /dev/null -w '%{redirect_url}' -m 10 https://rtc.wilddolphin.us/demo/room/room101)"
case "$loc" in *"/demo/login?next=/demo/room/room101") ;; *) fail "login next lost the room: $loc";; esac
[ "$(code https://rtc.wilddolphin.us/demo/room/ws/room101)" = 401 ] || fail "room socket open without auth"
[ "$(code "https://$host/healthz")" = 200 ] || fail "$host/healthz"
c="$(code -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
      -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' "https://$host/ws/room101")"
[ "$c" = 404 ] || fail "$host/ws/room101 reachable from this address ($c); expected 404 outside the rtc allowlist"
echo "phase B checks passed"
