# FiFi OS — Linux Desktop Branch

> **Branch:** `linux-desktop` — the gaming-capable, daily-driver target
> **Sibling:** `main` — the from-scratch bare-metal kernel (hobby/research track)

FiFi OS is a custom desktop operating system with its own GUI, shell, file browser, text editor, settings panel, and theme system. This branch runs that desktop on top of a minimal Linux kernel — giving it full GPU support, Steam/Proton compatibility, and real hardware coverage while keeping the entire FiFi identity intact.

Everything the user sees is FiFi OS. The kernel is infrastructure.

---

## What this branch is

The `main` branch of FiFi OS is a from-scratch x86-64 kernel — no Linux, no borrowed kernel code, every subsystem hand-written. That project continues as a long-term research and hobby track.

This branch takes a different approach: use the Linux kernel as the hardware abstraction layer (GPU drivers, USB, audio, networking — all solved), then build the FiFi desktop on top as a native compositor and app suite. The result is an OS that:

- Boots straight into the FiFi desktop with no login prompt
- Runs Steam and Proton games out of the box
- Works on any x86-64 hardware (NVIDIA, AMD, Intel GPU)
- Is distributed as a live USB image — try it without installing
- Has an in-OS installer to write itself to disk

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    FiFi Desktop                      │
│  Taskbar · Windows · File Browser · Terminal · Apps  │
├─────────────────────────────────────────────────────┤
│              FiFi Compositor                         │
│   Renders GUI on framebuffer (/dev/fb0 → DRM/KMS)   │
│   Handles input via evdev (/dev/input/event*)        │
│   Hosts XWayland for Steam and third-party apps      │
├─────────────────────────────────────────────────────┤
│                  Linux Kernel (zen)                  │
│   GPU drivers · Audio · USB · Networking · Storage   │
└─────────────────────────────────────────────────────┘
```

The FiFi compositor is a native C process that takes exclusive control of the display — no X11, no GNOME, no desktop environment underneath. It renders the FiFi GUI directly onto the framebuffer and handles all input.

---

## What carries over from `main`

The entire visible product transfers — only the kernel layer is replaced:

| Component | Status |
|---|---|
| GUI compositor: window manager, z-order, drag/resize | **Working (Phase 2 ✓)** |
| Taskbar: launcher, window buttons, clock, volume tray | **Working** |
| Theme system: 16 accent presets, 5 wallpaper patterns | **Working** |
| File browser: list/grid view, sidebar, search, operations | **Working** |
| Text viewer/editor: syntax highlight, edit mode, undo | **Working** |
| Settings panel: theme, clock, audio, display | **Working** |
| Shell (`ush`): pipes, redirects, job control, completion | Working in shell fallback mode |
| Audio: HDA driver, volume control | Stub (Phase 4: PipeWire) |
| Kernel infrastructure: PMM, VMM, IDT, scheduler | Replaced by Linux |

---

## Current State

**Phase 2 complete — FiFi desktop running on Linux.**

The full FiFi GUI (taskbar, window manager, file browser, text editor, settings, themes, clock, volume tray) runs as a Linux userspace process inside a minimal initramfs. QEMU boots to the FiFi desktop in under 3 seconds.

---

## Roadmap

### Phase 1 — Linux Foundation ✓

- [x] Minimal linux-zen kernel config (x86-64, DRM, evdev, virtio)
- [x] Custom initramfs: busybox userland, FiFi init script as PID 1
- [x] FiFi banner at boot
- [x] QEMU test target: `make linux-run`

### Phase 2 — FiFi Compositor ✓

- [x] `/dev/fb0` framebuffer backend
- [x] Port `gui.c` to compile as Linux userspace (platform stub headers)
- [x] Input via evdev: keyboard, mouse (relative + buttons)
- [x] Software cursor with save/restore
- [x] Double-buffered rendering (backbuffer → framebuffer flip at 100 Hz)
- [x] Full FiFi desktop running: taskbar, window manager, launcher, theme system
- [x] VFS mapped to `/fifi-data/` on real POSIX filesystem
- [x] RTC via `localtime()`, uptime via `CLOCK_MONOTONIC`
- [x] Static binary — no library dependencies in initramfs

### Phase 3 — Native FiFi Apps

- [ ] `ush` shell running inside a compositor-managed terminal window (PTY)
- [ ] File browser and text editor as standalone processes
- [ ] Settings panel as standalone process
- [ ] Inter-process communication: Unix socket or shared memory between compositor and apps
- [ ] App protocol: how apps register windows, send input, receive paint events

### Phase 4 — Gaming (Steam + Proton)

- [ ] DRM/KMS upgrade (from /dev/fb0 to direct GPU plane for higher resolution)
- [ ] XWayland instance managed by the FiFi compositor
- [ ] Steam installed in the image, launches inside a FiFi window
- [ ] Proton configured and tested (Vulkan via Mesa/RADV or NVIDIA open drivers)
- [ ] Audio via PipeWire: game audio, FiFi volume control wired up
- [ ] Gamepad input: HID gamepad events routed to focused game window
- [ ] Gaming mode toggle in Settings

### Phase 5 — Full System

- [ ] USB live boot: ISO boots straight to FiFi desktop
- [ ] In-OS installer: partitions disk, formats ext4, copies image
- [ ] WiFi: NetworkManager backend, FiFi WiFi UI in Settings
- [ ] Bluetooth: pairing UI, A2DP audio via PipeWire
- [ ] Browser: Firefox or LibreWolf in a FiFi window
- [ ] Desktop shortcuts, system monitor, image viewer

### v1.0

- [ ] Boots on any x86-64 machine without configuration
- [ ] Full desktop: browser, terminal, file manager, text editor, settings, system monitor
- [ ] Steam + Proton gaming verified on NVIDIA and AMD hardware
- [ ] USB installer: one click to install to disk
- [ ] Dual installer: choose bare-metal (main branch) or Linux-desktop version at install time
- [ ] Public release at GitHub Releases

---

## Building

```sh
# First time: clone linux-zen and apply FiFi kernel config (~1GB, takes a few minutes)
make linux-setup

# Build the kernel (~15 min first time, incremental after)
make linux-kernel

# Build the compositor + initramfs (~5 seconds)
make linux-initrd

# Launch in QEMU (GUI window — full FiFi desktop)
make linux-run

# Or serial mode (boots to shell fallback, compositor needs framebuffer)
make linux-rundbg
```

---

## Relationship to `main`

| | `main` (bare-metal) | `linux-desktop` (this branch) |
|---|---|---|
| Kernel | Hand-written from scratch | Linux zen (custom config) |
| GPU support | Software framebuffer only | Full /dev/fb0 → DRM/KMS |
| Steam/games | Not feasible | Yes, via Proton (Phase 4) |
| Goal | Research, learning, hobby | Daily-driver gaming OS |
| Status | Alpha v5.1, active | Phase 2 complete |

Discoveries from `main` — hardware driver knowledge, framebuffer rendering, audio, input handling — all feed into this branch. The two tracks inform each other.

---

## License

MIT
