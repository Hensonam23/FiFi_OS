#!/bin/sh
# FiFi OS disk installer
#
# Usage: fifi-install.sh <target> <browser> <software>
#   target:   whole disk  e.g. /dev/sda  (erased and repartitioned)
#             OR partition e.g. /dev/sda3 (formatted in place, existing EFI reused)
#   browser:  librewolf | firefox
#   software: libreoffice | none
#
# Progress is reported via stdout as "PROGRESS:N" (0-100).
# All other stdout/stderr lines are logged to the installer UI.
#
# Sections:
#   1. Helpers and setup
#   2. Detect mode (whole disk vs partition)
#   3. Validation
#   4. Whole-disk mode: partition and format
#   5. Partition mode: format in place + find EFI
#   6. Mount
#   7. Bootloader (GRUB + kernel)
#   8. Browser download
#   9. LibreOffice download
#  10. Cleanup and finish

set -e

# ── 1. Helpers and setup ───────────────────────────────────────────────────────

TARGET="$1"
BROWSER="$2"
SOFTWARE="$3"

MNT_EFI="/mnt/fifi-install-efi"
MNT_DATA="/mnt/fifi-install-data"
DEBUGLOG="/tmp/fifi-install-debug.log"
: > "$DEBUGLOG"   # create/clear log file

# sh-compatible logging: write to stdout AND log file
log() {
    echo "[fifi-install] $*"
    echo "[fifi-install] $*" >> "$DEBUGLOG"
}
prog() { echo "PROGRESS:$1"; }
fail() { log "FATAL: $*"; _save_debug_log; exit 1; }

_save_debug_log() {
    # Always dump log to stdout — visible in installer progress pane
    echo "=== INSTALL LOG ==="
    cat "$DEBUGLOG" 2>/dev/null
    echo "=== END ==="
    # Save to installed partitions
    [ -d "$MNT_EFI"  ] && cp "$DEBUGLOG" "$MNT_EFI/fifi-install-debug.txt"  2>/dev/null || true
    [ -d "$MNT_DATA" ] && cp "$DEBUGLOG" "$MNT_DATA/install-debug.log"       2>/dev/null || true
    # Save to live USB — scan every block device for FIFIOS label
    USB_DEV=""
    for _dev in /dev/sd?1 /dev/sd?? /dev/sd? /dev/mmcblk?p1 /dev/vd?1; do
        [ -b "$_dev" ] || continue
        _lbl="$(blkid -s LABEL -o value "$_dev" 2>/dev/null)"
        if [ "$_lbl" = "FIFIOS" ]; then USB_DEV="$_dev"; break; fi
    done
    if [ -n "$USB_DEV" ]; then
        mkdir -p /mnt/fifi-usb-log
        if mount -o rw "$USB_DEV" /mnt/fifi-usb-log 2>/dev/null; then
            cp "$DEBUGLOG" /mnt/fifi-usb-log/fifi-install-debug.txt 2>/dev/null || true
            sync
            umount /mnt/fifi-usb-log 2>/dev/null || true
        fi
        rmdir /mnt/fifi-usb-log 2>/dev/null || true
    fi
}

# Log a command's output to both stdout and the log file (sh-compatible)
logcmd() {
    "$@" 2>&1 | while IFS= read -r line; do
        echo "  $line"
        echo "  $line" >> "$DEBUGLOG"
    done
    return "${PIPESTATUS:-0}"
}

cleanup() {
    _save_debug_log
    umount "$MNT_DATA" 2>/dev/null || true
    umount "$MNT_EFI"  2>/dev/null || true
    rmdir  "$MNT_DATA" "$MNT_EFI" 2>/dev/null || true
}
trap cleanup EXIT

# ── 2. Detect mode ─────────────────────────────────────────────────────────────

prog 0
log "Starting FiFi OS installation"
log "Target: $TARGET"
log "Browser: $BROWSER"
log "Software: $SOFTWARE"

[ -n "$TARGET" ] || fail "No target specified"
[ -b "$TARGET" ] || fail "$TARGET is not a block device"

