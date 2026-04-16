#!/usr/bin/env bash
# FiFi OS Installer — Linux and macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Look for ISO in build/ first (where make puts it), then repo root
if [[ -f "$SCRIPT_DIR/build/fifi.iso" ]]; then
    ISO="$SCRIPT_DIR/build/fifi.iso"
else
    ISO="$SCRIPT_DIR/fifi.iso"
fi
TMPFILE=$(mktemp)
OS=$(uname -s)   # Linux or Darwin

trap 'rm -f "$TMPFILE"' EXIT INT TERM

# ── argument parsing ──────────────────────────────────────────────────────────
FORCED_TARGET=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) FORCED_TARGET="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

# ── dependency check ──────────────────────────────────────────────────────────
if [[ "$OS" == "Darwin" ]]; then
    for cmd in dd diskutil; do
        command -v "$cmd" &>/dev/null || { echo "Error: '$cmd' not found." >&2; exit 1; }
    done
    # dialog is optional on macOS
    USE_DIALOG=false
    command -v dialog &>/dev/null && USE_DIALOG=true || true
else
    for cmd in dialog lsblk dd mkfs.ext2 parted partprobe; do
        command -v "$cmd" &>/dev/null || {
            echo "Error: '$cmd' not found." >&2
            echo "Install: sudo pacman -S dialog e2fsprogs parted util-linux" >&2
            exit 1
        }
    done
    USE_DIALOG=true
fi

H=15; W=65

# ── dialog color theme — fixes washed-out/cyan button highlight ───────────────
if [[ $USE_DIALOG == true ]]; then
    _DRC=$(mktemp)
    cat > "$_DRC" << 'DIALOGRC_EOF'
screen_color         = (WHITE,BLUE,ON)
shadow_color         = (BLACK,BLACK,ON)
dialog_color         = (BLACK,WHITE,OFF)
title_color          = (BLUE,WHITE,ON)
border_color         = (BLUE,WHITE,ON)
border2_color        = (BLACK,WHITE,OFF)
button_active_color       = (WHITE,BLUE,ON)
button_inactive_color     = (BLACK,WHITE,OFF)
button_key_active_color   = (YELLOW,BLUE,ON)
button_key_inactive_color = (RED,WHITE,OFF)
button_label_active_color   = (WHITE,BLUE,ON)
button_label_inactive_color = (BLACK,WHITE,OFF)
menubox_color        = (BLACK,WHITE,OFF)
menubox_border_color = (BLUE,WHITE,ON)
item_color           = (BLACK,WHITE,OFF)
item_selected_color  = (WHITE,BLUE,ON)
tag_color            = (BLUE,WHITE,ON)
tag_selected_color   = (WHITE,BLUE,ON)
tag_key_color        = (RED,WHITE,OFF)
tag_key_selected_color = (YELLOW,BLUE,ON)
inputbox_color       = (BLACK,WHITE,OFF)
check_color          = (BLACK,WHITE,OFF)
check_selected_color = (WHITE,BLUE,ON)
gauge_color          = (WHITE,BLUE,ON)
uarrow_color         = (BLUE,WHITE,ON)
darrow_color         = (BLUE,WHITE,ON)
DIALOGRC_EOF
    export DIALOGRC="$_DRC"
    trap 'rm -f "$TMPFILE" "$_DRC"; [[ $USE_DIALOG == true ]] && clear || true' EXIT INT TERM
fi

# ── UI helpers ────────────────────────────────────────────────────────────────
_b='\033[1m'; _r='\033[31m'; _g='\033[32m'; _y='\033[33m'; _c='\033[36m'; _x='\033[0m'

_strip_dlg() { echo -e "$1" | sed 's/\\Z[0-9bn]//g; s/\\Zb//g; s/\\Zn//g'; }

ui_msg() {   # title body
    if [[ $USE_DIALOG == true ]]; then
        dialog --colors --title "$1" --ok-label "Next >" --msgbox "$2" $H $W
    else
        echo ""; echo -e "${_b}${_c}── $1 ──${_x}"; _strip_dlg "$2"
        echo ""; printf "Press Enter to continue..."; read -r
    fi
}

