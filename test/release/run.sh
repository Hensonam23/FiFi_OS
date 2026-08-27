#!/usr/bin/env bash
# Verify that test-release publication rejects unsafe inputs and that its
# preflight mode never changes GitHub state.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[test-release] CI embeds the exact workflow commit in release images"
grep -Fq 'FIFI_BUILD_ID: ${{ github.sha }}' \
    "$ROOT/.github/workflows/linux-desktop.yml"
grep -Fq 'FIFI_BUILD_ID="${FIFI_BUILD_ID:-${GITHUB_SHA:-}}"' \
    "$ROOT/scripts/build-initramfs.sh"
grep -Fq 'git -C "$REPO_ROOT" rev-parse --short=12 HEAD' \
    "$ROOT/scripts/build-initramfs.sh"

REPO="$TMP/repo"
MOCK_BIN="$TMP/bin"
LOG="$TMP/gh.log"
mkdir -p "$REPO/scripts" "$REPO/security" "$REPO/build-linux/update-test" \
    "$MOCK_BIN"
cp "$ROOT/scripts/publish-test-update.sh" "$REPO/scripts/"
printf '/build-linux/\n' > "$REPO/.gitignore"

openssl genpkey -algorithm ED25519 \
    -out "$TMP/signing-key.pem" >/dev/null 2>&1
openssl pkey -in "$TMP/signing-key.pem" -pubout \
    -out "$REPO/security/release-signing-public.pem" >/dev/null 2>&1

git -C "$REPO" init -q
git -C "$REPO" config user.name 'FiFi release test'
git -C "$REPO" config user.email 'release-test@invalid'
printf 'release test\n' > "$REPO/README"
git -C "$REPO" add .gitignore README scripts/publish-test-update.sh \
    security/release-signing-public.pem
git -C "$REPO" commit -qm 'Create release test fixture'
BUILD_ID="$(git -C "$REPO" rev-parse HEAD)"

printf 'kernel fixture\n' > "$REPO/build-linux/update-test/bzImage"
printf 'initramfs fixture\n' | gzip -c > \
    "$REPO/build-linux/update-test/initramfs.cpio.gz"
kernel_sha="$(sha256sum "$REPO/build-linux/update-test/bzImage" | awk '{print $1}')"
initramfs_sha="$(sha256sum "$REPO/build-linux/update-test/initramfs.cpio.gz" | awk '{print $1}')"
cat > "$REPO/build-linux/update-test/fifi-update.manifest" <<EOF
format=1
channel=test
build=$BUILD_ID
kernel_sha256=$kernel_sha
initramfs_sha256=$initramfs_sha
EOF
openssl pkeyutl -sign -rawin -inkey "$TMP/signing-key.pem" \
    -in "$REPO/build-linux/update-test/fifi-update.manifest" \
    -out "$REPO/build-linux/update-test/fifi-update.manifest.sig"

cat > "$MOCK_BIN/gh" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$FIFI_TEST_GH_LOG"
case "$1 $2" in
    'auth status') exit 0 ;;
    'api --silent')
        [ "${FIFI_TEST_REMOTE_COMMIT:-present}" = present ]
        exit
        ;;
    'release view') exit 1 ;;
    'release create') exit 0 ;;
    *) exit 1 ;;
esac
EOF
chmod +x "$MOCK_BIN/gh"

run_publisher() {
    FIFI_TEST_GH_LOG="$LOG" PATH="$MOCK_BIN:$PATH" \
        bash "$REPO/scripts/publish-test-update.sh" "$@"
}

echo "[test-release] preflight verifies without publishing"
: > "$LOG"
printf 'old unverified ISO\n' > "$REPO/build-linux/fifi-linux.iso"
run_publisher --check > "$TMP/check.out"
grep -Fq 'preflight passed' "$TMP/check.out"
grep -Fq 'no release was changed' "$TMP/check.out"
grep -Fq 'ignoring optional ISO without matching release provenance' "$TMP/check.out"
grep -Fq "api --silent repos/Hensonam23/FiFi_OS/commits/$BUILD_ID" "$LOG"
! grep -Fq 'release ' "$LOG"

echo "[test-release] unpublished commits are rejected"
: > "$LOG"
if FIFI_TEST_REMOTE_COMMIT=missing run_publisher --check \
    > "$TMP/missing.out" 2>&1; then
    echo "publisher accepted a commit that is absent from GitHub" >&2
    exit 1
fi
grep -Fq 'is not available from Hensonam23/FiFi_OS' "$TMP/missing.out"
! grep -Fq 'release ' "$LOG"

echo "[test-release] tampered artifacts are rejected before GitHub access"
printf 'tampered\n' >> "$REPO/build-linux/update-test/bzImage"
: > "$LOG"
if run_publisher --check > "$TMP/tampered.out" 2>&1; then
    echo "publisher accepted an artifact that did not match the manifest" >&2
    exit 1
fi
grep -Fq 'kernel hash does not match' "$TMP/tampered.out"
test ! -s "$LOG"

echo "[test-release] matching ISO provenance permits an intentional upload"
git -C "$REPO" checkout -q -- build-linux/update-test/bzImage 2>/dev/null || \
    printf 'kernel fixture\n' > "$REPO/build-linux/update-test/bzImage"
iso_sha="$(sha256sum "$REPO/build-linux/fifi-linux.iso" | awk '{print $1}')"
cat > "$REPO/build-linux/fifi-linux.iso.release-info" <<EOF
format=1
build=$BUILD_ID
kernel_sha256=$kernel_sha
initramfs_sha256=$initramfs_sha
iso_sha256=$iso_sha
EOF
: > "$LOG"
run_publisher > "$TMP/publish.out"
grep -Fq 'release create linux-desktop-test' "$LOG"
grep -Fq "$REPO/build-linux/fifi-linux.iso" "$LOG"

echo "[test-release] PASS"
