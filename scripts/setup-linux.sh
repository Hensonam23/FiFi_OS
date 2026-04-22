#!/usr/bin/env bash
# Download linux-zen source and apply FiFi kernel config.
# Run once before building: bash scripts/setup-linux.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEN_REPO="https://github.com/zen-kernel/zen-kernel.git"
SRC_DIR="$REPO_ROOT/linux/src"
CFG_FRAGMENT="$REPO_ROOT/linux/fifi.config"

# Detect latest zen stable branch from git ls-remote, or fall back to a known good one
detect_zen_branch() {
    local latest
    latest=$(git ls-remote --heads "$ZEN_REPO" 'refs/heads/zen/v*/zen' 2>/dev/null \
        | awk '{print $2}' \
        | sed 's|refs/heads/||' \
        | sort -t. -k1,1n -k2,2n \
        | tail -1)
    if [ -z "$latest" ]; then
        echo "zen/v6.14/zen"   # fallback — update if this gets too old
    else
        echo "$latest"
    fi
}

if [ -d "$SRC_DIR/.git" ]; then
    echo "[setup] linux-zen source already at $SRC_DIR"
    echo "[setup] To update: cd $SRC_DIR && git pull"
else
    BRANCH=$(detect_zen_branch)
    echo "[setup] Cloning linux-zen branch: $BRANCH"
    echo "[setup] This is ~1 GB and takes a few minutes..."
    git clone --depth=1 --branch "$BRANCH" "$ZEN_REPO" "$SRC_DIR"
    echo "[setup] Clone complete."
fi

echo "[setup] Applying FiFi kernel config fragment..."
cd "$SRC_DIR"

# Start from zen's recommended x86_64 desktop config
make x86_64_defconfig

# Merge our FiFi-specific overrides on top
scripts/kconfig/merge_config.sh -m .config "$CFG_FRAGMENT"

# Run olddefconfig to resolve any remaining symbols to defaults
make olddefconfig

echo ""
echo "[setup] Kernel config ready at: $SRC_DIR/.config"
echo "[setup] Run 'make linux-kernel' to build (takes 10–20 min)."
echo "[setup] Optional: 'make linux-menuconfig' to inspect/tweak the config."
