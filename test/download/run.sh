#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

APPS="$TMP/apps"
BIN="$TMP/bin"
FIXTURES="$TMP/fixtures"
mkdir -p "$APPS" "$BIN" "$FIXTURES"

cat > "$FIXTURES/Test.AppImage" <<'EOF'
#!/bin/sh
[ "${1:-}" = --appimage-extract ] && mkdir -p squashfs-root
EOF
chmod +x "$FIXTURES/Test.AppImage"
sha="$(sha256sum "$FIXTURES/Test.AppImage" | awk '{print $1}')"

cat > "$FIXTURES/github.json" <<EOF
{"assets":[{"name":"Test-x86_64.AppImage","digest":"sha256:$sha","browser_download_url":"https://github.test/Test-x86_64.AppImage"}]}
EOF
cat > "$FIXTURES/huggingface.json" <<EOF
{"siblings":[{"rfilename":"model.gguf","lfs":{"sha256":"$sha","size":70}}]}
EOF

cat > "$BIN/curl" <<'EOF'
#!/bin/sh
out=""
url=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) shift; out="$1" ;;
        --max-time|--retry|--retry-delay) shift ;;
        -*) ;;
        *) url="$1" ;;
    esac
    shift
done
case "$url" in
    *api.github.com*) src="$FIFI_TEST_FIXTURES/github.json" ;;
    *huggingface.co/api/models/*) src="$FIFI_TEST_FIXTURES/huggingface.json" ;;
    *github.test/*) src="$FIFI_TEST_FIXTURES/Test.AppImage" ;;
    *) echo "mock curl: unexpected URL: $url" >&2; exit 22 ;;
esac
if [ -n "$out" ]; then cp "$src" "$out"; else cat "$src"; fi
EOF
chmod +x "$BIN/curl"
cat > "$BIN/fifi-user-exec" <<'EOF'
#!/bin/sh
exec "$@"
EOF
chmod +x "$BIN/fifi-user-exec"

export PATH="$BIN:$PATH"
export FIFI_TEST_FIXTURES="$FIXTURES"
export FIFI_APPS_DIR="$APPS"
export FIFI_DESKTOP_CONF="$TMP/desktop.conf"
export FIFI_VERIFY_LIB="$ROOT/initramfs/root/usr/share/fifi/verified-download.sh"
export FIFI_USER_EXEC="$BIN/fifi-user-exec"

echo "[test-download] GitHub asset digest permits a matching AppImage"
sh "$ROOT/initramfs/root/usr/share/fifi/appstore-install.sh" owner/repo Test
cmp "$FIXTURES/Test.AppImage" "$APPS/Test.AppImage"
grep -Fxq "$sha" "$APPS/Test.sha256"
grep -Fq 'exec /usr/share/fifi/fifi-run "/fifi-data/apps/Test.AppImage"' \
    "$APPS/Test.sh"

echo "[test-download] a changed payload is rejected"
printf 'tampered\n' >> "$FIXTURES/Test.AppImage"
if sh "$ROOT/initramfs/root/usr/share/fifi/appstore-install.sh" owner/repo Tampered; then
    echo "tampered AppImage unexpectedly installed" >&2
    exit 1
fi
test ! -e "$APPS/Tampered.AppImage"

echo "[test-download] direct URLs without a digest are rejected"
if sh "$ROOT/initramfs/root/usr/share/fifi/appstore-install.sh" \
    url:https://example.test/Unknown.AppImage Unknown; then
    echo "unverified direct URL unexpectedly installed" >&2
    exit 1
fi

echo "[test-download] offline bundles require and verify sidecar hashes"
cp "$FIXTURES/Test.AppImage" "$FIXTURES/Offline.AppImage"
offline_sha="$(sha256sum "$FIXTURES/Offline.AppImage" | awk '{print $1}')"
printf '%s  Offline.AppImage\n' "$offline_sha" \
    > "$FIXTURES/Offline.AppImage.sha256"
sh "$ROOT/initramfs/root/usr/share/fifi/appstore-install.sh" \
    "file:$FIXTURES/Offline.AppImage" Offline
grep -Fxq "$offline_sha" "$APPS/Offline.sha256"

echo "[test-download] Hugging Face LFS SHA-256 is resolved"
. "$FIFI_VERIFY_LIB"
resolved="$(fifi_huggingface_lfs_sha256 \
    'https://huggingface.co/owner/repo/resolve/main/model.gguf')"
test "$resolved" = "$sha"

echo "[test-download] App Store catalog comes from the signed image"
catalog_apps="$TMP/catalog-apps"
FIFI_APPS_DIR="$catalog_apps" \
FIFI_APP_CATALOG="$ROOT/initramfs/root/usr/share/fifi/app-catalog.tsv" \
    sh "$ROOT/initramfs/root/usr/share/fifi/appstore-sync.sh"
cmp "$ROOT/initramfs/root/usr/share/fifi/app-catalog.tsv" \
    "$catalog_apps/catalog.tsv"
! grep -Fq 'appimage.github.io' \
    "$ROOT/initramfs/root/usr/share/fifi/appstore-sync.sh"
! awk -F '\t' '$3 ~ /^url:/ {bad=1} END {exit bad}' \
    "$ROOT/initramfs/root/usr/share/fifi/app-catalog.tsv"

echo "[test-download] PASS"
