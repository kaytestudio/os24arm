#!/bin/bash
# userland/make-stick.sh - an HPFS "USB stick" image with the desktop, all
# in \OS2, a hidden system folder (DIR shows it only with /A): the
# programs in build/userland/bin/* (busybox, dwl, foot, fuzzel, ...), the
# test programs and os2cmd, userland/rootfs/* (dwl.sh, env.sh, foot.ini,
# ...), a font (DejaVu Sans Mono, in fonts/) and keyboard layouts
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
    "dejavu-fonts-ttf-$DJV/ttf/DejaVuSansMono.ttf" "dejavu-fonts-ttf-$DJV/ttf/DejaVuSansMono-Bold.ttf"
[ -d "$W/bin" ] && cp "$W"/bin/* "$SYS"/
[ -d "$W/tests" ] && find "$W/tests" -maxdepth 1 -type f -perm -u+x -exec cp {} "$SYS"/ \;
cp -R "$ROOT"/userland/rootfs/* "$SYS"/
if [ -d "$W/xkb-data" ]; then cp -R "$W/xkb-data" "$SYS/xkb"; fi   # keyboard layouts
if [ -f "$W/Compose" ]; then cp "$W/Compose" "$SYS/Compose"; fi    # dead keys, compose
python3 "$ROOT/scripts/mkhpfs.py" "$IMG" --size 48M --from "$STAGE" --hidden OS2 --mbr >/dev/null
echo "made $IMG ($(ls "$SYS" | wc -l | tr -d ' ') entries in \\OS2)"
