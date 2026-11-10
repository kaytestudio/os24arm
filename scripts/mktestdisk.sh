#!/bin/bash
# scripts/mktestdisk.sh — build a real GPT test disk with BOOT (FAT16) +
# C: (btrfs or ext4) partitions for QEMU
#
# blkdev.c has no real VirtIO block driver yet (virtio_blk_read/write are
# unused stubs) — it only reads a "disk" from a fixed physical RAM address
# (PRELOADED_DISK_ADDR = 0x44000000, 24MB window), meant to be populated by
# injecting a real disk image there before boot via QEMU's -device loader.
#
# The disk has two GPT partitions, matching this project's "safe BOOT
# partition" design (see fat32_init() in kernel/src/fat32.c and
# ext4_init()/btrfs_init() in fs/ext4/ext4.c and fs/btrfs/btrfs.c): "BOOT"
# (FAT16, mounted at /BOOT — a recovery area any host OS can read) and "C"
# (btrfs or ext4, mounted at / — the main drive, holding CMD.EXE/HELLO.EXE).
# No mkfs.btrfs or FAT-populating tool ships with macOS, so the btrfs and
# FAT partitions are built directly by scripts/mkbtrfs_image.py and
# scripts/mkfat_image.py instead; ext4 uses mkfs.ext4 -d.
#
# Usage: [C_FS=btrfs|ext4] scripts/mktestdisk.sh [output.img]

set -e

DISK="${1:-build/testdisk.img}"
C_FS="${C_FS:-btrfs}"
BOOT_SIZE_MB=4
C_SIZE_MB=8
BOOT_START_SECTOR=2048
C_START_SECTOR=10240

case "$C_FS" in
    btrfs|ext4) ;;
    *) echo "C_FS must be btrfs or ext4 (got '$C_FS')" >&2; exit 1 ;;
esac

# ── Locate tools ────────────────────────────────────────────────────────────
if [ "$C_FS" = ext4 ]; then
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
fi

if ! command -v sgdisk >/dev/null 2>&1; then
    echo "sgdisk not found. Install with: brew install gptfdisk" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$(dirname "$DISK")"

# ── BOOT partition (FAT16) ───────────────────────────────────────────────────
# 4 MiB at 512-byte clusters is ~8100 clusters: FAT16 by cluster count,
# which is how kernel/src/fat32.c picks the variant.
echo "Building BOOT partition (FAT16, ${BOOT_SIZE_MB}MB)..."
cat > "$WORK/bootinfo.txt" <<EOF
This is the BOOT partition (FAT16) - a safety/recovery area.
The main C: drive ($C_FS) holds the regular files.
EOF
python3 "$SCRIPT_DIR/mkfat_image.py" "$WORK/boot.img" $((BOOT_SIZE_MB * 1024 * 1024)) \
    --label BOOT BOOTINFO.TXT:"$WORK/bootinfo.txt"

# ── C: partition (btrfs or ext4) ─────────────────────────────────────────────
echo "Building C: partition ($C_FS, ${C_SIZE_MB}MB)..."
C_FILES=()
add_file() {
    if [ -f "$2" ]; then
        C_FILES+=("$1:$2")
        echo "  + $1"
    else
        echo "  (skipped $1 — $2 not built yet)"
    fi
}
add_file CMD.EXE build/CMD.EXE
add_file HELLO.EXE build/HELLO.EXE
add_file KAYTEVM.EXE build/KAYTEVM.EXE

# KAYTE.BC — a demo .kayte script compiled to bytecode with the host
# `kayte` tool (kayte/CMakeLists.txt), for KAYTEVM.EXE to run on-target
# (KAYTEVM.EXE always runs this fixed filename — see kayte/kaytevm_main.c).
if [ -x build/kayte/kayte ] && [ -f kayte/examples/hello.kayte ]; then
    build/kayte/kayte --compile kayte/examples/hello.kayte -o "$WORK/kayte.bytecode" >/dev/null 2>&1
    if [ -f "$WORK/kayte.bytecode" ]; then
        C_FILES+=("KAYTE.BC:$WORK/kayte.bytecode")
        echo "  + KAYTE.BC (compiled from kayte/examples/hello.kayte)"
    else
        echo "  (skipped KAYTE.BC — compile failed)"
    fi
else
    echo "  (skipped KAYTE.BC — build/kayte/kayte not built yet)"
fi

# CONFIG.SYS — read from the root of C: by kernel/src/main.c at boot.
add_file CONFIG.SYS CONFIG.SYS

echo "Hello from OS/2 Warp ARM64 - $C_FS C: drive!" > "$WORK/readme.txt"
C_FILES+=("README.TXT:$WORK/readme.txt")

if [ "$C_FS" = btrfs ]; then
    python3 "$SCRIPT_DIR/mkbtrfs_image.py" "$WORK/c.img" $((C_SIZE_MB * 1024 * 1024)) "${C_FILES[@]}"
else
    # ^uninit_bg alongside ^64bit,^metadata_csum: fs/ext4/ext4_write.c's
    # write support (mkdir/create/write/unlink/rename) refuses to write to
    # an image with any of those three features, since it doesn't maintain
    # the group-descriptor/bitmap checksums they require - see its header
    # comment.
    mkdir "$WORK/c_root"
    for spec in "${C_FILES[@]}"; do
        cp "${spec#*:}" "$WORK/c_root/${spec%%:*}"
    done
    dd if=/dev/zero of="$WORK/c.img" bs=1M count=$C_SIZE_MB status=none
    "$E2FS_BIN/mkfs.ext4" -q -F -O ^64bit,^metadata_csum,^uninit_bg -b 1024 \
        -d "$WORK/c_root" "$WORK/c.img"
fi

# ── GPT layout + assembly ────────────────────────────────────────────────────
echo "Building GPT disk..."
TOTAL_MB=$((BOOT_SIZE_MB + C_SIZE_MB + 4))
truncate -s "${TOTAL_MB}M" "$DISK"
sgdisk --clear \
    --new=1:${BOOT_START_SECTOR}:+${BOOT_SIZE_MB}M --typecode=1:0700 --change-name=1:BOOT \
    --new=2:${C_START_SECTOR}:+${C_SIZE_MB}M --typecode=2:8300 --change-name=2:C \
    "$DISK" >/dev/null

dd if="$WORK/boot.img" of="$DISK" bs=512 seek=$BOOT_START_SECTOR conv=notrunc status=none
dd if="$WORK/c.img" of="$DISK" bs=512 seek=$C_START_SECTOR conv=notrunc status=none

echo
echo "Done: $DISK ($(stat -f%z "$DISK" 2>/dev/null || stat -c%s "$DISK") bytes)"
echo "  Partition 1 BOOT (FAT16, mounts at /BOOT)"
echo "  Partition 2 C    ($C_FS, mounts at /  — the main drive)"
echo
echo "Boot with:"
echo "  qemu-system-aarch64 -machine virt,gic-version=3 -cpu max -accel hvf -m 2G -smp 1 \\"
echo "    -kernel build/os2warp.img \\"
echo "    -device loader,file=$DISK,addr=0x44000000,force-raw=on \\"
echo "    -serial mon:stdio -display none -nographic"