# Detect whether target is a whole disk or a partition
# A partition has a "partition" file in sysfs
DEV_NAME="$(basename "$TARGET")"
IS_PART=0
if [ -f "/sys/class/block/$DEV_NAME/partition" ]; then
    IS_PART=1
    log "Mode: partition install (format in place, reuse existing EFI)"
else
    IS_PART=0
    log "Mode: whole disk install (erase and repartition)"
fi

# ── 3. Validation ──────────────────────────────────────────────────────────────

# Refuse to install to the USB we booted from
BOOT_DISK=""
for dev in /dev/disk/by-label/FIFIOS /dev/disk/by-label/FiFiOS; do
    [ -b "$dev" ] && BOOT_DISK="$(readlink -f "$dev" 2>/dev/null)" && break
done
if [ -n "$BOOT_DISK" ]; then
    BOOT_BASE="$(echo "$BOOT_DISK" | sed 's/p\?[0-9]*$//')"
    [ "$BOOT_BASE" = "$TARGET" ] && fail "Cannot install to the boot device"
    echo "$TARGET" | grep -q "^$BOOT_BASE" && fail "Cannot install to the boot device"
fi

for tool in mkfs.ext4 grub-install blkid; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool not found"
done
command -v curl >/dev/null 2>&1 || fail "curl not found — cannot download software"

# Find kernel and initramfs.
# Kernel is embedded in the initramfs at /boot/bzImage (added by build-initramfs.sh).
# Initramfs must be found by probing partitions for the boot/initramfs.cpio.gz file.
KERNEL_SRC=""
INITRD_SRC=""
USB_MNT_SRC=""

# Kernel: always embedded in running initramfs
[ -f "/boot/bzImage" ] && KERNEL_SRC="/boot/bzImage"
[ -z "$KERNEL_SRC"  ] && fail "Kernel not found at /boot/bzImage — initramfs was not built correctly"

# Initramfs: probe every FAT/vfat partition for boot/initramfs.cpio.gz
log "Searching for initramfs on all partitions..."
for _dev in /dev/sda1 /dev/sda /dev/sdb1 /dev/sdb /dev/sdc1 /dev/sdc \
            /dev/sdd1 /dev/sde1 /dev/mmcblk0p1 /dev/vda1; do
    [ -b "$_dev" ] || continue
    _probe_mnt="/mnt/fifi-probe"
    mkdir -p "$_probe_mnt"
    if mount -o ro "$_dev" "$_probe_mnt" 2>/dev/null; then
        if [ -f "$_probe_mnt/boot/initramfs.cpio.gz" ]; then
            log "Found initramfs on $_dev"
            INITRD_SRC="$_probe_mnt/boot/initramfs.cpio.gz"
            USB_MNT_SRC="$_probe_mnt"
            break
        fi
        umount "$_probe_mnt" 2>/dev/null || true
    fi
    rmdir "$_probe_mnt" 2>/dev/null || true
done

[ -n "$INITRD_SRC" ] || fail "Cannot find initramfs.cpio.gz on any partition"

log "Kernel:    $KERNEL_SRC  ($(du -sh "$KERNEL_SRC" 2>/dev/null | cut -f1))"
log "Initramfs: $INITRD_SRC  ($(du -sh "$INITRD_SRC" 2>/dev/null | cut -f1))"
prog 3

# ── 4. Whole-disk mode: partition and format ───────────────────────────────────

