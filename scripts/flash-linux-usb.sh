#!/usr/bin/env bash
# Flash FiFi OS linux-desktop directly to a USB drive as a single EFI partition.
# This replaces the grub-mkrescue ISO approach which suffers from drive DRAM
# cache not committing to NAND on unplug.
#
# Usage:  sudo bash scripts/flash-linux-usb.sh [/dev/sdX]
# Default device: /dev/sdd
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BZIMAGE="$REPO_ROOT/build-linux/bzImage"
INITRAMFS="$REPO_ROOT/build-linux/initramfs.cpio.gz"
DEV="${1:-/dev/sdd}"

if [ "$EUID" -ne 0 ]; then
    echo "[flash] ERROR: must run as root (sudo)"
    exit 1
fi
if [ ! -b "$DEV" ]; then
    echo "[flash] ERROR: $DEV is not a block device"
    exit 1
fi
DEVSIZE=$(blockdev --getsize64 "$DEV")
if [ "$DEVSIZE" -lt $((256*1024*1024)) ] || [ "$DEVSIZE" -gt $((64*1024*1024*1024)) ]; then
    echo "[flash] ERROR: $DEV size looks wrong — aborting"
    exit 1
fi
if [ ! -f "$BZIMAGE" ] || [ ! -f "$INITRAMFS" ]; then
    echo "[flash] ERROR: missing build outputs — run: make linux-usb"
    exit 1
fi

echo "[flash] Device:  $DEV  ($(( DEVSIZE / 1024 / 1024 )) MB)"
echo "[flash] Kernel:  $(du -sh "$BZIMAGE" | cut -f1)"
echo "[flash] Initrd:  $(du -sh "$INITRAMFS" | cut -f1)"

# Unmount any mounted partitions on this device
for p in "${DEV}"?*; do
    umount "$p" 2>/dev/null || true
done

echo "[flash] Partitioning (single GPT EFI partition)..."
parted -s "$DEV" mklabel gpt
parted -s "$DEV" mkpart "FiFiOS" fat32 1MiB 100%
parted -s "$DEV" set 1 esp on

PART="${DEV}1"
partprobe "$DEV" 2>/dev/null || true
sleep 2

echo "[flash] Formatting FAT32..."
mkfs.fat -F32 -n "FIFIOS" "$PART"

