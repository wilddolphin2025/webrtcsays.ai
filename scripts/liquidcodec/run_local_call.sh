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
  --audio-codec=liquidcodec --liquid-decode=ane
  --liquid-model-dir="${MODELS}"
)
{
  echo "${BIN} --mode=callee --user_name=alice --room_name=liquid ${COMMON[*]} --record-dir=${RECORD}/callee 127.0.0.1:${PORT}"
  echo "${BIN} --mode=caller --user_name=bob --target_name=alice --room_name=liquid ${COMMON[*]} --inject-wav=${INJECT} --record-dir=${RECORD}/caller --duration=${DURATION} 127.0.0.1:${PORT}"
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
  --inject-wav="${INJECT}" \
  --record-dir="${RECORD}/caller" \
  --duration="${DURATION}" \
  "127.0.0.1:${PORT}" >"${RECORD}/caller.log" 2>&1 || true
sleep 1
cleanup
trap - EXIT

missing=0
for f in "${RECORD}/caller/codec_in_24k.wav" "${RECORD}/callee/codec_out_24k.wav"; do
  if [[ ! -f "${f}" ]]; then
    echo "Missing ${f}" >&2
    missing=1
  fi
done
if [[ "${missing}" == "1" ]]; then
  echo "Logs: ${RECORD}/callee.log ${RECORD}/caller.log" >&2
  exit 1
fi
echo "codec in:  ${RECORD}/caller/codec_in_24k.wav"
echo "codec out: ${RECORD}/callee/codec_out_24k.wav"
