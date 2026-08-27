# FiFi Platform — Comprehensive Technical Assessment (Final)

*Scope: one git repo, two worktrees — `fifi-os` @38026cf (branch `bare-metal`, ~31.5K LOC) and `linux-desktop` @5505d5b (branch `linux-desktop`, ~78K LOC). `linux-desktop` is 191 commits ahead of the `bare-metal` merge-base and 0 behind (verified); `bare-metal` HEAD is the exact merge-base. All drift is one-directional (~3 months: 2026-04-21 vs 2026-07-14). Philosophy under assessment: "Linux is the proving ground; bare metal is the end goal."*

---

## 0. Scoring Methodology (stated once, applied uniformly)

Every score below uses ONE rubric: **distance from the project's OWN stated goal** — a high-performance, secure, "just-works" platform spanning desktop → mobile → server, with bare-metal as the eventual production kernel. Not "impressive for one developer," not "far along a demo." A score is a faithful function of verified evidence, published as a per-dimension vector so the composite is derivable and falsifiable rather than asserted.

**The 15 dimensions, weights, and subscores (0–10):**

| # | Dimension | Weight | Score | One-line basis (verified) |
|---|-----------|-------:|------:|---------------------------|
| 1 | Boot / lifecycle robustness | 8 | 4 | Real switch_root/persistence flow, but compositor is PID 1 with no supervisor + no `panic=` on any installed cmdline |
| 2 | Least-privilege / isolation | 10 | 1 | Everything root; sandboxes off; no per-app/IPC authorization; unverified AppImages run as root |
| 3 | Integrity / authenticity | 9 | 1 | Shipped dev SSH key + port 22 open; zero signature/checksum on any download or OS image |
| 4 | Privacy | 6 | 8 | No telemetry, offline-first AI, DoH/WG/Tor, LibreWolf — genuinely honored |
| 5 | Memory safety (bare-metal ring boundary) | 9 | 3 | W^X + copyin/out present, but two directly-reachable ring3→ring0 holes |
| 6 | Kernel core (mm/sched/syscall/fs) | 7 | 4 | Works uniprocessor; no SMP, no COW, PMM leak, 42-call ABI, ~265 KB file cap |
| 7 | Networking stack (bare-metal) | 5 | 7 | Full ARP/IP/UDP/TCP/DHCP/DNS/HTTP, real impl — genuinely strong |
| 8 | GUI/WM/compositor implementation | 8 | 5 | Works and feels real, but forked 3× and CPU-only presentation |
| 9 | Gaming presentation path | 7 | 2 | Single dumb buffer, no page-flip/vsync, no pointer-lock — mouselook structurally impossible |
| 10 | Shared-platform reality (code, not concept) | 8 | 3 | 10 of 54 kernel files shared; native bare-metal build won't link; empty bare platform layer |
| 11 | Test / CI gate | 9 | 0 | No CI, no `make test`, 0 asserts in FiFi-authored code on either branch |
| 12 | Docs / decision record | 5 | 2 | Stale pre-pivot; `decisions.md` records neither branch's reality |
| 13 | Mobile readiness | 6 | 2 | No OSK, touch defined-but-unadvertised, anti-mobile power behavior |
| 14 | ARM64 / multi-arch readiness | 6 | 2 | Zero arm64 arch dir; portable-C userspace by luck only |
| 15 | End-to-end integration | 6 | 6 | Real: boots to desktop, runs Firefox/Steam/Electron, App Store, AI, installer |

**Composite (weighted mean) = Σ(w·s)/Σw = 3.49 ≈ 3.5/10.** Reported as **3.5**, not rounded up — integration is already dimension 15, so rounding up "for integration" would double-count it, and a shipped remote-root backdoor + zero test gate argue against generosity.

The scalar section scores below (§§1–4,6–8) are roll-ups of the relevant rows. Where a single scalar buries a real split (security, bare-metal), the split is shown.

---

## 1. Overall Repo Maturity — 3.5/10

FiFi is a genuinely impressive one-developer platform with two real, bootable operating environments sharing a common GUI toolkit — but it is an advanced *prototype*, not a product. The Linux Desktop branch boots straight to a hand-rolled Wayland desktop on real hardware, runs Firefox/Steam/Electron/LibreOffice, has an App Store, an offline AI assistant, and an in-OS installer. The bare-metal kernel is a legitimately working from-scratch x86-64 OS (paging, preemptive scheduler, fork/exec, TCP/IP, ext2, GUI). That two-front achievement is rare, and the composite score should not be read as dismissing it.

But under the "distance from stated goal" rubric, maturity is capped by structural realities: (a) **zero automated test gate** anywhere (no CI, no `make test`; 0 `assert()` in FiFi-authored code — the only 53 asserts are in vendored `stb_truetype.h`); (b) **security-by-default is unmet** — everything runs as root, content sandboxes are explicitly disabled, and a real dev SSH key ships in every image (`initramfs/root/usr/share/fifi/authorized_keys`, 1 key, copied to `/root/.ssh` every boot at `init:321-324` while `nftables.conf:19` leaves port 22 open); (c) the "shared platform" is shared at the *header/concept* level but the GUI *implementation* has forked and the native bare-metal build no longer links; (d) docs are a stale pre-pivot snapshot and `decisions.md` never records the single biggest decision in the project's history (the Linux pivot).

The composite (3.5) is well past demo, far from production, and — to the project's credit — honest about most of this in its own code comments.

---

## 2. Linux Desktop Maturity — 5/10

The strongest half of the platform and the one under active development ("Beta 1.0"). It is a real, hardware-validated boot flow: GRUB → linux-zen → `initramfs/root/init` (569 lines) → optional `switch_root` into tmpfs → exec the compositor as session init, with `/fifi-data` as the ext4 persistence layer. It carries mature safety patterns (switch-once marker, `fifi_noswitch` escape hatch, verify-before-switch, marker-probe-every-partition detection, OS-managed-vs-user-data refresh policy, Windows chainload with backup, a dedicated safe-mode GRUB entry).

