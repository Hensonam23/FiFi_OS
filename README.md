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
│              FiFi Compositor (DRM/KMS)               │
│   Renders GUI directly on GPU framebuffer via KMS    │
│   Handles input via evdev (/dev/input/event*)        │
│   Hosts XWayland for Steam and third-party apps      │
├─────────────────────────────────────────────────────┤
│                  Linux Kernel                        │
│   GPU drivers · Audio · USB · Networking · Storage   │
└─────────────────────────────────────────────────────┘
```

The FiFi compositor is a native C process that takes exclusive control of the display via DRM/KMS — no X11 server, no GNOME, no desktop environment underneath. It renders the FiFi GUI directly onto the GPU framebuffer. Steam and other third-party apps run inside an XWayland instance managed by the compositor.

---

## What carries over from `main`

The entire visible product transfers — only the kernel layer is replaced:

| Component | Status |
|---|---|
| GUI compositor: window manager, z-order, drag/resize | Porting from bare-metal → DRM/KMS |
| Taskbar: launcher, window buttons, clock, volume tray | Carries over |
| Theme system: 16 accent presets, 5 wallpaper patterns | Carries over |
| File browser: list/grid view, sidebar, search, operations | Carries over |
| Text viewer/editor: syntax highlight, edit mode, undo | Carries over |
| Settings panel: theme, clock, audio, display | Carries over + new Linux settings |
| Shell (`ush`): pipes, redirects, job control, completion | Recompiles as Linux ELF binary |
| Audio: HDA driver, volume control | Replaced by PipeWire (Linux handles driver) |
| Kernel infrastructure: PMM, VMM, IDT, scheduler | Replaced by Linux (code not reused) |

---

## Current State

This branch was forked from `main` at Alpha v5.1. The bare-metal kernel is present but will be removed as Linux takes over each subsystem. Active work starts with the compositor port.

**Not yet functional as a Linux desktop — work in progress.**

---

## Roadmap

### Phase 1 — Linux Foundation
*Get to a bootable Linux system with a FiFi splash and minimal shell. ~4–6 weeks.*

- [ ] Minimal Linux kernel config (x86-64, DRM, GPU drivers, USB, ext4, networking)
- [ ] Custom initramfs: busybox userland, FiFi init script as PID 1
- [ ] Boot straight to a FiFi splash screen (no login, no desktop manager)
- [ ] `ush` shell compiled as Linux ELF, running as the initial console
- [ ] ISO build system: replace bare-metal Makefile with Linux + initramfs packaging
- [ ] QEMU test target: boots to FiFi splash + shell in QEMU with GPU passthrough
- [ ] Real hardware test: boots from USB on NVIDIA and AMD machines

### Phase 2 — FiFi Compositor (DRM/KMS)
*Port the GUI to run as a native Linux process. ~6–8 weeks.*

- [ ] Open `/dev/dri/card0` via libdrm, set video mode, map framebuffer
- [ ] Port `gui.c` rendering to write into the KMS framebuffer instead of Limine framebuffer
- [ ] Input via evdev: `/dev/input/event*` for keyboard, mouse, touchpad
- [ ] FiFi compositor process launched by init, takes over display at boot
- [ ] Full FiFi desktop running: taskbar, window manager, launcher, theme system
- [ ] Hardware cursor via KMS plane (replaces software cursor blitting)
- [ ] Multi-monitor: detect connected displays, mirror or extend
- [ ] Settings: display resolution and refresh rate picker (reads KMS connector info)

### Phase 3 — Native FiFi Apps
*Port apps to compile as standard Linux processes managed by the compositor. ~3–4 weeks.*

- [ ] `ush` shell runs inside a compositor-managed terminal window
- [ ] File browser compiled as standalone process, launched by compositor
- [ ] Text editor compiled as standalone process
- [ ] Settings panel as standalone process
- [ ] Inter-process communication between compositor and apps (Unix socket or shared memory)
- [ ] FiFi app protocol: how apps register windows, send input, receive paint events

### Phase 4 — Gaming (Steam + Proton)
*Wire up XWayland so Steam and Windows games run inside the FiFi desktop. ~2–3 weeks.*

- [ ] XWayland instance managed by the FiFi compositor
- [ ] Steam installed in the image, launches inside a FiFi window
- [ ] Proton configured and tested (Vulkan via Mesa/RADV or NVIDIA open drivers)
- [ ] GPU passthrough verified: games hit GPU at full speed, not software renderer
- [ ] Audio via PipeWire: game audio works, volume control wired to FiFi tray popup
- [ ] Gamepad input: HID gamepad events routed to the focused game window
- [ ] Gaming mode toggle in Settings: hides desktop chrome, max performance governor

### Phase 5 — Full System
*Everything that makes it a complete daily-driver OS. ~2–3 months.*

- [ ] USB live boot: ISO boots straight to FiFi desktop, no install required
- [ ] In-OS installer: "Install FiFi OS" option partitions disk, formats ext4, copies image
- [ ] WiFi: NetworkManager backend, FiFi WiFi UI in Settings (scan/connect/saved networks)
- [ ] Bluetooth: pairing UI in Settings, audio devices (A2DP/HFP via PipeWire)
- [ ] Package layer: minimal APT or custom FiFi package format for installing apps
- [ ] Browser: Firefox or LibreWolf in a FiFi window (required before v1.0)
- [ ] Image viewer: BMP/PNG/JPEG preview in file browser
- [ ] Desktop shortcuts: icons on the wallpaper that launch apps
- [ ] System monitor app: CPU, GPU, memory, network graphs
- [ ] FiFi app store stub: browse and install apps from a hosted repo

### v1.0
*The stable public release. Live USB OS, installer, full hardware support, browser, gaming.*

- [ ] Boots on any x86-64 machine without configuration
- [ ] Full desktop: browser, terminal, file manager, text editor, settings, system monitor
- [ ] Steam + Proton gaming verified on NVIDIA and AMD hardware
- [ ] USB installer: one click to install FiFi OS to disk
- [ ] Release zip: `fifi.iso` + cross-platform USB writers for Linux, macOS, Windows
- [ ] Public release at GitHub Releases

---

## Building (Phase 1 — in progress)

Build instructions will be updated as each phase lands. For now, the bare-metal kernel from `main` still builds here:

```sh
make iso      # builds the bare-metal ISO (temporary, will be replaced)
make rundbg   # run in QEMU
```

The Linux-based build system will replace this Makefile as Phase 1 progresses.

---

## Relationship to `main`

| | `main` (bare-metal) | `linux-desktop` (this branch) |
|---|---|---|
| Kernel | Hand-written from scratch | Linux (custom config) |
| GPU support | Software framebuffer only | Full DRM/KMS + NVIDIA/AMD |
| Steam/games | Not feasible | Yes, via Proton |
| Goal | Research, learning, hobby | Daily-driver gaming OS |
| Status | Alpha v5.1, active | Phase 1, starting |

Discoveries from `main` — hardware driver knowledge, framebuffer rendering, audio, input handling — all feed into this branch. The two tracks inform each other.

---

## License

MIT
