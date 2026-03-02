#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUTDIR="initrd/rootfs"
mkdir -p "$OUTDIR"

CFLAGS=(--target=x86_64-elf -ffreestanding -nostdlib -static
        -Ikernel/include -Ikernel/arch/x86_64/idt)

LDFLAGS=(-Wl,-e,_start -Wl,-Ttext=0x401000)

MANIFEST="tools/user/manifest.txt"

echo "[user] building userland -> ${OUTDIR}"

if [ ! -f "$MANIFEST" ]; then
  echo "[user] ERROR: missing $MANIFEST" >&2
  exit 1
fi

fail=0

while IFS= read -r name; do
  # strip whitespace
  name="$(echo "$name" | sed 's/[[:space:]]//g')"
  [ -z "$name" ] && continue
  case "$name" in \#*) continue ;; esac

  src="tools/user/${name}.c"
  out="${OUTDIR}/${name}.elf"

  if [ ! -f "$src" ]; then
    echo "[user] ERROR: missing source $src (listed in manifest)" >&2
    fail=1
    break
  fi

  echo "  [user] $name -> $out"
  if ! clang "${CFLAGS[@]}" "${LDFLAGS[@]}" -o "$out" tools/user/crt0.S "$src"; then
    echo "  [user] FAILED building $name (terminal stays open)" >&2
    fail=1
    break
  fi
done < "$MANIFEST"

if [ $fail -ne 0 ]; then
  echo "[user] build failed. Fix the error above and rerun:" >&2
  echo "       bash tools/user/build_all.sh" >&2
  exit 1
fi

echo "[user] build OK"
