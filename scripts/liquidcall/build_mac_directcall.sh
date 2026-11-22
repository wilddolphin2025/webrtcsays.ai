#!/usr/bin/env bash
# Build the macOS LiquidCall directcall (release, Core ML) into its own
# output directory, without touching out/release or out/lowrate.
#
#   scripts/liquidcall/build_mac_directcall.sh [out_name]   (default: lowlat)
#
# Produces src/out/<out_name>/directcall and libdirect.dylib with the same
# gn args as the earlier low-rate build:
#   is_debug=false symbol_level=0 rtc_include_opus=true
#   rtc_enable_symbol_export=true rtc_build_examples=true
#   rtc_use_speech_audio_devices=false mac_deployment_target="15.0"
#   target_cpu="arm64" rtc_use_liquidcall=true
#
# Refuses to build when a running directcall was started from that output
# directory: relinking libdirect.dylib under a live callee can crash it.
# Run the callee from it with:
#   DIRECTCALL=<src>/out/<out_name>/directcall <LiquidCall>/scripts/run_mac_callee.sh
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This builds the macOS directcall and needs Xcode on a Mac." >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")/../.." && pwd)"   # the webrtc src/ checkout
NAME="${1:-lowlat}"
case "${NAME}" in
  release|ios_arm64|*/*|"")
    echo "Pick a dedicated output name (not release/ios_arm64): ${NAME}" >&2
    exit 2
    ;;
esac
OUT="out/${NAME}"

if pgrep -f "${HERE}/${OUT}/directcall" >/dev/null 2>&1 ||
   pgrep -f "src/${OUT}/directcall" >/dev/null 2>&1; then
  echo "A directcall from ${OUT} is running:" >&2
  pgrep -fl "${OUT}/directcall" >&2 || true
  echo "Stop it first, or build into another directory." >&2
  exit 1
fi

DEPOT="${HERE}/third_party/depot_tools"
if [[ -d "${DEPOT}" ]]; then
  PATH="${DEPOT}:${PATH}"
  export PATH
fi
GN="gn"
[[ -x "${DEPOT}/gn" ]] && GN="${DEPOT}/gn"

ARGS='is_debug=false symbol_level=0 rtc_include_opus=true rtc_enable_symbol_export=true rtc_build_examples=true rtc_use_speech_audio_devices=false mac_deployment_target="15.0" target_cpu="arm64" rtc_use_liquidcall=true'

cd "${HERE}"
echo "==> gn gen ${OUT}"
"${GN}" gen "${OUT}" --args="${ARGS}"
echo "==> ninja -C ${OUT} directcall"
ninja -C "${OUT}" directcall
echo "Built ${HERE}/${OUT}/directcall"
