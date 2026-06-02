# FiFi OS

FiFi OS is a custom operating system for gaming, security, privacy, and everyday use. The whole desktop is its own: the window manager, taskbar, terminal, file browser, text editor, and settings are all built by hand. It runs on the Linux kernel so it works on real hardware and can play games, but everything you see and use is FiFi's own, not another desktop made to look different.

---

## What it is

FiFi OS lets Linux handle the hard, invisible parts (graphics cards, USB devices, sound, networking, and storage) so FiFi can focus on the part you actually use: the desktop. The result:

- Boots straight into the FiFi desktop with no login screen
- Runs Steam and Proton games
- Works on any x86-64 machine (NVIDIA, AMD, Intel)
- Live USB so you can try it without installing anything
- Has a built-in installer to write itself to disk
- Built for privacy: no telemetry, no tracking, minimal network exposure
- Built for security: hardened defaults, encrypted storage, offensive and defensive tools included

---

## Architecture

```
+-----------------------------------------------------+
|                    FiFi Desktop                      |
|  Taskbar / Windows / File Browser / Terminal / Apps  |
+-----------------------------------------------------+
|              FiFi Compositor                         |
|   Renders GUI via DRM/KMS (direct GPU framebuffer)   |
|   Handles input via evdev (/dev/input/event*)        |
|   PTY-based terminal with real shell                 |
+-----------------------------------------------------+
|                  Linux Kernel (zen)                  |
|   GPU drivers / Audio / USB / Networking / Storage   |
+-----------------------------------------------------+
```

The FiFi compositor is a native C program that takes exclusive control of the display. No X11, no GNOME, no desktop environment underneath. It talks to the GPU directly via DRM/KMS and handles all input.

---

## What works today

| Feature | Status |
|---|---|
| GUI compositor: window manager, z-order, drag/resize | **Working (Phase 2)** |
| Taskbar: launcher, window buttons, clock, volume/FPS tray | **Working** |
| Theme system: 16 accent presets, 5 wallpaper patterns | **Working** |
| File browser: list/grid view, sidebar, search, operations | **Working** |
| Text viewer/editor: syntax highlight, edit mode, undo | **Working** |
| Settings panel: theme, clock, audio, gaming, network | **Working** |
| PTY terminal: real shell (busybox sh) in a FiFi window | **Working (Phase 3)** |
| DRM/KMS display: direct GPU, no polling lag | **Working (Phase 4)** |
| Audio: ALSA volume control (slider works in UI) | **Working (Phase 4)** |
| Gamepad: evdev HID input routed to focused IPC app | **Working (Phase 4)** |
| Gaming mode: CPU governor switch, uncapped frame rate | **Working (Phase 4)** |
| App launcher: spawn IPC apps from GUI (Files, Settings, Gamepad) | **Working (Phase 4)** |

---

## Current State

**Phase 4 complete. Phase 5 (Security and Privacy) in progress.**

FiFi desktop runs on Linux with a DRM/KMS display backend, ALSA + PipeWire audio (multi-app mixing, PulseAudio-compatible), XWayland for X11 app support (Steam, browsers), a working IPC socket protocol for standalone apps, gamepad input routing, gaming mode, and an FPS counter in the taskbar tray. Steam launches via XWayland. Proton versions are detected and shown in the Proton Config panel. The compositor tells the GPU exactly when a frame is ready instead of polling on a timer. Both QEMU and SDL2 native runner work.

Phase 5 adds keyboard shortcuts (Alt+Tab, Ctrl+W, volume keys, window snap, numpad), screen lock, Super+D show desktop toggle, a firewall toggle in Settings, a Security Center with privacy controls, and context menus on the desktop and file browser. Toast notifications appear for all system actions. UI polish: context menus scale with font size, settings panel scrolls and clips correctly, right-click ring buffer fixed.

The window manager was reworked so overlapping app windows layer cleanly with no title bar or outline showing through from the window behind. The terminal now behaves like any other window in the stack: it comes to the front when you click it and apps cover it when you raise them. You can open multiple terminals from the start menu, and any window can be resized by dragging its edges or corners.

---

## Roadmap

### Phase 1: Linux Foundation (done)

- [x] Minimal linux-zen kernel config (x86-64, DRM, evdev, virtio)
- [x] Custom initramfs: busybox userland, FiFi init script as PID 1
- [x] FiFi banner at boot
- [x] QEMU test target: `make linux-run`

### Phase 2: FiFi Compositor (done)

