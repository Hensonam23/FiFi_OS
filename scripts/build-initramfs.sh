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

# Embed the kernel in the initramfs so fifi-install.sh can always find it
# without having to locate and mount the USB EFI partition
mkdir -p "$STAGE/boot"
if [ -f "$REPO_ROOT/build-linux/bzImage" ]; then
    cp "$REPO_ROOT/build-linux/bzImage" "$STAGE/boot/bzImage"
    echo "[initramfs] kernel embedded at /boot/bzImage ($(du -sh "$REPO_ROOT/build-linux/bzImage" | cut -f1))"
fi

# Create mount points git doesn't track (empty dirs)
mkdir -p "$STAGE/proc" "$STAGE/sys" "$STAGE/dev" "$STAGE/tmp" "$STAGE/run"
mkdir -p "$STAGE/root" "$STAGE/mnt" "$STAGE/etc"

# Install busybox and create symlinks for all applets
mkdir -p "$STAGE/bin" "$STAGE/sbin" "$STAGE/usr/bin" "$STAGE/usr/sbin"
cp "$BUSYBOX_BIN" "$STAGE/bin/busybox"
chmod +x "$STAGE/bin/busybox"

# Populate symlinks for EVERY applet this busybox supports (head, id, whoami,
# sort, sed, grep, tail, wc, tr, find, xargs, env, tee, du, stat, etc.).
# Querying `busybox --list` guarantees nothing the binary provides is missing.
# Excluded: 'busybox' itself (the real binary) and 'bash'/'blkid', which get
# real binaries bundled later — symlinking them would be overwritten via the
# symlink and corrupt the busybox binary.
for applet in $("$BUSYBOX_BIN" --list 2>/dev/null); do
    case "$applet" in
        busybox|bash|blkid) continue ;;
    esac
    ln -sf busybox "$STAGE/bin/$applet" 2>/dev/null || true
done

# Copy ush if it was compiled for linux target
USH_BIN="$REPO_ROOT/build-linux/ush"
if [ -x "$USH_BIN" ]; then
    cp "$USH_BIN" "$STAGE/bin/ush"
    echo "[initramfs] included ush shell"
fi

# ── Real GNU bash + its shared libraries ──────────────────────────────────────
# Many tools and scripts use bash-only syntax (arrays, [[ ]], pipefail,
# process substitution) that busybox ash cannot handle.
mkdir -p "$STAGE/usr/lib" "$STAGE/usr/lib64"
if [ -x /usr/bin/bash ]; then
    rm -f "$STAGE/bin/bash"
    cp /usr/bin/bash "$STAGE/bin/bash"
    ln -sf /bin/bash "$STAGE/usr/bin/bash" 2>/dev/null || true
    ldd /usr/bin/bash 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
        [ -f "$lib" ] || continue
        cp -n "$lib" "$STAGE/usr/lib/$(basename "$lib")" 2>/dev/null || true
    done
    echo "[initramfs] real bash bundled ($(du -sh /usr/bin/bash | cut -f1))"
else
    ln -sf busybox "$STAGE/bin/bash"
    echo "[initramfs] WARNING: /usr/bin/bash not found — bash falls back to busybox"
fi

# ── sudo shim ─────────────────────────────────────────────────────────────────
# FiFi OS runs everything as root (initramfs root system), so a real setuid sudo
# (with PAM + /etc/sudoers) is unnecessary and would fail without that config.
# This shim parses past sudo's common options and execs the target command
# directly, so 'sudo <cmd>' and 'sudo -u user <cmd>' both just run <cmd> as root.
cat > "$STAGE/bin/sudo" << 'SUDOSHIM'
#!/bin/sh
# Minimal sudo shim — already root, just run the command.
while [ $# -gt 0 ]; do
    case "$1" in
        -u|-g|-h|-p|-U|-C|-r|-t|-T) shift 2 ;;   # option that takes an argument
        -i|-s)                                    # login/shell mode
            shift
            [ $# -eq 0 ] && exec /bin/bash -l
            ;;
        -E|-H|-S|-b|-n|-k|-K|-v|-l|--) shift ;;   # flags with no argument
        --*) shift ;;                             # any other long option
        -*) shift ;;                              # any other short flag
        *) break ;;                               # first non-option = the command
    esac
