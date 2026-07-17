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
+------------------------------------------------------+
|                     FiFi Desktop                     |
|  Taskbar / Windows / File Browser / Terminal / Apps  |
+------------------------------------------------------+
|              FiFi Compositor (Wayland)               |
|   Hand-rolled Wayland server, software-composited    |
|     XWayland for X11 apps, own X window manager      |
|   Input via evdev, PTY terminal with a real shell    |
+------------------------------------------------------+
|                  Linux Kernel (zen)                  |
|   GPU drivers / Audio / USB / Networking / Storage   |
+------------------------------------------------------+
```

The FiFi compositor is a native, static C program that takes exclusive control of the display. No GNOME, no KDE, no desktop environment underneath. It is its own Wayland compositor and its own X window manager (for XWayland apps), drawing every pixel of the desktop itself and handling all input.

---

## What works today

| Feature | Status |
|---|---|
| GUI compositor: window manager, z-order, drag/resize, rounded corners | **Working** |
| Searchable app launcher (kickoff): live filter over built-in + installed apps, real icons | **Working** |
| Taskbar favorites: built-in + pinned apps as icons, drag-to-reorder, persist, running indicators | **Working** |
| App Store: install / launch / update / uninstall / search, Installed tab, live catalog | **Working** |
| App runtime: downloaded AppImages/apps launch via one unified launcher; real logos everywhere | **Working** |
| Desktop icons: drag to reposition (persisted), right-click menu, Properties, double-click to open | **Working** |
| Window controls for apps: LibreOffice gets a FiFi titlebar with close/minimize; LibreWolf draws its own minimize/maximize/close; Steam keeps its own chrome | **Working** |
| X11 apps via XWayland: built-in X window manager presents apps as titled FiFi windows, no black X-root border | **Working** |
| Update commands: `fifi update` / `fifi upgrade` check and apply app + OS updates from the terminal | **Working** |
| Wallpapers: smooth aurora plus 6 presets (Northern, Nebula, Dusk, Ocean, Spring, Ember), and 2K/4K image backgrounds with Fill/Fit/Stretch/Center | **Working** |
| System tray: battery (laptop-only, charge + charging bolt), volume, network, clock | **Working** |
| Tray hover tooltips: battery time-remaining, network IP, volume, memory, full date | **Working** |
| Clock calendar popup: month nav arrows + month/year picker, today highlighted | **Working** |
| Theme system: 16 accent presets, wallpapers, full-screen gradient | **Working** |
| File browser: list/grid view, sidebar, search, operations, crisp resize | **Working** |
| Text viewer/editor: syntax highlight, edit mode, undo | **Working** |
| Settings panel: theme, clock, audio, gaming, network, VPN | **Working** |
| PTY terminal: real shell in a FiFi window, multiple instances, full UTF-8 | **Working** |
| DRM/KMS display: direct GPU, no polling lag | **Working** |
| Audio: ALSA + PipeWire volume and routing | **Working** |
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
| Built-in offline AI: local llama.cpp model, terminal chat + agent, no internet | **Working** |
| Keyboard shortcuts: Alt+Tab, Ctrl+W, Win+L, Win+D, window snap | **Working** |
| Secure Boot: USB flashing signs EFI binaries, cert exported for enrollment | **Working** |
| In-OS installer: disk wizard, whole-disk and partition modes, GRUB dual-boot | **Working** |
| OS update: `update` (USB) or `fifi upgrade` (online, GitHub Releases) | **Working** |

---

## Current state

**Beta 1.0.** The build phases (Linux foundation, compositor, shell/terminal, display/gaming, security/privacy, full system) are all complete, followed by a full-codebase audit and optimization pass. The desktop runs on real hardware with DRM/KMS display, ALSA + PipeWire audio, XWayland for X11 apps (Steam, LibreOffice, browsers), WiFi via wpa_supplicant + iwd, and a full security suite.

Recent work:

- **App window controls.** LibreOffice (rootful XWayland) now gets a real FiFi titlebar with working close and minimize buttons above its menu bar, and the top bar auto-hides while it is maximized. LibreWolf draws its own titlebar with minimize/maximize/close. Steam stays borderless with its own chrome. Window buttons are large and easy to hit.
- **Update from the terminal.** `fifi update` checks for both app and OS updates; `fifi upgrade` applies them. Apps come from the App Store; the OS comes from GitHub Releases. An `xdg-open` shim lets apps open links in the browser (this also fixed LibreOffice's Download button).
- **Desktop visuals.** A smooth, seam-free aurora wallpaper plus six more presets, and 2K/4K image backgrounds with fit modes. Desktop icons open on double-click. The Files window re-renders crisply when resized.
- **Audit + optimization.** Correctness fixes (integer-overflow heap writes, use-after-free, resource leaks) and cleanup across the compositor and every app.

Earlier milestones include the full app ecosystem (searchable launcher, App Store, unified launcher, draggable desktop shortcuts), the UTF-8 terminal rewrite, the security feature set (DoH, WireGuard, Tor, scanners, IDS, AppArmor, LUKS2), the window-manager overhaul (clean stacking, multi-window, edge/corner resize), the offline AI assistant, the in-OS installer, and Secure Boot USB signing.

---

## Updating

FiFi separates app updates from OS updates and gives you one command for both.

```sh
fifi update      # check for app AND OS updates (changes nothing)
fifi upgrade     # apply everything (asks first); add -y to skip prompts
fifi version     # show the installed OS version
```

- **Apps** update from the App Store online.
- **The OS** (kernel + initramfs) updates from GitHub Releases on this repo: `fifi upgrade` compares the latest release to what is installed, downloads the `bzImage` + `initramfs.cpio.gz` assets into `/fifi-data/boot` (keeping `.prev` backups), and reboots into the new system.
- Offline alternative: `update` applies a new OS from a plugged-in FiFi USB. `app-update` updates apps only.

---

## Roadmap

**Scope:** Desktop, Laptop, Tablet, Phone. "Server" is not a separate build; it is the headless/CLI profile of the same platform. Embedded and automotive are out of scope.

**Two tracks, kept in parallel:**

- **Linux track** (this repo) is both the proving ground and a first-class product. It always ships and stays up to date. This is the daily driver for years.
- **Bare-metal track** is the end goal: a kernel written from scratch with no Linux underneath. It catches up subsystem by subsystem behind frozen shared APIs and eventually becomes the production kernel. Linux is never discarded; it is the co-development platform until bare metal is genuinely better.

**Near-term priority:** Linux **Desktop v1.0 is the sole goal.** Everything past it (ARM64, tablet, phone, bare-metal ARM) waits until Desktop v1.0 ships. Note also that the on-device AI ("Machine Spirit") stays a desktop/laptop feature and is designed as a cleanly-removable module, so mobile builds omit the local model entirely (a battery cannot run llama.cpp locally).

### Built so far

Phases 1 through 6 are complete: the Linux foundation and custom initramfs, the hand-rolled compositor and desktop, the PTY shell and terminal, DRM/KMS display and gaming, the security and privacy suite, and the full-system layer (installer, browser, LibreOffice, offline AI, unified settings). See **What works today** above. The project is at **Beta 1.0**.

### Where it is going

Timelines are active-effort ranges (steady part-time work with AI help). Hardware-gated items are marked with an hourglass.

- **Phase 0 - Harden and make it testable (blocks everything).** Strip the dev SSH key from release images, add a non-root user and re-enable per-app sandboxing, verify signatures/hashes on every download (apps, AI models, OS updates), run the compositor under a supervisor with auto-reboot, move OS update to A/B (never overwrite the only bootable copy), and build a screenshot-diff test harness plus a bare-metal QEMU self-test wired into CI.
- **Phase 1 - Consolidate the shared platform.** Extract the genuinely shared code (GUI toolkit, IPC protocol, config/theme formats, app framework) into one versioned library with a frozen, documented API consumed by both tracks, and stop the two branches from drifting.
- **Phase 2 - Desktop/Laptop 1.0 (Linux).** Finish the desktop UX and settings parity, ship an app framework/SDK and a verifying package manager, and do the gaming presentation rework: the compositor is CPU-only software compositing today, so it needs GPU-accelerated scanout/page-flip with vsync plus pointer-constraints and relative-pointer before FPS mouselook is possible. Then ship Desktop/Laptop 1.0, the first real release.
- **Phase 3 - ARM64 + Raspberry Pi CM5 bring-up (Linux).** Hourglass, gated by CM5 stock. Cross-compile the compositor and apps for aarch64, swap Intel/Mesa for the CM5's VideoCore VII (Mesa V3D), move the boot chain to u-boot/UEFI + device tree, and get FiFi Desktop running on the CM5 driving an external display.
- **Phase 4 - Touch and mobile foundations = FiFi Tablet.** Add a platform-neutral touch/gesture model, a DPI-aware touch-sized responsive toolkit, an on-screen keyboard, real power management and suspend/resume, rotation, notifications, and non-root per-app isolation. Milestone: a CM5 + touchscreen + battery handheld.
- **Phase 5 - FiFi Phone.** Hourglass, mostly hardware. Validate the software on a repairable phone (PinePhone Pro / Fairphone) first, then add cellular modem integration, a phone/SMS framework, audio routing, and the sensor stack. No built-in AI on the phone.
- **Phase 6 - Bare-metal catch-up (the end goal).** Continuous and multi-year. Behind the frozen APIs, port matured subsystems into the from-scratch kernel: fix the x86-64 ring0 holes, add the windowing syscalls, reach GUI/WM/IPC parity, add SMP, then the ARM64 port, then the irreducibly hard parts with no Linux head start (GPU driver, filesystem, TLS, power management).

### Hardware plan (the CM5 handheld/phone)

Target SoC is the Raspberry Pi Compute Module 5 (hourglass, when back in stock): the official IO board for bring-up, then a compact carrier for the handheld. The honest hard part is the display: a 2K, sunlight-readable (1000+ nit), small, capacitive DSI panel does not exist off-the-shelf in the hobby channel, so the plan is a 5-7" 1080p high-brightness panel first (get the software right), then chase a premium 6" 1440p 1000+ nit OLED via a DSI bridge for the phone. Power is LiPo + a PMIC/charge board with a fuel gauge; suspend/resume (Phase 4) is what makes the battery last. Bring-up order on real hardware: display + touch, then power, Wi-Fi/BT, audio, sensors, and (phone only) the modem.

### What accelerates with AI vs what does not

- **Fast (weeks):** compositor/UI/toolkit work, touch/gesture/on-screen keyboard, settings, app framework, the ARM64 software port, security hardening, tests/CI, the shared-library refactor.
- **Hardware-gated (months):** CM5 availability, panel sourcing and driving, battery/thermal, modem/RF, and every "debug it on the physical board" loop.
- **Irreducibly long (years):** the bare-metal kernel's from-scratch GPU/FS/TLS/power management and the ARM64 kernel port.

A full technical assessment (maturity scoring, technical debt, architectural risks, and the longer five/ten-year plans) lives in [`docs/PLATFORM_REVIEW.md`](docs/PLATFORM_REVIEW.md). The intended next-generation desktop shell design is in [`docs/design/`](docs/design/).

### v1.0 release checklist

- [ ] Boots on any x86-64 machine without configuration
- [ ] Full desktop: browser, terminal, file manager, text editor, settings, system monitor
- [ ] Steam and Proton gaming on NVIDIA and AMD hardware
- [ ] USB installer: one click to install to disk
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
```

