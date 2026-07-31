#!/bin/sh
# Download a browser to the specified output path.
# Usage: fifi-download-browser.sh <librewolf|firefox> <output_path>
# LibreWolf AppImage: GitLab project 24386000 (librewolf-community/browser/linux)

BROWSER="$1"
OUTPUT="$2"
. "${FIFI_VERIFY_LIB:-/usr/share/fifi/verified-download.sh}"

[ -n "$BROWSER" ] && [ -n "$OUTPUT" ] || { echo "Usage: $0 <librewolf|firefox> <output>"; exit 1; }

mkdir -p "$(dirname "$OUTPUT")"

# Wait for a default route (DHCP may still be running)
echo "Checking network..."
for i in $(seq 1 30); do
    ip route show 2>/dev/null | grep -q 'default' && break
    sleep 1
done
ip route show 2>/dev/null | grep -q 'default' || { echo "ERROR: No network after 30s"; exit 1; }

if [ "$BROWSER" = "librewolf" ]; then
    echo "Finding latest LibreWolf release..."
    # GitLab API — project 24386000 is the Linux AppImage project
    PROJ="24386000"
    VER=$(curl -s --max-time 10 \
        "https://gitlab.com/api/v4/projects/${PROJ}/releases?per_page=1" \
        | grep '"tag_name"' \
        | head -1 \
        | sed 's/.*"tag_name":[ ]*"v*//;s/".*//')

    if [ -z "$VER" ]; then
        echo "ERROR: Could not resolve a verified LibreWolf release."
        exit 1
    fi

    echo "Downloading LibreWolf ${VER}..."
    URL="https://gitlab.com/api/v4/projects/${PROJ}/packages/generic/librewolf/${VER}/LibreWolf.x86_64.AppImage"
    SHA="$(fifi_gitlab_package_sha256 "$PROJ" "$URL" || true)"
    [ -n "$SHA" ] || { echo "ERROR: LibreWolf SHA-256 unavailable."; exit 1; }
    fifi_download_verified "$URL" "$SHA" "$OUTPUT" || exit 1
    echo "LibreWolf verified and ready"
    exit 0

elif [ "$BROWSER" = "firefox" ]; then
    echo "Finding latest verified Firefox AppImage..."
    REL="$(curl -fsSL --max-time 30 \
        'https://api.github.com/repos/ivan-hc/Firefox-appimage/releases?per_page=30' || true)"
    PAIR="$(fifi_pick_x86_64_pair "$(printf '%s' "$REL" |
        fifi_github_appimage_pairs)")"
    URL="${PAIR%|*}"
    SHA="${PAIR##*|}"
    [ -n "$URL" ] && [ -n "$SHA" ] && [ "$URL" != "$SHA" ] || {
        echo "ERROR: Firefox SHA-256 unavailable."
        exit 1
    }
    fifi_download_verified "$URL" "$SHA" "$OUTPUT" || exit 1
    echo "Firefox verified and ready"
    exit 0

else
    echo "ERROR: Unknown browser '$BROWSER'"
    exit 1
fi
