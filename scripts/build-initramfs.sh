#!/usr/bin/env bash
# Build the FiFi OS initramfs.
# Packages initramfs/root/ + busybox into a cpio.gz the kernel can boot.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROOT_DIR="$REPO_ROOT/initramfs/root"
OUT_DIR="$REPO_ROOT/build-linux"
OUT_FILE="$OUT_DIR/initramfs.cpio.gz"

mkdir -p "$OUT_DIR"

# ── Busybox: provides /bin/sh, mount, ls, etc. ────────────────────────────────
BUSYBOX_BIN=""
for candidate in /usr/bin/busybox /bin/busybox; do
    if [ -x "$candidate" ]; then
        BUSYBOX_BIN="$candidate"
        break
    fi
done

if [ -z "$BUSYBOX_BIN" ]; then
    echo "[initramfs] busybox not found. Install it:"
    echo "  sudo pacman -S busybox"
    exit 1
fi

# ── Stage the root tree ───────────────────────────────────────────────────────
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

cp -a "$ROOT_DIR/." "$STAGE/"

# Install busybox and create symlinks for all applets
mkdir -p "$STAGE/bin" "$STAGE/sbin" "$STAGE/usr/bin" "$STAGE/usr/sbin"
cp "$BUSYBOX_BIN" "$STAGE/bin/busybox"
chmod +x "$STAGE/bin/busybox"

# Populate symlinks: sh, mount, ls, cat, echo, etc.
for applet in sh ash mount umount ls cat echo cp mv rm mkdir mknod \
              ln chmod chown hostname dmesg free ps kill sleep \
              modprobe insmod lsmod ifconfig ip udhcpc; do
    ln -sf busybox "$STAGE/bin/$applet" 2>/dev/null || true
done

# Copy ush if it was compiled for linux target (Phase 2+)
USH_BIN="$REPO_ROOT/build-linux/ush"
if [ -x "$USH_BIN" ]; then
    cp "$USH_BIN" "$STAGE/bin/ush"
    echo "[initramfs] included ush shell"
fi

# Ensure /init is executable
chmod +x "$STAGE/init"

# Minimal /dev nodes (devtmpfs fills the rest at runtime)
mkdir -p "$STAGE/dev"
mknod -m 600 "$STAGE/dev/console" c 5 1 2>/dev/null || true
mknod -m 666 "$STAGE/dev/null"    c 1 3 2>/dev/null || true
mknod -m 666 "$STAGE/dev/tty"     c 5 0 2>/dev/null || true

# ── Pack into cpio.gz ─────────────────────────────────────────────────────────
(cd "$STAGE" && find . | cpio -H newc -o 2>/dev/null | gzip -9) > "$OUT_FILE"

SIZE=$(du -sh "$OUT_FILE" | cut -f1)
echo "[initramfs] Done. $OUT_FILE ($SIZE)"
