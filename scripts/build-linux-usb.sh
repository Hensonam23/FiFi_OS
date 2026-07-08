#!/usr/bin/env bash
# Build a bootable USB ISO for FiFi OS linux-desktop.
# Uses grub-mkrescue to produce a hybrid BIOS+UEFI image that can be dd'd to USB.
#
# NOTE: Secure Boot must be DISABLED in BIOS/UEFI to boot from this USB.
#       (BIOS > Security > Secure Boot > Disabled)
#
# Flash the resulting ISO:
#   sudo dd if=build-linux/fifi-linux.iso of=/dev/sdX bs=4M status=progress oflag=sync
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BZIMAGE="$REPO_ROOT/build-linux/bzImage"
INITRAMFS="$REPO_ROOT/build-linux/initramfs.cpio.gz"
OUT_ISO="$REPO_ROOT/build-linux/fifi-linux.iso"

if [ ! -f "$BZIMAGE" ]; then
    echo "[usb] ERROR: No kernel found at $BZIMAGE"
    echo "[usb] Run: make linux-kernel"
    exit 1
fi
if [ ! -f "$INITRAMFS" ]; then
    echo "[usb] ERROR: No initramfs found at $INITRAMFS"
    echo "[usb] Run: make linux-initrd"
    exit 1
fi

KZ=$(du -sh "$BZIMAGE"   | cut -f1)
IZ=$(du -sh "$INITRAMFS" | cut -f1)
echo "[usb] kernel: $KZ   initramfs: $IZ"
echo "[usb] Building bootable ISO..."

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/boot/grub"
cp "$BZIMAGE"   "$STAGE/boot/bzImage"
cp "$INITRAMFS" "$STAGE/boot/initramfs.cpio.gz"

# Offline app bundle (optional): lets the guided installer set up the browser
# and office suite with no internet. Populate with scripts/fetch-app-bundle.sh.
BUNDLE="$REPO_ROOT/build-linux/apps-bundle"
if ls "$BUNDLE"/*.AppImage >/dev/null 2>&1; then
    echo "[usb] bundling offline apps: $(cd "$BUNDLE" && ls *.AppImage | tr '\n' ' ')"
    mkdir -p "$STAGE/apps-bundle"
    cp "$BUNDLE"/*.AppImage "$STAGE/apps-bundle/"
else
    echo "[usb] NOTE: no offline app bundle — run scripts/fetch-app-bundle.sh first"
    echo "[usb]       to include browser/office AppImages for offline installs"
fi

cat > "$STAGE/boot/grub/grub.cfg" << 'GRUBCFG'
set timeout=8
set default=0

menuentry "FiFi OS linux-desktop" {
    linux /boot/bzImage console=tty0 console=ttyS0,115200 quiet loglevel=3
    initrd /boot/initramfs.cpio.gz
}

menuentry "FiFi OS linux-desktop  [nomodeset — safe fallback]" {
    linux /boot/bzImage console=tty0 quiet loglevel=3 nomodeset
    initrd /boot/initramfs.cpio.gz
}

menuentry "FiFi OS linux-desktop  [serial debug — no GUI]" {
    linux /boot/bzImage console=ttyS0,115200 loglevel=7
    initrd /boot/initramfs.cpio.gz
}
GRUBCFG

grub-mkrescue \
    --modules="part_gpt part_msdos fat ext2 linux normal configfile search" \
    -o "$OUT_ISO" \
    "$STAGE" \
    2>/dev/null

ISO_SIZE=$(du -sh "$OUT_ISO" | cut -f1)
echo "[usb] Done: $OUT_ISO ($ISO_SIZE)"
echo ""
echo "  !! Secure Boot must be DISABLED in BIOS before booting this USB !!"
echo ""
echo "  Flash:  sudo dd if=$OUT_ISO of=/dev/sdX bs=4M status=progress oflag=sync"
echo ""
echo "  Boot order for testing:"
echo "    1. Try the default entry first (DRM auto-detect)"
echo "    2. If screen goes blank/black, reboot and pick [nomodeset]"
echo "       (forces EFI framebuffer — avoids NVIDIA/AMD KMS conflicts)"