ui_yesno() {  # title body yes_label  → 0=yes 1=no
    if [[ $USE_DIALOG == true ]]; then
        dialog --colors --title "$1" \
               --yes-label "${3:-Yes}" --no-label "Cancel" \
               --yesno "$2" $H $W
    else
        echo ""; echo -e "${_b}${_c}── $1 ──${_x}"; _strip_dlg "$2"; echo ""
        printf "%s / Cancel [Y/n]: " "${3:-Yes}"; read -r ANS
        [[ "${ANS:-Y}" =~ ^[Yy] ]]
    fi
}

ui_confirm_erase() {  # title body   (defaults to NO for safety)
    if [[ $USE_DIALOG == true ]]; then
        dialog --colors --title "$1" \
               --yes-label " Erase & Install " --no-label " Go Back " \
               --defaultno --yesno "$2" $H $W
    else
        echo ""; echo -e "${_b}${_r}── $1 ──${_x}"; _strip_dlg "$2"; echo ""
        printf "Type YES to confirm erase: "; read -r ANS
        [[ "$ANS" == "YES" ]]
    fi
}

ui_gauge() {  # title msg  (reads 0-100 from stdin)
    if [[ $USE_DIALOG == true ]]; then
        dialog --title "$1" --gauge "$2" 8 $W 0
    else
        echo -e "${_b}${_c}── $1 ──${_x}  $2"
        while IFS= read -r pct; do
            printf "\r  [%-40s] %3d%%" "$(printf '#%.0s' $(seq 1 $((pct*40/100))))" "$pct"
        done
        echo ""
    fi
}

ui_info() {  # title msg
    if [[ $USE_DIALOG == true ]]; then
        dialog --colors --title "$1" --infobox "$2" 5 $W
    else
        echo -e "${_b}${_c}── $1 ──${_x}  $(_strip_dlg "$2")"
    fi
}

