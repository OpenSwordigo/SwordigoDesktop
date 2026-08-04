#!/bin/bash
# run_openswordigo.sh — Build and launch OpenSwordigo through Swordfare
# Usage: ./run_openswordigo.sh [scene_name.scene] [--clean] [--no-build]

set -e
cd "$(dirname "$0")"

CLEAN=0
NO_BUILD=0
SCENE="menu.scene"

for arg in "$@"; do
    case $arg in
        --clean)    CLEAN=1 ;;
        --no-build) NO_BUILD=1 ;;
        --help)
            echo "Usage: ./run_openswordigo.sh [scene_name.scene] [--clean] [--no-build]"
            echo "  --clean     Perform a clean rebuild"
            echo "  --no-build  Skip build and run directly"
            exit 0 ;;
        *.scene|*.POD|*.pod|*hiro*|*anim*)
            SCENE="$arg" ;;
    esac
done

echo "=========================================="
echo "  OpenSwordigo Native Engine Launcher"
echo "=========================================="

if [ $NO_BUILD -eq 0 ]; then
    if [ $CLEAN -eq 1 ]; then
        echo "=== Cleaning OpenSwordigo Build ==="
        rm -rf OpenSwordigo/build
    fi

    echo "=== Building Swordfare Host and OpenSwordigo Plugin ==="
    make -j"$(nproc)" bin/swordfare openswordigo
    echo ""
fi

echo "=========================================="
echo "  Launching OpenSwordigo ($SCENE)"
echo "=========================================="
echo ""
exec ./bin/swordfare --openswordigo --openswordigo-scene "$SCENE" \
    --openswordigo-lib ./bin/libs/libopenswordigo.so
