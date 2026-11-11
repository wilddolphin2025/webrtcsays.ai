#!/bin/bash
# Print the loopback demo-auth container command. Default is check only.
# Secrets stay in /etc/webrtcsays-demo on the host. They are not arguments.
set -euo pipefail

CONF="${DEMO_AUTH_CONF:-/etc/webrtcsays-demo}"
IMAGE="${DEMO_AUTH_IMAGE:-webrtcsays-demo-auth:stage1}"

cat <<EOF
docker run -d \\
  --name webrtcsays-demo-auth \\
  --init \\
  --pids-limit 64 \\
  --memory 64m \\
  --read-only \\
  --cap-drop ALL \\
  --security-opt no-new-privileges:true \\
  --restart unless-stopped \\
  -p 127.0.0.1:8793:8793 \\
  -v ${CONF}:/conf:ro \\
  --tmpfs /tmp:rw,noexec,nosuid,size=8m \\
  -e DEMO_CONF_DIR=/conf \\
  -e DEMO_BIND=0.0.0.0 \\
  -e DEMO_PORT=8793 \\
  -e DEMO_TRUST_PROXY=1 \\
  -e PYTHONDONTWRITEBYTECODE=1 \\
  ${IMAGE}
EOF

if [ "${1:-}" != "--apply" ]; then
  echo "check only: docker was not invoked" >&2
  exit 0
fi

if [ "${DEMO_AUTH_INSTALL_CONFIRM:-}" != "loopback-8793" ]; then
  echo "refusing: set DEMO_AUTH_INSTALL_CONFIRM=loopback-8793" >&2
  exit 1
fi

for name in session.key; do
  if [ ! -f "${CONF}/${name}" ]; then
    echo "missing ${name}" >&2
    exit 1
  fi
done
auth_uid="${DEMO_AUTH_UID:-8793}"
key_uid="$(stat -c %u "${CONF}/session.key")"
key_mode="$(stat -c %a "${CONF}/session.key")"
dir_uid="$(stat -c %u "${CONF}")"
dir_mode="$(stat -c %a "${CONF}")"
if [ "${key_uid}" != "${auth_uid}" ] || [ "${key_mode}" != "600" ] || [ "${dir_uid}" != "${auth_uid}" ] || [ "${dir_mode}" != "700" ]; then
  echo "session.key and ${CONF} must be owned by uid ${auth_uid}, directory 0700, files 0600" >&2
  echo "run deploy/webrtcsays-demo/host/set-auth-perms.sh --apply" >&2
  exit 1
fi
if [ ! -f "${CONF}/password" ] && [ ! -f "${CONF}/tokens" ]; then
  echo "need password or tokens" >&2
  exit 1
fi
exec docker run -d \
  --name webrtcsays-demo-auth \
  --init \
  --pids-limit 64 \
  --memory 64m \
  --read-only \
  --cap-drop ALL \
  --security-opt no-new-privileges:true \
  --restart unless-stopped \
  -p 127.0.0.1:8793:8793 \
  -v "${CONF}:/conf:ro" \
  --tmpfs /tmp:rw,noexec,nosuid,size=8m \
  -e DEMO_CONF_DIR=/conf \
  -e DEMO_BIND=0.0.0.0 \
  -e DEMO_PORT=8793 \
  -e DEMO_TRUST_PROXY=1 \
  -e PYTHONDONTWRITEBYTECODE=1 \
  "${IMAGE}"
