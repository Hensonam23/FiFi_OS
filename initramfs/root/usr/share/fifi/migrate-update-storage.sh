#!/bin/sh
# Prepare private staging and channel state for the unprivileged updater.
set -eu

DATA_ROOT="${FIFI_DATA_ROOT:-/fifi-data}"
DESKTOP_UID="${FIFI_DESKTOP_UID:-1000}"
DESKTOP_GID="${FIFI_DESKTOP_GID:-1000}"
UPDATE_DIR="$DATA_ROOT/update"
LEGACY_CHANNEL="$DATA_ROOT/update-channel"
CHANNEL="$UPDATE_DIR/channel"
MARKER="$DATA_ROOT/.update-owned-by-fifi"

[ ! -L "$UPDATE_DIR" ] || {
    echo "update storage migration: refusing symlinked directory" >&2
    exit 1
}
mkdir -p "$UPDATE_DIR/staging"
if [ ! -e "$MARKER" ]; then
    if [ -f "$LEGACY_CHANNEL" ] && [ ! -L "$LEGACY_CHANNEL" ] && \
       [ ! -e "$CHANNEL" ] && [ ! -L "$CHANNEL" ]; then
        cp "$LEGACY_CHANNEL" "$CHANNEL"
    fi
    chown -RhP "$DESKTOP_UID:$DESKTOP_GID" "$UPDATE_DIR"
    : > "$MARKER"
fi
chown "$DESKTOP_UID:$DESKTOP_GID" "$UPDATE_DIR" "$UPDATE_DIR/staging"
chmod 0700 "$UPDATE_DIR" "$UPDATE_DIR/staging"
