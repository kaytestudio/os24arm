#!/bin/bash
# userland/build-libs.sh - cross-build the libraries wlroots needs (stage 4,
# userland/README.md) as static aarch64-linux-musl libraries:
#
#   Linux UAPI headers (sabotage's sanitized kernel-headers, as musl-cross-make
#   uses), libffi, libwayland (server, client, cursor, egl), pixman,
#   libxkbcommon, libdrm (core library only: wlroots uses its format and
#   modifier definitions), wayland-protocols, wlroots (pixman renderer;
#   no DRM/libinput/X11 backends, no session, no XWayland), expat, freetype,
#   fontconfig, tllist and fcft (font rendering, for the foot terminal)
#
# installed into the musl sysroot (build/userland/sysroot: include/, lib/,
# lib/pkgconfig/), plus a host wayland-scanner (with a host expat) in
# build/userland/host/bin, which the Wayland builds run to generate
# protocol code.
#
# Sources are pinned by version and SHA-256 (pixman and wayland match the
# checksums their maintainers publish; the others were pinned when first
# downloaded). Each is unpacked into its own directory under
# build/userland/src.
#
# Also writes the cross toolchain used for everything in userland/:
#   build/userland/toolchain/aarch64-linux-musl-cc   (compiler/linker wrapper)
#   build/userland/toolchain/cross.meson             (meson --cross-file)
#
# Usage: userland/build-libs.sh [--force]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build/userland"
SR="$WORK/sysroot"
HOST="$WORK/host"
TC="$WORK/toolchain"
DL="$WORK/dl"
SRC="$WORK/src"
LLVM="$(brew --prefix llvm 2>/dev/null || echo /usr)/bin"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
FORCE="${1:-}"

#        name          version  sha256                                                            url
PKGS=(
  "linux-headers 4.19.88-2 dc7abf734487553644258a3822cfd429d74656749e309f2b25f09f4282e05588 https://github.com/sabotage-linux/kernel-headers/releases/download/v4.19.88-2/linux-headers-4.19.88-2.tar.xz"
  "expat        2.7.3   71df8f40706a7bb0a80a5367079ea75d91da4f8c65c58ec59bcdfbf7decdab9f https://github.com/libexpat/libexpat/releases/download/R_2_7_3/expat-2.7.3.tar.xz"
  "libffi       3.5.2   f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz"
  "wayland      1.24.0  82892487a01ad67b334eca83b54317a7c86a03a89cfadacfef5211f11a5d0536 https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.24.0/downloads/wayland-1.24.0.tar.xz"
  "pixman       0.46.4  d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c https://www.cairographics.org/releases/pixman-0.46.4.tar.gz"
  "libdrm       2.4.128 3bb35db8700c2a0b569f2c6729a53f5495786856b310854c8de57782a22bddac https://dri.freedesktop.org/libdrm/libdrm-2.4.128.tar.xz"
  "wayland-protocols 1.45 4d2b2a9e3e099d017dc8107bf1c334d27bb87d9e4aff19a0c8d856d17cd41ef0 https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.45/downloads/wayland-protocols-1.45.tar.xz"
  "wlroots      0.19.2  09a0a8d60104c8b0772fee4f60ba76a89aed7e59dff7659d00960fba06bdf267 https://gitlab.freedesktop.org/wlroots/wlroots/-/releases/0.19.2/downloads/wlroots-0.19.2.tar.gz"
  "freetype     2.14.1  32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz"
  "fontconfig   2.16.0  6a33dc555cc9ba8b10caf7695878ef134eeb36d0af366041f639b1da9b6ed220 https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.16.0.tar.xz"
  "tllist       1.1.0   0e7b7094a02550dd80b7243bcffc3671550b0f1d8ba625e4dff52517827d5d23 https://codeberg.org/dnkl/tllist/archive/1.1.0.tar.gz"
  "fcft         3.3.3   b0c0f4a599f43723736c8565b8b84337c4195077f07f1bb8bb3252bb13a2306a https://codeberg.org/dnkl/fcft/archive/3.3.3.tar.gz"
  "libxkbcommon 1.11.0  78a6b14f16e9a55025978c252e53ce9e16a02bfdb929550b9a0db5af87db7e02 https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-1.11.0.tar.gz"
)

[ -f "$SR/lib/libc.a" ] || "$ROOT/userland/build-musl.sh"
for t in meson ninja pkg-config cmake bison; do
    command -v "$t" >/dev/null || { echo "$t not found (brew install $t)" >&2; exit 1; }
