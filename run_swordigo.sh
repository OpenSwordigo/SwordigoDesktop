#!/bin/bash
# run_swordigo.sh — One-shot build, install, and run
# Usage: ./run_swordigo.sh [--clean] [--sre-only] [--no-build] [--no-dynarmic]

set -e
cd ~/SwordigoDesktop

CLEAN=0
SRE_ONLY=0
NO_BUILD=0
USE_DYNARMIC=1

for arg in "$@"; do
    case $arg in
        --clean)       CLEAN=1 ;;
        --sre-only)    SRE_ONLY=1 ;;
        --no-build)    NO_BUILD=1 ;;
        --no-dynarmic) USE_DYNARMIC=0 ;;
        --help)
            echo "Usage: ./run_swordigo.sh [--clean] [--sre-only] [--no-build] [--no-dynarmic]"
            echo "  --clean        Full clean rebuild"
            echo "  --sre-only     Only rebuild libsre.so"
            echo "  --no-build     Skip build, just install and run"
            echo "  --no-dynarmic  Use Unicorn interpreter instead of Dynarmic JIT"
            exit 0 ;;
    esac
done

ENGINE_DIR="$HOME/.local/share/swordigo-desktop/engine/v1.4.12/arm64-v8a"
DYNARMIC_LIB="deps/dynarmic/build/src/dynarmic/libdynarmic.a"

# ---- Build Dynarmic JIT (if needed) ----
if [ $USE_DYNARMIC -eq 1 ] && [ $NO_BUILD -eq 0 ]; then
    if [ ! -f "$DYNARMIC_LIB" ]; then
        echo "=== Building Dynarmic JIT from source (first time only) ==="
        make dynarmic-build
        echo ""
    fi
fi

# ---- Build ----
if [ $NO_BUILD -eq 0 ]; then
    if [ $CLEAN -eq 1 ]; then
        echo "=== CLEAN BUILD ==="
        make clean
    fi

    if [ $SRE_ONLY -eq 1 ]; then
        echo "=== Building libsre.so only ==="
        make libsre.so
    else
        if [ $USE_DYNARMIC -eq 1 ]; then
            echo "=== Building with Dynarmic JIT ==="
            make -j$(nproc) DYNARMIC=1
        else
            echo "=== Building with Unicorn ==="
            make -j$(nproc)
        fi
    fi
fi

# ---- Install libsre.so to ALL ARM64 engine directories ----
if [ -f libsre.so ]; then
    SRE_INSTALLED=0
    # Overwrite libsre.so inside all matching engine instance subdirectories
    while IFS= read -r dir; do
        if [ -d "$dir" ]; then
            cp libsre.so "$dir/libsre.so"
            echo "[OK] libsre.so -> $dir/"
            SRE_INSTALLED=$((SRE_INSTALLED + 1))
        fi
    done < <(find "$HOME/.local/share/swordigo-desktop/engine" -type d -name "arm64-v8a" 2>/dev/null)

    if [ $SRE_INSTALLED -eq 0 ]; then
        mkdir -p "$ENGINE_DIR"
        cp libsre.so "$ENGINE_DIR/libsre.so"
        echo "[OK] libsre.so -> $ENGINE_DIR/ (fallback)"
    else
        echo "[OK] libsre.so installed to $SRE_INSTALLED ARM64 engine dir(s)"
    fi
else
    echo "[WARN] libsre.so not found — skipping install"
fi

# ---- Install updated manifest.json ----
if [ -f engine/manifest.json ]; then
    MANIFEST_DIR="$HOME/.local/share/swordigo-desktop/engine"
    mkdir -p "$MANIFEST_DIR"
    cp engine/manifest.json "$MANIFEST_DIR/manifest.json"
    echo "[OK] manifest.json -> $MANIFEST_DIR/"
fi

# ---- Run ----
echo ""
echo "=========================================="
if [ $USE_DYNARMIC -eq 1 ]; then
    echo "  Launching Swordigo Desktop (Dynarmic JIT)"
else
    echo "  Launching Swordigo Desktop (Unicorn)"
fi
echo "=========================================="
echo ""
export LD_LIBRARY_PATH="$(dirname "$0")/bin/libs:$LD_LIBRARY_PATH"
exec ./bin/swordfare "$@"
