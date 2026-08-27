#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

APPS="$TMP/apps"
TRUST="$TMP/trust"
SYSTEM_TRUST="$TMP/system-trust"
DESKTOP="$TMP/desktop.conf"
PKG="$ROOT/initramfs/root/bin/fifi-pkg"
SDK="$ROOT/sdk/fifi-sdk"
mkdir -p "$APPS" "$TRUST" "$SYSTEM_TRUST"
touch "$DESKTOP"
cat > "$TMP/runner" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$FIFI_TEST_RUN_LOG"
EOF
chmod 0755 "$TMP/runner"
export FIFI_TEST_RUN_LOG="$TMP/run.log"

run_pkg() {
    FIFI_PKG_APPS_DIR="$APPS" \
    FIFI_PKG_TRUST_DIR="$TRUST" \
    FIFI_PKG_SYSTEM_TRUST_DIR="$SYSTEM_TRUST" \
    FIFI_DESKTOP_CONF="$DESKTOP" \
    FIFI_PKG_RUNNER="$TMP/runner" \
        "$PKG" "$@"
}

openssl genpkey -algorithm ED25519 -out "$TMP/publisher.pem"
openssl pkey -in "$TMP/publisher.pem" -pubout -out "$TMP/publisher.pub"
openssl genpkey -algorithm ED25519 -out "$TMP/forger.pem"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$TMP/rsa.pem" 2>/dev/null
openssl pkey -in "$TMP/rsa.pem" -pubout -out "$TMP/rsa.pub"

cat > "$TMP/hello" <<'EOF'
#!/bin/sh
echo hello-from-fifi-package
EOF
chmod 0755 "$TMP/hello"

"$SDK" pack "$TMP/hello" hello "Hello Native" 1.0.0 example \
    "$TMP/publisher.pem" "$TMP/hello-1.0.0.fifi"
if "$SDK" pack "$TMP/hello" wrong "Wrong Key" 1.0.0 example \
    "$TMP/rsa.pem" "$TMP/wrong.fifi" 2>/dev/null; then
    echo "SDK accepted a non-Ed25519 signing key" >&2
    exit 1
fi

if run_pkg verify "$TMP/hello-1.0.0.fifi" 2>/dev/null; then
    echo "untrusted publisher package was accepted" >&2
    exit 1
fi
echo "[package-test] unknown publishers are rejected"

run_pkg trust example "$TMP/publisher.pub" | grep -Fq 'trusted example'
if run_pkg trust wrong-kind "$TMP/rsa.pub" 2>/dev/null; then
    echo "non-Ed25519 trust key was accepted" >&2
    exit 1
fi
run_pkg verify "$TMP/hello-1.0.0.fifi" | grep -Fq 'verified hello 1.0.0 from example'
run_pkg install "$TMP/hello-1.0.0.fifi" | grep -Fq 'installed Hello Native 1.0.0'
test -x "$APPS/packages/hello/1.0.0/payload"
grep -Fq 'fifi-pkg run "hello"' "$APPS/pkg-hello.sh"
grep -Fq $'icon='"$APPS/pkg-hello.sh"$'\tHello Native' "$DESKTOP"
run_pkg list | grep -Fq $'hello\t1.0.0\texample\tHello Native'
run_pkg run hello argument
grep -Fq "$APPS/packages/hello/1.0.0/payload" "$TMP/run.log"
grep -Fxq argument "$TMP/run.log"
printf '\ntampered-after-install\n' >> "$APPS/packages/hello/1.0.0/payload"
if run_pkg run hello 2>/dev/null; then
    echo "tampered installed payload was launched" >&2
    exit 1
fi
run_pkg install "$TMP/hello-1.0.0.fifi" >/dev/null
echo "[package-test] signed native package installs without root"

mkdir "$TMP/tampered"
tar -xzf "$TMP/hello-1.0.0.fifi" -C "$TMP/tampered"
printf '\nchanged\n' >> "$TMP/tampered/payload"
tar -czf "$TMP/tampered.fifi" -C "$TMP/tampered" manifest manifest.sig payload
if run_pkg verify "$TMP/tampered.fifi" 2>/dev/null; then
    echo "tampered payload was accepted" >&2
    exit 1
fi

cp "$TMP/tampered/manifest" "$TMP/forged-manifest"
openssl pkeyutl -sign -rawin -inkey "$TMP/forger.pem" \
    -in "$TMP/forged-manifest" -out "$TMP/tampered/manifest.sig"
tar -czf "$TMP/forged.fifi" -C "$TMP/tampered" manifest manifest.sig payload
if run_pkg verify "$TMP/forged.fifi" 2>/dev/null; then
    echo "forged signature was accepted" >&2
    exit 1
fi

printf evil > "$TMP/tampered/extra"
tar -czf "$TMP/unexpected.fifi" -C "$TMP/tampered" manifest manifest.sig payload extra
if run_pkg verify "$TMP/unexpected.fifi" 2>/dev/null; then
    echo "unexpected archive member was accepted" >&2
    exit 1
fi
echo "[package-test] tampering, forgery, and extra members are rejected"

ln -s "$TMP/publisher.pub" "$TMP/key-link.pem"
if run_pkg trust linked "$TMP/key-link.pem" 2>/dev/null; then
    echo "symlinked trust key was accepted" >&2
    exit 1
fi

run_pkg remove hello | grep -Fq 'removed hello'
test ! -e "$APPS/packages/hello"
! grep -Fq 'pkg-hello.sh' "$DESKTOP"
echo "[package-test] removal cleans package and desktop registration"

rmdir "$APPS/packages"
mkdir "$TMP/storage-victim"
touch "$TMP/storage-victim/keep"
ln -s "$TMP/storage-victim" "$APPS/packages"
if run_pkg remove trapped 2>/dev/null; then
    echo "symlinked package storage was accepted" >&2
    exit 1
fi
test -f "$TMP/storage-victim/keep"
rm "$APPS/packages"
mkdir "$APPS/packages"
echo "[package-test] storage symlinks cannot redirect package removal"

"$SDK" new "$TMP/sample" sample "Sample App"
make -C "$TMP/sample"
test -x "$TMP/sample/sample"
echo "[package-test] SDK scaffold builds against the shipped headers"

sh -n "$PKG" "$SDK" "$ROOT/initramfs/root/bin/fifi"
grep -Fq 'native app SDK and signed package manager bundled' \
    "$ROOT/scripts/build-initramfs.sh"
grep -Fq 'exec fifi-pkg "$@"' "$ROOT/initramfs/root/bin/fifi"
grep -Fq '/fifi-data/app-trust' "$ROOT/initramfs/root/init"
grep -Fq '[ ! -L /fifi-data/app-trust ] || rm -f' "$ROOT/initramfs/root/init"
grep -Fq 'native_package' "$ROOT/fifi/apps/appstore/appstore.c"
echo "[package-test] PASS"
