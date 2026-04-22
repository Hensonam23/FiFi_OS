#!/usr/bin/env bash
# FiFi OS — linux-desktop launcher
# Run from anywhere: bash /path/to/fifi-os/run-fifi-linux.sh
# Or pin to a terminal alias: alias fifi-linux='bash ~/src/fifi-os/run-fifi-linux.sh'
REPO="$(cd "$(dirname "$0")" && pwd)"
exec bash "$REPO/scripts/run-qemu.sh" "${1:-gui}"
