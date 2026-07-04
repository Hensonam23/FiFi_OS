#!/bin/sh
# FiFi App Store — update checker.
# For each installed app that recorded a source (<Name>.src) and installed URL
# (<Name>.url), re-resolve the latest x86_64 AppImage URL and compare. If it
# differs, an update is available → touch <Name>.update (containing the new URL);
# otherwise remove any stale <Name>.update. The App Store shows an Update button
# when <Name>.update exists.
#
# Usage: appstore-update-check.sh [AppName]   (no arg = check all installed apps)
APPS=/fifi-data/apps

# pick best x86_64 AppImage from a newline-separated URL list
pick() {
    u=$(printf '%s\n' "$1" | grep -iE 'x86_64|amd64|linux' | head -1)
    [ -n "$u" ] || u=$(printf '%s\n' "$1" | grep -viE 'arm|aarch|i386|i686' | head -1)
    [ -n "$u" ] || u=$(printf '%s\n' "$1" | head -1)
    printf '%s' "$u"
}

# resolve latest download URL for a source spec (same logic as appstore-install.sh)
resolve() {
    spec="$1"
    case "$spec" in
    url:*)
        printf '%s' "${spec#url:}" ;;
    gitlab:*)
        rel=$(curl -sL --max-time 30 "https://gitlab.com/api/v4/projects/${spec#gitlab:}/releases?per_page=10")
        pick "$(printf '%s' "$rel" | grep -oE '"url":"[^"]*\.AppImage"' | sed 's/^"url":"//;s/"$//')" ;;
    *)
        rel=$(curl -sL --max-time 30 "https://api.github.com/repos/$spec/releases?per_page=30")
        pick "$(printf '%s' "$rel" | grep -oE '"browser_download_url": *"[^"]*\.AppImage"' \
                | sed 's/.*"browser_download_url": *"//;s/"$//')" ;;
    esac
}

check_one() {
    n="$1"
    [ -f "$APPS/$n.src" ] || return
    cur=$(cat "$APPS/$n.url" 2>/dev/null)
    new=$(resolve "$(cat "$APPS/$n.src")")
    if [ -n "$new" ] && [ "$new" != "$cur" ]; then
        printf '%s' "$new" > "$APPS/$n.update"
        echo "update available: $n"
    else
        rm -f "$APPS/$n.update"
    fi
}

if [ -n "$1" ]; then
    check_one "$1"
else
    for f in "$APPS"/*.src; do
        [ -f "$f" ] || continue
        b=$(basename "$f" .src)
        check_one "$b"
    done
fi
echo "appstore-update-check: done"