done
[ $# -eq 0 ] && exec /bin/bash -l
exec "$@"
SUDOSHIM
chmod +x "$STAGE/bin/sudo"
ln -sf /bin/sudo "$STAGE/usr/bin/sudo" 2>/dev/null || true
echo "[initramfs] sudo shim installed"

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

echo "[initramfs] building fifi-gamepad..."
(cd "$REPO_ROOT/fifi/apps/gamepad" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/gamepad/fifi-gamepad" "$STAGE/bin/"
    echo "[initramfs] included fifi-gamepad"
} || echo "[initramfs] WARNING: fifi-gamepad build failed"

echo "[initramfs] building fifi-sysmon..."
(cd "$REPO_ROOT/fifi/apps/sysmon" && \
    gcc -O2 -static -Wall -o fifi-sysmon sysmon.c -s 2>&1) && {
    cp "$REPO_ROOT/fifi/apps/sysmon/fifi-sysmon" "$STAGE/bin/"
    echo "[initramfs] included fifi-sysmon"
} || echo "[initramfs] WARNING: fifi-sysmon build failed"

echo "[initramfs] building fifi-netmon..."
(cd "$REPO_ROOT/fifi/apps/netmon" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/netmon/fifi-netmon" "$STAGE/bin/"
    echo "[initramfs] included fifi-netmon"
} || echo "[initramfs] WARNING: fifi-netmon build failed"

echo "[initramfs] building fifi-terminal..."
(cd "$REPO_ROOT/fifi/apps/terminal" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/terminal/fifi-terminal" "$STAGE/bin/"
    echo "[initramfs] included fifi-terminal"
} || echo "[initramfs] WARNING: fifi-terminal build failed"

echo "[initramfs] building fifi-editor..."
(cd "$REPO_ROOT/fifi/apps/editor" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/editor/fifi-editor" "$STAGE/bin/"
    echo "[initramfs] included fifi-editor"
} || echo "[initramfs] WARNING: fifi-editor build failed"

echo "[initramfs] building fifi-calc..."
(cd "$REPO_ROOT/fifi/apps/calc" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/calc/fifi-calc" "$STAGE/bin/"
    echo "[initramfs] included fifi-calc"
} || echo "[initramfs] WARNING: fifi-calc build failed"

echo "[initramfs] building fifi-imageviewer..."
(cd "$REPO_ROOT/fifi/apps/imageviewer" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/imageviewer/fifi-imageviewer" "$STAGE/bin/"
    echo "[initramfs] included fifi-imageviewer"
} || echo "[initramfs] WARNING: fifi-imageviewer build failed"

echo "[initramfs] building fifi-proton..."
(cd "$REPO_ROOT/fifi/apps/proton" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/proton/fifi-proton" "$STAGE/bin/"
    echo "[initramfs] included fifi-proton"
} || echo "[initramfs] WARNING: fifi-proton build failed"

echo "[initramfs] building fifi-security..."
(cd "$REPO_ROOT/fifi/apps/security" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/security/fifi-security" "$STAGE/bin/"
    echo "[initramfs] included fifi-security"
} || echo "[initramfs] WARNING: fifi-security build failed"

echo "[initramfs] building fifi-wifi..."
(cd "$REPO_ROOT/fifi/apps/wifi" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/wifi/fifi-wifi" "$STAGE/bin/"
    echo "[initramfs] included fifi-wifi"
} || echo "[initramfs] WARNING: fifi-wifi build failed"

echo "[initramfs] building fifi-browser..."
(cd "$REPO_ROOT/fifi/apps/browser" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/browser/fifi-browser" "$STAGE/bin/"
    echo "[initramfs] included fifi-browser"
} || echo "[initramfs] WARNING: fifi-browser build failed"

echo "[initramfs] building fifi-installer..."
(cd "$REPO_ROOT/fifi/apps/installer" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/installer/fifi-installer" "$STAGE/bin/"
    echo "[initramfs] included fifi-installer"
} || echo "[initramfs] WARNING: fifi-installer build failed"

# ── Disk installer tools (parted, mkfs.ext4, mkfs.fat, blkid, grub-install) ──
echo "[initramfs] bundling disk installer tools..."
cp "$STAGE/bin/fifi-install.sh" "$STAGE/bin/fifi-install.sh" 2>/dev/null || true  # already staged above
install_tools_ok=true
mkdir -p "$STAGE/usr/lib"
for tool in parted mkfs.ext4 mkfs.fat blkid; do
    bin="$(which $tool 2>/dev/null)"
    if [ -x "$bin" ]; then
        rm -f "$STAGE/bin/$tool"   # never cp through a busybox applet symlink
        cp "$bin" "$STAGE/bin/$tool"
        # Copy shared library dependencies
        ldd "$bin" 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
            [ -f "$lib" ] || continue
            dest="$STAGE/usr/lib/$(basename "$lib")"
            [ -f "$dest" ] || cp "$lib" "$dest"
        done
    else
        echo "[initramfs] WARNING: $tool not found -- disk installer will fail"
        install_tools_ok=false
    fi
done
# grub-install + modules
GRUB_INSTALL="$(which grub-install 2>/dev/null)"
if [ -x "$GRUB_INSTALL" ]; then
    cp "$GRUB_INSTALL" "$STAGE/bin/grub-install"
    # Copy grub x86_64-efi modules
    for dir in /usr/lib/grub/x86_64-efi /usr/share/grub/x86_64-efi; do
        if [ -d "$dir" ]; then
            mkdir -p "$STAGE/usr/lib/grub/x86_64-efi"
            cp -r "$dir/"* "$STAGE/usr/lib/grub/x86_64-efi/" 2>/dev/null || true
            break
        fi
    done
    ldd "$GRUB_INSTALL" 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
        [ -f "$lib" ] || continue
        dest="$STAGE/usr/lib/$(basename "$lib")"
        [ -f "$dest" ] || cp "$lib" "$dest"
    done
    echo "[initramfs] disk installer tools bundled"
else
    echo "[initramfs] WARNING: grub-install not found -- disk installer will not work"
fi
# grub-mkimage — fallback if grub-install fails
GRUB_MKIMAGE="$(which grub-mkimage 2>/dev/null)"
if [ -x "$GRUB_MKIMAGE" ]; then
    cp "$GRUB_MKIMAGE" "$STAGE/bin/grub-mkimage"
    ldd "$GRUB_MKIMAGE" 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
        [ -f "$lib" ] || continue
        dest="$STAGE/usr/lib/$(basename "$lib")"
        [ -f "$dest" ] || cp "$lib" "$dest"
    done
    echo "[initramfs] grub-mkimage bundled"
else
    echo "[initramfs] WARNING: grub-mkimage not found"
fi
# efibootmgr — needed to register boot entry in UEFI NVRAM
EFIBOOTMGR="$(which efibootmgr 2>/dev/null)"
if [ -x "$EFIBOOTMGR" ]; then
    cp "$EFIBOOTMGR" "$STAGE/bin/efibootmgr"
    ldd "$EFIBOOTMGR" 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
        [ -f "$lib" ] || continue
        dest="$STAGE/usr/lib/$(basename "$lib")"
        [ -f "$dest" ] || cp "$lib" "$dest"
    done
    echo "[initramfs] efibootmgr bundled"
else
    echo "[initramfs] WARNING: efibootmgr not found"
fi

# ── nftables firewall ─────────────────────────────────────────────────────────
echo "[initramfs] bundling nftables..."
NFT_BIN=""
for candidate in /usr/sbin/nft /usr/bin/nft /sbin/nft; do
    [ -x "$candidate" ] && NFT_BIN="$candidate" && break
done
if [ -n "$NFT_BIN" ]; then
    mkdir -p "$STAGE/usr/sbin" "$STAGE/usr/lib"
    cp "$NFT_BIN" "$STAGE/usr/sbin/nft"
    chmod +x "$STAGE/usr/sbin/nft"
    NFT_LIBS=$(ldd "$NFT_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $NFT_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue
        [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib")
        link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] nftables bundled ($(du -sh "$NFT_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: nft not found — install nftables for firewall support"
fi

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
mkdir -p "$STAGE/fifi-data/docs" "$STAGE/fifi-data/config" "$STAGE/fifi-data/images"

cat > "$STAGE/fifi-data/docs/welcome.txt" << 'WELCOME'
FiFi OS — Linux Desktop
========================

Welcome to FiFi OS linux-desktop!

This is an early alpha running the FiFi desktop compositor
on top of a minimal Linux kernel.

WHAT WORKS:
  * Full FiFi desktop: taskbar, window manager, themes
  * File browser — drag and drop, rename (F5), delete (d+d), new folder (n)
  * Text editor — click any .txt/.c/.h/.sh/.md file; Ctrl+S save, Ctrl+F find
  * Settings panel (F3) with theme, font, clock settings
  * Terminal (F1) — real PTY shell, scrollback (PgUp/PgDn), resizes with window
  * Window snapping — drag to screen edges; left/right/maximize
  * Window resize — drag bottom-right corner
  * Clipboard — Ctrl+C/V across all apps
  * File drag-and-drop between windows
  * Screenshots — PrintScreen key saves PPM to /fifi-data/screenshots/
  * Real clock, real memory stats

KEYBOARD SHORTCUTS:
  F1 - Toggle terminal window
  F2 - Toggle file browser
  F3 - Toggle settings panel
  F4 - Toggle text viewer

TERMINAL:
  Real PTY shell (/bin/sh). Arrow keys + history. Ctrl+C/D/Z.
  PgUp/PgDn to scroll through 300-line scrollback buffer.
  Terminal resizes automatically when window is snapped or resized.

TEXT EDITOR:
  Full editor with line numbers, find (Ctrl+F), undo (Ctrl+Z), save (Ctrl+S).
  Opens from: file browser (click any text file) or launcher menu.
  Launch with: fifi-editor /path/to/file

PHASE ROADMAP:
  Phase 1 - Linux kernel foundation              [DONE]
  Phase 2 - FiFi compositor on /dev/fb0         [DONE]
  Phase 3 - PTY terminal, live stats            [DONE]
  Phase 4 - DRM/KMS, gaming mode, XWayland,    [DONE]
            PipeWire audio, Steam/Proton support
  Phase 5 - Security: encryption, AppArmor,     [IN PROGRESS]
            firewall, VPN, Tor, privacy tools
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

# Bundle sample images (scaled-down BMPs) if available
for img_src in /tmp/IMG_2201_small.bmp /tmp/IMG_2202_small.bmp; do
    [ -f "$img_src" ] && cp "$img_src" "$STAGE/fifi-data/images/" && \
        echo "[initramfs] bundled image $(basename $img_src)"
done

echo "[initramfs] added initial fifi-data content"

# ── PipeWire audio daemon (multi-app mixing, PulseAudio-compatible) ──────────
echo "[initramfs] bundling PipeWire..."
mkdir -p "$STAGE/usr/bin" "$STAGE/usr/lib" "$STAGE/usr/lib64"
mkdir -p "$STAGE/usr/lib/spa-0.2/alsa" "$STAGE/usr/lib/spa-0.2/audioconvert"
mkdir -p "$STAGE/usr/lib/spa-0.2/support" "$STAGE/usr/lib/spa-0.2/audiomixer"
mkdir -p "$STAGE/usr/lib/pipewire-0.3"
mkdir -p "$STAGE/usr/share/pipewire"

# Copy pipewire binaries
for bin in pipewire pipewire-pulse; do
    [ -x "/usr/bin/$bin" ] && cp "/usr/bin/$bin" "$STAGE/usr/bin/$bin"
done

# Copy spa plugins needed for ALSA output + audio mixing
for so in \
    /usr/lib/spa-0.2/alsa/libspa-alsa.so \
    /usr/lib/spa-0.2/audioconvert/libspa-audioconvert.so \
    /usr/lib/spa-0.2/support/libspa-support.so \
    /usr/lib/spa-0.2/audiomixer/libspa-audiomixer.so; do
    dir=$(dirname "$so" | sed "s|/usr/lib|$STAGE/usr/lib|")
    [ -f "$so" ] && mkdir -p "$dir" && cp "$so" "$dir/"
done

# Copy pipewire modules needed (protocol-native, protocol-pulse, client-node, etc.)
for mod in \
    libpipewire-module-protocol-native.so \
    libpipewire-module-protocol-pulse.so \
    libpipewire-module-client-node.so \
    libpipewire-module-adapter.so \
    libpipewire-module-link-factory.so \
    libpipewire-module-metadata.so \
    libpipewire-module-spa-node-factory.so \
    libpipewire-module-spa-device-factory.so \
    libpipewire-module-access.so \
    libpipewire-module-rt.so \
    libpipewire-module-profiler.so; do
    src="/usr/lib/pipewire-0.3/$mod"
    [ -f "$src" ] && cp "$src" "$STAGE/usr/lib/pipewire-0.3/"
done

# Copy pipewire-pulse config
[ -f /usr/share/pipewire/pipewire-pulse.conf ] && \
    cp /usr/share/pipewire/pipewire-pulse.conf "$STAGE/usr/share/pipewire/"

# Collect all unique shared library dependencies for pipewire + spa plugins
PW_LIBS=$(
    {
        ldd /usr/bin/pipewire
        ldd /usr/bin/pipewire-pulse
        for f in \
            /usr/lib/spa-0.2/alsa/libspa-alsa.so \
            /usr/lib/spa-0.2/audioconvert/libspa-audioconvert.so \
            /usr/lib/spa-0.2/support/libspa-support.so \
            /usr/lib/spa-0.2/audiomixer/libspa-audiomixer.so; do
            [ -f "$f" ] && ldd "$f"
        done
        for f in /usr/lib/pipewire-0.3/libpipewire-module-*.so; do
            [ -f "$f" ] && ldd "$f"
        done
    } 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u
)
for lib in $PW_LIBS; do
    real=$(realpath "$lib" 2>/dev/null) || continue
    [ -f "$real" ] || continue
    dest="$STAGE/usr/lib/$(basename "$real")"
    [ -f "$dest" ] || cp "$real" "$dest"
    # Also create the versioned symlink name the binary expects
    link_name=$(basename "$lib")
    link_path="$STAGE/usr/lib/$link_name"
    [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
done
# ld-linux loader lives in lib64
if [ -f /usr/lib64/ld-linux-x86-64.so.2 ]; then
    cp /usr/lib64/ld-linux-x86-64.so.2 "$STAGE/usr/lib64/"
fi
# glibc compatibility stubs — needed by AppImage-bundled binaries (LibreWolf etc.)
# On glibc 2.34+ these are stubs but must be present for older binaries to load.
for lib in \
    /usr/lib/libdl.so.2 \
    /usr/lib/libpthread.so.0 \
    /usr/lib/librt.so.1 \
    /usr/lib/libutil.so.1 \
    /usr/lib/libgcc_s.so.1 \
    /usr/lib/libnss_files.so.2 \
    /usr/lib/libnss_dns.so.2 \
    /usr/lib/libresolv.so.2 \
    /usr/lib/libm.so.6; do
    [ -f "$lib" ] && cp "$lib" "$STAGE/usr/lib/" 2>/dev/null || true
done

echo "[initramfs] PipeWire bundled"

# ── XWayland (runs X11 apps through the Wayland compositor) ──────────────────
echo "[initramfs] bundling XWayland..."
if [ -x /usr/bin/Xwayland ]; then
    cp /usr/bin/Xwayland "$STAGE/usr/bin/Xwayland"
    XW_LIBS=$(ldd /usr/bin/Xwayland 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $XW_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue
        [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib")
        link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] XWayland bundled ($(du -sh /usr/bin/Xwayland | cut -f1))"
else
    echo "[initramfs] WARNING: Xwayland not found — X11 app support disabled"
fi

# ── WireGuard tools (wg) ─────────────────────────────────────────────────────
echo "[initramfs] bundling WireGuard tools..."
WG_BIN=""
for candidate in /usr/bin/wg /usr/local/bin/wg; do
    [ -x "$candidate" ] && WG_BIN="$candidate" && break
done
if [ -n "$WG_BIN" ]; then
    cp "$WG_BIN" "$STAGE/usr/bin/wg"
    chmod +x "$STAGE/usr/bin/wg"
    WG_LIBS=$(ldd "$WG_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $WG_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] WireGuard tools bundled"
else
    echo "[initramfs] NOTE: wg not found -- install wireguard-tools for VPN support"
fi

# Make wg-up executable
[ -f "$STAGE/bin/wg-up" ] && chmod +x "$STAGE/bin/wg-up"

# ── nmap ─────────────────────────────────────────────────────────────────────
echo "[initramfs] bundling nmap..."
NMAP_BIN=""
for candidate in /usr/bin/nmap /usr/local/bin/nmap; do
    [ -x "$candidate" ] && NMAP_BIN="$candidate" && break
done
if [ -n "$NMAP_BIN" ]; then
    cp "$NMAP_BIN" "$STAGE/usr/bin/nmap"
    chmod +x "$STAGE/usr/bin/nmap"
    NMAP_LIBS=$(ldd "$NMAP_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $NMAP_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    if [ -d /usr/share/nmap ]; then
        mkdir -p "$STAGE/usr/share/nmap"
        # Core data files (needed for all scans)
        for f in nmap-services nmap-protocols nmap-rpc nmap-service-probes nmap-mac-prefixes; do
            [ -f "/usr/share/nmap/$f" ] && cp "/usr/share/nmap/$f" "$STAGE/usr/share/nmap/"
        done
        # NSE engine and support library (needed for -sV version detection)
        [ -f "/usr/share/nmap/nse_main.lua" ] && cp "/usr/share/nmap/nse_main.lua" "$STAGE/usr/share/nmap/"
        [ -d "/usr/share/nmap/nselib" ] && cp -r "/usr/share/nmap/nselib" "$STAGE/usr/share/nmap/"
        # Version detection scripts only (not the full 400+ script set)
        mkdir -p "$STAGE/usr/share/nmap/scripts"
        for s in /usr/share/nmap/scripts/banner.nse \
                  /usr/share/nmap/scripts/fingerprint-strings.nse \
                  /usr/share/nmap/scripts/ssl-cert.nse \
                  /usr/share/nmap/scripts/http-title.nse \
                  /usr/share/nmap/scripts/ssh-hostkey.nse; do
            [ -f "$s" ] && cp "$s" "$STAGE/usr/share/nmap/scripts/"
        done
    fi
    echo "[initramfs] nmap bundled ($(du -sh "$NMAP_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: nmap not found -- install nmap for network scanning"
fi

# ── tcpdump ───────────────────────────────────────────────────────────────────
echo "[initramfs] bundling tcpdump..."
TCPDUMP_BIN=""
for candidate in /usr/bin/tcpdump /usr/sbin/tcpdump; do
    [ -x "$candidate" ] && TCPDUMP_BIN="$candidate" && break
done
if [ -n "$TCPDUMP_BIN" ]; then
    cp "$TCPDUMP_BIN" "$STAGE/usr/bin/tcpdump"
    chmod +x "$STAGE/usr/bin/tcpdump"
    TD_LIBS=$(ldd "$TCPDUMP_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $TD_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] tcpdump bundled ($(du -sh "$TCPDUMP_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: tcpdump not found -- install tcpdump for packet capture"
fi

# ── tor ───────────────────────────────────────────────────────────────────────
echo "[initramfs] bundling tor..."
TOR_BIN=""
for candidate in /usr/bin/tor /usr/sbin/tor; do
    [ -x "$candidate" ] && TOR_BIN="$candidate" && break
done
if [ -n "$TOR_BIN" ]; then
    cp "$TOR_BIN" "$STAGE/usr/bin/tor"
    chmod +x "$STAGE/usr/bin/tor"
    TOR_LIBS=$(ldd "$TOR_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $TOR_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] tor bundled ($(du -sh "$TOR_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: tor not found -- install tor for Tor routing support"
fi

# ── Dropbear SSH daemon ───────────────────────────────────────────────────────
echo "[initramfs] bundling Dropbear SSH daemon..."
DROPBEAR_BIN=""
for candidate in /usr/sbin/dropbear /usr/bin/dropbear; do
    [ -x "$candidate" ] && DROPBEAR_BIN="$candidate" && break
done
DROPBEARKEY_BIN=""
for candidate in /usr/bin/dropbearkey /usr/sbin/dropbearkey; do
    [ -x "$candidate" ] && DROPBEARKEY_BIN="$candidate" && break
done
if [ -n "$DROPBEAR_BIN" ]; then
    mkdir -p "$STAGE/usr/sbin" "$STAGE/usr/bin" "$STAGE/usr/lib"
    cp "$DROPBEAR_BIN" "$STAGE/usr/sbin/dropbear"
    chmod +x "$STAGE/usr/sbin/dropbear"
    [ -n "$DROPBEARKEY_BIN" ] && cp "$DROPBEARKEY_BIN" "$STAGE/usr/bin/dropbearkey" && \
        chmod +x "$STAGE/usr/bin/dropbearkey"
    for _dbin in "$DROPBEAR_BIN" "$DROPBEARKEY_BIN"; do
        [ -z "$_dbin" ] && continue
        _dlibs=$(ldd "$_dbin" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
        for lib in $_dlibs; do
            real=$(realpath "$lib" 2>/dev/null) || continue
            [ -f "$real" ] || continue
            dest="$STAGE/usr/lib/$(basename "$real")"
            [ -f "$dest" ] || cp "$real" "$dest"
            link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
            [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
        done
    done
    echo "[initramfs] Dropbear bundled ($(du -sh "$DROPBEAR_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: dropbear not found — install dropbear for SSH access"
fi

# ── WiFi: iwd daemon + iw tool + Intel/Qualcomm firmware + regulatory DB ──────
echo "[initramfs] bundling WiFi support..."

# iwd daemon (handles WPA2/WPA3 auth + built-in DHCP)
IWD_BIN="/usr/lib/iwd/iwd"
IWCTL_BIN="/usr/bin/iwctl"
if [ -x "$IWD_BIN" ]; then
    mkdir -p "$STAGE/usr/lib/iwd"
    cp "$IWD_BIN" "$STAGE/usr/lib/iwd/iwd"; chmod +x "$STAGE/usr/lib/iwd/iwd"
    [ -x "$IWCTL_BIN" ] && cp "$IWCTL_BIN" "$STAGE/usr/bin/iwctl" && chmod +x "$STAGE/usr/bin/iwctl"
    # iwd only needs libell
    IWD_LIBS=$(ldd "$IWD_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $IWD_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] iwd bundled ($(du -sh "$IWD_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: iwd not found -- install iwd for WiFi support"
fi

# iw tool (WiFi interface control/scanning)
IW_BIN="/usr/bin/iw"
if [ -x "$IW_BIN" ]; then
    cp "$IW_BIN" "$STAGE/usr/bin/iw"; chmod +x "$STAGE/usr/bin/iw"
    IW_LIBS=$(ldd "$IW_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $IW_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
fi

# Intel WiFi firmware (latest versions of AX200/AX201/AX210/AX211 families)
# Stored as .ucode.zst — the kernel loads them natively (CONFIG_FW_LOADER_COMPRESS_ZSTD=y)
FWDIR="/lib/firmware/intel/iwlwifi"
FWSTAGE="$STAGE/lib/firmware/intel/iwlwifi"
if [ -d "$FWDIR" ]; then
    mkdir -p "$FWSTAGE"
    # Decompress and bundle the latest version of each sub-variant.
    # The FiFi kernel does not have CONFIG_FW_LOADER_COMPRESS so it needs raw .ucode files.
    # zstd -d decompresses each .zst to a plain .ucode which the kernel can load directly.
    find "$FWDIR" -name "iwlwifi-*.ucode.zst" 2>/dev/null | \
        sed 's/-[0-9]*\.ucode\.zst$//' | sort -u | while read prefix; do
            latest=$(find "$FWDIR" -name "$(basename "$prefix")-*.ucode.zst" 2>/dev/null | sort -V | tail -1)
            [ -z "$latest" ] && continue
            outname=$(basename "$latest" .zst)  # strip .zst → plain .ucode name
            zstd -d -q "$latest" -o "$FWSTAGE/$outname" 2>/dev/null || true
        done
    # pnvm files (required for AX211 160MHz mode) — decompress these too
    find "$FWDIR" -name "*.pnvm.zst" 2>/dev/null | while read pnvm; do
        outname=$(basename "$pnvm" .zst)
        zstd -d -q "$pnvm" -o "$FWSTAGE/$outname" 2>/dev/null || true
    done
    # Create symlinks at /lib/firmware/ pointing to the decompressed files
    find "$FWSTAGE" -name "*.ucode" -o -name "*.pnvm" 2>/dev/null | while read f; do
        bn=$(basename "$f")
        [ -e "$STAGE/lib/firmware/$bn" ] || ln -sf "intel/iwlwifi/$bn" "$STAGE/lib/firmware/$bn"
    done
    echo "[initramfs] Intel WiFi firmware bundled ($(du -sh "$FWSTAGE" | cut -f1))"
else
    echo "[initramfs] NOTE: Intel WiFi firmware not found -- install linux-firmware-intel"
fi

# Regulatory domain database (required for legal WiFi channel use)
for regdb in /usr/lib/firmware/regulatory.db /lib/firmware/regulatory.db; do
    if [ -f "$regdb" ]; then
        mkdir -p "$STAGE/lib/firmware"
        cp "$regdb" "$STAGE/lib/firmware/regulatory.db"
        sig="${regdb}.p7s"
        [ -f "$sig" ] && cp "$sig" "$STAGE/lib/firmware/regulatory.db.p7s"
        echo "[initramfs] regulatory.db bundled"
        break
    fi
done

# Create iwd runtime dirs
mkdir -p "$STAGE/var/lib/iwd" "$STAGE/etc/iwd" "$STAGE/var/run/wpa_supplicant"

# ── wpa_supplicant (WiFi auth — WPA2/WPA3, no D-Bus needed) ──────────────────
WPA_BIN="/usr/bin/wpa_supplicant"
if [ -x "$WPA_BIN" ]; then
    cp "$WPA_BIN" "$STAGE/usr/bin/wpa_supplicant"
    chmod +x "$STAGE/usr/bin/wpa_supplicant"
    WPA_LIBS=$(ldd "$WPA_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $WPA_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] wpa_supplicant bundled ($(du -sh "$WPA_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: wpa_supplicant not found -- WiFi connect may not work"
fi

# ── AppArmor tools (mandatory access control) ────────────────────────────────
AA_PARSER="/usr/bin/apparmor_parser"
if [ -x "$AA_PARSER" ]; then
    cp "$AA_PARSER" "$STAGE/usr/bin/apparmor_parser"
    chmod +x "$STAGE/usr/bin/apparmor_parser"
    AA_LIBS=$(ldd "$AA_PARSER" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $AA_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    # Include aa-status for checking profile load state
    [ -x /usr/bin/aa-status ] && cp /usr/bin/aa-status "$STAGE/usr/bin/aa-status" || true
    echo "[initramfs] apparmor_parser bundled"
fi
# AppArmor profiles are in initramfs/root/etc/apparmor.d/ (already staged via rootfs copy)

# ── cryptsetup (LUKS2 encrypted storage support) ─────────────────────────────
CRYPT_BIN="/usr/bin/cryptsetup"
if [ -x "$CRYPT_BIN" ]; then
    cp "$CRYPT_BIN" "$STAGE/usr/bin/cryptsetup"
    chmod +x "$STAGE/usr/bin/cryptsetup"
    CRYPT_LIBS=$(ldd "$CRYPT_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $CRYPT_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] cryptsetup bundled ($(du -sh "$CRYPT_BIN" | cut -f1))"
fi

# ── dnscrypt-proxy ────────────────────────────────────────────────────────────
echo "[initramfs] bundling dnscrypt-proxy..."
DCRYPT_BIN=""
for candidate in /usr/bin/dnscrypt-proxy /usr/local/bin/dnscrypt-proxy; do
    [ -x "$candidate" ] && DCRYPT_BIN="$candidate" && break
done
if [ -n "$DCRYPT_BIN" ]; then
    cp "$DCRYPT_BIN" "$STAGE/usr/bin/dnscrypt-proxy"
    chmod +x "$STAGE/usr/bin/dnscrypt-proxy"
    DCRYPT_LIBS=$(ldd "$DCRYPT_BIN" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u)
    for lib in $DCRYPT_LIBS; do
        real=$(realpath "$lib" 2>/dev/null) || continue; [ -f "$real" ] || continue
        dest="$STAGE/usr/lib/$(basename "$real")"
        [ -f "$dest" ] || cp "$real" "$dest"
        link_name=$(basename "$lib"); link_path="$STAGE/usr/lib/$link_name"
        [ -e "$link_path" ] || ln -sf "$(basename "$real")" "$link_path"
    done
    echo "[initramfs] dnscrypt-proxy bundled ($(du -sh "$DCRYPT_BIN" | cut -f1))"
else
    echo "[initramfs] NOTE: dnscrypt-proxy not found -- install dnscrypt-proxy for DoH support"
fi

# ── curl (for HTTPS downloads: browser AppImage, LibreOffice, etc.) ──────────
echo "[initramfs] bundling curl..."
CURL_BIN="$(which curl 2>/dev/null)"
if [ -x "$CURL_BIN" ]; then
    cp "$CURL_BIN" "$STAGE/bin/curl"
    # Copy all shared library dependencies
    ldd "$CURL_BIN" 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
        [ -f "$lib" ] || continue
        dest="$STAGE/usr/lib/$(basename "$lib")"
        [ -f "$dest" ] || cp "$lib" "$dest"
    done
    echo "[initramfs] curl bundled ($(du -sh "$CURL_BIN" | cut -f1))"
else
    echo "[initramfs] WARNING: curl not found -- browser/LibreOffice downloads will fail"
fi

# ── CA certificates (required for TLS in Go binaries like dnscrypt-proxy) ────
echo "[initramfs] bundling CA certificates..."
if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
    mkdir -p "$STAGE/etc/ssl/certs"
    cp /etc/ssl/certs/ca-certificates.crt "$STAGE/etc/ssl/certs/"
    echo "[initramfs] CA certificates bundled ($(du -sh /etc/ssl/certs/ca-certificates.crt | cut -f1))"
else
    echo "[initramfs] WARNING: /etc/ssl/certs/ca-certificates.crt not found -- TLS will fail"
fi

# /lib64 and /lib symlinks -- all Arch binaries expect ELF interpreter at /lib64/ld-linux-x86-64.so.2
# On Arch: /lib64 -> usr/lib and /usr/lib64 -> lib (both point at /usr/lib)
# We copy ld.so into $STAGE/usr/lib64/; create /lib64 -> usr/lib64 so binaries find it.
ln -sf usr/lib64 "$STAGE/lib64" 2>/dev/null || true
ln -sf usr/lib   "$STAGE/lib"   2>/dev/null || true

# /etc/ld.so.conf.d so the dynamic linker finds /usr/lib
mkdir -p "$STAGE/etc/ld.so.conf.d"
echo "/usr/lib" > "$STAGE/etc/ld.so.conf.d/fifi.conf"
echo "/usr/lib64" >> "$STAGE/etc/ld.so.conf.d/fifi.conf"
# ld.so.cache — pre-generate inside stage
ldconfig -r "$STAGE" 2>/dev/null || true

# Write a FiFi-specific PipeWire config (no udev, no wireplumber required)
cat > "$STAGE/usr/share/pipewire/fifi.conf" << 'PWCFG'
context.properties = {
    core.daemon = true
    core.name   = pipewire-0
    support.dbus = false
    log.level = 2
    link.max-buffers = 16
    default.clock.rate = 48000
    default.clock.quantum = 1024
    default.clock.min-quantum = 32
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    audio.adapt      = audioconvert/libspa-audioconvert
    api.alsa.*       = alsa/libspa-alsa
    support.*        = support/libspa-support
    audio.mixer      = audiomixer/libspa-audiomixer
}
context.modules = [
    { name = libpipewire-module-rt              flags = [ ifexists nofail ] }
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-metadata        flags = [ ifexists nofail ] }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-spa-device-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-access          flags = [ nofail ] }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-protocol-pulse  flags = [ ifexists nofail ] }
]
context.objects = [
    { factory = metadata
        args = { metadata.name = default }
    }
    { factory = spa-node-factory
        args = {
            factory.name    = support.node.driver
            node.name       = Dummy-Driver
            node.group      = pipewire.dummy
            priority.driver = 20000
        }
    }
    { factory = spa-device-factory
        args = {
            factory.name = api.alsa.enum.udev
            alsa.use-acp = true
            device.object.properties = {
                api.acp.auto-profile = true
                api.acp.auto-port    = true
                device.object.properties = {
                    node.adapter = audio.adapt
                    resample.disable = false
                    adapter.auto-port-config = {
                        mode = dsp
                        monitor = false
                        control = false
                        position = preserve
                    }
                }
            }
        }
    }
]
pulse.properties = {
    server.address = [ "unix:native" ]
}
PWCFG

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
