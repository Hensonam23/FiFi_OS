#!/usr/bin/env bash
# Package existing Linux desktop boot artifacts for a stable/test update release.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHANNEL="${1:-test}"
BUILD_DIR="${FIFI_BUILD_DIR:-$ROOT/build-linux}"
OUT="${2:-$BUILD_DIR/update-$CHANNEL}"

case "$CHANNEL" in
    stable|test) ;;
    *)
        echo "usage: $0 [stable|test] [output-directory]" >&2
        exit 2
        ;;
esac

KERNEL="$BUILD_DIR/bzImage"
INITRAMFS="$BUILD_DIR/initramfs.cpio.gz"
for artifact in "$KERNEL" "$INITRAMFS"; do
    [[ -s "$artifact" ]] || {
        echo "[package-update] missing artifact: $artifact" >&2
        echo "Build first with: make linux-kernel linux-initrd" >&2
        exit 1
    }
done

BUILD_ID="${FIFI_BUILD_ID:-$(git -C "$ROOT" rev-parse HEAD)}"
[[ "$BUILD_ID" =~ ^[A-Za-z0-9._-]+$ ]] || {
    echo "[package-update] invalid build identifier: $BUILD_ID" >&2
    exit 1
}

mkdir -p "$OUT"
cp "$KERNEL" "$OUT/bzImage"
cp "$INITRAMFS" "$OUT/initramfs.cpio.gz"
# Older installed FiFi images have a USB-only `update` command. Publish the new
# verified updater as a small standalone bootstrap so those systems can make
# their first online transition without downloading or flashing an ISO.
cp "$ROOT/initramfs/root/bin/system-update" "$OUT/fifi-bootstrap-update"
chmod +x "$OUT/fifi-bootstrap-update"

kernel_sha="$(sha256sum "$OUT/bzImage" | awk '{print $1}')"
initramfs_sha="$(sha256sum "$OUT/initramfs.cpio.gz" | awk '{print $1}')"

{
    printf 'format=1\n'
    printf 'channel=%s\n' "$CHANNEL"
    printf 'build=%s\n' "$BUILD_ID"
    printf 'kernel_sha256=%s\n' "$kernel_sha"
    printf 'initramfs_sha256=%s\n' "$initramfs_sha"
} > "$OUT/fifi-update.manifest"

gzip -t "$OUT/initramfs.cpio.gz"

echo "[package-update] ready: $OUT"
echo "[package-update] build: $BUILD_ID"
echo "[package-update] channel: $CHANNEL"
