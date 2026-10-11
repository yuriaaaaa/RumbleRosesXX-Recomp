#!/usr/bin/env bash
# Host (linux-x64) build of the rexglue CLI + Rumble Roses XX codegen.
#
# Reads YOUR default.xex and writes rrxx/generated/default/*.cpp, the recompiled
# source the Android build consumes, then applies the port's edits to it
# (rrxx/tools/apply_code_patches.py: Rumble-XX patch switches and the host-paired
# guest setjmp/longjmp). Both the xex and the output are git-ignored: they are
# game code and never belong in the repository.
#
# Where the xex comes from (first match wins):
#   1. RRXX_XEX=/path/to/default.xex
#   2. rrxx/assets/default.xex (already in place)
#
# Inputs:
#   RRXX_SRC    (default: android/rrxx)
#   REXSDK_SRC  (default: android/rexglue-sdk)
#   SKIP_CLI_BUILD=1  reuse an existing build/host-codegen
#   FORCE_CODEGEN=1   regenerate even if generated/default exists
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RRXX_SRC="${RRXX_SRC:-$ROOT/rrxx}"
REXSDK_SRC="${REXSDK_SRC:-$ROOT/rexglue-sdk}"
BUILD_DIR="$ROOT/build/host-codegen"

# The build this port was made and tested with: Title ID 4B4E07D1, Media ID 1A6D4362,
# version 0.0.0.3 (5,124,096 bytes). rrxx/rrxx_functions.toml and the patch sites in
# rrxx/tools/apply_code_patches.py are addresses in this image.
EXPECTED_SHA256="${RRXX_XEX_SHA256:-04271f1e60b7a5e5be028caa6073f250003e82b055eabf83d65e5496f5699660}"

echo "== [1/4] default.xex =="
XEX_PATH="$RRXX_SRC/assets/default.xex"
mkdir -p "$RRXX_SRC/assets"
if [[ -n "${RRXX_XEX:-}" ]]; then
    cp "$RRXX_XEX" "$XEX_PATH"
fi
if [[ ! -f "$XEX_PATH" ]]; then
    echo "ERROR: no default.xex. Set RRXX_XEX=/path/to/default.xex (from your own copy of the game)." >&2
    exit 1
fi
ACTUAL_SHA256=$(sha256sum "$XEX_PATH" | cut -d' ' -f1)
echo "  sha256: $ACTUAL_SHA256"
if [[ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]]; then
    echo "WARNING: default.xex differs from the tested build (Title 4B4E07D1, Media 1A6D4362)." >&2
    echo "  rrxx_functions.toml and the patch sites target that image; codegen or the patches may fail." >&2
fi

echo "== [2/4] rexglue CLI (host) =="
HOST_ARCH_FLAGS="-march=x86-64-v2"  # same floor as the SDK's linux-base preset
if [[ "${SKIP_CLI_BUILD:-0}" != "1" || ! -d "$BUILD_DIR" ]]; then
    cmake -S "$REXSDK_SRC" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$HOST_ARCH_FLAGS" \
        -DCMAKE_CXX_FLAGS="$HOST_ARCH_FLAGS" \
        -DREXGLUE_BUILD_TESTS=OFF \
        -DREXGLUE_ENABLE_TRACY=OFF \
        -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
fi
cmake --build "$BUILD_DIR" --target rexglue --parallel "$(nproc)"

echo "== [3/4] Codegen =="
REXGLUE_CLI=$(find "$REXSDK_SRC/out" "$BUILD_DIR" -type f -name rexglue -perm -u+x 2>/dev/null | head -1)
if [[ -z "$REXGLUE_CLI" ]]; then
    echo "ERROR: rexglue binary not found after build" >&2
    exit 1
fi
echo "  CLI: $REXGLUE_CLI"
if [[ -f "$RRXX_SRC/generated/default/sources.cmake" && "${FORCE_CODEGEN:-0}" != "1" ]]; then
    echo "  codegen output present — skipping (FORCE_CODEGEN=1 to regenerate)"
else
    (cd "$RRXX_SRC" && "$REXGLUE_CLI" codegen rrxx_manifest.toml)
fi

echo "== [4/4] Port edits to the generated code =="
python3 "$RRXX_SRC/tools/apply_code_patches.py" "$RRXX_SRC/generated/default"
ls "$RRXX_SRC/generated/default/" | head -8
echo "Codegen done."
