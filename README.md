# FiFi OS

A hobby x86-64 operating system built from scratch in C and assembly.

Boots on real hardware. Has a shell. Saves files. Runs programs. Pings things. That's the point.

---

## Features

- **Interactive shell** — pipes, redirects (`>`, `>>`, `<`), tab completion, command history, job control (`bg`, `fg`, `jobs`, `&`, Ctrl-Z)
- **Persistent storage** — ext2 filesystem; files survive reboots
- **Preemptive multitasking** — PIT-driven scheduler, fork/exec, waitpid
- **Signals** — `kill`, `signal()`, Ctrl-C / Ctrl-Z, user-space handlers
- **Memory mapping** — `mmap` / `munmap` with per-process VA watermark
- **ELF loader** — runs statically-linked user-space programs from the initrd
- **Networking** — Ethernet, ARP, IPv4, ICMP ping, UDP, DHCP; works on QEMU (virtio-net) and real hardware (RTL8168 GbE)
- **USB keyboard** — XHCI driver; works on real laptops with built-in USB keyboards
- **USB installer** — write to a USB drive and boot on real hardware (Linux, macOS, Windows)

---

## Quick Start (QEMU)

**Dependencies:** `clang`, `ld.lld`, `xorriso`, `limine`, `qemu-system-x86_64`, `parted`, `e2fsprogs`

```sh
# Arch Linux
sudo pacman -S clang lld xorriso limine qemu-system-x86_64 parted e2fsprogs

make         # build fifi.iso + disk.img
make rundbg  # run in QEMU (serial → terminal)
make run     # run in QEMU (serial → serial.log)
```

Once booted, the network is up automatically on QEMU. On real hardware, run `dhcp` to get an IP address.

---

## Install to USB (bare metal)

Download the latest release from the [Releases](../../releases) page.
The release zip contains `fifi.iso` and installers for every platform.

### Linux
```sh
chmod +x install.sh
./install.sh
```

### macOS
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
| `env` | list environment |
| `source script` | run script file |
| `jobs` | list background jobs |
| `fg [%n]` | bring job to foreground |
| `bg [%n]` | resume job in background |
| `kill [-SIG] tid` | send signal to process |
| `ping <ip> [count]` | send ICMP echo requests |
| `ifconfig [eth0 ip mask gw]` | show or set network config |
| `dhcp` | get IP address from DHCP server |
| `arp` | show ARP cache |
| `help` | list builtins |

---

## Project Layout

```
kernel/         kernel source (C + asm)
  src/          core subsystems
  arch/x86_64/  architecture-specific (IDT, ISR, context switch)
  include/      kernel headers
initrd/         initial ramdisk
  rootfs/       files baked into the kernel image
tools/          build tools and user-space programs
  user/         user-space source (shell, libc, programs)
docs/           design notes and roadmap
install.sh      Linux + macOS installer
install.ps1     Windows installer
install.bat     Windows double-click launcher
test_install.sh VM install test (loop device)
```

---

## Roadmap

- **v1.0** — interactive shell, persistent ext2, signals, job control, mmap, USB installer ✓
- **v1.1** — XHCI USB keyboard driver; boots and types on real laptops ✓
- **v2.0** — Ethernet, ARP, IPv4/ICMP, UDP, DHCP; ping works on real LAN hardware ✓
- **v3.0** — TCP, wget/HTTP client, DNS
- **v4.0** — framebuffer GUI

---

## License

MIT
