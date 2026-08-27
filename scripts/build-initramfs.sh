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
# The development shell may run in a container while the static BusyBox binary
# is installed on the host. Host paths are read-only inputs and produce the same
# self-contained initramfs.
for candidate in /usr/bin/busybox /bin/busybox \
                 /run/host/usr/bin/busybox /run/host/root/usr/bin/busybox; do
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

# Identify the exact image and its update channel. Test-channel builds keep
# following test releases after installation; stable builds follow normal
# releases. CI may pass FIFI_BUILD_ID explicitly, otherwise use the Git commit.
FIFI_UPDATE_CHANNEL="${FIFI_UPDATE_CHANNEL:-stable}"
case "$FIFI_UPDATE_CHANNEL" in
    stable|test) ;;
    *)
        echo "[initramfs] ERROR: FIFI_UPDATE_CHANNEL must be stable or test" >&2
        exit 1
        ;;
esac
FIFI_BUILD_ID="${FIFI_BUILD_ID:-${GITHUB_SHA:-}}"
if [ -z "$FIFI_BUILD_ID" ]; then
    FIFI_BUILD_ID="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
    if [ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=normal 2>/dev/null)" ]; then
        FIFI_BUILD_ID="${FIFI_BUILD_ID}-dirty"
    fi
fi
printf '%s\n' "$FIFI_UPDATE_CHANNEL" > "$STAGE/etc/fifi-update-channel"
printf '%s\n' "$FIFI_BUILD_ID" > "$STAGE/etc/fifi-build-id"
FIFI_BUILD_SHORT="${FIFI_BUILD_ID:0:12}"
case "$FIFI_BUILD_ID" in *-dirty) FIFI_BUILD_SHORT="${FIFI_BUILD_SHORT}-dirty" ;; esac
printf 'FiFi OS linux-desktop Beta 1.0 (build %s, %s channel)\n' \
    "$FIFI_BUILD_SHORT" "$FIFI_UPDATE_CHANNEL" > "$STAGE/etc/fifi-version"
echo "[initramfs] update channel: $FIFI_UPDATE_CHANNEL  build: $FIFI_BUILD_ID"

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
# Excluded: 'busybox' itself (the real binary), 'bash'/'blkid', which get real
# binaries bundled later, and the reboot/poweroff wrappers, which send desktop
# requests through the narrow root broker while retaining BusyBox for UID 0.
for applet in $("$BUSYBOX_BIN" --list 2>/dev/null); do
    case "$applet" in
        busybox|bash|blkid|reboot|poweroff) continue ;;
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

# Small, auditable privilege boundary used when the root compositor launches
# ordinary apps and terminal shells.
gcc -std=c11 -O2 -Wall -Wextra -static \
    "$REPO_ROOT/fifi/platform/linux/fifi-user-exec.c" \
    -o "$STAGE/bin/fifi-user-exec"
chmod 755 "$STAGE/bin/fifi-user-exec"
echo "[initramfs] non-root app launcher installed"

# Root daemon with a fixed command grammar for desktop administrative actions.
# The client uses the same binary without elevation; authorization comes from
# SO_PEERCRED on the root-owned Unix socket, not setuid or a general shell.
gcc -std=c11 -O2 -Wall -Wextra -static \
    "$REPO_ROOT/fifi/platform/linux/fifi-admin.c" \
    -o "$STAGE/bin/fifi-admin"
chmod 755 "$STAGE/bin/fifi-admin"
echo "[initramfs] narrow administrative broker installed"

# Fixed-purpose Wi-Fi helper reached only through the authenticated broker.
# Network names and passwords arrive on stdin, never in argv or logs.
gcc -std=c11 -O2 -Wall -Wextra -static \
    "$REPO_ROOT/fifi/platform/linux/fifi-wifi-ctl.c" \
    -o "$STAGE/bin/fifi-wifi-ctl"
chmod 755 "$STAGE/bin/fifi-wifi-ctl"
echo "[initramfs] narrow Wi-Fi control helper installed"

# libinput relies on the standard udev input properties (ID_INPUT_MOUSE,
# ID_INPUT_TOUCHPAD, device grouping, fuzz overrides). FiFi keeps its custom
# init/service model, but runs the small device manager and only the input rules
# needed to describe evdev hardware correctly.
UDEVADM_BIN="$(command -v udevadm 2>/dev/null || true)"
UDEVD_BIN=""
for candidate in /usr/lib/systemd/systemd-udevd /lib/systemd/systemd-udevd; do
    [ -x "$candidate" ] && { UDEVD_BIN="$candidate"; break; }
