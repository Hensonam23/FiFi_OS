#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
broker_pid=""
cleanup() {
    if [[ -n "$broker_pid" ]] && kill -0 "$broker_pid" 2>/dev/null; then
        kill "$broker_pid" 2>/dev/null || true
        wait "$broker_pid" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

echo "[test-security] legacy developer key is absent"
test ! -e "$ROOT/initramfs/root/usr/share/fifi/authorized_keys"
! grep -q '/usr/share/fifi/authorized_keys' "$ROOT/initramfs/root/init"

echo "[test-security] SSH is owner opt-in"
grep -Fq '[ -s /fifi-data/ssh/authorized_keys ]' "$ROOT/initramfs/root/init"
! grep -Eq '^[[:space:]]*tcp dport 22 accept' \
    "$ROOT/initramfs/root/etc/nftables.conf"

echo "[test-security] legacy auto-installed key is migrated automatically"
migration_root="$TMP/migration"
mkdir -p "$migration_root/ssh"
legacy_line='ssh-ed25519 test-legacy-key old-development-key'
owner_line='ssh-ed25519 test-owner-key owner@example.test'
legacy_sha="$(
    printf '%s\n' 'ssh-ed25519 test-legacy-key' |
        sha256sum | awk '{print $1}'
)"
printf '%s\n%s\n' "$legacy_line" "$owner_line" \
    > "$migration_root/ssh/authorized_keys"
FIFI_DATA_ROOT="$migration_root" FIFI_LEGACY_KEY_SHA256="$legacy_sha" \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-legacy-ssh-keys.sh"
grep -Fxq "$owner_line" "$migration_root/ssh/authorized_keys"
! grep -Fq 'test-legacy-key' "$migration_root/ssh/authorized_keys"
grep -Fxq "$legacy_line" \
    "$migration_root/ssh/authorized_keys.before-ssh-hardening"
test -e "$migration_root/ssh/.legacy-key-migrated"

echo "[test-security] pending update completion is automatic"
completion_root="$TMP/completion"
mock_bin="$TMP/completion-bin"
mkdir -p "$completion_root" "$mock_bin"
: > "$completion_root/post-update.pending"
cat > "$mock_bin/app-update" <<'EOF'
#!/bin/sh
test "$1" = -y
printf 'updated\n' >> "$FIFI_TEST_APP_UPDATE_LOG"
EOF
chmod +x "$mock_bin/app-update"
FIFI_DATA_ROOT="$completion_root" FIFI_NETWORK_READY=1 \
FIFI_TEST_APP_UPDATE_LOG="$completion_root/apps.log" \
PATH="$mock_bin:$PATH" \
    sh "$ROOT/initramfs/root/usr/share/fifi/complete-pending-update.sh"
test ! -e "$completion_root/post-update.pending"
grep -Fxq updated "$completion_root/apps.log"
grep -Fq '[post-update] complete' "$completion_root/update-completion.log"

echo "[test-security] non-root launcher enforces the desktop identity"
gcc -std=c11 -O2 -Wall -Wextra \
    "$ROOT/fifi/platform/linux/fifi-user-exec.c" \
    -o "$TMP/fifi-user-exec"
test "$("$TMP/fifi-user-exec" id -u)" = 1000
test "$("$TMP/fifi-user-exec" id -g)" = 1000
grep -Fq 'chown(FIFI_SOCK, 0, 1000)' \
    "$ROOT/fifi/platform/linux/ipc.c"
grep -Fq '(cr.uid != 0 && cr.uid != 1000)' \
    "$ROOT/fifi/platform/linux/ipc.c"
grep -Fq 'chown(g_sock_path, 1000, 1000)' \
    "$ROOT/fifi/platform/linux/wayland.c"
grep -Fq 'CONFIG_USER_NS=y' "$ROOT/linux/fifi.config"
grep -Fq 'PR_SET_NO_NEW_PRIVS' \
    "$ROOT/fifi/platform/linux/fifi-user-exec.c"
grep -Fq 'strcmp(name, "fifi-terminal") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq '/bin/fifi-user-exec "$target" --appimage-extract' \
    "$ROOT/initramfs/root/usr/share/fifi/fifi-run"
