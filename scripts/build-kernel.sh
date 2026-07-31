#!/usr/bin/env bash
# Build the FiFi OS linux-zen kernel.
# Assumes setup-linux.sh has already been run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="${FIFI_LINUX_SRC:-${XDG_CACHE_HOME:-$HOME/.cache}/fifi-os/linux-src}"
OUT_DIR="$REPO_ROOT/build-linux"

if [ ! -f "$SRC_DIR/.config" ]; then
    echo "[build-kernel] No kernel config found. Run: bash scripts/setup-linux.sh"
    exit 1
fi

# Reapply the tracked FiFi fragment on every build so security requirements
# added after initial setup cannot be silently missed by a stale cached config.
CFG_FRAGMENT="$REPO_ROOT/linux/fifi.config"
STAGED_CFG="$SRC_DIR/fifi.config.fragment"
cp "$CFG_FRAGMENT" "$STAGED_CFG"
(
    cd "$SRC_DIR"
    scripts/kconfig/merge_config.sh -m .config "$STAGED_CFG"
    make olddefconfig
)

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
