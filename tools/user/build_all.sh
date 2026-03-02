#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUTDIR="initrd/rootfs"
mkdir -p "$OUTDIR"

CFLAGS=(--target=x86_64-elf -ffreestanding -nostdlib -static
        -Ikernel/include -Ikernel/arch/x86_64/idt)

LDFLAGS=(-Wl,-e,_start -Wl,-Ttext=0x401000)

echo "[user] building tools/user/*.c -> ${OUTDIR}/*.elf"
fail=0

for c in tools/user/*.c; do
  base="$(basename "$c" .c)"
  out="${OUTDIR}/${base}.elf"
  echo "  [user] $base -> $out"

  if ! clang "${CFLAGS[@]}" "${LDFLAGS[@]}" -o "$out" tools/user/crt0.S "$c"; then
    echo "  [user] FAILED building $base" >&2
    fail=1
    break
  fi
done

if [ $fail -ne 0 ]; then
  echo "[user] build failed (no terminal exit). Fix the error above and rerun:" >&2
  echo "       bash tools/user/build_all.sh" >&2
  exit 1
fi

echo "[user] build OK"
