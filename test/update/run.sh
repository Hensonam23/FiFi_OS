#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
grep -Fq 'Download and install this OS update now? [Y/N]' \
    "$ROOT/initramfs/root/bin/system-update"
! grep -Fq 'Download and install this OS update now? [y/N]' \
    "$ROOT/initramfs/root/bin/system-update"
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

DATA="$TMP/data"
ETC="$TMP/etc"
FIXTURES="$TMP/fixtures"
MOCK_BIN="$TMP/bin"
mkdir -p "$DATA/boot" "$DATA/update/staging" "$ETC" "$FIXTURES" "$MOCK_BIN"
openssl genpkey -algorithm ED25519 -out "$FIXTURES/test-signing-key.pem"
openssl pkey -in "$FIXTURES/test-signing-key.pem" -pubout \
    -out "$ETC/fifi-release-signing.pub"

printf 'old-kernel\n' > "$DATA/boot/bzImage"
printf 'old-initramfs\n' | gzip -c > "$DATA/boot/initramfs.cpio.gz"
printf 'build-001\n' > "$DATA/os-build-id"
printf 'stable\n' > "$ETC/fifi-update-channel"
printf 'FiFi OS updater test\n' > "$ETC/fifi-version"

printf 'new-kernel\n' > "$FIXTURES/bzImage"
printf 'new-initramfs\n' | gzip -c > "$FIXTURES/initramfs.cpio.gz"
kernel_sha="$(sha256sum "$FIXTURES/bzImage" | awk '{print $1}')"
initramfs_sha="$(sha256sum "$FIXTURES/initramfs.cpio.gz" | awk '{print $1}')"

write_manifest() {
    local build="$1"
    local ksha="$2"
    {
        printf 'format=1\n'
        printf 'channel=test\n'
        printf 'build=%s\n' "$build"
        printf 'kernel_sha256=%s\n' "$ksha"
        printf 'initramfs_sha256=%s\n' "$initramfs_sha"
    } > "$FIXTURES/fifi-update.manifest"
    openssl pkeyutl -sign -rawin \
        -inkey "$FIXTURES/test-signing-key.pem" \
        -in "$FIXTURES/fifi-update.manifest" \
        -out "$FIXTURES/fifi-update.manifest.sig"
}

write_api() {
    cat > "$FIXTURES/api.json" <<'EOF'
{
  "tag_name": "linux-desktop-test",
  "assets": [
    {"browser_download_url": "https://fixtures/fifi-update.manifest"},
    {"browser_download_url": "https://fixtures/fifi-update.manifest.sig"},
    {"browser_download_url": "https://fixtures/bzImage"},
    {"browser_download_url": "https://fixtures/initramfs.cpio.gz"}
  ]
}
EOF
}

cat > "$MOCK_BIN/curl" <<'EOF'
#!/bin/sh
out=""
url=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o)
            shift
            out="$1"
            ;;
        --max-time)
            shift
            ;;
        -*)
            ;;
        *)
            url="$1"
            ;;
    esac
    shift
done
case "$url" in
    *api.github.com*)
        if [ "${FIFI_TEST_API_MISSING:-0}" = 1 ]; then
            printf '{"message":"Not Found"}\n'
            exit 22
        fi
        src="$FIFI_TEST_FIXTURES/api.json"
        ;;
    */fifi-update.manifest) src="$FIFI_TEST_FIXTURES/fifi-update.manifest" ;;
    */fifi-update.manifest.sig) src="$FIFI_TEST_FIXTURES/fifi-update.manifest.sig" ;;
    */bzImage) src="$FIFI_TEST_FIXTURES/bzImage" ;;
    */initramfs.cpio.gz) src="$FIFI_TEST_FIXTURES/initramfs.cpio.gz" ;;
    *) echo "mock curl: unexpected URL: $url" >&2; exit 22 ;;
esac
if [ -n "$out" ]; then
    cp "$src" "$out"
else
    cat "$src"
fi
EOF
chmod +x "$MOCK_BIN/curl"

cat > "$MOCK_BIN/update-usb" <<'EOF'
#!/bin/sh
if [ -e "$FIFI_TEST_USB_PRESENT_FILE" ]; then
    printf 'usb\n' >> "$FIFI_TEST_USB_LOG"
    exit 0
fi
exit 2
EOF
chmod +x "$MOCK_BIN/update-usb"

write_manifest build-002 "$kernel_sha"
write_api

