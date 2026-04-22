# FiFi OS

A hobby x86-64 operating system built completely from scratch in C and assembly — no Linux, no POSIX, no libc, no borrowed kernel code. Every subsystem (memory management, scheduler, filesystem, networking, GUI) is written by hand and runs directly on the hardware.

It boots from a USB drive, runs a desktop environment, opens files, browses the web (HTTP), pings things, and saves your work across reboots. There is no root, no sudo, no package manager — it is a self-contained freestanding kernel that owns the whole machine from boot to desktop.

---

## What makes it a "real" OS

Most hobby OSes are a kernel with a shell. FiFi OS goes further:

- **Freestanding kernel** — no host OS underneath. Bare metal from the first instruction.
- **Own memory manager** — physical page allocator, virtual address space, per-process page tables, mmap/munmap.
- **Own scheduler** — preemptive, PIT-driven, supports fork/exec/waitpid, job control, signals.
- **Own filesystem** — three-layer VFS: initrd (read-only boot archive) → ramfs (session) → ext2 (persistent disk). Files survive reboots.
- **Own network stack** — Ethernet, ARP, IPv4, ICMP, UDP, DHCP, TCP, DNS, HTTP; wget downloads real files from the internet.
- **Own GUI** — pixel-framebuffer compositor with overlapping windows, drag/resize, themes, a file browser, text editor, and settings panel.
- **Own shell** — `ush`, with pipes, redirects, job control, tab completion, and script support.
- **Own drivers** — VirtIO block/net, RTL8168 GbE, XHCI USB keyboard, PS/2 keyboard, I2C-HID touchpad, VirtIO framebuffer.

There is no userspace supervisor, no dynamic linker, no system services. The kernel is the OS.

---

## Features

**Desktop**
- Multi-window GUI with taskbar, drag, resize, minimize, maximize, close
- 16 accent color presets and 5 wallpaper patterns (Gradient, Solid, Stars, Grid, Waves)
- 6 context-sensitive mouse cursor shapes (Arrow, Resize H/V, Text, Hand, Move)
- RTC wall clock in taskbar tray (reads CMOS hardware clock)
- Toast notifications; desktop info overlay (OS/kernel/memory/network stats)
- F1–F4 shortcuts to toggle windows; Alt+Tab window cycling

**File Browser**
- List view and icon grid view (toggle with V or the `#` toolbar button)
- Places sidebar (Home, Documents, Pictures, Downloads, Trash)
- Search bar, back/forward/up toolbar, column-header sorting
- File operations: create file/dir (Ctrl+N/D), rename (Ctrl+R), delete (Del)

**Text Viewer / Editor**
- Syntax highlighting for C, shell, Python, and assembly
- Edit mode (Ctrl+E): full cursor-based editing, Ctrl+S to save, undo/redo
- Horizontal scroll, word-wrap toggle (W), file reload (R)

**Settings**
- Accent color picker (16 swatches), wallpaper picker (5 patterns)
- 12h/24h clock toggle, animations on/off, status bar show/hide, desktop info toggle

**Shell (`ush`)**
- Pipes (`cmd1 | cmd2`), redirects (`>`, `>>`, `<`), background jobs (`&`)
- Job control: `bg`, `fg`, `jobs`, Ctrl-Z (SIGTSTP), Ctrl-C (SIGINT)
- Tab completion, 20-entry command history, shell variables, `source` scripting
- ANSI VT100 SGR color support (16 colors + bold)

**Networking**
- Ethernet, ARP (60s TTL cache), IPv4, ICMP ping, UDP, DHCP, TCP, DNS, HTTP
- `wget <url> [file]` downloads files from the internet
- Auto-DHCP at boot; `ifconfig`, `ping`, `dns`, `arp` commands
- Works on QEMU (virtio-net) and real hardware (RTL8168 GbE)

**Storage**
- ext2 filesystem: read, write, delete, mkdir, rename, stat
- Indirect block support (files up to ~265KB at 1KB block size)
- Three-layer VFS: ramfs session storage + initrd boot archive + ext2 persistent disk

**Kernel**
- x86-64, Limine bootloader, boots via UEFI or BIOS
- PMM (bump + free-list), VMM (4-level paging, HHDM), heap (kmalloc/kfree/kzalloc)
- IDT, ISR, PIC, PIT (100Hz), GDT/TSS, per-process CR3 (process isolation)
- ELF loader with W^X enforcement
- Preemptive scheduler: fork/exec/waitpid, argc/argv/envp
- Signals: SIGKILL, SIGTSTP, SIGCONT; user-space signal handler delivery
- 41 syscalls (NOP through UNAME)
- Workqueue, double-buffered framebuffer (backbuffer + VRAM flip at 100Hz)