- [x] `/dev/fb0` framebuffer backend
- [x] Port `gui.c` to compile as Linux userspace (platform stub headers)
- [x] Input via evdev: keyboard, mouse (relative + buttons)
- [x] Software cursor with save/restore
- [x] Double-buffered rendering (backbuffer to dirty-row flip at 250 Hz)
- [x] Full FiFi desktop: taskbar, window manager, launcher, theme system
- [x] VFS mapped to `/fifi-data/` on real POSIX filesystem
- [x] RTC via `localtime()`, uptime via `CLOCK_MONOTONIC`
- [x] Static binary with no library dependencies in initramfs

### Phase 3: Shell and Terminal (done)

- [x] PTY-based terminal: real shell (busybox sh) running in a FiFi window
- [x] Keyboard routed to PTY when terminal is focused, to GUI otherwise
- [x] F-key shortcuts always reach the GUI regardless of terminal focus
- [x] PTY window size calculated from font metrics and terminal geometry
- [x] SDL2 native runner: smooth VSync-locked display for development (no QEMU needed)
- [x] IPC socket server: compositor listens on `/tmp/fifi-compositor.sock`
- [x] App protocol: connect, register window, push pixel frames, receive input events
- [x] Hello World app demonstrates the full IPC round-trip
- [x] File browser as standalone IPC process (PSF font, dir nav, mouse and keyboard)
- [x] Settings panel as standalone IPC process (system info, ALSA volume slider)

### Phase 4: Display and Gaming (done)

- [x] DRM/KMS upgrade: compositor talks to GPU directly via `/dev/dri/card0`
- [x] virtio-gpu-pci: explicit per-frame flush instead of poll timer (QEMU smooth display)
- [x] Dirty-row tracking: only copies changed rows to GPU
- [x] ALSA volume control: volume slider in FiFi taskbar controls real system audio
- [x] ALSA test tone: test button plays a real sine wave at current volume via PCM ioctls
- [x] Gamepad input: evdev HID events detected, normalized, routed to focused IPC app
- [x] Gaming mode toggle: Settings panel button switches CPU governor and uncaps frame rate
- [x] FPS counter: live frame rate shown in taskbar tray when gaming mode is active
- [x] Gamepad visualizer app: shows live button/axis state
- [x] Launcher spawns apps: FiFi, Files, Settings, Gamepad launchable from taskbar
- [x] IPC window drag: grab any IPC app window by its top strip to move it
- [x] CPU frequency in Settings: reads from sysfs, shown in System Information panel
- [x] Gamepad status in Settings: shows Connected/None in Gaming section
- [x] IPC window close button: red X in top-right of each app window
- [x] IPC window z-ordering: click-to-front with repaint, topmost window wins hit-test
- [x] IPC taskbar buttons: each open IPC app gets a taskbar button
- [x] IPC window minimize: hide window, restore via taskbar button click
- [x] F-key pass-through: F1-F4 always reach GUI even when IPC app has keyboard focus
- [x] Screen blanking: display goes black after 5 minutes idle, any input wakes it
- [x] PipeWire audio: game audio routing, multi-app mixing (PulseAudio-compatible)
- [x] XWayland: run X11 apps (Steam, browsers) inside a FiFi window
- [x] Steam installed in image, launches in a FiFi window
- [x] Proton configured and tested (fifi-proton panel shows versions, Vulkan detected)

### Phase 5: Security and Privacy (in progress)

