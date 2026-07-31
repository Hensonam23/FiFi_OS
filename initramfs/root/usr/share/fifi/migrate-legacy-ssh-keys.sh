#!/bin/sh
# Remove FiFi's historical auto-installed development key without disturbing
# authorization keys that the machine owner added.
set -u
set -f

DATA_ROOT="${FIFI_DATA_ROOT:-/fifi-data}"
SSH_DIR="$DATA_ROOT/ssh"
KEY_FILE="$SSH_DIR/authorized_keys"
BACKUP="$SSH_DIR/authorized_keys.before-ssh-hardening"
MARKER="$SSH_DIR/.legacy-key-migrated"
LEGACY_SHA256="${FIFI_LEGACY_KEY_SHA256:-c7cffe8fc69ce13b8558827fc2bc471b07e2e8f4e9e9c7695db2272d48680a74}"

mkdir -p "$SSH_DIR"
chmod 700 "$SSH_DIR"
[ -e "$MARKER" ] && exit 0

if [ ! -s "$KEY_FILE" ]; then
    : > "$MARKER"
    chmod 600 "$MARKER"
    exit 0
fi

tmp="$SSH_DIR/.authorized_keys.migrate.$$"
: > "$tmp"
removed=0

while IFS= read -r line || [ -n "$line" ]; do
    set -- $line
    if [ "$#" -ge 2 ]; then
        case "$1" in
            ssh-rsa|ssh-ed25519|ecdsa-sha2-nistp*)
                key_sha="$(
                    printf '%s %s\n' "$1" "$2" |
                        sha256sum | awk '{print $1}'
                )"
                if [ "$key_sha" = "$LEGACY_SHA256" ]; then
                    removed=$((removed + 1))
                    continue
                fi
                ;;
        esac
    fi
    printf '%s\n' "$line" >> "$tmp"
done < "$KEY_FILE"

if [ "$removed" -gt 0 ]; then
    [ -e "$BACKUP" ] || cp -p "$KEY_FILE" "$BACKUP"
    mv "$tmp" "$KEY_FILE"
    chmod 600 "$KEY_FILE" "$BACKUP"
    printf 'ssh: quarantined %s legacy development key(s); owner keys preserved\n' \
        "$removed" >> "$DATA_ROOT/network.log"
else
    rm -f "$tmp"
fi

: > "$MARKER"
chmod 600 "$MARKER"
