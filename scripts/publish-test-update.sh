#!/usr/bin/env bash
# Publish build-linux/update-test as the fixed linux-desktop-test prerelease.
# This never commits, pushes, or merges source code.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${FIFI_UPDATE_REPO:-Hensonam23/FiFi_OS}"
TAG="linux-desktop-test"
ASSET_DIR="$ROOT/build-linux/update-test"
TEST_ISO="$ROOT/build-linux/fifi-linux.iso"
BUILD_ID="$(git -C "$ROOT" rev-parse HEAD)"

if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=normal)" ]]; then
    echo "[publish-update] refusing to publish from a dirty working tree" >&2
    echo "Commit the fully verified linux-desktop changes, then rebuild the release assets." >&2
    exit 1
fi

for file in bzImage initramfs.cpio.gz fifi-update.manifest \
            fifi-update.manifest.sig fifi-bootstrap-update; do
    [[ -s "$ASSET_DIR/$file" ]] || {
        echo "[publish-update] missing asset: $ASSET_DIR/$file" >&2
        echo "Run: make linux-test-update" >&2
        exit 1
    }
done
ASSETS=(
    "$ASSET_DIR/bzImage"
    "$ASSET_DIR/initramfs.cpio.gz"
    "$ASSET_DIR/fifi-update.manifest"
    "$ASSET_DIR/fifi-update.manifest.sig"
    "$ASSET_DIR/fifi-bootstrap-update"
)
if [[ -s "$TEST_ISO" ]]; then
    ASSETS+=("$TEST_ISO")
else
    echo "[publish-update] optional test ISO not present; publishing in-place update only"
fi

manifest_build="$(sed -n 's/^build=//p' "$ASSET_DIR/fifi-update.manifest")"
[[ "$manifest_build" == "$BUILD_ID" ]] || {
    echo "[publish-update] manifest does not match current commit" >&2
    echo "manifest: $manifest_build" >&2
    echo "current:  $BUILD_ID" >&2
    exit 1
}

GH=(gh)
if ! command -v gh >/dev/null 2>&1; then
    if command -v flatpak-spawn >/dev/null 2>&1 &&
       flatpak-spawn --host sh -lc 'command -v gh >/dev/null 2>&1'
    then
        GH=(flatpak-spawn --host gh)
    else
        echo "[publish-update] GitHub CLI is required" >&2
        exit 1
    fi
fi

"${GH[@]}" auth status >/dev/null

notes="Automated FiFi OS linux-desktop test-channel build.

Commit: $BUILD_ID

This prerelease is consumed only by systems on the test update channel."

if "${GH[@]}" release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    "${GH[@]}" release upload "$TAG" \
        "${ASSETS[@]}" \
        --clobber --repo "$REPO"
    "${GH[@]}" release edit "$TAG" \
        --target "$BUILD_ID" \
        --title "FiFi OS Linux Desktop Test" \
        --notes "$notes" \
        --prerelease --latest=false \
        --repo "$REPO"
else
    "${GH[@]}" release create "$TAG" \
        "${ASSETS[@]}" \
        --target "$BUILD_ID" \
        --title "FiFi OS Linux Desktop Test" \
        --notes "$notes" \
        --prerelease --latest=false \
        --repo "$REPO"
fi

echo "[publish-update] test channel now points to build $BUILD_ID"
