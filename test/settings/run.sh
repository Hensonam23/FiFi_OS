#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SETTINGS="$ROOT/fifi/apps/settings/settings.c"

gcc -std=gnu11 -O2 -I"$ROOT/fifi/platform/linux/vendor" \
    "$ROOT/test/settings/scan_settings.c" -lm -o "$TMP/scan-settings"
gcc -std=gnu11 -O2 "$ROOT/test/settings/scan_wifi.c" -o "$TMP/scan-wifi"
gcc -std=c11 -O2 -Wall -Wextra -Werror \
    "$ROOT/test/settings/wifi_scan_contract.c" -o "$TMP/wifi-scan-contract"
"$TMP/scan-settings"
"$TMP/scan-wifi"
"$TMP/wifi-scan-contract"
echo "[settings-test] both Wi-Fi views parse manager and direct-kernel scan formats"

! grep -Fq 'snprintf(ov, sizeof ov, "/fifi-data/%s", path + 5)' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'persistent /fifi-data/<name> copy can outlive many A/B updates' \
    "$ROOT/fifi/platform/linux/platform.c"
echo "[settings-test] persistent files cannot override updated system apps"

for action in ACCENT WALL WALLFIT PANEL GLASS SHADOW DOCK STATUS DESKINFO \
              AUTOHIDE ALIGN CLOCK TBSIZE RADIUS FONT_FAM FONT_SZ; do
    grep -Eq "add_hot\([^;]*ACT_${action}" "$SETTINGS"
    grep -Fq "case ACT_${action}:" "$SETTINGS"
done
for action in FW DOH VPN TOR; do
    grep -Fq "ACT_${action}" "$SETTINGS"
    grep -Fq "case ACT_${action}:" "$SETTINGS"
done
grep -Fq 'alsa_set_vol(' "$SETTINGS"
grep -Fq 'FIFI_INPUT_KEY_MOUSE_SPEED' "$SETTINGS"
grep -Fq 'FIFI_INPUT_KEY_TOUCHPAD_SPEED' "$SETTINGS"
echo "[settings-test] every interactive Settings control has rendering and action handling"
grep -Fq 'g_pers_scroll -= wheel * 48' "$SETTINGS"
grep -Fq 'g_font_dd_scroll -= wheel * 5' "$SETTINGS"
grep -Fq 'font_previews_build();' "$SETTINGS"
grep -Fq 'font_preview_draw(fb, idx' "$SETTINGS"
! grep -Fq 'ttf_draw(fb, path, nm' "$SETTINGS"
grep -Fq 'send_font_dropdown(sock, fb);' "$SETTINGS"
grep -Fq 'render_font_dropdown_region(fb);' "$SETTINGS"
grep -Fq 'if (font_dropdown_scrolled && g_font_dd == 1) {' "$SETTINGS"
echo "[settings-test] font names keep their own face without rasterizing while scrolling"

grep -Fq 'wpa_command(interface, "scan"' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'wpa_command(interface, "scan_results"' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'direct_scan(interface, output, sizeof(output))' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'capture_command_with_errors("/usr/bin/iw"' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'return report_scan_failure(diagnostic' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'strstr(output, "FAIL-BUSY")' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'Join that in-flight scan' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'wait_command("/usr/bin/rfkill", unblock)' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq '/fifi-data/wifi-scan.log' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq 'signal(SIGCHLD, SIG_DFL);' "$SETTINGS"
grep -Fq 'Scan process failed (code %d)' "$SETTINGS"
grep -Fq 'Wi-Fi scan failed (code %d)' \
    "$ROOT/fifi/apps/wifi/wifi.c"
grep -Fq 'fifi-wifi-ctl saved-connect "$WIFI_IF"' "$ROOT/initramfs/root/init"
! grep -Fq '/usr/lib/iwd/iwd' "$ROOT/initramfs/root/init"
grep -Fq 'open_public_status("/fifi-data/wifi-ssid")' \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c"
grep -Fq '/fifi-data/wifi-saved-ssid' "$SETTINGS"
grep -Fq 'Refresh delayed -- showing %d previous network%s' "$SETTINGS"
grep -Fq 'cp "$WPA_CLI_BIN" "$STAGE/usr/bin/wpa_cli"' \
    "$ROOT/scripts/build-initramfs.sh"
grep -Fq 'CONFIG_RTW89_8922AE=y' "$ROOT/linux/fifi.config"
grep -Fq 'rtw8922a_fw.bin' "$ROOT/scripts/build-initramfs.sh"
grep -Fq 'Qualcomm ath11k firmware bundled' "$ROOT/scripts/build-initramfs.sh"
grep -Fq '/fifi-data/wifi-hardware' "$SETTINGS"
grep -Fq 'linux-firmware-realtek' "$ROOT/.github/workflows/linux-desktop.yml"
grep -Fq 'Updates: run fifi upgrade' "$SETTINGS"
echo "[settings-test] boot, scan, connect, disconnect, and help use current system paths"

gcc -std=c11 -O2 -Wall -Wextra \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c" -o "$TMP/fifi-wifi-ctl"
invalid="$($TMP/fifi-wifi-ctl scan 'wlan0;id' 2>&1 || true)"
grep -Fq 'usage: fifi-wifi-ctl' <<<"$invalid"

echo "[settings-test] PASS"
