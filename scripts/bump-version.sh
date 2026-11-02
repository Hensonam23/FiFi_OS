#!/usr/bin/env bash
# Advance FiFi OS from V1.xxx to the next tracked source-build version.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
current="$(tr -d '\r\n' < "$ROOT/VERSION")"
[[ "$current" =~ ^1\.([0-9]{3,})$ ]] || {
    echo "VERSION must use the 1.xxx format" >&2
    exit 1
}
digits="${BASH_REMATCH[1]}"
next=$((10#$digits + 1))
width=${#digits}
if (( ${#next} > width )); then width=${#next}; fi
printf '1.%0*d\n' "$width" "$next" > "$ROOT/VERSION"
echo "FiFi OS is now V$(cat "$ROOT/VERSION")"