done

# ── Fetch and unpack ────────────────────────────────────────────────────
mkdir -p "$DL" "$SRC"
fetch() {   # name -> sets DIR to the unpacked source directory
    local line name ver sum url f
    for line in "${PKGS[@]}"; do
        read -r name ver sum url <<<"$line"
        [ "$name" = "$1" ] || continue
        f="$DL/$(basename "$url")"
        case "$(basename "$url")" in "$name"*|lib*|xkbcommon*) ;; *) f="$DL/$name-$(basename "$url")" ;; esac
        if [ ! -f "$f" ]; then
            echo "Downloading $url"
            curl -sSfL -o "$f.part" "$url"
            mv "$f.part" "$f"
        fi
        echo "$sum  $f" | shasum -a 256 -c - >/dev/null \
            || { echo "SHA-256 mismatch for $f - refusing to build" >&2; rm -f "$f"; exit 1; }
        DIR="$SRC/$name-$ver"
        if [ ! -d "$DIR" ]; then
            mkdir -p "$DIR.tmp"
            tar xf "$f" -C "$DIR.tmp" --strip-components=1
            mv "$DIR.tmp" "$DIR"
        fi
        return
    done
    echo "unknown package $1" >&2; exit 1
}

# ── Cross toolchain ─────────────────────────────────────────────────────
mkdir -p "$TC"
CC_WRAP="$TC/aarch64-linux-musl-cc"
cat > "$CC_WRAP" <<EOF
#!/bin/bash
# Generated by userland/build-libs.sh. clang for aarch64-linux-musl with the
# musl sysroot; links static executables the kernel's loader accepts
# (ET_EXEC inside the user window at 0x60000000).
SR="$SR"
CFLAGS=(--target=aarch64-linux-musl --sysroot="\$SR" -mno-outline-atomics
        -nostdinc -isystem "\$SR/include" -isystem "$("$LLVM/clang" -print-resource-dir)/include")
link=1
for a in "\$@"; do case "\$a" in -c|-S|-E|-M|-MM) link=0;; esac; done
if [ \$link = 0 ]; then exec "$LLVM/clang" "\${CFLAGS[@]}" "\$@"; fi
exec "$LLVM/clang" "\${CFLAGS[@]}" -fuse-ld=lld -static -nostdlib \\
    -Wl,--image-base=0x60000000 -L"\$SR/lib" \\
    "\$SR/lib/crt1.o" "\$SR/lib/crti.o" "\$@" \\
    -Wl,--start-group -lc "\$SR/lib/libclang_rt.builtins-aarch64.a" -Wl,--end-group \\
    "\$SR/lib/crtn.o"
EOF
chmod +x "$CC_WRAP"
cat > "$TC/cross.meson" <<EOF
# Generated by userland/build-libs.sh
[binaries]
c = '$CC_WRAP'
ar = '$LLVM/llvm-ar'
strip = '$LLVM/llvm-strip'
ranlib = '$LLVM/llvm-ranlib'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$SR/lib/pkgconfig:$SR/share/pkgconfig'

[built-in options]
default_library = 'static'
prefer_static = true
c_args = ['-O2', '-fno-stack-protector']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a72'
endian = 'little'
EOF

done_marker() { [ -f "$WORK/.built-$1" ] && [ "$FORCE" != "--force" ]; }
mark() { touch "$WORK/.built-$1"; }

# ── Host: expat + wayland-scanner ───────────────────────────────────────
if ! done_marker host-scanner; then
    fetch expat
    rm -rf "$WORK/b-expat-host"
    cmake -S "$DIR" -B "$WORK/b-expat-host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$HOST" -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
        -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF >/dev/null
    ninja -C "$WORK/b-expat-host" install >/dev/null
    fetch wayland
    rm -rf "$WORK/b-wayland-host"
    PKG_CONFIG_PATH="$HOST/lib/pkgconfig" meson setup "$WORK/b-wayland-host" "$DIR" \
        --prefix="$HOST" -Dlibraries=false -Dscanner=true -Ddocumentation=false \
        -Ddtd_validation=false -Dtests=false >/dev/null
    ninja -C "$WORK/b-wayland-host" install >/dev/null
    mark host-scanner
fi
echo "host wayland-scanner: $("$HOST/bin/wayland-scanner" --version 2>&1)"

