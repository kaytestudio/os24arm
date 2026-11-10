#!/usr/bin/env bash
# Configures and builds the Raspberry Pi 3+ os24arm kernel image
# (kernel8.img), via the CMake targets already defined in CMakeLists.txt.
#
# Usage: build-rpi.sh [build-dir]
# Env:   CC (default: clang) - the clang binary to build with; on a
#        Debian/Ubuntu CI runner set this to the versioned package name
#        (e.g. clang-17) installed by install-deps-linux.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build-rpi}"
CC="${CC:-clang}"

NPROC="$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)"

echo "==> Configuring (compiler: ${CC}, build dir: ${BUILD_DIR})"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DTARGET_PLATFORM=RPI3 \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_ASM_COMPILER="${CC}"

echo "==> Building kernel8.img"
cmake --build "${BUILD_DIR}" --target kernel8.img -j"${NPROC}"

echo ""
echo "Kernel image: ${BUILD_DIR}/kernel8.img"
echo ""
echo "Write it to an SD card's boot partition with:"
echo "  cp ${BUILD_DIR}/kernel8.img /Volumes/BOOT/"