MNT=$(mktemp -d)
cleanup() { umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT

mount "$PART" "$MNT"

echo "[flash] Installing GRUB EFI..."
grub-install \
    --target=x86_64-efi \
    --efi-directory="$MNT" \
    --boot-directory="$MNT/boot" \
    --removable \
    --no-nvram \
    2>/dev/null

mkdir -p "$MNT/boot/grub"
cp "$BZIMAGE"   "$MNT/boot/bzImage"
cp "$INITRAMFS" "$MNT/boot/initramfs.cpio.gz"

cat > "$MNT/boot/grub/grub.cfg" << 'GRUBCFG'
set timeout=8
set default=0

menuentry "FiFi OS linux-desktop" {
    linux /boot/bzImage console=tty0 console=ttyS0,115200 quiet loglevel=3 apparmor=1 security=apparmor
    initrd /boot/initramfs.cpio.gz
}

menuentry "FiFi OS linux-desktop  [nomodeset]" {
    linux /boot/bzImage console=tty0 quiet loglevel=3 nomodeset apparmor=1 security=apparmor
    initrd /boot/initramfs.cpio.gz
}
GRUBCFG

echo "[flash] Signing for Secure Boot..."
if command -v sbctl >/dev/null 2>&1 && sbctl status 2>/dev/null | grep -q -i "installed\|guid\|keys"; then
    SIGNED=0
    # Sign GRUB EFI binary — this is what the firmware validates
    for efi in "$MNT/EFI/BOOT/BOOTX64.EFI" "$MNT/EFI/BOOT/grubx64.efi"; do
        [ -f "$efi" ] || continue
        if sbctl sign "$efi" 2>/dev/null; then
            echo "[flash] Signed $(basename $efi)"
            SIGNED=$((SIGNED+1))
        fi
    done
    # Sign the kernel too (GRUB can verify it when Secure Boot is enforced)
    if sbctl sign "$MNT/boot/bzImage" 2>/dev/null; then
        echo "[flash] Signed bzImage"
        SIGNED=$((SIGNED+1))
    fi
    if [ "$SIGNED" -gt 0 ]; then
        echo "[flash] Secure Boot signing complete ($SIGNED files signed)"
    else
        echo "[flash] WARNING: sbctl found but signing failed -- check sbctl status"
    fi
    # Export the signing cert to the USB root so it can be enrolled in another machine's BIOS
    CERT_SRC="/var/lib/sbctl/keys/db/db.pem"
    CERT_DST="$MNT/fifi-secureboot-key.cer"
    if [ -f "$CERT_SRC" ] && openssl x509 -in "$CERT_SRC" -outform DER -out "$CERT_DST" 2>/dev/null; then
        echo "[flash] Cert exported to USB root: fifi-secureboot-key.cer"
        echo "[flash]   To boot on another machine with Secure Boot: enroll this cert"
        echo "[flash]   in that machine's BIOS under Secure Boot > DB / Authorized Signatures"
    fi
else
    echo "[flash] Skipping Secure Boot signing (sbctl not set up or keys not enrolled)"
    echo "[flash]   Run: sudo sbctl create-keys && sudo sbctl enroll-keys --microsoft"
    echo "[flash]   Then re-flash to enable Secure Boot booting"
fi

touch "$MNT/.fifi-live-usb"

echo "[flash] Bundling Node.js v22 on USB (enables offline setup)..."
NODE_URL=$(curl -sf "https://nodejs.org/dist/latest-v22.x/" | grep -oE "node-v[0-9.]+-linux-x64\.tar\.xz" | head -1)
if [ -n "$NODE_URL" ]; then
    curl -fL "https://nodejs.org/dist/latest-v22.x/$NODE_URL" -o "$MNT/$NODE_URL" 2>/dev/null && \
        echo "[flash] Node.js bundled: $NODE_URL" || \
        echo "[flash] WARNING: Node.js download failed -- setup will download at runtime"
else
    echo "[flash] WARNING: Could not find Node.js URL -- setup will download at runtime"
fi

echo "[flash] Syncing to device..."
sync
umount "$MNT"
rmdir "$MNT"
trap - EXIT
sync
sleep 3

echo "[flash] Verifying..."
mkdir -p /tmp/fifi-flash-verify
mount "$PART" /tmp/fifi-flash-verify
INITRD_ON_DEV=$(md5sum /tmp/fifi-flash-verify/boot/initramfs.cpio.gz | cut -d' ' -f1)
BUILD_INITRD=$(md5sum "$INITRAMFS" | cut -d' ' -f1)
# Write devtool API key to USB if available (survives flash for fifi-dev-setup.sh)
DEVTOOL_JSON="${SUDO_HOME:-$HOME}/.devtool.json"
[ -f "$DEVTOOL_JSON" ] || DEVTOOL_JSON="/home/aaron/.devtool.json"
if [ -f "$DEVTOOL_JSON" ]; then
    API_KEY=$(python3 -c "import json; print(json.load(open('$DEVTOOL_JSON')).get('primaryApiKey',''))" 2>/dev/null || true)
    if [ -n "$API_KEY" ]; then
        echo "$API_KEY" > /tmp/fifi-flash-verify/devtool-api-key.txt
        echo "[flash] the assistant API key written to USB"
    fi
fi
umount /tmp/fifi-flash-verify
rmdir /tmp/fifi-flash-verify

if [ "$INITRD_ON_DEV" = "$BUILD_INITRD" ]; then
    echo "[flash] initramfs verified on device"
else
    echo "[flash] ERROR: initramfs mismatch after verify"
    echo "  device: $INITRD_ON_DEV"
    echo "  build:  $BUILD_INITRD"
    exit 1
fi

echo "[flash] Ejecting safely..."
udisksctl power-off -b "$DEV" 2>/dev/null || eject "$DEV" 2>/dev/null || true
echo "[flash] Done. Safe to unplug."