unset PKG_CONFIG_PATH          # build-machine lookups: -Dbuild.pkg_config_path (below)
CROSS=(--cross-file "$TC/cross.meson" --prefix="$SR" --libdir=lib --buildtype=release
       -Dbuild.pkg_config_path="$HOST/lib/pkgconfig")

# ── Linux UAPI headers (linux/, asm/, drm/, ...) ────────────────────────
if ! done_marker linux-headers; then
    fetch linux-headers
    make -C "$DIR" ARCH=arm64 prefix= DESTDIR="$SR" install >/dev/null
    mark linux-headers
fi

# ── libffi (autotools) ──────────────────────────────────────────────────
# Static trampolines are off: they need /proc/self/maps.
if ! done_marker libffi; then
    fetch libffi
    rm -rf "$WORK/b-libffi"; mkdir -p "$WORK/b-libffi"
    (cd "$WORK/b-libffi" &&
     CC="$CC_WRAP" AR="$LLVM/llvm-ar" RANLIB="$LLVM/llvm-ranlib" CFLAGS="-O2" \
         "$DIR/configure" --host=aarch64-linux-musl --prefix="$SR" --libdir="$SR/lib" \
         --disable-shared --enable-static --disable-docs --disable-multi-os-directory \
         --disable-exec-static-tramp >/dev/null &&
     make -j"$JOBS" >/dev/null && make install >/dev/null)
    mark libffi
fi
echo "libffi: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig pkg-config --modversion libffi)"

# ── libwayland ──────────────────────────────────────────────────────────
if ! done_marker wayland; then
    fetch wayland
    rm -rf "$WORK/b-wayland"
    meson setup "$WORK/b-wayland" "$DIR" "${CROSS[@]}" -Dscanner=false -Dlibraries=true \
        -Ddocumentation=false -Ddtd_validation=false -Dtests=false >/dev/null
    ninja -C "$WORK/b-wayland" install >/dev/null
    mark wayland
fi
echo "wayland-server: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig pkg-config --modversion wayland-server)"

# ── pixman ──────────────────────────────────────────────────────────────
if ! done_marker pixman; then
    fetch pixman
    rm -rf "$WORK/b-pixman"
    meson setup "$WORK/b-pixman" "$DIR" "${CROSS[@]}" -Dtests=disabled -Ddemos=disabled \
        -Dgtk=disabled -Dlibpng=disabled -Dopenmp=disabled -Dtimers=false >/dev/null
    ninja -C "$WORK/b-pixman" install >/dev/null
    mark pixman
fi
echo "pixman: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig pkg-config --modversion pixman-1)"

# ── libxkbcommon ────────────────────────────────────────────────────────
# No X11, no registry (libxml2), no tools. Keymaps come from strings
# (xkb_keymap_new_from_string) until an xkeyboard-config tree is installed.
if ! done_marker xkbcommon; then
    fetch libxkbcommon
    rm -rf "$WORK/b-xkbcommon"
    meson setup "$WORK/b-xkbcommon" "$DIR" "${CROSS[@]}" -Denable-x11=false \
        -Denable-wayland=false -Denable-docs=false -Denable-tools=false \
        -Denable-xkbregistry=false -Denable-bash-completion=false \
        -Dxkb-config-root=/usr/share/X11/xkb -Dx-locale-root=/usr/share/X11/locale >/dev/null
    ninja -C "$WORK/b-xkbcommon" install >/dev/null
    mark xkbcommon
fi
echo "xkbcommon: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig pkg-config --modversion xkbcommon)"

# ── libdrm (no GPU drivers, no udev) ──────────────────────────────────
if ! done_marker libdrm; then
    fetch libdrm
    rm -rf "$WORK/b-libdrm"
    meson setup "$WORK/b-libdrm" "$DIR" "${CROSS[@]}" -Dintel=disabled -Dradeon=disabled \
        -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Domap=disabled \
        -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Dvc4=disabled \
        -Detnaviv=disabled -Dcairo-tests=disabled -Dman-pages=disabled \
        -Dvalgrind=disabled -Dtests=false -Dudev=false -Dinstall-test-programs=false >/dev/null
    ninja -C "$WORK/b-libdrm" install >/dev/null
    mark libdrm
fi
echo "libdrm: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig pkg-config --modversion libdrm)"