### On the machine

```sh
setup           # first boot: clone the FiFi OS source into persistent storage
fifi upgrade    # check for and apply app + OS updates
update          # offline: apply a new OS from a plugged-in FiFi USB
```

Anything installed in `/fifi-data` and the source repo are untouched by updates.

---

## Security and Privacy

FiFi OS is built with three equal priorities: gaming performance, security, and privacy.

**Privacy:**
- No telemetry, ever. Nothing phones home by default.
- DNS encrypted by default. No plain-text DNS leaks.
- Camera and microphone off unless an app explicitly requests and you approve.
- No crash reports, no analytics, no usage data collected anywhere.

**Security:**
- Full disk encryption available. Your data is locked without your key.
- App sandboxing (per-app isolation is being hardened toward v1.0).
- OS updates from a signed source, with A/B rollback on the roadmap.
- Signed boot chain. Nobody can swap your kernel without you knowing.

**Offensive and defensive tools (legal use only):**
- Network scanner, port scanner, packet capture: for testing your own network or learning how things work.
- Password strength tester, vulnerability scanner: find problems before someone else does.
- Log monitor, intrusion detection, process integrity checker: know when something is wrong.
- These tools are included for legitimate security work and education, not for attacking systems you do not own.

---

## Built-in AI

A local AI assistant with no cloud, no account, and no data leaving the machine. It runs a small language model (via llama.cpp) entirely on your hardware, useful for help with the terminal, writing, and security analysis. Completely optional and completely offline. Chat from the terminal with `ai` or `fifi-ai`, run the agentic mode with `fifi-agent`, or keep a resident model loaded with `fifi-ai-serve`. It is a desktop/laptop feature by design and is omitted from future mobile builds.

---

## The two versions

There are two FiFi OS kernels developed together. The version described here runs on the Linux kernel and is the daily driver for everyday use and gaming, where the active work happens. The second runs on its own kernel written entirely from scratch with no Linux underneath: a longer-term, multi-year effort that catches up to the Linux version subsystem by subsystem behind shared APIs, and eventually becomes the production kernel. Linux always ships; bare metal always wins in the end.

---

## License

MIT
