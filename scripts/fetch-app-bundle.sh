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

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/initramfs/root/usr/share/fifi/verified-download.sh"
OUT="${1:-"$ROOT/build-linux/apps-bundle"}"
DRY="${FIFI_BUNDLE_DRY:-0}"
mkdir -p "$OUT"

fetch() {  # <Name> <url> <sha256>
    echo "[bundle] $1  <-  $2"
    [[ "$3" =~ ^[0-9a-fA-F]{64}$ ]] || {
        echo "[bundle] ERROR: no trusted SHA-256 for $1" >&2
        return 1
    }
    [ "$DRY" = 1 ] && return 0
    curl -L --fail --progress-bar -o "$OUT/$1.AppImage.part" "$2"
    fifi_verify_sha256 "$OUT/$1.AppImage.part" "$3" || {
        echo "[bundle] ERROR: SHA-256 mismatch for $1" >&2
        rm -f "$OUT/$1.AppImage.part"
        return 1
    }
    mv "$OUT/$1.AppImage.part" "$OUT/$1.AppImage"
    printf '%s  %s.AppImage\n' "$3" "$1" > "$OUT/$1.AppImage.sha256"
    chmod +x "$OUT/$1.AppImage"
}

# LibreWolf — Codeberg releases API (same source the App Store catalog uses)
LW_PAIR="$(fifi_codeberg_appimage_pair 'librewolf/bsys6' || true)"
LW_URL="${LW_PAIR%|*}"; LW_SHA="${LW_PAIR##*|}"
if [ -n "$LW_URL" ] && [ "$LW_URL" != "$LW_SHA" ]; then fetch LibreWolf "$LW_URL" "$LW_SHA"
else echo "[bundle] WARNING: could not resolve LibreWolf" >&2; fi

# Firefox — ivan-hc AppImage build, GitHub releases API
FF_PAIR="$(curl -sf 'https://api.github.com/repos/ivan-hc/Firefox-appimage/releases?per_page=30' |
    fifi_github_appimage_pairs | grep -iE 'x86_64|amd64' | head -1 || true)"
FF_URL="${FF_PAIR%|*}"; FF_SHA="${FF_PAIR##*|}"
if [ -n "$FF_URL" ] && [ "$FF_URL" != "$FF_SHA" ]; then fetch Firefox "$FF_URL" "$FF_SHA"
else echo "[bundle] WARNING: could not resolve Firefox" >&2; fi

# LibreOffice — GitHub asset with a server-computed release digest.
LO_PAIR="$(curl -sf 'https://api.github.com/repos/ivan-hc/LibreOffice-appimage/releases?per_page=10' |
    fifi_github_appimage_pairs |
    grep -iE 'fresh-standard.*x86_64|fresh.*x86_64' | head -1 || true)"
LO_URL="${LO_PAIR%|*}"; LO_SHA="${LO_PAIR##*|}"
if [ -n "$LO_URL" ] && [ "$LO_URL" != "$LO_SHA" ]; then fetch LibreOffice "$LO_URL" "$LO_SHA"
else echo "[bundle] WARNING: could not resolve LibreOffice" >&2; fi

[ "$DRY" = 1 ] || { echo "[bundle] done:"; du -sh "$OUT"; ls -lh "$OUT"; }
