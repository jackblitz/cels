#!/usr/bin/env bash
set -e

echo "======================================================================"
echo "  [CELS] Building Application Shared Library (Linux / macOS)"
echo "======================================================================"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-}"
if [ -z "$BUILD_DIR" ]; then
    if [ -f "${ROOT_DIR}/cmake-build-debug/build.ninja" ] || [ -d "${ROOT_DIR}/cmake-build-debug" ]; then
        BUILD_DIR="${ROOT_DIR}/cmake-build-debug"
    elif [ -d "${ROOT_DIR}/build" ]; then
        BUILD_DIR="${ROOT_DIR}/build"
    else
        BUILD_DIR="${ROOT_DIR}/cmake-build-debug"
    fi
fi

CMAKE_BIN="cmake"
if ! command -v cmake &> /dev/null; then
    if [ -x "/Applications/CLion.app/Contents/bin/cmake/mac/x64/bin/cmake" ]; then
        CMAKE_BIN="/Applications/CLion.app/Contents/bin/cmake/mac/x64/bin/cmake"
    elif [ -x "/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake" ]; then
        CMAKE_BIN="/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin/cmake"
    fi
fi

echo "[CELS] Using CMake : ${CMAKE_BIN}"
echo "[CELS] Build Dir   : ${BUILD_DIR}"
echo "[CELS] Target      : cel_app"
echo ""

"${CMAKE_BIN}" --build "${BUILD_DIR}" --target cel_app

echo ""
echo "======================================================================"
echo "  [CELS] Application library successfully updated!"
echo "======================================================================"