if [ "$IS_PART" = "0" ]; then
    command -v parted >/dev/null 2>&1 || fail "parted not found"
    command -v mkfs.fat >/dev/null 2>&1 || fail "mkfs.fat not found"

    log "Unmounting any existing partitions on $TARGET..."
    for part in "${TARGET}"[0-9]* "${TARGET}"p[0-9]*; do
        [ -b "$part" ] && umount "$part" 2>/dev/null || true
    done
    sleep 1

    log "Partitioning $TARGET (GPT: 512MB EFI + rest for data)..."
    parted -s "$TARGET" mklabel gpt
    parted -s "$TARGET" mkpart "EFI"       fat32  1MiB   513MiB
    parted -s "$TARGET" mkpart "FIFI-DATA" ext4   513MiB 100%
    parted -s "$TARGET" set 1 esp on
    partprobe "$TARGET" 2>/dev/null || true
    sleep 2

    # Handle both /dev/sdX1 and /dev/nvme0n1p1 naming
    EFI_PART="${TARGET}1"
    DATA_PART="${TARGET}2"
    [ -b "${TARGET}p1" ] && EFI_PART="${TARGET}p1" && DATA_PART="${TARGET}p2"

    prog 8
    log "Formatting EFI partition ($EFI_PART) as FAT32..."
    mkfs.fat -F32 -n "FIFIOS-EFI" "$EFI_PART"

    log "Formatting data partition ($DATA_PART) as ext4..."
    mkfs.ext4 -L "FIFI-DATA" -F "$DATA_PART"
    prog 12
fi

# ── 5. Partition mode: format in place + find EFI ────────────────────────────

if [ "$IS_PART" = "1" ]; then
    DATA_PART="$TARGET"

    log "Unmounting $TARGET if mounted..."
    umount "$TARGET" 2>/dev/null || true
    sleep 1

    log "Formatting $TARGET as ext4 (FIFI-DATA)..."
    mkfs.ext4 -L "FIFI-DATA" -F "$TARGET"
    prog 8

    # Find the parent disk (strip partition number)
    # e.g. sda3 -> sda, nvme0n1p3 -> nvme0n1
    PARENT_DISK="$(cat "/sys/class/block/$DEV_NAME/../uevent" 2>/dev/null | grep DEVNAME | cut -d= -f2)"
    if [ -z "$PARENT_DISK" ]; then
        # Fallback: strip trailing digits (and optional 'p' prefix for nvme)
        PARENT_DISK="$(echo "$DEV_NAME" | sed 's/p\?[0-9]*$//')"
    fi
    PARENT_DEV="/dev/$PARENT_DISK"
    log "Parent disk: $PARENT_DEV"

    # Find EFI partition by MOUNTING candidates and checking for EFI/ directory.
    # blkid TYPE/LABEL detection doesn't work in this environment, so we probe directly.
    EFI_PART=""
    _efi_probe="/mnt/fifi-efi-probe"
    mkdir -p "$_efi_probe"

    # Build candidate list: all partitions on the parent disk, then all system partitions
    _candidates=""
    for _p in /dev/${PARENT_DISK}p1 /dev/${PARENT_DISK}p2 /dev/${PARENT_DISK}p3 \
              /dev/${PARENT_DISK}1 /dev/${PARENT_DISK}2 /dev/${PARENT_DISK}3 \
              /dev/sda1 /dev/sdb1 /dev/nvme0n1p1 /dev/nvme1n1p1; do
        [ -b "$_p" ] && [ "$_p" != "$TARGET" ] && _candidates="$_candidates $_p"
    done

    log "Probing EFI candidates:$_candidates"
    for _p in $_candidates; do
        [ -b "$_p" ] || continue
        if mount -o ro "$_p" "$_efi_probe" 2>/dev/null; then
            if [ -d "$_efi_probe/EFI" ] || [ -d "$_efi_probe/efi" ]; then
                EFI_PART="$_p"
                log "EFI partition found: $_p (has EFI/ directory)"
                umount "$_efi_probe" 2>/dev/null || true
                break
            fi
            umount "$_efi_probe" 2>/dev/null || true
        fi
    done
    rmdir "$_efi_probe" 2>/dev/null || true

    [ -n "$EFI_PART" ] || fail "Could not find EFI partition (no mountable partition with EFI/ directory found)"
    log "Using EFI partition: $EFI_PART"
    prog 12
fi

# ── 6. Mount ───────────────────────────────────────────────────────────────────

mkdir -p "$MNT_EFI" "$MNT_DATA"

log "Mounting data partition ($DATA_PART)..."
mount "$DATA_PART" "$MNT_DATA"

log "Mounting EFI partition ($EFI_PART)..."
mount "$EFI_PART" "$MNT_EFI"

