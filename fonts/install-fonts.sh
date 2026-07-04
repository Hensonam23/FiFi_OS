#!/bin/sh
# FiFi OS font provisioning.
#
# Installs the bundled free font families and the compatibility-alias config
# into a target sysroot, then rebuilds the fontconfig cache. Run at image-build
# time (or once on a live system) so every FiFi OS install has a complete,
# cross-platform font set available by default.
#
# Usage:  install-fonts.sh [SYSROOT] [FONT_SRC]
#   SYSROOT   root under which usr/share/fonts + etc/fonts live
#             (default: /fifi-data/browser/sysroot)
#   FONT_SRC  directory holding the font-family folders to install
#             (default: /usr/share/fonts on the build host)
#
# Bundled families (all freely redistributable):
#   liberation  metric-compatible with Arial / Times New Roman / Courier New
#   carlito     metric-compatible with Calibri
#   caladea     metric-compatible with Cambria
#   noto-cjk    Chinese / Japanese / Korean (Noto Sans/Serif CJK)
#   (DejaVu + the broad Noto set are already part of the base image)
#
# Proprietary Microsoft and Apple fonts are NOT shipped — they are not legally
# redistributable. 60-fifi-aliases.conf maps their names onto the substitutes
# above so "Arial", "Calibri", "Segoe UI", "Helvetica", "San Francisco", etc.
# all resolve to a metric/appearance match.
set -e

SYSROOT="${1:-/fifi-data/browser/sysroot}"
FONT_SRC="${2:-/usr/share/fonts}"
HERE="$(cd "$(dirname "$0")" && pwd)"

FONT_DST="$SYSROOT/usr/share/fonts"
CONF_D="$SYSROOT/etc/fonts/conf.d"

mkdir -p "$FONT_DST" "$CONF_D"

for fam in liberation carlito caladea noto-cjk; do
    if [ -d "$FONT_SRC/$fam" ]; then
        cp -r "$FONT_SRC/$fam" "$FONT_DST/"
        echo "[fonts] installed $fam"
    else
        echo "[fonts] WARN: $FONT_SRC/$fam not found, skipping" >&2
    fi
done

cp "$HERE/60-fifi-aliases.conf" "$CONF_D/60-fifi-aliases.conf"
echo "[fonts] installed alias config"

# Rebuild cache using the sysroot's own fontconfig if present.
if [ -x "$SYSROOT/usr/bin/fc-cache" ]; then
    FONTCONFIG_PATH="$SYSROOT/etc/fonts" \
        "$SYSROOT/usr/bin/fc-cache" -f "$FONT_DST" >/dev/null 2>&1 || true
    echo "[fonts] rebuilt fontconfig cache"
elif command -v fc-cache >/dev/null 2>&1; then
    fc-cache -f "$FONT_DST" >/dev/null 2>&1 || true
    echo "[fonts] rebuilt fontconfig cache (host fc-cache)"
fi

echo "[fonts] done"
