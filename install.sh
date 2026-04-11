#!/usr/bin/env bash
# FiFi OS Installer — step-by-step TUI wizard
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO="$SCRIPT_DIR/build/fifi.iso"
TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"; clear' EXIT INT TERM

# ── dependency check ──────────────────────────────────────────────────────────
for cmd in dialog lsblk dd mkfs.ext2 parted partprobe; do
    command -v "$cmd" &>/dev/null || {
        echo "Error: '$cmd' not found." >&2
        echo "Install missing tools: sudo pacman -S dialog e2fsprogs parted util-linux" >&2
        exit 1
    }
done

H=15; W=65

# ── helpers ───────────────────────────────────────────────────────────────────
# All dialog output (selections) captured via temp file to avoid fd juggling
dlg() { dialog --colors "$@" 2>"$TMPFILE"; }
result() { cat "$TMPFILE"; }

msg() {   # msg TITLE TEXT
    dialog --colors --title "$1" --ok-label "Next >" --msgbox "$2" $H $W
}
confirm() {  # confirm TITLE TEXT yes_label   — returns 0=yes 1=no
    dialog --colors --title "$1" \
           --yes-label "${3:-Yes}" --no-label "Cancel" \
           --yesno "$2" $H $W
}

# ── STEP 1: Welcome ───────────────────────────────────────────────────────────
clear
dialog --colors \
       --title " FiFi OS Installer " \
       --yes-label "  Begin  " --no-label "  Exit  " \
       --yesno \
"\n\
\ZbWelcome to the FiFi OS Installer!\Zn\n\n\
This wizard will guide you through writing\n\
FiFi OS to a USB drive or disk in four steps:\n\n\
  1.  Build the OS image  (if needed)\n\
  2.  Select target drive\n\
  3.  Write FiFi OS to the drive\n\
  4.  Add a persistent data partition  (optional)\n\n\
\Zb\Z1WARNING:\Zn  The selected drive will be\n\
        completely and permanently erased." \
       $H $W || exit 0

# ── STEP 2: Build ISO ─────────────────────────────────────────────────────────
if [[ ! -f "$ISO" ]]; then
    confirm "Build Required" \
"\Zbuild/fifi.iso\Zn was not found.\n\n\
Build it now?  (\Zbmake iso\Zn in $SCRIPT_DIR)" "Build" || exit 0

    cd "$SCRIPT_DIR"
    { make iso 2>&1; echo "EXIT:$?"; } \
        | dialog --title " Building FiFi OS " \
                 --ok-label "Continue" \
                 --programbox "Compiling — please wait..." 20 $W

    [[ -f "$ISO" ]] || {
        msg "Build Failed" \
"\Z1Build failed.\Zn\n\nCheck terminal output above for errors.\n\nFix the error, then re-run install.sh."
        exit 1
    }
fi

ISO_SIZE=$(stat -c%s "$ISO")
ISO_MB=$(( ISO_SIZE / 1024 / 1024 + 1 ))

msg " Step 1 of 4 — Image Ready " \
"\Z2\Zb✓  FiFi OS image is ready\Zn\n\n\
  File:  build/fifi.iso\n\
  Size:  ${ISO_MB} MB\n\n\
Click \ZbNext\Zb to choose a target drive."

