#!/usr/bin/env bash
# Download LiquidCall Core ML packages. Weights are not committed.
#
# Default source is wilddolphin2025/liquidcodec-ane. The default ref is the
# draft streaming branch (PR 1) until that branch is merged; override it:
#   LIQUIDCALL_REF=master scripts/liquidcall/fetch_models.sh
#   LIQUIDCALL_REPO=shershah1024/liquidcodec-ane LIQUIDCALL_REF=master \
#     scripts/liquidcall/fetch_models.sh
#
# The weights and the Liquid-Codec design are Liquid AI's, under the
# LFM Open License v1.0 (commercial use only below USD 10M annual revenue).
# See scripts/liquidcall/NOTICE.

set -euo pipefail

# Root liquidcall.json names .mlpackage files. Core ML loads .mlmodelc.
rewrite_manifest_paths() {
  local json="$1"
  [[ -f "${json}" ]] || return 0
  if command -v python3 >/dev/null 2>&1; then
    python3 - "${json}" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
text = p.read_text()
updated = text.replace(".mlpackage", ".mlmodelc")
if updated != text:
    p.write_text(updated)
    print("Rewrote %s model paths to .mlmodelc" % p)
else:
    print("Manifest already uses .mlmodelc: %s" % p)
PY
  else
    sed -i.bak 's/\.mlpackage/.mlmodelc/g' "${json}"
    rm -f "${json}.bak"
    echo "Rewrote ${json} model paths to .mlmodelc"
  fi
}

# The upstream conversion repo publishes the manifest as liquidcodec.json.
# LiquidCall reads liquidcall.json; keep the upstream name as a symlink so
# older directcall builds and scripts still find it.
adopt_manifest_name() {
  local dest="$1"
  if [[ -f "${dest}/liquidcodec.json" && ! -L "${dest}/liquidcodec.json" && ! -e "${dest}/liquidcall.json" ]]; then
    mv "${dest}/liquidcodec.json" "${dest}/liquidcall.json"
    ln -s liquidcall.json "${dest}/liquidcodec.json"
    echo "Manifest: ${dest}/liquidcall.json (liquidcodec.json kept as a symlink)"
  fi
}

# True when liquidcall.json names .mlmodelc files that exist, and every
# .mlpackage already has a sibling .mlmodelc. A second ./build.sh can skip
# the download.
models_ready() {
  local dest="$1"
  [[ -f "${dest}/liquidcall.json" ]] || return 1
  if grep -q '\.mlpackage' "${dest}/liquidcall.json"; then
    return 1
  fi
  local pkg base dir
  while IFS= read -r pkg; do
    [[ -n "${pkg}" ]] || continue
    base="$(basename "${pkg}" .mlpackage)"
    dir="$(dirname "${pkg}")"
    [[ -d "${dir}/${base}.mlmodelc" ]] || return 1
  done < <(find "${dest}" -name '*.mlpackage' -type d 2>/dev/null)
  python3 - "${dest}" <<'PY' || return 1
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
data = json.loads((root / "liquidcall.json").read_text())
enc = root / data.get("encoder_model", "")
dec = root / data.get("decoder_model", "")
sys.exit(0 if enc.exists() and dec.exists() else 1)
PY
}

