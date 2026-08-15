#!/bin/bash
set -e
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build-cmake"
CLEAN=0
SRE_ONLY=0
BUILD=1
USE_DYNARMIC=1
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --sre-only) SRE_ONLY=1 ;;
        --build) BUILD=1 ;;
        --no-build) BUILD=0 ;;
        --no-dynarmic) USE_DYNARMIC=0 ;;
        --help) printf '%s\n' 'Usage: ./run_swordigo.sh [--build] [--clean] [--sre-only] [--no-build] [--no-dynarmic]' '  --build        Incremental build before launching (default)' '  --clean        Delete build-cmake and rebuild everything' '  --sre-only     Incrementally build only libsre.so' '  --no-build     Skip the incremental build' '  --no-dynarmic  Use Unicorn instead of Dynarmic'; exit 0 ;;
        *) ARGS+=("$arg") ;;
    esac
done
if [ "$CLEAN" -eq 1 ] || [ "$SRE_ONLY" -eq 1 ]; then BUILD=1; fi
if [ "$CLEAN" -eq 1 ]; then rm -rf "$BUILD_DIR"; fi
if [ "$BUILD" -eq 1 ]; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DSWORDIGO_USE_DYNARMIC="$USE_DYNARMIC" -DSWORDIGO_BUILD_SRE=ON
    if [ "$SRE_ONLY" -eq 1 ]; then
        cmake --build "$BUILD_DIR" --target sre -j "$(nproc)"
    else
        if [ "$USE_DYNARMIC" -eq 1 ] && [ ! -f "$ROOT_DIR/deps/dynarmic/build/src/dynarmic/libdynarmic.a" ]; then
            cmake --build "$BUILD_DIR" --target dynarmic-build -j "$(nproc)"
        fi
            cmake --build "$BUILD_DIR" --target sre -j "$(nproc)"
        cmake --build "$BUILD_DIR" -j "$(nproc)"
    fi
fi
# Products are built straight into bin/ + bin/libs/ — no install step needed.
if [ ! -x "$ROOT_DIR/bin/swordfare" ]; then
    echo "Swordfare is not installed. Run: ./run_swordigo.sh --build" >&2
    exit 1
fi
if [ -f "$ROOT_DIR/engine/manifest.json" ]; then mkdir -p "$HOME/.local/share/swordigo-desktop/engine"; cp "$ROOT_DIR/engine/manifest.json" "$HOME/.local/share/swordigo-desktop/engine/manifest.json"; fi
export LD_LIBRARY_PATH="$ROOT_DIR/bin/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$ROOT_DIR/bin/swordfare" "${ARGS[@]}"
