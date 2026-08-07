#!/bin/sh
# One-way fixes for persistent App Store metadata. This runs as uid 1000.
set -eu

source_file="${FIFI_APPS_DIR:-/fifi-data/apps}/LibreOffice.src"
legacy_source='url:https://appimages.libreitalia.org/LibreOffice-fresh.standard-x86_64.AppImage'
if [ -f "$source_file" ] && grep -Fxq "$legacy_source" "$source_file"; then
    printf '%s' 'ivan-hc/LibreOffice-appimage' > "$source_file"
fi
