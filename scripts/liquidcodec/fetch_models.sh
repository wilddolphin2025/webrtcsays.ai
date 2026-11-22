#!/usr/bin/env bash
# Download LiquidCodec Core ML packages. Weights are not committed.
#
# Default source is wilddolphin2025/liquidcodec-ane. The default ref is the
# draft streaming branch (PR 1) until that branch is merged; override it:
#   LIQUIDCODEC_REF=master scripts/liquidcodec/fetch_models.sh
#   LIQUIDCODEC_REPO=shershah1024/liquidcodec-ane LIQUIDCODEC_REF=master \
#     scripts/liquidcodec/fetch_models.sh
#
# The weights and the Liquid-Codec design are Liquid AI's, under the
# LFM Open License v1.0 (commercial use only below USD 10M annual revenue).
# See scripts/liquidcodec/NOTICE.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# README runs directcall from the gclient parent, with BIN=src/out/release/directcall,
# so models belong at <parent>/scripts/liquidcodec/models. Invoking
# src/scripts/liquidcodec/fetch_models.sh from that parent used to write a
# cwd-relative scripts/ path only by accident. Pin the parent when this script
# lives inside src/.
if [[ "$(basename "${SCRIPT_ROOT}")" == "src" && -e "${SCRIPT_ROOT}/../.git" ]]; then
  DEFAULT_DEST="$(cd "${SCRIPT_ROOT}/.." && pwd)/scripts/liquidcodec/models"
else
  DEFAULT_DEST="${SCRIPT_DIR}/models"
fi

REPO="${LIQUIDCODEC_REPO:-wilddolphin2025/liquidcodec-ane}"
REF="${LIQUIDCODEC_REF:-cursor/liquidcodec-stream-51d0}"
DEST="${LIQUIDCODEC_DEST:-${DEFAULT_DEST}}"
DRY=0
PROBE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) REPO="$2"; shift 2 ;;
    --ref) REF="$2"; shift 2 ;;
    --dest) DEST="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    --probe) PROBE=1; shift ;;
    -h|--help)
      echo "usage: $0 [--repo owner/name] [--ref branch] [--dest dir] [--probe] [--dry-run]"
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

URL="https://github.com/${REPO}.git"
echo "LiquidCodec models: ${REPO} @ ${REF} -> ${DEST}"

if [[ "${PROBE}" == "1" || "${DRY}" == "1" ]]; then
  if ! git ls-remote --heads "${URL}" >/tmp/liquidcodec-ls-remote.txt 2>/tmp/liquidcodec-ls-remote.err; then
    echo "Cannot see ${URL}." >&2
    cat /tmp/liquidcodec-ls-remote.err >&2 || true
    echo "The fork wilddolphin2025/liquidcodec-ane is where the Core ML encoder and the smaller-chunk decoder will be published. This script does not convert models." >&2
    exit 1
  fi
  echo "Remote heads:"
  cat /tmp/liquidcodec-ls-remote.txt
fi

if [[ "${DRY}" == "1" ]]; then
  echo "dry-run: git clone --depth 1 --branch ${REF} --filter=blob:none --sparse ${URL}"
  echo "dry-run: git sparse-checkout set models"
  exit 0
fi

if [[ "${PROBE}" == "1" ]]; then
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
git clone --depth 1 --branch "${REF}" --filter=blob:none --sparse "${URL}" "${TMP}/src"
git -C "${TMP}/src" sparse-checkout set models
if [[ ! -d "${TMP}/src/models" ]]; then
  echo "The checkout has no models/ directory." >&2
  exit 1
fi
mkdir -p "$(dirname "${DEST}")"
rm -rf "${DEST}"
mv "${TMP}/src/models" "${DEST}"
printf '%s\n' "${REPO}@${REF}" > "${DEST}/FETCHED_FROM.txt"
echo "Fetched models into ${DEST}"
if [[ ! -f "${DEST}/liquidcodec.json" ]]; then
  echo "No liquidcodec.json in the fetch. directcall will assume ctx=20 new=20 until the fork publishes a manifest." >&2
fi
