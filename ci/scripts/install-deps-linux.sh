#!/usr/bin/env bash
# Installs the toolchain os24arm needs to build on a Debian/Ubuntu CI runner:
# a pinned clang/lld/llvm (matching the versioned names CMakeLists.txt
# searches for), cmake, and xorriso for the ISO target.
#
# Usage: install-deps-linux.sh
set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-17}"

sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}" "llvm-${LLVM_VERSION}" \
    cmake python3 xorriso

echo "==> Toolchain versions"
"clang-${LLVM_VERSION}" --version
"llvm-objcopy-${LLVM_VERSION}" --version
xorriso --version | head -1
cmake --version | head -1
