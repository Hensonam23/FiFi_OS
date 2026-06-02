#!/bin/bash
# Launch FiFi OS linux-desktop in QEMU on the 32" monitor (DP-4, x=0..2560).
# XAUTHORITY changes every X session — derive it live instead of hardcoding.
cd /home/aaron/src/linux-desktop
export DISPLAY=:0
export XAUTHORITY="${XAUTHORITY:-$(ls -t /tmp/xauth_* 2>/dev/null | head -1)}"
export SDL_VIDEO_X11_FORCE_EGL=1          # NVIDIA 610 broke SDL GLX
export SDL_VIDEO_WINDOW_POS=300,150       # within DP-4 (0..2560); DP-0 (2560+) has YouTube
exec qemu-system-x86_64 -M q35 -enable-kvm -cpu host -m 4G -smp 4 \
  -kernel build-linux/bzImage -initrd build-linux/initramfs.cpio.gz -no-reboot -no-shutdown \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -device virtio-mouse-pci -device virtio-tablet-pci \
  -audiodev sdl,id=snd0 -device intel-hda,id=hda0 -device hda-output,audiodev=snd0 \
  -append "console=tty0 console=ttyS0,115200 quiet loglevel=3" \
  -device virtio-vga,xres=1920,yres=1080 -display sdl,gl=off \
  -serial file:serial-linux.log \
  -monitor unix:qemu-monitor.sock,server,nowait -qmp unix:qemu-qmp.sock,server,nowait
