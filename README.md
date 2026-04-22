# FiFi OS — Linux Desktop Branch

> **Branch:** `linux-desktop` — gaming, security, privacy, and daily-driver target
> **Sibling:** `main` — the from-scratch bare-metal kernel (research track)

FiFi OS is a custom desktop operating system built from scratch — its own GUI, window manager, terminal, file browser, text editor, and settings panel. This branch runs that desktop on top of a minimal Linux kernel, giving it real hardware support while keeping everything you see as FiFi.

The Linux kernel is just the engine. Everything the user sees and touches is FiFi.

---

## What this branch is

The `main` branch of FiFi OS is a hand-written x86-64 kernel with no borrowed code. That continues as a long-term project.

This branch uses the Linux kernel as the hardware layer — GPU drivers, USB, audio, networking all handled — so FiFi can focus on being a great desktop. The result:

- Boots straight into the FiFi desktop, no login screen
- Runs Steam and Proton games
- Works on any x86-64 machine (NVIDIA, AMD, Intel)
- Live USB — try it without installing anything
- Has a built-in installer to write itself to disk
- Built for privacy: no telemetry, no tracking, minimal network exposure
- Built for security: hardened defaults, encrypted storage, offensive and defensive tools included

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    FiFi Desktop                      │
│  Taskbar · Windows · File Browser · Terminal · Apps  │
├─────────────────────────────────────────────────────┤
│              FiFi Compositor                         │
│   Renders GUI via DRM/KMS (direct GPU framebuffer)   │
│   Handles input via evdev (/dev/input/event*)        │
│   PTY-based terminal with real shell                 │
├─────────────────────────────────────────────────────┤
│                  Linux Kernel (zen)                  │
│   GPU drivers · Audio · USB · Networking · Storage   │
└─────────────────────────────────────────────────────┘
```

The FiFi compositor is a native C program that takes exclusive control of the display — no X11, no GNOME, no desktop environment underneath. It talks to the GPU directly via DRM/KMS and handles all input.

---

## What carries over from `main`

Everything visible transfers. Only the kernel layer is replaced:

| Component | Status |
|---|---|
| GUI compositor: window manager, z-order, drag/resize | **Working (Phase 2 ✓)** |
| Taskbar: launcher, window buttons, clock, volume tray | **Working** |
| Theme system: 16 accent presets, 5 wallpaper patterns | **Working** |
| File browser: list/grid view, sidebar, search, operations | **Working** |
| Text viewer/editor: syntax highlight, edit mode, undo | **Working** |
| Settings panel: theme, clock, audio, display | **Working** |
| PTY terminal: real shell (busybox sh) in a FiFi window | **Working (Phase 3 ✓)** |
| DRM/KMS display: direct GPU, no polling lag | **Working (Phase 4 ✓)** |
| Audio: ALSA volume control (slider works in UI) | **Working (Phase 4 ✓)** |
| Kernel infrastructure: PMM, VMM, IDT, scheduler | Replaced by Linux |

---

## Current State

**Phase 4 well underway — DRM/KMS display and ALSA audio working.**

FiFi desktop runs on Linux with a DRM/KMS display backend. Instead of polling the framebuffer on a timer, the compositor tells the GPU exactly when a frame is ready — immediate update, no stutter. Volume control in the FiFi taskbar and settings panel is wired to the real ALSA mixer. The SDL2 native runner gives smooth VSync-locked display for development. Both paths work.

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
- [x] Double-buffered rendering (backbuffer → dirty-row flip at 250 Hz)
- [x] Full FiFi desktop: taskbar, window manager, launcher, theme system
- [x] VFS mapped to `/fifi-data/` on real POSIX filesystem
- [x] RTC via `localtime()`, uptime via `CLOCK_MONOTONIC`
- [x] Static binary — no library dependencies in initramfs

### Phase 3 — Shell and Terminal ✓

- [x] PTY-based terminal: real shell (busybox sh) running in a FiFi window
- [x] Keyboard routed to PTY when terminal is focused, to GUI otherwise
- [x] F-key shortcuts always reach the GUI regardless of terminal focus
- [x] PTY window size calculated from font metrics and terminal geometry
- [x] SDL2 native runner: smooth VSync-locked display for development (no QEMU needed)
- [ ] File browser and text editor as separate processes (IPC)
- [ ] App protocol: how apps register windows and exchange paint events

### Phase 4 — Display and Gaming

- [x] DRM/KMS upgrade: compositor talks to GPU directly via `/dev/dri/card0`
- [x] virtio-gpu-pci: explicit per-frame flush instead of poll timer (QEMU smooth display)
- [x] Dirty-row tracking: only copies changed rows to GPU — much less work per frame
- [x] ALSA volume control: volume slider in FiFi taskbar controls real system audio
- [ ] PipeWire audio: game audio routing, multi-app mixing
- [ ] XWayland: run X11 apps (Steam, browsers) inside a FiFi window
- [ ] Steam installed in image, launches in a FiFi window
- [ ] Proton configured and tested (Vulkan via Mesa/RADV or NVIDIA open drivers)
- [ ] Gamepad input: HID events routed to focused game window
- [ ] Gaming mode toggle in Settings

### Phase 5 — Security and Privacy

- [ ] **Encrypted storage**: full disk encryption by default (LUKS2), key stretching with Argon2
- [ ] **AppArmor profiles**: each app runs with least privilege — compositor, browser, terminal sandboxed separately
- [ ] **Network firewall**: nftables rules, default-deny inbound, per-app outbound filtering
- [ ] **DNS over HTTPS**: system-wide encrypted DNS, no plain-text leaks
- [ ] **VPN integration**: WireGuard built in, one-click connect from Settings
- [ ] **Tor mode**: route all traffic through Tor from a Settings toggle
- [ ] **Offensive tools (legal)**: network scanner (nmap), vulnerability scanner, password strength tester, packet capture — bundled for pentesting and learning
- [ ] **Defensive tools**: intrusion detection (fail2ban-style), port scanner, log monitor, process integrity checker
- [ ] **Privacy mode**: block telemetry domains at the system level, camera/mic off by default, no crash reports sent anywhere
- [ ] **Secure boot**: signed bootloader and kernel, TPM-backed key storage
- [ ] **Automatic updates**: security patches applied silently, rollback on failure

### Phase 6 — Full System

- [ ] USB live boot: ISO boots straight to FiFi desktop
- [ ] In-OS installer: partitions disk, formats ext4, copies image, one click
- [ ] WiFi: NetworkManager backend, FiFi WiFi UI in Settings
- [ ] Bluetooth: pairing UI, A2DP audio via PipeWire
- [ ] Browser: Firefox or LibreWolf in a FiFi window
- [ ] Desktop shortcuts, system monitor, image viewer
- [ ] Built-in AI assistant: local model (Ollama/llama.cpp), no internet required, private by design

### v1.0

- [ ] Boots on any x86-64 machine without configuration
- [ ] Full desktop: browser, terminal, file manager, text editor, settings, system monitor
- [ ] Steam + Proton gaming on NVIDIA and AMD hardware
- [ ] USB installer: one click to install to disk
- [ ] Dual installer: bare-metal (main branch) or Linux-desktop at your choice
- [ ] Default encrypted, default private, default hardened
- [ ] Public release at GitHub Releases

---

## Building

```sh
# First time: clone linux-zen and apply FiFi kernel config (~1GB, a few minutes)
make linux-setup