done
[ -n "$UDEVADM_BIN" ] && [ -n "$UDEVD_BIN" ] || {
    echo "[initramfs] ERROR: udev is required for reliable input discovery" >&2
    exit 1
}
cp "$UDEVADM_BIN" "$STAGE/usr/bin/udevadm"
mkdir -p "$STAGE/usr/lib/systemd" "$STAGE/usr/lib/udev/rules.d"
cp -L "$UDEVD_BIN" "$STAGE/usr/lib/systemd/systemd-udevd"
for binary in "$UDEVADM_BIN" "$UDEVD_BIN"; do
    ldd "$binary" 2>/dev/null | awk '/=>/{print $3}' | while read -r lib; do
        [ -f "$lib" ] || continue
        cp -L "$lib" "$STAGE/usr/lib/$(basename "$lib")"
    done
done
for rule in 60-evdev.rules 60-input-id.rules 60-persistent-input.rules \
            70-touchpad.rules 80-libinput-device-groups.rules \
            90-libinput-fuzz-override.rules; do
    [ -f "/usr/lib/udev/rules.d/$rule" ] && \
        cp "/usr/lib/udev/rules.d/$rule" "$STAGE/usr/lib/udev/rules.d/"
done
echo "[initramfs] udev input discovery bundled"

# ── Build and include fifi-compositor ────────────────────────────────────────
echo "[initramfs] building fifi-compositor..."
COMP_BIN="$REPO_ROOT/build-linux/fifi-compositor"
rm -f "$COMP_BIN"
(cd "$REPO_ROOT/fifi/compositor" && make -s) || {
    echo "[initramfs] ERROR: fifi-compositor build failed" >&2
    exit 1
}
[ -x "$COMP_BIN" ] || {
    echo "[initramfs] ERROR: compositor build produced no executable" >&2
    exit 1
}
if ldd "$COMP_BIN" 2>&1 | grep -q 'not found'; then
    echo "[initramfs] ERROR: compositor has an unresolved runtime library" >&2
    ldd "$COMP_BIN" >&2 || true
    exit 1
fi
[ -d /usr/share/libinput ] || {
    echo "[initramfs] ERROR: libinput quirks database is missing" >&2
    exit 1
}
cp "$COMP_BIN" "$STAGE/bin/fifi-compositor"
ldd "$COMP_BIN" 2>/dev/null | awk '/=>/{print $3}' | while read -r lib; do
    [ -f "$lib" ] || continue
    cp -L "$lib" "$STAGE/usr/lib/$(basename "$lib")"
done
mkdir -p "$STAGE/usr/share"
cp -a /usr/share/libinput "$STAGE/usr/share/"
echo "[initramfs] included fifi-compositor with libinput runtime"

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

echo "[initramfs] building fifi-appstore..."
(cd "$REPO_ROOT/fifi/apps/appstore" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/appstore/fifi-appstore" "$STAGE/bin/"
    echo "[initramfs] included fifi-appstore"
} || echo "[initramfs] WARNING: fifi-appstore build failed"

echo "[initramfs] building fifi-aichat..."
(cd "$REPO_ROOT/fifi/apps/aichat" && make -s) && {
    cp "$REPO_ROOT/fifi/apps/aichat/fifi-aichat" "$STAGE/bin/"
    echo "[initramfs] included fifi-aichat"
} || echo "[initramfs] WARNING: fifi-aichat build failed"

# ── Offline AI runtime (llama.cpp) — the `ai` command / installed model runs on it ──
# Prebuilt once by scripts/build-llama.sh into build-linux/llama/ (CPU, portable).
# Optional: if absent the image still builds, but AI models won't run.
LLAMA_CLI="$REPO_ROOT/build-linux/llama/llama-cli"
if [ -x "$LLAMA_CLI" ]; then
    echo "[initramfs] bundling llama.cpp runtime (llama-cli)..."
    cp "$LLAMA_CLI" "$STAGE/usr/bin/llama-cli"
    chmod +x "$STAGE/usr/bin/llama-cli"
    ldd "$LLAMA_CLI" 2>/dev/null | grep '=>' | awk '{print $3}' | while read -r lib; do
        [ -f "$lib" ] || continue
        dest="$STAGE/usr/lib/$(basename "$lib")"
        [ -f "$dest" ] || cp "$lib" "$dest" 2>/dev/null || true
    done
    echo "[initramfs] included llama-cli ($(du -h "$LLAMA_CLI" | cut -f1))"
    # Resident server (keeps the model warm across turns; used by fifi-agent and
    # the GUI AI chat app via fifi-ai-serve, loopback-only).
    LLAMA_SRV="$REPO_ROOT/build-linux/llama/llama-server"
    if [ -x "$LLAMA_SRV" ]; then
        cp "$LLAMA_SRV" "$STAGE/usr/bin/llama-server"; chmod +x "$STAGE/usr/bin/llama-server"
        ldd "$LLAMA_SRV" 2>/dev/null | grep '=>' | awk '{print $3}' | while read -r lib; do
            [ -f "$lib" ] || continue
            dest="$STAGE/usr/lib/$(basename "$lib")"
            [ -f "$dest" ] || cp "$lib" "$dest" 2>/dev/null || true
        done
        echo "[initramfs] included llama-server ($(du -h "$LLAMA_SRV" | cut -f1))"
    fi
