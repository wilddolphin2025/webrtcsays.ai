#!/usr/bin/env bash
# iOS WebRTC.framework and direct.framework for the voice-swap app.
#
# Run this from the webrtc checkout (the Mac src/ worktree). It does not run
# gclient. build.sh runs `gclient sync -D --force`, which breaks a worktree
# whose third_party (and the other DEPS checkouts) are symlinks into the main
# tree. Do not use build.sh there.
#
# Default output is out/ios_voiceswap. A second directory leaves that build
# alone:
#   OUT=out/ios_dualcodec scripts/build_voiceswap_frameworks.sh
#   scripts/build_voiceswap_frameworks.sh --out=out/ios_dualcodec
#   scripts/build_voiceswap_frameworks.sh --out=ios_dualcodec
#
# Gn args match `./build.sh ios release cetacall`: device arm64, Opus on,
# CetaCall on, speech devices off, unsigned. Models are not fetched here.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

OUT_ARG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out)
      if [ -z "${2:-}" ]; then
        echo "ERROR: --out requires a directory, for example out/ios_dualcodec" >&2
        exit 1
      fi
      OUT_ARG="$2"
      shift 2
      ;;
    --out=*)
      OUT_ARG="${1#*=}"
      if [ -z "${OUT_ARG}" ]; then
        echo "ERROR: --out= requires a directory, for example out/ios_dualcodec" >&2
        exit 1
      fi
      shift
      ;;
    -h|--help)
      echo "Usage: OUT=out/ios_dualcodec $0"
      echo "       $0 --out=out/ios_dualcodec"
      echo "Default OUT is out/ios_voiceswap. Does not run gclient."
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [ -n "${OUT_ARG}" ]; then
  OUT="${OUT_ARG}"
else
  OUT="${OUT:-out/ios_voiceswap}"
fi

case "${OUT}" in
  out/*) ;;
  *) OUT="out/${OUT}" ;;
esac
OUT_NAME="${OUT#out/}"
case "${OUT_NAME}" in
  *[!A-Za-z0-9_-]*|"")
    echo "ERROR: OUT must be out/<name>, such as out/ios_voiceswap or out/ios_dualcodec (got ${OUT})" >&2
    exit 1
    ;;
esac

echo "iOS frameworks output: ${OUT}"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "iOS frameworks are built on a Mac. This script does not run gclient." >&2
  exit 1
fi

DEPOT="${ROOT}/third_party/depot_tools"
if [ -d "${DEPOT}" ]; then
  PATH="${DEPOT}:${PATH}"
  export PATH
fi
GN="gn"
if [ -x "${DEPOT}/gn" ]; then
  GN="${DEPOT}/gn"
fi
if ! command -v "${GN}" >/dev/null 2>&1 && [ ! -x "${GN}" ]; then
  echo "ERROR: gn not found. Use the gn in third_party/depot_tools in this worktree." >&2
  echo "Do not run build.sh here: it runs gclient sync -D --force." >&2
  exit 1
fi

# Same args as build.sh for `ios release cetacall`.
IOS_ARGS='target_environment="device" target_os="ios" target_cpu="arm64" ios_deployment_target="16.4.0" ios_enable_code_signing=false use_custom_libcxx=false treat_warnings_as_errors=false is_debug=false rtc_include_opus=true rtc_build_examples=true rtc_enable_symbol_export=true mac_deployment_target="15.0" mac_min_system_version="15.0" rtc_use_speech_audio_devices=false rtc_use_cetacall=true'

echo "gn gen ${OUT}"
"${GN}" gen "${OUT}" --args="${IOS_ARGS}"
echo "ninja -C ${OUT} sdk:framework_objc examples:direct_framework directcall"
ninja -C "${OUT}" -j"${IOS_NINJA_JOBS:-1}" sdk:framework_objc examples:direct_framework directcall

for fw in WebRTC.framework direct.framework; do
  if [ ! -d "${OUT}/${fw}" ]; then
    found="$(find "${OUT}" -name "${fw}" -type d | head -n 1)"
    if [ -z "${found}" ]; then
      echo "ERROR: ${fw} was not produced under ${OUT}" >&2
      exit 1
    fi
  fi
done

echo "Built ${ROOT}/${OUT}/WebRTC.framework"
echo "Built ${ROOT}/${OUT}/direct.framework"
