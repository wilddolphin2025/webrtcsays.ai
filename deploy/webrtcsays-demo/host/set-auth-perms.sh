#!/bin/bash
# Owner and mode for the auth secret directory.
# The container runs as uid 8793 and cannot read root-owned mode 0600 files.
# directcall.env is left unchanged: systemd reads it as root.
set -euo pipefail

CONF="${DEMO_AUTH_CONF:-/etc/webrtcsays-demo}"
AUTH_UID="${DEMO_AUTH_UID:-8793}"
AUTH_GID="${DEMO_AUTH_GID:-8793}"
APPLY=0
if [ "${1:-}" = "--apply" ]; then
  APPLY=1
fi

case "${CONF}" in
  /etc/nginx|/etc/nginx/*|*/sites-enabled|*/sites-enabled/*|*/sites-available|*/sites-available/*)
    echo "refusing to change ownership inside the nginx config tree" >&2
    exit 1
    ;;
esac

echo "auth secrets owner ${AUTH_UID}:${AUTH_GID} directory 0700 files 0600"
echo "directcall.env is not changed"

if [ "${APPLY}" -ne 1 ]; then
  echo "check only: ownership not changed"
  exit 0
fi

install -d -m 0700 "${CONF}"
chown "${AUTH_UID}:${AUTH_GID}" "${CONF}"
chmod 0700 "${CONF}"
for name in session.key password tokens turn.json; do
  if [ -f "${CONF}/${name}" ]; then
    chown "${AUTH_UID}:${AUTH_GID}" "${CONF}/${name}"
    chmod 0600 "${CONF}/${name}"
  fi
done
echo "auth secret ownership updated"
