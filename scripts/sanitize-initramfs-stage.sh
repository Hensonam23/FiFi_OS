#!/usr/bin/env bash
# Prevent developer SSH credentials from entering a FiFi OS image.
set -euo pipefail

if [[ "$#" -ne 1 || ! -d "$1" ]]; then
    echo "usage: $0 INITRAMFS_STAGE" >&2
    exit 2
fi

stage="$1"

# This was the historical baked-key location. Remove it even when an ignored
# local copy exists in the developer's checkout.
rm -f "$stage/usr/share/fifi/authorized_keys"

credential=""
while IFS= read -r candidate; do
    credential="$candidate"
    break
done < <(
    find "$stage" -type f \
        \( -name authorized_keys -o -name 'id_rsa' -o -name 'id_ecdsa' \
           -o -name 'id_ed25519' -o -name '*.pub' \) \
        ! -path "$stage/etc/fifi-release-signing.pub" -print
)

if [[ -z "$credential" ]]; then
    credential="$(
        grep -IRIlE \
            '(^|[[:space:]])(ssh-rsa|ssh-ed25519|ecdsa-sha2-nistp[0-9]+)[[:space:]]|-----BEGIN (OPENSSH |RSA |EC )?PRIVATE KEY-----' \
            "$stage" 2>/dev/null | head -n 1 || true
    )"
fi

if [[ -n "$credential" ]]; then
    echo "[initramfs] ERROR: SSH credential would be included: ${credential#"$stage"/}" >&2
    exit 1
fi

echo "[initramfs] verified: no baked SSH credentials"
