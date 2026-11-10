#!/usr/bin/env bash
# Configures and builds a BeagleBoard.org kernel image plus its uEnv.txt,
# or the same RV64 kernel for QEMU, via the CMake targets already defined
# in CMakeLists.txt:
#   BBB            -> os2warp-bbb.img            (BeagleBone Black, ARMv7)
#   BEAGLEV_AHEAD  -> os2warp-beaglev-ahead.img  (BeagleV-Ahead, RV64)
#   QEMU_RISCV     -> os2warp-qemu-riscv.img     (QEMU riscv64 virt; make run)
#
# Usage: build-beagle.sh <BBB|BEAGLEV_AHEAD|QEMU_RISCV> [build-dir]
# Env:   CC (default: clang) - the clang binary to build with; on a
#        Debian/Ubuntu CI runner set this to the versioned package name
#        (e.g. clang-17) installed by install-deps-linux.sh.
set -euo pipefail

PLATFORM="${1:?usage: build-beagle.sh <BBB|BEAGLEV_AHEAD|QEMU_RISCV> [build-dir]}"
case "${PLATFORM}" in
    BBB)           IMAGE=os2warp-bbb.img;           DEFAULT_DIR=build-bbb ;;
    BEAGLEV_AHEAD) IMAGE=os2warp-beaglev-ahead.img; DEFAULT_DIR=build-beaglev-ahead ;;
    QEMU_RISCV)    IMAGE=os2warp-qemu-riscv.img;    DEFAULT_DIR=build-qemu-riscv ;;
    *) echo "Unknown platform '${PLATFORM}' (expected BBB, BEAGLEV_AHEAD or QEMU_RISCV)" >&2; exit 1 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${2:-${REPO_ROOT}/${DEFAULT_DIR}}"
CC="${CC:-clang}"

NPROC="$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)"

echo "==> Configuring ${PLATFORM} (compiler: ${CC}, build dir: ${BUILD_DIR})"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DTARGET_PLATFORM="${PLATFORM}" \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_ASM_COMPILER="${CC}"

echo "==> Building ${IMAGE}"
cmake --build "${BUILD_DIR}" --target "${IMAGE}" -j"${NPROC}"

echo ""
echo "Kernel image: ${BUILD_DIR}/${IMAGE}"
if [ "${PLATFORM}" = QEMU_RISCV ]; then
    echo ""
    echo "Run it in QEMU (needs qemu-system-riscv64) with:"
    echo "  cmake --build ${BUILD_DIR} --target run"
else
    echo "Boot script:  ${BUILD_DIR}/uEnv.txt"
    echo ""
    echo "Copy both to the SD card's first (FAT) partition, e.g.:"
    echo "  cp ${BUILD_DIR}/${IMAGE} ${BUILD_DIR}/uEnv.txt /Volumes/BOOT/"
fi