# ── wayland-protocols (XML + pkg-config file) ───────────────────────────
if ! done_marker wayland-protocols; then
    fetch wayland-protocols
    rm -rf "$WORK/b-wayland-protocols"
    meson setup "$WORK/b-wayland-protocols" "$DIR" "${CROSS[@]}" -Dtests=false >/dev/null
    ninja -C "$WORK/b-wayland-protocols" install >/dev/null
    mark wayland-protocols
fi
echo "wayland-protocols: $(PKG_CONFIG_LIBDIR=$SR/share/pkgconfig pkg-config --modversion wayland-protocols)"

# ── wlroots ─────────────────────────────────────────────────────────────
if ! done_marker wlroots; then
    fetch wlroots
    rm -rf "$WORK/b-wlroots"
    meson setup "$WORK/b-wlroots" "$DIR" "${CROSS[@]}" -Dxwayland=disabled -Dexamples=false \
        -Drenderers= -Dbackends= -Dallocators= -Dsession=disabled -Dcolor-management=disabled \
        -Dlibliftoff=disabled -Dxcb-errors=disabled >/dev/null
    ninja -C "$WORK/b-wlroots" install >/dev/null
    mark wlroots
fi
echo "wlroots: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig:$SR/share/pkgconfig pkg-config --modversion wlroots-0.19)"

# ── expat (for fontconfig; the host one above is for wayland-scanner) ───
if ! done_marker expat; then
    fetch expat
    rm -rf "$WORK/b-expat"
    cmake -S "$DIR" -B "$WORK/b-expat" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER="$CC_WRAP" \
        -DCMAKE_AR="$LLVM/llvm-ar" -DCMAKE_RANLIB="$LLVM/llvm-ranlib" \
        -DCMAKE_INSTALL_PREFIX="$SR" -DCMAKE_INSTALL_LIBDIR=lib -DEXPAT_BUILD_TOOLS=OFF \
        -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF \
        -DEXPAT_SHARED_LIBS=OFF >/dev/null
    ninja -C "$WORK/b-expat" install >/dev/null
    mark expat
fi

# ── freetype (no zlib/png/brotli/bzip2/harfbuzz) ────────────────────────
if ! done_marker freetype; then
    fetch freetype
    rm -rf "$WORK/b-freetype"
    meson setup "$WORK/b-freetype" "$DIR" "${CROSS[@]}" -Dbrotli=disabled -Dbzip2=disabled \
        -Dharfbuzz=disabled -Dpng=disabled -Dzlib=internal -Dtests=disabled >/dev/null
    ninja -C "$WORK/b-freetype" install >/dev/null
    mark freetype
fi
echo "freetype: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig pkg-config --modversion freetype2)"

# ── fontconfig (library only; its configuration comes at run time from
#    FONTCONFIG_FILE, e.g. a fonts.conf next to the fonts) ───────────────
if ! done_marker fontconfig; then
    fetch fontconfig
    rm -rf "$WORK/b-fontconfig"
    meson setup "$WORK/b-fontconfig" "$DIR" "${CROSS[@]}" -Ddoc=disabled -Dnls=disabled \
        -Dtests=disabled -Dtools=disabled -Dcache-build=disabled -Dxml-backend=expat \
        -Dcache-dir=/tmp/fontconfig >/dev/null
    ninja -C "$WORK/b-fontconfig" install >/dev/null
    mark fontconfig
fi
echo "fontconfig: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig pkg-config --modversion fontconfig)"

# ── tllist (header-only) and fcft ──────────────────────────────────────
if ! done_marker tllist; then
    fetch tllist
    rm -rf "$WORK/b-tllist"
    meson setup "$WORK/b-tllist" "$DIR" "${CROSS[@]}" >/dev/null
    ninja -C "$WORK/b-tllist" install >/dev/null
    mark tllist
fi
if ! done_marker fcft; then
    fetch fcft
    rm -rf "$WORK/b-fcft"
    meson setup "$WORK/b-fcft" "$DIR" "${CROSS[@]}" -Ddocs=disabled -Dgrapheme-shaping=disabled \
        -Drun-shaping=disabled -Dtest-text-shaping=false -Dexamples=false >/dev/null
    ninja -C "$WORK/b-fcft" install >/dev/null
    mark fcft
fi
echo "fcft: $(PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig:$SR/share/pkgconfig pkg-config --modversion fcft)"

echo "libraries installed in $SR/lib:"
ls "$SR/lib" | grep -E '\.a$' | tr '\n' ' '; echo
