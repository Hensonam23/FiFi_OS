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

grep -Fq 'O_NOFOLLOW' "$ROOT/kernel/src/gui.c"
grep -Fq '!S_ISREG(st.st_mode) || st.st_nlink != 1' "$ROOT/kernel/src/gui.c"
grep -Fq 'fchown(fd, 1000, 1000)' "$ROOT/kernel/src/gui.c"
echo "[test-security] root settings writes reject user-controlled links"
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
if [ "$1" = scan ] && [ "$2" = fail0 ]; then
    echo 'mock Wi-Fi scan failure'
    exit 9
fi
if [ "$1" = connect ]; then
    od -An -v -tx1 >> "$FIFI_TEST_ADMIN_LOG"
    printf 'FIFI_WIFI_OK\n'
fi
EOF
cat > "$broker_bin/fifi-export-diagnostics" <<'EOF'
#!/bin/sh
echo 'diagnostics export complete'
printf 'diagnostics export\n' >> "$FIFI_TEST_ADMIN_LOG"
EOF
cat > "$broker_bin/fifi-apply-update" <<'EOF'
#!/bin/sh
printf 'update apply %s\n' "$1" >> "$FIFI_TEST_ADMIN_LOG"
EOF
cat > "$broker_bin/update-usb" <<'EOF'
#!/bin/sh
printf 'update usb %s\n' "${1:-}" >> "$FIFI_TEST_ADMIN_LOG"
echo 'mock USB failure'
exit 7
EOF
cat > "$broker_bin/update-rollback" <<'EOF'
#!/bin/sh
printf 'update rollback\n' >> "$FIFI_TEST_ADMIN_LOG"
EOF
cat > "$broker_bin/fifi-install.sh" <<'EOF'
#!/bin/sh
printf 'install %s %s %s %s\n' "$1" "$2" "$3" "$4" >> "$FIFI_TEST_ADMIN_LOG"
EOF
cat > "$broker_bin/fifi-powerctl" <<'EOF'
#!/bin/sh
printf 'power %s\n' "$1" >> "$FIFI_TEST_ADMIN_LOG"
EOF
chmod +x "$broker_bin/fifi-secctl" "$broker_bin/tcpdump" \
    "$broker_bin/fifi-wifi-ctl" "$broker_bin/fifi-apply-update" \
    "$broker_bin/update-usb" "$broker_bin/update-rollback" \
    "$broker_bin/fifi-install.sh" "$broker_bin/fifi-powerctl" \
    "$broker_bin/fifi-export-diagnostics"
FIFI_ADMIN_SOCKET="$broker_socket" \
FIFI_ADMIN_ALLOWED_UID="$(id -u)" FIFI_ADMIN_GID="$(id -g)" \
FIFI_SECCTL="$broker_bin/fifi-secctl" FIFI_TCPDUMP="$broker_bin/tcpdump" \
FIFI_WIFI_CTL="$broker_bin/fifi-wifi-ctl" \
FIFI_DIAGNOSTICS_EXPORT="$broker_bin/fifi-export-diagnostics" \
FIFI_UPDATE_APPLY="$broker_bin/fifi-apply-update" \
FIFI_UPDATE_USB="$broker_bin/update-usb" \
FIFI_UPDATE_ROLLBACK="$broker_bin/update-rollback" \
FIFI_INSTALL_APPLY="$broker_bin/fifi-install.sh" \
FIFI_INSTALL_ALLOWED=1 \
FIFI_POWERCTL="$broker_bin/fifi-powerctl" \
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
if FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" wifi scan fail0 >"$TMP/wifi-scan-failure.out" 2>&1; then
    echo "failed privileged Wi-Fi scan reported success" >&2
    exit 1
else
    test "$?" -eq 9
fi
grep -Fxq 'mock Wi-Fi scan failure' "$TMP/wifi-scan-failure.out"
! grep -Fq 'FIFI_ADMIN_STATUS' "$TMP/wifi-scan-failure.out"
printf '\000\010Cafe Net\000\013secret pass' |
    FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" wifi connect wlan0
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" wifi disconnect wlan0
diagnostics_out="$(FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" diagnostics export)"
grep -Fxq 'diagnostics export complete' <<<"$diagnostics_out"
grep -Fxq 'wifi scan wlan0' "$broker_log"
grep -Fxq 'wifi connect wlan0' "$broker_log"
grep -Fq '00 08 43 61 66 65 20 4e 65 74 00 0b 73 65 63 72' "$broker_log"
grep -Fq '65 74 20 70 61 73 73' "$broker_log"
grep -Fxq 'wifi disconnect wlan0' "$broker_log"
grep -Fxq 'diagnostics export' "$broker_log"
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" update apply stable
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" update rollback
grep -Fxq 'update apply stable' "$broker_log"
grep -Fxq 'update rollback' "$broker_log"
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" power reboot
FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" power poweroff
grep -Fxq 'power reboot' "$broker_log"
grep -Fxq 'power poweroff' "$broker_log"
bad_power="$(FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" power suspend 2>&1 || true)"
grep -Fq 'operation is not allowed' <<<"$bad_power"
if FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" update usb >"$TMP/update-usb.out" 2>&1; then
    echo "failed privileged USB action reported success" >&2
    exit 1
