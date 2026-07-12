#!/bin/sh
# build-llama.sh — build the llama.cpp CPU runtime for the FiFi OS image.
#
# Produces build-linux/llama/llama-cli (and llama-server), which build-initramfs.sh
# bundles so the installed local AI model actually runs. Built with GGML_NATIVE=OFF
# so the binary is portable across x86-64 targets (it does not bake in the build
# machine's -march=native). Run once; re-run to update llama.cpp.
#
# Usage: scripts/build-llama.sh
set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$REPO_ROOT/build-linux/llama"
SRC="${LLAMA_SRC:-/tmp/llamacpp}"
REPO="${LLAMA_REPO:-https://github.com/ggml-org/llama.cpp}"

command -v cmake >/dev/null 2>&1 || { echo "cmake is required"; exit 1; }
command -v git   >/dev/null 2>&1 || { echo "git is required";   exit 1; }

if [ ! -d "$SRC/.git" ]; then
    echo "[llama] cloning $REPO -> $SRC"
    git clone --depth 1 "$REPO" "$SRC"
else
    echo "[llama] updating $SRC"; ( cd "$SRC" && git pull --ff-only || true )
fi

echo "[llama] configuring (CPU, AVX2 baseline, static libs)"
# GGML_NATIVE=OFF so we do NOT bake in the build machine's -march=native (the dev
# box is Zen5 w/ AVX-512; the target i9-14900HX Raptor Lake has NO AVX-512, so a
# native build would SIGILL there). Instead enable AVX2/FMA/F16C explicitly: that
# ISA level is present on essentially every x86-64 CPU since ~2013 (Haswell/Zen1),
# and it makes llama.cpp CPU inference ~50-100x faster than the SSE2 baseline the
# portable build was falling back to (measured: <0.3 tok/s -> should be 10-20+).
cmake -S "$SRC" -B "$SRC/build" \
    -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON \
    -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON

echo "[llama] building llama-cli + llama-server"
cmake --build "$SRC/build" -j"$(nproc)" --target llama-cli llama-server

mkdir -p "$OUT"
cp "$SRC/build/bin/llama-cli"    "$OUT/llama-cli"
cp "$SRC/build/bin/llama-server" "$OUT/llama-server" 2>/dev/null || true
echo "[llama] done -> $OUT"
ls -la "$OUT"
