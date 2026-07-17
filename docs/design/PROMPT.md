# PROMPT — paste this into Claude Code

---

The **FiFi OS desktop shell** design already exists as working code: `index.html` in this folder is the exact design — a standalone plain HTML/CSS/vanilla JS file, no build step. **Do not rewrite it from scratch. Copy `index.html` as-is to the target location, open it in a browser, and confirm it matches `screenshots/desktop-full.png`. Then make only the changes I ask for.** Every visual decision in it is intentional; when you edit, preserve all values you weren't asked to change.

The spec below documents every value in `index.html` — use it to understand the file and to keep any edits on-system. Target is a fixed 1920×1080 canvas. Screenshots in `screenshots/` are ground truth — `desktop-full.png` is the whole design; the numbered crops are close-ups. Never improvise, "improve", or substitute your own aesthetic.

## 1. Global setup

- Google Fonts: `Space Grotesk` (500,600,700), `Manrope` (400,500,600,700), `JetBrains Mono` (400,500,600).
- `body { margin:0; background:#06080d; }`
- Root: `<div id="desktop">` — `width:1920px; height:1080px; position:relative; overflow:hidden; background:#06080d; font-family:'Manrope',sans-serif; color:#e9edf3;`
- Define the accent as a CSS variable on the root: `--ac: #FF9A6B` (coral — this exact value). Every accent usage below is `var(--ac)`. Secondary accent is fixed violet `#8F7BFF` (do NOT make it a variable of --ac).
- Accent tints use `color-mix(in oklab, var(--ac) N%, transparent)`.
- All positioned elements below use absolute positioning inside `#desktop` with the exact px coordinates given.

Text color system: primary `#e9edf3`; 75% `rgba(233,237,243,0.75)`; 60% `rgba(233,237,243,0.6)`; dim 35–45% `rgba(233,237,243,0.35)`–`rgba(233,237,243,0.45)`.

"Eyebrow" style (used for small section labels): JetBrains Mono, 9.5px, `letter-spacing:0.14em`, uppercase, color `rgba(233,237,243,0.35)`.

## 2. Wallpaper (4 stacked full-bleed layers, in order)

1. Gradient layer:
```
background:
  radial-gradient(1200px 720px at 72% -8%, oklch(0.55 0.11 210 / 0.30), transparent 62%),
  radial-gradient(1000px 700px at 10% 112%, oklch(0.45 0.13 290 / 0.28), transparent 62%),
  radial-gradient(760px 520px at 42% 46%, oklch(0.35 0.06 230 / 0.22), transparent 65%),
  linear-gradient(180deg,#070a10,#05070c);
```
2. Film grain: inline SVG `feTurbulence type="fractalNoise" baseFrequency="0.9" numOctaves="2"` tiled 160×160, as a data-URI background, `opacity:0.05; mix-blend-mode:overlay;`
3. Vignette: `background:radial-gradient(1600px 1000px at 50% 42%, transparent 55%, rgba(2,3,6,0.55));`

## 3. Shared vocabulary

**Glass surface** (every panel/window): translucent dark bg + `backdrop-filter: blur(28px) saturate(1.5)` + `border:1px solid rgba(255,255,255,0.08)`.

**FiFi star** (logo + Spark): a square div with
`clip-path: polygon(50% 0%, 61% 39%, 100% 50%, 61% 61%, 50% 100%, 39% 61%, 0% 50%, 39% 39%);` — a slim 4-point star.

**Window chrome** — every window has a 38px header: `display:flex; align-items:center; gap:10px; padding:0 14px; background:rgba(255,255,255,0.03); border-bottom:1px solid rgba(255,255,255,0.05); position:relative;`
- Left: an app glyph (per window, below) + title `12.5px / 600 / rgba(233,237,243,0.75)`.
- Absolutely centered: drag grip — `26×3px`, radius 2, `rgba(255,255,255,0.12)`.
- Right (after `flex:1` spacer): three `7px` circles, gap 10px from flex: two `rgba(233,237,243,0.22)`, last one warm `oklch(0.72 0.13 35)` (close). No Windows-style square buttons, no macOS traffic-light colors.

## 4. Horizon bar (top panel)

`top:16px; left:16px; right:16px; height:44px; z-index:50; display:flex; align-items:center; gap:14px; padding:0 14px; border-radius:14px; background:rgba(12,16,23,0.55); backdrop-filter:blur(28px) saturate(1.5); border:1px solid rgba(255,255,255,0.08); box-shadow:0 12px 40px -12px rgba(0,0,0,0.5);`