else
    test "$?" -eq 7
fi
grep -Fq 'mock USB failure' "$TMP/update-usb.out"
! grep -Fq 'FIFI_ADMIN_STATUS' "$TMP/update-usb.out"
denied_out="$(FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" shell root 2>&1)"
grep -Fq 'operation is not allowed' <<<"$denied_out"
bad_iface="$(FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" wifi scan 'wlan0;id' 2>&1 || true)"
grep -Fq 'operation is not allowed' <<<"$bad_iface"
bad_diagnostics="$(FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" diagnostics '../../etc' 2>&1 || true)"
grep -Fq 'operation is not allowed' <<<"$bad_diagnostics"
bad_channel="$(FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" update apply edge 2>&1 || true)"
grep -Fq 'operation is not allowed' <<<"$bad_channel"
bad_target="$(FIFI_ADMIN_SOCKET="$broker_socket" \
    "$TMP/fifi-admin" install apply /dev/null librewolf none none 2>&1 || true)"
grep -Fq 'operation is not allowed' <<<"$bad_target"
install_target="$(find /dev -maxdepth 1 -type b -print -quit 2>/dev/null || true)"
if [[ -n "$install_target" ]]; then
    FIFI_ADMIN_SOCKET="$broker_socket" "$TMP/fifi-admin" install apply \
        "$install_target" librewolf libreoffice llama3.2-1b
    grep -Fxq "install $install_target librewolf libreoffice llama3.2-1b" \
        "$broker_log"
    bad_model="$(FIFI_ADMIN_SOCKET="$broker_socket" \
        "$TMP/fifi-admin" install apply "$install_target" librewolf none \
        '../../root' 2>&1 || true)"
    grep -Fq 'operation is not allowed' <<<"$bad_model"
fi
grep -Fq 'strcmp(name, "fifi-security") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strcmp(name, "fifi-wifi") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strcmp(name, "fifi-settings") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strcmp(name, "fifi-appstore") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strcmp(name, "fifi-browser") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strcmp(name, "fifi-installer") == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'strncmp(path, app_library, sizeof(app_library) - 1) == 0' \
    "$ROOT/fifi/platform/linux/platform.c"
grep -Fq 'chown 1000:1000 /fifi-data/fifi-settings.conf' \
    "$ROOT/initramfs/root/init"
grep -Fq 'chown 0:1000 "$_audio_ctl"' "$ROOT/initramfs/root/init"
! grep -Fq 'fopen("/fifi-data/wifi-ssid", "w")' \
    "$ROOT/fifi/apps/settings/settings.c"
grep -Fq '#define APP_INSTALLER   "/usr/share/fifi/appstore-install.sh"' \
    "$ROOT/fifi/apps/appstore/appstore.c"
! grep -Fq '"/fifi-data/apps/appstore-install.sh"' \
    "$ROOT/fifi/apps/appstore/appstore.c"
! grep -Fq 'execl("/bin/sh", "sh", "-c"' \
    "$ROOT/fifi/apps/appstore/appstore.c"
grep -Fq 'exec /bin/fifi-user-exec "$0" "$@"' \
    "$ROOT/initramfs/root/bin/app-update"
grep -Fq 'FIFI_APP_HELPERS:-/usr/share/fifi' \
    "$ROOT/initramfs/root/bin/app-update"
grep -Fq 'chown -RhP 1000:1000 /fifi-data/apps' "$ROOT/initramfs/root/init"
grep -Fq '/bin/fifi-user-exec /bin/sh /usr/share/fifi/appstore-sync.sh' \
    "$ROOT/initramfs/root/init"
! grep -Fq 'cp "/usr/share/fifi/$f" "/fifi-data/apps/$f"' \
    "$ROOT/initramfs/root/init"

echo "[test-security] browser setup owns only private user storage"
browser_root="$TMP/browser-storage"
mkdir -p "$browser_root"
printf 'firefox\n' > "$browser_root/browser-choice"
FIFI_DATA_ROOT="$browser_root" FIFI_DESKTOP_UID="$(id -u)" \
FIFI_DESKTOP_GID="$(id -g)" \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-browser-storage.sh"
grep -Fxq firefox "$browser_root/browser/choice"
test "$(stat -c '%u:%g:%a' "$browser_root/browser")" = \
    "$(id -u):$(id -g):700"
