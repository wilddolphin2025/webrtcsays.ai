#!/usr/bin/env bash
# Cross-compile examples/direct:directcall for linux arm64 (Steam Frame).
# Run from a gclient checkout whose src/ directory is this repository.
# Requires depot_tools on PATH and a completed `gclient sync`.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${ROOT_DIR}/out/linux_arm64"
DIST_DIR="${ROOT_DIR}/dist/direct-linux-arm64"
JOBS="${JOBS:-2}"

if [[ ! -f "${ROOT_DIR}/build/config/BUILDCONFIG.gn" ]]; then
  echo "build/config is missing. Run gclient sync from the parent of src/ first." >&2
  exit 1
fi

export DEPOT_TOOLS_UPDATE=0
if [[ -d "${HOME}/depot_tools" ]]; then
  export PATH="${HOME}/depot_tools:${PATH}"
fi

if [[ -f "${ROOT_DIR}/build/linux/sysroot_scripts/install-sysroot.py" ]]; then
  python3 "${ROOT_DIR}/build/linux/sysroot_scripts/install-sysroot.py" --arch=arm64
fi

# Chromium enables experimental ELF CREL for arm64. That encoding segfaults
# in the Linux loader on aarch64 (crbug.com/376278218, which already excludes
# 32-bit arm). Drop it so the binary runs under qemu and on SteamOS.
python3 - "${ROOT_DIR}/build/config/compiler/BUILD.gn" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = 'current_cpu != "arm" && current_cpu != "s390x"'
new = 'current_cpu != "arm" && current_cpu != "arm64" && current_cpu != "s390x"'
if old not in text:
    raise SystemExit("CREL guard not found in compiler BUILD.gn")
if 'current_cpu != "arm64"' not in text[text.find("Enable ELF CREL"):text.find("Enable ELF CREL")+500]:
    text = text.replace(old, new, 1)
    path.write_text(text)
    print("disabled ELF CREL for arm64")
else:
    print("ELF CREL already disabled for arm64")
PY

GN_BIN=""
for candidate in \
  "${ROOT_DIR}/buildtools/linux64/gn" \
  "$(command -v gn || true)"; do
  if [[ -n "${candidate}" && -x "${candidate}" ]]; then
    GN_BIN="${candidate}"
    break
  fi
done
if [[ -z "${GN_BIN}" ]]; then
  echo "gn was not found" >&2
  exit 1
fi

# H.264 via OpenH264 + the FFmpeg decoder branding that includes H.264.
# AV1 (libaom) and HEVC stay off. Pulse is the PipeWire compatibility layer.
# libc++ is linked statically (use_custom_libcxx). The Debian arm64 sysroot
# keeps the glibc requirement older than current SteamOS.
read -r -d '' GN_ARGS <<'EOF' || true
target_os="linux"
target_cpu="arm64"
is_debug=false
is_clang=true
use_sysroot=true
use_custom_libcxx=true
treat_warnings_as_errors=false
symbol_level=1
rtc_include_tests=false
rtc_build_examples=true
rtc_build_tools=false
rtc_include_opus=true
rtc_enable_protobuf=false
enable_js_protobuf=false
enable_libaom=false
proprietary_codecs=true
rtc_use_h264=true
rtc_use_h265=false
ffmpeg_branding="Chrome"
rtc_include_pulse_audio=true
rtc_use_pipewire=false
rtc_use_x11=false
arm_control_flow_integrity="none"
rtc_use_speech_audio_devices=false
rtc_enable_symbol_export=true
is_component_build=false
EOF

"${GN_BIN}" gen "${OUT_DIR}" --args="${GN_ARGS}"

NINJA_BIN=""
for candidate in \
  "${ROOT_DIR}/third_party/ninja/ninja" \
  "$(command -v ninja || true)"; do
  if [[ -n "${candidate}" && -x "${candidate}" ]]; then
    NINJA_BIN="${candidate}"
    break
  fi
done
if [[ -z "${NINJA_BIN}" ]]; then
  echo "ninja was not found" >&2
  exit 1
fi

"${NINJA_BIN}" -C "${OUT_DIR}" -j "${JOBS}" directcall

rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}"
cp "${OUT_DIR}/directcall" "${DIST_DIR}/directcall"
if [[ -f "${OUT_DIR}/libdirect.so" ]]; then
  cp "${OUT_DIR}/libdirect.so" "${DIST_DIR}/libdirect.so"
fi
if command -v llvm-strip >/dev/null 2>&1; then
  llvm-strip "${DIST_DIR}/directcall" || true
  [[ -f "${DIST_DIR}/libdirect.so" ]] && llvm-strip "${DIST_DIR}/libdirect.so" || true
fi
chmod +x "${DIST_DIR}/directcall"

