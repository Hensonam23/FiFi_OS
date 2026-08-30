#!/bin/sh
# Upgrade an installed FiFi GRUB menu to automatic A/B fallback with one normal
# user-facing boot choice.
set -u

DATA_ROOT="${FIFI_DATA_ROOT:-/fifi-data}"
MARKER="$DATA_ROOT/.grub-single-menu-migrated"
MOUNT_POINT="${FIFI_EFI_MOUNT_POINT:-/mnt/fifi-grub-migrate}"
GRUB_EDITENV="${FIFI_GRUB_EDITENV:-grub-editenv}"
WRITER="${FIFI_GRUB_CONFIG_WRITER:-fifi-write-grub-config}"

log() { printf '%s\n' "grub migration: $*" >> "$DATA_ROOT/boot-slots.log"; }

[ -f "$DATA_ROOT/installed" ] || exit 0
[ -e "$MARKER" ] && exit 0

DATA_UUID="${FIFI_DATA_UUID:-}"
if [ -z "$DATA_UUID" ]; then
    DATA_DEV="$(awk -v root="$DATA_ROOT" '$2 == root {print $1; exit}' /proc/mounts)"
    [ -n "$DATA_DEV" ] || { log "cannot identify the data partition"; exit 0; }
    DATA_UUID="$(blkid "$DATA_DEV" 2>/dev/null |
        sed -n 's/.* UUID="\([^"]*\)".*/\1/p' | head -1)"
fi
printf '%s\n' "$DATA_UUID" | grep -Eq '^[A-Fa-f0-9-]+$' || {
    log "cannot identify the data partition UUID"
    exit 0
}
FIFI_DATA_ROOT="$DATA_ROOT" fifi-boot-slots ensure >/dev/null 2>&1 || {
    log "cannot initialize both boot slots; legacy GRUB left unchanged"
    exit 0
}

config_matches_install() {
    [ -f "$1/boot/grub/grub.cfg" ] &&
        grep -Fq "fifi_data_uuid=$DATA_UUID" "$1/boot/grub/grub.cfg"
}

migrate_root() {
    root="$1"
    config="$root/boot/grub/grub.cfg"
    grubenv="$root/boot/grub/grubenv"
    backup="$root/boot/grub/grub.cfg.before-single-menu"

    config_matches_install "$root" || return 1
    if grep -Fq 'source /boot/fifi-slot.cfg' "$config" &&
       grep -Fq 'save_env -f $fifi_grubenv fifi_attempted' "$config" &&
       grep -Fq 'menuentry "FiFi OS"' "$config" &&
       ! grep -Fq 'menuentry "FiFi OS (slot A)"' "$config"; then
        : > "$MARKER"
        return 0
    fi

    command -v "$GRUB_EDITENV" >/dev/null 2>&1 || {
        log "grub-editenv is unavailable; will retry next boot"
        return 1
    }
    command -v "$WRITER" >/dev/null 2>&1 || {
        log "A/B GRUB config writer is unavailable; will retry next boot"
        return 1
    }

    windows="$(sed -n 's/^[[:space:]]*chainloader[[:space:]][[:space:]]*\([^[:space:]]*\).*/\1/p' "$config" |
        head -1)"
    case "$windows" in
        /EFI/*) ;;
        *) windows="" ;;
    esac

    [ -e "$backup" ] || cp -p "$config" "$backup" || return 1
    "$GRUB_EDITENV" "$grubenv" create || {
        log "could not create GRUB boot-attempt state; will retry next boot"
        return 1
    }
    "$WRITER" "$config" "$DATA_UUID" "$windows" || {
        log "could not write the A/B GRUB menu; will retry next boot"
        return 1
    }
    sync
    : > "$MARKER"
    log "installed single-choice automatic fallback menu; old config retained at $backup"
    return 0
}

# Tests and recovery tools may supply an already-mounted EFI root.
if [ -n "${FIFI_EFI_ROOT:-}" ]; then
    migrate_root "$FIFI_EFI_ROOT" || true
    exit 0
fi

mkdir -p "$MOUNT_POINT" || exit 0
MOUNTED=0
cleanup() {
    [ "$MOUNTED" = 1 ] && umount "$MOUNT_POINT" 2>/dev/null || true
    rmdir "$MOUNT_POINT" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
for sys_part in /sys/class/block/*; do
    [ -f "$sys_part/partition" ] || continue
    dev="/dev/${sys_part##*/}"
    [ -b "$dev" ] || continue
    mount -o ro "$dev" "$MOUNT_POINT" 2>/dev/null || continue
    MOUNTED=1
    if config_matches_install "$MOUNT_POINT"; then
        umount "$MOUNT_POINT" 2>/dev/null || break
        MOUNTED=0
        mount -o rw "$dev" "$MOUNT_POINT" 2>/dev/null || {
            log "matched EFI partition $dev but could not mount it writable"
            break
        }
        MOUNTED=1
        migrate_root "$MOUNT_POINT" || true
        exit 0
    fi
    umount "$MOUNT_POINT" 2>/dev/null || break
    MOUNTED=0
done
log "matching legacy FiFi EFI configuration not found; will retry next boot"
exit 0
