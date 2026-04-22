#!/usr/bin/env bash
# Build the FiFi OS linux-zen kernel.
# Assumes setup-linux.sh has already been run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO_ROOT/linux/src"
OUT_DIR="$REPO_ROOT/build-linux"

if [ ! -f "$SRC_DIR/.config" ]; then
    echo "[build-kernel] No kernel config found. Run: bash scripts/setup-linux.sh"
    exit 1
fi

mkdir -p "$OUT_DIR"

JOBS=$(nproc)
echo "[build-kernel] Building with $JOBS parallel jobs..."
echo "[build-kernel] This takes 10–20 minutes on first build."

cd "$SRC_DIR"
make -j"$JOBS" bzImage

BZIMAGE="$SRC_DIR/arch/x86/boot/bzImage"
if [ ! -f "$BZIMAGE" ]; then
    echo "[build-kernel] ERROR: bzImage not found after build."
    exit 1
fi

cp "$BZIMAGE" "$OUT_DIR/bzImage"
echo ""
echo "[build-kernel] Done. Kernel at: $OUT_DIR/bzImage"
echo "[build-kernel] Kernel version: $(cat $SRC_DIR/include/config/kernel.release 2>/dev/null || echo unknown)"