**Hardware**
- VirtIO block (disk) and VirtIO net (QEMU)
- RTL8168 GbE — works on real hardware (tested: Lenovo Legion 5)
- XHCI USB keyboard driver — works on real laptops
- I2C-HID touchpad — Intel Meteor Lake LPSS / FocalTech FTCS0038 (Lenovo-specific)
- PS/2 Set 2 keyboard decoder
- RTC (CMOS) hardware clock

**USB Installer**
- Write `fifi.iso` to a USB drive and boot on real hardware
- Installers for Linux/macOS (`install.sh`) and Windows (`install.bat` / `install.ps1`)

---

## Quick Start (QEMU)

**Dependencies:** `clang`, `ld.lld`, `xorriso`, `limine`, `qemu-system-x86_64`, `parted`, `e2fsprogs`

```sh
# Arch Linux
sudo pacman -S clang lld xorriso limine qemu-system-x86_64 parted e2fsprogs

make iso      # build fifi.iso
make disk     # build blank ext2 data disk (requires sudo — mounts a loop device)
make rundbg   # build + run in QEMU (serial output to terminal)
make run      # build + run in QEMU (serial output to serial.log)
```

Once booted, DHCP runs automatically. The GUI starts immediately. Press F1 to open the Terminal, F2 for Files, F3 for Settings.

> **Note:** `make clean` deletes `disk.img`. Run `make disk` again after a clean build (requires sudo because it mounts a loop device to format ext2).

---

## Install to USB (bare metal)

Download the latest release from the [Releases](../../releases) page. The release zip contains `fifi.iso` and installers for every platform.

### Linux / macOS
```sh
chmod +x install.sh
./install.sh
```

### Windows
Right-click `install.bat` → **Run as administrator**

---

## Building from Source

```sh
git clone https://github.com/Hensonam23/FiFi_OS.git
cd FiFi_OS
make iso      # build fifi.iso only
make disk     # build blank ext2 data disk (requires sudo)
make rundbg   # build + run in QEMU
make release  # package fifi.iso + installers into build/fifi-os-release.zip
```

**Toolchain:** Clang (cross-compiles to x86-64 ELF), `ld.lld`, NASM for assembly stubs.
No GCC or binutils required.

---

## Shell Commands

| Command | Description |
|---------|-------------|
| `ls [path]` | list files |
| `cat file` | print file |
| `echo text` | print text |
| `cp src dst` | copy file |
| `mv src dst` | move/rename |
| `rm file` | delete file |
| `mkdir dir` | create directory |
| `stat file` | file info |
| `cd path` | change directory |
| `pwd` | print working directory |
| `export KEY=val` | set environment variable |
| `env` | list environment variables |
| `source script` | run shell script |
| `jobs` | list background jobs |
| `fg [%n]` | bring job to foreground |
| `bg [%n]` | resume job in background |
| `kill [-SIG] tid` | send signal to process |
| `ping <ip> [count]` | ICMP echo |
| `wget <url> [file]` | download file over HTTP |
| `dns <hostname>` | resolve hostname |
| `ifconfig [eth0 ip mask gw]` | show or configure network |
| `dhcp` | renew IP from DHCP server |
| `arp` | show ARP cache |
| `help` | list all builtins |

---

## Project Layout

```
kernel/
  src/          core subsystems (gui, shell, fs, net, drivers, ...)
  arch/x86_64/  CPU-specific code (IDT, ISR, GDT, context switch)
  include/       kernel headers
initrd/
  rootfs/       files baked into the kernel image at build time
tools/
  user/         user-space source (shell ush, libc ulibc, demo programs)
install.sh      Linux + macOS USB installer
install.ps1     Windows USB installer (PowerShell)
install.bat     Windows USB installer (double-click)
test_install.sh VM install test via loop device
Makefile        build system (iso, disk, run, release targets)
limine.conf     bootloader config (sets resolution, kernel path, initrd)
```

---

## Roadmap

> FiFi OS is in active alpha development. Releases are versioned as **Alpha vX.x** until the OS is stable, feature-complete, and ready for daily use as a live USB OS — at which point it ships as **v1.0**.

### Completed

