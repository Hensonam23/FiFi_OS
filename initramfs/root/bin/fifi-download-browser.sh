#!/bin/sh
# Download a browser to the specified output path.
# Usage: fifi-download-browser.sh <librewolf|firefox> <output_path>
# LibreWolf AppImage: GitLab project 24386000 (librewolf-community/browser/linux)

BROWSER="$1"
OUTPUT="$2"

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
        echo "Could not get version, using known good version..."
        VER="151.0.3-1"
    fi

    echo "Downloading LibreWolf ${VER}..."
    URL="https://gitlab.com/api/v4/projects/${PROJ}/packages/generic/librewolf/${VER}/LibreWolf.x86_64.AppImage"
    curl -L --progress-bar --output "$OUTPUT" "$URL"
    exit $?

elif [ "$BROWSER" = "firefox" ]; then
    OUTDIR="$(dirname "$OUTPUT")/firefox-bin"
    TMPTAR="$(dirname "$OUTPUT")/firefox.tar.bz2"

    echo "Downloading Firefox..."
    curl -L --progress-bar \
        --output "$TMPTAR" \
        "https://download.mozilla.org/?product=firefox-latest-ssl&os=linux64&lang=en-US"
    [ $? -eq 0 ] && [ -s "$TMPTAR" ] || { echo "ERROR: Download failed"; rm -f "$TMPTAR"; exit 1; }

    echo "Extracting Firefox..."
    mkdir -p "$OUTDIR"
    tar -xjf "$TMPTAR" -C "$OUTDIR" --strip-components=1 2>/dev/null
    rm -f "$TMPTAR"
    [ -x "$OUTDIR/firefox" ] || { echo "ERROR: Extract failed"; exit 1; }

    printf '#!/bin/sh\nDIR="%s"\nexport LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"\nexec "$DIR/firefox" --no-sandbox "$@"\n' \
        "$OUTDIR" > "$OUTPUT"
    chmod +x "$OUTPUT"
    echo "Firefox ready"
    exit 0

else
    echo "ERROR: Unknown browser '$BROWSER'"
    exit 1
fi
