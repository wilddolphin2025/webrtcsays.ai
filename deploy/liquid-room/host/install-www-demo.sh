#!/usr/bin/env bash
# Install www/demo.html as demo.html on the www.wilddolphin.us web root.
# Dry run by default. Backs up the live file to ~/site-backups (never under the web root).
#   SITE_ROOT=~/public_html bash host/install-www-demo.sh
#   SITE_ROOT=~/public_html INSTALL_WWW_DEMO_CONFIRM=replace-demo-html bash host/install-www-demo.sh
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
src="$here/www/demo.html"
root="${SITE_ROOT:?set SITE_ROOT to the www web root}"
backups="${SITE_BACKUP_DIR:-$HOME/site-backups}"
dst="$root/demo.html"
[ -f "$src" ] || { echo "missing $src" >&2; exit 1; }
[ -d "$root" ] || { echo "SITE_ROOT $root is not a directory" >&2; exit 1; }
case "$(cd "$backups" 2>/dev/null && pwd || echo "$backups")/" in
  "$(cd "$root" && pwd)/"*) echo "refusing: backup dir is inside the web root" >&2; exit 1 ;;
esac
new_sha="$(sha256sum "$src" | cut -c1-16)"
old_sha="$( [ -f "$dst" ] && sha256sum "$dst" | cut -c1-16 || echo none)"
echo "demo.html live=$old_sha new=$new_sha dst=$dst backup_dir=$backups"
if [ "${INSTALL_WWW_DEMO_CONFIRM:-}" != "replace-demo-html" ]; then
  echo "dry run. Set INSTALL_WWW_DEMO_CONFIRM=replace-demo-html to apply."
  exit 0
fi
install -d -m 0700 "$backups"
ts="$(date +%Y%m%d%H%M%S)"
[ -f "$dst" ] && install -m 0600 "$dst" "$backups/demo.html.$ts"
install -m 0644 "$src" "$dst.new.$ts"
mv -f "$dst.new.$ts" "$dst"
echo "installed demo.html sha=$(sha256sum "$dst" | cut -c1-16); backup $backups/demo.html.$ts"
echo "rollback: install -m 0644 $backups/demo.html.$ts $dst"
