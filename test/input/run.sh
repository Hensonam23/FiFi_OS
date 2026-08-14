#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/touchpad-axis.c" <<'EOF'
#include "fifi/platform/linux/include/touchpad_axis.h"

int main(void) {
    touchpad_axis_state_t axes;
    touchpad_axis_reset(&axes);
    touchpad_axis_set_x(&axes, 100);
    touchpad_axis_set_y(&axes, 200);

    /* A later evdev batch may report only X; Y must remain available. */
    touchpad_axis_set_x(&axes, 125);
    if (axes.x[0] != 125 || axes.y[0] != 200) return 1;

    /* MT_SLOT persists until Linux reports another slot selection. */
    touchpad_axis_select_slot(&axes, 1);
    touchpad_axis_set_x(&axes, 300);
    touchpad_axis_set_y(&axes, 400);
    touchpad_axis_set_x(&axes, 325);
    if (axes.x[1] != 325 || axes.y[1] != 400 || axes.current_slot != 1) return 2;

    touchpad_axis_reset(&axes);
    if (axes.x[0] != -1 || axes.y[0] != -1 || axes.x[1] != -1 ||
        axes.y[1] != -1 || axes.current_slot != 0) return 3;
    return 0;
}
EOF

gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/touchpad-axis.c" -o "$TMP/touchpad-axis"
"$TMP/touchpad-axis"
grep -Fq 'touchpad_axis_state_t axes' "$ROOT/fifi/platform/linux/input.c"
grep -Fq 'touchpad_axis_select_slot(&dev->axes, cur_slot)' \
    "$ROOT/fifi/platform/linux/input.c"
echo "[input-test] persistent touchpad axes and MT slots passed"
