#!/bin/sh
# FiFi App Store — catalog sync.
# Fetches the live AppImageHub feed and parses it into a categorized catalog the
# App Store UI reads. One line per app:
#   name <TAB> category <TAB> owner/repo <TAB> icon_path <TAB> description
#
# Categories come from the feed's freedesktop categories; toolkit/generic tags
# (Qt/GTK/GNOME/KDE/Application) are skipped so the first *meaningful* category wins.
# icon_path is relative to https://appimage.github.io/database/ (PNG only; SVGs skipped).
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
/"description":[[:space:]]*"/ {
    if (name!="" && desc=="") {
        d=$0; sub(/.*"description":[[:space:]]*"/,"",d); sub(/",?[[:space:]]*$/,"",d)
        gsub(/\\"/,"\"",d); gsub(/\\n/," ",d); gsub(/\\\\/,"\\",d)
        gsub(/<[^>]*>/," ",d); gsub(/\t/," ",d); gsub(/  +/," ",d)
        if (length(d)>500) d=substr(d,1,500)
        desc=d
    }
}
/"categories"[[:space:]]*:[[:space:]]*\[/ {
    line=$0; sub(/.*"categories"[[:space:]]*:[[:space:]]*\[/,"",line);
    while (match(line, /"[^"]+"/)) {
        t=substr(line,RSTART+1,RLENGTH-2); line=substr(line,RSTART+RLENGTH);
        if (t!="Qt" && t!="GTK" && t!="GNOME" && t!="KDE" && t!="Application" && t!="null" && t!="") { cat=t; break }
    }
}
/"type":"GitHub","url":"/ { r=$0; sub(/.*"type":"GitHub","url":"/,"",r); sub(/".*/,"",r); repo=r }
/"icons"[[:space:]]*:[[:space:]]*\[/ { ic=$0; sub(/.*"icons"[[:space:]]*:[[:space:]]*\["/,"",ic); sub(/".*/,"",ic); if (ic !~ /\.svg$/) icon=ic }
/^[[:space:]]*},?[[:space:]]*$/ {
    if (name!="" && repo!="") { if (cat=="") cat="Other"; print name"\t"cat"\t"repo"\t"icon"\t"desc }
    name=""; repo=""; icon=""; cat=""; desc=""
}
' "$APPS/feed.json" | sort -f > "$APPS/catalog.tsv.feed"

# Curated big-name apps first (the feed only lists GitHub-hosted projects, so
# staples like LibreOffice/Firefox/Steam are missing from it). Sources verified
# to publish x86_64 .AppImage release assets. gitlab:<project> uses GitLab API.
cat > "$APPS/catalog.tsv" <<'FEATURED'
LibreOffice	Office	url:https://appimages.libreitalia.org/LibreOffice-fresh.standard-x86_64.AppImage		Full-featured office suite: Writer, Calc, Impress and more. Compatible with Microsoft Office documents.
LibreWolf	Network	gitlab:librewolf-community%2Fbrowser%2Fappimage	LibreWolf/icons/128x128/librewolf.png	A privacy-focused fork of Firefox with telemetry removed and hardened defaults.
Firefox	Network	ivan-hc/Firefox-appimage	Firefox/icons/128x128/firefox.png	Mozilla's fast, private and open-source web browser.
Brave	Network	ivan-hc/Brave-appimage		Privacy-first web browser with built-in ad and tracker blocking.
GIMP	Graphics	ivan-hc/GIMP-appimage		The GNU Image Manipulation Program - a powerful free image editor.
Blender	Graphics	ivan-hc/Blender-appimage		Professional 3D modeling, animation and rendering suite.
VLC	AudioVideo	ivan-hc/VLC-appimage	VLC/icons/128x128/vlc.png	Plays virtually any audio or video file, disc or stream.
OBS-Studio	AudioVideo	ivan-hc/OBS-Studio-appimage		Live streaming and screen recording studio.
Audacity	AudioVideo	audacity/audacity		Free multi-track audio editor and recorder.
Steam	Game	ivan-hc/Steam-appimage		Valve's game store and launcher - install and play your Steam library.
VSCodium	Development	VSCodium/vscodium		Community build of VS Code without Microsoft telemetry.
GitHub-Desktop	Development	shiftkey/desktop		Manage your GitHub repositories with a simple graphical interface.
Obsidian	Office	obsidianmd/obsidian-releases	Obsidian/icons/128x128/obsidian.png	Powerful knowledge base and note-taking app built on local Markdown files.
qBittorrent	Network	qbittorrent/qBittorrent	qBittorrent_Enhanced_Edition/icons/128x128/qbittorrent.png	Free and open-source BitTorrent client.
LocalSend	Utility	localsend/localsend		Share files with nearby devices over Wi-Fi - no internet needed.
FEATURED
grep -vE '^(LibreOffice|Firefox|Brave|GIMP|Blender|VLC|Audacity|Steam|VSCodium|Obsidian|qBittorrent|LocalSend)	' \
    "$APPS/catalog.tsv.feed" >> "$APPS/catalog.tsv" || cat "$APPS/catalog.tsv.feed" >> "$APPS/catalog.tsv"
rm -f "$APPS/catalog.tsv.feed"

echo "appstore-sync: $(wc -l < "$APPS/catalog.tsv") apps"
