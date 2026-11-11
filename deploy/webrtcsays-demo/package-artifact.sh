#!/bin/bash
# Pack only this deployment directory. Does not include the WebRTC tree.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/opt/cursor/artifacts/webrtcsays-demo-stage1.tar.gz}"
mkdir -p "$(dirname "${OUT}")"
tar -C "$(dirname "${ROOT}")" -czf "${OUT}" \
  --exclude '__pycache__' \
  --exclude '*.pyc' \
  "$(basename "${ROOT}")"
echo "${OUT}"
