#!/bin/bash
# Move the unused on-disk demo page aside, then delete the web copy.
# The file at /var/www/directcall/index.html is not served, and it embeds
# static TURN credentials. Back it up under a root-only directory first.
# This script does not print the file. Do not run it from CI.
set -euo pipefail

TARGET="${DEMO_PAGE_TARGET:-/var/www/directcall/index.html}"
BACKUP_DIR="${DEMO_PAGE_BACKUP_DIR:-/root/webrtcsays-demo-backups}"

TARGET_DIR="$(cd "$(dirname "${TARGET}")" 2>/dev/null && pwd || dirname "${TARGET}")"
case "${BACKUP_DIR}" in
  /var/www|/var/www/*|/srv/webrtcsays-demo|/srv/webrtcsays-demo/*|/usr/share/nginx|/usr/share/nginx/*)
    echo "refusing to store the backup under a web root" >&2
    exit 1
    ;;
  "${TARGET_DIR}"|"${TARGET_DIR}"/*)
    echo "refusing to store the backup next to the page" >&2
    exit 1
    ;;
esac

if [ "${RETIRE_DEMO_PAGE_CONFIRM:-}" != "backup-then-delete" ]; then
  echo "refusing: set RETIRE_DEMO_PAGE_CONFIRM=backup-then-delete after choosing the backup directory" >&2
  exit 1
fi

if [ -d "${TARGET}" ]; then
  echo "refusing: target is a directory" >&2
  exit 1
fi

if [ ! -f "${TARGET}" ]; then
  echo "target already absent"
  exit 0
fi

install -d -m 0700 "${BACKUP_DIR}"
STAMP="$(date +%Y%m%d%H%M%S)"
DEST="${BACKUP_DIR}/index.html.bak-${STAMP}"
cp -p "${TARGET}" "${DEST}"
chmod 0600 "${DEST}"
cmp -s "${TARGET}" "${DEST}"
rm -f "${TARGET}"
echo "backed up and removed the old demo page"
