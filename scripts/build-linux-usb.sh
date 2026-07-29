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

# Unique marker so GRUB can pin $root to THIS USB, never the internal disk.
# On a machine that already has FiFi installed, both the USB and the internal
# disk have /boot/grub/grub.cfg and /boot/bzImage, so a plain relative-path
# boot can silently load the INTERNAL (old) kernel+initramfs even though the
# user picked the USB. The `search --file` below forces $root to the device
# that actually holds this marker — the USB — so the live view always reflects
# what was flashed.
FIFI_USB_ID="FIFI-USB-$(date -u +%Y%m%d%H%M%S 2>/dev/null || echo build)"
printf '%s\n' "$FIFI_USB_ID" > "$STAGE/fifi-usb-boot.id"
# The updater present on older installed images identifies update media by this
# marker. Keep it alongside the unique GRUB marker so a dd-flashed ISO can
# perform the one-time in-place transition to the new online updater.
touch "$STAGE/.fifi-live-usb"

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

# Pin $root to THIS USB (the device holding the unique marker), so every entry
# below loads the USB's own kernel + initramfs — never a stale copy on an
# internal disk that also has FiFi installed.
search --no-floppy --set=root --file /fifi-usb-boot.id

# Try without installing: boots the full FiFi desktop straight off the USB.
# The `fifi_live` flag tells init NOT to adopt an already-installed /fifi-data
# on disk (and to skip switch_root), so nothing on the machine is touched.
menuentry "Try FiFi OS (Live)" {
    linux /boot/bzImage console=tty0 console=ttyS0,115200 quiet loglevel=3 fifi_live
    initrd /boot/initramfs.cpio.gz
}

# Install to disk: identical live boot — then double-click the "Install FiFi OS"
# desktop icon to run the guided installer. Still `fifi_live` so the USB never
# mounts an existing install while you are setting one up.
menuentry "Install FiFi OS" {
    linux /boot/bzImage console=tty0 console=ttyS0,115200 quiet loglevel=3 fifi_live
    initrd /boot/initramfs.cpio.gz
}

# Safe graphics fallback: forces the EFI framebuffer (nomodeset) for machines
# whose NVIDIA/AMD KMS driver blanks the screen on the entries above.
menuentry "Try FiFi OS (Live)  [nomodeset — safe graphics]" {
    linux /boot/bzImage console=tty0 quiet loglevel=3 nomodeset fifi_live
    initrd /boot/initramfs.cpio.gz
}

# Serial debug: verbose kernel log to ttyS0, no GUI.
menuentry "FiFi OS  [serial debug — no GUI]" {
    linux /boot/bzImage console=ttyS0,115200 loglevel=7 fifi_live
    initrd /boot/initramfs.cpio.gz
}
GRUBCFG

grub-mkrescue \
    --modules="part_gpt part_msdos fat ext2 iso9660 linux normal configfile search search_fs_file" \
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
