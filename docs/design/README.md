# Handoff: FiFi OS Desktop Shell

## Overview
Hi-fi design for the FiFi OS desktop shell — a dark, glassy, next-generation Linux desktop (Wayland). It replaces the Windows-style taskbar/start-menu paradigm with three shell surfaces: a floating **horizon bar** (top), a centered **glass dock** with an AI "Spark" orb, and an ambient **Spark card** on the desktop. Window management is hybrid: floating windows plus linked tile pairs with a draggable seam.

## About the Design Files
The files in this bundle are **design references created in HTML** — they show intended look and behavior; they are NOT production code to copy directly. The task is to **recreate this design in the target environment** (e.g. a Wayland compositor shell: QML/Kirigami, GTK/gjs, Rust + Slint/iced, or web-tech shell like Quickshell/AGS) using its established patterns. If no shell framework is chosen yet, pick what fits the OS stack and implement the design there.

`FiFi Desktop.dc.html` is the design source. It is a templated HTML file (all styling is inline `style="…"` attributes — read those for exact values). `{{ name }}` holes are filled at runtime by the small logic class at the bottom of the file; `support.js` is only the preview runtime and is irrelevant to implementation. Open the .dc.html next to support.js in a browser to view it rendered at 1920×1080.

## Fidelity
**High-fidelity.** Colors, typography, spacing, radii, and shadows are final. Recreate pixel-perfectly, substituting the OS's real icon set where the mock uses minimal CSS glyphs.

## Design Tokens

Colors
- Background base: `#06080d`
- Wallpaper "aurora": layered radial gradients over `linear-gradient(180deg,#070a10,#05070c)`:
  - `radial-gradient(1200px 720px at 72% -8%, oklch(0.55 0.11 210 / 0.30), transparent 62%)`
  - `radial-gradient(1000px 700px at 10% 112%, oklch(0.45 0.13 290 / 0.28), transparent 62%)`
  - `radial-gradient(760px 520px at 42% 46%, oklch(0.35 0.06 230 / 0.22), transparent 65%)`
  - plus film grain (SVG fractalNoise, opacity 0.05, blend overlay) and vignette `radial-gradient(1600px 1000px at 50% 42%, transparent 55%, rgba(2,3,6,0.55))`
- **Accent (user-selected): `#FF9A6B` (coral)** — exposed as `--ac`; every accent use is `var(--ac)`, so accent is a single theme token. Alternate palette: `#5BD9E3` cyan, `#8F7BFF` violet, `#5CE6A9` mint.
- Secondary accent (fixed): `#8F7BFF` violet — used in gradients paired with `--ac`, terminal `::` glyphs, "design" tag dot
- Warm/close dot: `oklch(0.72 0.13 35)`
- Text primary: `#e9edf3`; secondary `rgba(233,237,243,0.6)`; tertiary/dim `rgba(233,237,243,0.35–0.45)`
- Accent tints: `color-mix(in oklab, var(--ac) N%, transparent)` at 10–60%

Glass surfaces (all panels/windows)
- Bar/dock/card: `rgba(12,16,23,0.55–0.6)`; windows: `rgba(13,17,24,0.72–0.78)`; terminal darker: `rgba(9,12,17,0.82)`
- `backdrop-filter: blur(28–30px) saturate(1.4–1.5)`
- Border: `1px solid rgba(255,255,255,0.08–0.10)`
- Hairlines/dividers: `rgba(255,255,255,0.05–0.10)`

Typography
- Display/brand/clock: **Space Grotesk** 600–700
- UI body: **Manrope** 400–700 (11–14.5px in shell chrome)
- Mono (terminal, metadata, eyebrows): **JetBrains Mono** 400–600; eyebrow style = 9.5–10px, `letter-spacing: 0.14em`, uppercase, dim

Radii: windows 16px; bar 14px; dock 22px; dock icons 14–16px; cards/chips 12px; pills 99px
Shadows: windows `0 32px 90px -20px rgba(0,0,0,0.65)`; floating window `0 40px 110px -18px rgba(0,0,0,0.75)`; bar `0 12px 40px -12px rgba(0,0,0,0.5)`; dock `0 20px 60px -12px rgba(0,0,0,0.6)`; accent glow `0 0 20–36px color-mix(accent 30–50%)`
Spacing rhythm: 16px screen margins; 10px tile gap; panel padding 14–18px

