#!/bin/sh
# FiFi App Store — uninstall an app installed by appstore-install.sh.
# Kills any running instance, deletes the AppImage + its extracted dir, icon,
# launcher, and status file, and removes its desktop-icon line.
#
# Usage: appstore-uninstall.sh <AppName>
# Writes progress to /fifi-data/apps/<AppName>.status: removing|gone|error
name="$1"
[ -n "$name" ] || { echo "usage: appstore-uninstall.sh <AppName>" >&2; exit 2; }

APPS="${FIFI_APPS_DIR:-/fifi-data/apps}"
status="$APPS/$name.status"
echo removing > "$status"

# Kill any running processes launched from this app's extracted dir.
needle="$APPS/$name.d/"
for c in /proc/[0-9]*/cmdline; do
    pid=${c#/proc/}; pid=${pid%/cmdline}
    args=$(tr '\0' ' ' < "$c" 2>/dev/null)
    case "$args" in
        *"$needle"*|*"$APPS/$name.AppImage"*) kill -9 "$pid" 2>/dev/null ;;
    esac
done

# Remove files.
rm -f  "$APPS/$name.AppImage" "$APPS/$name.png" "$APPS/$name.sh"
rm -rf "$APPS/$name.d"

# Strip the desktop-icon line(s) referencing this app (launcher or AppImage path).
conf="${FIFI_DESKTOP_CONF:-/fifi-data/fifi-desktop.conf}"
if [ -f "$conf" ]; then
    grep -v "	$name\$" "$conf" 2>/dev/null > "$conf.tmp" || true
    mv "$conf.tmp" "$conf" 2>/dev/null || true
fi

rm -f "$status"
echo "appstore-uninstall: removed $name"