! grep -Fq -- '--no-sandbox' "$ROOT/fifi/apps/browser/browser.c"
! grep -Fq -- '--no-sandbox' "$ROOT/initramfs/root/bin/fifi-download-browser.sh"
! grep -Fq 'export MOZ_DISABLE_CONTENT_SANDBOX=1' \
    "$ROOT/initramfs/root/usr/share/fifi/fifi-run"
! grep -Fq 'export ELECTRON_DISABLE_SANDBOX=1' \
    "$ROOT/initramfs/root/usr/share/fifi/fifi-run"

echo "[test-security] administrative actions cross a narrow root broker"
gcc -std=c11 -O2 -Wall -Wextra \
    "$ROOT/fifi/platform/linux/fifi-admin.c" \
    -o "$TMP/fifi-admin"
broker_bin="$TMP/broker-bin"
broker_socket="$TMP/fifi-admin.sock"
broker_log="$TMP/admin.log"
mkdir -p "$broker_bin"
cat > "$broker_bin/fifi-secctl" <<'EOF'
#!/bin/sh
printf 'security %s %s\n' "$1" "$2" >> "$FIFI_TEST_ADMIN_LOG"
EOF
cat > "$broker_bin/tcpdump" <<'EOF'
#!/bin/sh
printf 'capture args: %s\n' "$*"
EOF
cat > "$broker_bin/fifi-wifi-ctl" <<'EOF'
#!/bin/sh
printf 'wifi %s %s\n' "$1" "$2" >> "$FIFI_TEST_ADMIN_LOG"
if [ "$1" = connect ]; then
    od -An -v -tx1 >> "$FIFI_TEST_ADMIN_LOG"
    printf 'FIFI_WIFI_OK\n'
fi
EOF
chmod +x "$broker_bin/fifi-secctl" "$broker_bin/tcpdump" \
    "$broker_bin/fifi-wifi-ctl"
FIFI_ADMIN_SOCKET="$broker_socket" \
FIFI_ADMIN_ALLOWED_UID="$(id -u)" FIFI_ADMIN_GID="$(id -g)" \
FIFI_SECCTL="$broker_bin/fifi-secctl" FIFI_TCPDUMP="$broker_bin/tcpdump" \
FIFI_WIFI_CTL="$broker_bin/fifi-wifi-ctl" \
FIFI_TEST_ADMIN_LOG="$broker_log" \
    "$TMP/fifi-admin" --daemon &
broker_pid=$!
for _ in $(seq 1 50); do
    [[ -S "$broker_socket" ]] && break
    sleep 0.05
done
[[ -S "$broker_socket" ]]
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" security firewall on
grep -Fxq 'security firewall on' "$broker_log"
capture_out="$(FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" capture)"
grep -Fq 'capture args: -c 20 -nn -i any -q' <<<"$capture_out"
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" wifi scan wlan0
printf '\000\010Cafe Net\000\013secret pass' |
    FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" wifi connect wlan0
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" wifi disconnect wlan0
grep -Fxq 'wifi scan wlan0' "$broker_log"
grep -Fxq 'wifi connect wlan0' "$broker_log"
grep -Fq '00 08 43 61 66 65 20 4e 65 74 00 0b 73 65 63 72' "$broker_log"
grep -Fq '65 74 20 70 61 73 73' "$broker_log"
grep -Fxq 'wifi disconnect wlan0' "$broker_log"
denied_out="$(FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" shell root 2>&1)"
grep -Fq 'operation is not allowed' <<<"$denied_out"
bad_iface="$(FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" wifi scan 'wlan0;id' 2>&1)"
grep -Fq 'operation is not allowed' <<<"$bad_iface"
grep -Fq 'strcmp(name, "fifi-security") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strcmp(name, "fifi-wifi") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strcmp(name, "fifi-settings") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'chown 1000:1000 /fifi-data/fifi-settings.conf' \
    "$ROOT/initramfs/root/init"
grep -Fq 'chown 0:1000 "$_audio_ctl"' "$ROOT/initramfs/root/init"
! grep -Fq 'fopen("/fifi-data/wifi-ssid", "w")' \
    "$ROOT/fifi/apps/settings/settings.c"
grep -Fq 'execl("/bin/fifi-admin", "fifi-admin", "security"' \
    "$ROOT/fifi/apps/security/security.c"
! grep -Fq 'execl("/usr/bin/dnscrypt-proxy"' \
    "$ROOT/fifi/apps/security/security.c"