export PATH="$MOCK_BIN:$ROOT/initramfs/root/bin:$PATH"
export FIFI_TEST_FIXTURES="$FIXTURES"
export FIFI_DATA_ROOT="$DATA"
export FIFI_ETC_ROOT="$ETC"
export FIFI_SKIP_APP_UPDATE=1
export FIFI_TEST_USB_LOG="$TMP/usb-dispatch.log"
export FIFI_TEST_USB_PRESENT_FILE="$TMP/usb-present"
export FIFI_UPDATE_STAGING="$DATA/update/staging"
export FIFI_ALLOW_UNPRIVILEGED_TEST=1
export FIFI_BOOT_SLOTS="$ROOT/initramfs/root/bin/fifi-boot-slots"

gcc -std=c11 -O2 -Wall -Wextra \
    "$ROOT/fifi/platform/linux/fifi-admin.c" \
    -o "$MOCK_BIN/fifi-admin"
export FIFI_ADMIN_SOCKET="$TMP/fifi-admin.sock"
FIFI_ADMIN_ALLOWED_UID="$(id -u)" FIFI_ADMIN_GID="$(id -g)" \
FIFI_UPDATE_APPLY="$ROOT/initramfs/root/bin/fifi-apply-update" \
FIFI_UPDATE_USB="$MOCK_BIN/update-usb" \
FIFI_UPDATE_ROLLBACK="$ROOT/initramfs/root/bin/update-rollback" \
    "$MOCK_BIN/fifi-admin" --daemon &
broker_pid=$!
for _ in $(seq 1 50); do
    [[ -S "$FIFI_ADMIN_SOCKET" ]] && break
    sleep 0.05
done
[[ -S "$FIFI_ADMIN_SOCKET" ]]

echo "[test-update] missing release is distinct from a network failure"
export FIFI_TEST_API_MISSING=1
if "$ROOT/initramfs/root/bin/system-update" -c --channel test \
    >"$TMP/missing-release.out" 2>&1; then
    echo "missing test release unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq 'test channel has no published update yet' "$TMP/missing-release.out"
unset FIFI_TEST_API_MISSING

echo "[test-update] verified one-time bootstrap installation"
"$ROOT/initramfs/root/bin/system-update" -y --channel test
cmp "$FIXTURES/bzImage" "$DATA/boot/bzImage"
cmp "$FIXTURES/initramfs.cpio.gz" "$DATA/boot/initramfs.cpio.gz"
grep -Fxq build-001 "$DATA/os-build-id"
grep -Fxq build-002 "$DATA/os-build-id.pending"
grep -Fxq test "$DATA/update/channel"
grep -Fxq old-kernel "$DATA/boot/bzImage.prev"
grep -Fxq 'set fifi_active=B' "$DATA/boot/fifi-slot.cfg"
grep -Fxq 'set fifi_previous=A' "$DATA/boot/fifi-slot.cfg"
grep -Fxq 'set fifi_pending=1' "$DATA/boot/fifi-slot.cfg"
test "$(readlink "$DATA/boot/bzImage")" = bzImage.B
test -e "$DATA/post-update.pending"

echo "[test-update] pending update cannot overwrite the known-good slot"
before_sha="$(sha256sum "$DATA/boot/bzImage" | awk '{print $1}')"
update -y
after_sha="$(sha256sum "$DATA/boot/bzImage" | awk '{print $1}')"
[[ "$before_sha" == "$after_sha" ]]

echo "[test-update] compositor-ready boot confirms the new slot"
FIFI_BOOT_SLOT=B FIFI_BOOT_FALLBACK=0 fifi-confirm-boot
grep -Fxq build-002 "$DATA/os-build-id"
grep -Fxq build-001 "$DATA/os-build-id.prev"
test ! -e "$DATA/os-build-id.pending"
grep -Fxq 'set fifi_active=B' "$DATA/boot/fifi-slot.cfg"
grep -Fxq 'set fifi_pending=0' "$DATA/boot/fifi-slot.cfg"

echo "[test-update] root broker re-verifies user-owned staging"
write_manifest build-003 "$kernel_sha"
cp "$FIXTURES/fifi-update.manifest" "$DATA/update/staging/fifi-update.manifest"
cp "$FIXTURES/fifi-update.manifest.sig" "$DATA/update/staging/fifi-update.manifest.sig"
cp "$FIXTURES/bzImage" "$DATA/update/staging/bzImage"
cp "$FIXTURES/initramfs.cpio.gz" "$DATA/update/staging/initramfs.cpio.gz"
printf 'linux-desktop-test\n' > "$DATA/update/staging/release-tag"
printf 'changed-after-download\n' >> "$DATA/update/staging/bzImage"
before_broker_sha="$(sha256sum "$DATA/boot/bzImage" | awk '{print $1}')"
if fifi-admin update apply test; then
    echo "broker accepted modified user staging" >&2
    exit 1
