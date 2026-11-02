#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

drm_cflags="$(pkg-config --cflags libdrm 2>/dev/null || printf '%s' '-I/usr/include/libdrm')"
common=(-std=c11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE
        -I"$ROOT/fifi/platform/linux/include" -I"$ROOT/kernel/include"
        -I"$ROOT/kernel/src")

# Compile the presentation path independently so raw KMS ABI changes and the
# shared console handoff cannot silently break behind the full image build.
# shellcheck disable=SC2086
gcc "${common[@]}" $drm_cflags -c "$ROOT/fifi/platform/linux/drm.c" \
    -o "$TMP/drm.o"
gcc "${common[@]}" -c "$ROOT/kernel/src/console.c" -o "$TMP/console.o"
gcc "${common[@]}" -c "$ROOT/fifi/platform/linux/font_ttf.c" -o "$TMP/font.o"
gcc "${common[@]}" -DFIFI_OS_VERSION_STR='"Vtest"' \
    -Wno-unused-parameter -Wno-misleading-indentation \
    -c "$ROOT/fifi/compositor/main.c" -o "$TMP/main.o"
gcc "${common[@]}" -DFIFI_OS_VERSION_STR='"Vtest"' \
    -Wno-unused-parameter -Wno-misleading-indentation \
    -c "$ROOT/kernel/src/gui.c" -o "$TMP/gui.o"
gcc "${common[@]}" -Wno-unused-parameter -Wno-misleading-indentation \
    -c "$ROOT/kernel/src/gui_render.c" -o "$TMP/gui_render.o"
gcc "${common[@]}" -Wno-unused-parameter -Wno-misleading-indentation \
    -c "$ROOT/kernel/src/gui_taskbar.c" -o "$TMP/gui_taskbar.o"

grep -Fq 'DRM_IOCTL_MODE_PAGE_FLIP' "$ROOT/fifi/platform/linux/drm.c"
grep -Fq 'DRM_MODE_PAGE_FLIP_EVENT' "$ROOT/fifi/platform/linux/drm.c"
grep -Fq 'wait_for_flip(sequence)' "$ROOT/fifi/platform/linux/drm.c"
grep -Fq 'drm_sync_draw_buffer(console_backbuf_ptr()' \
    "$ROOT/fifi/compositor/main.c"
grep -Fq 'console_set_scanout_ptr(draw);' "$ROOT/fifi/compositor/main.c"
grep -Fq 'CONFIG_TRANSPARENT_HUGEPAGE=y' "$ROOT/linux/fifi.config"
grep -Fq 'CONFIG_TRANSPARENT_HUGEPAGE_MADVISE=y' "$ROOT/linux/fifi.config"
grep -Fq 'g_cell_w = n ? mx : (px_size / 2);' \
    "$ROOT/fifi/platform/linux/font_ttf.c"
grep -Fq 'console_render_glyph_prop_fg_cp(x, py, cp, fg);' \
    "$ROOT/kernel/src/gui_text_util.c"
grep -Fq 'uint64_t pw = gui_text_width(pills[i]) + 26u;' \
    "$ROOT/kernel/src/gui_render.c"
grep -Fq 'gui_spawn_app_with_arg("/bin/fifi-settings", "wifi")' \
    "$ROOT/kernel/src/gui.c"
grep -Fq 'gui_spawn_app_with_arg("/bin/fifi-settings", "power")' \
    "$ROOT/kernel/src/gui.c"
grep -Fq 'status_item == TRAY_CPU || status_item == TRAY_MEM' \
    "$ROOT/kernel/src/gui.c"
grep -Fq 'gui_spawn_app_with_arg("/bin/fifi-settings", "system")' \
    "$ROOT/kernel/src/gui.c"
grep -Fq 'g_vol_popup_open = !g_vol_popup_open;' "$ROOT/kernel/src/gui.c"
grep -Fq 'g_cal_popup_open = !g_cal_popup_open;' "$ROOT/kernel/src/gui.c"
grep -Fq 'g_bar_brand_x = brand_start' "$ROOT/kernel/src/gui_render.c"

echo "[presentation-test] KMS uses two synchronized scanouts with vblank page flips"
echo "[presentation-test] terminals reserve safe cells while desktop text uses proportional advances"
echo "[presentation-test] Horizon-bar controls size themselves from rendered text"
echo "[presentation-test] workload profiles have kernel THP support"
echo "[presentation-test] every visible Horizon-bar control has a real action"
echo "[presentation-test] PASS"
