#!/usr/bin/env bash
# Rebuild the FiFi compositor, repack the minimal test initramfs, boot it in
# QEMU headless, drive a menu open + in-menu click via QMP (virtio-tablet abs),
# and print the mtest pointer-event log. Reproduces the browser popup-menu
# interaction so popup-grab fixes can be tested locally (no hardware reboots).
#
# Menu opens at (100,101) size 200x300. We click at (250,350) — visually inside
# the drawn menu — and check whether the compositor routes it to MENU-SUB.
set -u
cd "$(cd "$(dirname "$0")/../.." && pwd)"   # repo root
S=/tmp/mini-initrd

echo "[run] building compositor..."
( cd fifi/compositor && make -s ) || { echo "BUILD FAILED"; exit 1; }
cp build-linux/fifi-compositor "$S/bin/fifi-compositor"
echo "[run] building mtest..."
( cd test/popup && gcc -O2 -D_GNU_SOURCE -o mtest mtest.c xdg-shell-protocol.c -lwayland-client ) || { echo "MTEST BUILD FAILED"; exit 1; }
cp test/popup/mtest "$S/bin/mtest"
( cd "$S" && find . | busybox cpio -o -H newc 2>/dev/null | gzip -9 > /home/aaron/src/linux-desktop/build-linux/mini-initrd.cpio.gz )

rm -f serial-qtest.log /tmp/qmp.sock
qemu-system-x86_64 -M q35 -enable-kvm -cpu host -m 4G -smp 4 \
  -kernel build-linux/bzImage -initrd build-linux/mini-initrd.cpio.gz \
  -no-reboot -device virtio-tablet-pci -device virtio-keyboard-pci \
  -append "console=ttyS0,115200 loglevel=4 panic=-1" \
  -device virtio-vga,xres=1920,yres=1080 -display none \
  -serial file:"$PWD/serial-qtest.log" \
  -qmp unix:/tmp/qmp.sock,server,nowait >/tmp/qemu-err.log 2>&1 &
QPID=$!

for i in $(seq 1 40); do grep -qa "MTEST: ready" serial-qtest.log 2>/dev/null && break; timeout 0.5 tail -f /dev/null; done

qmp(){ { echo '{"execute":"qmp_capabilities"}'; for e in "$@"; do echo "$e"; done; } | socat -T1 - unix-connect:/tmp/qmp.sock >/dev/null 2>&1; }
abs(){ echo "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$1}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$2}}]}}"; }
DN='{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":true}}]}}'
UP='{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":false}}]}}'

# Menu opens at (100,101) size 200x300 (anchor 100,100). Click just above it,
# then move the cursor CONTINUOUSLY (stepped) down into the menu — this crosses
# the toplevel first (like moving off a toolbar button toward the menu), which
# is what makes a real GTK menu dismiss.
mv2(){ qmp "$(abs $1 $2)"; }
qmp "$(abs 2560 2731)" "$DN" "$UP"    # click (150,90) on toplevel -> open menu
timeout 2 tail -f /dev/null
echo "--- now stepping cursor down toward/into the menu ---"
mv2 2645 2882;  timeout 0.4 tail -f /dev/null   # (155,95)  toplevel
mv2 2731 3100;  timeout 0.4 tail -f /dev/null   # (160,102) menu top edge
mv2 2900 4551;  timeout 0.4 tail -f /dev/null   # (170,150) menu
mv2 3413 7585;  timeout 0.4 tail -f /dev/null   # (200,250) menu
qmp "$DN" "$UP"                                   # click the menu item
timeout 1 tail -f /dev/null

echo "=== MTEST + popup log ==="
grep -aE "MTEST|popup parent_xdg" serial-qtest.log | cat
kill $QPID 2>/dev/null
echo "[run] done"
