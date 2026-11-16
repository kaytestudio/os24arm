#!/bin/bash
# userland/make-stick.sh - an HPFS "USB stick" image with the desktop, all
# in \OS2, a hidden system folder (DIR shows it only with /A): the
# programs in build/userland/bin/* (busybox, dwl, foot, fuzzel, ...), the
# test programs and os2cmd, userland/rootfs/* (dwl.sh, env.sh, foot.ini,
# ...), fonts (DejaVu Sans Mono for the terminals, DejaVu Sans for the
# Workplace Shell, in fonts/) and keyboard layouts
# (xkeyboard-config, in xkb/, from userland/build-libs.sh). The rest of the
# stick is the user's.
#   userland/make-stick.sh [image]     (default build/userland/stick.img)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"
IMG="${1:-$W/stick.img}"
DJV=2.37
DJV_SHA256=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
DJV_TAR="$W/dl/dejavu-fonts-ttf-$DJV.tar.bz2"
if [ ! -f "$DJV_TAR" ]; then
    mkdir -p "$W/dl"
    curl -sSfL -o "$DJV_TAR.part" \
        "https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-$DJV.tar.bz2"
    mv "$DJV_TAR.part" "$DJV_TAR"
fi
echo "$DJV_SHA256  $DJV_TAR" | shasum -a 256 -c - >/dev/null \
    || { echo "SHA-256 mismatch for $DJV_TAR" >&2; rm -f "$DJV_TAR"; exit 1; }
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
SYS="$STAGE/OS2"
mkdir -p "$SYS/fonts"
tar xjf "$DJV_TAR" -C "$SYS/fonts" --strip-components=2 \
    "dejavu-fonts-ttf-$DJV/ttf/DejaVuSansMono.ttf" "dejavu-fonts-ttf-$DJV/ttf/DejaVuSansMono-Bold.ttf" \
    "dejavu-fonts-ttf-$DJV/ttf/DejaVuSans.ttf" "dejavu-fonts-ttf-$DJV/ttf/DejaVuSans-Bold.ttf"
[ -d "$W/bin" ] && cp "$W"/bin/* "$SYS"/
[ -d "$W/tests" ] && find "$W/tests" -maxdepth 1 -type f -perm -u+x -exec cp {} "$SYS"/ \;
cp -R "$ROOT"/userland/rootfs/* "$SYS"/
if [ -d "$W/xkb-data" ]; then cp -R "$W/xkb-data" "$SYS/xkb"; fi   # keyboard layouts
if [ -f "$W/Compose" ]; then cp "$W/Compose" "$SYS/Compose"; fi    # dead keys, compose
# UNIXROOT's /bin/sh (the stick is UNIXROOT): busybox's sh, through a
# launcher - make, system() and popen() check that it's there
if [ -f "$W/tests/applet" ]; then mkdir -p "$STAGE/bin"; cp "$W/tests/applet" "$STAGE/bin/sh"; fi
# shared libraries (userland/build-dynamic.sh): musl's libc.so and dynamic
# linker in \lib (UNIXROOT's /lib), and the dynamic-linking tests
# (and the dynamic linker's library path: <its prefix>/etc/ld-musl-aarch64.path)
if [ -d "$W/dyn/lib" ]; then
    mkdir -p "$STAGE/lib" "$STAGE/etc"; cp "$W"/dyn/lib/* "$STAGE/lib"/; cp "$W"/dyn/bin/* "$SYS"/
    printf '/lib\n/usr/local/lib\n/usr/lib\n/h/lib\n' > "$STAGE/etc/ld-musl-aarch64.path"
fi
# the UNIXROOT packages (userland/build-rpms.sh), for RPM -i NAME
if ls "$W"/rpms/*.rpm >/dev/null 2>&1; then mkdir -p "$SYS/RPMS"; cp "$W"/rpms/*.rpm "$SYS/RPMS"/; fi
# STICK_EXTRA=dir: that folder too, at the stick's root (scripts/desktop.sh --os2-demos: \OS2DEMOS)
if [ -n "${STICK_EXTRA:-}" ] && [ -d "$STICK_EXTRA" ]; then cp -R "$STICK_EXTRA" "$STAGE"/; fi
python3 "$ROOT/scripts/mkhpfs.py" "$IMG" --size 512M --from "$STAGE" --hidden OS2 --mbr >/dev/null
echo "made $IMG ($(ls "$SYS" | wc -l | tr -d ' ') entries in \\OS2)"
