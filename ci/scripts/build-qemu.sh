#!/usr/bin/env bash
# Configures and builds the QEMU-target os24arm kernel image
# (os2warp.img) and the bootable, dd-able ISO (os2warp-arm64.iso) that
# wraps it, via the CMake targets already defined in CMakeLists.txt.
#
# Usage: build-qemu.sh [build-dir]
# Env:   CC (default: clang) - the clang binary to build with; on a
#        Debian/Ubuntu CI runner set this to the versioned package name
#        (e.g. clang-17) installed by install-deps-linux.sh.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"
CC="${CC:-clang}"

NPROC="$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)"

echo "==> Configuring (compiler: ${CC}, build dir: ${BUILD_DIR})"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_ASM_COMPILER="${CC}"

echo "==> Building os2warp.img"
cmake --build "${BUILD_DIR}" --target os2warp.img -j"${NPROC}"

echo "==> Building os2warp-arm64.iso"
cmake --build "${BUILD_DIR}" --target iso -j"${NPROC}"

echo ""
echo "Kernel image: ${BUILD_DIR}/os2warp.img"
echo "ISO image:    ${BUILD_DIR}/os2warp-arm64.iso"
echo ""
echo "Write the ISO to a USB stick with:"
echo "  dd if=${BUILD_DIR}/os2warp-arm64.iso of=/dev/sdX bs=4M status=progress conv=fsync"
