#!/usr/bin/env bash
# Callee and caller on 127.0.0.1 using the binary and models ./build.sh wrote.
# Generates a 24 kHz tone when --inject-wav is omitted. Does not fetch or compile.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=layout.sh
source "${SCRIPT_DIR}/layout.sh"

DURATION=8
PORT=3456
INJECT=""
RECORD="${LC_TREE}/scripts/liquidcodec/out/local-call"
PREPARE=0
BIN="${LC_BIN}"
MODELS="${LC_MODELS}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration) DURATION="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --inject-wav) INJECT="$2"; shift 2 ;;
    --record-dir) RECORD="$2"; shift 2 ;;
    --bin) BIN="$2"; shift 2 ;;
    --model-dir) MODELS="$2"; shift 2 ;;
    --prepare-only) PREPARE=1; shift ;;
    -h|--help)
      echo "usage: $0 [--duration N] [--inject-wav PATH] [--record-dir DIR] [--port N] [--prepare-only]"
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "${RECORD}/callee" "${RECORD}/caller"
if [[ -z "${INJECT}" ]]; then
  python3 "${SCRIPT_DIR}/tone_wav.py" "${RECORD}/inject.wav" 3
  INJECT="${RECORD}/inject.wav"
fi

COMMON=(
  --no-encryption --no-websocket_signaling --no-video --no-whisper --no-llama
  --audio-codec=liquidcodec --liquid-decode=ane --aec=off
  --stats=1
  --liquid-model-dir="${MODELS}"
  --inject-wav="${INJECT}"
)
{
  echo "${BIN} --mode=callee --user_name=alice --room_name=liquid ${COMMON[*]} --record-dir=${RECORD}/callee 127.0.0.1:${PORT}"
  echo "${BIN} --mode=caller --user_name=bob --target_name=alice --room_name=liquid ${COMMON[*]} --record-dir=${RECORD}/caller --duration=${DURATION} 127.0.0.1:${PORT}"
} > "${RECORD}/commands.txt"

if [[ "${PREPARE}" == "1" ]]; then
  echo "Prepared ${RECORD} (directcall not started)."
  cat "${RECORD}/commands.txt"
  exit 0
fi

if [[ ! -x "${BIN}" ]]; then
  echo "directcall is not at ${BIN}." >&2
  echo "From the gclient parent: ./build.sh release liquidcodec" >&2
  exit 1
fi
if [[ ! -f "${MODELS}/liquidcodec.json" ]]; then
  echo "No manifest at ${MODELS}/liquidcodec.json." >&2
  echo "From the gclient parent: ./build.sh release liquidcodec" >&2
  exit 1
fi

echo "Callee ${BIN} on 127.0.0.1:${PORT} for ${DURATION}s"
"${BIN}" --mode=callee --user_name=alice --room_name=liquid \
  "${COMMON[@]}" \
  --record-dir="${RECORD}/callee" \
  "127.0.0.1:${PORT}" >"${RECORD}/callee.log" 2>&1 &
callee=$!
cleanup() { kill "${callee}" 2>/dev/null || true; wait "${callee}" 2>/dev/null || true; }
trap cleanup EXIT
sleep 2
"${BIN}" --mode=caller --user_name=bob --target_name=alice --room_name=liquid \
  "${COMMON[@]}" \
  --record-dir="${RECORD}/caller" \
  --duration="${DURATION}" \
  "127.0.0.1:${PORT}" >"${RECORD}/caller.log" 2>&1 || true
sleep 1
cleanup
trap - EXIT

python3 - "${RECORD}" "${DURATION}" <<'PY'
import json, re, sys
from pathlib import Path
record = Path(sys.argv[1])
duration = float(sys.argv[2])
expected = duration * 1000.0 / 80.0
bad = re.compile(r"Check failed|outside the codebook|session #[0-9]+ ended|SIGABRT")
failed = False
for name in ("callee.log", "caller.log"):
    text = (record / name).read_text(errors="replace")
    hit = bad.search(text)
    if hit:
        print(f"{name}: {hit.group(0)}", file=sys.stderr)
        failed = True

def packets(path):
    if not path.is_file():
        print(f"missing {path}", file=sys.stderr)
        return None
    return json.loads(path.read_text()).get("packets")

pairs = [
    (record / "caller" / "encoder_stats.json", record / "callee" / "decoder_stats.json"),
    (record / "callee" / "encoder_stats.json", record / "caller" / "decoder_stats.json"),
]
for enc, dec in pairs:
    ep, dp = packets(enc), packets(dec)
    if ep is None or dp is None:
        failed = True
        continue
    if ep < 0.9 * expected:
        print(f"{enc}: packets {ep} < 90% of {expected:.0f}", file=sys.stderr)
        failed = True
    if abs(ep - dp) > max(2, 0.01 * max(ep, dp)):
        print(f"packet mismatch {enc}={ep} {dec}={dp}", file=sys.stderr)
        failed = True
sys.exit(1 if failed else 0)
PY
