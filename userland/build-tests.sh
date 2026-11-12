#!/bin/bash
# userland/build-tests.sh - build userland/tests/*.c as static AArch64 Linux
# executables for the kernel's RUNELF (see userland/README.md).
#
# Linked at 0x60000000, inside the kernel's user window (kernel/include/mmu.h).
# Output: build/userland/tests/<name> (no extension).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SR="$ROOT/build/userland/sysroot"
OUT="$ROOT/build/userland/tests"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
[ -f "$SR/lib/libc.a" ] || "$ROOT/userland/build-musl.sh"
mkdir -p "$OUT"
CFLAGS=(--target=aarch64-linux-musl -O2 -Wall -mno-outline-atomics -fno-pie
        -nostdinc -isystem "$SR/include" -isystem "$("$LLVM/clang" -print-resource-dir)/include")
CC_WRAP="$ROOT/build/userland/toolchain/aarch64-linux-musl-cc"
for src in "$ROOT"/userland/tests/*.c; do
    name="$(basename "${src%.c}")"
    # " * pkg: a b c" in a test: link against those libraries (userland/build-libs.sh)
    pkgs="$(sed -n 's/^ \* pkg: //p' "$src" | head -1)"
    if [ -n "$pkgs" ]; then
        if [ ! -x "$CC_WRAP" ] || ! PKG_CONFIG_LIBDIR="$SR/lib/pkgconfig" pkg-config --exists $pkgs; then
            echo "  $name: skipped (needs $pkgs - run userland/build-libs.sh)"
            continue
        fi
        # shellcheck disable=SC2046
        "$CC_WRAP" -O2 -Wall -o "$OUT/$name" "$src" \
            $(PKG_CONFIG_LIBDIR="$SR/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR= pkg-config --static --cflags --libs $pkgs)
        echo "  $name ($pkgs)"
        continue
    fi
    "$LLVM/clang" "${CFLAGS[@]}" -c "$src" -o "$OUT/$name.o"
    ld.lld -static --image-base=0x60000000 -o "$OUT/$name" \
        "$SR/lib/crt1.o" "$SR/lib/crti.o" "$OUT/$name.o" \
        "$SR/lib/libc.a" "$SR/lib/libclang_rt.builtins-aarch64.a" "$SR/lib/crtn.o"
    rm -f "$OUT/$name.o"
    echo "  $name"
done
