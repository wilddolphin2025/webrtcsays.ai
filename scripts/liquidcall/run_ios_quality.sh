#!/usr/bin/env bash
# Build the unsigned iOS framework drop, then print how RobotPhone runs
# stages 1-3. The XCTest lives in RobotPhone (branch liquidcodec-sync)
# because that repo owns the signed app bundle and its Documents directory.
# This script does not sign and does not modify RobotPhone.
#
#   src/scripts/liquidcall/run_ios_quality.sh
#
# Simulator runs use compute=cpu. testOfflineAneInt8 is device-only.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=layout.sh
source "${SCRIPT_DIR}/layout.sh"

if [[ -n "${LC_PARENT:-}" && -f "${LC_PARENT}/build.sh" ]]; then
  ROOT="${LC_PARENT}"
elif [[ -f "${LC_TREE}/build.sh" ]]; then
  ROOT="${LC_TREE}"
else
  ROOT="$(cd "${LC_TREE}/.." && pwd)"
fi
DROP="${ROOT}/out/ios-arm64"
SHA="$(git -C "${LC_TREE}" rev-parse --short=12 HEAD 2>/dev/null || echo liquidcodec-ane)"

if [[ ! -d "${DROP}/direct.framework" || ! -d "${DROP}/WebRTC.framework" ]]; then
  if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "iOS frameworks are not in ${DROP}."
    echo "On a Mac: ./build.sh -b ${SHA} ios release liquidcall"
    echo "The test source is ${SCRIPT_DIR}/ios/LiquidQualityTests.mm"
    exit 0
  fi
  if [[ ! -f "${ROOT}/build.sh" ]]; then
    echo "build.sh not found from ${ROOT}" >&2
    exit 1
  fi
  (cd "${ROOT}" && ./build.sh -b "${SHA}" ios release liquidcall)
fi

echo "iOS drop: ${DROP}"
echo "Frameworks:"
ls -d "${DROP}/WebRTC.framework" "${DROP}/direct.framework" 2>/dev/null || true
if [[ -d "${DROP}/models" ]]; then
  echo "models: ${DROP}/models"
else
  echo "models directory is missing from the drop"
fi
cat <<EOF

Add ${SCRIPT_DIR}/ios/LiquidQualityTests.mm to the RobotPhone unit-test
target on branch liquidcodec-sync. Link:
  ${DROP}/WebRTC.framework
  ${DROP}/direct.framework
Copy ${DROP}/models into the app bundle as a folder named models.
The test calls DirectQualityRun and appends quality.csv in Documents.

Simulator (no ANE):
  xcodebuild test -project RobotPhone.xcodeproj -scheme RobotPhone \\
    -destination 'platform=iOS Simulator,name=iPhone 16' \\
    -only-testing:RobotPhoneTests/LiquidQualityTests/testOfflineCpuInt8 \\
    -only-testing:RobotPhoneTests/LiquidQualityTests/testFactoryCpuInt8 \\
    -only-testing:RobotPhoneTests/LiquidQualityTests/testOfflineFp16Cpu

Device (ANE):
  xcodebuild test -project RobotPhone.xcodeproj -scheme RobotPhone \\
    -destination 'platform=iOS,id=DEVICE_UDID' \\
    -only-testing:RobotPhoneTests/LiquidQualityTests/testOfflineAneInt8

Pull the CSV after a run:
  xcrun simctl get_app_container booted WILDDOLPHIN.RobotPhone data
  # or, for a device: Xcode Devices window, download the container
EOF
