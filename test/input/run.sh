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

cat > "$TMP/touchpad-motion.c" <<'EOF'
#include "fifi/platform/linux/include/touchpad_motion.h"

int main(void) {
    touchpad_motion_state_t motion;
    int32_t dx, dy;
    touchpad_motion_reset(&motion);

    /* First contact is an anchor, never a cursor jump. */
    if (touchpad_motion_update(&motion, 1000, 500, 4000, 1000, 0, 0,
                               2000, 1000, &dx, &dy)) return 1;

    /* Equal normalized X/Y movement must cover the same fraction of each
     * screen axis even when the hardware ranges differ. Motion is immediate. */
    if (!touchpad_motion_update(&motion, 1040, 510, 4000, 1000, 0, 0,
                                2000, 1000, &dx, &dy)) return 2;
    if (dx != 30 || dy != 15) return 3;

    /* Slow subpixel motion accumulates instead of being rounded away. */
    touchpad_motion_reset(&motion);
    touchpad_motion_update(&motion, 0, 0, 4000, 4000, 0, 0,
                           1000, 1000, &dx, &dy);
    for (int i = 1; i <= 3; ++i) {
        if (touchpad_motion_update(&motion, i, 0, 4000, 4000, 0, 0,
                                   1000, 1000, &dx, &dy)) return 4;
    }
    if (!touchpad_motion_update(&motion, 4, 0, 4000, 4000, 0, 0,
                                1000, 1000, &dx, &dy) || dx != 1) return 5;

    /* A contact discontinuity re-anchors without crossing the screen. */
    if (touchpad_motion_update(&motion, 1000, 1000, 4000, 4000, 0, 0,
                               1000, 1000, &dx, &dy)) return 6;
    if (touchpad_motion_update(&motion, 1001, 1001, 4000, 4000, 0, 0,
                               1000, 1000, &dx, &dy)) return 7;

    /* Hardware-declared fuzz suppresses stationary chatter without averaging
     * genuine movement across later frames. */
    touchpad_motion_reset(&motion);
    touchpad_motion_update(&motion, 100, 100, 1000, 1000, 3, 3,
                           1000, 1000, &dx, &dy);
    if (touchpad_motion_update(&motion, 102, 98, 1000, 1000, 3, 3,
                               1000, 1000, &dx, &dy)) return 8;
    if (!touchpad_motion_update(&motion, 108, 100, 1000, 1000, 3, 3,
                                1000, 1000, &dx, &dy) || dx != 6 || dy != 0)
        return 9;
    return 0;
}
EOF

gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/touchpad-motion.c" -o "$TMP/touchpad-motion"
"$TMP/touchpad-motion"
grep -Fq '#include "touchpad_motion.h"' "$ROOT/fifi/platform/linux/input.c"
grep -Fq 'dev->x_max - dev->x_min + 1' "$ROOT/fifi/platform/linux/input.c"
grep -Fq 'dev->x_fuzz, dev->y_fuzz' "$ROOT/fifi/platform/linux/input.c"
grep -Fq 'input_remove_touchpad_companion(dev->phys)' \
    "$ROOT/fifi/platform/linux/input.c"
! grep -Fq 'ema_x_q8' "$ROOT/fifi/platform/linux/input.c"
echo "[input-test] touchpad motion is immediate and axis-correct"

cat > "$TMP/event-handoff.c" <<'EOF'
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "fifi/compositor/event_handoff.h"

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static fifi_event_handoff_t handoff = FIFI_EVENT_HANDOFF_INIT;
static atomic_bool processed = ATOMIC_VAR_INIT(false);

static void *event_thread(void *unused) {
    (void)unused;
    fifi_event_handoff_request(&handoff);
    pthread_mutex_lock(&mx);
    fifi_event_handoff_acquired(&handoff);
    atomic_store_explicit(&processed, true, memory_order_release);
    pthread_mutex_unlock(&mx);
    return NULL;
}

