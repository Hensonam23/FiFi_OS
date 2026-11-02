#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

gcc -std=c11 -O2 -Wall -Wextra -Werror \
    "$ROOT/test/profile/profile_contract.c" -o "$TMP/profile-contract"
"$TMP/profile-contract"

grep -Fq '"General", "Build", "Research", "Play"' \
    "$ROOT/kernel/src/gui_render.c"
grep -Fq 'system_profile_set(i);' "$ROOT/kernel/src/gui.c"
grep -Fq 'case IPC_SYSTEM_PROFILE:' "$ROOT/fifi/platform/linux/ipc.c"
grep -Fq 'Workload Profile' "$ROOT/fifi/apps/settings/settings.c"
grep -Fq 'system_profile_init();' "$ROOT/fifi/compositor/main.c"
grep -Fq 'fifi_system_profile_policy(g_system_profile).event_poll_ms' \
    "$ROOT/fifi/compositor/main.c"
grep -Fq '.inhibit_display_blanking' "$ROOT/fifi/compositor/main.c"
grep -Fq '_boot_governor=schedutil' "$ROOT/initramfs/root/init"
grep -Fq 'power_dpm_force_performance_level' "$ROOT/fifi/compositor/main.c"
grep -Fq '"/sys/class/drm/card%d/device/power/control"' \
    "$ROOT/fifi/compositor/main.c"

echo "[profile-test] four persisted workload profiles control system and compositor policy"
echo "[profile-test] PASS"
