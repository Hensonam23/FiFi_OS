#!/usr/bin/env bash
# Run FiFi OS linux-desktop in QEMU.
# Uses direct kernel boot (-kernel/-initrd) — no ISO needed for development.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BZIMAGE="$REPO_ROOT/build-linux/bzImage"
INITRAMFS="$REPO_ROOT/build-linux/initramfs.cpio.gz"
DISK="$REPO_ROOT/build/disk.img"

if [ ! -f "$BZIMAGE" ]; then
    echo "[run] No kernel found at $BZIMAGE"
    echo "[run] Build it first: make linux-kernel"
    exit 1
fi

if [ ! -f "$INITRAMFS" ]; then
    echo "[run] No initramfs found at $INITRAMFS"
    echo "[run] Build it first: make linux-initrd"
    exit 1
fi

DISK_ARG=""
if [ -f "$DISK" ]; then
    DISK_ARG="-drive file=$DISK,format=raw,if=virtio"
fi

MODE="${1:-gui}"   # gui | serial

QEMU_BASE=(
    qemu-system-x86_64
    -M q35
    -m 512M
    -smp 2
    -kernel "$BZIMAGE"
    -initrd "$INITRAMFS"
    -no-reboot
    -no-shutdown
    $DISK_ARG
    -netdev user,id=net0
    -device virtio-net-pci,netdev=net0
    -audiodev pa,id=snd0
    -device intel-hda,id=hda0
    -device hda-output,audiodev=snd0
)

if [ "$MODE" = "serial" ]; then
    # Serial mode: kernel console on stdout, no graphical window
    "${QEMU_BASE[@]}" \
        -append "console=ttyS0,115200 quiet loglevel=3" \
        -vga none \
        -nographic \
        -serial stdio
else
    # GUI mode: framebuffer window + serial log to file
    "${QEMU_BASE[@]}" \
        -append "console=tty0 console=ttyS0,115200 quiet loglevel=3" \
        -device virtio-vga \
        -serial file:"$REPO_ROOT/serial-linux.log"
fi
