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
| OS update: plain `update` from stable/test channels, verified staging, USB fallback, rollback | **Working** |

---

## Current state

**Beta 1.0.** The build phases (Linux foundation, compositor, shell/terminal, display/gaming, security/privacy, full system) are all complete, followed by a full-codebase audit and optimization pass. The desktop runs on real hardware with DRM/KMS display, ALSA + PipeWire audio, XWayland for X11 apps (Steam, LibreOffice, browsers), WiFi via wpa_supplicant + iwd, and a full security suite.

Recent work:

- **App window controls.** LibreOffice (rootful XWayland) now gets a real FiFi titlebar with working close and minimize buttons above its menu bar, and the top bar auto-hides while it is maximized. LibreWolf draws its own titlebar with minimize/maximize/close. Steam stays borderless with its own chrome. Window buttons are large and easy to hit.
- **Update from the terminal.** Plain `update` now applies an in-place OS update from the selected stable or test channel. `fifi update` remains the check-only command and `fifi upgrade` applies app and OS updates together. An `xdg-open` shim lets apps open links in the browser (this also fixed LibreOffice's Download button).
- **Desktop visuals.** A smooth, seam-free aurora wallpaper plus six more presets, and 2K/4K image backgrounds with fit modes. Desktop icons open on double-click. The Files window re-renders crisply when resized.
- **Audit + optimization.** Correctness fixes (integer-overflow heap writes, use-after-free, resource leaks) and cleanup across the compositor and every app.

Earlier milestones include the full app ecosystem (searchable launcher, App Store, unified launcher, draggable desktop shortcuts), the UTF-8 terminal rewrite, the security feature set (DoH, WireGuard, Tor, scanners, IDS, AppArmor, LUKS2), the window-manager overhaul (clean stacking, multi-window, edge/corner resize), the offline AI assistant, the in-OS installer, and Secure Boot USB signing.

---

## Updating

FiFi updates in place. It does not repartition or reinstall the machine, and
everything under `/fifi-data` is preserved.

```sh
update                 # download and apply from the selected channel (asks first)
update -y              # apply without prompting
update --check         # check without changing anything
update test            # select the test channel and update
update stable          # return to stable and update
update channel         # show the selected channel
update rollback        # swap back to the previous kernel + initramfs
update usb             # offline update from a plugged-in FiFi USB

fifi update            # check app and OS updates without changing anything
fifi upgrade           # apply app and OS updates together
fifi version           # show the installed OS version
```

- **Apps** come from the catalog embedded in the signed OS image. GitHub and
  GitLab SHA-256 metadata is checked before an AppImage is installed; offline
  USB bundles carry matching hash sidecars.
- **AI models** are checked against their Hugging Face Git LFS SHA-256 object
  IDs before activation.
- **The OS** downloads a matching kernel, initramfs, and Ed25519-signed manifest from GitHub Releases. The signature and both SHA-256 hashes must pass before the inactive A/B slot is written. Boot selection changes only after the complete pair is durable.
- Update checks, downloads, and prompts run as the normal desktop user. A fixed
  root-broker action copies the staged files into root-owned snapshots and
  repeats the signature, checksum, and gzip checks before writing a boot slot.
- A pending slot is confirmed only after the desktop compositor is ready. New installations record each GRUB boot attempt and automatically select the previous slot if the pending image does not reach that confirmation point. `update rollback` switches slots manually.
- Existing installations migrate their matching EFI GRUB menu automatically on the first hardened boot. The old menu is retained as `grub.cfg.before-ab-migration`; unrelated EFI partitions are never rewritten.
- `app-update` updates apps only.
- A bootable USB includes **Update Installed FiFi OS**: select it once and FiFi
  installs the verified boot pair, powers off safely, then completes migrations
  and application updates automatically on the next normal boot.

### Moving an existing laptop onto the test channel

The updater installed on older FiFi images only understands USB updates. Use
the bootable FiFi USB's **Update Installed FiFi OS** entry once; this avoids
downloading and executing an updater that the old image cannot authenticate.
Applications, settings, models, and user files remain untouched. Future updates
are simply:

```sh
update
```

The test ISO remains an optional recovery and live hardware-test path, not a
requirement for updating an existing installation.

### Developer verification and publishing

```sh
make linux-update-test    # simulated install, corruption, no-op, and rollback tests
make linux-test-update    # real kernel + test-channel initramfs + release package
make linux-test-usb       # also produce the hardware-test ISO
make linux-publish-test   # publish update assets (+ ISO when built); never commits/pushes
```

`linux-publish-test` refuses a dirty working tree. Verify locally first, commit
and push the reviewed `linux-desktop` changes, rebuild with
`make linux-test-update`, and only then publish the fixed `linux-desktop-test`
prerelease. Running `make linux-test-usb` first also includes the optional live
hardware-test and recovery ISO.

### Remote access

SSH is disabled by default and no authorization key is included in FiFi OS.
To opt in on a specific installation, create
`/fifi-data/ssh/authorized_keys` with your public key, set it to mode `600`,
and reboot. Dropbear then starts with password authentication and port
forwarding disabled. Removing that file and rebooting disables SSH again.
When an older installation first boots a hardened image, FiFi automatically
removes its historical development key while preserving different owner-added
keys. The pre-migration file is retained as
`authorized_keys.before-ssh-hardening`, but is never used for authentication.

---

## Roadmap

**Active development:** The Linux version is the sole focus for the foreseeable future. FiFi OS first ships as a polished, reliable Linux-based desktop/laptop system; future ARM64, tablet, phone, and headless profiles also use Linux. Embedded and automotive are out of scope.

**Long-term direction:** Bare-metal remains a future project, not an active parallel track. Work on it resumes only after the Linux version is mature and reliably shipping; Linux development will not be delayed to maintain bare-metal parity in the meantime.

**Near-term priority:** **Desktop v1.0 is the sole goal.** Everything past it (ARM64, tablet, and phone) waits until Desktop v1.0 ships. The on-device AI ("Machine Spirit") stays a desktop/laptop feature and is designed as a cleanly-removable module, so mobile builds omit the local model entirely (a battery cannot run llama.cpp locally).

### The Linux plan

What was built at each stage, and what still has to happen for v1.0 and beyond. Timelines are active-effort ranges (steady part-time work with AI help). Hardware-gated items are marked with an hourglass.

#### Phase 1: Linux Foundation (done)

- [x] Minimal linux-zen kernel config (x86-64, DRM, evdev, virtio)
- [x] Custom initramfs: busybox userland, FiFi init script as PID 1
- [x] FiFi banner at boot
- [x] QEMU test target: `make linux-run`

#### Phase 2: FiFi Compositor (done)

- [x] `/dev/fb0` framebuffer backend
- [x] Port `gui.c` to compile as Linux userspace (platform stub headers)
- [x] Input via evdev: keyboard, mouse (relative + buttons)
- [x] Software cursor with save/restore
- [x] Double-buffered rendering (backbuffer to dirty-row flip at 250 Hz)
- [x] Full FiFi desktop: taskbar, window manager, launcher, theme system
- [x] VFS mapped to `/fifi-data/` on real POSIX filesystem
- [x] RTC via `localtime()`, uptime via `CLOCK_MONOTONIC`
- [x] Static binary with no library dependencies in initramfs

#### Phase 3: Shell and Terminal (done)

- [x] PTY-based terminal: real shell (busybox sh) running in a FiFi window
- [x] Keyboard routed to PTY when terminal is focused, to GUI otherwise
- [x] F-key shortcuts always reach the GUI regardless of terminal focus
- [x] PTY window size calculated from font metrics and terminal geometry
- [x] SDL2 native runner: smooth VSync-locked display for development (no QEMU needed)
- [x] IPC socket server: compositor listens on `/tmp/fifi-compositor.sock`
- [x] App protocol: connect, register window, push pixel frames, receive input events
- [x] File browser as standalone IPC process (PSF font, dir nav, mouse and keyboard)
- [x] Settings panel as standalone IPC process (system info, ALSA volume slider)

#### Phase 4: Display and Gaming (done)

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

#### Phase 5: Security and Privacy (done)

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

#### Phase 6: Full System (done)

- [x] In-OS installer: disk wizard, whole-disk and partition modes, dual-boot alongside Windows
- [x] OS update command: plain `update` applies the selected stable/test channel in place
- [x] Verified update staging: signed manifest, inactive A/B slot, rollback, and USB fallback
- [x] Terminal UTF-8: multi-byte sequences decoded correctly, OSC and alternate screen handled
- [x] Bluetooth: dbus + bluetoothd autostart at boot, pairing via `bt`/bluetoothctl, A2DP audio via PipeWire bluez5
- [x] Browser: LibreWolf in a FiFi window (chrome-text rendering and typing fixed)
- [x] LibreOffice: installed by default during installation
- [x] Desktop shortcuts, image viewer, draggable desktop icons
- [x] Font system: Settings gets a dropdown where each font name renders in its own font as a preview
- [x] Desktop themes: optional theme system with selectable styles
- [x] Built-in AI assistant: local model (llama.cpp), no internet required, completely offline; `ai`/`fifi-ai` chat, `fifi-agent` agentic mode, resident llama-server via `fifi-ai-serve`
- [x] AI chat app: windowed "FiFi AI" GUI client for the offline model, in the launcher
- [x] Unified Settings: single tabbed hub (Personalize, Wi-Fi, Network, System, Security, About) with a theme/wallpaper personalization tab; `fifi-settings <tab>` deep-links
- [x] Modernized desktop: accent-themed titlebars/dock/launcher/menus, rounded corners, glass, centered dock
- [x] Live trial: "Try FiFi OS (Live)" boot entry; LibreWolf + LibreOffice auto-provision on first boot
- [x] Full-codebase audit + optimization pass: correctness bugs fixed (integer-overflow heap writes, use-after-free, resource leaks), dead code and redundant comments removed, across the compositor and every app

Phases 1 through 6 put the project at **Beta 1.0**. The phases below are what remain.

#### Phase 7: Harden and make it testable (next; blocks v1.0)

- [x] Strip the dev SSH key from release images; keep SSH owner-opt-in
- [x] Non-root desktop identity; ordinary apps and browser content sandboxes restored
- [x] Privilege brokers for hardware/admin apps: Security Center, Wi-Fi, Settings, App Store, browser setup, updater, and installer run without root
- [x] Run Steam as namespace-root mapped to the non-root desktop identity
- [x] Verify signatures/hashes on every download (apps, AI models, OS updates)
- [x] Run the compositor under a PID 1 supervisor with automatic restart on crash
- [x] A/B OS updates: inactive-slot writes, boot confirmation, rollback, installed-GRUB migration, and two-boot EFI fallback gate
- [x] Screenshot-diff test harness + QEMU boot self-test wired into CI

#### Phase 8: Consolidate the Linux desktop platform

- [ ] Extract duplicated Linux desktop code (GUI toolkit, IPC protocol, config/theme formats, app framework) into versioned shared modules
  - [x] Centralize and version the compositor/application IPC message contract
  - [x] Share theme identifiers, palettes, and font-size choices across the compositor and Settings
  - [x] Share the persisted theme configuration keys and defaults
  - [ ] Move every native application onto the shared app-side IPC transport
    - [x] Centralize reliable writes, compositor registration, and frame uploads
    - [x] Migrate Calculator, File Browser, Image Viewer, Network Monitor, Security Center, Settings, System Monitor, and Wi-Fi
- [ ] Freeze and document the Linux desktop APIs so the compositor and applications stop drifting

#### Phase 9: Desktop/Laptop v1.0

- [ ] Desktop UX polish and settings parity
- [ ] App framework/SDK and a verifying package manager
- [ ] Gaming presentation rework: the compositor is CPU-only software compositing today; it needs GPU-accelerated scanout/page-flip with vsync
- [ ] Pointer-constraints and relative-pointer protocols (required for FPS mouselook)
- [ ] Ship Desktop/Laptop v1.0, the first real release (checklist below)

#### v1.0 release checklist

- [ ] Boots on any x86-64 machine without configuration
- [ ] Full desktop: browser, terminal, file manager, text editor, settings, system monitor
- [ ] Steam and Proton gaming on NVIDIA and AMD hardware
- [ ] USB installer: one click to install to disk
- [ ] Default encrypted, default private, default hardened
- [ ] Public release at GitHub Releases

#### Beyond v1.0

Goals in the order they unlock, each defined by the milestone that proves it. Mobile work happens on its own branch when it starts, so the desktop track stays stable.

- [ ] **ARM64.** Goal: FiFi Desktop running on a Raspberry Pi CM5 driving an external display. Cross-compile the compositor and apps for aarch64, swap Intel/Mesa for the CM5's VideoCore VII (Mesa V3D), move the boot chain to u-boot/UEFI + device tree. Hourglass, gated by CM5 stock.
- [ ] **FiFi Tablet.** Goal: a CM5 + touchscreen + battery handheld running FiFi. Needs a platform-neutral touch/gesture model, a DPI-aware touch-sized responsive toolkit, an on-screen keyboard, real power management and suspend/resume, rotation, notifications, and non-root per-app isolation.
- [ ] **FiFi Phone.** Goal: FiFi making calls and sending texts on real hardware, proven on a repairable phone (PinePhone Pro / Fairphone) first, then the custom CM5 carrier from the hardware plan below. Needs cellular modem integration, a phone/SMS framework, call audio routing, and the sensor stack. No built-in AI on the phone; mobile builds omit the local model.
- [ ] **Future bare-metal project.** Goal: eventually build the from-scratch kernel into a complete FiFi platform. This is intentionally deferred until the Linux version is mature, polished, and reliably shipping; it is not part of the current Linux roadmap or release gates.

### Hardware plan (the CM5 handheld/phone)

Target SoC is the Raspberry Pi Compute Module 5 (hourglass, when back in stock): the official IO board for bring-up, then a compact carrier for the handheld. The honest hard part is the display: a 2K, sunlight-readable (1000+ nit), small, capacitive DSI panel does not exist off-the-shelf in the hobby channel, so the plan is a 5-7" 1080p high-brightness panel first (get the software right), then chase a premium 6" 1440p 1000+ nit OLED via a DSI bridge for the phone. Power is LiPo + a PMIC/charge board with a fuel gauge; suspend/resume (the tablet goal above) is what makes the battery last. Bring-up order on real hardware: display + touch, then power, Wi-Fi/BT, audio, sensors, and (phone only) the modem.

A technical assessment of the longer-term platform plan lives in [`docs/PLATFORM_REVIEW.md`](docs/PLATFORM_REVIEW.md). Its bare-metal findings describe future work, while the root roadmap defines current Linux priorities. The intended next-generation desktop shell design is in [`docs/design/`](docs/design/).

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

# Headless boot, non-root app, and screenshot stability gate
make linux-qemu-test

# Two-boot UEFI/GRUB automatic fallback gate
make linux-boot-fallback-test

# Native SDL2 window (smooth VSync, no QEMU needed)
make sdl-build
make sdl-run

# Flash to USB (single EFI partition, Secure Boot signed, verified write)
sudo bash scripts/flash-linux-usb.sh /dev/sdX
```

### On the machine

```sh
setup           # first boot: clone the FiFi OS source into persistent storage
update          # check for and apply an OS update from the selected channel
fifi upgrade    # check for and apply both app and OS updates
update usb      # offline fallback from a plugged-in FiFi USB
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
- OS updates from a signed source into an inactive A/B slot; new installations automatically fall back after an unconfirmed boot.
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
