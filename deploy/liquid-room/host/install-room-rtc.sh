#!/usr/bin/env bash
# Phase B on rtc.wilddolphin.us (run as root by DevOps). Dry run by default.
# Installs the room page, the two nginx files, the gate header include, and one
# include line in the rtc site file. It does not reload nginx.
#   bash host/install-room-rtc.sh
#   INSTALL_ROOM_CONFIRM=room-locations bash host/install-room-rtc.sh
#   then: nginx -t && systemctl reload nginx
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
site="${RTC_SITE:-/etc/nginx/sites-enabled/rtc.wilddolphin.us.conf}"
nginx_dir="${NGINX_DIR:-/etc/nginx}"
backups="${BACKUP_DIR:-/root/backups/liquid-room-$(date +%Y%m%d%H%M%S)}"
page_dir="${ROOM_PAGE_DIR:-/srv/liquid-room}"
gate_conf="$nginx_dir/liquid-gate.conf"
gate_hex="${GATE_HEX_FILE:-/root/liquid-gate.hex}"
snippet="$nginx_dir/snippets/liquid-room-locations.conf"
http_conf="$nginx_dir/conf.d/liquid-room-http.conf"
include_line="    include $snippet;"
real_site="$(readlink -f "$site")"
[ -f "$real_site" ] || { echo "site file $site not found" >&2; exit 1; }
grep -q "liquid-room-locations.conf" "$real_site" && already=1 || already=0
anchor="$(grep -n -E 'include .*(frame-view-locations|webrtcsays-demo-locations)\.conf;' "$real_site" | tail -1 | cut -d: -f1 || true)"
[ -n "$anchor" ] || [ "$already" = 1 ] || { echo "no demo/frame-view include found in $real_site; refusing" >&2; exit 1; }
echo "site=$real_site anchor_line=${anchor:-n/a} already_included=$already"
echo "will install: $page_dir/index.html $snippet $http_conf $gate_conf (from $gate_hex)"
if [ "${INSTALL_ROOM_CONFIRM:-}" != "room-locations" ]; then
  echo "dry run. Set INSTALL_ROOM_CONFIRM=room-locations to apply."
  exit 0
fi
[ -s "$gate_hex" ] || { echo "run host/make-gate-secret.sh first ($gate_hex missing)" >&2; exit 1; }
grep -qxE '[0-9a-f]{64}' "$gate_hex" || { echo "$gate_hex must hold 64 lowercase hex" >&2; exit 1; }
install -d -m 0700 "$backups"
cp -a "$real_site" "$backups/"
nginx -T 2>/dev/null | sha256sum > "$backups/nginx-T.sha256" || true
install -d -m 0755 "$page_dir" "$nginx_dir/snippets" "$nginx_dir/conf.d"
install -m 0644 "$here/page/index.html" "$page_dir/index.html"
install -m 0644 "$here/nginx/liquid-room-locations.conf" "$snippet"
install -m 0644 "$here/nginx/liquid-room-http.conf" "$http_conf"
umask 077
printf 'proxy_set_header X-Liquid-Gate "%s";\n' "$(cat "$gate_hex")" > "$gate_conf.new"
install -m 0600 -o root -g root "$gate_conf.new" "$gate_conf" && rm -f "$gate_conf.new"
if [ "$already" = 0 ]; then
  sed -i "${anchor}a\\
$include_line" "$real_site"
fi
cat > "$backups/rollback.sh" <<ROLLBACK
#!/usr/bin/env bash
set -euo pipefail
cp -a "$backups/$(basename "$real_site")" "$real_site"
rm -f "$snippet" "$http_conf" "$gate_conf"
nginx -t && systemctl reload nginx
ROLLBACK
chmod 0700 "$backups/rollback.sh"
echo "installed. backups and rollback.sh in $backups. Next: nginx -t && systemctl reload nginx"
