#!/bin/sh
# FiFi App Store — install an app.
# Resolves the latest .AppImage asset for a GitHub repo, downloads it to
# /fifi-data/apps/, makes it executable, and registers a desktop icon that
# launches it (via the existing desktop-shortcut system, fifi-desktop.conf).
#
# Usage: appstore-install.sh <owner/repo> <AppName>
# Writes progress markers to /fifi-data/apps/<AppName>.status: resolving|downloading|done|error
set -e
repo="$1"
name="$2"
[ -n "$repo" ] && [ -n "$name" ] || { echo "usage: appstore-install.sh <owner/repo> <AppName>" >&2; exit 2; }

APPS=/fifi-data/apps
mkdir -p "$APPS"
status="$APPS/$name.status"
echo resolving > "$status"

# Search the LIST of recent releases (not just /latest) — many repos (e.g.
# bitwarden/clients) ship multiple products, so /latest is often a non-desktop
# release with no AppImage. Grab the newest x86_64 AppImage across recent releases.
# "gitlab:<url-encoded-project-path>" entries use the GitLab releases API instead.
case "$repo" in
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
echo downloading > "$status"
curl -sL --max-time 600 "$url" -o "$dest.part" || { echo error > "$status"; exit 1; }
mv "$dest.part" "$dest"
chmod +x "$dest"

# Pre-extract now (no FUSE on FiFi OS) so first launch is instant.
echo extracting > "$status"
ext="$APPS/$name.d"
if [ ! -d "$ext/squashfs-root" ]; then
    mkdir -p "$ext"
    ( cd "$ext" && "$dest" --appimage-extract >/dev/null 2>&1 ) || true
fi

# App icon: AppImages carry their logo as squashfs-root/.DirIcon (or a top-level
# .png). Copy it next to the launcher as <Name>.png — the desktop draws it.
icon_png="$APPS/$name.png"
if [ ! -f "$icon_png" ] && [ -d "$ext/squashfs-root" ]; then
    src_icon=""
    if [ -e "$ext/squashfs-root/.DirIcon" ]; then
        src_icon="$ext/squashfs-root/.DirIcon"
        # .DirIcon may be a (possibly chained) symlink — resolve inside the dir
        n=0
        while [ -h "$src_icon" ] && [ $n -lt 5 ]; do
            tgt=$(readlink "$src_icon")
            case "$tgt" in /*) src_icon="$tgt";; *) src_icon="$ext/squashfs-root/$tgt";; esac
            n=$((n+1))
        done
    fi
    if [ ! -f "$src_icon" ]; then
        for f in "$ext/squashfs-root"/*.png; do [ -f "$f" ] && src_icon="$f" && break; done
    fi
    [ -f "$src_icon" ] && cp "$src_icon" "$icon_png" 2>/dev/null || true
fi

# Per-app launcher: everything runs through fifi-run (shared runtime, uniform
# env), and the desktop-icon system only needs a plain executable path.
launcher="$APPS/$name.sh"
printf '#!/bin/sh\nexec /fifi-data/apps/fifi-run "%s" "$@"\n' "$dest" > "$launcher"
chmod +x "$launcher"

# Register a desktop icon (path<TAB>label). fifi-desktop.conf is read by gui_desktop_load.
touch /fifi-data/fifi-desktop.conf
if ! grep -q "	$name\$" /fifi-data/fifi-desktop.conf 2>/dev/null; then
    printf 'icon=%s\t%s\n' "$launcher" "$name" >> /fifi-data/fifi-desktop.conf
fi

echo done > "$status"
echo "appstore-install: $name -> $dest"
