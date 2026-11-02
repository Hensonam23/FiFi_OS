#!/usr/bin/env bash
# Build and stage NVIDIA's open kernel modules plus the matching userspace.
# The generated files live under build-linux/ and are never committed.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_SRC="${FIFI_LINUX_SRC:-${XDG_CACHE_HOME:-$HOME/.cache}/fifi-os/linux-src}"
if [ ! -s "$KERNEL_SRC/include/config/kernel.release" ] &&
   [ -s "$HOME/.cache/fifi-os/linux-src/include/config/kernel.release" ]; then
    KERNEL_SRC="$HOME/.cache/fifi-os/linux-src"
fi
OUT="$REPO_ROOT/build-linux/nvidia-open"

[ -s "$KERNEL_SRC/include/config/kernel.release" ] || {
    echo "[nvidia] FiFi kernel is not configured; run scripts/build-kernel.sh first" >&2
    exit 1
}
KERNEL_RELEASE="$(cat "$KERNEL_SRC/include/config/kernel.release")"

DRIVER_SRC="${FIFI_NVIDIA_SRC:-}"
if [ -z "$DRIVER_SRC" ]; then
    for candidate in /usr/src/nvidia-* /run/host/usr/src/nvidia-*; do
        [ -f "$candidate/Makefile" ] || continue
        DRIVER_SRC="$candidate"
    done
fi
[ -n "$DRIVER_SRC" ] || {
    echo "[nvidia] NVIDIA open-module sources were not found" >&2
    echo "[nvidia] Install nvidia-open-dkms, then rerun this script" >&2
    exit 1
}

DRIVER_VERSION="${DRIVER_SRC##*/nvidia-}"
case "$DRIVER_VERSION" in
    ''|*[!0-9.]*)
        echo "[nvidia] Cannot determine driver version from $DRIVER_SRC" >&2
        exit 1
        ;;
esac

CACHE_ROOT="${FIFI_NVIDIA_CACHE:-$HOME/.cache/fifi-os/nvidia-$DRIVER_VERSION-fifi}"
if [ ! -f "$CACHE_ROOT/Makefile" ]; then
    rm -rf "$CACHE_ROOT"
    mkdir -p "$CACHE_ROOT"
    cp -a "$DRIVER_SRC/." "$CACHE_ROOT/"
fi

run_build() {
    make -C "$KERNEL_SRC" -j"$(nproc)" modules
    make -C "$CACHE_ROOT" -j"$(nproc)" modules \
        SYSSRC="$KERNEL_SRC" IGNORE_PREEMPT_RT_PRESENCE=1
}

NEED_BUILD="${FIFI_NVIDIA_REBUILD:-0}"
for module in nvidia nvidia-modeset nvidia-uvm nvidia-drm; do
    if [ ! -f "$CACHE_ROOT/kernel-open/$module.ko" ] ||
       ! strings "$CACHE_ROOT/kernel-open/$module.ko" |
           grep -F "vermagic=$KERNEL_RELEASE " >/dev/null; then
        NEED_BUILD=1
    fi
done
if [ "$NEED_BUILD" = 1 ]; then
    # Codex commonly runs in a Flatpak while both source trees are host-visible.
    # Compile on that host so GCC exactly matches FiFi's kernel compiler.
    if [ -e /.flatpak-info ] && command -v flatpak-spawn >/dev/null 2>&1; then
        flatpak-spawn --host bash -lc \
            "make -C '$KERNEL_SRC' -j\"\$(nproc)\" modules && make -C '$CACHE_ROOT' -j\"\$(nproc)\" modules SYSSRC='$KERNEL_SRC' IGNORE_PREEMPT_RT_PRESENCE=1"
    else
        run_build
    fi
fi

rm -rf "$OUT"
mkdir -p "$OUT/modules" "$OUT/bin" "$OUT/lib" "$OUT/lib32" \
         "$OUT/firmware" "$OUT/share/vulkan/icd.d" \
         "$OUT/share/vulkan/implicit_layer.d" \
         "$OUT/share/glvnd/egl_vendor.d" "$OUT/share/nvidia"

for module in nvidia nvidia-modeset nvidia-uvm nvidia-drm; do
    src="$CACHE_ROOT/kernel-open/$module.ko"
    [ -f "$src" ] || { echo "[nvidia] Missing built module: $src" >&2; exit 1; }
    cp "$src" "$OUT/modules/"
    strings "$src" | grep -F "vermagic=$KERNEL_RELEASE " >/dev/null || {
        echo "[nvidia] $module.ko does not match $KERNEL_RELEASE" >&2
        exit 1
    }
done
printf '%s\n' "$DRIVER_VERSION" > "$OUT/driver.version"
printf '%s\n' "$KERNEL_RELEASE" > "$OUT/kernel.release"

HOST_USR=/usr
[ -d /run/host/usr/lib ] && HOST_USR=/run/host/usr
for tool in nvidia-modprobe nvidia-smi; do
    [ -x "$HOST_USR/bin/$tool" ] && cp -L "$HOST_USR/bin/$tool" "$OUT/bin/"
done

copy_driver_libs() {
    local source_dir="$1" destination="$2" item
    [ -d "$source_dir" ] || return 0
    for item in "$source_dir"/libnvidia*.so* \
                "$source_dir"/libcuda*.so* \
                "$source_dir"/libnvcuvid*.so* \
                "$source_dir"/libGLX_nvidia*.so* \
                "$source_dir"/libEGL_nvidia*.so* \
                "$source_dir"/libGLES*nvidia*.so*; do
        [ -e "$item" ] || continue
        cp -a "$item" "$destination/"
    done
}
copy_driver_libs "$HOST_USR/lib" "$OUT/lib"
copy_driver_libs "$HOST_USR/lib32" "$OUT/lib32"

cp "$HOST_USR/share/vulkan/icd.d/nvidia_icd.json" \
   "$OUT/share/vulkan/icd.d/"
cp "$HOST_USR/share/vulkan/implicit_layer.d/nvidia_layers.json" \
   "$OUT/share/vulkan/implicit_layer.d/"
cp "$HOST_USR/share/glvnd/egl_vendor.d/10_nvidia.json" \
   "$OUT/share/glvnd/egl_vendor.d/"
cp "$HOST_USR/share/nvidia/"nvidia-application-profiles-* \
   "$OUT/share/nvidia/"

FIRMWARE_SRC="$HOST_USR/lib/firmware/nvidia/$DRIVER_VERSION"
[ -d "$FIRMWARE_SRC" ] || {
    echo "[nvidia] Matching $DRIVER_VERSION firmware was not found" >&2
    exit 1
}
cp -a "$FIRMWARE_SRC/." "$OUT/firmware/"

echo "[nvidia] staged open driver $DRIVER_VERSION for $KERNEL_RELEASE"
echo "[nvidia] output: $OUT ($(du -sh "$OUT" | cut -f1))"
