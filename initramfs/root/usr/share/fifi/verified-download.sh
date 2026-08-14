#!/bin/sh
# Shared download-integrity helpers for AppImages and other executable content.

fifi_verify_sha256() {
    _fifi_file="$1"
    _fifi_expected="$2"
    printf '%s\n' "$_fifi_expected" | grep -Eq '^[0-9a-fA-F]{64}$' || return 1
    _fifi_actual="$(sha256sum "$_fifi_file" | awk '{print $1}')"
    [ "$_fifi_actual" = "$_fifi_expected" ]
}

# Print URL|SHA256 pairs for AppImage assets in GitHub release JSON. GitHub
# computes the digest server-side and exposes it in the release-asset object.
fifi_github_appimage_pairs() {
    grep -oE '"(name|digest|browser_download_url)"[[:space:]]*:[[:space:]]*"[^"]*"' |
        awk '
        /^"name"/ {
            value=$0
            sub(/^"name"[[:space:]]*:[[:space:]]*"/, "", value)
            sub(/"$/, "", value)
            app=(value ~ /\.AppImage$/)
            digest=""
            next
        }
        app && /^"digest"/ {
            value=$0
            sub(/^"digest"[[:space:]]*:[[:space:]]*"sha256:/, "", value)
            sub(/"$/, "", value)
            if (value ~ /^[0-9a-fA-F]{64}$/) digest=value
            next
        }
        app && /^"browser_download_url"/ {
            value=$0
            sub(/^"browser_download_url"[[:space:]]*:[[:space:]]*"/, "", value)
            sub(/"$/, "", value)
            if (digest != "") print value "|" digest
            app=0
        }'
}

fifi_pick_x86_64_pair() {
    _fifi_pairs="$1"
    _fifi_pair="$(printf '%s\n' "$_fifi_pairs" |
        grep -iE 'x86_64|amd64' | head -1)"
    [ -n "$_fifi_pair" ] || _fifi_pair="$(printf '%s\n' "$_fifi_pairs" |
        grep -viE 'arm|aarch|i386|i686' | head -1)"
    printf '%s' "$_fifi_pair"
}

# Resolve the newest x86-64 AppImage published by a Codeberg repository.  Forgejo
# release assets do not expose a server-computed digest in the JSON response, so
# require the publisher's adjacent .sha256sum asset and validate its contents.
fifi_codeberg_appimage_pair() {
    _fifi_repo="$1"
    _fifi_release="$(curl -fsSL --max-time 30 \
        "https://codeberg.org/api/v1/repos/${_fifi_repo}/releases?limit=5")" ||
        return 1
    _fifi_urls="$(printf '%s' "$_fifi_release" |
        grep -oE '"browser_download_url":"[^"]+"' |
        sed 's/^"browser_download_url":"//;s/"$//')"
    _fifi_url="$(printf '%s\n' "$_fifi_urls" |
        grep -iE 'x86_64.*appimage\.AppImage$' | head -1)"
    [ -n "$_fifi_url" ] || return 1
    _fifi_sum_url="$(printf '%s\n' "$_fifi_urls" |
        grep -F "${_fifi_url}.sha256sum" | head -1)"
    [ -n "$_fifi_sum_url" ] || return 1
    _fifi_sha="$(curl -fsSL --max-time 30 "$_fifi_sum_url" |
        awk 'NF {print $1; exit}')" || return 1
    printf '%s\n' "$_fifi_sha" | grep -Eq '^[0-9a-fA-F]{64}$' || return 1
    printf '%s|%s' "$_fifi_url" "$_fifi_sha"
}

# Resolve a GitLab generic-package URL to the SHA-256 recorded by GitLab.
fifi_gitlab_package_sha256() {
    _fifi_project="$1"
    _fifi_url="$2"
    _fifi_rest="${_fifi_url#*/packages/generic/}"
    [ "$_fifi_rest" != "$_fifi_url" ] || return 1
    _fifi_package="${_fifi_rest%%/*}"
    _fifi_rest="${_fifi_rest#*/}"
    _fifi_version="${_fifi_rest%%/*}"
    _fifi_file="${_fifi_rest#*/}"
    [ -n "$_fifi_package" ] && [ -n "$_fifi_version" ] &&
        [ -n "$_fifi_file" ] || return 1

    _fifi_packages="$(curl -fsSL --max-time 30 \
        "https://gitlab.com/api/v4/projects/${_fifi_project}/packages?package_type=generic&package_name=${_fifi_package}&package_version=${_fifi_version}&per_page=5")" ||
        return 1
    _fifi_package_id="$(printf '%s' "$_fifi_packages" |
        grep -oE '"id":[0-9]+' | head -1 | cut -d: -f2)"
    [ -n "$_fifi_package_id" ] || return 1

    curl -fsSL --max-time 30 \
        "https://gitlab.com/api/v4/projects/${_fifi_project}/packages/${_fifi_package_id}/package_files" |
        sed 's/},{/}\n{/g' |
        grep -F "\"file_name\":\"$_fifi_file\"" |
        head -1 |
        grep -oE '"file_sha256":"[0-9a-fA-F]{64}"' |
        sed 's/.*:"//;s/"$//'
}

# Hugging Face stores GGUF files in Git LFS, whose object ID is the file's
# SHA-256. Resolve that ID from the model metadata before downloading.
fifi_huggingface_lfs_sha256() {
    _fifi_url="$1"
    _fifi_rest="${_fifi_url#https://huggingface.co/}"
    [ "$_fifi_rest" != "$_fifi_url" ] || return 1
    _fifi_owner="${_fifi_rest%%/*}"
    _fifi_rest="${_fifi_rest#*/}"
    _fifi_repo="${_fifi_rest%%/*}"
    _fifi_rest="${_fifi_rest#*/}"
    [ "${_fifi_rest%%/*}" = resolve ] || return 1
    _fifi_rest="${_fifi_rest#*/}"
    _fifi_rest="${_fifi_rest#*/}"
    _fifi_file="$_fifi_rest"
    [ -n "$_fifi_owner" ] && [ -n "$_fifi_repo" ] &&
        [ -n "$_fifi_file" ] || return 1

    curl -fsSL --max-time 30 \
        "https://huggingface.co/api/models/${_fifi_owner}/${_fifi_repo}?blobs=true" |
        sed 's/},{/}\n{/g' |
        grep -F "\"rfilename\":\"$_fifi_file\"" |
        head -1 |
        grep -oE '"sha256":"[0-9a-fA-F]{64}"' |
        sed 's/.*:"//;s/"$//'
}

fifi_download_verified() {
    _fifi_url="$1"
    _fifi_sha="$2"
    _fifi_dest="$3"
    curl -fL --retry 3 --retry-delay 2 --max-time 1800 \
        -o "$_fifi_dest.part" "$_fifi_url" || {
        rm -f "$_fifi_dest.part"
        return 1
    }
    if ! fifi_verify_sha256 "$_fifi_dest.part" "$_fifi_sha"; then
        echo "download SHA-256 mismatch: $_fifi_url" >&2
        rm -f "$_fifi_dest.part"
        return 1
    fi
    mv "$_fifi_dest.part" "$_fifi_dest"
}