int main(void) {
    pthread_t thread;
    pthread_mutex_lock(&mx); /* model a software render in progress */
    if (pthread_create(&thread, NULL, event_thread, NULL) != 0) return 1;
    while (!fifi_event_handoff_is_waiting(&handoff)) {}

    fifi_event_handoff_yield(&handoff, &mx);
    bool event_ran = atomic_load_explicit(&processed, memory_order_acquire);
    pthread_mutex_unlock(&mx);
    pthread_join(thread, NULL);
    return event_ran ? 0 : 2;
}
EOF

gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/event-handoff.c" -o "$TMP/event-handoff" -lpthread
"$TMP/event-handoff"

input_line="$(grep -n '^        input_poll_motion();' "$ROOT/fifi/compositor/main.c" | cut -d: -f1)"
ipc_line="$(grep -n '^        ipc_poll();' "$ROOT/fifi/compositor/main.c" | cut -d: -f1)"
test "$input_line" -lt "$ipc_line"
echo "[input-test] compositor yields to input and reads evdev first"

grep -Fq 'DRM_MODE_CURSOR_BO' "$ROOT/fifi/platform/linux/drm.c"
grep -Fq 'DRM_MODE_CURSOR_MOVE' "$ROOT/fifi/platform/linux/drm.c"
grep -Fq 'while (pthread_mutex_trylock(&g_mx) != 0)' \
    "$ROOT/fifi/compositor/main.c"
grep -Fq 'input_flush_deferred_clicks();' "$ROOT/fifi/compositor/main.c"
grep -Fq 'pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)' \
    "$ROOT/fifi/platform/linux/input.c"
echo "[input-test] KMS cursor keeps draining input during slow frames"

grep -Fq 'pointer_thread_fn' "$ROOT/fifi/compositor/main.c"
grep -Fq 'input_get_pointer_fds(pointer_fds, 64)' \
    "$ROOT/fifi/compositor/main.c"
grep -Fq 'input_poll_controls();' "$ROOT/fifi/compositor/main.c"
grep -Fq 'input_poll_mode(false, true)' "$ROOT/fifi/platform/linux/input.c"
grep -Fq 'pthread_join(pointer_tid, NULL)' "$ROOT/fifi/compositor/main.c"
echo "[input-test] pointer evdev has an independent event-driven thread"

# Pointer readers must stop at each kernel report. Draining through several
# SYN_REPORT boundaries collapses a batched touchpad gesture into one jump and
# makes the hardware cursor visibly stall between those jumps.
test "$(grep -c 'if (ev.code == SYN_REPORT) break;' \
    "$ROOT/fifi/platform/linux/input.c")" -eq 2
test "$(grep -c 'ev.code == SYN_DROPPED' \
    "$ROOT/fifi/platform/linux/input.c")" -eq 2
grep -Fq 'touchpad_motion_reset(&dev->motion);' \
    "$ROOT/fifi/platform/linux/input.c"
echo "[input-test] every evdev motion report reaches the hardware cursor"

grep -Fq 'inotify_add_watch(g_hotplug_fd, "/dev/input"' \
    "$ROOT/fifi/platform/linux/input.c"
grep -Fq 'if (input_hotplug_pending())' "$ROOT/fifi/compositor/main.c"
! grep -Fq '_rescan_ticks' "$ROOT/fifi/compositor/main.c"
echo "[input-test] device rescans happen only after hotplug notifications"

grep -Fq 'input_is_touchpad_companion(phys)' \
    "$ROOT/fifi/platform/linux/input.c"
grep -Fq '[input] rescan: touchpad companion still skipped' \
    "$ROOT/fifi/platform/linux/input.c"
grep -Fq 'dev->is_touchpad = is_pointer && !is_direct;' \
    "$ROOT/fifi/platform/linux/input.c"
echo "[input-test] rescans cannot restore a duplicate touchpad companion"

SEND_LOGS="$ROOT/initramfs/root/bin/send-logs"
grep -Fq "ip -4 addr show scope global" "$SEND_LOGS"
grep -Fq "tail -n 2000 /fifi-data/compositor.log" "$SEND_LOGS"
grep -Fq "nc -w 15" "$SEND_LOGS"
! grep -Fq "udhcpc" "$SEND_LOGS"
echo "[input-test] log transfer preserves the active Wi-Fi connection"