Left → right:
1. Logo group (gap 9): 16px accent star with `filter:drop-shadow(0 0 8px color-mix(in oklab, var(--ac) 60%, transparent))`; wordmark "FiFi" Space Grotesk 700 / 14px.
2. Divider: `1×18px rgba(255,255,255,0.1)` (same divider used again later).
3. **Intent pills** (gap 6) — labels "Build" (active), "Research", "Play": `display:flex; align-items:center; gap:7px; padding:5px 13px; border-radius:99px; font-size:12.5px; font-weight:600; letter-spacing:0.02em; cursor:pointer;` each with a leading 5px dot.
   - Active: `background:var(--ac); color:#0a0d12;` dot `#0a0d12`.
   - Inactive: `background:rgba(255,255,255,0.05); border:1px solid rgba(255,255,255,0.07); color:rgba(233,237,243,0.62);` dot `rgba(233,237,243,0.35)`.
   - After them a 26px circle, `border:1px dashed rgba(255,255,255,0.14)`, centered "+", color 40% white; hover → 80% white, border 30% white.
   - JS: clicking a pill makes it active (swap styles).
4. `flex:1` spacer.
5. Wifi: 3 ascending bars, 3px wide, radius 2, heights 5/8/12, gap 2.5, `rgba(233,237,243,0.85)`, bottom-aligned.
6. Battery: outline `22×11px` radius 3.5 `border:1px solid rgba(233,237,243,0.4)` with 1.5px padding, inner fill 70% width `rgba(233,237,243,0.8)` radius 1.5; a `2×5px` nub after it (margin-left −3px); text "82%" JetBrains Mono 12px 60% white.
7. Divider.
8. Notification pill: `padding:4px 10px; border-radius:99px; background:rgba(255,255,255,0.05); border:1px solid rgba(255,255,255,0.07);` containing a 5px accent dot with `box-shadow:0 0 6px var(--ac)` + count "2" (11.5px / 600 / 70% white). Hover bg `rgba(255,255,255,0.1)`.
9. Divider.
10. Date "Wed, Jul 16" — 12.5px, 55% white. Live from JS: `toLocaleDateString('en-US',{weekday:'short',month:'short',day:'numeric'})`.
11. Clock "18:05" — Space Grotesk 600, 14.5px, `letter-spacing:0.02em`. Live, 24h `HH:MM`, updates every 30s.

## 5. Tiled window pair (terminal + files)

Terminal: `left:96px; top:132px; width:784px; height:648px`. Files: `left:890px; top:132px; width:530px; height:648px` (10px gap between them).
Both: `border-radius:16px; box-shadow:0 32px 90px -20px rgba(0,0,0,0.65); overflow:hidden;` + glass border. Terminal bg `rgba(9,12,17,0.82)`; files bg `rgba(13,17,24,0.72)`.

**Seam handle** (tiling affordance) centered on the gap: `left:881px; top:428px; width:8px; height:56px; border-radius:4px; background:rgba(255,255,255,0.14); border:1px solid rgba(255,255,255,0.12); cursor:col-resize;` hover → `background:color-mix(in oklab, var(--ac) 50%, transparent)`.

### 5a. Terminal window — "term — ~/projects/fifi-shell"

Header glyph: `❯_` JetBrains Mono 11px in accent.
Body: `padding:20px 24px; font-family:'JetBrains Mono',monospace; font-size:13px; line-height:1.75;`

Fetch block — flex row, gap 20:
- 38px accent star (opacity 0.9, margin-top 6px).
- Info column:
  - `ray@aurora` — "ray" accent 600, "@" 40% white, "aurora" violet #8F7BFF 600
  - dim separator line `─────────────` (25% white)
  - rows as label+value, label in a 76px inline-block column at 45% white:
    `os` FiFi OS 0.4 (aurora) · `kernel` 6.15.3-fifi · `shell` fish 4.0 · `wm` fifi-shell · wayland · `uptime` 3h 12m · `memory` 6.2 GiB / 32 GiB (the "/ 32 GiB" at 45%)

16px gap, then package-manager session:
```
❯ paru -Syu                        (❯ in accent)
:: Synchronizing package databases...   (:: violet, text 55% white)
 core is up to date
 extra            8.4 MiB  12.1 MiB/s
:: 4 packages to upgrade
 mesa 25.1.4 → 25.1.5  linux-fifi 6.15.3 → 6.15.4   (old versions 35% white, new versions #e9edf3)
████████████████████ 82%    (14 █ in accent, 6 █ at rgba(233,237,243,0.15), "82%" 55%)
```
16px gap, then prompt: `❯` in accent + block cursor `8×15px rgba(233,237,243,0.8)`.

### 5b. Files window — "files"

Header glyph: folder shape — `14×10px` radius 2.5 `rgba(233,237,243,0.4)` with a `7×3px` tab (radius 2 2 0 0) sitting on its top-left. (This folder-with-tab construction repeats at other sizes.)