cat > "${DIST_DIR}/run.sh" <<'EOF'
#!/bin/sh
# Usage: ./run.sh [room] [duration_seconds] [fps]
# Token: set DIRECT_SIGNAL_TOKEN or pass TOKEN_FILE=/path/to/token.
# Live socket today is wss://rtc.wilddolphin.us:443/ws.
# The login gate (not live yet; currently HTTP 404) will be:
#   SIGNAL_URL=wss://rtc.wilddolphin.us:443/demo/ws TOKEN_FILE=./token ./run.sh room101 60
set -eu
ROOM="${1:-room101}"
DURATION="${2:-60}"
FPS="${3:-30}"
DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
SIGNAL_URL="${SIGNAL_URL:-wss://rtc.wilddolphin.us:443/ws}"
TOKEN_ARGS=""
if [ -n "${TOKEN_FILE:-}" ]; then
  TOKEN_ARGS="--token-file=${TOKEN_FILE}"
fi
export LD_LIBRARY_PATH="${DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "${DIR}/directcall" \
  --mode=caller \
  --user_name=frame \
  --target_name=Slim \
  --room_name="${ROOM}" \
  --encryption \
  --signal-url="${SIGNAL_URL}" \
  ${TOKEN_ARGS} \
  --stun=stun:stun.l.google.com:19302 \
  --res=1920x1080 \
  --fps="${FPS}" \
  --video-source=pattern \
  --stats=5 \
  --duration="${DURATION}"
EOF
chmod +x "${DIST_DIR}/run.sh"

GLIBC="unknown"
if command -v llvm-objdump >/dev/null 2>&1; then
  GLIBC=$(llvm-objdump -T "${DIST_DIR}/directcall" 2>/dev/null | awk '/GLIBC_/ {print $NF}' | sort -uV | tail -1)
fi

cat > "${DIST_DIR}/README.md" <<EOF
# directcall linux arm64

Headless caller for the WebRTCsays demo on a Steam Frame (SteamOS, aarch64).
The sender aims at 1920x1080 and 30 fps (60 is allowed) from a test pattern
when no camera is present. \`--video-source=screen\` uses the X11 root window
when \`DISPLAY\` is set and otherwise falls back to the pattern. H.264 is
listed first, with VP8 and VP9 kept as fallbacks. \`--codec=h264\` offers
H.264 only. The robot on \`/ws\` today answers a forced H.264 offer with BYE
and, when H.264 is only preferred, sends VP8 at 640x480.

## Join room101

\`\`\`sh
./run.sh room101 60
\`\`\`

That expands to:

\`\`\`sh
./directcall \\
  --mode=caller \\
  --user_name=frame \\
  --target_name=Slim \\
  --room_name=room101 \\
  --encryption \\
  --signal-url=wss://rtc.wilddolphin.us:443/ws \\
  --stun=stun:stun.l.google.com:19302 \\
  --res=1920x1080 \\
  --fps=30 \\
  --video-source=pattern \\
  --stats=5 \\
  --duration=60
\`\`\`

\`www.wilddolphin.us/demo.html\` is currently a request-a-demo page and does not
open a WebSocket. The live direct-protocol socket is
\`wss://rtc.wilddolphin.us:443/ws\` (HELLO, 200 OK, INVITE with room_name,
WAITING, OFFER/ANSWER/ICE). DevOps is adding a login gate at
\`wss://rtc.wilddolphin.us:443/demo/ws\`; that path currently returns HTTP 404.
When it is live, set \`SIGNAL_URL\` or \`--signal-url\` to it and pass
\`--token-file\`.

## Token

The gate will require a bearer token. Put it in a file (mode 600) or in
\`DIRECT_SIGNAL_TOKEN\`. It is sent as \`Authorization: Bearer\` and as
\`?token=\`, and it is not written to the log.

\`\`\`sh
TOKEN_FILE=/path/to/token ./run.sh room101 120 30
\`\`\`

TLS certificates are verified. If the Frame's trust store is not in the usual
place, pass \`--ca-file=/etc/ssl/certs/ca-certificates.crt\`.

## ICE

STUN and TURN are not compiled in. \`--stun=\` may be repeated. TURN uses
\`--turns=turns:host:5349?transport=tcp;username;password\` and verifies the
TURN TLS certificate. Hosts must be DNS names, not IP addresses.

## Stats

\`--stats=5\` prints one JSON object per line on stdout. The exit summary
includes ICE pair type, RTT, time to ICE connected, time to first audio,
send/receive resolution, fps, bitrate, codec, H.264 profile-level-id,
encode/decode milliseconds per frame, quality limitation (none/cpu/bandwidth),
frames dropped, jitter, loss, concealment, NACK and PLI.

## Runtime

glibc requirement observed on this package: ${GLIBC}.
Pulse (\`libpulse.so.0\`) is optional; PipeWire's pulse layer satisfies it.
\`--audio=dummy\` runs with no audio server.
EOF

tar -C "${ROOT_DIR}/dist" -czf "${ROOT_DIR}/dist/direct-linux-arm64.tar.gz" direct-linux-arm64
echo "packaged ${ROOT_DIR}/dist/direct-linux-arm64.tar.gz"
ls -lh "${DIST_DIR}/directcall" "${ROOT_DIR}/dist/direct-linux-arm64.tar.gz"
