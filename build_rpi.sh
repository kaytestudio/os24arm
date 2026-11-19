#!/bin/bash
# build_rpi.sh - Build OS/2 Warp for Raspberry Pi 3+

set -e

echo "════════════════════════════════════════════════════════════"
echo "OS/2 Warp ARM64 - Raspberry Pi 3+ Builder"
echo "════════════════════════════════════════════════════════════"
echo ""

# Configuration
CC=clang
OBJCOPY=llvm-objcopy
TARGET=aarch64-none-elf
BUILD_DIR=build-rpi

CFLAGS="--target=$TARGET -ffreestanding -nostdlib -O2"
CFLAGS="$CFLAGS -Wall -Wextra -Wno-unused-parameter"
CFLAGS="$CFLAGS -Ikernel/include"

LDFLAGS="-T rpi_linker.ld -nostdlib"

# Create build directory
echo "Step 1: Creating build directory..."
rm -rf $BUILD_DIR
mkdir -p $BUILD_DIR
cd $BUILD_DIR
echo "Build directory created"
echo ""

# Compile boot.S
echo "Step 2: Compiling boot code..."
$CC $CFLAGS -c ../rpi_boot.S -o boot.o
echo "boot.o"
echo ""

# Compile main.c
echo "Step 3: Compiling kernel..."
$CC $CFLAGS -c ../rpi_main.c -o main.o
echo "main.o"
echo ""

# Compile kernel subsystems (stubs for now)
echo "Step 4: Compiling subsystems..."

# Create stub files if they don't exist
if [ ! -f ../kernel/src/mem.c ]; then
    cat > mem_stub.c << 'EOF'
#include <stdint.h>
#include <stddef.h>
void mem_init(uintptr_t start, size_t size) { (void)start; (void)size; }
EOF
    $CC $CFLAGS -c mem_stub.c -o mem.o
    echo "mem.o (stub)"
else
    $CC $CFLAGS -c ../kernel/src/mem.c -o mem.o
    echo "mem.o"
fi

if [ ! -f ../kernel/src/scheduler.c ]; then
    cat > scheduler_stub.c << 'EOF'
void scheduler_init(void) {}
EOF
    $CC $CFLAGS -c scheduler_stub.c -o scheduler.o
    echo "scheduler.o (stub)"
else
    $CC $CFLAGS -c ../kernel/src/scheduler.c -o scheduler.o
    echo "scheduler.o"
fi

if [ ! -f ../kernel/src/keyboard.c ]; then
    cat > keyboard_stub.c << 'EOF'
void kbd_init(void) {}
EOF
    $CC $CFLAGS -c keyboard_stub.c -o keyboard.o
    echo "keyboard.o (stub)"
else
    $CC $CFLAGS -c ../kernel/src/keyboard.c -o keyboard.o
    echo "keyboard.o"
fi

if [ ! -f ../kernel/src/vfs.c ]; then
    cat > vfs_stub.c << 'EOF'
void vfs_init(void) {}
EOF
    $CC $CFLAGS -c vfs_stub.c -o vfs.o
    echo "vfs.o (stub)"
else
    $CC $CFLAGS -c ../kernel/src/vfs.c -o vfs.o
    echo "vfs.o"
fi

if [ ! -f ../kernel/src/blkdev.c ]; then
    cat > blkdev_stub.c << 'EOF'
void blk_init(void) {}
EOF
    $CC $CFLAGS -c blkdev_stub.c -o blkdev.o
    echo "blkdev.o (stub)"
else
    $CC $CFLAGS -c ../kernel/src/blkdev.c -o blkdev.o
    echo "blkdev.o"
fi

echo ""

# Link
echo "Step 5: Linking kernel..."
$CC $CFLAGS $LDFLAGS \
    boot.o main.o mem.o scheduler.o keyboard.o vfs.o blkdev.o \
    -o os2warp.elf
echo "os2warp.elf"
echo ""

# Convert to binary
echo "Step 6: Creating kernel image..."
$OBJCOPY -O binary os2warp.elf kernel8.img
echo "kernel8.img"
echo ""

# Show results
echo "════════════════════════════════════════════════════════════"
echo "Build Complete."
echo "════════════════════════════════════════════════════════════"
echo ""
ls -lh kernel8.img
echo ""
echo "Kernel ready for Raspberry Pi 3+"
echo ""
echo "Next steps:"
echo "  1. Format SD card as FAT32"
echo "  2. Download RPi firmware:"
echo "     git clone --depth=1 https://github.com/raspberrypi/firmware /tmp/firmware"
echo "  3. Copy to SD card:"
echo "     cp /tmp/firmware/boot/bootcode.bin /Volumes/BOOT/"
echo "     cp /tmp/firmware/boot/start.elf /Volumes/BOOT/"
echo "     cp /tmp/firmware/boot/fixup.dat /Volumes/BOOT/"
echo "     cp config.txt /Volumes/BOOT/"
echo "     cp $BUILD_DIR/kernel8.img /Volumes/BOOT/"
echo "  4. Insert SD card into Pi and boot!"
echo ""
echo "See RPI_BOOT_GUIDE.md for detailed instructions"
echo "════════════════════════════════════════════════════════════"