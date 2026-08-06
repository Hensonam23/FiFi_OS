#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

DATA="$TMP/data"
ETC="$TMP/etc"
FIXTURES="$TMP/fixtures"
MOCK_BIN="$TMP/bin"
mkdir -p "$DATA/boot" "$ETC" "$FIXTURES" "$MOCK_BIN"
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
    *api.github.com*) src="$FIFI_TEST_FIXTURES/api.json" ;;
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
if [ "${FIFI_TEST_USB_PRESENT:-0}" = 1 ]; then
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

echo "[test-update] verified one-time bootstrap installation"
"$ROOT/initramfs/root/bin/system-update" -y --channel test
cmp "$FIXTURES/bzImage" "$DATA/boot/bzImage"
cmp "$FIXTURES/initramfs.cpio.gz" "$DATA/boot/initramfs.cpio.gz"
grep -Fxq build-001 "$DATA/os-build-id"
grep -Fxq build-002 "$DATA/os-build-id.pending"
grep -Fxq test "$DATA/update-channel"
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

echo "[test-update] plain update prefers a recognized USB"
FIFI_TEST_USB_PRESENT=1 update
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
grep -Fq 'load_env -f \$fifi_grubenv fifi_attempted' \
    "$ROOT/initramfs/root/bin/fifi-install.sh"
grep -Fq 'save_env -f \$fifi_grubenv fifi_attempted' \
    "$ROOT/initramfs/root/bin/fifi-install.sh"
grep -Fq 'fifi_boot_fallback=\$fifi_entry_fallback' \
    "$ROOT/initramfs/root/bin/fifi-install.sh"
grep -Fq 'fifi_boot_fallback=1 apparmor=1' \
    "$ROOT/initramfs/root/bin/fifi-install.sh"

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
cmp "$ROOT/initramfs/root/bin/system-update" \
    "$PACKAGE_BUILD/update-test/fifi-bootstrap-update"
test -x "$PACKAGE_BUILD/update-test/fifi-bootstrap-update"
openssl pkeyutl -verify -pubin \
    -inkey "$ETC/fifi-release-signing.pub" -rawin \
    -in "$PACKAGE_BUILD/update-test/fifi-update.manifest" \
    -sigfile "$PACKAGE_BUILD/update-test/fifi-update.manifest.sig" >/dev/null

echo "[test-update] PASS"
