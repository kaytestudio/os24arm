#!/bin/bash
# userland/build-binutils.sh - GNU binutils for the system itself: as, ld,
# ar, nm, objdump, objcopy, readelf, strip, size, strings, addr2line,
# c++filt, ranlib - static AArch64 programs (aarch64-linux-musl, native:
# they assemble and link for this machine), built with the musl toolchain
# from userland/build-libs.sh and installed under /usr into
# build/userland/pkgroot/binutils, from which userland/build-rpms.sh makes
# the RPM that installs them into UNIXROOT.
#   userland/build-binutils.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
CC_WRAP="$W/toolchain/aarch64-linux-musl-cc"
[ -x "$CC_WRAP" ] || { echo "no $CC_WRAP - run userland/build-libs.sh first" >&2; exit 1; }

VER=2.44
SHA256=ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237
URL="https://ftp.gnu.org/gnu/binutils/binutils-$VER.tar.xz"
TAR="$W/dl/binutils-$VER.tar.xz"
SRC="$W/src/binutils-$VER"
B="$W/b-binutils"
DEST="$W/pkgroot/binutils"

mkdir -p "$W/dl" "$W/src"
if [ ! -f "$TAR" ]; then
    echo "Downloading $URL"
    curl -sSfL -o "$TAR.part" "$URL" && mv "$TAR.part" "$TAR"
fi
echo "$SHA256  $TAR" | shasum -a 256 -c - >/dev/null \
    || { echo "SHA-256 mismatch for $TAR" >&2; rm -f "$TAR"; exit 1; }
if [ ! -d "$SRC" ]; then
    mkdir -p "$SRC.tmp" && tar xJf "$TAR" -C "$SRC.tmp" --strip-components=1
    mv "$SRC.tmp" "$SRC"
fi

# ld's default load address: the kernel runs programs in its user window
# at 0x60000000 (kernel/include/mmu.h), not at Linux's 0x400000 - so plain
# "ld x.o" makes a program this system runs.
sed -i '' 's/^TEXT_START_ADDR=0x400000$/TEXT_START_ADDR=0x60000000/' \
    "$SRC/ld/emulparams/aarch64linux.sh"
grep -q '^TEXT_START_ADDR=0x60000000$' "$SRC/ld/emulparams/aarch64linux.sh" \
    || { echo "ld/emulparams/aarch64linux.sh changed upstream" >&2; exit 1; }

rm -rf "$B" "$DEST"; mkdir -p "$B"
cd "$B"
# No gold or gprofng (C++), no plugins (no dlopen here), no NLS; the
# bundled zlib. CC_FOR_BUILD: the Mac's compiler, for the build-time tools.
"$SRC/configure" --host=aarch64-linux-musl --target=aarch64-linux-musl \
    --prefix=/usr --disable-nls --disable-werror --disable-gdb --disable-gdbserver \
    --disable-sim --disable-gold --disable-gprofng --disable-plugins \
    --disable-shared --enable-static --without-zstd --without-debuginfod \
    --with-system-zlib=no --disable-libctf \
    CC="$CC_WRAP" AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" \
    NM="$LLVM/llvm-nm" STRIP="$LLVM/llvm-strip" \
    CC_FOR_BUILD=/usr/bin/cc CFLAGS="-O2" > "$B/configure.log" 2>&1 \
    || { tail -30 "$B/configure.log"; exit 1; }
make -j"$(sysctl -n hw.ncpu)" MAKEINFO=true > "$B/make.log" 2>&1 \
    || { tail -40 "$B/make.log"; exit 1; }
make install DESTDIR="$DEST" MAKEINFO=true > "$B/install.log" 2>&1 \
    || { tail -30 "$B/install.log"; exit 1; }

# What the package carries: the programs (stripped) and the man pages; not
# the libraries for building binutils-using tools, nor the second copies
# Linux would hard-link (usr/aarch64-linux-musl/bin, ld.bfd - this system
# has no hard links). ld has its default linker scripts built in.
rm -rf "$DEST/usr/include" "$DEST/usr/lib" "$DEST/usr/share/info" \
       "$DEST/usr/aarch64-linux-musl" "$DEST/usr/bin/ld.bfd" 2>/dev/null || true
for f in "$DEST"/usr/bin/* "$DEST"/usr/aarch64-linux-musl/bin/*; do
    [ -f "$f" ] && "$LLVM/llvm-strip" "$f"
done
echo "binutils $VER in $DEST:"
du -sh "$DEST" | cut -f1
ls "$DEST/usr/bin"