Column layout, top to bottom:

**Toolbar** 46px (border-bottom hairline `rgba(255,255,255,0.05)`, padding 0 14, gap 12):
- `‹` `›` in 22px-wide centered divs, 16px, 45% white; `›` disabled at 20% white.
- Breadcrumb JetBrains Mono 12px: `home / ray / projects` — slashes 25% white, "projects" white 600, rest 45%.
- `flex:1`, then search pill: `padding:5px 11px; border-radius:99px; background:rgba(255,255,255,0.05); border:1px solid rgba(255,255,255,0.07);` mono accent `/` 11px + "search" 11.5px 40% white.

**Main row** (flex): sidebar 148px (border-right hairline; padding 14px 10px) + content.

Sidebar: eyebrow "PLACES", then rows `padding:6px 8px; border-radius:8px; font-size:12.5px; gap:8;` each with a 5px dot: Home, **Projects** (active: `background:color-mix(in oklab, var(--ac) 13%, transparent)`, white 600 text, accent dot), Downloads, Music, Trash (inactive: 60% white, 30%-white dot, hover bg `rgba(255,255,255,0.05)`). 14px gap. Eyebrow "TAGS": `shell` (accent dot), `design` (violet dot).

Content: folder grid `padding:14px; grid-template-columns:repeat(3,1fr); gap:10px;`
Tile: `padding:14px 10px 12px; border-radius:12px;` centered column, gap 8: folder shape `36×26px` radius 5 with `16×4px` tab; name 12px/600; count JetBrains Mono 10px 40% white (margin-top −4px).
- **Selected** (fifi-shell · 42 items, compositor · 18 items): `background:color-mix(in oklab, var(--ac) 10%, transparent); border:1px solid color-mix(in oklab, var(--ac) 45%, transparent);` folder shape `color-mix(in oklab, var(--ac) 55%, rgba(233,237,243,0.2))`.
- Normal (icons · 156, mockups · 7, sounds · 23, dotfiles · 31): `background:rgba(255,255,255,0.035); border:1px solid rgba(255,255,255,0.05);` folder `rgba(233,237,243,0.18)`; hover bg `rgba(255,255,255,0.06)`.

Below grid, eyebrow "RECENT", then 3 rows `padding:7px 10px; border-radius:9px;` hover bg 5% white: doc glyph (`12×15px` radius 2.5, `rgba(233,237,243,0.18)`, `border-top:3px solid rgba(233,237,243,0.35)`) + name 12.5px 85% white + `flex:1` + mono 10.5px 35% time:
`gestures.spec.md — 14:02`, `panel.blend — 11:47`, `spark-prompts.toml — yesterday`.

**Status bar** 34px (border-top hairline, padding 0 16): JetBrains Mono 10.5px 40% white. Left: `6 folders · 2 selected` ("2 selected" in accent). Right: `512 GB · 84 free`.

## 6. Spark ambient card (AI surface on the desktop)

`left:1556px; top:132px; width:324px; border-radius:18px; padding:18px; background:rgba(12,16,23,0.6);` glass blur/border, `box-shadow:0 24px 70px -18px rgba(0,0,0,0.6);`

1. Header row (gap 9): 14px star with `background:linear-gradient(135deg,var(--ac),#8F7BFF)`; "Spark" Space Grotesk 600 13.5px; `flex:1`; eyebrow "AMBIENT" (10px letter-spacing 0.1em).
2. Focus row (margin-top 16, gap 14): **progress ring** 52px — `background:conic-gradient(var(--ac) 252deg, rgba(255,255,255,0.09) 0);` masked to a ring via `mask:radial-gradient(circle, transparent 57%, #000 58%);` center label "2:14" JetBrains Mono 11px 600. Text: "Focus · Build" 14px/700; below "since 12:18 · notifications muted" 11.5px 50%.
3. Divider `1px rgba(255,255,255,0.07)`, margins `16px 0 12px`.
4. Eyebrow "SPARK SUGGESTS" (margin-bottom 8), then 2 chips (gap 8): `padding:10px 12px; border-radius:12px; background:rgba(255,255,255,0.04); border:1px solid rgba(255,255,255,0.06);` hover bg 7%; title 12.5px/600, sub 11px 45%, right accent `→`:
   - "Resume PR #142 · gestures" / "one failing test left yesterday"
   - "Standup in 25 min" / "wraps your Build session"
5. Footer (margin-top 14, gap 7): kbd pills `super` `space` — JetBrains Mono 10px, `padding:3px 7px; border-radius:6px; border:1px solid rgba(255,255,255,0.14);` 60% white — + "ask anything" 11px 40%.

## 7. Floating music window — "waves"