# xcrun coremlcompiler compile <pkg> <dir> writes <dir>/<name>.mlmodelc.
compile_mlpackages() {
  local dest="$1"
  if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "Not macOS; leaving .mlpackage files. directcall compiles each one to .mlmodelc on first load."
    return 0
  fi
  if ! xcrun --find coremlcompiler >/dev/null 2>&1; then
    echo "WARNING: xcrun coremlcompiler not found. directcall will compile .mlpackage on first load." >&2
    return 0
  fi
  local pkg base dir n=0
  while IFS= read -r pkg; do
    [[ -n "${pkg}" ]] || continue
    base="$(basename "${pkg}" .mlpackage)"
    dir="$(dirname "${pkg}")"
    if [[ -d "${dir}/${base}.mlmodelc" ]]; then
      echo "Already compiled ${dir}/${base}.mlmodelc"
    else
      echo "xcrun coremlcompiler compile ${pkg} ${dir}"
      xcrun coremlcompiler compile "${pkg}" "${dir}"
    fi
    n=$((n + 1))
  done < <(find "${dest}" -name '*.mlpackage' -type d | sort)
  if [[ "${n}" == "0" ]]; then
    echo "No .mlpackage under ${dest}."
    return 0
  fi
  rewrite_manifest_paths "${dest}/liquidcall.json"
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# README runs directcall from the gclient parent, with BIN=src/out/release/directcall,
# so models belong at <parent>/scripts/liquidcall/models. Invoking
# src/scripts/liquidcall/fetch_models.sh from that parent used to write a
# cwd-relative scripts/ path only by accident. Pin the parent when this script
# lives inside src/.
if [[ "$(basename "${SCRIPT_ROOT}")" == "src" && -e "${SCRIPT_ROOT}/../.git" ]]; then
  DEFAULT_DEST="$(cd "${SCRIPT_ROOT}/.." && pwd)/scripts/liquidcall/models"
else
  DEFAULT_DEST="${SCRIPT_DIR}/models"
fi

REPO="${LIQUIDCALL_REPO:-${LIQUIDCODEC_REPO:-wilddolphin2025/liquidcodec-ane}}"
REF="${LIQUIDCALL_REF:-${LIQUIDCODEC_REF:-cursor/liquidcodec-stream-51d0}}"
DEST="${LIQUIDCALL_DEST:-${LIQUIDCODEC_DEST:-${DEFAULT_DEST}}}"
DRY=0
PROBE=0
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) REPO="$2"; shift 2 ;;
    --ref) REF="$2"; shift 2 ;;
    --dest) DEST="$2"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    --probe) PROBE=1; shift ;;
    --force) FORCE=1; shift ;;
    --rewrite-manifest)
      if [[ -z "${2:-}" ]]; then
        echo "ERROR: --rewrite-manifest requires a liquidcall.json path" >&2
        exit 2
      fi
      rewrite_manifest_paths "$2"
      exit 0
      ;;
    -h|--help)
      echo "usage: $0 [--repo owner/name] [--ref branch] [--dest dir] [--probe] [--dry-run] [--force]"
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

URL="https://github.com/${REPO}.git"
echo "LiquidCall models: ${REPO} @ ${REF} -> ${DEST}"

if [[ "${PROBE}" == "1" || "${DRY}" == "1" ]]; then
  if ! git ls-remote --heads "${URL}" >/tmp/liquidcall-ls-remote.txt 2>/tmp/liquidcall-ls-remote.err; then
    echo "Cannot see ${URL}." >&2
    cat /tmp/liquidcall-ls-remote.err >&2 || true
    echo "The fork wilddolphin2025/liquidcodec-ane is where the Core ML encoder and the smaller-chunk decoder will be published. This script does not convert models." >&2
    exit 1
  fi
  echo "Remote heads:"
  cat /tmp/liquidcall-ls-remote.txt
fi

if [[ "${DRY}" == "1" ]]; then
  echo "dry-run: git clone --depth 1 --branch ${REF} --filter=blob:none --sparse ${URL}"
  echo "dry-run: git sparse-checkout set models"
  echo "dry-run: on macOS, xcrun coremlcompiler compile every .mlpackage and rewrite liquidcall.json to .mlmodelc"
  exit 0
fi

[[ -d "${DEST}" ]] && adopt_manifest_name "${DEST}"
if [[ "${FORCE}" != "1" ]] && models_ready "${DEST}"; then
  echo "LiquidCall models ready at ${DEST} (compiled .mlmodelc)."
  echo "LIQUIDCALL_MODELS=${DEST}"
  exit 0
fi

if [[ "${PROBE}" == "1" ]]; then
  exit 0
fi

if [[ "${FORCE}" == "1" || ! -f "${DEST}/liquidcall.json" ]]; then
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
  adopt_manifest_name "${DEST}"
  echo "Fetched models into ${DEST}"
else
  echo "Models already downloaded at ${DEST}; compiling any .mlpackage still uncompiled."
fi
compile_mlpackages "${DEST}"
if [[ ! -f "${DEST}/liquidcall.json" ]]; then
  echo "No liquidcall.json in the fetch. directcall will assume ctx=20 new=20 until the fork publishes a manifest." >&2
fi
echo "LIQUIDCALL_MODELS=${DEST}"