browser_symlink_root="$TMP/browser-storage-symlink"
mkdir -p "$browser_symlink_root/browser"
printf 'protected\n' > "$browser_symlink_root/outside-choice"
printf 'firefox\n' > "$browser_symlink_root/browser-choice"
ln -s "$browser_symlink_root/outside-choice" \
    "$browser_symlink_root/browser/choice"
FIFI_DATA_ROOT="$browser_symlink_root" FIFI_DESKTOP_UID="$(id -u)" \
FIFI_DESKTOP_GID="$(id -g)" \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-browser-storage.sh"
grep -Fxq protected "$browser_symlink_root/outside-choice"
browser_dir_link_root="$TMP/browser-directory-symlink"
mkdir -p "$browser_dir_link_root" "$TMP/browser-outside"
ln -s "$TMP/browser-outside" "$browser_dir_link_root/browser"
if FIFI_DATA_ROOT="$browser_dir_link_root" \
   FIFI_DESKTOP_UID="$(id -u)" FIFI_DESKTOP_GID="$(id -g)" \
       sh "$ROOT/initramfs/root/usr/share/fifi/migrate-browser-storage.sh" \
           2>/dev/null; then
    echo "browser migration accepted a symlinked directory" >&2
    exit 1
fi
test ! -e "$browser_dir_link_root/.browser-owned-by-fifi"
grep -Fq '#define BROWSER_CHOICE   "/fifi-data/browser/choice"' \
    "$ROOT/fifi/apps/browser/browser.c"
grep -Fq '#define BROWSER_LOG      "/fifi-data/browser/launch.log"' \
    "$ROOT/fifi/apps/browser/browser.c"
grep -Fq '. "${FIFI_VERIFY_LIB:-/usr/share/fifi/verified-download.sh}"' \
    "$ROOT/initramfs/root/bin/fifi-download-browser.sh"

echo "[test-security] updater stages privately and crosses fixed broker verbs"
update_root="$TMP/update-storage"
mkdir -p "$update_root"
printf 'test\n' > "$update_root/update-channel"
FIFI_DATA_ROOT="$update_root" FIFI_DESKTOP_UID="$(id -u)" \
FIFI_DESKTOP_GID="$(id -g)" \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-update-storage.sh"
grep -Fxq test "$update_root/update/channel"
test "$(stat -c '%u:%g:%a' "$update_root/update")" = \
    "$(id -u):$(id -g):700"
test "$(stat -c '%u:%g:%a' "$update_root/update/staging")" = \
    "$(id -u):$(id -g):700"
update_link_root="$TMP/update-directory-symlink"
mkdir -p "$update_link_root" "$TMP/update-outside"
ln -s "$TMP/update-outside" "$update_link_root/update"
if FIFI_DATA_ROOT="$update_link_root" FIFI_DESKTOP_UID="$(id -u)" \
   FIFI_DESKTOP_GID="$(id -g)" \
       sh "$ROOT/initramfs/root/usr/share/fifi/migrate-update-storage.sh" \
           2>/dev/null; then
    echo "update migration accepted a symlinked directory" >&2
    exit 1
fi
test ! -e "$update_link_root/.update-owned-by-fifi"
grep -Fq 'exec "${FIFI_ADMIN_CLIENT:-fifi-admin}" update apply "$channel"' \
    "$ROOT/initramfs/root/bin/system-update"
grep -Fq 'exec "$ADMIN" update rollback' "$ROOT/initramfs/root/bin/update"
grep -Fq 'exec "$ADMIN" update usb' "$ROOT/initramfs/root/bin/update"
grep -Fq 'root broker required' "$ROOT/initramfs/root/bin/fifi-apply-update"
! grep -Fq 'fifi-boot-slots install' "$ROOT/initramfs/root/bin/system-update"

echo "[test-security] terminal power commands use only fixed broker verbs"
test -x "$ROOT/initramfs/root/bin/reboot"
test -x "$ROOT/initramfs/root/bin/poweroff"
grep -Fq 'fifi-admin}" power reboot' "$ROOT/initramfs/root/bin/reboot"
grep -Fq 'fifi-admin}" power poweroff' "$ROOT/initramfs/root/bin/poweroff"
grep -Fq 'busybox|bash|blkid|reboot|poweroff' \
    "$ROOT/scripts/build-initramfs.sh"
