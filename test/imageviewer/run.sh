#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APP="$ROOT/fifi/apps/imageviewer/fifi-imageviewer"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[imageviewer-test] build the production decoder"
make -s -C "$ROOT/fifi/apps/imageviewer" clean all

echo "[imageviewer-test] decode a real FiFi PNG screenshot"
"$APP" --decode-test "$ROOT/docs/design/screenshots/desktop-full.png" |
    grep -Eq '^PNG [1-9][0-9]*x[1-9][0-9]*$'

echo "[imageviewer-test] decode a generated standards-compliant JPEG"
printf 'P6\n2 1\n255\n\377\000\000\000\377\000' > "$TMP/pixels.ppm"
cjpeg -outfile "$TMP/pixels.jpg" "$TMP/pixels.ppm" 2>/dev/null
"$APP" --decode-test "$TMP/pixels.jpg" | grep -Fxq 'JPEG 2x1'

echo "[imageviewer-test] reject malformed compressed input"
printf 'not an image\n' > "$TMP/broken.png"
if "$APP" --decode-test "$TMP/broken.png" >/dev/null 2>&1; then
    echo "[imageviewer-test] malformed PNG was accepted" >&2
    exit 1
fi

echo "[imageviewer-test] PASS"
