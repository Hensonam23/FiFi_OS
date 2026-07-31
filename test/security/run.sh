#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

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