dlg_menu() {  # title prompt entries...  — sets TMPFILE, returns 0/1
    if [[ $USE_DIALOG == true ]]; then
        dialog --colors --title "$1" \
               --ok-label "Select >" --cancel-label "Exit" \
               --menu "$2" $H $W 8 "${@:3}" 2>"$TMPFILE"
    else
        echo ""; echo -e "${_b}${_c}── $1 ──${_x}"; echo -e "$2"; echo ""
        local i=1
        local -a keys=() labels=()
        local args=("${@:3}")
        while [[ $i -lt ${#args[@]} ]]; do
            keys+=("${args[$((i-1))]}"); labels+=("${args[$i]}")
            echo "  ${#keys[@]}) ${args[$((i-1))]}  ${args[$i]}"
            i=$((i+2))
        done
        echo ""
        printf "Enter number: "; read -r NUM
        local idx=$(( NUM - 1 ))
        [[ $idx -ge 0 && $idx -lt ${#keys[@]} ]] || { echo "Invalid choice." >&2; return 1; }
        echo "${keys[$idx]}" > "$TMPFILE"
    fi
}

result() { cat "$TMPFILE"; }

# ── platform helpers ──────────────────────────────────────────────────────────
list_drives_linux() {
    ENTRIES=()
    while read -r name size model tran; do
        name="${name// /}"
        if lsblk -ln -o MOUNTPOINT "/dev/$name" 2>/dev/null | grep -q .; then continue; fi
        [[ -z "$model" ]] && model="Unknown device"
        model="${model//  / }"
        label="$(printf "%-6s  %s" "$size" "$model")"
        [[ -n "$tran" ]] && label="$label  [$tran]"
        ENTRIES+=("/dev/$name" "$label")
    done < <(lsblk -d -n -o NAME,SIZE,MODEL,TRAN | grep -v '^loop')
}

list_drives_macos() {
    ENTRIES=()
    while IFS= read -r line; do
        if [[ "$line" =~ ^(/dev/disk[0-9]+)[[:space:]] ]]; then
            DEV="${BASH_REMATCH[1]}"
            # Skip system disk (disk0 is almost always the boot disk on Mac)
            [[ "$DEV" == "/dev/disk0" ]] && continue
            INFO=$(diskutil info "$DEV" 2>/dev/null || true)
            SIZE=$(echo "$INFO" | grep "Disk Size:" | awk -F'[:(]' '{print $2}' | xargs)
            NAME=$(echo "$INFO" | grep "Device / Media Name:" | cut -d: -f2- | xargs)
            [[ -z "$SIZE" ]] && SIZE="?"
            [[ -z "$NAME" ]] && NAME="Removable disk"
            ENTRIES+=("$DEV" "$(printf "%-8s  %s" "$SIZE" "$NAME")")
        fi
    done < <(diskutil list 2>/dev/null)
}

write_iso_linux() {   # TARGET ISO_FILE ISO_SIZE
    local target="$1" iso="$2" iso_size="$3"
    if command -v pv &>/dev/null; then
        {
            pv -n -s "$iso_size" "$iso" 2>&3 \
                | sudo dd of="$target" bs=4M iflag=fullblock oflag=direct status=none
        } 3>&1 | ui_gauge " Installing FiFi OS " "Writing to $target..."
    else
        local estimate=$(( iso_size / 1024 / 1024 / 20 + 2 ))
        (
            sudo dd if="$iso" of="$target" bs=4M oflag=direct status=none &
            local pid=$!; local t0=$SECONDS; local pct=0
            while kill -0 $pid 2>/dev/null; do
                pct=$(( (SECONDS-t0)*100/estimate )); (( pct>95 )) && pct=95
                echo $pct; sleep 0.4
            done; wait $pid 2>/dev/null || true; echo 100
        ) | ui_gauge " Installing FiFi OS " "Writing to $target..."
    fi
    sudo sync
}

write_iso_macos() {   # TARGET ISO_FILE ISO_SIZE
    local target="$1" iso="$2" iso_size="$3"
    local rdisk="${target/\/dev\/disk//dev/rdisk}"
    ui_info "Installing" "Unmounting $target..."
    sudo diskutil unmountDisk "$target" >/dev/null 2>&1 || true
    local mb=$(( iso_size / 1024 / 1024 + 1 ))
    if command -v pv &>/dev/null; then
        {
            pv -n -s "$iso_size" "$iso" 2>&3 \
                | sudo dd of="$rdisk" bs=4m 2>/dev/null
        } 3>&1 | ui_gauge " Installing FiFi OS " "Writing ${mb}MB to $rdisk..."
    else
        local estimate=$(( mb / 15 + 2 ))
        (
            sudo dd if="$iso" of="$rdisk" bs=4m status=none &
            local pid=$!; local t0=$SECONDS; local pct=0
            while kill -0 $pid 2>/dev/null; do
                pct=$(( (SECONDS-t0)*100/estimate )); (( pct>95 )) && pct=95
                echo $pct; sleep 0.5
            done; wait $pid 2>/dev/null || true; echo 100
        ) | ui_gauge " Installing FiFi OS " "Writing ${mb}MB to $rdisk..."
    fi
    sudo sync
}

add_data_partition_linux() {  # TARGET ISO_SIZE
    local target="$1" iso_size="$2"
    local start=$(( (iso_size / 1024 / 1024 + 3) / 2 * 2 ))
    ui_info "Partitioning" "Creating ext2 data partition..."
    sudo parted -s "$target" mkpart primary ext2 "${start}MiB" 100% 2>/dev/null || true
    sudo partprobe "$target" 2>/dev/null || true
    sleep 1
    local new_part
    new_part=$(lsblk -ln -o NAME "$target" | grep -v "^$(basename "$target")$" | tail -1)
    if [[ -n "$new_part" ]]; then
        sudo mkfs.ext2 -L "fifi-data" "/dev/$new_part" &>/dev/null
        echo "/dev/$new_part"
    fi
}

# ── STEP 1: Welcome ───────────────────────────────────────────────────────────
clear
ui_yesno " FiFi OS Installer " \
"Welcome to the FiFi OS Installer!\n\n\
This wizard will:\n\
  1.  Locate the FiFi OS image\n\
  2.  Let you choose a target drive\n\
  3.  Write FiFi OS to the drive\n\
  4.  Optionally add a data partition  (Linux only)\n\n\
\ZbWARNING:\Zn  The target drive will be completely erased." \
"Begin" || exit 0

# ── STEP 2: Locate ISO ────────────────────────────────────────────────────────
if [[ ! -f "$ISO" ]]; then
    ui_msg "Image Not Found" \
"\Z1fifi.iso not found.\Zn\n\n\
Place fifi.iso in the same directory as\nthis script and run it again."
    exit 1
fi

ISO_SIZE=$(stat -c%s "$ISO" 2>/dev/null || stat -f%z "$ISO")
ISO_MB=$(( ISO_SIZE / 1024 / 1024 + 1 ))

ui_msg " Image Ready " \
"\Z2\Zb✓  FiFi OS image found\Zn\n\n\
  fifi.iso  (${ISO_MB} MB)\n\n\
Click \ZbNext\Zb to select a drive."

# ── STEP 3: Drive selection ───────────────────────────────────────────────────
if [[ -n "$FORCED_TARGET" ]]; then
    TARGET="$FORCED_TARGET"
else
    while true; do
        if [[ "$OS" == "Darwin" ]]; then
            list_drives_macos
        else
            list_drives_linux
        fi

        if [[ ${#ENTRIES[@]} -eq 0 ]]; then
            ui_yesno "No Drives Found" \
"No eligible drives found.\n\nPlug in a USB drive and click \ZbRetry\Zb." "Retry" || exit 1
            continue
        fi

        dlg_menu " Select Target Drive " \
"\ZbChoose the drive to install FiFi OS onto.\Zn\n\Z1All data will be erased.\Zn" \
"${ENTRIES[@]}" || exit 0

        TARGET=$(result)
        [[ -n "$TARGET" ]] && break
    done
fi

# ── STEP 4: Confirm ───────────────────────────────────────────────────────────
if [[ "$OS" == "Darwin" ]]; then
    DRIVE_SIZE=$(diskutil info "$TARGET" 2>/dev/null | grep "Disk Size:" | awk -F'[:(]' '{print $2}' | xargs || echo "?")
    DRIVE_MODEL=$(diskutil info "$TARGET" 2>/dev/null | grep "Device / Media Name:" | cut -d: -f2- | xargs || echo "")
else
    DRIVE_SIZE=$(lsblk -d -n -o SIZE "$TARGET" 2>/dev/null | xargs || echo "?")
    DRIVE_MODEL=$(lsblk -d -n -o MODEL "$TARGET" 2>/dev/null | xargs || echo "")
fi
[[ -z "$DRIVE_MODEL" ]] && DRIVE_MODEL="(unknown)"

ui_confirm_erase " Confirm Installation " \
"\n\Z1THIS WILL ERASE:\Zn\n\n\
  Drive:  \Zb$TARGET\Zn\n\
  Model:  $DRIVE_MODEL\n\
  Size:   $DRIVE_SIZE\n\n\
  FiFi OS: ${ISO_MB} MB\n\n\
\ZbThis cannot be undone.\Zn" || exec "$0" ${FORCED_TARGET:+--target "$FORCED_TARGET"}

# ── STEP 5: Write ─────────────────────────────────────────────────────────────
ui_info "Installing" "Requesting elevated privileges..."
sudo -v || { ui_msg "Error" "sudo authentication failed."; exit 1; }

if [[ "$OS" == "Darwin" ]]; then
    write_iso_macos "$TARGET" "$ISO" "$ISO_SIZE"
else
    write_iso_linux "$TARGET" "$ISO" "$ISO_SIZE"
fi

# ── STEP 6: Data partition (Linux only) ──────────────────────────────────────
DATA_MSG=""
if [[ "$OS" != "Darwin" ]] && [[ -z "$FORCED_TARGET" || "${1:-}" != "--no-data" ]]; then
    WANT_DATA=0
    ui_yesno " Persistent Data Partition " \
"Add an \Zbext2 data partition\Zn in remaining space?\n\n\
FiFi OS stores files written during a session\n\
in this partition so they survive reboots." \
"Add Partition" && WANT_DATA=1 || true

    if [[ $WANT_DATA -eq 1 ]]; then
        NEW_PART=$(add_data_partition_linux "$TARGET" "$ISO_SIZE")
        if [[ -n "$NEW_PART" ]]; then
            DATA_MSG="\n  Data partition: \Zb$NEW_PART\Zn  (ext2)"
        fi
    fi
elif [[ "$OS" == "Darwin" ]]; then
    DATA_MSG="\n  \Z3Note:\Zn  Data partition requires e2fsprogs (brew install e2fsprogs)"
fi

# ── STEP 7: Done ─────────────────────────────────────────────────────────────
ui_msg " Installation Complete " \
"\n\Z2\Zb✓  FiFi OS installed successfully!\Zn\n\n\
  Drive:   $TARGET\n\
  Written: ${ISO_MB} MB${DATA_MSG}\n\n\
Remove the drive and boot from it.\n\
Set USB as first boot device in BIOS/UEFI."

[[ $USE_DIALOG == true ]] && clear || true
echo "FiFi OS installed to $TARGET."
