#!/bin/bash
# userland/build-wps.sh - build/userland/bin/wps, the Workplace Shell: a
# Wayland compositor in C++ on wlroots 0.19 (userland/wps), in the style of
# OS/2 Warp 4. It's C++17 with LLVM's libc++, taken from the system's own
# packages (build/userland/rpms: libcxx-devel, and llvm for libunwind), so
# it links against what UNIXROOT ships.
#
# Needs userland/build-libs.sh, userland/build-compositor.sh (wlroots, fcft
# in the sysroot) and userland/build-rpms.sh llvm first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"
SR="$W/sysroot"
SCANNER="$W/host/bin/wayland-scanner"
OUT="$W/bin"
GEN="$W/gen-wps"
CXXR="$W/cxx-root"                     # libcxx-devel, unpacked
CLANG=/opt/homebrew/opt/llvm/bin/clang
[ -x "$CLANG" ] || { echo "no $CLANG (brew install llvm)" >&2; exit 1; }
[ -f "$SR/lib/pkgconfig/wlroots-0.19.pc" ] || { echo "no wlroots in the sysroot - run userland/build-libs.sh" >&2; exit 1; }
mkdir -p "$OUT" "$GEN"

# libc++: from the packages
rm -rf "$CXXR"; mkdir -p "$CXXR"
cxx_rpm="$(ls "$W"/rpms/libcxx-devel-[0-9]*.rpm 2>/dev/null | head -1)"
llvm_rpm="$(ls "$W"/rpms/llvm-[0-9]*.rpm 2>/dev/null | head -1)"
[ -n "$cxx_rpm" ] || { echo "no libcxx-devel package - run userland/build-rpms.sh llvm" >&2; exit 1; }
tar xf "$cxx_rpm" -C "$CXXR"
[ -f "$CXXR/usr/lib/libunwind.a" ] || tar xf "$llvm_rpm" -C "$CXXR" ./usr/lib/libunwind.a
for f in libc++.a libc++abi.a libunwind.a; do
    [ -f "$CXXR/usr/lib/$f" ] || { echo "no $f in the packages" >&2; exit 1; }
done

# the protocols
P="$SR/share/wayland-protocols"
DP="$W/src/dwl-0.8/protocols"
"$SCANNER" server-header "$P/stable/xdg-shell/xdg-shell.xml" "$GEN/xdg-shell-protocol.h"
"$SCANNER" server-header "$P/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml" \
    "$GEN/xdg-decoration-unstable-v1-protocol.h"
"$SCANNER" server-header "$DP/wlr-layer-shell-unstable-v1.xml" "$GEN/wlr-layer-shell-unstable-v1-protocol.h"

PKGS="wlroots-0.19 wayland-server xkbcommon pixman-1 libdrm fcft"
# shellcheck disable=SC2086
PC_CFLAGS="$(PKG_CONFIG_LIBDIR="$SR/lib/pkgconfig:$SR/share/pkgconfig" pkg-config --static --cflags $PKGS)"
# shellcheck disable=SC2086
PC_LIBS="$(PKG_CONFIG_LIBDIR="$SR/lib/pkgconfig:$SR/share/pkgconfig" pkg-config --static --libs $PKGS)"
RES="$("$CLANG" -print-resource-dir)/include"
BASE=(--target=aarch64-linux-musl --sysroot="$SR" -mno-outline-atomics -O2 -Wall
      -Wno-unused-parameter -DWLR_USE_UNSTABLE -I"$GEN" -I"$ROOT/userland/wlr-os2" -I"$ROOT/userland/wps")
CINC=(-nostdinc -isystem "$SR/include" -isystem "$RES")
CXXINC=(-nostdinc -nostdinc++ -isystem "$CXXR/usr/include/c++/v1" -isystem "$SR/include" -isystem "$RES")

O="$GEN/obj"; mkdir -p "$O"
# shellcheck disable=SC2086
"$CLANG" "${BASE[@]}" "${CINC[@]}" -std=c11 -D_POSIX_C_SOURCE=200809L $PC_CFLAGS \
    -c "$ROOT/userland/wps/text.c" -o "$O/text.o"
# shellcheck disable=SC2086
"$CLANG" "${BASE[@]}" "${CINC[@]}" $PC_CFLAGS -c "$ROOT/userland/wlr-os2/os2_backend.c" -o "$O/os2_backend.o"
# shellcheck disable=SC2086
"$CLANG" "${BASE[@]}" "${CINC[@]}" -std=c11 -w -c "$ROOT/userland/wps/image.c" -o "$O/image.o"   # (stb_image, as it comes)
# shellcheck disable=SC2086
"$CLANG" -x c++ "${BASE[@]}" "${CXXINC[@]}" -std=c++17 -fno-exceptions -fno-rtti \
    -Wno-c99-designator -Wno-missing-field-initializers -Wno-keyword-macro $PC_CFLAGS \
    -c "$ROOT/userland/wps/wps.cpp" -o "$O/wps.o"
# shellcheck disable=SC2086
"$CLANG" --target=aarch64-linux-musl --sysroot="$SR" -fuse-ld=lld -static -nostdlib \
    -Wl,--image-base=0x60000000 -L"$SR/lib" -o "$OUT/wps" \
    "$SR/lib/crt1.o" "$SR/lib/crti.o" "$O/wps.o" "$O/text.o" "$O/image.o" "$O/os2_backend.o" \
    $PC_LIBS \
    "$CXXR/usr/lib/libc++.a" "$CXXR/usr/lib/libc++abi.a" "$CXXR/usr/lib/libunwind.a" \
    -Wl,--start-group -lc "$SR/lib/libclang_rt.builtins-aarch64.a" -Wl,--end-group \
    "$SR/lib/crtn.o"
echo "  wps ($(du -h "$OUT/wps" | cut -f1))"
