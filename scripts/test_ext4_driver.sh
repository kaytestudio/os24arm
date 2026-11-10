#!/bin/bash
# scripts/test_ext4_driver.sh — build a real ext4 image with mke2fs, compile
# fs/ext4/tests/host_harness.c against the *actual* driver sources
# (fs/ext4/ext4.c + ext4_write.c, host-native clang, not the ARM64 cross
# compiler), run it against that image, and cross-check the result with
# real e2fsprogs tools (e2fsck -f, debugfs) as an independent correctness
# oracle. See fs/ext4/ext4_write.c's header comment for what write support
# this driver actually provides.
#
# Usage: scripts/test_ext4_driver.sh

set -e

# ── Locate e2fsprogs (same search order as scripts/mktestdisk.sh) ──────────
if command -v mkfs.ext4 >/dev/null 2>&1; then
    E2FS_BIN="$(dirname "$(command -v mkfs.ext4)")"
elif [ -x /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4 ]; then
    E2FS_BIN=/opt/homebrew/opt/e2fsprogs/sbin
elif [ -x /usr/local/opt/e2fsprogs/sbin/mkfs.ext4 ]; then
    E2FS_BIN=/usr/local/opt/e2fsprogs/sbin
else
    echo "mkfs.ext4 not found. Install with: brew install e2fsprogs" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

IMAGE="$WORK/ext4_test.img"
HARNESS="$WORK/host_harness"

echo "=== Building test image (8MB, 1024-byte blocks, no uninit_bg/metadata_csum/64bit) ==="
dd if=/dev/zero of="$IMAGE" bs=1M count=8 status=none
"$E2FS_BIN/mkfs.ext4" -F -O ^64bit,^metadata_csum,^uninit_bg -b 1024 "$IMAGE" >/dev/null
echo "Feature flags on the fresh image:"
"$E2FS_BIN/dumpe2fs" -h "$IMAGE" 2>/dev/null | grep -i "Filesystem features"

echo
echo "=== e2fsck on fresh image (sanity) ==="
"$E2FS_BIN/e2fsck" -fn "$IMAGE"

echo
echo "=== Compiling host_harness against the real driver (host clang) ==="
# -idirafter, not -I: kernel/include ships freestanding stub replacements
# for libc headers (e.g. its own stdint.h has no uint64_t) for the real
# ARM64 cross build's -nostdinc. -I would let those shadow the real system
# headers here and break unrelated system includes (e.g. sys/resource.h
# needs the real uint64_t); -idirafter only falls back to them for names
# the system doesn't already provide (vfs.h, blkdev.h, terminal.h, ...).
# -fno-builtin on kernel/src/string.c specifically: without it, a normal
# (non-freestanding) host build idiom-recognizes its hand-written
# memset/bzero loops as calls to the real library functions and rewrites
# them into each other (bzero -> memset -> optimizer's "memset-to-zero is
# bzero" idiom -> back into this file's own bzero = infinite recursion).
# The real ARM64 cross build never hits this because it already always
# passes -ffreestanding -fno-builtin (see kernel/CMakeLists.txt).
clang -std=c11 -Wall -Wextra -fno-builtin \
    -idirafter "$ROOT/fs/ext4" \
    -idirafter "$ROOT/kernel/include" \
    -o "$HARNESS" \
    "$ROOT/fs/ext4/tests/host_harness.c" \
    "$ROOT/fs/ext4/ext4.c" \
    "$ROOT/fs/ext4/ext4_write.c" \
    "$ROOT/kernel/src/string.c"

echo
echo "=== Running host_harness against the image ==="
set +e
"$HARNESS" "$IMAGE"
HARNESS_RC=$?
set -e

echo
echo "=== e2fsck -f on the image after the driver's writes (the real oracle) ==="
"$E2FS_BIN/e2fsck" -fy "$IMAGE"
FSCK_RC=$?

echo
echo "=== debugfs listing of what the driver wrote ==="
"$E2FS_BIN/debugfs" -R "ls -l /otherdir/moved" "$IMAGE" 2>/dev/null | head -20

if [ $HARNESS_RC -eq 0 ] && [ $FSCK_RC -eq 0 ]; then
    echo
    echo "RESULT: PASS (harness + e2fsck both clean)"
    exit 0
else
    echo
    echo "RESULT: FAIL (harness_rc=$HARNESS_RC fsck_rc=$FSCK_RC)"
    exit 1
fi
