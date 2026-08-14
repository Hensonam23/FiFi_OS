#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[test-shared-api] versioned IPC contract compiles"
cat > "$TMP/ipc-contract.c" <<'EOF'
#include "fifi/shared/ipc.h"
_Static_assert(FIFI_IPC_VERSION == 1u, "unexpected IPC version");
_Static_assert(IPC_HDR_SZ == 8u, "wire header changed");
_Static_assert(IPC_APP_CONNECT == 0x01u, "application IDs changed");
_Static_assert(IPC_WIN_CREATED == 0x10u, "compositor IDs changed");
_Static_assert(IPC_ADD_DESK_ICON == 0x1fu, "message range changed");
int main(void) { return 0; }
EOF
gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/ipc-contract.c" -o "$TMP/ipc-contract"
"$TMP/ipc-contract"

echo "[test-shared-api] shared theme contract compiles for Linux consumers"
cat > "$TMP/theme-contract.c" <<'EOF'
#include "fifi/shared/theme.h"
static const unsigned accents[] = FIFI_ACCENT_PRESETS;
static const int sizes[] = FIFI_FONT_SIZES;
_Static_assert(FIFI_THEME_API_VERSION == 1u, "unexpected theme version");
_Static_assert(FIFI_THEME_CONFIG_VERSION == 1u, "unexpected config version");
_Static_assert(FIFI_THEME_DEFAULT_ACCENT == 0x00409cffu, "accent default changed");
_Static_assert(FIFI_THEME_DEFAULT_CORNER_RADIUS == 9, "radius default changed");
_Static_assert(FIFI_THEME_DEFAULT_FONT_PX == 20, "font default changed");
_Static_assert(WALLPAPER_IMAGE == 5 && WALLPAPER_COUNT == 13, "wallpaper IDs changed");
_Static_assert(PANEL_BOTTOM == 0 && PANEL_RIGHT == 3, "panel IDs changed");
_Static_assert(sizeof(accents) / sizeof(accents[0]) == FIFI_ACCENT_PRESET_COUNT,
               "accent count changed");
_Static_assert(sizeof(sizes) / sizeof(sizes[0]) == 13, "font sizes changed");
int main(void) { return 0; }
EOF
gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/theme-contract.c" -o "$TMP/theme-contract"
"$TMP/theme-contract"
grep -Fq '#include "../../fifi/shared/theme.h"' \
    "$ROOT/kernel/src/gui_internal.h"
grep -Fq '#include "../../shared/theme.h"' \
    "$ROOT/fifi/apps/settings/settings.c"
grep -Fq 'FIFI_THEME_CONFIG_FORMAT_KEY "=%u' "$ROOT/kernel/src/gui.c"
grep -Fq 'cfg_set_uint(FIFI_THEME_CONFIG_FORMAT_KEY, FIFI_THEME_CONFIG_VERSION)' \
    "$ROOT/fifi/apps/settings/settings.c"
grep -Fq 'FIFI_THEME_KEY_ACCENT' "$ROOT/kernel/src/gui.c"
grep -Fq 'FIFI_THEME_KEY_ACCENT' "$ROOT/fifi/apps/settings/settings.c"
grep -Fq 'FIFI_THEME_DEFAULT_CORNER_RADIUS' "$ROOT/kernel/src/gui.c"
grep -Fq 'FIFI_THEME_DEFAULT_CORNER_RADIUS' "$ROOT/fifi/apps/settings/settings.c"

echo "[test-shared-api] every IPC producer and consumer uses the contract"
while IFS= read -r source; do
    grep -Fq '#include "../../shared/ipc.h"' "$source" || {
        echo "shared IPC include missing from ${source#$ROOT/}" >&2
        exit 1
    }
    makefile="$(dirname "$source")/Makefile"
    if [[ "$source" == */apps/* ]] &&
       ! grep -Fq '../../shared/ipc.h' "$makefile"; then
        echo "shared IPC dependency missing from ${makefile#$ROOT/}" >&2
        exit 1
    fi
done < <(grep -Rl --include='*.c' 'IPC_APP_CONNECT' "$ROOT/fifi/apps" \
    "$ROOT/fifi/platform/linux" | sort)

grep -Fq '$(BUILD)/comp/ipc.o: ../platform/linux/ipc.c ../shared/ipc.h' \
    "$ROOT/fifi/compositor/Makefile"

if grep -REn --include='*.[ch]' '^#define IPC_(APP_(CONNECT|FRAME|TITLE|CLOSE)|WIN_(CREATED|RESIZE)|INPUT_(KEY|MOUSE|GAMEPAD)|FOCUS|INVALIDATE|NOTIFY|CLIP_(SET|GET|DATA)|OPEN_FILE|DRAG_START|DROP_FILE|SET_WALLPAPER|ADD_DESK_ICON)' \
    "$ROOT/fifi/apps" "$ROOT/fifi/platform"; then
    echo "private IPC message definition found outside the shared contract" >&2
    exit 1
fi

echo "[test-shared-api] PASS"