## Screens / Views

### Desktop (single view, 1920×1080 reference)

**1. Horizon bar** — floating, `top/left/right: 16px`, height 44px, radius 14
- Left: FiFi logo = 16px 4-point star (`clip-path: polygon(50% 0%, 61% 39%, 100% 50%, 61% 61%, 50% 100%, 39% 61%, 0% 50%, 39% 39%)`) in accent with accent drop-shadow glow; "FiFi" Space Grotesk 700/14px; 1×18px divider
- **Intent workspaces** (the core paradigm — intents, not app tabs): pills "Build" (active), "Research", "Play", plus a dashed-border "+" circle (26px). Pill: padding 5px 13px, radius 99px, 12.5px/600, 5px dot. Active: accent bg, near-black text `#0a0d12`. Inactive: `rgba(255,255,255,0.05)` bg, border `0.07`, text 62% white. Click switches intent.
- Right cluster: wifi = 3 ascending bars (3px wide, heights 5/8/12); battery = 22×11 outline + 70% fill + nub + "82%" mono 12px; notifications pill = accent dot (glow) + count "2"; date 12.5px dim; clock Space Grotesk 600/14.5px (live)

**2. Tiled pair** (hybrid tiling) — terminal at x96 y132, 784×648; files at x890 y132, 530×648; 10px gap; **seam handle** centered on the gap (8×56px pill, radius 4, `rgba(255,255,255,0.14)`, cursor col-resize, hover → 50% accent)

**Window chrome** (all windows): 38px header — left app glyph + title 12.5px/600 at 75% white; centered drag grip 26×3px pill `rgba(255,255,255,0.12)`; right three 7px dots (two at `rgba(233,237,243,0.22)` = minimize/maximize, one warm `oklch(0.72 0.13 35)` = close). Header bg `rgba(255,255,255,0.03)`, bottom hairline. No square Windows-style buttons.

**3. Terminal window** ("term — ~/projects/fifi-shell") — JetBrains Mono 13px, line-height 1.75, padding 20px 24px
- Fetch block: 38px accent star + rows (label column 76px dim / value light): `ray@aurora` (user accent, host violet), os FiFi OS 0.4 (aurora), kernel 6.15.3-fifi, shell fish 4.0, wm fifi-shell · wayland, uptime 3h 12m, memory 6.2/32 GiB
- `❯ paru -Syu` session: `::` in violet, output at 55% white, progress bar of `█` glyphs (filled = accent, rest 15% white) at 82%
- Prompt `❯` accent + block cursor 8×15px at 80% white

**4. Files window** ("files")
- Toolbar 46px: back/forward ‹ › (forward disabled at 20%); breadcrumb mono 12px `home / ray / projects` (current segment bold white); search pill with mono accent `/` + "search"
- Sidebar 148px: eyebrow "PLACES"; rows (28px, 12.5px, 5px dot glyph): Home, **Projects (active: accent 13% tint bg + accent dot + white 600)**, Downloads, Music, Trash; "TAGS": shell (accent dot), design (violet dot)
- Grid 3 columns, 10px gap: folder tiles (radius 12, centered folder shape 36×26 + tab, name 12px/600, mono count). Default: `rgba(255,255,255,0.035)` bg. **Selected (fifi-shell, compositor): accent 10% bg + accent 45% border + accent-tinted folder**
- "RECENT" list: gestures.spec.md 14:02, panel.blend 11:47, spark-prompts.toml yesterday (doc glyph 12×15 with darker top edge, mono time right)
- Status bar 34px mono 10.5px: "6 folders · 2 selected" (count in accent) / "512 GB · 84 free"

