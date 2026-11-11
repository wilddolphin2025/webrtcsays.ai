#!/bin/bash
# Copy selected environment variables from a live directcall process.
# Prints key names only. --apply writes the file mode 0600.
# Does not stop the process and does not print values.
set -euo pipefail

OUT="${DEMO_ENV_OUT:-/etc/webrtcsays-demo/directcall.env}"
ENV_FILE="${DEMO_ENVIRON_FILE:-}"
APPLY=0
if [ "${1:-}" = "--apply" ]; then
  APPLY=1
fi

if [ -z "${ENV_FILE}" ]; then
  if [ -n "${DEMO_DIRECTCALL_PID:-}" ]; then
    ENV_FILE="/proc/${DEMO_DIRECTCALL_PID}/environ"
  else
    echo "set DEMO_ENVIRON_FILE or DEMO_DIRECTCALL_PID" >&2
    exit 1
  fi
fi

if [ ! -r "${ENV_FILE}" ]; then
  echo "environment source is not readable" >&2
  exit 1
fi

TMP="$(mktemp)"
trap 'rm -f "${TMP}"' EXIT
python3 - "${ENV_FILE}" "${TMP}" <<'PY'
import sys
src, dest = sys.argv[1], sys.argv[2]
raw = open(src, "rb").read()
allowed = []
names = []
for item in raw.split(b"\0"):
    if b"=" not in item:
        continue
    key, value = item.split(b"=", 1)
    try:
        name = key.decode("ascii")
    except UnicodeDecodeError:
        continue
    keep = (
        name in {"LD_LIBRARY_PATH", "PATH", "HOME", "LANG"}
        or name.startswith("WHILLATS_")
        or name.startswith("STYLETTS")
        or name.startswith("ESPEAK")
        or name.startswith("GGML_")
        or name.startswith("LC_")
    )
    if not keep:
        continue
    names.append(name)
    allowed.append(key + b"=" + value)
sys.stdout.write("keys: " + ", ".join(names) + "\n")
open(dest, "wb").write(b"\n".join(allowed) + (b"\n" if allowed else b""))
PY

if [ "${APPLY}" -ne 1 ]; then
  echo "check only: environment file not written"
  exit 0
fi

install -d -m 0700 "$(dirname "${OUT}")"
install -m 0600 "${TMP}" "${OUT}"
echo "wrote environment file mode 0600"
