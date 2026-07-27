#!/bin/bash
# run_openswordigo.sh — Build and launch native OpenSwordigo engine
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
        *.scene)
            SCENE="$arg" ;;
    esac
done

echo "=========================================="
echo "  OpenSwordigo Native Engine Launcher"
echo "=========================================="

if [ $NO_BUILD -eq 0 ]; then
    if [ ! -d "OpenSwordigo/build" ]; then
        echo "=== Configuring CMake Build System ==="
        cmake -B OpenSwordigo/build -S OpenSwordigo
    fi

    if [ $CLEAN -eq 1 ]; then
        echo "=== Clean Rebuilding OpenSwordigo ==="
        cmake --build OpenSwordigo/build --target clean
    fi

    echo "=== Compiling OpenSwordigo Native Engine ==="
    cmake --build OpenSwordigo/build -j$(nproc)
    echo ""
fi

echo "=========================================="
echo "  Launching OpenSwordigo ($SCENE)"
echo "=========================================="
echo ""
exec ./OpenSwordigo/build/openswordigo_render "$SCENE"