fi
[[ "$before_broker_sha" == "$(sha256sum "$DATA/boot/bzImage" | awk '{print $1}')" ]]
test ! -e "$DATA/os-build-id.pending"
rm -f "$DATA/update/staging/bzImage"
ln -s "$FIXTURES/bzImage" "$DATA/update/staging/bzImage"
if fifi-admin update apply test; then
    echo "broker accepted symlinked user staging" >&2
    exit 1
fi
test ! -e "$DATA/os-build-id.pending"

echo "[test-update] plain update prefers a recognized USB"
touch "$FIFI_TEST_USB_PRESENT_FILE"
update
rm -f "$FIFI_TEST_USB_PRESENT_FILE"
grep -Fxq usb "$FIFI_TEST_USB_LOG"

echo "[test-update] checksum failure preserves active files"
write_manifest build-003 \
    aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
if system-update -y --channel test; then
    echo "checksum mismatch unexpectedly succeeded" >&2
    exit 1
fi
cmp "$FIXTURES/bzImage" "$DATA/boot/bzImage"
grep -Fxq build-002 "$DATA/os-build-id"

echo "[test-update] forged manifest is rejected"
printf 'tampered\n' >> "$FIXTURES/fifi-update.manifest"
if system-update -y --channel test; then
    echo "forged manifest unexpectedly succeeded" >&2
    exit 1
fi
cmp "$FIXTURES/bzImage" "$DATA/boot/bzImage"
write_manifest build-003 "$kernel_sha"

echo "[test-update] rollback swaps complete boot pairs"
update rollback
grep -Fxq old-kernel "$DATA/boot/bzImage"
gzip -t "$DATA/boot/initramfs.cpio.gz"
grep -Fxq build-001 "$DATA/os-build-id"
grep -Fxq build-002 "$DATA/os-build-id.prev"
grep -Fxq 'set fifi_active=A' "$DATA/boot/fifi-slot.cfg"
grep -Fxq 'set fifi_pending=0' "$DATA/boot/fifi-slot.cfg"

echo "[test-update] failed pending boot falls back without trusting its build"
system-update -y --channel test
grep -Fxq build-003 "$DATA/os-build-id.pending"
FIFI_BOOT_SLOT=A FIFI_BOOT_FALLBACK=1 fifi-confirm-boot
grep -Fxq build-001 "$DATA/os-build-id"
test ! -e "$DATA/os-build-id.pending"
grep -Fxq 'set fifi_active=A' "$DATA/boot/fifi-slot.cfg"
grep -Fxq 'set fifi_pending=0' "$DATA/boot/fifi-slot.cfg"

echo "[test-update] rolling back before reboot preserves the running build ID"
system-update -y --channel test
update rollback
grep -Fxq build-001 "$DATA/os-build-id"
test ! -e "$DATA/os-build-id.pending"
grep -Fxq 'set fifi_active=A' "$DATA/boot/fifi-slot.cfg"

echo "[test-update] installer GRUB records attempts and selects fallback slots"
grep -Fq 'fifi-write-grub-config' "$ROOT/initramfs/root/bin/fifi-install.sh"
grep -Fq 'load_env -f \$fifi_grubenv fifi_attempted' \
    "$ROOT/initramfs/root/bin/fifi-write-grub-config"
grep -Fq 'save_env -f \$fifi_grubenv fifi_attempted' \
    "$ROOT/initramfs/root/bin/fifi-write-grub-config"
grep -Fq 'fifi_boot_fallback=\$fifi_entry_fallback' \
    "$ROOT/initramfs/root/bin/fifi-write-grub-config"
grep -Fq 'fifi_boot_fallback=1 apparmor=1' \
    "$ROOT/initramfs/root/bin/fifi-write-grub-config"