else
    echo "[initramfs] NOTE: build-linux/llama/llama-cli not found — run scripts/build-llama.sh for offline AI"
fi

# ── App-support libraries (baked so downloaded apps run on a FRESH install) ──
# Chromium/Electron apps (Discord, etc.) expect NSS/NSPR from the system. These
# were previously only hand-copied to /fifi-data on the test box; bake them (plus
# each library's ldd closure) into the image so a clean install works out of box.
echo "[initramfs] bundling app-support libraries (NSS/NSPR)..."
for lib in libnss3 libnssutil3 libsmime3 libssl3 libnspr4 libplc4 libplds4 \
           libnssckbi libsoftokn3 libfreebl3 libsqlite3 libibus-1.0; do
    for f in /usr/lib/$lib.so*; do
        [ -e "$f" ] || continue
        cp -a "$f" "$STAGE/usr/lib/" 2>/dev/null || true
        real=$(readlink -f "$f" 2>/dev/null)
        [ -n "$real" ] && ldd "$real" 2>/dev/null | grep '=>' | awk '{print $3}' | while read -r dep; do
            [ -f "$dep" ] || continue
            dest="$STAGE/usr/lib/$(basename "$dep")"
            [ -f "$dest" ] || cp "$dep" "$dest" 2>/dev/null || true
        done
    done
done
echo "[initramfs] app-support libraries bundled"

# ── GUI runtime libraries (GTK3 / cairo / X11) ───────────────────────────────
# Apps like LibreWolf and LibreOffice expect the GTK3/cairo/X11 stack FROM the
# system (they don't bundle it), so a from-scratch image can't launch them.
# Bake each library + its full ldd closure into /usr/lib (on the default loader
# search path, so no seeding needed). ~80 libs / ~84MB; negligible RAM on any
# real machine. Verified on hardware: without these, LibreWolf fails on
# libgtk-3.so.0 and LibreOffice on libcairo.so.2 / libX11-xcb.so.1.
echo "[initramfs] bundling GUI runtime libraries (GTK3/cairo/X11)..."
for lib in libgtk-3.so.0 libgdk-3.so.0 libcairo.so.2 libcairo-gobject.so.2 \
           libpango-1.0.so.0 libpangocairo-1.0.so.0 libpangoft2-1.0.so.0 \
           libgdk_pixbuf-2.0.so.0 libatk-1.0.so.0 libatk-bridge-2.0.so.0 \
           libgtk-x11-2.0.so.0 libgdk-x11-2.0.so.0 libX11-xcb.so.1 \
           libcups.so.2 libdbus-glib-1.so.2; do
    for base in /usr/lib /usr/lib64; do
        f="$base/$lib"; [ -e "$f" ] || continue
        cp -nL "$f" "$STAGE/usr/lib/$lib" 2>/dev/null || true
        ldd "$f" 2>/dev/null | awk '/=>/{print $3}' | while read -r d; do
            [ -f "$d" ] || continue
            dest="$STAGE/usr/lib/$(basename "$d")"
            [ -f "$dest" ] || cp -L "$d" "$dest" 2>/dev/null || true
        done
    done
done
echo "[initramfs] GUI runtime libraries bundled"

# ── Disk installer tools (parted, mkfs.ext4, mkfs.fat, blkid, grub-install) ──
echo "[initramfs] bundling disk installer tools..."
cp "$STAGE/bin/fifi-install.sh" "$STAGE/bin/fifi-install.sh" 2>/dev/null || true  # already staged above
mkdir -p "$STAGE/usr/lib"
for tool in parted mkfs.ext4 mkfs.fat blkid; do
    bin="$(command -v "$tool" 2>/dev/null || true)"
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
    fi
done
# grub-install + modules
GRUB_INSTALL="$(command -v grub-install 2>/dev/null || true)"
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
GRUB_MKIMAGE="$(command -v grub-mkimage 2>/dev/null || true)"
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
# grub-editenv creates the fixed-size environment block used to remember a
# pending slot attempt across reboots without writing to the data filesystem.
GRUB_EDITENV="$(command -v grub-editenv 2>/dev/null || true)"
if [ -x "$GRUB_EDITENV" ]; then
    cp "$GRUB_EDITENV" "$STAGE/bin/grub-editenv"
    ldd "$GRUB_EDITENV" 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
        [ -f "$lib" ] || continue
        dest="$STAGE/usr/lib/$(basename "$lib")"
        [ -f "$dest" ] || cp "$lib" "$dest"
    done
