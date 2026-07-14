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
| GUI compositor: window manager, z-order, drag/resize, rounded corners | **Working** |
| Searchable app launcher (kickoff): live filter over built-in + installed apps, real icons | **Working** |
| Taskbar favorites: built-in + pinned apps as icons, drag-to-reorder, persist, running indicators | **Working** |
| App Store: install / launch / update / uninstall / search, Installed tab, live catalog | **Working** |
| App runtime: downloaded AppImages/apps launch via one unified launcher; real logos everywhere | **Working** |
| Desktop icons: drag to reposition (persisted), right-click menu (Open / Properties / Remove), Properties dialog | **Working** |
| X11 apps via XWayland: built-in X11 window manager presents apps (LibreOffice) as decorated, titled, borderless FiFi windows | **Working** |
| System tray: battery (laptop-only, charge + charging bolt), volume, network, clock | **Working** |
| Tray hover tooltips: battery time-remaining, network IP, volume, memory, full date | **Working** |
| Clock calendar popup: month nav arrows + month/year picker, today highlighted | **Working** |
| Theme system: 16 accent presets, wallpaper patterns, full-screen gradient | **Working** |
| File browser: list/grid view, sidebar, search, operations | **Working** |
| Text viewer/editor: syntax highlight, edit mode, undo | **Working** |
| Settings panel: theme, clock, audio, gaming, network, VPN | **Working** |
| PTY terminal: real shell in a FiFi window, multiple instances | **Working** |
| DRM/KMS display: direct GPU, no polling lag | **Working** |
| Audio: ALSA volume control (slider works in UI) | **Working** |
| Gamepad: evdev HID input routed to focused IPC app | **Working** |
| Gaming mode: CPU governor switch, uncapped frame rate | **Working** |
| WiFi: auto-connect, Intel AX-series firmware bundled | **Working** |
| Security Center: firewall, privacy mode, active connections | **Working** |
| DNS over HTTPS: system-wide encrypted DNS, toggle in Security Center | **Working** |
| VPN: WireGuard built in, one-click connect from Settings | **Working** |
| Tor mode: SOCKS5 proxy, bootstrap status in Security Center | **Working** |
| Network tools: scanner, port scan, packet capture, password tester | **Working** |
| Intrusion detection: log monitor, process integrity, listener scan | **Working** |
| AppArmor: compositor and security apps run with MAC profiles | **Working** |
| Encrypted storage: LUKS2 support, status shown in Security Center | **Working** |
| Keyboard shortcuts: Alt+Tab, Ctrl+W, Win+L, Win+D, window snap | **Working** |
| Context menus: right-click desktop and file browser | **Working** |
| Toast notifications: volume, lock, snap, and system actions | **Working** |
| Secure Boot: USB flashing signs EFI binaries, cert exported for enrollment | **Working** |
| In-OS installer: disk wizard, whole-disk and partition modes, GRUB dual-boot | **Working** |
| OS update: type `update` with USB plugged in to update without reinstalling | **Working** |

---

## Current State

**Beta 1.0 — Phases 5 and 6 (Full System) complete, followed by a full-codebase audit + optimization pass.**

FiFi desktop runs on Linux with DRM/KMS display, ALSA + PipeWire audio, XWayland for X11 app support (Steam, browsers), WiFi via wpa_supplicant + iwd, and a full security suite in the Security Center.

Phase 6 brings the in-OS installer, browser integration, and a full developer workflow. The installer is a 7-screen wizard that handles disk selection, formatting, GRUB install, and Windows dual-boot alongside existing installs. Type `setup` after booting to clone the source. Type `update` with the USB plugged in to update the OS in place without reinstalling or losing any downloaded tools.

The desktop now has a full app ecosystem. A searchable kickoff launcher lists every built-in and installed app with its real icon; the App Store installs, launches, updates, and uninstalls apps and tracks running services; downloaded AppImages run through one unified launcher so everything opens the same way. Desktop shortcuts can be dragged anywhere and stay put, right-clicked for an Open / Properties / Remove menu, and double-clicked to launch the app. X11-only apps run through a built-in X11 window manager over rootless-style XWayland: the compositor launches and manages XWayland itself, so an app like LibreOffice appears as an ordinary decorated FiFi window titled with the app's name, with no black X-root border. The launcher also handles bootstrap/downloader packages (Discord fetches its real client on first run) and RunImage bundles, pulling in the shared libraries downloaded apps expect. The taskbar is a single icon strip — the built-in apps (Terminal, Files, Settings, Viewer) sit alongside pinned favorites, each showing a running indicator when open, and favorites can be dragged to reorder and are remembered across reboots. The tray adds a laptop-only battery indicator (charge level, a charging bolt when plugged in, and hover-for-time-remaining) plus hover tooltips across every tray item, and clicking the clock opens a month calendar with a month/year picker.

The terminal received a full UTF-8 rewrite. Multi-byte sequences are now decoded correctly so modern terminal tools render cleanly. OSC sequences (window title), alternate screen, and cursor hide/show are all handled properly. The PTY grid tracks the terminal window: text wraps at the window border and re-flows live when the window is resized.

Phase 5 added a security-focused feature set: DNS over HTTPS via dnscrypt-proxy, WireGuard VPN with a settings panel, Tor mode with bootstrap status, a network scanner, nmap port scanner, packet capture, password strength tester, vulnerability scanner, and intrusion detection. AppArmor profiles run the compositor and security apps in MAC mode. LUKS2 encrypted storage status and EFI Secure Boot status are shown in Security Center.

