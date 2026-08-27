# FiFi Native App SDK and Packages

FiFi's native app SDK is the version-1 IPC, bitmap UI, and theme contract in
`fifi/shared/`. Every built-in native application uses these same headers. A
new project can be created on a development machine with:

```sh
sdk/fifi-sdk new my-app my-app "My App"
make -C my-app
```

Installed images expose the headers under `/usr/include/fifi` and the helper as
`fifi-sdk`. FiFi does not bundle a full C compiler in the release image, so
normal development and packaging happen on a workstation.

## Signed package format version 1

A `.fifi` package is a gzip tar archive containing exactly:

- `manifest`
- `manifest.sig`, an Ed25519 signature over the exact manifest bytes
- `payload`, the executable application
- optional `icon.png`, only when its digest is signed in the manifest

The manifest has one of each fixed field and permits no unknown fields:

```text
format=1
id=my-app
name=My App
version=1.0.0
arch=x86_64
publisher=publisher-id
payload_sha256=<64 lowercase or uppercase hexadecimal characters>
icon_sha256=none
```

Build and sign a package with a protected Ed25519 private key:

```sh
fifi-sdk pack ./my-app my-app "My App" 1.0.0 publisher-id \
  ./publisher-private.pem ./my-app-1.0.0.fifi
```

Set `FIFI_SDK_ICON=/path/icon.png` to include a signed PNG. Private signing keys
must never be copied into an application package, FiFi image, or source commit.

## Trust and installation

The user adds a publisher's public key explicitly, then verifies or installs:

```sh
fifi pkg trust publisher-id ./publisher-public.pem
fifi pkg verify ./my-app-1.0.0.fifi
fifi pkg install ./my-app-1.0.0.fifi
fifi pkg list
fifi pkg remove my-app
```

`fifi-pkg` runs as the desktop user. It rejects symlinked packages and trust
keys, unknown publishers, malformed or duplicate manifest fields, unexpected
archive members, oversized inputs, invalid signatures, payload/icon hash
mismatches, and incompatible architectures. It extracts only the four fixed
member names into bounded temporary files, then installs under
`/fifi-data/apps/packages/<id>/<version>/`. Launchers still cross the standard
`fifi-run` namespace and no-root boundary. The signature and installed payload
hash are checked again on every launch, so post-install tampering is refused.
