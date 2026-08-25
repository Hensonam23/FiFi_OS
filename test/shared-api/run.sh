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
_Static_assert(FIFI_IPC_MAX_PAYLOAD == 64u * 1024u * 1024u, "payload limit changed");
_Static_assert(IPC_APP_CONNECT == 0x01u, "application IDs changed");
_Static_assert(IPC_WIN_CREATED == 0x10u, "compositor IDs changed");
_Static_assert(IPC_ADD_DESK_ICON == 0x1fu, "message range changed");
int main(void) { return 0; }
EOF
gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/ipc-contract.c" -o "$TMP/ipc-contract"
"$TMP/ipc-contract"

echo "[test-shared-api] shared Linux app transport preserves wire framing"
cat > "$TMP/app-ipc-contract.c" <<'EOF'
#include "fifi/shared/app_ipc.h"
#include <sys/socket.h>
#include <unistd.h>

static int read_exact(int fd, void *data, size_t length) {
    uint8_t *bytes = data;
    while (length) {
        ssize_t got = read(fd, bytes, length);
        if (got <= 0) return 1;
        bytes += got;
        length -= (size_t)got;
    }
    return 0;
}

int main(void) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 1;
    const char text[] = "ready";
    if (!fifi_app_ipc_send(sockets[0], IPC_NOTIFY, text, sizeof(text) - 1u)) return 2;
    uint32_t header[2];
    char received[sizeof(text)] = {0};
    if (read_exact(sockets[1], header, sizeof(header)) ||
        read_exact(sockets[1], received, sizeof(text) - 1u)) return 3;
    if (header[0] != IPC_NOTIFY || header[1] != sizeof(text) - 1u ||
        memcmp(received, text, sizeof(text) - 1u) != 0) return 4;

    uint32_t pixels[6] = {1, 2, 3, 4, 5, 6};
    if (!fifi_app_ipc_send_frame(sockets[0], 3, 2, pixels)) return 5;
    uint32_t frame[4];
    if (read_exact(sockets[1], header, sizeof(header)) ||
        read_exact(sockets[1], frame, sizeof(frame))) return 6;
    if (header[0] != IPC_APP_FRAME || header[1] != sizeof(frame) + sizeof(pixels) ||
        frame[0] != 0 || frame[1] != 0 || frame[2] != 3 || frame[3] != 2) return 7;
    uint32_t copied[6];
    if (read_exact(sockets[1], copied, sizeof(copied)) ||
        memcmp(copied, pixels, sizeof(pixels)) != 0) return 8;
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}
EOF
gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/app-ipc-contract.c" -o "$TMP/app-ipc-contract"
"$TMP/app-ipc-contract"

echo "[test-shared-api] shared bitmap UI validates fonts and clips drawing"
cat > "$TMP/app-ui-contract.c" <<'EOF'
#include "fifi/shared/app_ui.h"
#include <stdio.h>
#include <string.h>

static void put_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    _Static_assert(FIFI_APP_UI_API_VERSION == 1u, "unexpected UI version");

    uint8_t font_file[32 + 256 * 8] = {0};
    put_le32(font_file, 0x864ab572u);
    put_le32(font_file + 8, 32);
    put_le32(font_file + 16, 256);
    put_le32(font_file + 20, 8);
    put_le32(font_file + 24, 8);
    put_le32(font_file + 28, 8);
    memset(font_file + 32 + 'A' * 8, 0x81, 8);
    FILE *file = fopen(argv[1], "wb");
    if (!file || fwrite(font_file, 1, sizeof(font_file), file) != sizeof(font_file) ||
        fclose(file) != 0) return 2;

    fifi_ui_font_t font = {0};
    if (!fifi_ui_font_load_psf2(&font, argv[1]) || font.advance != 9 ||
        font.height != 8 || font.glyph_count != 256) return 3;

    uint32_t pixels[6 * 4] = {0};
    fifi_ui_canvas_t canvas = { pixels, 6, 4 };
    fifi_ui_fill(canvas, -2, -2, 4, 4, 0x11u);
    if (pixels[0] != 0x11u || pixels[1] != 0x11u || pixels[2] != 0) return 4;
    fifi_ui_glyph(canvas, &font, 4, 0, 'A', 0x22u, 0);
    if (pixels[4] != 0x22u || pixels[5] == 0x22u) return 5;

    file = fopen(argv[2], "wb");
    if (!file || fwrite(font_file, 1, 40, file) != 40 || fclose(file) != 0) return 6;
    if (fifi_ui_font_load_psf2(&font, argv[2])) return 7;
    if (!font.glyphs || font.advance != 9) return 8;
    fifi_ui_font_destroy(&font);
    return 0;
}
EOF
gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT" \
    "$TMP/app-ui-contract.c" -o "$TMP/app-ui-contract"
"$TMP/app-ui-contract" "$TMP/valid.psf" "$TMP/truncated.psf"
grep -Fq '#include "../../shared/app_ui.h"' \
    "$ROOT/fifi/apps/browser/browser.c"
grep -Fq '#include "../../shared/app_ui.h"' \
    "$ROOT/fifi/apps/installer/installer.c"
grep -Fq '../../shared/app_ui.h' "$ROOT/fifi/apps/browser/Makefile"
grep -Fq '../../shared/app_ui.h' "$ROOT/fifi/apps/installer/Makefile"
if grep -Eq 'static (uint32_t psf2_u32|bool load_font.+open\()' \
    "$ROOT/fifi/apps/browser/browser.c" "$ROOT/fifi/apps/installer/installer.c"; then
    echo "private PSF2 loader remains in a migrated native app" >&2
    exit 1
fi

for app in aichat appstore browser calc editor filebrowser gamepad imageviewer installer netmon proton security settings sysmon terminal wifi; do
    grep -Fq '#include "../../shared/app_ipc.h"' "$ROOT/fifi/apps/$app/$app.c"
    grep -Fq '../../shared/app_ipc.h' "$ROOT/fifi/apps/$app/Makefile"
    if grep -Fq 'socket(AF_UNIX' "$ROOT/fifi/apps/$app/$app.c" ||
       grep -Fq 'IPC_APP_CONNECT' "$ROOT/fifi/apps/$app/$app.c"; then
        echo "private compositor connection remains in fifi/apps/$app/$app.c" >&2
        exit 1
    fi
done

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
grep -Fq 'FIFI_IPC_MAX_PAYLOAD' "$ROOT/fifi/platform/linux/ipc.c"
grep -Fq 'IPC, bitmap UI, and theme API version 1 are stable' \
    "$ROOT/docs/LINUX_DESKTOP_API.md"

if grep -REn --include='*.[ch]' '^#define IPC_(APP_(CONNECT|FRAME|TITLE|CLOSE)|WIN_(CREATED|RESIZE)|INPUT_(KEY|MOUSE|GAMEPAD)|FOCUS|INVALIDATE|NOTIFY|CLIP_(SET|GET|DATA)|OPEN_FILE|DRAG_START|DROP_FILE|SET_WALLPAPER|ADD_DESK_ICON)' \
    "$ROOT/fifi/apps" "$ROOT/fifi/platform"; then
    echo "private IPC message definition found outside the shared contract" >&2
    exit 1
fi

echo "[test-shared-api] PASS"
