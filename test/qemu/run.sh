#!/usr/bin/env bash
# Boot the real Linux image headlessly, exercise an unprivileged GUI app, and
# compare the rendered desktop against a compact screenshot baseline.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL="$ROOT/build-linux/bzImage"
INITRAMFS="$ROOT/build-linux/initramfs.cpio.gz"
BASELINE="$ROOT/test/qemu/screenshot-baseline.json"
MODE="${1:-check}"

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    if command -v flatpak-spawn >/dev/null 2>&1; then
        exec flatpak-spawn --host bash "$ROOT/test/qemu/run.sh" "$@"
    fi
    echo "[qemu-test] qemu-system-x86_64 is not installed" >&2
    exit 1
fi

[ -f "$KERNEL" ] || { echo "[qemu-test] missing $KERNEL"; exit 1; }
[ -f "$INITRAMFS" ] || { echo "[qemu-test] missing $INITRAMFS"; exit 1; }
if [ "$MODE" != check ] && [ "$MODE" != record ]; then
    echo "usage: $0 [check|record]" >&2
    exit 2
fi

WORK="$(mktemp -d)"
QMP="$WORK/qmp.sock"
SERIAL="$WORK/serial.log"
SCREENSHOT="$WORK/desktop.ppm"
QEMU_LOG="$WORK/qemu.log"
PID=""

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# GitHub's Arch Linux job runs QEMU inside a container without KVM. MTTCG has
# repeatedly segfaulted there before the guest executes its first instruction,
# so the portable fallback deliberately uses single-threaded TCG. Hardware KVM
# remains the fast path wherever it is available.
ACCEL=(-accel tcg,thread=single -cpu max)
if [ "${FIFI_QEMU_FORCE_TCG:-0}" != 1 ] &&
   [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL=(-enable-kvm -cpu host)
fi

echo "[qemu-test] booting hardened desktop..."
qemu-system-x86_64 \
    -M q35 "${ACCEL[@]}" -m 4G -smp 4 \
    -kernel "$KERNEL" -initrd "$INITRAMFS" \
    -append "console=tty0 console=ttyS0,115200 quiet loglevel=3 panic=-1 fifi_live fifi_selftest" \
    -no-reboot -display none \
    -device virtio-vga,xres=1920,yres=1080 \
    -device virtio-mouse-pci \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -serial "file:$SERIAL" \
    -qmp "unix:$QMP,server=on,wait=off" \
    >"$QEMU_LOG" 2>&1 &
PID=$!

RESULT=""
for _ in $(seq 1 120); do
    if grep -aq 'FIFI_SELFTEST PASS' "$SERIAL" 2>/dev/null; then
        RESULT=pass
        break
    fi
    if grep -aqE 'FIFI_SELFTEST FAIL|Kernel panic|not syncing|Attempted to kill init' \
        "$SERIAL" 2>/dev/null; then
        RESULT=fail
        break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        RESULT=exited
        break
    fi
    sleep 1
done

if [ "$RESULT" != pass ]; then
    echo "[qemu-test] boot failed (${RESULT:-timeout})"
    tail -n 80 "$SERIAL" 2>/dev/null || true
    tail -n 40 "$QEMU_LOG" 2>/dev/null || true
    exit 1
fi

python3 - "$QMP" "$SCREENSHOT" <<'PY'
import json
import socket
import sys
import time

qmp, screenshot = sys.argv[1:]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(50):
    try:
        sock.connect(qmp)
        break
    except OSError:
        time.sleep(0.1)
else:
    raise SystemExit("QMP socket did not become ready")

stream = sock.makefile("rwb", buffering=0)
stream.readline()
for command in (
    {"execute": "qmp_capabilities"},
    {"execute": "screendump", "arguments": {"filename": screenshot}},
):
    stream.write(json.dumps(command).encode() + b"\n")
    while True:
        reply = json.loads(stream.readline())
        if "return" in reply:
            break
        if "error" in reply:
            raise SystemExit(f"QMP command failed: {reply['error']}")
sock.close()
PY

if [ "$MODE" = record ]; then
    python3 "$ROOT/test/qemu/screenshot_diff.py" record "$SCREENSHOT" "$BASELINE"
else
    python3 "$ROOT/test/qemu/screenshot_diff.py" check "$SCREENSHOT" "$BASELINE"
fi

grep -a 'FIFI_SELFTEST PASS' "$SERIAL" | tail -1
echo "[qemu-test] boot, non-root app, and screenshot checks passed"