power_wrapper_log="$TMP/power-wrapper.log"
mkdir -p "$TMP/power-wrapper-bin"
cat > "$TMP/power-wrapper-bin/id" <<'EOF'
#!/bin/sh
test "${1:-}" = -u && echo 1000
EOF
cat > "$TMP/fifi-admin-client" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$FIFI_TEST_POWER_WRAPPER_LOG"
EOF
chmod +x "$TMP/power-wrapper-bin/id" "$TMP/fifi-admin-client"
PATH="$TMP/power-wrapper-bin:$PATH" \
FIFI_ADMIN_CLIENT="$TMP/fifi-admin-client" \
FIFI_TEST_POWER_WRAPPER_LOG="$power_wrapper_log" \
    "$ROOT/initramfs/root/bin/reboot"
PATH="$TMP/power-wrapper-bin:$PATH" \
FIFI_ADMIN_CLIENT="$TMP/fifi-admin-client" \
FIFI_TEST_POWER_WRAPPER_LOG="$power_wrapper_log" \
    "$ROOT/initramfs/root/bin/poweroff"
grep -Fxq 'power reboot' "$power_wrapper_log"
grep -Fxq 'power poweroff' "$power_wrapper_log"

echo "[test-security] installer UI uses only live-gated broker actions"
grep -Fq 'execl("/bin/fifi-admin","fifi-admin","install","apply",disk' \
    "$ROOT/fifi/apps/installer/installer.c"
grep -Fq 'execl("/bin/fifi-admin","fifi-admin","install","reboot",NULL)' \
    "$ROOT/fifi/apps/installer/installer.c"
! grep -Fq 'execl("/bin/fifi-install.sh"' \
    "$ROOT/fifi/apps/installer/installer.c"
grep -Fq 'install_allowed()' "$ROOT/fifi/platform/linux/fifi-admin.c"
grep -Fq 'S_ISBLK(st.st_mode)' "$ROOT/fifi/platform/linux/fifi-admin.c"
grep -Fq 'mktemp /run/fifi-install-debug.XXXXXX' \
    "$ROOT/initramfs/root/bin/fifi-install.sh"
! grep -Fq '/tmp/fifi-install-debug.log' \
    "$ROOT/initramfs/root/bin/fifi-install.sh"

echo "[test-security] app maintenance never executes writable helper copies"
app_library="$TMP/app-library"
app_helpers="$TMP/app-helpers"
app_log="$TMP/app-helper.log"
mkdir -p "$app_library" "$app_helpers"
cat > "$app_library/appstore-update-check.sh" <<'EOF'
#!/bin/sh
echo writable-helper-ran >&2
exit 99
EOF
cat > "$app_helpers/appstore-update-check.sh" <<'EOF'
#!/bin/sh
printf 'trusted-helper-ran\n' >> "$FIFI_TEST_APP_HELPER_LOG"
EOF
chmod +x "$app_library/appstore-update-check.sh" \
    "$app_helpers/appstore-update-check.sh"
FIFI_APPS_DIR="$app_library" FIFI_APP_HELPERS="$app_helpers" \
FIFI_TEST_APP_HELPER_LOG="$app_log" \
    sh "$ROOT/initramfs/root/bin/app-update" -c
grep -Fxq trusted-helper-ran "$app_log"

printf '%s' \
    'url:https://appimages.libreitalia.org/LibreOffice-fresh.standard-x86_64.AppImage' \
    > "$app_library/LibreOffice.src"
FIFI_APPS_DIR="$app_library" \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-app-library.sh"
grep -Fxq 'ivan-hc/LibreOffice-appimage' "$app_library/LibreOffice.src"
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

echo "[test-security] diagnostics export uses only removable verified FiFi media"
grep -Fxq 'exec /bin/fifi-admin diagnostics export' \
    "$ROOT/initramfs/root/bin/save-logs"
DIAGNOSTICS_EXPORT="$ROOT/initramfs/root/bin/fifi-export-diagnostics"
diagnostics_denied="$("$DIAGNOSTICS_EXPORT" 2>&1 || true)"
grep -Fq 'root broker required' <<<"$diagnostics_denied"
grep -Fq '[ "$(id -u)" = 0 ] || fail "root broker required"' \
    "$DIAGNOSTICS_EXPORT"
grep -Fq 'cat "$parent_path/removable"' "$DIAGNOSTICS_EXPORT"
grep -Fq '[ -f "$USB_MNT/.fifi-live-usb" ]' "$DIAGNOSTICS_EXPORT"
grep -Fq '[ -s "$USB_MNT/boot/initramfs.cpio.gz" ]' "$DIAGNOSTICS_EXPORT"
grep -Fq '/bin/fifi-wifi-ctl scan "$interface"' "$DIAGNOSTICS_EXPORT"
! grep -Fq '/fifi-data/wpa.conf' "$DIAGNOSTICS_EXPORT"
! grep -Fq '/fifi-data/wifi.conf' "$DIAGNOSTICS_EXPORT"

echo "[test-security] PASS"