**5. Spark ambient card** (AI woven into the shell) — x1556 y132, w324, radius 18, padding 18
- Header: 14px gradient star (accent→violet), "Spark" Space Grotesk 600/13.5px, right eyebrow "AMBIENT"
- Focus row: 52px progress ring (conic-gradient accent 252deg on `rgba(255,255,255,0.09)` track, ring mask at 57–58%, center mono "2:14"), "Focus · Build" 14px/700, "since 12:18 · notifications muted" 11.5px dim
- Divider; eyebrow "SPARK SUGGESTS"; two suggestion chips (radius 12, `rgba(255,255,255,0.04)` bg): title 12.5px/600 + reason 11px dim + accent → arrow. Copy: "Resume PR #142 · gestures / one failing test left yesterday", "Standup in 25 min / wraps your Build session"
- Footer: kbd pills `super` `space` (mono 10px, 3px 7px, radius 6, border `0.14`) + "ask anything"

**6. Floating music window** ("waves") — x1180 y660, w410, overlaps files window (z above tiles), heavier shadow
- 132px cover-art placeholder (45° stripe pattern, mono label "cover art") — replace with real album art
- "NOW PLAYING" eyebrow in accent; "Midnight Drive" Space Grotesk 600/17px; "Solar Fields · Movements" dim
- Progress 4px track at 44% + 10px white thumb; mono times 2:41 / 6:05
- Controls: prev/next = paired 9px triangles at 65% opacity; play/pause = 38px accent circle with two dark bars + accent glow

**7. Dock** — bottom-center 18px, glass pill radius 22, padding 9, gap 7
- Icons 48px (radius 14), minimal glyphs: terminal `❯_`, files folder, editor `{ }`, browser circle-globe, music equalizer bars, settings 3 slider rows
- Center **Spark orb** 54px (radius 16): accent→violet 30% gradient tint, accent border 40%, white star 22px, accent glow — flanked by 1×30px dividers
- Running indicator: 4px accent dot under terminal, files, music
- Hover: `translateY(-3px)` + `rgba(255,255,255,0.08)` bg (orb also scales 1.04, glow widens)

## Interactions & Behavior
- Intent pills: click switches active intent (workspace). All transitions ~150ms ease.
- Seam handle: drag to resize the tile pair; hover tints accent.
- Dock: hover lift (150ms); Spark orb opens command palette (not designed yet).
- `Super+Space`: summon Spark palette.
- Window dots: hover should reveal function (min/max/close); close is the warm dot.
- Clock updates every 30s. Focus ring reflects elapsed session (252° ≈ 70%).
- Suggestion chips + notification pill + search: hover bg lightens to `rgba(255,255,255,0.07–0.1)`.

## State Management
- `activeIntent: 'Build' | 'Research' | 'Play'` — switching swaps the window set (not shown)
- `now: Date` (clock, 30s tick)
- Theme: `accent` (user default `#FF9A6B`), `wallpaper: 'aurora' | 'nebula' | 'void'`, `showNowCard: boolean` — the shell is deeply themeable; accent is one token
- Per-window: tiled-vs-floating, tile-group membership, selection state (files), playback state (music)
- Spark: focus session (start time, elapsed, mute state), suggestions[] (title, reason, action)

## Assets
- No bitmap assets. Logo/Spark = 4-point-star clip-path; all icons are minimal CSS glyphs — substitute the OS icon set.
- Fonts from Google Fonts: Space Grotesk (500–700), Manrope (400–700), JetBrains Mono (400–600).
- Cover art is a striped placeholder — feed real album art.
- Wallpaper is pure CSS gradients (see tokens) — reproducible as a shader or pre-rendered image.

## Files
- `index.html` — **the design as working code**: standalone plain HTML/CSS/JS, opens in any browser, pixel-identical to the screenshots. Copy it verbatim as the starting point; don't rebuild from scratch.
- `PROMPT.md` — **paste this into Claude Code**: tells it to copy index.html, then documents every value (exact geometry, colors, copy, states) for making edits
- `screenshots/desktop-full.png` — ground-truth render (3840×2160, coral accent); `01`–`06` crops are close-ups of bar, terminal, files, Spark card, music, dock
- `FiFi Desktop.dc.html` — the design source (inline styles = exact spec; logic class at bottom holds intent-pill styles, wallpaper variants, clock)
- `support.js` — preview runtime only; open the two together in a browser to view. Not part of the implementation.
