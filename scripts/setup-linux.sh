#!/usr/bin/env bash
# Download linux-zen source and apply FiFi kernel config.
# Run once before building: bash scripts/setup-linux.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEN_REPO="https://github.com/zen-kernel/zen-kernel.git"
SRC_DIR="${FIFI_LINUX_SRC:-${XDG_CACHE_HOME:-$HOME/.cache}/fifi-os/linux-src}"
LEGACY_SRC="$REPO_ROOT/linux/src"
CFG_FRAGMENT="$REPO_ROOT/linux/fifi.config"
STAGED_CFG="$SRC_DIR/fifi.config.fragment"

# Detect latest zen stable branch (format: X.Y/main)
detect_zen_branch() {
    local latest
    latest=$(git ls-remote --heads "$ZEN_REPO" 2>/dev/null \
        | awk '{print $2}' \
        | grep -E 'refs/heads/6\.[0-9]+/main$' \
        | sed 's|refs/heads/||' \
        | sort -t. -k1,1n -k2,2n \
        | tail -1)
    if [ -z "$latest" ]; then
        echo "6.19/main"   # fallback
    else
        echo "$latest"
    fi
}

mkdir -p "$(dirname "$SRC_DIR")"

# Linux's build system rejects source paths containing spaces. Older FiFi
# checkouts cloned into linux/src, so migrate that ignored tree once and keep a
# convenience symlink in the repository.
if [ "$LEGACY_SRC" != "$SRC_DIR" ] && [ -d "$LEGACY_SRC/.git" ] &&
   [ ! -e "$SRC_DIR" ]; then
    echo "[setup] Moving Linux source to space-free cache: $SRC_DIR"
    mv "$LEGACY_SRC" "$SRC_DIR"
fi
if [ "$LEGACY_SRC" != "$SRC_DIR" ] && [ ! -e "$LEGACY_SRC" ]; then
    ln -s "$SRC_DIR" "$LEGACY_SRC"
fi

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
# merge_config.sh internally word-splits its arguments, so give it a path that
# cannot contain spaces even when the repository path does.
cp "$CFG_FRAGMENT" "$STAGED_CFG"
scripts/kconfig/merge_config.sh -m .config "$STAGED_CFG"

# Run olddefconfig to resolve any remaining symbols to defaults
make olddefconfig

echo ""
echo "[setup] Kernel config ready at: $SRC_DIR/.config"
echo "[setup] Run 'make linux-kernel' to build (takes 10–20 min)."
echo "[setup] Optional: 'make linux-menuconfig' to inspect/tweak the config."
