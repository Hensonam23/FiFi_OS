# FiFi OS → Desktop Environment: Vision & Roadmap

**Decided 2026-07-01 with Aaron. Direction: EVOLVE FiFi OS — do NOT rewrite in Rust/smithay.**
This file is the north star for the FiFi desktop. Follow it across sessions.

## Why evolve, not rewrite
Aaron's goal is a KDE-Plasma-like DE but with customization as a *unified, first-class*
system (not bolted-on config-file/GUI splits). FiFi OS **already satisfies the two hardest
pillars**: it is a real from-scratch **Wayland compositor** (C, `fifi/platform/linux/wayland.c`)
and it is **fast/lightweight, not Electron**. A Rust/smithay rewrite would discard a working,
booting OS (compositor + GUI + taskbar + settings + apps + browser). The vision's
*differentiators* are built ON TOP of FiFi incrementally.

## Vision goals
1. **Unified config** — every visual/behavioral aspect (panel, decorations, layout,
   keybindings, animations, widgets) configurable through ONE consistent, **hot-reloadable**
   structured config. The Settings GUI edits the SAME underlying file (no GUI-vs-file split).
2. **Plugin/widget architecture** — sandboxed, hot-loadable (no rebuild/restart).
3. **Fast/lightweight** — C core, not Electron. *(Already true.)*
4. **Real Wayland compositor**, not X11. *(Already true.)*

## Phased roadmap (sign-off per phase before building)
- **Phase 0 — Solidify the shell (IN PROGRESS)** — make it daily-usable.
  - DONE: crash-free multi-window browser; scroll-wheel forwarding; right-click routed to
    browser (not desktop menu); window move/resize/maximize/fullscreen; hover-tooltips
    suppressed; settings persistence (`/fifi-data/fifi-settings.conf`); screenshot-on-demand
    (`kill -USR1 <compositor pid>` → `/fifi-data/screenshots/shotNNN.ppm`); crash backtrace
    handler.
  - REMAINING: CSD **shadow** (black ring around browser — hard, see below); **z-order /
    click-to-raise** for Wayland toplevels (browser always draws over FiFi windows);
    **minimize → taskbar** (needs a taskbar entry for Wayland windows); **sandbox infobar**
    (browser-side "security sandbox is disabled").
- **Phase 1 — Unified config** — one hot-reloadable file (`/fifi-data/fifi.conf`) the
  compositor reads on boot + re-reads on change; Settings GUI writes the same file.
  (Started: `gui_settings_save/load` in `kernel/src/gui.c` persist the theme — generalize
  this into the unified config. This is the vision's #1 requirement.)
- **Phase 2 — Window-manager maturity** — proper stacking, tiling, window rules.
- **Phase 3 — Panel/widget system** driven by config.
- **Phase 4 — Theming engine** — all colors/decorations/animations from config.
- **Phase 5 — Plugin sandboxing.**

## Hardest/riskiest — design around these early
Hot-reload without crashing the compositor; widget sandboxing; theming-engine flexibility.

## Current hard problem: CSD shadow (Phase 0)
LibreWolf draws a transparent CSD shadow margin (~23–30px) around its window. FiFi's
dirty-region desktop doesn't repaint the wallpaper under the window footprint, so the shadow
band composites as pure black `(0,0,0)` on all four sides. Tried: opaque-only skip,
alpha-blend, and per-frame `full_redraw()` under the window — band persists.
Candidate real fixes, in order of preference (avoid fragile per-app env hacks):
1. **xdg-decoration protocol + request server-side decorations** so Firefox stops drawing
   CSD (no shadow in the buffer at all). Most correct; needs testing of Firefox SSD behavior.
2. Properly region-composite the wallpaper under the window each frame (currently the
   `full_redraw()` approach isn't reaching the band — verify it's actually running/covering).

## Deploy / debug workflow (box specifics)
- Box IP **10.0.0.187** (it moves — rescan `nmap -p22,2323 10.0.0.0/24`). SSH `root@10.0.0.187`.
- The box is minimal: **no grep/head/tail/rsync/scp/node**. Transfer with
  `cat | ssh 'cat > dest'`; process/inspect locally.
- Build (ABSOLUTE path — cwd resets to /home/aaron after any `cd`):
  `make -C /home/aaron/src/linux-desktop/fifi/compositor -s`
- Deploy: pipe binary to `/fifi-data/fifi-compositor-desktop-v7`, then
  `echo s > /proc/sysrq-trigger` (**sync — required or the write is lost**),
  `echo 3 > /proc/sys/vm/drop_caches`, then `echo b > /proc/sysrq-trigger` (reboot).
  Boot profile installs v7 → `/bin/fifi-compositor.fixed`; a supervisor reverts to stock if
  it dies within 8s ("cannot open display" cascade = our compositor crashed).
- Screenshot: `kill -USR1 <fifi-compositor.fixed pid>`; pull the PPM and `pnmtopng` to view.
- Crash backtrace: built with `-g`; SIGABRT/SIGSEGV handler prints addresses →
  `addr2line -e build-linux/fifi-compositor <addr>`.

## Commit policy
Aaron has NOT authorized commit or push. There is a large body of working, uncommitted work
on branch `linux-desktop`. **Do not push to GitHub. Local commit only on Aaron's explicit OK.**
