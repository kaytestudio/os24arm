#!/bin/bash
# userland/build-busybox.sh - busybox (ash + the usual tools) as one static
# aarch64-linux-musl binary: build/userland/bin/busybox.
#
# defconfig, plus: static; no tc (doesn't build against these kernel
# headers); no x86 SHA acceleration; FEATURE_SH_STANDALONE and
# PREFER_APPLETS, so ash runs applets without /bin symlinks (it re-runs
# itself through /proc/self/exe, which the kernel provides); and
# BB_GLOBAL_CONST='' - busybox's "const pointer to globals" trick lets this
# clang read the pointer before it's set (vi crashed on a NULL), and its
# documented fix is to turn the trick off.
#
# Run it with RUNELF \h\busybox sh (after MOUNT usb0p1 \h).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"
VER=1.37.0
SHA256=3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4
URL="https://busybox.net/downloads/busybox-$VER.tar.bz2"
TAR="$W/dl/busybox-$VER.tar.bz2"
SRC="$W/src/busybox-$VER"
B="$W/b-busybox"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
CC="$W/toolchain/aarch64-linux-musl-cc"
MAKE="$(command -v gmake || command -v make)"
[ -x "$CC" ] || "$ROOT/userland/build-libs.sh"
mkdir -p "$W/dl" "$W/bin"
if [ ! -f "$TAR" ]; then
    echo "Downloading $URL"
    curl -sSfL -o "$TAR.part" "$URL" && mv "$TAR.part" "$TAR"
fi
echo "$SHA256  $TAR" | shasum -a 256 -c - >/dev/null \
    || { echo "SHA-256 mismatch for $TAR" >&2; rm -f "$TAR"; exit 1; }
if [ ! -d "$SRC" ]; then
    mkdir -p "$SRC.tmp" && tar xjf "$TAR" -C "$SRC.tmp" --strip-components=1 && mv "$SRC.tmp" "$SRC"
fi
mkdir -p "$B"
if [ ! -f "$B/.config" ]; then
    "$MAKE" -C "$SRC" O="$B" HOSTCC=cc defconfig >/dev/null
    sed -i.bak -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
        -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' \
        -e 's/^CONFIG_SHA1_HWACCEL=y/# CONFIG_SHA1_HWACCEL is not set/' \
        -e 's/^CONFIG_SHA256_HWACCEL=y/# CONFIG_SHA256_HWACCEL is not set/' \
        -e 's/^# CONFIG_FEATURE_SH_STANDALONE is not set/CONFIG_FEATURE_SH_STANDALONE=y/' \
        -e 's/^# CONFIG_FEATURE_PREFER_APPLETS is not set/CONFIG_FEATURE_PREFER_APPLETS=y/' "$B/.config"
    yes "" | "$MAKE" -C "$SRC" O="$B" HOSTCC=cc oldconfig >/dev/null 2>&1 || true
fi
if ! grep -q '^CONFIG_EXTRA_CFLAGS="-DBB_GLOBAL_CONST="$' "$B/.config"; then
    sed -i.bak 's/^CONFIG_EXTRA_CFLAGS=.*/CONFIG_EXTRA_CFLAGS="-DBB_GLOBAL_CONST="/' "$B/.config"
    grep -q '^CONFIG_EXTRA_CFLAGS="-DBB_GLOBAL_CONST="$' "$B/.config" \
        || { echo "busybox .config has no CONFIG_EXTRA_CFLAGS line" >&2; exit 1; }
    "$MAKE" -C "$SRC" O="$B" clean >/dev/null 2>&1 || true
fi
"$MAKE" -C "$SRC" O="$B" HOSTCC=cc CC="$CC" AR="$LLVM/llvm-ar" LD="$(command -v ld.lld)" \
    STRIP="$LLVM/llvm-strip" NM="$LLVM/llvm-nm" OBJCOPY="$LLVM/llvm-objcopy" \
    -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" busybox >"$B/build.log" 2>&1 \
    || { tail -20 "$B/build.log" >&2; exit 1; }
cp "$B/busybox" "$W/bin/busybox"
echo "  busybox $VER ($(wc -c < "$W/bin/busybox" | tr -d ' ') bytes)"
