#!/bin/sh
# FiFi App Store — install an app.
# Resolves the latest .AppImage asset for a GitHub repo, downloads it to
# /fifi-data/apps/, makes it executable, and registers a desktop icon that
# launches it (via the existing desktop-shortcut system, fifi-desktop.conf).
#
# Usage: appstore-install.sh <source> <AppName>
#   <source>: owner/repo (GitHub) | gitlab:<url-encoded-project> |
#             url:<direct-AppImage-url> | file:<local-AppImage-path>
# Writes progress markers to <apps>/<AppName>.status: resolving|downloading|done|error
#
# FIFI_APPS_DIR / FIFI_DESKTOP_CONF override the target paths — the disk
# installer uses this to install onto the freshly-formatted data partition.
set -e
repo="$1"
name="$2"
[ -n "$repo" ] && [ -n "$name" ] || { echo "usage: appstore-install.sh <source> <AppName>" >&2; exit 2; }

APPS="${FIFI_APPS_DIR:-/fifi-data/apps}"
DESKTOP_CONF="${FIFI_DESKTOP_CONF:-/fifi-data/fifi-desktop.conf}"
mkdir -p "$APPS"
status="$APPS/$name.status"
echo resolving > "$status"

# Search the LIST of recent releases (not just /latest) — many repos (e.g.
# bitwarden/clients) ship multiple products, so /latest is often a non-desktop
# release with no AppImage. Grab the newest x86_64 AppImage across recent releases.
# "gitlab:<url-encoded-project-path>" entries use the GitLab releases API instead.
srcfile=""
case "$repo" in
file:*)
    # local AppImage (USB offline bundle) — no network at all
    srcfile="${repo#file:}"
    [ -f "$srcfile" ] || { echo error > "$status"; echo "no such file: $srcfile" >&2; exit 1; }
    allurls="file"
    ;;
url:*)
    # direct download URL — no release-API resolution needed
    allurls="${repo#url:}"
    ;;
gitlab:*)
    proj="${repo#gitlab:}"
    rel=$(curl -sL --max-time 30 "https://gitlab.com/api/v4/projects/$proj/releases?per_page=10")
    allurls=$(printf '%s' "$rel" | grep -oE '"url":"[^"]*\.AppImage"' \
              | sed 's/^"url":"//;s/"$//')
    ;;
*)
    rel=$(curl -sL --max-time 30 "https://api.github.com/repos/$repo/releases?per_page=30")
    allurls=$(printf '%s' "$rel" | grep -oE '"browser_download_url": *"[^"]*\.AppImage"' \
              | sed 's/.*"browser_download_url": *"//;s/"$//')
    ;;
esac
# Prefer x86_64/amd64/linux; else any AppImage; releases are newest-first so head -1 = newest.
url=$(printf '%s\n' "$allurls" | grep -iE 'x86_64|amd64|linux' | head -1)
[ -n "$url" ] || url=$(printf '%s\n' "$allurls" | grep -viE 'arm|aarch|i386|i686' | head -1)
[ -n "$url" ] || url=$(printf '%s\n' "$allurls" | head -1)

if [ -z "$url" ]; then echo error > "$status"; echo "no .AppImage asset for $repo" >&2; exit 1; fi

dest="$APPS/$name.AppImage"
was_installed=0
[ -f "$dest" ] && was_installed=1   # update/reinstall, not a first install
echo downloading > "$status"
if [ -n "$srcfile" ]; then
    cp "$srcfile" "$dest.part" || { echo error > "$status"; exit 1; }
else
    curl -sL --max-time 600 "$url" -o "$dest.part" || { echo error > "$status"; exit 1; }
fi
mv "$dest.part" "$dest"
chmod +x "$dest"

# Record the source spec + resolved URL for update checking, and clear any
# pending-update marker (we just installed the latest we could resolve).
printf '%s' "$repo" > "$APPS/$name.src"
printf '%s' "$url"  > "$APPS/$name.url"
rm -f "$APPS/$name.update"

# Pre-extract now (no FUSE on FiFi OS) so first launch is instant. Re-extract
# fresh every time (rm old dir) so an update replaces the previous version.
echo extracting > "$status"
ext="$APPS/$name.d"
rm -rf "$ext"
mkdir -p "$ext"
( cd "$ext" && "$dest" --appimage-extract >/dev/null 2>&1 ) || true

# App icon: AppImages carry their logo as squashfs-root/.DirIcon, but some ship
# a tiny placeholder there (LibreOffice's is 1.3KB), so also scan the top level
# and the hicolor icon dirs and keep the LARGEST PNG. The existing icon competes
# too, so a good icon is never downgraded, but a placeholder gets replaced on
# the next install/update.
icon_png="$APPS/$name.png"
if [ -d "$ext/squashfs-root" ]; then
    best=""; bestsz=0
    if [ -f "$icon_png" ]; then best="$icon_png"; bestsz=$(wc -c < "$icon_png"); fi
    for f in "$ext/squashfs-root/.DirIcon" \
             "$ext/squashfs-root"/*.png \
             "$ext/squashfs-root"/usr/share/icons/hicolor/512x512/apps/*.png \
             "$ext/squashfs-root"/usr/share/icons/hicolor/256x256/apps/*.png \
             "$ext/squashfs-root"/usr/share/icons/hicolor/128x128/apps/*.png; do
        # resolve (possibly chained) symlinks inside the extracted tree
        n=0
        while [ -h "$f" ] && [ $n -lt 5 ]; do
            tgt=$(readlink "$f")
            case "$tgt" in /*) f="$tgt";; *) f="$(dirname "$f")/$tgt";; esac
            n=$((n+1))
        done
        [ -f "$f" ] || continue
        sz=$(wc -c < "$f")
        [ "$sz" -gt "$bestsz" ] && { best="$f"; bestsz=$sz; }
    done
    if [ -n "$best" ] && [ "$best" != "$icon_png" ]; then
        cp "$best" "$icon_png" 2>/dev/null || true
    fi
fi

# Per-app launcher: everything runs through fifi-run (shared runtime, uniform
# env), and the desktop-icon system only needs a plain executable path.
# Paths inside the launcher are the FINAL runtime paths (/fifi-data/...), even
# when installing onto a mounted target partition.
launcher="$APPS/$name.sh"
printf '#!/bin/sh\nexec /fifi-data/apps/fifi-run "/fifi-data/apps/%s.AppImage" "$@"\n' "$name" > "$launcher"
chmod +x "$launcher"

# Register a desktop icon (path<TAB>label; read by gui_desktop_load) — but only
# on a FIRST install. Updates/reinstalls never re-add it, so removing an icon
# from the desktop is a decision that sticks. The app always stays available
# in the launcher and the App Store's Installed tab regardless.
if [ "$was_installed" = 0 ]; then
    touch "$DESKTOP_CONF"
    if ! grep -q "	$name\$" "$DESKTOP_CONF" 2>/dev/null; then
        printf 'icon=%s\t%s\n' "/fifi-data/apps/$name.sh" "$name" >> "$DESKTOP_CONF"
    fi
fi

echo done > "$status"
echo "appstore-install: $name -> $dest"
