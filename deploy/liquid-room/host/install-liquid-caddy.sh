#!/usr/bin/env bash
# Phase B on the LiquidServer VPS (root). Dry run by default.
# Needs Caddy installed (apt install caddy) and DNS liquidserver.wilddolphin.us -> this host.
#   RTC_VPS_IP=<rtc address> bash host/install-liquid-caddy.sh
#   RTC_VPS_IP=<rtc address> INSTALL_CADDY_CONFIRM=liquidserver-site bash host/install-liquid-caddy.sh
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ip="${RTC_VPS_IP:?set RTC_VPS_IP to the rtc.wilddolphin.us IPv4 address}"
printf '%s' "$ip" | grep -qxE '([0-9]{1,3}\.){3}[0-9]{1,3}' || { echo "RTC_VPS_IP must be IPv4" >&2; exit 1; }
dst="${CADDYFILE:-/etc/caddy/Caddyfile}"
tmp="$(mktemp)"
sed "s/RTC_VPS_IP_PLACEHOLDER/$ip/" "$here/caddy/Caddyfile.liquidserver" > "$tmp"
caddy validate --adapter caddyfile --config "$tmp" >/dev/null
echo "validated. ufw needed: 80/tcp 443/tcp 7881/tcp 7882/udp"
if [ "${INSTALL_CADDY_CONFIRM:-}" != "liquidserver-site" ]; then
  echo "dry run. Set INSTALL_CADDY_CONFIRM=liquidserver-site to apply."
  rm -f "$tmp"; exit 0
fi
ts="$(date +%Y%m%d%H%M%S)"
install -d -m 0700 /root/backups
[ -f "$dst" ] && cp -a "$dst" "/root/backups/Caddyfile.$ts"
install -m 0644 "$tmp" "$dst" && rm -f "$tmp"
systemctl reload caddy || systemctl restart caddy
echo "installed $dst; backup /root/backups/Caddyfile.$ts"
