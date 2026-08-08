#!/bin/sh
# Move legacy browser chooser state into private storage owned by the desktop.
set -eu

DATA_ROOT="${FIFI_DATA_ROOT:-/fifi-data}"
DESKTOP_UID="${FIFI_DESKTOP_UID:-1000}"
DESKTOP_GID="${FIFI_DESKTOP_GID:-1000}"
BROWSER_DIR="$DATA_ROOT/browser"
LEGACY_CHOICE="$DATA_ROOT/browser-choice"
CHOICE="$BROWSER_DIR/choice"
MARKER="$DATA_ROOT/.browser-owned-by-fifi"

[ ! -L "$BROWSER_DIR" ] || {
    echo "browser storage migration: refusing symlinked directory" >&2
    exit 1
}
mkdir -p "$BROWSER_DIR"
if [ ! -e "$MARKER" ]; then
    if [ -f "$LEGACY_CHOICE" ] && [ ! -L "$LEGACY_CHOICE" ] && \
       [ ! -e "$CHOICE" ] && [ ! -L "$CHOICE" ]; then
        cp "$LEGACY_CHOICE" "$CHOICE"
    fi
    chown -RhP "$DESKTOP_UID:$DESKTOP_GID" "$BROWSER_DIR"
    : > "$MARKER"
fi

chown "$DESKTOP_UID:$DESKTOP_GID" "$BROWSER_DIR"
chmod 0700 "$BROWSER_DIR"
