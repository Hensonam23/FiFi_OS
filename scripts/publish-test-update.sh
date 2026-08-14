#!/usr/bin/env bash
# Verify and publish build-linux/update-test as the fixed linux-desktop-test
# prerelease. This never commits, pushes, or merges source code.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${FIFI_UPDATE_REPO:-Hensonam23/FiFi_OS}"
TAG="linux-desktop-test"
ASSET_DIR="${FIFI_UPDATE_ASSET_DIR:-$ROOT/build-linux/update-test}"
TEST_ISO="${FIFI_UPDATE_TEST_ISO:-$ROOT/build-linux/fifi-linux.iso}"
ISO_RELEASE_INFO="${FIFI_UPDATE_ISO_RELEASE_INFO:-$TEST_ISO.release-info}"
PUBLIC_KEY="${FIFI_RELEASE_PUBLIC_KEY:-$ROOT/security/release-signing-public.pem}"
BUILD_ID="$(git -C "$ROOT" rev-parse HEAD)"
MODE=publish

case "${1:-}" in
    "") ;;
    --check) MODE=check ;;
    -h|--help)
        echo "usage: $0 [--check]"
        exit 0
        ;;
    *)
        echo "usage: $0 [--check]" >&2
        exit 2
        ;;
esac

if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=normal)" ]]; then
    echo "[publish-update] refusing to publish from a dirty working tree" >&2
    echo "Commit the fully verified linux-desktop changes, then rebuild the release assets." >&2
    exit 1
fi

for file in bzImage initramfs.cpio.gz fifi-update.manifest \
            fifi-update.manifest.sig; do
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
)

manifest_build="$(sed -n 's/^build=//p' "$ASSET_DIR/fifi-update.manifest")"
[[ "$manifest_build" == "$BUILD_ID" ]] || {
    echo "[publish-update] manifest does not match current commit" >&2
    echo "manifest: $manifest_build" >&2
    echo "current:  $BUILD_ID" >&2
    exit 1
}

manifest_format="$(sed -n 's/^format=//p' "$ASSET_DIR/fifi-update.manifest")"
manifest_channel="$(sed -n 's/^channel=//p' "$ASSET_DIR/fifi-update.manifest")"
kernel_sha="$(sed -n 's/^kernel_sha256=//p' "$ASSET_DIR/fifi-update.manifest")"
initramfs_sha="$(sed -n 's/^initramfs_sha256=//p' "$ASSET_DIR/fifi-update.manifest")"
[[ "$manifest_format" == 1 && "$manifest_channel" == test &&
   "$kernel_sha" =~ ^[0-9a-fA-F]{64}$ &&
   "$initramfs_sha" =~ ^[0-9a-fA-F]{64}$ ]] || {
    echo "[publish-update] malformed or wrong-channel update manifest" >&2
    exit 1
}
[[ -s "$PUBLIC_KEY" ]] || {
    echo "[publish-update] missing release verification key: $PUBLIC_KEY" >&2
    exit 1
}
openssl pkeyutl -verify -pubin -inkey "$PUBLIC_KEY" -rawin \
    -in "$ASSET_DIR/fifi-update.manifest" \
    -sigfile "$ASSET_DIR/fifi-update.manifest.sig" >/dev/null 2>&1 || {
    echo "[publish-update] manifest signature verification failed" >&2
    exit 1
}
printf '%s  %s\n' "$kernel_sha" "$ASSET_DIR/bzImage" | sha256sum -c - >/dev/null || {
    echo "[publish-update] kernel hash does not match the signed manifest" >&2
    exit 1
}
printf '%s  %s\n' "$initramfs_sha" "$ASSET_DIR/initramfs.cpio.gz" | sha256sum -c - >/dev/null || {
    echo "[publish-update] initramfs hash does not match the signed manifest" >&2
    exit 1
}
gzip -t "$ASSET_DIR/initramfs.cpio.gz" || {
    echo "[publish-update] initramfs gzip validation failed" >&2
    exit 1
}

if [[ -s "$TEST_ISO" && -s "$ISO_RELEASE_INFO" ]]; then
    iso_format="$(sed -n 's/^format=//p' "$ISO_RELEASE_INFO")"
    iso_build="$(sed -n 's/^build=//p' "$ISO_RELEASE_INFO")"
    iso_kernel_sha="$(sed -n 's/^kernel_sha256=//p' "$ISO_RELEASE_INFO")"
    iso_initramfs_sha="$(sed -n 's/^initramfs_sha256=//p' "$ISO_RELEASE_INFO")"
    iso_sha="$(sed -n 's/^iso_sha256=//p' "$ISO_RELEASE_INFO")"
    [[ "$iso_format" == 1 && "$iso_build" == "$BUILD_ID" &&
       "$iso_kernel_sha" == "$kernel_sha" &&
       "$iso_initramfs_sha" == "$initramfs_sha" &&
       "$iso_sha" =~ ^[0-9a-fA-F]{64}$ ]] || {
        echo "[publish-update] optional test ISO does not match the signed build" >&2
        echo "Rebuild it with: make linux-test-usb" >&2
        exit 1
    }
    printf '%s  %s\n' "$iso_sha" "$TEST_ISO" | sha256sum -c - >/dev/null || {
        echo "[publish-update] optional test ISO hash does not match its provenance" >&2
        exit 1
    }
    ASSETS+=("$TEST_ISO")
    echo "[publish-update] verified optional test ISO for build $BUILD_ID"
elif [[ -e "$TEST_ISO" || -e "$ISO_RELEASE_INFO" ]]; then
    echo "[publish-update] ignoring optional ISO without matching release provenance"
    echo "[publish-update] rebuild it with: make linux-test-usb"
else
    echo "[publish-update] optional test ISO not present; publishing in-place update only"
fi

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

if ! "${GH[@]}" api --silent "repos/$REPO/commits/$BUILD_ID" >/dev/null 2>&1; then
    echo "[publish-update] commit $BUILD_ID is not available from $REPO" >&2
    echo "Push the reviewed linux-desktop commit before publishing its assets." >&2
    exit 1
fi

if [[ "$MODE" == check ]]; then
    echo "[publish-update] preflight passed for build $BUILD_ID"
    echo "[publish-update] no release was changed"
    exit 0
fi

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
