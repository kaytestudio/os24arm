#!/bin/bash
# userland/build-rpms.sh - the UNIXROOT packages, built by rpmbuild on the
# Mac from userland/rpm/<name>.spec: %prep unpacks the release and applies
# the patches, %build cross-compiles with the system's toolchain
# (userland/rpm/macros.os2warp: %{os2_configure}, %{os2_cc}, ...), %install
# installs into the buildroot. Results: build/userland/rpms/*.rpm (copied
# onto the stick by make-stick.sh, in \OS2\RPMS, for rpm -i) and the
# source RPMs in build/userland/rpms/SRPMS.
#
# Every SourceN is downloaded once into build/userland/dl and must match
# its SHA-256 in userland/rpm/sources (and its signature, when one is
# listed there) before rpmbuild sees it.
#   userland/build-rpms.sh [name...]      (default: every userland/rpm/*.spec)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"
SPECS="$ROOT/userland/rpm"
TOP="$W/rpmbuild"
OUT="$W/rpms"
DL="$W/dl"
for t in rpmbuild rpmspec; do
    command -v $t >/dev/null || { echo "$t not found (brew install rpm)" >&2; exit 1; }
done
[ -x "$W/toolchain/aarch64-linux-musl-cc" ] || { echo "no cross toolchain - run userland/build-libs.sh" >&2; exit 1; }
mkdir -p "$TOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS} "$OUT/SRPMS" "$DL"

# macros.os2warp goes on the end of rpm's macro path (not --load: --target
# makes rpm read its configuration again, which drops --load'ed macros)
MACROPATH="$(rpmbuild --showrc 2>/dev/null | sed -n 's/^Macro path: //p'):$SPECS/macros.os2warp"
RPMARGS=(--target aarch64-linux --macros "$MACROPATH"
         --define "_topdir $TOP" --define "_sourcedir $TOP/SOURCES"
         --define "os2_root $ROOT" --define "os2_w $W")

# fetch_source URL: into $DL, verified, then into SOURCES
fetch_source() {
    local url="$1" f; f="$(basename "$url")"
    local line; line="$(grep -E "^$f[[:space:]]" "$SPECS/sources" || true)"
    [ -n "$line" ] || { echo "$f: no SHA-256 in userland/rpm/sources" >&2; exit 1; }
    local sha sig; sha="$(echo "$line" | awk '{print $2}')"; sig="$(echo "$line" | awk '{print $3}')"
    if [ ! -f "$DL/$f" ]; then
        echo "Downloading $url"
        curl -sSfL -o "$DL/$f.part" "$url" && mv "$DL/$f.part" "$DL/$f"
    fi
    echo "$sha  $DL/$f" | shasum -a 256 -c - >/dev/null \
        || { echo "SHA-256 mismatch for $f" >&2; rm -f "$DL/$f"; exit 1; }
    if [ -n "$sig" ]; then
        local s="$DL/$(basename "$sig")" kr="$DL/gnu-keyring.gpg"
        [ -f "$s" ] || curl -sSfL -o "$s" "$sig"
        [ -f "$kr" ] || curl -sSfL -o "$kr" https://ftp.gnu.org/gnu/gnu-keyring.gpg
        gpgv --keyring "$kr" "$s" "$DL/$f" 2>/dev/null \
            || { echo "bad signature on $f" >&2; exit 1; }
    fi
    ln -sf "$DL/$f" "$TOP/SOURCES/$f"
}

names=("$@")
if [ ${#names[@]} = 0 ]; then
    for s in "$SPECS"/*.spec; do names+=("$(basename "${s%.spec}")"); done
fi
for n in "${names[@]}"; do
    spec="$SPECS/$n.spec"
    [ -f "$spec" ] || { echo "no $spec" >&2; exit 1; }
    # the sources (URLs) and the patches (files next to the spec)
    while read -r src; do
        case "$src" in
            *://*) fetch_source "$src" ;;
            *)     cp "$SPECS/$src" "$TOP/SOURCES/" ;;
        esac
    done < <(rpmspec -P "${RPMARGS[@]}" "$spec" 2>/dev/null | sed -En 's/^(Source|Patch)[0-9]*:[[:space:]]*//p')
    echo "rpmbuild $n ..."
    rpmbuild -ba "${RPMARGS[@]}" "$spec" > "$TOP/$n.log" 2>&1 \
        || { tail -30 "$TOP/$n.log"; echo "(log: $TOP/$n.log)" >&2; exit 1; }
    rm -f "$OUT/$n"-[0-9]*.rpm "$OUT/SRPMS/$n"-[0-9]*.src.rpm
    v="$(rpmspec -q --srpm --qf '%{VERSION}-%{RELEASE}' "${RPMARGS[@]}" "$spec" 2>/dev/null)"
    cp "$TOP"/RPMS/*/"$n-$v".*.rpm "$OUT"/
    cp "$TOP/SRPMS/$n-$v.src.rpm" "$OUT/SRPMS"/
done
ls -l "$OUT" "$OUT/SRPMS"