# ── STEP 3: Drive selection ───────────────────────────────────────────────────
while true; do
    # Build menu entries: exclude loop devices and any drive with a mounted partition
    ENTRIES=()
    while IFS='|' read -r name size model tran; do
        name="${name// /}"
        # skip if any partition of this disk is mounted
        if lsblk -ln -o MOUNTPOINT "/dev/$name" 2>/dev/null | grep -q .; then
            continue
        fi
        [[ -z "$model" ]] && model="Unknown device"
        model="${model//  / }"
        label="$(printf "%-6s  %s" "$size" "$model")"
        [[ -n "$tran" ]] && label="$label  [$tran]"
        ENTRIES+=("/dev/$name" "$label")
    done < <(lsblk -d -n -o NAME,SIZE,MODEL,TRAN | grep -v '^loop')

    if [[ ${#ENTRIES[@]} -eq 0 ]]; then
        confirm "No Drives Found" \
"No eligible drives were found.\n\n\
Drives with mounted filesystems are excluded\n\
for safety.\n\n\
Plug in a USB drive, then click \ZbRetry\Zb." "Retry" || exit 1
        continue
    fi

    dlg --title " Step 2 of 4 — Select Target Drive " \
        --ok-label "Select >" --cancel-label "Exit" \
        --menu \
"\ZbChoose the drive to install FiFi OS onto.\Zn\n\
\Z1All existing data will be erased.\Zn\n" \
        $H $W 8 "${ENTRIES[@]}" || exit 0

    TARGET=$(result)
    [[ -n "$TARGET" ]] && break
done

DRIVE_SIZE=$(lsblk -d -n -o SIZE "$TARGET" 2>/dev/null || echo "?")
DRIVE_MODEL=$(lsblk -d -n -o MODEL "$TARGET" 2>/dev/null | xargs || echo "Unknown")
DRIVE_TRAN=$(lsblk -d -n -o TRAN "$TARGET" 2>/dev/null | xargs || echo "")

# ── STEP 4: Confirm ───────────────────────────────────────────────────────────
dialog --colors \
       --title " Step 3 of 4 — Confirm " \
       --yes-label " Erase & Install " --no-label " Go Back " \
       --defaultno \
       --yesno \
"\n\
\Zb\Z1You are about to ERASE:\Zn\n\n\
  Drive:   \Zb$TARGET\Zn\n\
  Model:   $DRIVE_MODEL\n\
  Size:    $DRIVE_SIZE$([ -n "$DRIVE_TRAN" ] && echo "  [$DRIVE_TRAN]")\n\n\
  FiFi OS image: ${ISO_MB} MB\n\n\
\ZbThis cannot be undone.\Zn\n\n\
Select \Zb Erase & Install \Zb to proceed." \
       $H $W || exec "$0"

# ── STEP 5: Write ISO ─────────────────────────────────────────────────────────
dialog --colors --title " Installing " \
       --infobox "Requesting elevated privileges..." 5 $W
sudo -v || { msg "Authentication Failed" "sudo authentication failed.\nCannot write to block device."; exit 1; }

if command -v pv &>/dev/null; then
    # Real progress via pv
    {
        pv -n -s "$ISO_SIZE" "$ISO" 2>&3 \
            | sudo dd of="$TARGET" bs=4M iflag=fullblock oflag=direct status=none
    } 3>&1 \
        | dialog --title " Step 3 of 4 — Writing FiFi OS " \
                 --gauge "Writing ${ISO_MB} MB to $TARGET ..." \
                 8 $W 0
else
    # Time-based progress estimate (no pv installed)
    ESTIMATE_SECS=$(( ISO_MB / 20 + 2 ))   # assume ~20 MB/s
    (
        sudo dd if="$ISO" of="$TARGET" bs=4M oflag=direct status=none &
        DD_PID=$!
        T0=$SECONDS
        PCT=0
        while kill -0 $DD_PID 2>/dev/null; do
            ELAPSED=$(( SECONDS - T0 ))
            PCT=$(( ELAPSED * 100 / ESTIMATE_SECS ))
            (( PCT > 95 )) && PCT=95
            echo $PCT
            sleep 0.4
        done
        wait $DD_PID 2>/dev/null || true
        echo 100
    ) | dialog --title " Step 3 of 4 — Writing FiFi OS " \
               --gauge "Writing ${ISO_MB} MB to $TARGET ..." \
               8 $W 0
fi

sudo sync

# ── STEP 6: Data partition (optional) ────────────────────────────────────────
WANT_DATA=0
dialog --colors \
       --title " Step 4 of 4 — Persistent Data Partition " \
       --yes-label " Add Partition " --no-label " Skip " \
       --yesno \
"\n\
Add a persistent \Zbext2 data partition\Zn in the\n\
remaining space on $TARGET?\n\n\
FiFi OS stores files written during a session\n\
in this partition, so they survive reboots.\n\n\
  • Recommended for USB drives\n\
  • The rest of the drive space will be used\n" \
       $H $W && WANT_DATA=1 || true

if [[ $WANT_DATA -eq 1 ]]; then
    dialog --colors --title " Partitioning " \
           --infobox "Creating ext2 data partition..." 5 $W

    # Start just after ISO data, rounded up to next 2 MiB boundary
    START_MIB=$(( (ISO_SIZE / 1024 / 1024 + 3) / 2 * 2 ))

    sudo parted -s "$TARGET" mkpart primary ext2 "${START_MIB}MiB" 100% 2>/dev/null || true
    sudo partprobe "$TARGET" 2>/dev/null || true
    sleep 1

    # Find the newly created partition (last one listed)
    NEW_PART=$(lsblk -ln -o NAME "$TARGET" | grep -v "^$(basename "$TARGET")$" | tail -1)
    if [[ -n "$NEW_PART" ]]; then
        sudo mkfs.ext2 -L "fifi-data" "/dev/$NEW_PART" &>/dev/null
        DATA_MSG="\n  Data partition: \Zb/dev/$NEW_PART\Zn  (ext2, label: fifi-data)"
    else
        DATA_MSG="\n  \Z3Note:\Zn Could not create data partition\n  (ISO may already fill the drive)"
    fi
else
    DATA_MSG=""
fi

# ── STEP 7: Done ─────────────────────────────────────────────────────────────
dialog --colors \
       --title " Installation Complete " \
       --ok-label "  Finish  " \
       --msgbox \
"\n\
\Z2\Zb✓  FiFi OS installed successfully!\Zn\n\n\
  Drive:   $TARGET  ($DRIVE_SIZE)\n\
  Written: ${ISO_MB} MB${DATA_MSG}\n\n\
You can now remove the drive and boot from it.\n\n\
  \Zb→\Zn  Set USB as first boot device in BIOS/UEFI\n\
  \Zb→\Zn  FiFi OS will load the interactive shell\n" \
       $H $W

clear
printf "FiFi OS installed to %s. Happy booting!\n" "$TARGET"
