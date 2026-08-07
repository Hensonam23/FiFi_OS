#!/usr/bin/env bash
# Exercise FiFi's A/B attempt state through real OVMF and GRUB across two boots.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL="$ROOT/build-linux/bzImage"

required=(qemu-system-x86_64 grub-mkimage grub-editenv mkfs.fat mkfs.ext4 parted mcopy mmd debugfs cpio busybox)
missing=()
for tool in "${required[@]}"; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if ((${#missing[@]})); then
    if command -v flatpak-spawn >/dev/null 2>&1 &&
       [[ "${FIFI_BOOT_FALLBACK_ON_HOST:-0}" != 1 ]]; then
        exec flatpak-spawn --host env FIFI_BOOT_FALLBACK_ON_HOST=1 \
            bash "$ROOT/test/boot-fallback/run.sh"
    fi
    printf '[boot-fallback-test] missing tools: %s\n' "${missing[*]}" >&2
    exit 1
fi

[[ -s "$KERNEL" ]] || {
    echo "[boot-fallback-test] missing $KERNEL; run make linux-kernel" >&2
    exit 1
}

OVMF_CODE="${FIFI_OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS="${FIFI_OVMF_VARS:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
[[ -r "$OVMF_CODE" && -r "$OVMF_VARS" ]] || {
    echo "[boot-fallback-test] OVMF x86-64 firmware is unavailable" >&2
    exit 1
}

WORK="$(mktemp -d)"
QEMU_PID=""
cleanup() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM
ESP="$WORK/efi-disk.img"
ESP_PART="$WORK/efi-partition.img"
ESP_MTOOLS="$ESP_PART"
DATA="$WORK/data.img"
INITRD="$WORK/test-initramfs.cpio.gz"
GRUB_CFG="$WORK/grub.cfg"
GRUB_ENV="$WORK/grubenv"
BOOT_EFI="$WORK/BOOTX64.EFI"
TEST_KERNEL="$WORK/bzImage"

echo "[boot-fallback-test] building disposable UEFI disks"
truncate -s 196M "$ESP"
truncate -s 62M "$ESP_PART"
truncate -s 128M "$DATA"
parted -s "$ESP" mklabel gpt \
    mkpart EFI fat32 2048s 129023s \
    mkpart FIFI ext4 131072s 393215s \
    set 1 esp on
mkfs.fat -F 32 "$ESP_PART" >/dev/null
mkfs.ext4 -q -F "$DATA"
DATA_UUID="$(blkid -s UUID -o value "$DATA")"
[[ "$DATA_UUID" =~ ^[A-Fa-f0-9-]+$ ]]
cp "$KERNEL" "$TEST_KERNEL"

# Both slots use the same tiny probe. Slot B represents an image that fails
# before desktop confirmation; slot A proves GRUB supplied the fallback flag.
mkdir -p "$WORK/initramfs/bin" "$WORK/initramfs/proc" "$WORK/initramfs/sys"
cp "$(command -v busybox)" "$WORK/initramfs/bin/busybox"
for applet in sh mount cat sync poweroff sleep; do
    ln -s busybox "$WORK/initramfs/bin/$applet"
done
cat > "$WORK/initramfs/init" <<'INIT'
#!/bin/sh
mount -t proc proc /proc
cmdline="$(cat /proc/cmdline)"
case "$cmdline" in
    *fifi_slot=B*)
        echo "FIFI_AB_TEST BAD_SLOT slot=B"
        ;;
    *fifi_slot=A*fifi_boot_fallback=1*)
        echo "FIFI_AB_TEST FALLBACK_PASS slot=A fallback=1"
        ;;
    *)
        echo "FIFI_AB_TEST FAIL unexpected-cmdline: $cmdline"
        ;;
esac
sync
poweroff -f
sleep 5
exit 1
INIT
chmod +x "$WORK/initramfs/init"
(cd "$WORK/initramfs" && find . -print0 |
    cpio --null -o --format=newc 2>/dev/null | gzip -9) > "$INITRD"

cat > "$WORK/fifi-slot.cfg" <<'EOF'
set fifi_active=B
set fifi_previous=A
set fifi_pending=1
set fifi_attempt=integration-test
EOF
"$ROOT/initramfs/root/bin/fifi-write-grub-config" \
    "$GRUB_CFG" "$DATA_UUID"
# The installed menu is graphical. This disposable probe needs a serial console
# so the host can make a deterministic assertion without screenshot parsing.
sed -i 's/set timeout=5/set timeout=1/' "$GRUB_CFG"
sed -i 's/console=tty0 quiet loglevel=3/console=ttyS0,115200 loglevel=7/g' "$GRUB_CFG"
grub-editenv "$GRUB_ENV" create
cat > "$WORK/grub-early.cfg" <<'EOF'
search --no-floppy --file --set=root /boot/grub/grub.cfg
set prefix=($root)/boot/grub
configfile /boot/grub/grub.cfg
EOF
grub-mkimage -O x86_64-efi -o "$BOOT_EFI" -p /boot/grub \
    -c "$WORK/grub-early.cfg" part_gpt part_msdos fat ext2 normal boot linux \
    configfile search search_fs_uuid search_fs_file loadenv test >/dev/null

mmd -i "$ESP_MTOOLS" ::/EFI ::/EFI/BOOT ::/boot ::/boot/grub
mcopy -i "$ESP_MTOOLS" "$BOOT_EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_MTOOLS" "$GRUB_CFG" ::/boot/grub/grub.cfg
mcopy -i "$ESP_MTOOLS" "$GRUB_ENV" ::/boot/grub/grubenv

debugfs -w -R 'mkdir /boot' "$DATA" >/dev/null 2>&1
for slot in A B; do
    debugfs -w -R "write $TEST_KERNEL /boot/bzImage.$slot" "$DATA" >/dev/null 2>&1
    debugfs -w -R "write $INITRD /boot/initramfs.cpio.gz.$slot" "$DATA" >/dev/null 2>&1
done
debugfs -w -R "write $WORK/fifi-slot.cfg /boot/fifi-slot.cfg" "$DATA" >/dev/null 2>&1
for required_file in /boot/bzImage.A /boot/initramfs.cpio.gz.A \
                     /boot/bzImage.B /boot/initramfs.cpio.gz.B \
                     /boot/fifi-slot.cfg; do
    debugfs -R "stat $required_file" "$DATA" 2>/dev/null |
        grep -Fq 'Type: regular' || {
        echo "[boot-fallback-test] failed to populate $required_file" >&2
        exit 1
    }
done
dd if="$ESP_PART" of="$ESP" bs=1M seek=1 conv=notrunc status=none
dd if="$DATA" of="$ESP" bs=1M seek=64 conv=notrunc status=none
cp "$OVMF_VARS" "$WORK/OVMF_VARS.fd"

run_boot() {
    local number="$1"
    local expected="$2"
    local log="$WORK/boot-$number.log"
    qemu-system-x86_64 \
        -M q35 -accel tcg,thread=multi -cpu max -m 512M -smp 2 \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$WORK/OVMF_VARS.fd" \
        -device qemu-xhci \
        -drive file="$ESP",format=raw,if=none,id=fifi-efi \
        -device usb-storage,drive=fifi-efi,bootindex=1 \
        -display none -monitor none -serial stdio -no-reboot >"$log" 2>&1 &
    QEMU_PID=$!
    for _ in $(seq 1 60); do
        if grep -aFq "$expected" "$log" 2>/dev/null; then
            kill "$QEMU_PID" 2>/dev/null || true
            wait "$QEMU_PID" 2>/dev/null || true
            QEMU_PID=""
            sync
            return 0
        fi
        kill -0 "$QEMU_PID" 2>/dev/null || break
        sleep 1
    done
    echo "[boot-fallback-test] boot $number did not report: $expected" >&2
    tail -n 80 "$log" >&2
    return 1
}

echo "[boot-fallback-test] boot 1: pending slot must be attempted"
run_boot 1 'FIFI_AB_TEST BAD_SLOT slot=B'
grep -aFq 'FIFI_AB_TEST BAD_SLOT slot=B' "$WORK/boot-1.log" || {
    tail -n 80 "$WORK/boot-1.log" >&2
    exit 1
}

echo "[boot-fallback-test] boot 2: GRUB must select the previous slot"
run_boot 2 'FIFI_AB_TEST FALLBACK_PASS slot=A fallback=1'
grep -aFq 'FIFI_AB_TEST FALLBACK_PASS slot=A fallback=1' "$WORK/boot-2.log" || {
    tail -n 80 "$WORK/boot-2.log" >&2
    exit 1
}

echo "[boot-fallback-test] PASS pending slot failed and GRUB selected slot A"