**Why 5, not 6 (reconciled with this doc's own findings):** the earlier draft's 6 rewarded demo breadth. But this is the same artifact whose least-privilege score is **1** and whose integrity score is **1** (§0 rows 2–3); all four top release-blockers in §9 live in *this* branch (shipped backdoor, root-everywhere, sandboxes off, PID-1-with-no-recovery); and §11 risk #2 calls its gaming presentation path structurally unmet (no pointer-lock → no FPS mouselook). A flagship "gaming PC" OS that ships a live remote-root backdoor, disables content sandboxes, and cannot do mouselook is not 60% of the way to production. **5 is the honest ceiling.**

What specifically holds it back:
- **The compositor runs as PID 1 with no supervisor and no `panic=` on any installed cmdline** (verified: `init:556-566` exec `/bin/fifi-compositor`; `fifi-install.sh:403` and `flash-linux-usb.sh:78` carry no `panic=`). Any compositor crash — or a returning `main()` on failed framebuffer open — becomes an unrecoverable kernel panic that hangs the box and dirty-unmounts `/fifi-data`. Highest-impact robustness gap.
- The 306 MB initramfs is copied whole into a 75%-of-RAM tmpfs on every boot; `OVERLAY_FS` is compiled in but unused.
- Latent multi-path bugs: `flash-linux-usb.sh` omits `fifi_live` (a live "Try" boot can adopt/modify an on-disk install), USB marker names diverge between the two build scripts, and the installer repack can bake `.fifi-realroot` into the installed image (silently disabling the tmpfs relocation Steam depends on).
- Config writes are non-atomic (`fopen(...,"w")` truncate-in-place); a power loss mid-write corrupts `fifi-settings.conf`, and the mtime hot-reload poller can torn-read it.

Verdict: the DE *works* and feels real; it is single-machine-tuned and not production-hardened.

## 3. Bare-Metal Kernel Maturity — 4/10 (networking axis ~7)

A genuinely working from-scratch x86-64 OS (rebuilds to a 5.2 MB ISO and boots to `FiFi>`): clean Limine boot, real 4-level paging with per-process address spaces and W^X, a truly preemptive priority scheduler with aging, fork/exec/waitpid/signals, a bounds-checked 42-call syscall ABI, a 3-layer VFS, ext2 read/write, a full ARP/IP/UDP/TCP/DHCP/DNS/HTTP stack, and real drivers (virtio, RTL8168, XHCI, PS/2, I2C-HID, Intel HDA).

**Why 4, not 5:** the earlier draft graded bare-metal on "mechanism-completeness for one dev" while grading Linux on product-readiness — two different axes, making the old 6-vs-5 comparison meaningless. Under the single "distance from its OWN stated goal" rubric (high-performance gaming across all device classes), a kernel that runs **zero third-party apps**, has **no SMP**, **two directly-reachable privilege-escalation holes**, a **permanent PMM leak**, a **~265 KB file cap**, and **no TLS** is *further* from its goal than the Linux branch is from a shippable desktop. It cannot sit one point under Linux. **4 is defensible; the composite scalar buries two things the dimension vector surfaces:** the strong networking stack (row 7 ≈ 7/10) and the two ring0 holes (row 5 = 3/10).

Architectural ceilings requiring rework, not extension:
- **No SMP / single core only** (`-smp 1`; no APIC/IOAPIC/LAPIC code anywhere). Scheduler, PIC, and timer are uniprocessor by construction. Largest gap for a gaming-first OS.
- Legacy 8259 PIC (blocks MSI/MSI-X, SMP IRQ routing); slow `int 0x80` syscalls (no `syscall`/`sysret` MSR path); eager full-copy `fork()` with no COW; fixed 64 MiB heap; `THREAD_MAX=32`; PMM manages only the single largest RAM region (rest silently wasted; `pmm_alloc_pages` has no free path — permanent leak); no IST fault stacks (a kernel stack overflow triple-faults with no diagnostic).
- **Two directly-reachable ring3→ring0 memory-corruption holes** (verified): `SYS_MUNMAP` page-aligns and calls `vmm_unmap_range_and_free` with **no bounds check** against `FIFI_USER_TOP` (`syscall.c:1162-1170`); since kernel higher-half PML4 entries are shared into every process, a user program can unmap/free kernel pages. `exec_load`'s `map_user_pages` (`exec.c:29-48`) maps any ELF `p_vaddr` with no user-range check (`elf.c` validates only file offsets). These violate CORE_REQUIREMENT #3 and become exploitable the moment App Store/AppImage ELFs are trusted on bare-metal.
- Stranded fixes: `heap.c` (`kmalloc_aligned` boundary bug, 4081–4095 B) and `mouse.c` (shift-UB) were fixed only on linux-desktop and never back-ported. Stale identity strings (`Alpha v4.0`/`v5.0`/`Beta 1.0` disagree across `syscall.c`, `shell.c`, READMEs).

Verdict: solid advanced-Alpha mechanisms with one genuinely strong subsystem (networking); **fix the two security holes and land SMP/APIC/COW before building more on top.**

## 4. Shared-Platform Maturity — 3/10

**Lead finding (the transfer boundary, quantified):** the Linux compositor compiles exactly **10 of the 54** files in `kernel/src` — `console.c` + `gui.c` + the eight `gui_*.c` split files (`fifi/compositor/Makefile:32-40`, verified). The other **~44/54** — `pmm`, `vmm`, `heap`, scheduler, `syscall`, `exec`, `fork`, `ext2`, the entire `tcp/ip/arp/udp/dhcp/dns/http` stack, and every driver (`virtio_blk`, `rtl8168`, `xhci`, `hda`, `i2c_hid`, `pci`) — are **never built or exercised on Linux**; the host Linux kernel provides all of it. This refutes the philosophy *as literally stated* ("prototypes/validates EVERY subsystem before bare-metal"). The proving ground validates the **GUI toolkit + console + the userspace app/IPC/config/AI framework** — and those genuinely transfer. It validates **none** of the kernel internals that are the entire point of the bare-metal end goal.

The unified-platform promise is *real at the header/API/concept layer* and *forked at the implementation layer*:
- **The native bare-metal build no longer links (headline).** The kernel `Makefile:70` OBJS lists only `$(BUILD)/gui.o` (no wildcard, no split objects), while `gui.c` calls `win_draw_chrome` (defined non-static at `gui_window.c:188`) 13 times plus other split-file symbols. Only the *compositor* Makefile was updated for the 9-file split. So `make kernel`/`make iso` from the linux-desktop worktree fails to link. **The branch whose sole strategic justification is "de-risk bare-metal before implementing it" can no longer emit the bare-metal artifact.** This is the divergence prediction already realized, not hypothetical.
- **The "shared" WM is forked inside the canonical branch.** `ipc.c` reuses `gui_window.c`'s `win_draw_chrome`, but `wayland.c` calls it 0 times and carries 21 of its own `ssd_*`/`hit_resize`/`resize_dir` chrome+resize helpers. The one artifact genuinely destined for both kernels already has two independent implementations on the proving-ground side alone. When bare-metal back-ports "the shared WM," which one is truth?
- **The 9-file GUI split does not exist on bare-metal at all** — that branch still has the pre-split monolith, ~13.4K new lines behind.
- **Shared headers are drifting on exactly the hot files.** Of `kernel/include/*`, 48 are byte-identical across branches but **5 already differ** — `console.h`, `gui.h`, `keyboard.h`, `mouse.h`, `vfs.h` (the actively-developed ones). Diffs are small and mostly `#ifdef __linux__`-guarded (still ABI-compatible today), but the sync mechanism is hand-editing with **no diff-guard**. Header-sharing is not a solved stable seam.
- **The bare platform layer is empty:** `fifi/platform/bare/` is a lone 195-byte `.keep`. There is no shared platform-API surface on the bare side and no compile-time contract forcing bare-metal to keep up.

**Why 3, not 4:** a "shared platform" whose native build does not link, whose bare platform layer is a stub, and where sharing is a one-time snapshot fork (191/0 drift) is barely shared *in practice*. 4 credits intent; **3 credits reality.** The genuine win — the compositor-compiles-kernel-GUI mechanism validating the userspace half — is real and preserved in the score's non-zero value.

---

## 5. Repository Architecture Review

- **Topology (verified):** single repo, two linked worktrees; `bare-metal` HEAD is the strict merge-base ancestor of `linux-desktop` (191 commits behind, 0 ahead). Sharing is a *snapshot fork*, not a live module.
- **Layout:** both share the top-level skeleton (Makefile, install scripts, `kernel/{src,arch,include}`, `docs/`). linux-desktop adds `fifi/` (compositor + `platform/{linux,sdl,bare}` + 18 IPC apps), `linux/` (zen kernel config), build scripts, `apps/appstore/`, and the only current architecture record — top-level `ROADMAP.md`.
- **Two build systems in one Makefile:** freestanding cross-compile (clang/lld/nasm, `-ffreestanding`, Limine, xorriso) for bare-metal; hosted static gcc for the compositor. The 60 KB `scripts/build-initramfs.sh` is the real image builder.
- **Platform abstraction is asymmetric/aspirational:** `fifi/platform/bare/` is a lone 195-byte `.keep`. The intended `platform/{linux,sdl,bare}` split is real only for linux+sdl; the "bare platform" is de-facto the `kernel/src` tree, so there is **no shared platform-API surface on the bare side** and no compile-time contract forcing bare-metal to keep up.
- **Docs are stale-in-time:** `CORE_REQUIREMENTS.md`, `docs/ROADMAP.md`, `ai-agent-plan.md`, `decisions.md`, `packages-plan.md` are byte-identical across both branches and describe the pre-pivot from-scratch OS. `packages-plan.md` still says FiFi "cannot run normal Linux packages yet" — contradicted by the shipping App Store. **`decisions.md` has one stale Feb-2026 entry and never records the Linux pivot, the boot architecture, the `/fifi-data` layout, the config format, the security model, or the logging/recovery contract.**

**Architectural verdict:** the skeleton and the compositor-compiles-kernel-GUI mechanism are sound and clever. The debt is: (1) triplicated window/WM code across three unrelated implementations, (2) no shared client SDK (protocol constants copy-pasted into 14+ apps), (3) a broken native build target, (4) an empty bare platform layer, and (5) documentation that describes neither branch's reality.

---

## 6. Mobile Readiness — 2/10

FiFi is a floating-window *desktop* DE with **no mobile UX layer in either branch**. The one asset is a decent evdev input abstraction that already parses multitouch slots — but collapses everything to a single mouse cursor and **never exposes `wl_touch` to apps** (verified: seat advertises only `KEYBOARD|POINTER` at `wayland.c:858`; `WL_SEAT_GET_TOUCH` opcode is defined but unhandled). Every mobile primitive is absent:
- **No on-screen keyboard anywhere** (grep: 0 hits) — a keyboard-less device literally cannot type a WiFi password or unlock PIN. The hardest single blocker.
- No display rotation/autorotate, no accelerometer/IIO, no responsive/portrait layout, no global scale factor (only font size), no touch-sized hit targets (8px resize margins, font-height rows).
- **Actively anti-mobile power behavior:** the compositor force-pins the CPU `performance` governor + unlocks turbo at startup (`main.c:184`), busy-polls at 2–4 ms / 60–240 fps, and "power management" is blanking the framebuffer to black after 1 h while the SoC runs full-tilt. A battery device would run hot and die in ~1–2 h.
- No real suspend/resume (no `/sys/power/state`, no logind/elogind, no lid/power-button handling), no notification center (only a single 80-char/3-s toast), no quick-settings, no BT pairing UI (despite a bundled BlueZ+PipeWire stack), no cellular/GPS/NFC/fingerprint/camera, no audio routing (ALSA-direct volume only).

Kernel config is x86 desktop/laptop only (no touchscreen/WWAN/NFC/IIO/GNSS classes; DRM = virtio/i915/amdgpu). Bare-metal is further behind (I2C-HID hardcoded to one Meteor Lake Lenovo touchpad). *(Well-calibrated at 2: floored by verified absence, credited for the evdev MT-slot parsing already present.)*

## 7. ARM64 Readiness — 2/10

Near-zero by intent, but the DE userspace is portable-by-luck. The shared GUI toolkit (`kernel/src/gui*.c`, `console.c`) is pure C11 with no inline asm/port-I/O/SIMD; the compositor renders in software into a DRM dumb buffer via raw ioctls (no libdrm); the Linux platform shim is POSIX-only. So the compositor + toolkit would *likely compile and run* on an aarch64 KMS/evdev box with trivial flag changes.

Everything around it is hard x86: the bare-metal kernel is 100% x86_64 (only `kernel/arch/x86_64` exists — no GIC/generic-timer/PSCI/device-tree/arm64 vectors); the kernel config layers on `x86_64_defconfig` and is Intel/AMD-only (no Panfrost/Panthor/Mali, no Rockchip, no DTB); every build/boot script hardcodes `x86_64_defconfig`/`bzImage`/`qemu-system-x86_64`/`grub x86_64-efi`; the initramfs is a **host x86_64 capture, not a cross-build** (copies `ld-linux-x86-64.so.2`, iris/i915 Mesa); and gaming is x86-native (Steam/Proton + x86_64-only AppImages). No `CROSS_COMPILE`/`ARCH=arm64` plumbing exists anywhere. The multi-device-class vision is unrecorded in any doc. *(Correctly credits the portable-C toolkit; floored by the total absence of an arm64 arch path.)*

## 8. Security Readiness — Privacy 8/10, Least-Privilege & Integrity 1–2/10

**These are orthogonal axes and must not be averaged into a single flattering number.** The earlier draft's "3" was propped up by privacy; scored on the pillar the goal actually requires (least-privilege), the evidence is near-floor.

**Privacy — 8/10 (genuinely honored end-to-end):** no telemetry, offline-first AI, DoH/WireGuard/Tor, telemetry-blocking hosts, LibreWolf. This is a real, verified strength.

**Least-privilege & integrity — 1–2/10 (near-floor):**
- **The entire OS runs as root** via a fake sudo shim; no privilege boundary, no app-vs-app isolation, no capability/permission model. IPC has no per-app authorization — any client can read the clipboard, set the wallpaper, open arbitrary files, inject desktop icons.
- **Browser/Electron content sandboxes are DISABLED** (`--no-sandbox` at `browser.c:660`, `MOZ_DISABLE_CONTENT_SANDBOX=1` at `fifi-run:312`); AppArmor runs complain-only over 2 profiles; AppImages get **zero signature/checksum verification** and run as root.
- **Shipped remote-root backdoor** (verified): a real dev ed25519 key baked into the image, copied to `/root/.ssh/authorized_keys` on every boot (`init:321-324`) while port 22 is open (`nftables.conf:19`). **Release blocker.**
- The AI agent executes arbitrary commands as root behind a bypassable string-classifier (first-token allowlist, then `sh -c "$whole_string"` — `echo hi && rm -rf ~` classifies as read-only); `auto` mode does the opposite of its own documentation and auto-runs mutating commands.
- Installer never invokes LUKS (plaintext ext4); OS image swaps are integrity- but not signature-verified; no A/B rollback.
- The bare-metal kernel has a stronger *hardware* boundary (ring0/3, W^X, copyin/out) but no app-permission model — and two ring3→ring0 holes (§3).

Neither branch has the untrusted-app sandbox + permission/consent framework the mobile end-goal requires. **Bundling privacy in to reach "3" hides that the security pillar specifically is at the bottom.**

---

## 9. Technical Debt (Ranked)

1. **Shipped dev SSH key + root-everywhere + disabled content sandboxes** — a single drive-by or malicious AppImage = full persistent compromise. Release-blocking.
2. **Two ring3→ring0 memory-safety holes on bare-metal** (unbounded `SYS_MUNMAP` at `syscall.c:1162`, unvalidated ELF `p_vaddr` at `exec.c:29`).
3. **Compositor runs as PID 1 with no supervisor / no `panic=`** — any crash hangs the machine, corrupts dirty ext4 `/fifi-data`.
4. **Zero automated tests / no CI on either branch** — 0 asserts in FiFi-authored code (53 exist only in vendored `stb_truetype.h`); the stranded heap.c/mouse.c fixes are the proof there is no regression gate.
5. **GUI toolkit forked** (9-file split on Linux, monolith on bare-metal) and **linux-desktop's native bare-metal build is broken** (`Makefile:70` OBJS lists only `gui.o`).
6. **No integrity/authenticity verification of any downloaded artifact** (AppImages, GGUF models, browser, OS update image).
7. **Triplicated window/WM code** (`gui_window.c` / `ipc.c` / `wayland.c`) — chrome/resize/snap/z implemented 3×; `wayland.c` forks the WM even though `ipc.c` reuses `gui_window.c`.
8. **No shared client SDK** — IPC protocol constants copy-pasted into 14+ apps; no version field in the connect handshake.
9. **Non-atomic config writes** + duplicated schema in two lossy writers.
10. **CPU-only compositing + single dumb buffer + no page-flip/vsync + no pointer-lock** — structurally fails the gaming-first requirement in the presentation path.
11. **Stale docs / empty `decisions.md`** — the biggest architectural decision (the pivot) is unrecorded.
12. **Empty `fifi/platform/bare/`** — no bare platform-API surface; no compile-time drift guard.

## 10. Refactoring Priorities (Ranked)

1. **Remove the baked SSH key; introduce a non-root user + privilege-drop; run system actions through a small audited helper.** Highest leverage — unlocks every other boundary.
2. **Close the two bare-metal ring3→ring0 holes** (bounds-check munmap, validate ELF `p_vaddr`, guard `ensure_table` against PML4 ≥ 256).
3. **Stop running the compositor as bare PID 1** — wrap in a tiny respawn supervisor and add `panic=10 panic_on_oops` to installed cmdlines.
4. **Bare-metal build hygiene NOW (decoupled from SMP):** fix `Makefile:70` OBJS (add `gui_window.o`/`gui_taskbar.o`/etc.), back-port the mechanical 9-file GUI split + the stranded `heap.c`/`mouse.c` fixes, and add a `make kernel` smoke build to CI. This is a ~1-day fix today; deferred three years it becomes an unrecoverable rewrite. (Ranked high deliberately — see §14 for why this must NOT wait for Year 4.)
5. **Extract ONE window manager + one window object**; make built-in/IPC/Wayland thin adapters; delete duplicated chrome/`ssd_col_*`. Prerequisite for any bare-metal port — but must come *after* the test oracle (§7 below), never before.
6. **Extract a shared client SDK** (`fifi/apps/libfifi/`) owning protocol constants, handshake, event loop, frame submission; add a protocol-version + capability bitset to `IPC_APP_CONNECT`.
7. **Extract a shared config module** with atomic write (temp+fsync+rename) and one schema; implement the bare-metal `gui_settings_save/load` stubs over existing ext2.
8. **Stand up CI + `make test`** (headless QEMU boot of both branches with self-tests on, plus a QMP/screenshot oracle) and a shared-core diff-guard. **This is the gate that must exist before #5.**
9. **Add mandatory integrity verification** (pinned SHA256/signature) to every download path; redesign OS update as A/B with rollback.
10. **Fold per-app font loaders/draw primitives into a real UI toolkit** (`fifi/ui/`) with widgets, semantic color roles, and a DPI/scale factor.

## 11. Largest Architectural Risks (Ranked)

1. **Security model contradicts the project's own frozen charter** — root-everything + no verification + shipped backdoor makes every other boundary porous.
2. **Gaming-first is structurally unmet in the presentation path, and the proving ground cannot fix it — a category error, not a timeline risk.** The compositor is *designed* to avoid the GPU: it composites into a DRM dumb buffer (`drm.c:149` — literally "software-rendered pixel store", `CREATE_DUMB` at `:150`, mmap `:174`, presents via `DIRTYFB`/`SETCRTC`), and even GPU-accelerated clients are handled by importing their dmabufs as LINEAR-modifier only and CPU-mmapping them (`wayland.c:327` — "no GL import"). All real acceleration lives in client Mesa/i915 + the host Linux DRM driver; FiFi's compositor never touches the GPU. So the proving ground validates *CPU compositing into a linear framebuffer* — which is exactly what bare-metal already does via the Limine framebuffer and therefore never needed proving. The hard end-goal subsystem — a real GPU driver + accelerated compositor for "high-performance gaming" — gets **zero validation and zero transferable code by construction.** Combined with no page-flip/vsync and no pointer-lock (`zwp_pointer_constraints`/`relative_pointer` absent), FPS mouselook is impossible at the architecture level. **Gaming-first on bare-metal is unproven greenfield regardless of how mature Linux Desktop gets.**
3. **"Bare metal is the end goal" is currently regressing.** The branch is 191 commits behind, its GUI is un-split, its build-from-Linux is broken (`Makefile:70`), and the compositor/window/IPC/app framework it must eventually host requires new syscalls that don't exist (`syscall_numbers.h` tops at 41, verified).
4. **No test gate + freezing un-characterized behavior** — freezing any API today locks in unproven behavior with nothing to conform against.
5. **PID-1 compositor with no recovery** — a crash bricks the session and risks dirty-unmount data loss.
6. **Freezing prematurely** — the window structs, dmabuf pipeline, IPC privileged messages, catalog schema, and agent classifier all need redesign *before* any freeze.
7. **Multi-device-class vision has no code path and no recorded decision** — every x86/root/desktop assumption baked into Beta 1.0 increases future porting cost.

---

## 12. Features to Complete BEFORE FiFi Mobile

Ordered prerequisites (blocking → enabling). **Note the reorder from the earlier draft:** per-app isolation is promoted to a co-blocker with the OSK, because a touch device's *primary* mode is running untrusted third-party apps.

1. **On-screen keyboard** as a first-class compositor overlay (render in shared GUI, inject via the existing key path, auto-raise on text focus). Non-negotiable — without it a touch device is unusable.
2. **A non-root app model + per-app capability grants** (co-blocker with the OSK, NOT an item-8 nice-to-have). Everything runs as root, AppImages run as root with zero verification, and IPC has no per-app authorization — a touch OS running untrusted apps cannot ship without this.
3. **Real power management** — a suspend path (`/sys/power/state`, elogind or minimal idle manager), lid/power-button handling, low-battery critical action, **stop force-pinning `performance`**, and convert the render loop from 2–4 ms busy-poll to event-driven/vsync-blocked idle.
4. **`wl_touch` + a platform-neutral touch/gesture event model** in `kernel/include` (down/up/motion/frame using the MT slots already parsed), keeping pointer emulation as fallback.
5. **Coordinate-transform / rotation layer** in the compositor (software rotate + input remap), manual portrait toggle first, autorotate via IIO later; derive status-bar/taskbar reserves from orientation.
6. **A DPI/scale factor + `px()/dp()` helper** through every fixed constant, 44px minimum touch target; make apps use the TTF path instead of the 9px PSF bitmap font.
7. **Notification service** (promote the toast to history/actions/DND) and a **swipe-down quick-settings panel** (WiFi/BT/rfkill/brightness/rotation/gaming-mode) reusing the existing tray indicators and wpa_supplicant/BlueZ backends.
8. **Bluetooth pairing UI** and **PipeWire audio routing** (sink/source switch, auto-route to BT/headset).
9. **App lifecycle** (freeze/reclaim backgrounded apps) and a **responsive "mobile shell" mode** in shared `gui*.c` (single-fullscreen-app, bottom nav, gesture area).

Design all of these as **additive, platform-independent APIs in `kernel/include` + shared `gui*.c`** so bare-metal inherits them instead of forking (the mistake the GUI split already made). **But write these as design notes now and code them only after Desktop 1.0 ships** — see §14 for why a solo owner cannot code mobile and harden the desktop in the same window.

## 13. Features Before Bare-Metal Catches Up

1. **Fix the two ring3→ring0 security holes; add IST fault stacks.**
2. **Build hygiene (Year 1, cheap):** back-port the GUI 9-file split + heap.c/mouse.c fixes; fix the `Makefile` OBJS list; add a `make kernel` smoke build to CI. Decoupled from the expensive kernel rework below.
3. **SMP/APIC rework** (LAPIC+IOAPIC, per-CPU state, spinlocks around PMM/heap/scheduler) and the `syscall`/`sysret` MSR fast path — do this *before* freezing PMM/thread/syscall internals because it reshapes all three.
4. **Copy-on-write fork** (resolve write faults in the existing `#PF` path).
5. **PMM redesign** to track all usable regions with a real `pmm_free_pages`; growable heap with coalescing/reclamation; raise `THREAD_MAX`.
6. **A windowing/surface syscall family** (`SYS_WIN_CREATE`/`SYS_WIN_FRAME`/`SYS_WIN_POLL`) + a graphical IPC transport so the same `libfifi` compiles for bare-metal userspace — the prerequisite for a shared app framework.
7. **Config/log persistence** over the existing ext2 write path (implement the empty `gui_settings_save/load` stubs); a unified logging API wrapping `kprintf` with an in-memory ring buffer + panic dump.
8. **ACPI S5 poweroff** (write `SLP_TYPa|SLP_EN` — the FADT parse already exists) and ACPI reset instead of the 8042 path.
9. **ext2 double/triple-indirect blocks** (current ~265 KB/file cap blocks real apps) and **TLS/HTTPS** (blocks the browser milestone). **These, plus a GPU driver, receive ZERO Linux head start** — the kernel net stack is a stub on Linux (`platform/linux/platform.c:454` `net_send_eth` returns `false`; the real `net.c:62` implementation is never compiled into the compositor), and GPU/USB/NVMe are uncompiled — so they must be estimated as from-scratch bare-metal work with their own QEMU self-test harness.

---

## The Two-Track Reframe (the bottom line the code forces)

The philosophy is not one claim but two, and the code says one is true and one is a category error:

**(a) Shared userspace platform** (GUI/WM/IPC/config/app-framework/AI/settings/themes): Linux **is** a valid, working proving ground. 10 real kernel files compile and run as a Linux process and validate contracts that genuinely transfer. Enforce it with a CI **diff-guard on the shared core**, a **single WM/window object** (kill the `wayland.c` fork), and a **fixed kernel-Makefile OBJS** so both targets build from one corpus.

**(b) Bare-metal kernel / drivers / GPU:** Linux is **not** the proving ground and cannot be. 44 of 54 kernel files, the whole driver layer, and the GPU are never exercised on Linux. These need their **own bare-metal test harness** (headless QEMU boot + in-kernel self-tests + a QMP/screenshot oracle) in CI on the bare-metal branch.

Today neither track has any test gate (0 FiFi-authored asserts, no CI, no `make test`). So "proving ground → end goal" is currently a **one-way UX-only pipe**, not a full-platform strategy. The roadmap below is built to convert it into one.

---

## 14. Five-Year Roadmap

**Velocity assumption (stated explicitly):** ~1 developer, part-time (~15–20 hrs/week). There is exactly **one critical path** — there are no parallel tracks; every "parallel" item is serial and stacks. **"Year N" labels are PHASES, not calendar years**; a solo owner planning against literal years will miss by 200%+. Where calendar guidance is given it is a solo estimate.

- **Phase 1 — Harden & unify (Linux). Split into a strict gated order:**
  - **1a — Release blockers + safety net (weeks, do-nothing-else-until-done):** remove the SSH key; non-root user + privilege-drop; content sandboxes on; PID-1 respawn supervisor + `panic=` on cmdlines. Then **CI + a QMP/screenshot oracle**. Then the ~1-day **bare-metal build-hygiene fix** (Makefile OBJS + GUI-split back-port + `make kernel` smoke build) to *stop the 191-commit rot from compounding* — this is decoupled from SMP/COW, which stay in Phase 4.
  - **1b — Refactors (only after the oracle exists):** extract `libfifi` SDK; unify the 3-way WM behind one window object; shared atomic-config module; signed downloads + A/B OS update with rollback; LUKS in the installer. Freeze the genuinely-stable seams. **The 3-way WM merge before the oracle exists is the single most dangerous ordering in the whole plan — do not do it.**
  - *Exit: FiFi Desktop 1.0 a stranger can safely install. Realistic solo timing: **18–30 months**, not 12–18.*
- **Phase 2 — Mobile foundations (Linux/ARM64).** Design notes for the touch/power/notification/isolation APIs are written during Phase 1; **mobile CODE begins only here, after Desktop 1.0 ships.** OSK + per-app isolation (co-blockers) → real power management → `wl_touch` → rotation → DPI scaling → notification center → quick settings. Parameterize `ARCH` across all build scripts; split the kernel config into `fifi-common`/`fifi-x86`/`fifi-arm64`; convert the initramfs from host-capture to a cross/sysroot build; add Panthor/Panfrost. *Exit: FiFi runs on an RK3588/CM5-class ARM64 board with accelerated GL.*
- **Phase 3 — FiFi Tablet / handheld (existing ARM64 HW, NOT custom silicon).** Mobile shell mode, gesture navigation, split-view, PipeWire routing, BT pairing UI, a **native-ARM catalog**. **box64/FEX + Proton AAA is CUT** — x86→ARM64 dynamic recompilation of Proton is Valve/Asahi-scale with unsolved latency, is a dependency of nothing, and is fantasy for a solo dev. ARM64 = light/native gaming, stated honestly. *Exit: shippable FiFi Tablet on existing ARM64 hardware (PinePhone Pro / mainline Fairphone-class).*
- **Phase 4 — Bare-metal parity groundwork.** The *expensive* kernel rework: SMP/APIC/COW/fast-syscall; GPU/accel HAL designed and stubbed; windowing syscalls + graphical IPC transport; back-port the full toolkit. Bare-metal becomes buildable and testable in CI on both branches from one corpus. (Build hygiene already landed in Phase 1a; only the reshaping work is here.)
- **Phase 5 — First bare-metal app framework.** `libfifi` compiles for bare-metal userspace; a handful of native IPC apps run on the bare kernel using the shared WM. First convergence of the two app models.

## 15. Ten-Year Roadmap

- **Phase 6–7 — Bare-metal desktop viability.** A real GPU driver for one target GPU — **its own long-pole phase with an independent from-scratch estimate and ZERO Linux head start** (the proving ground is designed to avoid the GPU, §11 risk #2). Plus TLS stack, larger/native FS, real ACPI power management (P-states/C-states via APIC + tickless timer), battery/thermal. Bare-metal boots to the shared desktop on one blessed x86 machine.
- **Phase 7–8 — FiFi Phone HW.** Only after modem/cellular (ModemManager/oFono), GPS, NFC, fingerprint, camera, and telephony/SMS exist as platform-neutral APIs on the Linux side. Existing ARM64 phone HW first; custom hardware deferred. Bare-metal remains Linux-hosted for radios.
- **Phase 8–9 — Server & embedded profiles.** Headless build profile, service management, remote admin, minimal embedded footprint. **Gated on the Phase-1 security blockers** — a network-exposed root box with a shipped key and open port 22 is *disqualifying*, worse than a single-user desktop, so these cannot precede hardening. They are cheap only on the UX axis and additionally need service management + multi-user + remote admin. With those, the server profile (built on the already-strong bare-metal networking stack) is realistically the earliest non-desktop shippable class.
- **Phase 9–10 — Gradual bare-metal substitution.** Replace Linux subsystems one HAL at a time behind frozen contracts, starting with the pure/validated ones (display/timer/RTC/input event/IPC wire), keeping Linux for GPU/radio until bare-metal drivers exist. Automotive stays research-only ("not-in-plan-horizon").

## 16. Milestone Plan From Today to Each Target

*(Headlines; each assumes the release-blocker fixes in §9 land first as a hard gate. Solo estimates. "Parallel" nowhere — single critical path.)*

**→ FiFi Desktop (18–30 mo solo):** §1a release blockers + PID-1 supervisor (weeks) → CI + screenshot oracle (weeks) → bare-metal build-hygiene fix (~1 day) → THEN §1b `libfifi` SDK + unified WM + shared config + signed A/B updates + LUKS → **Desktop 1.0**. The oracle strictly precedes the WM merge.

**→ FiFi Tablet / handheld (Desktop + ~2–3 yr):** design notes during Phase 1 → OSK + per-app isolation (co-blockers) → power management → `wl_touch` + gestures → rotation + DPI → notifications + quick settings → mobile shell → ARM64 build pipeline + Panthor → **Tablet 1.0 on existing ARM64 HW** (native-ARM catalog; no Proton AAA).

**→ FiFi Server / Embedded (post-hardening):** minimal headless profile off the shared core (no touch/GPU/mobile stack) + service management + remote admin, built on the strong bare-metal networking stack. Cheapest on the UX axis, but **gated on the §9 security blockers** — cannot ship before them.

**→ FiFi Phone (7–8 yr):** Tablet base + cellular/GPS/NFC/fingerprint/camera/telephony platform APIs → power/thermal for phone SoCs → existing ARM64 phone HW (custom silicon deferred to a later phase) → **Phone 1.0**.

**→ Bare-Metal (5–10 yr):** security holes + IST stacks → SMP/APIC/COW/fast-syscall → toolkit back-port + windowing syscalls → **GPU driver as its own long-pole phase (from-scratch, no Linux head start)** + TLS + FS → bare-metal desktop on one machine → gradual HAL substitution.

---

## What Stays Linux-Only

Wayland server (`wayland.c`), XWayland WM (`xwm.c`), AppImage/squashfs/`unshare`/glibc-dynamic-linker mechanics, `switch_root`/initramfs/PID1-exec mechanics, the llama.cpp engine (needs libc/threads/AVX2/mmap/multi-GB files), GRUB/Secure-Boot recovery, and box64/FEX gaming emulation (now cut from the plan entirely for ARM64). These are POSIX/Linux protocols or host-kernel facilities and are correctly Linux-only for third-party apps.

## What Becomes Platform-Independent

The window/WM behavior model, the FiFi IPC message model (behind a transport abstraction), the config file format + persistence API, the theme model + UI toolkit, the `/fifi-data` layout + refresh policy, the boot-mode flag semantics, the battery/power-profile/display-power/suspend contracts, the touch/orientation/notification event models, the model.conf/catalog schema + agent RUN/DONE protocol + llama-server HTTP contract, the package lifecycle (resolve→verify→stage→activate→rollback), and the `gui_spawn_app` app-launch API.

## What Moves Into Bare-Metal

The compositor-as-session-init role, a native graphical IPC transport + windowing syscalls, the config/log persistence bodies (empty stubs today, ext2 already exists), the battery API implementation (via ACPI `_BIF`/`_BST`), ACPI S5/reset, and eventually a native package backend and GPU HAL (greenfield — no Linux head start).

## APIs to Finalize First (with a pinning test before each freeze)

1. **Syscall ABI** (`syscall_numbers.h`, 42 numbers + `fifi_stat`/`fifi_utsname` layouts) — after a conformance ELF.
2. **Additive shared kernel headers** (`console.h`, `gui.h`, `keyboard.h`, `mouse.h`, `vfs.h`) — **caveat: these 5 (the hot ones) already differ across branches**; still ABI-compatible today (small, mostly `#ifdef __linux__`) but drifting with no diff-guard. Freeze existing decls, add a CI diff-guard, gate future changes to additive-only. Do not treat header-sharing as already solved.
3. **The display contract** (`limine_framebuffer` + `console_fb_*` + `drm_open/flush/blank/close`) and the **draw-vs-present seam** (`console_*` primitives).
4. **The IPC 8-byte framing + core window/input opcode taxonomy** — after extracting to one shared header and adding a version field + conformance test.
5. **The `fifi-settings.conf` key schema** and the **`gui_settings_*`/`gui_desktop_*`/`gui_fav_*` signatures** — after unifying `gui_theme_t` to a superset on both branches.
6. **The boot-mode contract** (`fifi_live`/`fifi_noswitch`/`fifi_data_uuid`), the **`/fifi-data` refresh policy**, and the **`fifi-secctl` toggle surface**.
7. **The battery quartet** (`battery_present/percent/charging/minutes`) and the **weak-symbol optional-capability pattern**.
8. **The privacy/no-telemetry charter** (already the effective frozen contract).

## Stable Decisions

x86_64 first target; UEFI/OVMF/Limine boot; the CORE_REQUIREMENTS charter; offline-first/loopback-only/no-telemetry AI invariants; the compositor-compiles-kernel-GUI sharing mechanism (valid for the userspace half only); the `/fifi-data`-persistent / tmpfs-ephemeral split; Linux-as-proving-ground-for-userspace / bare-metal-as-end-goal (EVOLVE, do not rewrite in Rust). **These should be written into `decisions.md`, which currently records none of them.**

---

## Is Linux Desktop Mature Enough to BEGIN FiFi Mobile?

**Design now; code after Desktop 1.0.** The input/GUI/IPC core is solid enough to build on, and the anti-re-fork argument is real — but for a solo owner, "begin mobile code now" directly competes with "harden the desktop now" (Phase 1) for the same hands. The anti-re-fork goal is satisfied by a **written event-model/API design note**, not shipping code. Putting mobile code in the hardening phase is exactly the breadth-over-hardening dilution this assessment criticizes elsewhere.

Three hard blockers must clear before any mobile alpha:
1. **On-screen keyboard** (device unusable without it).
2. **Per-app isolation / capability model** (co-blocker, not item-8 — touch devices run untrusted apps as their primary mode).
3. **Real power management** (forced-`performance` + busy-poll would drain a battery in ~1–2 h).

Then: 4. `wl_touch` + platform-neutral touch/gesture model; 5. rotation + DPI scaling; 6. notification/quick-settings; 7. suspend/session recovery (which also fixes the PID-1 desktop risk).

Pick the **first form factor = docked/handheld-gaming or TABLET on existing ARM64 hardware**, not a phone, not custom silicon. Cellular/GPS/NFC/telephony are years out.

---

## FiFi Mobile Roadmap (subsystem-by-subsystem)

- **Touch:** advertise `WL_SEAT_CAP_TOUCH`, implement `WL_SEAT_GET_TOUCH` + down/up/motion/frame from the already-parsed MT slots; keep pointer emulation fallback. Define the event struct in `kernel/include`.
- **Isolation (co-blocker):** non-root app model + per-app capability grants + IPC authorization before any untrusted-app store on touch.
- **Gestures:** tap/long-press/drag/momentum-scroll/swipe/pinch as a shared recognizer above the touch events.
- **OSK:** compositor overlay, auto-raise on text focus, injects via the key path.
- **Rotation:** compositor coordinate-transform + input remap; manual toggle → IIO autorotate.
- **Notifications:** promote the toast primitive to a service with history/actions/DND + a freedesktop bridge.
- **Status bar / quick settings:** reuse tray indicators; swipe-down panel toggling WiFi/BT(rfkill)/brightness/rotation/gaming-mode.
- **Launcher:** evolve the Super-tap app menu into a touch app-grid.
- **Docked / split / scaling:** mobile-shell single-fullscreen-app mode, split-view via the existing half-snap, global scale factor.
- **Accessibility:** screen reader, magnifier, high-contrast, large-text (only a magnifier glyph exists today).
- **Suspend:** `/sys/power/state` + lid/power-button; **stop forcing `performance`**.
- **BT/WiFi:** pairing UI over the bundled BlueZ; WiFi uses the fixed wpa_supplicant broker path.
- **Audio:** PipeWire sink/source routing behind a neutral API.
- **box64/FEX + Proton:** **CUT.** Ship native-ARM catalog only; ARM64 = light/native, not Proton AAA.
- **Camera/GPS/modem/NFC/fingerprint/phone+SMS:** absent in code *and* kernel config — **out of near-term scope**; add kernel classes (WWAN/NFC/IIO/GNSS/V4L2) + ModemManager/oFono only at the Phone stage.

## FiFi Phone Roadmap

Phone is a *superset of Tablet plus radios/sensors* and should not start until Tablet ships. Sequence: (1) Tablet base stable; (2) add kernel `CONFIG_WWAN/NFC/IIO/GNSS/V4L2` + ModemManager/oFono, GPS, NFC, fingerprint, camera (V4L2/libcamera) as platform-neutral APIs; (3) telephony/SMS stack + dialer/messages apps; (4) phone-SoC power/thermal profiles + real DPMS/backlight; (5) portrait-first mobile shell + always-on OSK; (6) **existing well-supported ARM64 phone HW** (PinePhone Pro / mainline Fairphone — u-boot/ABL boot glue; GRUB-EFI does not generalize); custom silicon deferred to a much later phase; (7) security: per-app sandbox + LUKS + verified boot are *mandatory* on a phone, not optional. Realistically 7–8 years given the GPU/modem/telephony greenfield.

---

## Critique of the 4-Phase Hardware Strategy

*(Linux-on-ARM64 dev HW → FiFi Mobile on Linux → custom FiFi Phone HW → gradually replace Linux subsystems with bare-metal.)*

**Sound in its bones.** The core instinct — validate the userspace platform on Linux, delegate drivers to Linux, converge to bare-metal last — is right and matches how the desktop was built. Phasing hardware before bare-metal is correct: bare-metal has no GPU/radio/modem story and won't for years.

**Flaws:**
1. **No velocity model / fictional parallelism.** This is a one-developer platform, so there is one critical path; the strategy's "parallel tracks" are all serial. As originally written it is unschedulable. Fixed above by a single linear queue + explicit velocity assumption + "phases, not years."
2. **Phase ordering skips the cheapest wins BUT they are not free.** Server/embedded need none of the mobile/GPU/touch stack and could ship earlier off the shared core — *except* they are network-exposed and thus **gated on the Phase-1 security blockers** (a root box with a shipped key + open port 22 is disqualifying) and additionally need service management + multi-user + remote admin. Cheap on the UX axis only.
3. **"Custom FiFi Phone HW" is the riskiest, most capital-intensive step and is placed too early.** Building custom hardware while the OS runs on Linux and bare-metal is years behind is a resource trap. Target an existing well-supported ARM64 phone (PinePhone Pro / mainline Fairphone); defer custom HW to a much later phase.
4. **GPU is a category error, not a timeline risk (strongest correction).** The proving ground is *designed* to avoid the GPU (dumb-buffer software compositing, LINEAR-only dmabuf CPU-mmap — §11 risk #2), so it cannot validate the one subsystem the gaming-first end goal most needs, even in principle. GPU gets its own from-scratch long-pole phase with zero Linux head start.
5. **The strategy assumes the proving ground validates what matters — it doesn't for the hardest drivers.** Only 10 of 54 kernel files are exercised on Linux. GPU/block/NVMe/USB and the kernel network stack are stubbed or uncompiled (`platform/linux/platform.c:454` `net_send_eth` returns `false`; the real `net.c:62` is never built into the compositor), so bare-metal bugs there surface with **no** Linux head start. False confidence.
6. **No decision record.** None of this multi-device-class strategy exists in `decisions.md`, so every current Beta-1.0 choice bakes in x86/root/desktop assumptions that raise the porting cost of all four phases.

**Better alternative:** insert an explicit **Phase 0 (harden + parameterize + record)** now — fix the security blockers, add a test gate (both a Linux QMP/screenshot oracle *and* a bare-metal QEMU self-test harness, since Linux cannot prove the kernel), fix the bare-metal build (Makefile OBJS), parameterize `ARCH`, freeze the arch-crossing seams, and write the strategy into `decisions.md` while those edits are one-liners. Then target **existing ARM64 phones** (not custom HW), split **GPU driver** into its own long-pole phase, gate **Server/Embedded** on the security work, and keep **custom hardware last**. This preserves the sound "Linux-first, bare-metal-last" spine while removing the capital risk and the false-confidence gaps.

---

### Bottom Line

FiFi is a remarkable two-front achievement — a working from-scratch kernel *and* a working Linux-based desktop sharing a real toolkit — held back by a consistent pattern: **breadth prioritized over hardening.** But be precise about what the proving ground actually proves: it validates the **shared userspace platform** (10 of 54 kernel files — GUI/WM/IPC/config/app framework), and those contracts genuinely transfer. It does **not** and **cannot** validate the bare-metal kernel, drivers, or GPU (the other 44 files, all stubbed or uncompiled on Linux) — so "bare metal is the end goal" is today a one-way, UX-only pipe, and it is currently *regressing* (191 commits behind, GUI un-split, native build broken).

The single most valuable thing the project can do before adding any new device class is, in strict order: (1) fix the security blockers (shipped SSH key, root-everywhere, disabled sandboxes) and the PID-1 recovery gap; (2) stand up BOTH test gates (Linux screenshot oracle + a bare-metal QEMU self-test harness); (3) fix the bare-metal build and stop the fork rot while it is still a one-day fix; (4) unify the forked GUI/WM/IPC/config/SDK behind frozen contracts — *after* the oracle exists, never before; and (5) write the decisions down. Do that, and both "begin FiFi Mobile" and "bare metal is the end goal" become tractable. Skip it, and each new target multiplies the un-tested, un-shared, root-everything debt already present.