! grep -Fq 'execl("/usr/bin/tor"' "$ROOT/fifi/apps/security/security.c"
! grep -Fq 'execl("/bin/ip", "ip", "link", "del", "wg0"' \
    "$ROOT/fifi/apps/security/security.c"
grep -Fq 'execl("/bin/fifi-admin", "fifi-admin", "wifi", "connect", g_wif' \
    "$ROOT/fifi/apps/wifi/wifi.c"
! grep -Fq 'execl("/usr/bin/wpa_supplicant"' "$ROOT/fifi/apps/wifi/wifi.c"
! grep -Fq 'execl("/usr/bin/wpa_supplicant"' "$ROOT/fifi/apps/settings/settings.c"
gcc -std=c11 -O2 -Wall -Wextra \
    "$ROOT/fifi/platform/linux/fifi-wifi-ctl.c" \
    -o "$TMP/fifi-wifi-ctl"
invalid_wifi_out="$("$TMP/fifi-wifi-ctl" scan 'wlan0;id' 2>&1 || true)"
grep -Fq 'usage: fifi-wifi-ctl' <<<"$invalid_wifi_out"
kill "$broker_pid"
wait "$broker_pid" 2>/dev/null || true
broker_pid=""

echo "[test-security] Steam container cannot retain host-root privileges"
steam_block="$TMP/steam-block"
sed -n '/# Steam (ivan-hc RunImage)/,/# Other RunImage\/sharun bundles/p' \
    "$ROOT/initramfs/root/usr/share/fifi/fifi-run" > "$steam_block"
grep -Fq 'exec /bin/fifi-user-exec busybox unshare -r -m --' "$steam_block"
! grep -Eq '^[[:space:]]*exec busybox unshare' "$steam_block"
grep -Fq 'HOME=/root USER=root LOGNAME=root' "$steam_block"

echo "[test-security] ignored legacy key is removed from staged images"
stage="$TMP/stage"
mkdir -p "$stage/usr/share/fifi"
printf 'ssh-ed25519 test-only-placeholder\n' \
    > "$stage/usr/share/fifi/authorized_keys"
bash "$ROOT/scripts/sanitize-initramfs-stage.sh" "$stage"
test ! -e "$stage/usr/share/fifi/authorized_keys"

echo "[test-security] unexpected credentials fail the build"
mkdir -p "$stage/root/.ssh"
printf '%s\n' '-----BEGIN OPENSSH PRIVATE KEY-----' \
    > "$stage/root/.ssh/id_ed25519"
if bash "$ROOT/scripts/sanitize-initramfs-stage.sh" "$stage"; then
    echo "credential scan unexpectedly succeeded" >&2
    exit 1
fi

echo "[test-security] release verification key is allowed"
rm -rf "$stage/root/.ssh"
mkdir -p "$stage/etc"
cp "$ROOT/security/release-signing-public.pem" \
    "$stage/etc/fifi-release-signing.pub"
bash "$ROOT/scripts/sanitize-initramfs-stage.sh" "$stage"

echo "[test-security] compositor crashes are supervised"
grep -Fq '# ── Supervise FiFi compositor' "$ROOT/initramfs/root/init"
grep -Fq '"$FIFI_COMPOSITOR" 2>>/fifi-data/compositor.log' \
    "$ROOT/initramfs/root/init"
grep -Fq 'compositor exited status=%s; restarting in 2s' \
    "$ROOT/initramfs/root/init"

echo "[test-security] USB update boot cannot terminate PID 1"
grep -Fq '"$(cat "$sys_part/removable" 2>/dev/null)" = 1' \
    "$ROOT/initramfs/root/bin/update-usb"
update_boot_block="$TMP/update-boot-block"
sed -n '/if \[ "$FIFI_UPDATE_BOOT" = 1 \]/,/# Track the running image/p' \
    "$ROOT/initramfs/root/init" > "$update_boot_block"
test "$(grep -Fc 'while :; do sleep 3600; done' "$update_boot_block")" -eq 2
! grep -Eq 'exec /bin/(ba)?sh|exit [0-9]' "$update_boot_block"
grep -Fq 'menuentry "Update Installed FiFi OS"' \
    "$ROOT/scripts/flash-linux-usb.sh"
grep -Fq 'fifi_live fifi_update' "$ROOT/scripts/flash-linux-usb.sh"

echo "[test-security] PASS"
