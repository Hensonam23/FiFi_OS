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
PROFILE_SCREENSHOT="$WORK/profile-play.ppm"
BRAND_SCREENSHOT="$WORK/brand-menu.ppm"
VOLUME_SCREENSHOT="$WORK/volume-popup.ppm"
CLOCK_SCREENSHOT="$WORK/clock-popup.ppm"
NETWORK_SCREENSHOT="$WORK/network-settings.ppm"
HOTUNPLUG_SCREENSHOT="$WORK/pointer-hotunplug.ppm"
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
    -device pcie-root-port,id=fifi-pointer-port,chassis=5,slot=5 \
    -device virtio-tablet-pci,bus=fifi-pointer-port,id=fifi-test-pointer \
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

python3 - "$QMP" "$SCREENSHOT" "$PROFILE_SCREENSHOT" "$BRAND_SCREENSHOT" \
    "$VOLUME_SCREENSHOT" "$CLOCK_SCREENSHOT" "$NETWORK_SCREENSHOT" \
    "$HOTUNPLUG_SCREENSHOT" <<'PY'
import json
import socket
import sys
import time

(qmp, screenshot, profile_screenshot, brand_screenshot, volume_screenshot,
 clock_screenshot, network_screenshot, hotunplug_screenshot) = sys.argv[1:]
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


def command(execute, arguments=None):
    message = {"execute": execute}
    if arguments is not None:
        message["arguments"] = arguments
    stream.write(json.dumps(message).encode() + b"\n")
    while True:
        reply = json.loads(stream.readline())
        if "return" in reply:
            return
        if "error" in reply:
            raise SystemExit(f"QMP {execute} failed: {reply['error']}")


def move_to(x, y):
    # QEMU absolute axes use 0..32767. The virtio tablet avoids libinput's
    # acceleration of enormous synthetic relative deltas between controls.
    ax = round(x * 32767 / 1919)
    ay = round(y * 32767 / 1079)
    command("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
    ]})
    time.sleep(0.15)


def click():
    for down in (True, False):
        command("input-send-event", {"events": [
            {"type": "btn", "data": {"down": down, "button": "left"}}
        ]})
        time.sleep(0.15)


def capture(path, delay=0.5):
    time.sleep(delay)
    command("screendump", {"filename": path})


for execute, arguments in (
    ("qmp_capabilities", None),
    ("screendump", {"filename": screenshot}),
):
    command(execute, arguments)

# Exercise the real top-bar Play pill at 1920x1080. A press and release must
# reach gui_on_tick and the compositor profile controller. The
# second screenshot verifies that Play became the selected profile.
move_to(445, 38)
click()
capture(profile_screenshot)

# Exercise every interactive cluster in the Horizon bar. These coordinates are
# centers of real hit regions at the gate's fixed 1920x1080 resolution.
move_to(65, 38)               # FiFi brand/app menu
click()
capture(brand_screenshot)
click()                       # close app menu before opening status popups
move_to(1782, 38)             # volume
click()
capture(volume_screenshot)
move_to(1860, 38)             # clock
click()
capture(clock_screenshot)
move_to(1717, 38)             # network
click()
capture(network_screenshot, 1.5)

# A real laptop crash was found when a pointer disappeared while libinput still
# had queued events. Remove QEMU's tablet and wait past the supervisor restart
# delay; the open Settings window must remain, proving the compositor survived.
command("device_del", {"id": "fifi-test-pointer"})
capture(hotunplug_screenshot, 4.0)
sock.close()
PY

if [ -n "${FIFI_QEMU_SCREENSHOT_OUT:-}" ]; then
    cp "$SCREENSHOT" "$FIFI_QEMU_SCREENSHOT_OUT"
    cp "$PROFILE_SCREENSHOT" "$FIFI_QEMU_SCREENSHOT_OUT.profile.ppm"
    cp "$BRAND_SCREENSHOT" "$FIFI_QEMU_SCREENSHOT_OUT.brand.ppm"
    cp "$VOLUME_SCREENSHOT" "$FIFI_QEMU_SCREENSHOT_OUT.volume.ppm"
    cp "$CLOCK_SCREENSHOT" "$FIFI_QEMU_SCREENSHOT_OUT.clock.ppm"
    cp "$NETWORK_SCREENSHOT" "$FIFI_QEMU_SCREENSHOT_OUT.network.ppm"
    echo "[qemu-test] screenshot saved: $FIFI_QEMU_SCREENSHOT_OUT"
fi

if [ "$MODE" = record ]; then
    python3 "$ROOT/test/qemu/screenshot_diff.py" record "$SCREENSHOT" "$BASELINE"
else
    python3 "$ROOT/test/qemu/screenshot_diff.py" check "$SCREENSHOT" "$BASELINE"
fi
python3 "$ROOT/test/qemu/profile_click.py" "$SCREENSHOT" "$PROFILE_SCREENSHOT"
python3 "$ROOT/test/qemu/status_click.py" "$PROFILE_SCREENSHOT" \
    "$BRAND_SCREENSHOT" "$VOLUME_SCREENSHOT" "$CLOCK_SCREENSHOT" \
    "$NETWORK_SCREENSHOT"
python3 "$ROOT/test/qemu/hotunplug_survival.py" \
    "$NETWORK_SCREENSHOT" "$HOTUNPLUG_SCREENSHOT"

grep -a 'FIFI_SELFTEST PASS' "$SERIAL" | tail -1
echo "[qemu-test] boot, top-bar controls, non-root app, and screenshot checks passed"
