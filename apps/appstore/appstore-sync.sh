#!/bin/sh
# FiFi App Store — catalog sync.
# Fetches the live AppImageHub feed and parses it into a categorized catalog the
# App Store UI reads. One line per app:  name <TAB> category <TAB> owner/repo <TAB> icon_path
#
# Categories come from the feed's freedesktop categories; toolkit/generic tags
# (Qt/GTK/GNOME/KDE/Application) are skipped so the first *meaningful* category wins.
#
# Output: /fifi-data/apps/catalog.tsv   (+ caches the raw feed.json)
set -e
APPS=/fifi-data/apps
mkdir -p "$APPS"

curl -sL --max-time 30 https://appimage.github.io/feed.json -o "$APPS/feed.json" || {
    echo "appstore-sync: feed download failed" >&2
    [ -s "$APPS/feed.json" ] || exit 1   # keep an old cache if present
}

awk '
/"name":[[:space:]]*"/ {
    if (name=="") { n=$0; sub(/.*"name":[[:space:]]*"/,"",n); sub(/".*/,"",n); name=n }
}
/"categories"[[:space:]]*:[[:space:]]*\[/ {
    line=$0; sub(/.*"categories"[[:space:]]*:[[:space:]]*\[/,"",line);
    while (match(line, /"[^"]+"/)) {
        t=substr(line,RSTART+1,RLENGTH-2); line=substr(line,RSTART+RLENGTH);
        if (t!="Qt" && t!="GTK" && t!="GNOME" && t!="KDE" && t!="Application" && t!="null" && t!="") { cat=t; break }
    }
}
/"type":"GitHub","url":"/ { r=$0; sub(/.*"type":"GitHub","url":"/,"",r); sub(/".*/,"",r); repo=r }
/"icons"[[:space:]]*:[[:space:]]*\[/ { ic=$0; sub(/.*"icons"[[:space:]]*:[[:space:]]*\["/,"",ic); sub(/".*/,"",ic); icon=ic }
/^[[:space:]]*},?[[:space:]]*$/ {
    if (name!="" && repo!="") { if (cat=="") cat="Other"; print name"\t"cat"\t"repo"\t"icon }
    name=""; repo=""; icon=""; cat=""
}
' "$APPS/feed.json" | sort -f > "$APPS/catalog.tsv.feed"

# Curated big-name apps first (the feed only lists GitHub-hosted projects, so
# staples like LibreOffice/Firefox/Steam are missing from it). Sources verified
# to publish x86_64 .AppImage release assets. gitlab:<project> uses GitLab API.
cat > "$APPS/catalog.tsv" <<'FEATURED'
LibreOffice	Office	url:https://appimages.libreitalia.org/LibreOffice-fresh.standard-x86_64.AppImage
LibreWolf	Network	gitlab:librewolf-community%2Fbrowser%2Fappimage
Firefox	Network	ivan-hc/Firefox-appimage
Brave	Network	ivan-hc/Brave-appimage
GIMP	Graphics	ivan-hc/GIMP-appimage
Blender	Graphics	ivan-hc/Blender-appimage
VLC	AudioVideo	ivan-hc/VLC-appimage
OBS-Studio	AudioVideo	ivan-hc/OBS-Studio-appimage
Audacity	AudioVideo	audacity/audacity
Steam	Game	ivan-hc/Steam-appimage
VSCodium	Development	VSCodium/vscodium
GitHub-Desktop	Development	shiftkey/desktop
Obsidian	Office	obsidianmd/obsidian-releases
qBittorrent	Network	qbittorrent/qBittorrent
LocalSend	Utility	localsend/localsend
FEATURED
grep -vE '^(LibreOffice|Firefox|Brave|GIMP|Blender|VLC|Audacity|Steam|VSCodium|Obsidian|qBittorrent|LocalSend)	' \
    "$APPS/catalog.tsv.feed" >> "$APPS/catalog.tsv" || cat "$APPS/catalog.tsv.feed" >> "$APPS/catalog.tsv"
rm -f "$APPS/catalog.tsv.feed"

echo "appstore-sync: $(wc -l < "$APPS/catalog.tsv") apps"