- [x] Keyboard shortcuts: Alt+Tab (cycle windows), Ctrl+W (close), F11/F12 (volume down/up)
- [x] Volume keys: dedicated media keys on any keyboard work for volume control
- [x] Window snap: Win+Left/Right snaps to half screen, Win+Up maximizes, Win+Down restores
- [x] Numpad keys: all numpad digits and operators work in terminal and apps
- [x] Taskbar click alignment fixed: buttons now match their visual position
- [x] Screen lock: Win+L locks the screen, password required to unlock
- [x] Super+D show desktop: hides all windows to show the desktop, press again to restore
- [x] Firewall toggle: nftables on/off switch in Settings Network section
- [x] Security Center app: firewall status, privacy mode (73 telemetry domains blocked), port scanner, active connections
- [x] Context menus: right-click on desktop opens app launcher menu, right-click in file browser shows file actions; menus scale with font size
- [x] Toast notifications: short overlay appears for volume changes, lock, show desktop, window snap, and other actions
- [x] Settings panel scroll and clip: long settings lists scroll correctly and content does not bleed outside the window
- [x] Right-click menu ring buffer fixed: opening menus mid-render no longer corrupts input state
- [x] Window layering: overlapping app windows stack cleanly, no title bar or outline bleeds through from the window behind
- [x] Terminal in the stack: the terminal is a normal window now, comes to the front when clicked and is covered when another window is raised
- [x] Multiple terminals: open extra terminal windows from the start menu, each one independent and resizable
- [x] Window resize: drag any edge or corner to resize; terminal windows resize cleanly with no leftover artifacts or stacked prompts
- [x] Mouse wheel: scroll routes to whichever window is on top at the cursor
- [ ] DNS over HTTPS: system-wide encrypted DNS with no plain-text leaks
- [ ] VPN integration: WireGuard built in, one-click connect from Settings
- [ ] Tor mode: route all traffic through Tor from a Settings toggle
- [ ] AppArmor profiles: each app runs with least privilege
- [ ] Encrypted storage: full disk encryption by default (LUKS2)
- [ ] Secure boot: signed bootloader and kernel, TPM-backed key storage
- [ ] Automatic updates: security patches applied in the background, rollback on failure
- [ ] Offensive tools: network scanner (nmap), vulnerability scanner, password strength tester, packet capture
- [ ] Intrusion detection: log monitor, process integrity checker, fail2ban-style blocking

### Phase 6: Full System

- [ ] WiFi: NetworkManager backend, FiFi WiFi UI in Settings
- [ ] Bluetooth: pairing UI, A2DP audio via PipeWire
- [ ] Browser: Firefox or LibreWolf in a FiFi window
- [ ] Desktop shortcuts, image viewer
- [ ] In-OS installer: partitions disk, formats ext4, copies image, one click
- [ ] Built-in AI assistant: local model (Ollama/llama.cpp), no internet required, completely offline

### v1.0

- [ ] Boots on any x86-64 machine without configuration
- [ ] Full desktop: browser, terminal, file manager, text editor, settings, system monitor
- [ ] Steam and Proton gaming on NVIDIA and AMD hardware
- [ ] USB installer: one click to install to disk
- [ ] Dual installer: choose the everyday Linux version or the from-scratch version at install time
- [ ] Default encrypted, default private, default hardened
- [ ] Public release at GitHub Releases

---

## Building

```sh
# First time: clone linux-zen and apply FiFi kernel config (about 1GB, takes a few minutes)
make linux-setup

# Build the kernel (about 15 min first time, fast after)
make linux-kernel

# Build the compositor and initramfs (about 5 seconds)
make linux-initrd

# Run in QEMU - full FiFi desktop, DRM/KMS display
make linux-run

# Serial debug mode
make linux-rundbg

# Native SDL2 window (smooth VSync, no QEMU needed)
make sdl-build
make sdl-run

# Flash to USB (single EFI partition, verified write)
sudo bash scripts/flash-linux-usb.sh /dev/sdX
```

---

## Security and Privacy

FiFi OS is built with three equal priorities: gaming performance, security, and privacy.

**Privacy:**
- No telemetry, ever. Nothing phones home by default.
- DNS encrypted by default. No plain-text DNS leaks.
- Camera and microphone off unless an app explicitly requests and you approve.
- No crash reports, no analytics, no usage data collected anywhere.

**Security:**
- Full disk encryption on by default. Your data is locked without your key.
- Each app runs in a sandbox. A compromised browser cannot touch your files.
- Automatic security updates with rollback. Stay patched without having to manage it yourself.
- Signed boot chain. Nobody can swap your kernel without you knowing.

**Offensive and defensive tools (legal use only):**
- Network scanner, port scanner, packet capture: useful for testing your own network or learning how things work.
- Password strength tester, vulnerability scanner: find problems before someone else does.
- Log monitor, intrusion detection, process integrity checker: know when something is wrong.
- These tools are included for legitimate security work and education, not for attacking systems you do not own.

---

## Built-in AI (coming later)

The plan is a local AI assistant with no cloud, no account, and no data leaving the machine. It runs a small language model (via llama.cpp) entirely on your hardware. Useful for help with the terminal, writing, and security analysis. Completely optional and completely offline.

---

## The other version

There is a second version of FiFi OS that runs on its own kernel written entirely from scratch, with no Linux underneath. It is a separate, longer-term project for research and learning. The version described here is the one built for everyday use and gaming, and it is where the active work happens.

---

## License

MIT
