#!/bin/sh
# One-way fixes for persistent App Store metadata. This runs as uid 1000.
set -eu

apps_dir="${FIFI_APPS_DIR:-/fifi-data/apps}"
data_root="${FIFI_DATA_ROOT:-/fifi-data}"
source_file="$apps_dir/LibreOffice.src"
legacy_source='url:https://appimages.libreitalia.org/LibreOffice-fresh.standard-x86_64.AppImage'
if [ -f "$source_file" ] && grep -Fxq "$legacy_source" "$source_file"; then
    printf '%s' 'ivan-hc/LibreOffice-appimage' > "$source_file"
fi

# Browser Setup historically kept a generic AppImage outside the App Store, so
# LibreWolf's disabled AppImage self-updater had no FiFi-managed replacement
# path. Move that browser into the normal library and attach source metadata.
choice="$(cat "$data_root/browser/choice" 2>/dev/null || true)"
case "$choice" in
    firefox|Firefox)
        browser_name=Firefox
        browser_source=ivan-hc/Firefox-appimage
        ;;
    *)
        browser_name=LibreWolf
        browser_source=codeberg:librewolf/bsys6
        ;;
esac
legacy_browser="$data_root/browser/browser.AppImage"
browser_app="$apps_dir/$browser_name.AppImage"
mkdir -p "$apps_dir"
if [ -f "$legacy_browser" ] && [ ! -L "$legacy_browser" ] && \
   [ ! -e "$browser_app" ] && [ ! -L "$browser_app" ]; then
    mv "$legacy_browser" "$browser_app"
fi
if [ -f "$browser_app" ] && [ ! -L "$browser_app" ]; then
    [ -f "$apps_dir/$browser_name.src" ] ||
        printf '%s' "$browser_source" > "$apps_dir/$browser_name.src"
    [ -f "$apps_dir/$browser_name.url" ] ||
        printf '%s' 'migrated-browser-setup' > "$apps_dir/$browser_name.url"
    [ -f "$apps_dir/$browser_name.sha256" ] ||
        sha256sum "$browser_app" | awk '{print $1}' > "$apps_dir/$browser_name.sha256"
    if [ ! -f "$apps_dir/$browser_name.sh" ]; then
        printf '#!/bin/sh\nexec /usr/share/fifi/fifi-run "/fifi-data/apps/%s.AppImage" "$@"\n' \
            "$browser_name" > "$apps_dir/$browser_name.sh"
        chmod 0700 "$apps_dir/$browser_name.sh"
    fi
fi
