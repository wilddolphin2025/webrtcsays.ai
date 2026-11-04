#!/usr/bin/env bash
# End-to-end DIRECT protocol shutdown checks on loopback:
#   1. caller SIGTERM mid-call  -> BYE / 200 OK, callee keeps listening
#   2. caller SIGKILL mid-call  -> callee notices the dead socket and hangs up
#   3. callee SIGTERM mid-call  -> caller gets BYE, callee exits in time
#   4. caller frozen (SIGSTOP)  -> no FIN, callee hangs up once media is gone
#   4. caller frozen (SIGSTOP)  -> no FIN, callee hangs up once media is gone
set -u
BIN=${1:-out/m156/directcall}
CERT=${CERT:-cert.pem}
KEY=${KEY:-key.pem}
PORT=${PORT:-3466}
LOG=${LOG:-/tmp/direct_shutdown}
export DIRECT_LOG_LEVEL=${DIRECT_LOG_LEVEL:-info}
mkdir -p "$LOG"
COMMON=(--encryption --webrtc_cert_path="$CERT" --webrtc_key_path="$KEY"
        --no-whisper --no-llama --no-video --no-websocket_signaling
        --media_grace_ms=3000)

fail() { echo "FAIL: $*"; exit 1; }
count() { grep -c "$2" "$1" 2>/dev/null || true; }
wait_log() {  # file pattern timeout_s [min_count]
  local want=${4:-1}
  for _ in $(seq 1 $(( $3 * 10 ))); do
    [ "$(count "$1" "$2")" -ge "$want" ] && return 0
    sleep 0.1
  done
  return 1
}
CALLS=0
start_callee() {
  CALLS=0
  "$BIN" --mode=callee --user_name=robot "${COMMON[@]}" 127.0.0.1:$PORT > "$LOG/callee.log" 2>&1 &
  CALLEE=$!
  wait_log "$LOG/callee.log" "Server listening on port" 10 || fail "callee did not listen"
}
start_caller() {  # tag
  "$BIN" --mode=caller --user_name=human --target_name=robot "${COMMON[@]}" 127.0.0.1:$PORT > "$LOG/caller_$1.log" 2>&1 &
  CALLER=$!
  CALLS=$((CALLS + 1))
  wait_log "$LOG/callee.log" "PeerConnectionState -> connected" 20 $CALLS || fail "$1: call did not connect"
}

start_callee

# 1. Graceful caller hangup.
start_caller term
kill -TERM $CALLER
wait $CALLER; rc=$?
grep -q "Shutdown complete" "$LOG/caller_term.log" || fail "1: caller did not finish shutdown (rc=$rc)"
wait_log "$LOG/callee.log" "Ending call (remote BYE)" 5 || fail "1: callee did not see BYE"
echo "PASS 1: caller SIGTERM -> BYE, exit $rc"

# 2. Caller crashes; callee must free itself without waiting for ICE timeouts.
sleep 1
start_caller kill
kill -KILL $CALLER; wait $CALLER 2>/dev/null
wait_log "$LOG/callee.log" "Ending call (signaling connection lost)" 5 || fail "2: callee kept dead call"
echo "PASS 2: caller SIGKILL -> callee hung up"

# Callee must accept a new call after both teardowns.
sleep 1
start_caller again
echo "PASS 2b: callee accepted a follow-up call"

# 3. Callee shuts down mid-call.
t0=$(date +%s)
kill -TERM $CALLEE
wait $CALLEE; rc=$?
elapsed=$(( $(date +%s) - t0 ))
grep -q "Shutdown complete" "$LOG/callee.log" || fail "3: callee did not finish shutdown (rc=$rc)"
[ $elapsed -le 10 ] || fail "3: callee shutdown took ${elapsed}s"
wait_log "$LOG/caller_again.log" "Ending call (remote BYE)" 5 || fail "3: caller did not see BYE"
kill -TERM $CALLER 2>/dev/null; wait $CALLER 2>/dev/null
echo "PASS 3: callee SIGTERM -> BYE to caller, exit $rc in ${elapsed}s"

# 4. Frozen peer: TCP stays open, only media liveness can detect it.
start_callee
start_caller frozen
kill -STOP $CALLER
wait_log "$LOG/callee.log" "Ending call (media" 30 || fail "4: callee kept a frozen call"
grep -m1 "Ending call (media" "$LOG/callee.log" | sed 's/^/      /'
kill -KILL $CALLER 2>/dev/null; wait $CALLER 2>/dev/null
kill -TERM $CALLEE; wait $CALLEE
echo "PASS 4: frozen caller -> callee hung up on media loss"