- **Alpha v1.0** ✓ — interactive shell (`ush`), persistent ext2, signals, job control, mmap, ELF loader, USB installer
- **Alpha v1.1** ✓ — XHCI USB keyboard driver; boots and types on real laptops
- **Alpha v2.0** ✓ — Ethernet, ARP, IPv4/ICMP, UDP, DHCP, virtio-net + RTL8168; `ping` works on real hardware
- **Alpha v3.0** ✓ — TCP stack, DNS resolver, HTTP client, `wget`, framebuffer status bar (clock + IP), auto-DHCP at boot, heap/PIT/networking optimization pass
- **Alpha v3.01** ✓ — TCP checksum fix, heap large-alloc fix, net TX reentrancy, ramfs preload order
- **Alpha v4.0** ✓ — per-process page tables (fork clones CR3), ext2 write support, syscalls 34–41, boot splash on desktop, repo cleanup

### In Progress

- **Alpha v5.0** *(current)* — full desktop GUI environment
  - [x] Multi-window compositor: terminal, file browser, text viewer/editor, settings
  - [x] Taskbar: app launcher, window buttons, system tray (clock, IP address)
  - [x] Window chrome: title bar, close/max/min buttons, drag, resize, half-snap
  - [x] File browser: list + icon grid view, Places sidebar, search, toolbar, column sorting
  - [x] Text viewer/editor: syntax highlighting (C/sh/py/asm), edit mode, undo/redo, Ctrl+S save
  - [x] File operations: create, rename, delete, copy, move
  - [x] Theme system: 16 accent presets, 5 wallpaper patterns
  - [x] Settings: accent/wallpaper picker, clock format, animations, status bar, desktop info
  - [x] 6 context-sensitive cursor shapes, focus ring on active window
  - [x] RTC hardware clock in taskbar tray and status bar
  - [x] ANSI VT100 SGR colors in terminal (16 colors + bold)
  - [x] PSF2 font loading, PS/2 Set 2 decoder, I2C-HID touchpad
  - [x] F1–F4 shortcuts, Alt+Tab, keyboard navigation in file/text windows
  - [x] Toast notifications, desktop info overlay (neofetch-style)
  - [x] 2560×1440 (2K) framebuffer resolution

### Upcoming

- **Alpha v5.1** — audio driver (Intel HDA / AC'97), speaker output, volume control in Settings
- **Alpha v5.2** — image preview in file browser (BMP/PPM initially, then PNG/JPEG)
- **Alpha v5.3** — desktop icon shortcuts (launch apps directly from the wallpaper)
- **Alpha v5.4** — background image from file (load from disk/USB, set as wallpaper)
- **Alpha v5.5** — display settings: resolution picker, refresh rate control (based on detected monitor)
- **Alpha v6.0** — HTTPS/TLS (mbedTLS or custom), HTTP server, package downloader, more POSIX syscalls (pipe2, poll, select, readdir, getenv), WiFi driver (802.11 — bcm43xx or iwlwifi)
- **Alpha v6.1** — WiFi settings UI: scan networks, connect/disconnect, saved networks
- **Alpha v6.2** — Bluetooth stack (HCI over USB), Bluetooth audio (A2DP/HFP); pairing UI in Settings; goal: work with Bose QC headphones and similar devices
- **Alpha v7.0** — USB live → install to disk: GPT partition creation, filesystem formatting, bootloader installation, in-OS installer wizard UI, "Try FiFi OS" / "Install FiFi OS" boot menu
- **Alpha v7.1** — package/app manager stub: download and install apps from a FiFi repo over HTTP
- **Alpha v8.0** — internet browser (LibreWolf — a privacy-focused Firefox fork); **required before v1.0**
- **Alpha v8.1** — I2C-HID portability: read ECAM base from ACPI MCFG table, DW MMIO from BAR — currently hardcoded to Meteor Lake Lenovo hardware
- **Alpha v9.0** — polished app suite: advanced Settings (display, network, audio, users), driver viewer, system monitor
- **Alpha v9.1** — gaming optimizations: low-latency scheduler mode, gamepad input (HID), gaming mode toggle, CPU/GPU profiling overlay
- **Alpha v10.0+** — custom FiFi bootloader (replacing Limine), local AI assistant, GPU driver (DRM/KMS layer, Vulkan compute)
- **v1.0** — stable, complete, daily-driver live USB OS: desktop environment, internet browser, installer wizard, all core hardware working; ready for general use and public release

---

## License

MIT