echo "[test-update] legacy installed GRUB gains automatic A/B fallback"
EFI="$TMP/efi"
mkdir -p "$EFI/boot/grub"
: > "$DATA/installed"
cat > "$EFI/boot/grub/grub.cfg" <<'EOF'
set timeout=5
search --no-floppy --fs-uuid --set=root 1234-ABCD
menuentry "FiFi OS" {
    linux /boot/bzImage fifi_data_uuid=1234-ABCD
    initrd /boot/initramfs.cpio.gz
}
menuentry "Windows Boot Manager" {
    chainloader /EFI/Microsoft/Boot/bootmgfw_backup.efi
}
EOF
cat > "$MOCK_BIN/grub-editenv" <<'EOF'
#!/bin/sh
[ "$2" = create ] || exit 2
: > "$1"
EOF
chmod +x "$MOCK_BIN/grub-editenv"
OTHER_EFI="$TMP/other-efi"
mkdir -p "$OTHER_EFI/boot/grub"
printf '%s\n' 'menuentry "Other OS" { linux /boot/vmlinuz fifi_data_uuid=FFFF-0000; }' \
    > "$OTHER_EFI/boot/grub/grub.cfg"
other_sha="$(sha256sum "$OTHER_EFI/boot/grub/grub.cfg" | awk '{print $1}')"
FIFI_EFI_ROOT="$OTHER_EFI" FIFI_DATA_UUID=1234-ABCD \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-legacy-grub.sh"
[[ "$other_sha" == "$(sha256sum "$OTHER_EFI/boot/grub/grub.cfg" | awk '{print $1}')" ]]
test ! -e "$DATA/.grub-ab-migrated"
FIFI_EFI_ROOT="$EFI" FIFI_DATA_UUID=1234-ABCD \
FIFI_GRUB_EDITENV="$MOCK_BIN/grub-editenv" \
FIFI_GRUB_CONFIG_WRITER="$ROOT/initramfs/root/bin/fifi-write-grub-config" \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-legacy-grub.sh"
test -e "$DATA/.grub-ab-migrated"
test -e "$EFI/boot/grub/grubenv"
grep -Fq 'linux /boot/bzImage fifi_data_uuid=1234-ABCD' \
    "$EFI/boot/grub/grub.cfg.before-ab-migration"
grep -Fq 'source /boot/fifi-slot.cfg' "$EFI/boot/grub/grub.cfg"
grep -Fq 'save_env -f $fifi_grubenv fifi_attempted' "$EFI/boot/grub/grub.cfg"
grep -Fq 'chainloader /EFI/Microsoft/Boot/bootmgfw_backup.efi' \
    "$EFI/boot/grub/grub.cfg"
before_migration_sha="$(sha256sum "$EFI/boot/grub/grub.cfg" | awk '{print $1}')"
FIFI_EFI_ROOT="$EFI" FIFI_DATA_UUID=1234-ABCD \
    sh "$ROOT/initramfs/root/usr/share/fifi/migrate-legacy-grub.sh"
after_migration_sha="$(sha256sum "$EFI/boot/grub/grub.cfg" | awk '{print $1}')"
[[ "$before_migration_sha" == "$after_migration_sha" ]]

echo "[test-update] release packaging"
PACKAGE_BUILD="$TMP/package-build"
mkdir -p "$PACKAGE_BUILD"
cp "$FIXTURES/bzImage" "$PACKAGE_BUILD/bzImage"
cp "$FIXTURES/initramfs.cpio.gz" "$PACKAGE_BUILD/initramfs.cpio.gz"
FIFI_BUILD_DIR="$PACKAGE_BUILD" FIFI_BUILD_ID=build-004 \
FIFI_RELEASE_SIGNING_KEY="$FIXTURES/test-signing-key.pem" \
FIFI_RELEASE_PUBLIC_KEY="$ETC/fifi-release-signing.pub" \
    bash "$ROOT/scripts/package-update.sh" test
grep -Fxq channel=test "$PACKAGE_BUILD/update-test/fifi-update.manifest"
grep -Fxq build=build-004 "$PACKAGE_BUILD/update-test/fifi-update.manifest"
cmp "$FIXTURES/bzImage" "$PACKAGE_BUILD/update-test/bzImage"
test ! -e "$PACKAGE_BUILD/update-test/fifi-bootstrap-update"
openssl pkeyutl -verify -pubin \
    -inkey "$ETC/fifi-release-signing.pub" -rawin \
    -in "$PACKAGE_BUILD/update-test/fifi-update.manifest" \
    -sigfile "$PACKAGE_BUILD/update-test/fifi-update.manifest.sig" >/dev/null

echo "[test-update] PASS"
