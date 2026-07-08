#!/usr/bin/env bash
# Fetch the offline app bundle for the FiFi OS USB installer.
#
# Downloads the latest REAL upstream AppImages for the installer's software
# choices (LibreWolf, Firefox, LibreOffice) into build-linux/apps-bundle/.
# build-linux-usb.sh copies that dir onto the ISO as /apps-bundle so the
# guided installer works with no internet connection — the App Store's
# "Check Updates" upgrades the apps once the machine is online.
#
# Usage: scripts/fetch-app-bundle.sh [outdir]
#        FIFI_BUNDLE_DRY=1 scripts/fetch-app-bundle.sh   # resolve URLs only
set -euo pipefail

OUT="${1:-"$(cd "$(dirname "$0")/.." && pwd)/build-linux/apps-bundle"}"
DRY="${FIFI_BUNDLE_DRY:-0}"
mkdir -p "$OUT"

fetch() {  # <Name> <url>
    echo "[bundle] $1  <-  $2"
    [ "$DRY" = 1 ] && return 0
    curl -L --fail --progress-bar -o "$OUT/$1.AppImage.part" "$2"
    mv "$OUT/$1.AppImage.part" "$OUT/$1.AppImage"
    chmod +x "$OUT/$1.AppImage"
}

# LibreWolf — GitLab releases API (same source the App Store catalog uses)
LW_URL="$(curl -sf 'https://gitlab.com/api/v4/projects/librewolf-community%2Fbrowser%2Fappimage/releases?per_page=10' \
    | grep -oE '"url":"[^"]*\.AppImage"' | sed 's/^"url":"//;s/"$//' \
    | grep -iE 'x86_64' | head -1 || true)"
if [ -n "$LW_URL" ]; then fetch LibreWolf "$LW_URL"
else echo "[bundle] WARNING: could not resolve LibreWolf" >&2; fi

# Firefox — ivan-hc AppImage build, GitHub releases API
FF_URL="$(curl -sf 'https://api.github.com/repos/ivan-hc/Firefox-appimage/releases?per_page=30' \
    | grep -oE '"browser_download_url": *"[^"]*\.AppImage"' \
    | sed 's/.*"browser_download_url": *"//;s/"$//' \
    | grep -iE 'x86_64|amd64' | head -1 || true)"
if [ -n "$FF_URL" ]; then fetch Firefox "$FF_URL"
else echo "[bundle] WARNING: could not resolve Firefox" >&2; fi

# LibreOffice — versionless "fresh" URL, always the latest release
fetch LibreOffice "https://appimages.libreitalia.org/LibreOffice-fresh.standard-x86_64.AppImage"

[ "$DRY" = 1 ] || { echo "[bundle] done:"; du -sh "$OUT"; ls -lh "$OUT"; }