# Build the kernel (~15 min first time, fast after)
make linux-kernel

# Build the compositor + initramfs (~5 seconds)
make linux-initrd

# Run in QEMU — full FiFi desktop, DRM/KMS display
make linux-run

# Serial debug mode
make linux-rundbg

# Native SDL2 window (smooth VSync, no QEMU needed)
make sdl-build
make sdl-run
```

---

## Security and Privacy Philosophy

FiFi OS is built with three equal priorities: **gaming performance**, **security**, and **privacy**.

**Privacy:**
- No telemetry, ever. Nothing phones home by default.
- DNS encrypted by default. No plain-text DNS leaks.
- Camera and microphone off unless an app explicitly requests and you approve.
- No crash reports, no analytics, no usage data collected anywhere.

**Security:**
- Full disk encryption on by default — your data is locked without your key.
- Each app runs in a sandbox. A compromised browser can't touch your files.
- Automatic security updates with rollback. Stay patched without babysitting it.
- Signed boot chain. Someone can't swap your kernel without you knowing.

**Offensive and defensive tools (all legal):**
- Network scanner, port scanner, packet capture — useful for pentesting your own network or learning how attacks work.
- Password strength tester, vulnerability scanner — find problems before someone else does.
- Log monitor, intrusion detection, process integrity checker — know when something is wrong.
- These tools are included for legitimate security work and education, not for attacking systems you don't own.

---

## Built-in AI (coming later)

The plan is a local AI assistant — no cloud, no account, no data leaving the machine. It runs a small language model (via llama.cpp) entirely on your hardware. Useful for help with the terminal, writing, and security analysis. Completely optional, completely offline.

---

## Relationship to `main`

| | `main` (bare-metal) | `linux-desktop` (this branch) |
|---|---|---|
| Kernel | Hand-written from scratch | Linux zen (custom config) |
| GPU support | Software framebuffer only | DRM/KMS direct GPU access |
| Steam/games | Not feasible | Yes, via Proton (Phase 4) |
| Security tools | Not yet | Phase 5 |
| Goal | Research, learning, hobby | Daily-driver gaming + security OS |
| Status | Alpha v5.1, active | Phase 4 complete |

---

## License

MIT