The window manager was overhauled. Overlapping app windows stack cleanly with no title bar or outline showing through from the window behind. The terminal behaves like any other window: it comes to the front when you click it and apps cover it when raised. You can open multiple terminals from the start menu, and every window can be resized by dragging its edges or corners. Mouse wheel scrolls the topmost window under the cursor.

USB flashing signs the EFI binaries with your Secure Boot key and exports the certificate to the USB root so it can be enrolled in another machine's BIOS.

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
- [x] IPC window drag: grab any IPC app window by its title bar to move it
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

### Phase 5: Security and Privacy (done)

- [x] Keyboard shortcuts: Alt+Tab, Ctrl+W, F11/F12 volume, Win+L lock, Win+D show desktop, window snap
- [x] Numpad keys: all numpad digits and operators work in terminal and apps
- [x] Screen lock: Win+L locks the screen, password required to unlock
- [x] Firewall toggle: nftables on/off switch in Settings
- [x] Security Center app: firewall status, privacy mode (73 telemetry domains blocked), port scanner, active connections
- [x] Context menus: right-click desktop and file browser, scale with font size
- [x] Toast notifications: volume, lock, snap, show desktop, and other system actions
- [x] Window layering: overlapping windows stack cleanly, no bleed from windows behind
- [x] Terminal in the stack: terminal is a normal window, comes to front when clicked
- [x] Multiple terminals: open extra terminal windows from the start menu, each independent
- [x] Window resize: drag any edge or corner, terminal resizes cleanly with no artifacts
- [x] Mouse wheel: routes to whichever window is on top at the cursor
- [x] DNS over HTTPS: system-wide encrypted DNS via dnscrypt-proxy, toggle in Security Center
- [x] VPN integration: WireGuard built in, connect/disconnect from Settings and Security Center
- [x] Tor mode: toggle in Security Center, SOCKS5 on port 9050, bootstrap status shown
- [x] Network scanner: detect live hosts on local subnet (nmap -sn)
- [x] Port scanner: nmap service/version scan on any target
- [x] Packet capture: tcpdump-based capture in Security Center
- [x] Password strength tester: masked input, color-coded score
- [x] Intrusion detection: log monitor, process integrity check, unexpected listener detection
- [x] AppArmor: kernel built with MAC support, compositor and security center run in complain mode
- [x] Encrypted storage: cryptsetup/LUKS2 bundled, status shown in Security Center
- [x] Secure Boot status: EFI variable read and shown in Security Center
- [x] Automatic updates: version shown in Security Center with update link
- [x] WiFi Manager: scan, select, and connect to networks from a start-menu app
- [x] Secure Boot USB signing: EFI binaries signed on flash, cert exported to USB root for BIOS enrollment

### Phase 6: Full System

- [x] In-OS installer: disk wizard, whole-disk and partition modes, dual-boot alongside Windows
- [x] OS update command: `update` copies new kernel+initramfs from USB without reinstalling
- [x] Terminal UTF-8: multi-byte sequences decoded correctly, OSC and alternate screen handled
- [x] Bluetooth: dbus + bluetoothd autostart at boot, pairing via `bt`/bluetoothctl, A2DP audio via PipeWire bluez5
- [x] Browser: LibreWolf in a FiFi window (chrome-text rendering and typing fixed)
- [x] LibreOffice: installed by default during installation
- [x] Desktop shortcuts, image viewer
- [x] Font system: Settings gets a dropdown where each font name renders in its own font as a preview
- [x] Desktop themes: optional theme system with selectable styles
- [x] Built-in AI assistant: local model (llama.cpp), no internet required, completely offline — `ai`/`fifi-ai` chat, `fifi-agent` agentic mode, resident llama-server via `fifi-ai-serve`
- [x] AI chat app: windowed "FiFi AI" GUI client for the offline model, in the launcher
- [x] Unified Settings: single tabbed hub (Personalize, Wi-Fi, Network, System, Security, About) with a theme/wallpaper personalization tab; `fifi-settings <tab>` deep-links
- [x] Modernized desktop: accent-themed titlebars/dock/launcher/menus, rounded corners, glass, centered dock
- [x] Live trial: "Try FiFi OS (Live)" boot entry; LibreWolf + LibreOffice auto-provision on first boot
- [x] Full-codebase audit + optimization pass: correctness bugs fixed (integer-overflow heap writes, use-after-free, resource leaks), dead code and redundant comments removed, across the compositor and every app

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

# Flash to USB (single EFI partition, Secure Boot signed, verified write)
sudo bash scripts/flash-linux-usb.sh /dev/sdX

# Update an installed FiFi OS without reinstalling (plug in USB, then run from terminal)
update
```

### First boot after install

```sh
# Clone the FiFi OS source into persistent storage
setup
```

### Updating after that

```sh
# From the FiFi OS terminal with USB plugged in:
update
# Then reboot. Anything installed in /fifi-data and the source repo are untouched.
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

## Built-in AI

A local AI assistant with no cloud, no account, and no data leaving the machine. It runs a small language model (via llama.cpp) entirely on your hardware. Useful for help with the terminal, writing, and security analysis. Completely optional and completely offline. Chat from the terminal with `ai` or `fifi-ai`, run the agentic mode with `fifi-agent`, or keep a resident model loaded with `fifi-ai-serve`.

---

## The other version

There is a second version of FiFi OS that runs on its own kernel written entirely from scratch, with no Linux underneath. It is a separate, longer-term project for research and learning. The version described here is the one built for everyday use and gaming, and it is where the active work happens.

---

## License

MIT
