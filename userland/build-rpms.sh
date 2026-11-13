#!/bin/bash
# userland/build-rpms.sh - the UNIXROOT packages, as RPMs, with rpmbuild on
# the Mac: userland/rpm/<name>.spec, packaging what userland/build-<name>.sh
# installed into build/userland/pkgroot/<name>. Results in
# build/userland/rpms/ (and copied onto the stick by make-stick.sh, in
# \OS2\RPMS, for RPM -i).
#
# The packages are made for the system's installer (busybox's rpm, under
# the RPM command): format 4 headers and an xz payload (rpm 6 would write
# format 6 and zstd), Linux/aarch64, no Mac-side strip or debuginfo, and no
# automatic dependencies.
#   userland/build-rpms.sh [name...]      (default: every spec that has a pkgroot)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$ROOT/build/userland"
TOP="$W/rpmbuild"
OUT="$W/rpms"
command -v rpmbuild >/dev/null || { echo "rpmbuild not found (brew install rpm)" >&2; exit 1; }
mkdir -p "$TOP"/{BUILD,BUILDROOT,RPMS,SPECS,SRPMS} "$OUT"
names=("$@")
if [ ${#names[@]} = 0 ]; then
    for s in "$ROOT"/userland/rpm/*.spec; do
        n="$(basename "${s%.spec}")"
        [ -d "$W/pkgroot/$n" ] && names+=("$n")
    done
fi
for n in "${names[@]}"; do
    [ -d "$W/pkgroot/$n" ] || { echo "$n: no $W/pkgroot/$n - run userland/build-$n.sh" >&2; exit 1; }
    rpmbuild -bb --target aarch64-linux \
        --define "_topdir $TOP" \
        --define "pkgroot $W/pkgroot/$n" \
        --define "_rpmformat 4" \
        --define "_binary_payload w9.xzdio" \
        --define "debug_package %{nil}" \
        --define "__os_install_post %{nil}" \
        --define "_build_id_links none" \
        --define "_buildhost os2warp-build" \
        --define "_unpackaged_files_terminate_build 1" \
        "$ROOT/userland/rpm/$n.spec" > "$TOP/$n.log" 2>&1 \
        || { tail -25 "$TOP/$n.log"; exit 1; }
    cp "$TOP"/RPMS/*/"$n"-[0-9]*.rpm "$OUT"/
done
ls -la "$OUT"
