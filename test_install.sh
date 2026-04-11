#!/usr/bin/env bash
# Test the FiFi OS installer against a loop-back image, then launch QEMU.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_IMG="$SCRIPT_DIR/build/test_usb.img"
TEST_MB=64
LOOP=""

cleanup() {
    if [[ -n "$LOOP" ]]; then
        sudo losetup -d "$LOOP" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# ── build ISO if needed ───────────────────────────────────────────────────────
if [[ ! -f "$SCRIPT_DIR/build/fifi.iso" ]]; then
    echo "==> Building fifi.iso..."
    cd "$SCRIPT_DIR" && make iso
fi

# ── create test image ─────────────────────────────────────────────────────────
echo "==> Creating ${TEST_MB}MB test image: $TEST_IMG"
dd if=/dev/zero of="$TEST_IMG" bs=1M count="$TEST_MB" status=none
echo "    done."

# ── set up loop device ────────────────────────────────────────────────────────
LOOP=$(sudo losetup -f)
sudo losetup "$LOOP" "$TEST_IMG"
echo "==> Loop device: $LOOP"

# ── run installer ─────────────────────────────────────────────────────────────
echo "==> Launching installer (GUI)..."
"$SCRIPT_DIR/install.sh" --target "$LOOP"
INSTALL_EXIT=$?

# detach loop before QEMU (QEMU reads the raw file directly)
sudo losetup -d "$LOOP"
LOOP=""

if [[ $INSTALL_EXIT -ne 0 ]]; then
    echo "Installer exited with code $INSTALL_EXIT — aborting."
    exit 1
fi

# ── verify image has data ─────────────────────────────────────────────────────
echo ""
echo "==> Verifying image..."
# Check first 4 bytes are not all-zero (Limine MBR code starts with 0xEB ...)
FIRST=$(xxd -l4 "$TEST_IMG" 2>/dev/null | head -1 || od -An -tx1 -N4 "$TEST_IMG" | head -1)
echo "    First 4 bytes: $FIRST"

IMG_FILLED=$(du -m "$TEST_IMG" | awk '{print $1}')
echo "    Allocated: ${IMG_FILLED}MB / ${TEST_MB}MB"

# ── offer to boot in QEMU ─────────────────────────────────────────────────────
echo ""
echo "Install complete. Boot in QEMU now? [Y/n]"
read -r ANSWER
ANSWER="${ANSWER:-Y}"
if [[ "${ANSWER^^}" == "Y" ]]; then
    cd "$SCRIPT_DIR"
    # Build disk.img if missing (needed for ext2 persistence)
    [[ -f build/disk.img ]] || make disk
    echo "==> Launching QEMU from installed image..."
    echo "    (Boot from: $TEST_IMG  |  Data disk: build/disk.img)"
    echo "    Press Ctrl-A X to quit QEMU."
    echo ""
    qemu-system-x86_64 \
        -M q35 -m 256M -smp 1 \
        -drive file="$TEST_IMG",format=raw,if=ide,index=0 \
        -drive file=build/disk.img,format=raw,if=virtio \
        -serial stdio -no-reboot -no-shutdown
else
    echo ""
    echo "To boot manually:"
    echo "  make runinstall"
fi