mkdir -p "$MNT_DATA/browser" "$MNT_DATA/libreoffice" "$MNT_DATA/settings"
prog 15

# ── 7. Bootloader (GRUB + kernel) ─────────────────────────────────────────────

log "Copying kernel and initramfs to EFI partition..."
mkdir -p "$MNT_EFI/boot"
cp "$KERNEL_SRC" "$MNT_EFI/boot/bzImage"
cp "$INITRD_SRC" "$MNT_EFI/boot/initramfs.cpio.gz"
log "Kernel:    $(du -sh "$MNT_EFI/boot/bzImage" 2>/dev/null | cut -f1)"
log "Initramfs: $(du -sh "$MNT_EFI/boot/initramfs.cpio.gz" 2>/dev/null | cut -f1)"
# Unmount the USB source now that we've copied what we need
if [ -n "$USB_MNT_SRC" ]; then
    umount "$USB_MNT_SRC" 2>/dev/null && rmdir "$USB_MNT_SRC" 2>/dev/null || true
    USB_MNT_SRC=""
fi
prog 22

log "Installing GRUB EFI bootloader..."
log "EFI partition: $EFI_PART  mounted at: $MNT_EFI"
log "EFI contents before:"
ls "$MNT_EFI/EFI/" 2>&1 | while IFS= read -r l; do log "  $l"; done

# Run grub-install and capture all output for the debug log
GRUB_OUT="$(grub-install \
    --target=x86_64-efi \
    --efi-directory="$MNT_EFI" \
    --boot-directory="$MNT_EFI/boot" \
    --bootloader-id="FiFiOS" \
    --no-nvram 2>&1)"
GRUB_EXIT=$?
log "grub-install exit=$GRUB_EXIT  output: $GRUB_OUT"

# Hard check: did grub-install actually create the binary?
GRUB_EFI="$MNT_EFI/EFI/FiFiOS/grubx64.efi"
if [ ! -f "$GRUB_EFI" ]; then
    log "grub-install did not create grubx64.efi — trying grub-mkimage fallback"
    mkdir -p "$MNT_EFI/EFI/FiFiOS"
    # Embed a config that searches for grub.cfg by file path (finds NVMe EFI partition)
    printf 'search --file --no-floppy --set=root /boot/grub/grub.cfg\nset prefix=($root)/boot/grub\n' \
        > /tmp/grub_early.cfg
    MKIMG_OUT="$(grub-mkimage \
        -c /tmp/grub_early.cfg \
        -O x86_64-efi \
        -o "$GRUB_EFI" \
        -p /boot/grub \
        part_gpt part_msdos fat ext2 normal boot linux configfile \
        loopback search search_fs_uuid search_fs_file chain efifwsetup \
        gfxterm gfxterm_menu all_video 2>&1)"
    MKIMG_EXIT=$?
    log "grub-mkimage exit=$MKIMG_EXIT  output: $MKIMG_OUT"
fi

if [ ! -f "$GRUB_EFI" ]; then
    fail "Could not create GRUB EFI binary. grub-install and grub-mkimage both failed. Check debug log."
fi
log "GRUB binary ready: $GRUB_EFI ($(ls -lh "$GRUB_EFI" 2>/dev/null | awk '{print $5}'))"

# ── Boot takeover: replace Windows Boot Manager with GRUB ─────────────────────
# UEFI has a "Windows Boot Manager" NVRAM entry pointing to
# EFI/Microsoft/Boot/bootmgfw.efi — put GRUB there so it loads automatically.
# GRUB then shows a menu: FiFi OS (default) + Windows (chainload backup).
WIN_BOOT_DIR="$MNT_EFI/EFI/Microsoft/Boot"
WIN_BOOT_EFI="$WIN_BOOT_DIR/bootmgfw.efi"
WIN_BOOT_BAK="$WIN_BOOT_DIR/bootmgfw_backup.efi"
WIN_CHAINLOAD=""