`left:1180px; top:660px; width:410px; z-index:10;` (overlaps the files window bottom-left corner). `border-radius:16px; background:rgba(13,17,24,0.78);` glass, heavier shadow `0 40px 110px -18px rgba(0,0,0,0.75);`

Header glyph: 3 accent equalizer bars (3px wide, radius 2, heights 7/12/9, gap 2, bottom-aligned). Title "waves".

Body — flex, gap 16, padding 16:
- Cover placeholder `132×132px` radius 12: `background:repeating-linear-gradient(45deg, rgba(233,237,243,0.06) 0 10px, rgba(233,237,243,0.02) 10px 20px); border:1px solid rgba(255,255,255,0.08);` centered mono 10px 40% label "cover art".
- Right column (vertically centered):
  - Eyebrow "NOW PLAYING" in **accent** (not dim).
  - "Midnight Drive" Space Grotesk 600 17px (margin-top 6).
  - "Solar Fields · Movements" 12.5px 50%.
  - Progress (margin-top 12): 4px track radius 99 `rgba(255,255,255,0.1)`; filled 44% in accent; thumb 10px white circle at 44%, `box-shadow:0 1px 6px rgba(0,0,0,0.5)`.
  - Times row: mono 10px 40%, "2:41" left / "6:05" right (margin-top 6).
  - Controls (margin-top 10, gap 18, vertically centered): prev = two left-pointing 9px CSS triangles side by side (65% opacity, hover 100%); play = 38px accent circle, two `4×14px` dark `#0a0d12` bars (gap 4), `box-shadow:0 0 20px color-mix(in oklab, var(--ac) 45%, transparent)`, hover `scale(1.06)`; next = mirrored triangles.

## 8. Dock (bottom center)

`left:50%; bottom:18px; transform:translateX(-50%); z-index:50; display:flex; align-items:center; gap:7px; padding:9px; border-radius:22px; background:rgba(12,16,23,0.55);` glass blur/border, `box-shadow:0 20px 60px -12px rgba(0,0,0,0.6);`

Icon tile: `48×48px; border-radius:14px; display:grid; place-items:center; cursor:pointer; transition:all .15s;` hover: `background:rgba(255,255,255,0.08); transform:translateY(-3px);` Glyph color `rgba(223,230,238,0.85)`.
Running indicator: 4px accent dot centered 2px from bottom — on terminal, files, music.

Order:
1. Terminal — `❯_` JetBrains Mono 14px/600 (running dot)
2. Files — folder 20×14 radius 3 + 9×4 tab (running dot)
3. Editor — `{ }` JetBrains Mono 14px/600
4. Browser — globe: 20px circle `border:1.5px solid`, horizontal line through middle, and a 9px-wide ellipse (border left/right only) centered — all same color
5. Divider `1×30px rgba(255,255,255,0.1)`, margin 0 3px
6. **Spark orb** — `54×54px; border-radius:16px; background:linear-gradient(135deg, color-mix(in oklab, var(--ac) 30%, transparent), color-mix(in oklab, #8F7BFF 30%, transparent)); border:1px solid color-mix(in oklab, var(--ac) 40%, transparent); box-shadow:0 0 26px color-mix(in oklab, var(--ac) 30%, transparent);` white star 22px (`linear-gradient(135deg,#fff,#cfe9ec)`). Hover: `translateY(-3px) scale(1.04)` + glow 36px at 50%.
7. Divider
8. Music — 3 equalizer bars 3.5px wide, heights 9/16/12 (running dot)
9. Settings — 3 slider rows: `18×2px` bars (radius 2) each with a 6px circle knob at a different x (left 3px / right 2px / left 7px), stacked gap 4

## 9. Behavior (vanilla JS)

- Clock + date live, update every 30s.
- Intent pills switch active state on click.
- All hovers above via CSS `:hover`. Transitions 150ms ease.
- No other functionality required — this is a static hi-fi mockup.

## 10. Verify before declaring done

Open `index.html` in a browser at 1920×1080 and compare against `screenshots/desktop-full.png` region by region (use the numbered crops):
1. Coral accent everywhere (pills, star, seam hover, selection, orb, play button) — never blue/cyan.
2. Window headers: grip centered, warm close dot rightmost.
3. Terminal content alignment: 76px label column, progress bar glyph counts (14 filled / 6 dim).
4. Files: exactly 2 selected tiles with coral tint; breadcrumb weights; status bar counts.
5. Spark ring at 252° with "2:14" centered; two suggestion chips; kbd pills.
6. Music window overlaps the files window; play button glows coral.
7. Dock: 4 icons | divider | orb | divider | 2 icons; running dots under terminal/files/music; hover lifts.
8. Wallpaper: teal top-right glow, violet bottom-left glow, grain, vignette — dark navy, not black.
