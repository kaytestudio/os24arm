#!/bin/bash
# scripts/test_f2fs_driver.sh — compile fs/f2fs/tests/host_harness.c against
# the *actual* driver sources (fs/f2fs/f2fs.c + f2fs_write.c, host-native
# clang, not the ARM64 cross compiler) and run it. The harness formats its
# own image (no mkfs.f2fs available - see fs/f2fs/f2fs_write.c's header
# comment) and self-verifies through a full unmount/remount cycle - there is
# no independent fsck.f2fs oracle here the way scripts/test_ext4_driver.sh
# has e2fsck, so this is a weaker (self-consistency-only) guarantee. See
# fs/f2fs/f2fs_write.c's header comment for exactly what is and isn't
# verified.
#
# Usage: scripts/test_f2fs_driver.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

IMAGE="$WORK/f2fs_test.img"
HARNESS="$WORK/host_harness"

echo "=== Compiling host_harness against the real driver (host clang) ==="
# -fno-builtin + -idirafter kernel/include: same fixes worked out for the
# ext4 harness (scripts/test_ext4_driver.sh) - kernel/include ships
# freestanding stub replacements for libc headers (e.g. its own stdint.h
# has no uint64_t) that must not shadow the real system headers, and this
# project's hand-written memset/bzero loops must not be idiom-recognized
# and rewritten into each other by a non-freestanding host build.
clang -std=c11 -Wall -Wextra -fno-builtin \
    -idirafter "$ROOT/fs/f2fs" \
    -idirafter "$ROOT/kernel/include" \
    -o "$HARNESS" \
    "$ROOT/fs/f2fs/tests/host_harness.c" \
    "$ROOT/fs/f2fs/f2fs.c" \
    "$ROOT/fs/f2fs/f2fs_write.c" \
    "$ROOT/kernel/src/string.c"

echo
echo "=== Running host_harness (formats its own image, then exercises it) ==="
set +e
"$HARNESS" "$IMAGE"
HARNESS_RC=$?
set -e

if [ $HARNESS_RC -eq 0 ]; then
    echo
    echo "RESULT: PASS (self-consistency only - no independent fsck.f2fs oracle available)"
    exit 0
else
    echo
    echo "RESULT: FAIL (harness_rc=$HARNESS_RC)"
    exit 1
fi