if [ -f "$WIN_BOOT_EFI" ]; then
    log "Backing up Windows Boot Manager to bootmgfw_backup.efi"
    cp "$WIN_BOOT_EFI" "$WIN_BOOT_BAK"
    cp "$GRUB_EFI" "$WIN_BOOT_EFI"
    WIN_CHAINLOAD="/EFI/Microsoft/Boot/bootmgfw_backup.efi"
    log "Boot takeover complete"
else
    log "Windows Boot Manager not found — installing to EFI/BOOT/BOOTX64.EFI fallback"
    mkdir -p "$MNT_EFI/EFI/BOOT"
    cp "$GRUB_EFI" "$MNT_EFI/EFI/BOOT/BOOTX64.EFI"
fi

# Also try NVRAM registration
mount -t efivarfs efivarfs /sys/firmware/efi/efivars 2>/dev/null || true
EFI_DISK="$(echo "$EFI_PART" | sed 's/p\?[0-9]*$//')"
EFI_PARTNUM="$(cat "/sys/class/block/$(basename "$EFI_PART")/partition" 2>/dev/null)"
if [ -n "$EFI_PARTNUM" ] && command -v efibootmgr >/dev/null 2>&1; then
    OLD="$(efibootmgr 2>/dev/null | grep -i 'FiFi OS' | grep -oE '[0-9A-F]{4}' | head -1)"
    [ -n "$OLD" ] && efibootmgr -b "$OLD" -B 2>/dev/null || true
    EFIBM_OUT="$(efibootmgr --create --disk "$EFI_DISK" --part "$EFI_PARTNUM" \
        --label "FiFi OS" --loader '\EFI\FiFiOS\grubx64.efi' 2>&1)"
    log "efibootmgr: $EFIBM_OUT"
fi

DATA_UUID="$(blkid -s UUID -o value "$DATA_PART" 2>/dev/null)"
[ -n "$DATA_UUID" ] || fail "Could not read UUID of data partition"
log "Data UUID: $DATA_UUID"
prog 30

log "Writing GRUB configuration..."
mkdir -p "$MNT_EFI/boot/grub"
cat > "$MNT_EFI/boot/grub/grub.cfg" << GRUBCFG
set timeout=5
set default=0

menuentry "FiFi OS" {
    linux /boot/bzImage console=tty0 quiet loglevel=3 fifi_data_uuid=$DATA_UUID apparmor=1 security=apparmor
    initrd /boot/initramfs.cpio.gz
}

menuentry "FiFi OS (safe mode)" {
    linux /boot/bzImage console=tty0 quiet loglevel=3 nomodeset fifi_data_uuid=$DATA_UUID
    initrd /boot/initramfs.cpio.gz
}
GRUBCFG

if [ -n "$WIN_CHAINLOAD" ]; then
    cat >> "$MNT_EFI/boot/grub/grub.cfg" << WINCFG

menuentry "Windows" {
    insmod part_gpt
    insmod fat
    insmod chain
    insmod search_fs_file
    search --set=root --file $WIN_CHAINLOAD
    chainloader $WIN_CHAINLOAD
}
WINCFG
    log "GRUB menu: FiFi OS + Windows chainload at $WIN_CHAINLOAD"
fi

log "Bootloader installed"
prog 34

# ── 8. Browser download ────────────────────────────────────────────────────────

BROWSER_DIR="$MNT_DATA/browser"
mkdir -p "$BROWSER_DIR"
BROWSER_URL=""
BROWSER_DEST=""

