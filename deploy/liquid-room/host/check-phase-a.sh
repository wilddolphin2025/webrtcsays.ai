#!/usr/bin/env bash
# Phase A checks with no credentials (safe to run from anywhere). Exit 1 on the first failure.
set -uo pipefail
fail() { echo "FAIL $*"; exit 1; }
code() { curl -s -o /dev/null -w '%{http_code}' -m 10 "$@"; }
body="$(curl -fsS -m 10 https://www.wilddolphin.us/demo.html)" || fail "www demo.html"
grep -q 'id="demo-redirect"' <<<"$body" || fail "www demo.html is not the redirect page"
grep -q 'https://rtc.wilddolphin.us' <<<"$body" || fail "redirect origin missing"
loc="$(curl -s -o /dev/null -w '%{redirect_url}' -m 10 https://rtc.wilddolphin.us/demo/view/room101)"
[ "$(code https://rtc.wilddolphin.us/demo/view/room101)" = 302 ] || fail "viewer page is not behind login"
case "$loc" in *"/demo/login?next=/demo/view/room101") ;; *) fail "login next lost the room: $loc";; esac
[ "$(code https://rtc.wilddolphin.us/demo/view/ws/room101)" = 401 ] || fail "viewer socket open without auth"
[ "$(code https://rtc.wilddolphin.us/demo/ws)" = 401 ] || fail "Slim socket open without auth"
echo "phase A checks passed"
