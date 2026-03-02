#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Default: old behavior (build into initrd/rootfs) unless OUTDIR is set
OUTDIR="${OUTDIR:-initrd/rootfs}"
mkdir -p "$OUTDIR"

CFLAGS=(--target=x86_64-elf -ffreestanding -nostdlib -static
        -Ikernel/include -Ikernel/arch/x86_64/idt)
LDFLAGS=(-Wl,-e,_start -Wl,-Ttext=0x401000)

echo "[user] building userland -> $OUTDIR"

# Build in manifest order if present, otherwise build all *.c in tools/user/
MANIFEST="tools/user/manifest.txt"
LIST=()

if [ -f "$MANIFEST" ]; then
  while IFS= read -r name; do
    [ -z "$name" ] && continue
    LIST+=("tools/user/${name}.c")
  done < "$MANIFEST"
else
  for c in tools/user/*.c; do
    LIST+=("$c")
  done
fi

for c in "${LIST[@]}"; do
  base="$(basename "$c" .c)"
  out="${OUTDIR}/${base}.elf"
  echo "  [user] $base -> $out"

  if ! clang "${CFLAGS[@]}" "${LDFLAGS[@]}" -o "$out" tools/user/crt0.S "$c"; then
    echo "  [user] FAILED building $base" >&2
    echo "[user] build failed — fix the error above and rerun:" >&2
    echo "       OUTDIR=$OUTDIR bash tools/user/build_all.sh" >&2
    exit 1
  fi
done

echo "[user] build OK"
