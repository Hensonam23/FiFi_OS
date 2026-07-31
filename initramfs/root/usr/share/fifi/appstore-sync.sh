#!/bin/sh
# Install the curated App Store catalog embedded in the signed FiFi OS image.
# App binaries are still resolved from their upstream projects, but are accepted
# only when the hosting provider supplies a matching SHA-256 digest.
set -e

APPS="${FIFI_APPS_DIR:-/fifi-data/apps}"
CATALOG="${FIFI_APP_CATALOG:-/usr/share/fifi/app-catalog.tsv}"
mkdir -p "$APPS"
[ -s "$CATALOG" ] || {
    echo "appstore-sync: signed catalog is missing" >&2
    exit 1
}
cp "$CATALOG" "$APPS/catalog.tsv.new"
chmod 0644 "$APPS/catalog.tsv.new"
mv "$APPS/catalog.tsv.new" "$APPS/catalog.tsv"
echo "appstore-sync: $(wc -l < "$APPS/catalog.tsv") verified apps"
