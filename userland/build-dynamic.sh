#!/bin/bash
# userland/build-dynamic.sh - musl as a shared library (libc.so, which is
# also its dynamic linker, ld-musl-aarch64.so.1) and the dynamic-linking
# test (userland/tests/dyn), into build/userland/dyn:
#   lib/ld-musl-aarch64.so.1, lib/libc.so, lib/libdyn.so, lib/libdyn2.so
#   bin/dyntest        fixed address (0x60000000), its interpreter /h/lib/...
#   bin/dyntest-pie    position-independent, the same
#   bin/dyntest-std    the usual /lib/ld-musl-aarch64.so.1 (UNIXROOT's \lib)
# userland/make-stick.sh puts lib/ in the stick's \lib, bin/ in \OS2.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"; SR="$W/sysroot"; OUT="$W/dyn"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
[ -f "$W/musl-1.2.5.tar.gz" ] || "$ROOT/userland/build-musl.sh"
if [ ! -f "$W/musl-shared/musl-1.2.5/lib/libc.so" ]; then
    rm -rf "$W/musl-shared"; mkdir -p "$W/musl-shared"
    tar xzf "$W/musl-1.2.5.tar.gz" -C "$W/musl-shared"
    ( cd "$W/musl-shared/musl-1.2.5"
      CC="$LLVM/clang --target=aarch64-linux-musl" AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" \
      CFLAGS="-O2 -fno-stack-protector -mno-outline-atomics" LDFLAGS="-fuse-ld=lld" \
      LIBCC="$SR/lib/libclang_rt.builtins-aarch64.a" \
          ./configure --target=aarch64-linux-musl --prefix=/ --syslibdir=/lib --enable-shared --disable-static >/dev/null
      make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" lib/libc.so >/dev/null )
fi
mkdir -p "$OUT/lib" "$OUT/bin" "$OUT/obj"
cp "$W/musl-shared/musl-1.2.5/lib/libc.so" "$OUT/lib/libc.so"
cp "$OUT/lib/libc.so" "$OUT/lib/ld-musl-aarch64.so.1"
CC=("$LLVM/clang" --target=aarch64-linux-musl --sysroot="$SR" -nostdinc -isystem "$SR/include"
    -isystem "$("$LLVM/clang" -print-resource-dir)/include" -O2 -mno-outline-atomics)
LD=("$LLVM/clang" --target=aarch64-linux-musl -fuse-ld=lld -nostdlib)
RT="$SR/lib/libclang_rt.builtins-aarch64.a"
T="$ROOT/userland/tests/dyn"
for l in libdyn libdyn2; do
    "${CC[@]}" -fPIC -c "$T/$l.c" -o "$OUT/obj/$l.o"
done
"${LD[@]}" -shared -Wl,-soname,libdyn.so -o "$OUT/lib/libdyn.so" "$OUT/obj/libdyn.o" -L"$OUT/lib" -lc "$RT"
"${LD[@]}" -shared -Wl,-soname,libdyn2.so -o "$OUT/lib/libdyn2.so" "$OUT/obj/libdyn2.o" -L"$OUT/lib" -ldyn -lc "$RT"
"${CC[@]}" -c "$T/dyntest.c" -o "$OUT/obj/dyntest.o"
"${CC[@]}" -fPIE -c "$T/dyntest.c" -o "$OUT/obj/dyntest-pie.o"
link() {   # out interp obj crt1 [flags]
    local out=$1 interp=$2 obj=$3 crt=$4; shift 4
    "${LD[@]}" "$@" -o "$OUT/bin/$out" "$SR/lib/$crt" "$SR/lib/crti.o" "$obj" \
        -L"$OUT/lib" -ldyn -lc "$RT" "$SR/lib/crtn.o" \
        -Wl,-dynamic-linker,"$interp" -Wl,-rpath,/h/lib:/lib
}
link dyntest     /h/lib/ld-musl-aarch64.so.1 "$OUT/obj/dyntest.o"     crt1.o  -no-pie -Wl,--image-base=0x60000000
link dyntest-pie /h/lib/ld-musl-aarch64.so.1 "$OUT/obj/dyntest-pie.o" Scrt1.o -pie
link dyntest-std /lib/ld-musl-aarch64.so.1   "$OUT/obj/dyntest.o"     crt1.o  -no-pie -Wl,--image-base=0x60000000
echo "built $OUT (lib: $(ls "$OUT/lib" | tr '\n' ' '))"
