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

# Create mount points git doesn't track (empty dirs)
mkdir -p "$STAGE/proc" "$STAGE/sys" "$STAGE/dev" "$STAGE/tmp" "$STAGE/run"
mkdir -p "$STAGE/root" "$STAGE/mnt" "$STAGE/etc"

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

# Copy ush if it was compiled for linux target
USH_BIN="$REPO_ROOT/build-linux/ush"
if [ -x "$USH_BIN" ]; then
    cp "$USH_BIN" "$STAGE/bin/ush"
    echo "[initramfs] included ush shell"
fi

# ── Build and include fifi-compositor ────────────────────────────────────────
echo "[initramfs] building fifi-compositor..."
(cd "$REPO_ROOT/fifi/compositor" && make -s) || {
    echo "[initramfs] WARNING: fifi-compositor build failed — falling back to shell"
}
COMP_BIN="$REPO_ROOT/build-linux/fifi-compositor"
if [ -x "$COMP_BIN" ]; then
    cp "$COMP_BIN" "$STAGE/bin/fifi-compositor"
    echo "[initramfs] included fifi-compositor"
fi

# ── Build and include standalone IPC apps ────────────────────────────────────
echo "[initramfs] building fifi-filebrowser..."
(cd "$REPO_ROOT/fifi/apps/filebrowser" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/filebrowser/fifi-filebrowser" "$STAGE/bin/"
    echo "[initramfs] included fifi-filebrowser"
} || echo "[initramfs] WARNING: fifi-filebrowser build failed"

echo "[initramfs] building fifi-settings..."
(cd "$REPO_ROOT/fifi/apps/settings" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/settings/fifi-settings" "$STAGE/bin/"
    echo "[initramfs] included fifi-settings"
} || echo "[initramfs] WARNING: fifi-settings build failed"

# Create VFS data directory (file browser root) + fonts + initial content
mkdir -p "$STAGE/fifi-data"

# Copy fonts into the VFS so the GUI can load ter16b.psf
FONT_SRC="$REPO_ROOT/initrd/rootfs/fonts"
if [ -d "$FONT_SRC" ]; then
    mkdir -p "$STAGE/fifi-data/fonts"
    cp "$FONT_SRC"/*.psf "$STAGE/fifi-data/fonts/" 2>/dev/null || true
    echo "[initramfs] included fonts from $FONT_SRC"
fi

# Populate initial fifi-data content for the file browser
mkdir -p "$STAGE/fifi-data/docs" "$STAGE/fifi-data/config"

cat > "$STAGE/fifi-data/docs/welcome.txt" << 'WELCOME'
FiFi OS — Linux Desktop
========================

Welcome to FiFi OS linux-desktop!

This is an early alpha running the FiFi desktop compositor
on top of a minimal Linux kernel.

WHAT WORKS:
  * Full FiFi desktop: taskbar, window manager, themes
  * File browser with this directory as root
  * Text editor (click any .txt file in the file browser)
  * Settings panel (F3) with theme, font, clock settings
  * Terminal window (F1) with interactive shell
  * Real clock, real memory stats

KEYBOARD SHORTCUTS:
  F1 - Toggle terminal window
  F2 - Toggle file browser
  F3 - Toggle settings panel
  F4 - Toggle text viewer

TERMINAL:
  The terminal runs a real BusyBox shell (/bin/sh).
  Type commands and press Enter. Arrow keys work for history.
  Ctrl+C to interrupt a process.

PHASE ROADMAP:
  Phase 1 - Linux kernel foundation       [DONE]
  Phase 2 - FiFi compositor on /dev/fb0  [DONE]
  Phase 3 - PTY terminal, live stats     [DONE]
  Phase 4 - DRM/KMS, XWayland, Steam    [NEXT]
  Phase 5 - Live USB, installer, WiFi    [PLANNED]
WELCOME

cat > "$STAGE/fifi-data/docs/shortcuts.txt" << 'SHORTCUTS'
FiFi OS Keyboard Shortcuts
===========================

WINDOW MANAGEMENT:
  F1          Toggle terminal window
  F2          Toggle file browser
  F3          Toggle settings panel
  F4          Toggle text viewer
  Alt+Tab     Cycle windows

TERMINAL (when focused):
  Arrow keys  Navigate history / move cursor
  Ctrl+C      Interrupt running process
  Ctrl+D      End of input / logout
  Ctrl+Z      Suspend process
  Ctrl+L      Clear screen (in bash/sh)

THEME:
  Settings > Theme to change accent color
  Settings > Wallpaper to change background

FILE BROWSER:
  Click files to open in text viewer
  Double-click directories to navigate
SHORTCUTS

echo "[initramfs] added initial fifi-data content"

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
