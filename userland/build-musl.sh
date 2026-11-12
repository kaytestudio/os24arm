#!/bin/bash
# userland/build-musl.sh - build a static musl sysroot for aarch64-linux-musl
#
# Downloads musl (pinned version, SHA-256 checked), builds it with the same
# LLVM clang the kernel uses, and installs headers + libc.a + crt files to
# build/userland/sysroot. Programs for the kernel's Linux-ABI user mode
# (see userland/README.md) are then built with:
#
#   clang --target=aarch64-linux-musl --sysroot=build/userland/sysroot \
#         -static -fuse-ld=lld ...
#
# Also builds compiler-rt's builtins (libclang_rt.builtins-aarch64.a) from
# the matching LLVM release, which static aarch64 programs link against.
#
# Usage: userland/build-musl.sh [--force]
set -euo pipefail

MUSL_VERSION=1.2.5
MUSL_SHA256=a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
# compiler-rt builtins (long double soft-float etc.), from the LLVM release
# matching the kernel's clang; only lib/builtins is fetched (sparse clone).
LLVM_TAG=llvmorg-23.1.5
LLVM_COMMIT=392dfff97d29b59d77e5cac6d4b6ef2585fe7117

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build/userland"
SYSROOT="$WORK/sysroot"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
CC="$LLVM/clang"

if [ -f "$SYSROOT/lib/libc.a" ] && [ -f "$SYSROOT/lib/libclang_rt.builtins-aarch64.a" ] &&
   [ "${1:-}" != "--force" ]; then
    echo "musl sysroot already built: $SYSROOT (use --force to rebuild)"
    exit 0
fi
command -v "$CC" >/dev/null || { echo "clang not found at $CC" >&2; exit 1; }
LLD="$(command -v ld.lld || true)"
[ -n "$LLD" ] || { echo "ld.lld not found on PATH" >&2; exit 1; }

mkdir -p "$WORK"
cd "$WORK"
TARBALL="musl-${MUSL_VERSION}.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "Downloading $MUSL_URL"
    curl -sSfL -o "$TARBALL.part" "$MUSL_URL"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$MUSL_SHA256  $TARBALL" | shasum -a 256 -c - >/dev/null \
    || { echo "SHA-256 mismatch for $TARBALL - refusing to build" >&2; rm -f "$TARBALL"; exit 1; }

rm -rf "musl-${MUSL_VERSION}"
tar xzf "$TARBALL"
cd "musl-${MUSL_VERSION}"

# Static only: the kernel's loader runs ET_EXEC binaries, no dynamic linker.
CC="$CC --target=aarch64-linux-musl" \
AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" \
CFLAGS="-O2 -fno-stack-protector -mno-outline-atomics" \
LDFLAGS="-fuse-ld=lld" \
    ./configure --target=aarch64-linux-musl --prefix="$SYSROOT" \
                --disable-shared --enable-static >/dev/null
make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
make install >/dev/null

# ── compiler-rt builtins ────────────────────────────────────────────────
cd "$WORK"
if [ ! -d llvm-project/compiler-rt/lib/builtins ]; then
    rm -rf llvm-project
    git clone -q --depth 1 --filter=blob:none --sparse --branch "$LLVM_TAG" \
        https://github.com/llvm/llvm-project.git llvm-project
    git -C llvm-project sparse-checkout set compiler-rt/lib/builtins
fi
[ "$(git -C llvm-project rev-parse HEAD)" = "$LLVM_COMMIT" ] \
    || { echo "llvm-project is not at $LLVM_COMMIT ($LLVM_TAG) - refusing to build" >&2; exit 1; }
B="$WORK/builtins"
rm -rf "$B"; mkdir -p "$B"
ok=0; skipped=0
for f in llvm-project/compiler-rt/lib/builtins/*.c; do
    o="$B/$(basename "${f%.c}").o"
    # Portable C builtins; files that need an OS service, an unwinder or
    # another architecture fail to build here and aren't needed.
    if "$CC" --target=aarch64-linux-musl -O2 -fno-builtin -mno-outline-atomics \
            -nostdinc -isystem "$SYSROOT/include" \
            -isystem "$("$CC" -print-resource-dir)/include" \
            -c "$f" -o "$o" 2>/dev/null; then ok=$((ok + 1)); else skipped=$((skipped + 1)); fi
done
rm -f "$SYSROOT/lib/libclang_rt.builtins-aarch64.a"
"$LLVM/llvm-ar" rcs "$SYSROOT/lib/libclang_rt.builtins-aarch64.a" "$B"/*.o
echo "compiler-rt builtins: $ok built, $skipped not applicable"

echo "musl ${MUSL_VERSION} sysroot: $SYSROOT"
ls "$SYSROOT/lib" | tr '\n' ' '; echo