if [ "$BROWSER" = "librewolf" ]; then
    log "Downloading LibreWolf (latest)..."
    # Get latest version from GitLab API
    LW_VER="$(curl -sf 'https://gitlab.com/api/v4/projects/24386000/releases' 2>/dev/null | \
        grep -o '"tag_name":"[^"]*"' | head -1 | cut -d'"' -f4)"
    if [ -n "$LW_VER" ]; then
        BROWSER_URL="https://gitlab.com/api/v4/projects/24386000/packages/generic/librewolf/${LW_VER}/LibreWolf.x86_64.AppImage"
    else
        BROWSER_URL="https://github.com/librewolf-community/browser-linux/releases/latest/download/librewolf.AppImage"
    fi
    BROWSER_DEST="$BROWSER_DIR/browser.AppImage"
elif [ "$BROWSER" = "firefox" ]; then
    log "Downloading Firefox (latest)..."
    BROWSER_URL="https://download.mozilla.org/?product=firefox-latest-ssl&os=linux64&lang=en-US"
    BROWSER_DEST="$BROWSER_DIR/firefox.tar.bz2"
fi

if [ -n "$BROWSER_URL" ]; then
    curl -L --progress-bar --output "$BROWSER_DEST" "$BROWSER_URL" 2>&1 | \
    while IFS= read -r line; do
        pct="$(echo "$line" | grep -oE '[0-9]+\.[0-9]+%' | head -1 | tr -d '%.')"
        [ -n "$pct" ] && echo "PROGRESS:$(( 34 + pct*28/100 ))" || echo "$line"
    done || { log "WARNING: Browser download failed"; rm -f "$BROWSER_DEST"; }

    if [ "$BROWSER" = "firefox" ] && [ -f "$BROWSER_DEST" ]; then
        log "Extracting Firefox..."
        mkdir -p "$BROWSER_DIR/firefox"
        tar -xjf "$BROWSER_DEST" -C "$BROWSER_DIR/firefox" --strip-components=1 2>/dev/null || true
        rm -f "$BROWSER_DEST"
        BROWSER_DEST="$BROWSER_DIR/firefox/firefox"
    fi

    [ -f "$BROWSER_DEST" ] && chmod +x "$BROWSER_DEST" || true
    echo "$BROWSER" > "$BROWSER_DIR/choice"
    log "Browser downloaded"
fi
prog 62

# ── 9. LibreOffice download ────────────────────────────────────────────────────

if echo "$SOFTWARE" | grep -q "libreoffice"; then
    log "Downloading LibreOffice (latest AppImage)..."
    LO_DIR="$MNT_DATA/libreoffice"
    mkdir -p "$LO_DIR"
    LO_DEST="$LO_DIR/libreoffice.AppImage"

    # Try to get the latest version number from LibreOffice website
    LO_VER="$(curl -sf 'https://www.libreoffice.org/download/download-libreoffice/' 2>/dev/null | \
        grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)"

    LO_URL=""
    if [ -n "$LO_VER" ]; then
        LO_URL="https://downloadarchive.documentfoundation.org/libreoffice/old/${LO_VER}/AppImage/LibreOffice_${LO_VER}_Linux_x86-64_compatible_AppImage.AppImage"
    fi
    # Fallback
    LO_FALLBACK="https://downloadarchive.documentfoundation.org/libreoffice/old/latest/AppImage/LibreOffice_latest_Linux_x86-64_compatible_AppImage.AppImage"

    curl -L --progress-bar --output "$LO_DEST" "${LO_URL:-$LO_FALLBACK}" 2>&1 | \
    while IFS= read -r line; do
        pct="$(echo "$line" | grep -oE '[0-9]+\.[0-9]+%' | head -1 | tr -d '%.')"
        [ -n "$pct" ] && echo "PROGRESS:$(( 62 + pct*28/100 ))" || echo "$line"
    done || { log "WARNING: LibreOffice download failed — install manually later"; rm -f "$LO_DEST"; }

    [ -f "$LO_DEST" ] && [ -s "$LO_DEST" ] && chmod +x "$LO_DEST" && \
        log "LibreOffice downloaded ($(du -sh "$LO_DEST" | cut -f1))" || true
fi
prog 92

# ── 10. Cleanup and finish ─────────────────────────────────────────────────────

log "Syncing to disk..."
sync

# Write installed marker — hides the installer icon on next boot
echo "$(date)" > "$MNT_DATA/installed"

log "Unmounting..."
umount "$MNT_DATA" 2>/dev/null || true
umount "$MNT_EFI"  2>/dev/null || true
rmdir  "$MNT_DATA" "$MNT_EFI" 2>/dev/null || true
trap - EXIT

sync; sleep 1
prog 100
log "Installation complete! Remove the USB and reboot."
exit 0
