#!/usr/bin/env bash
# Run on the LiquidServer VPS as root, with the gate value on stdin (see make-gate-secret.sh).
# Writes /etc/liquidserver/gate (0640 root:liquid) for LIQUID_GATE_FILE. Prints no secret.
set -euo pipefail
dir="${LIQUID_CONF_DIR:-/etc/liquidserver}"
svc_user="${LIQUID_USER:-liquid}"
value="$(head -c 65 | tr -d '\r\n')"
printf '%s' "$value" | grep -qxE '[0-9a-f]{64}' || { echo "stdin must be 64 lowercase hex" >&2; exit 1; }
install -d -m 0750 -o root -g "$svc_user" "$dir"
umask 027
printf '%s\n' "$value" > "$dir/gate.new"
chown root:"$svc_user" "$dir/gate.new" && chmod 0640 "$dir/gate.new" && mv "$dir/gate.new" "$dir/gate"
echo "wrote $dir/gate sha256 prefix $(sha256sum "$dir/gate" | cut -c1-8)"
