# FiFi OS Fonts

A complete, cross-platform font set so users can use their preferred font —
whether it originates on Windows, macOS, or Linux — with no extra installation.

## What ships

**Base image (already present):**
- **DejaVu** — Sans / Serif / Mono, the classic Linux defaults
- **Noto** — Google's "No Tofu" family: near-complete Unicode script coverage
  (Latin, Cyrillic, Greek, Arabic, Hebrew, Devanagari, Thai, Armenian, …) plus
  **Noto Color Emoji**

**Added by this bundle (`install-fonts.sh`):**
- **Liberation** Sans / Serif / Mono — metric-compatible with Arial, Times New
  Roman, Courier New
- **Carlito** — metric-compatible with Calibri
- **Caladea** — metric-compatible with Cambria
- **Noto CJK** — Noto Sans/Serif CJK (Chinese, Japanese, Korean)

## Windows & macOS fonts

Microsoft (Segoe UI, Calibri, Arial, …) and Apple (San Francisco, Helvetica
Neue, PingFang, …) system fonts are **proprietary and cannot be legally
redistributed**, so FiFi OS does not bundle the originals. Instead
`60-fifi-aliases.conf` maps every common Windows/macOS font name onto a bundled
free font of matching metrics and appearance. Applications that request those
names — web pages, documents, GTK apps — render correctly and consistently.

This is the same approach every mainstream Linux distribution uses to provide
"Windows font compatibility" without shipping licensed binaries.

## Installing / rebuilding

```sh
# Default: install into the browser sysroot on a running FiFi OS box
./install-fonts.sh

# Or target a specific sysroot / font source (image build)
./install-fonts.sh /path/to/sysroot /path/to/host/fonts
```

The script copies the family folders into `usr/share/fonts`, installs the alias
config into `etc/fonts/conf.d`, and rebuilds the fontconfig cache.

## Verifying

```sh
fc-match 'Arial'            # -> LiberationSans-Regular.ttf "Liberation Sans"
fc-match 'Calibri'          # -> Carlito-Regular.ttf "Carlito"
fc-match 'Segoe UI'         # -> NotoSans-Regular.ttf "Noto Sans"
fc-match 'Times New Roman'  # -> LiberationSerif-Regular.ttf "Liberation Serif"
fc-match 'San Francisco'    # -> NotoSans-Regular.ttf "Noto Sans"
fc-match 'PingFang SC'      # -> Noto Sans CJK SC
```
