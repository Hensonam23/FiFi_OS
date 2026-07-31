#!/bin/sh
# Finish work that needs the newly booted image or an active network.
set -u

DATA_ROOT="${FIFI_DATA_ROOT:-/fifi-data}"
PENDING="$DATA_ROOT/post-update.pending"
LOG="$DATA_ROOT/update-completion.log"

[ -e "$PENDING" ] || exit 0

echo "[post-update] completing application updates and migrations" >> "$LOG"

if [ "${FIFI_NETWORK_READY:-0}" != 1 ]; then
    ready=0
    attempts=0
    while [ "$attempts" -lt 60 ]; do
        if ip route 2>/dev/null | grep -q '^default ' &&
           curl -fsI --max-time 10 https://api.github.com/ >/dev/null 2>&1; then
            ready=1
            break
        fi
        attempts=$((attempts + 1))
        sleep 2
    done
    if [ "$ready" != 1 ]; then
        echo "[post-update] network unavailable; will retry next boot" >> "$LOG"
        exit 0
    fi
fi

if command -v app-update >/dev/null 2>&1; then
    if ! app-update -y >> "$LOG" 2>&1; then
        echo "[post-update] an application update failed; will retry next boot" >> "$LOG"
        exit 0
    fi
fi

rm -f "$PENDING"
echo "[post-update] complete" >> "$LOG"