else
    echo "[initramfs] WARNING: grub-editenv not found -- automatic fallback installation will fail"
fi
# efibootmgr — needed to register boot entry in UEFI NVRAM
EFIBOOTMGR="$(command -v efibootmgr 2>/dev/null || true)"
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
    # Also into the seed source: the mounted data partition shadows the baked
    # /fifi-data on an installed system, so /init re-seeds .psf from here (the App
    # Store loads ter16b.psf; missing it = blank text).
    mkdir -p "$STAGE/usr/share/fifi/fonts"
    cp "$FONT_SRC"/*.psf "$STAGE/usr/share/fifi/fonts/" 2>/dev/null || true
    echo "[initramfs] included fonts from $FONT_SRC"
fi

# Bundle the C.UTF-8 locale. The image's glibc ships no locale files and has no
# built-in C.UTF-8, so bash/programs print "setlocale: LC_ALL: cannot change
# locale (C.UTF-8)". Compile it into the archive the runtime glibc reads (same
# glibc as the host, so the archive is compatible). Without this the terminal
# shows a locale warning at every prompt.
if command -v localedef >/dev/null 2>&1; then
    mkdir -p "$STAGE/usr/lib/locale"
    if localedef --prefix="$STAGE" -i C -f UTF-8 C.UTF-8 2>/dev/null; then
        echo "[initramfs] generated C.UTF-8 locale ($(du -h "$STAGE/usr/lib/locale/locale-archive" 2>/dev/null | cut -f1))"
    else
        echo "[initramfs] WARNING: localedef failed — terminal may warn about locale"
    fi
else
    echo "[initramfs] WARNING: localedef not found — terminal may warn about locale"
fi

# Scalable TTF/OTF fonts for the Settings font picker. gui_font_scan() reads
# /fonts (-> /fifi-data/fonts); we bake a broad set into /usr/share/fifi/fonts
# and /init seeds them onto the data partition on first boot. The set spans the
# platform "looks" (Windows via Liberation/Carlito/Caladea metric-compat,
# Android via Roboto, macOS/iOS via Inter, Linux via DejaVu/Ubuntu/Cantarell)
# plus the Noto CHARACTER-COVERAGE fonts the compositor's fallback chain uses
# (font_ttf.c fb_load_all): NotoSans / NotoSansSymbols2 / NotoSansCJK so any
# script/CJK renders even when the chosen UI font lacks the glyph.
FIFI_FONT_DST="$STAGE/usr/share/fifi/fonts"
mkdir -p "$FIFI_FONT_DST"
for fdir in /usr/share/fonts/TTF \
            /usr/share/fonts/liberation \
            /usr/share/fonts/carlito /usr/share/fonts/caladea \
            /usr/share/fonts/roboto /usr/share/fonts/ubuntu \
            /usr/share/fonts/inter /usr/share/fonts/cantarell; do
    [ -d "$fdir" ] || continue
    # Match both lower- and upper-case extensions: the MS core fonts in
    # /usr/share/fonts/TTF ship as Arial.TTF / Times.TTF / Verdana.TTF etc.
    # (uppercase .TTF), so a lowercase-only glob silently skipped every
    # recognizable Windows family — the font picker looked half-empty.
    for f in "$fdir"/*.ttf "$fdir"/*.otf "$fdir"/*.ttc \
             "$fdir"/*.TTF "$fdir"/*.OTF "$fdir"/*.TTC; do
        [ -f "$f" ] || continue
        cp -n "$f" "$FIFI_FONT_DST/" 2>/dev/null || true
    done
done
# Core Noto families + the fallback coverage set. NotoSansCJK-Regular.ttc is
# ~20MB but is what makes CJK visible system-wide; keep it. The fallback loader
# looks for these exact names under /fonts, so preserve them.
for nf in /usr/share/fonts/noto/NotoSans-Regular.ttf \
          /usr/share/fonts/noto/NotoSerif-Regular.ttf \
          /usr/share/fonts/noto/NotoSansMono-Regular.ttf \
          /usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf \
          /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc; do
    [ -f "$nf" ] && cp -n "$nf" "$FIFI_FONT_DST/" 2>/dev/null || true
done
# Monochrome emoji (outline glyphs stb_truetype can rasterize) if present —
# Noto COLOR Emoji is CBDT/bitmap and won't render, so only take a mono build.
for ef in /usr/share/fonts/noto/NotoEmoji-Regular.ttf \
          /usr/share/fonts/TTF/NotoEmoji-Regular.ttf; do
    [ -f "$ef" ] && cp -n "$ef" "$FIFI_FONT_DST/" 2>/dev/null || true
done
_nfonts=$(ls "$FIFI_FONT_DST" 2>/dev/null | wc -l)
echo "[initramfs] bundled $_nfonts scalable fonts ($(du -sh "$FIFI_FONT_DST" 2>/dev/null | cut -f1)) for the font picker + coverage fallback"

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

if [ -x /usr/bin/pipewire ] && [ -x /usr/bin/pipewire-pulse ]; then
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
else
    echo "[initramfs] WARNING: PipeWire not found -- audio support disabled"
fi

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

    # Helper: copy a lib + resolve its symlink chain into $STAGE/usr/lib.
    stage_lib() {
        for _l in "$@"; do
            for _p in /usr/lib/"$_l" /usr/lib64/"$_l"; do
                [ -e "$_p" ] || continue
                _r=$(realpath "$_p" 2>/dev/null) || continue
                [ -f "$_r" ] || continue
                [ -f "$STAGE/usr/lib/$(basename "$_r")" ] || cp "$_r" "$STAGE/usr/lib/"
                [ -e "$STAGE/usr/lib/$_l" ] || ln -sf "$(basename "$_r")" "$STAGE/usr/lib/$_l"
                break
            done
        done
    }

    # xkbcomp: Xwayland EXECs it by absolute path to build the keymap (it is not
    # a linked dependency, so ldd above missed it). Without it Xwayland aborts
    # with "Failed to activate virtual core keyboard".
    if [ -x /usr/bin/xkbcomp ]; then
        cp /usr/bin/xkbcomp "$STAGE/usr/bin/xkbcomp"
        for lib in $(ldd /usr/bin/xkbcomp 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/"); do
            stage_lib "$(basename "$lib")"
        done
    fi
    # xkeyboard-config data (Xwayland -xkbdir) — copy the real dir X11/xkb points to.
    if [ -d /usr/share/X11/xkb ]; then
        mkdir -p "$STAGE/usr/share/X11"
        cp -aL /usr/share/X11/xkb "$STAGE/usr/share/X11/xkb" 2>/dev/null || true
    fi
    # glamor dlopen()s libEGL/libGLdispatch at startup (also not in ldd); without
    # them Xwayland aborts before falling back to software rendering.
    stage_lib libEGL.so.1 libGLdispatch.so.0 libgbm.so.1
    # X11 client libraries used by X11-only apps (LibreOffice's "gen" VCL).
    stage_lib libXext.so.6 libSM.so.6 libICE.so.6 libXrender.so.1 libXrandr.so.2 \
              libXi.so.6 libXinerama.so.1 libXcursor.so.1 libXfixes.so.3 \
              libXdamage.so.1 libXcomposite.so.1 libXtst.so.6 libX11.so.6 \
              libxcb.so.1 libXau.so.6 libXdmcp.so.6 libxkbfile.so.1
    echo "[initramfs] X11 support: xkbcomp + xkb data + EGL/X client libs bundled"
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

# Realtek PCIe Ethernet firmware. rtl8169 requests this raw path on the target
# laptop; Arch stores it compressed, while this kernel expects it decompressed.
RTL_NIC_SRC=""
for candidate in \
    /usr/lib/firmware/rtl_nic/rtl8168h-2.fw.zst \
    /lib/firmware/rtl_nic/rtl8168h-2.fw.zst \
    /usr/lib/firmware/rtl_nic/rtl8168h-2.fw \
    /lib/firmware/rtl_nic/rtl8168h-2.fw; do
    [ -f "$candidate" ] && RTL_NIC_SRC="$candidate" && break
done
if [ -n "$RTL_NIC_SRC" ]; then
    mkdir -p "$STAGE/lib/firmware/rtl_nic"
    case "$RTL_NIC_SRC" in
        *.zst)
            zstd -d -q "$RTL_NIC_SRC" \
                -o "$STAGE/lib/firmware/rtl_nic/rtl8168h-2.fw"
            ;;
        *)
            cp "$RTL_NIC_SRC" "$STAGE/lib/firmware/rtl_nic/rtl8168h-2.fw"
            ;;
    esac
    echo "[initramfs] Realtek Ethernet firmware bundled"
else
    echo "[initramfs] NOTE: rtl8168h-2 firmware not found -- install linux-firmware-realtek"
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

# ── GPU: i915 firmware (GuC/HuC/DMC) + Mesa OpenGL/Vulkan userspace ──────────
# Without GuC firmware the kernel declares modern Intel iGPUs WEDGED at boot:
# the display still works (modeset needs no firmware) but every render engine
# is dead, so glamor/GLX/Vulkan all fail. Bundle the whole i915 firmware
# family so FiFi lights up any Intel iGPU, decompressed (the FiFi kernel has
# no CONFIG_FW_LOADER_COMPRESS, same as the WiFi firmware above).
echo "[initramfs] bundling GPU stack (i915 firmware + Mesa GL/Vulkan)..."
if [ -d /usr/lib/firmware/i915 ]; then
    mkdir -p "$STAGE/lib/firmware/i915"
    for fw in /usr/lib/firmware/i915/*; do
        bn=$(basename "$fw")
        case "$bn" in
            *.zst) [ -f "$STAGE/lib/firmware/i915/${bn%.zst}" ] || \
                       zstd -d -q "$fw" -o "$STAGE/lib/firmware/i915/${bn%.zst}" 2>/dev/null || true ;;
            *)     [ -f "$STAGE/lib/firmware/i915/$bn" ] || cp "$fw" "$STAGE/lib/firmware/i915/$bn" ;;
        esac
    done
    echo "[initramfs] i915 firmware bundled ($(du -sh "$STAGE/lib/firmware/i915" | cut -f1))"
else
    echo "[initramfs] NOTE: i915 firmware not found -- install linux-firmware-intel"
fi

# Mesa: GLX/EGL vendor libs behind glvnd, the DRI shim + gallium driver (iris
# for Intel hardware, swrast/zink fallbacks), GBM, and the Vulkan loader +
# Intel anv driver, each with its full ldd closure (libLLVM and friends).
# Everything goes to the loader/Mesa default search paths: /usr/lib,
# /usr/lib/dri, /usr/share/vulkan/icd.d, /usr/share/glvnd/egl_vendor.d.
mkdir -p "$STAGE/usr/lib/dri" "$STAGE/usr/share/vulkan/icd.d" "$STAGE/usr/share/glvnd/egl_vendor.d"
MESA_SEEDS="/usr/lib/libGLX_mesa.so.0 /usr/lib/libEGL_mesa.so.0 /usr/lib/libgbm.so.1 \
    /usr/lib/libvulkan.so.1 /usr/lib/libvulkan_intel.so"
for d in /usr/lib/dri/*_dri.so /usr/lib/dri/libdril_dri.so /usr/lib/libgallium*.so; do
    [ -e "$d" ] && MESA_SEEDS="$MESA_SEEDS $d"
done
for seed in $MESA_SEEDS; do
    [ -e "$seed" ] || continue
    real=$(realpath "$seed" 2>/dev/null) || continue
    [ -f "$real" ] || continue
    case "$real" in
        */dri/*) dest_dir="$STAGE/usr/lib/dri" ;;
        *)       dest_dir="$STAGE/usr/lib" ;;
    esac
    [ -f "$dest_dir/$(basename "$real")" ] || cp "$real" "$dest_dir/"
    case "$seed" in
        */dri/*) link_dir="$STAGE/usr/lib/dri" ;;
        *)       link_dir="$STAGE/usr/lib" ;;
    esac
    [ -e "$link_dir/$(basename "$seed")" ] || \
        ln -sf "$(basename "$real")" "$link_dir/$(basename "$seed")"
    # ldd prints the full transitive closure, one level is enough
    for lib in $(ldd "$real" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/"); do
        lreal=$(realpath "$lib" 2>/dev/null) || continue
        [ -f "$lreal" ] || continue
        [ -f "$STAGE/usr/lib/$(basename "$lreal")" ] || cp "$lreal" "$STAGE/usr/lib/"
        [ -e "$STAGE/usr/lib/$(basename "$lib")" ] || \
            ln -sf "$(basename "$lreal")" "$STAGE/usr/lib/$(basename "$lib")"
    done
done
# Mesa 26 GBM backend: libgbm dlopens /usr/lib/gbm/dri_gbm.so — without it
# "couldn't create gbm device" and XWayland glamor falls back to llvmpipe.
if [ -f /usr/lib/gbm/dri_gbm.so ]; then
    mkdir -p "$STAGE/usr/lib/gbm"
    cp /usr/lib/gbm/dri_gbm.so "$STAGE/usr/lib/gbm/"
    for lib in $(ldd /usr/lib/gbm/dri_gbm.so 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/"); do
        lreal=$(realpath "$lib" 2>/dev/null) || continue
        [ -f "$lreal" ] || continue
        [ -f "$STAGE/usr/lib/$(basename "$lreal")" ] || cp "$lreal" "$STAGE/usr/lib/"
        [ -e "$STAGE/usr/lib/$(basename "$lib")" ] || \
            ln -sf "$(basename "$lreal")" "$STAGE/usr/lib/$(basename "$lib")"
    done
fi
# Vulkan ICD + glvnd EGL vendor manifests (hasvk = ancient Gen7/8, skip it)
cp /usr/share/vulkan/icd.d/intel_icd*.json "$STAGE/usr/share/vulkan/icd.d/" 2>/dev/null || true
rm -f "$STAGE/usr/share/vulkan/icd.d/"intel_hasvk* 2>/dev/null
cp /usr/share/glvnd/egl_vendor.d/50_mesa.json "$STAGE/usr/share/glvnd/egl_vendor.d/" 2>/dev/null || true
# GPU diagnostics (vulkaninfo/glxinfo/eglinfo) — small, invaluable on-box
for tool in vulkaninfo glxinfo eglinfo; do
    [ -x "/usr/bin/$tool" ] || continue
    cp "/usr/bin/$tool" "$STAGE/usr/bin/$tool"
    for lib in $(ldd "/usr/bin/$tool" 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/"); do
        lreal=$(realpath "$lib" 2>/dev/null) || continue
        [ -f "$lreal" ] || continue
        [ -f "$STAGE/usr/lib/$(basename "$lreal")" ] || cp "$lreal" "$STAGE/usr/lib/"
        [ -e "$STAGE/usr/lib/$(basename "$lib")" ] || \
            ln -sf "$(basename "$lreal")" "$STAGE/usr/lib/$(basename "$lib")"
    done
done
echo "[initramfs] Mesa GL/Vulkan bundled ($(du -sh "$STAGE/usr/lib" | cut -f1) total usr/lib)"

# ── Bluetooth: BlueZ (bluetoothd + bluetoothctl) over a root-mode system D-Bus;
#    A2DP audio via PipeWire's bluez5 plugin. Kernel BT is enabled in fifi.config.
#    Untested without BT hardware — see fifi-pre-install-test-pass memory. ───────
echo "[initramfs] bundling Bluetooth (BlueZ + D-Bus + firmware)..."
mkdir -p "$STAGE/usr/lib/bluetooth" "$STAGE/usr/lib/spa-0.2/bluez5" \
         "$STAGE/etc/bluetooth" "$STAGE/var/lib/bluetooth" \
         "$STAGE/usr/share/dbus-1/system.d" "$STAGE/run/dbus"
[ -x /usr/lib/bluetooth/bluetoothd ] && cp /usr/lib/bluetooth/bluetoothd "$STAGE/usr/lib/bluetooth/"
for b in bluetoothctl dbus-daemon dbus-uuidgen; do
    src="$(command -v "$b" 2>/dev/null || true)"
    [ -x "$src" ] && cp "$src" "$STAGE/usr/bin/" || true
done
cp /usr/lib/spa-0.2/bluez5/*.so "$STAGE/usr/lib/spa-0.2/bluez5/" 2>/dev/null || true
# D-Bus system bus config: the MAIN system.conf is REQUIRED or `dbus-daemon --system`
# refuses to start (no socket -> bluetoothd can't run). Copy it plus every system.d
# policy (the BlueZ policy is `bluetooth.conf` on current BlueZ, NOT `org.bluez.conf`)
# and the bluez activation service file.
mkdir -p "$STAGE/usr/share/dbus-1/system-services"
cp /usr/share/dbus-1/system.conf "$STAGE/usr/share/dbus-1/system.conf" 2>/dev/null || true
cp /usr/share/dbus-1/system.d/*.conf "$STAGE/usr/share/dbus-1/system.d/" 2>/dev/null || true
cp /usr/share/dbus-1/system-services/org.bluez.service "$STAGE/usr/share/dbus-1/system-services/" 2>/dev/null || true
[ -f /etc/bluetooth/main.conf ] && cp /etc/bluetooth/main.conf "$STAGE/etc/bluetooth/" 2>/dev/null || true
# dbus-daemon --system drops privileges to the `dbus` user; the minimal rootfs has
# no such account, so the bus aborts ("Could not get UID/GID for username dbus").
# The source rootfs has NO /etc/passwd (init creates a root-only one at boot, but
# only if absent), so seed root FIRST here, then append dbus — otherwise we'd bake
# a passwd with dbus but no root and init would leave it (breaking SSH/root).
mkdir -p "$STAGE/etc"
[ -s "$STAGE/etc/passwd" ] || echo 'root:x:0:0:root:/root:/bin/sh' > "$STAGE/etc/passwd"
[ -s "$STAGE/etc/group" ]  || echo 'root:x:0:' > "$STAGE/etc/group"
grep -q '^fifi:' "$STAGE/etc/passwd" ||
    echo 'fifi:x:1000:1000:FiFi Desktop:/fifi-data/home:/bin/sh' >> "$STAGE/etc/passwd"
grep -q '^fifi:' "$STAGE/etc/group" ||
    echo 'fifi:x:1000:' >> "$STAGE/etc/group"
grep -q "^dbus:" "$STAGE/etc/passwd" 2>/dev/null || echo "dbus:x:81:81:System Message Bus:/:/sbin/nologin" >> "$STAGE/etc/passwd"
grep -q "^dbus:" "$STAGE/etc/group"  2>/dev/null || echo "dbus:x:81:" >> "$STAGE/etc/group"
# A machine-id is required by D-Bus/BlueZ; seed a static one (init regenerates if empty).
mkdir -p "$STAGE/var/lib/dbus"
if [ ! -s "$STAGE/etc/machine-id" ]; then
    dbus-uuidgen 2>/dev/null > "$STAGE/etc/machine-id" || true
fi
cp "$STAGE/etc/machine-id" "$STAGE/var/lib/dbus/machine-id" 2>/dev/null || true
{
    ldd /usr/lib/bluetooth/bluetoothd
    ldd "$(command -v bluetoothctl)"
    ldd "$(command -v dbus-daemon)"
    for f in /usr/lib/spa-0.2/bluez5/*.so; do [ -f "$f" ] && ldd "$f"; done
} 2>/dev/null | grep "=>" | awk '{print $3}' | grep "^/" | sort -u | while read -r lib; do
    real=$(realpath "$lib" 2>/dev/null) || continue
    [ -f "$real" ] || continue
    dest="$STAGE/usr/lib/$(basename "$real")"
    [ -f "$dest" ] || cp "$real" "$dest" 2>/dev/null || true
    ln="$STAGE/usr/lib/$(basename "$lib")"
    [ -e "$ln" ] || ln -sf "$(basename "$real")" "$ln" 2>/dev/null || true
done || true
# BT controller firmware (Intel ibt-*, Realtek rtl_bt, Qualcomm qca); decompress
# .zst since the kernel firmware loader expects raw files.
mkdir -p "$STAGE/lib/firmware/intel" "$STAGE/lib/firmware/rtl_bt" "$STAGE/lib/firmware/qca"
for fw in /lib/firmware/intel/ibt-* /lib/firmware/rtl_bt/* /lib/firmware/qca/*; do
    [ -f "$fw" ] || continue
    rel="${fw#/lib/firmware/}"; dest="$STAGE/lib/firmware/$rel"
    case "$fw" in
        *.zst) dest="${dest%.zst}"; zstd -dqf "$fw" -o "$dest" 2>/dev/null || true ;;
        *)     cp "$fw" "$dest" 2>/dev/null || true ;;
    esac
done
echo "[initramfs] Bluetooth stack bundled"

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
CURL_BIN="$(command -v curl 2>/dev/null || true)"
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

# OpenSSL verifies the Ed25519 signature on every OS update manifest before any
# downloaded kernel or initramfs is trusted.
OPENSSL_BIN="$(command -v openssl 2>/dev/null || true)"
if [ -x "$OPENSSL_BIN" ]; then
    cp "$OPENSSL_BIN" "$STAGE/bin/openssl"
    ldd "$OPENSSL_BIN" 2>/dev/null | grep '=>' | awk '{print $3}' | while read lib; do
        [ -f "$lib" ] || continue
        dest="$STAGE/usr/lib/$(basename "$lib")"
        [ -f "$dest" ] || cp "$lib" "$dest"
    done
    cp "$REPO_ROOT/security/release-signing-public.pem" \
        "$STAGE/etc/fifi-release-signing.pub"
    chmod 0644 "$STAGE/etc/fifi-release-signing.pub"
    echo "[initramfs] release signature verifier bundled"
else
    echo "[initramfs] ERROR: openssl not found -- secure updates cannot be built" >&2
    exit 1
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

# Remove the legacy developer-key location and reject any other SSH credentials
# before packing. This also catches ignored local files copied into the stage.
bash "$REPO_ROOT/scripts/sanitize-initramfs-stage.sh" "$STAGE"

# Minimal /dev nodes (devtmpfs fills the rest at runtime)
mkdir -p "$STAGE/dev"
mknod -m 600 "$STAGE/dev/console" c 5 1 2>/dev/null || true
mknod -m 666 "$STAGE/dev/null"    c 1 3 2>/dev/null || true
mknod -m 666 "$STAGE/dev/tty"     c 5 0 2>/dev/null || true

# ── Pack into cpio.gz ─────────────────────────────────────────────────────────
# -R 0:0 forces every file to be owned by root in the archive. Without it the
# archive keeps the build user's uid/gid (e.g. 1000), and dropbear then refuses
# SSH because /root is not owned by root ("insecure" home dir, no error logged).
(cd "$STAGE" && find . | cpio -H newc -o -R 0:0 2>/dev/null | gzip -9) > "$OUT_FILE"

SIZE=$(du -sh "$OUT_FILE" | cut -f1)
echo "[initramfs] Done. $OUT_FILE ($SIZE)"
