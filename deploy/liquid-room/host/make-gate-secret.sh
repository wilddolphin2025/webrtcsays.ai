#!/usr/bin/env bash
# Create the rtc -> LiquidServer gate value on rtc.wilddolphin.us (root). Prints no secret.
#   bash host/make-gate-secret.sh            # writes /root/liquid-gate.hex (0600) if missing
# Copy it to the LiquidServer VPS without printing it, from the founder's Mac:
#   scp deploy/liquid-room/host/install-gate-liquid.sh root@<liquidserver>:/root/
#   ssh root@rtc.wilddolphin.us 'cat /root/liquid-gate.hex' | \
#     ssh root@<liquidserver> 'bash /root/install-gate-liquid.sh'
set -euo pipefail
out="${GATE_HEX_FILE:-/root/liquid-gate.hex}"
if [ -s "$out" ]; then echo "exists: $out (not changed)"; exit 0; fi
umask 077
openssl rand -hex 32 > "$out.new" && mv "$out.new" "$out"
chmod 0600 "$out"
echo "wrote $out ($(wc -c < "$out") bytes, sha256 prefix $(sha256sum "$out" | cut -c1-8))"
