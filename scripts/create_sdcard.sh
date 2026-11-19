#!/bin/bash

set -e

BUILD_DIR="$1"
SD_PREP_DIR="$BUILD_DIR/sdcard_files"

echo "Preparing SD Card Files for Raspberry Pi 3+"
echo ""

rm -rf "$SD_PREP_DIR"
mkdir -p "$SD_PREP_DIR"

if [ ! -f "$BUILD_DIR/kernel8.img" ]; then
    echo "Error: kernel8.img not found!"
    echo "Run 'make' first to build the kernel."
    exit 1
fi

echo "Step 1: Copying kernel..."
cp "$BUILD_DIR/kernel8.img" "$SD_PREP_DIR/"
echo "  kernel8.img"
echo ""

if [ -f "config.txt" ]; then
    echo "Step 2: Copying config.txt..."
    cp config.txt "$SD_PREP_DIR/"
    echo "  config.txt"
else
    echo "Step 2: Creating config.txt..."
    cat > "$SD_PREP_DIR/config.txt" << 'EOF'
# Raspberry Pi 3+ Boot Configuration for OS/2 Warp ARM64

# Enable 64-bit mode
arm_64bit=1

# Enable UART
enable_uart=1
uart_2ndstage=1

# Kernel
kernel=kernel8.img

# Core frequency for UART
core_freq=250

# Memory
gpu_mem=16

# Disable device tree (bare metal)
device_tree=

# Boot delay
boot_delay=1
EOF
    echo "  config.txt (generated)"
fi
echo ""

echo "Step 3: Downloading Raspberry Pi firmware..."
if [ ! -d "/tmp/rpi-firmware" ]; then
    git clone --depth=1 https://github.com/raspberrypi/firmware /tmp/rpi-firmware
    echo "  Firmware downloaded"
else
    echo "  Using cached firmware"
fi
echo ""

echo "Step 4: Copying bootloader files..."
cp /tmp/rpi-firmware/boot/bootcode.bin "$SD_PREP_DIR/"
echo "  bootcode.bin"
cp /tmp/rpi-firmware/boot/start.elf "$SD_PREP_DIR/"
echo "  start.elf"
cp /tmp/rpi-firmware/boot/fixup.dat "$SD_PREP_DIR/"
echo "  fixup.dat"
echo ""

cat > "$SD_PREP_DIR/README.txt" << 'EOF'
OS/2 Warp 4.52 ARM64 - Raspberry Pi 3+ Boot Files

Copy all files from this directory to a FAT32-formatted SD card.

Required files:
  - bootcode.bin   (Raspberry Pi bootloader)
  - start.elf      (GPU firmware)
  - fixup.dat      (GPU configuration)
  - config.txt     (Boot configuration)
  - kernel8.img    (OS/2 Warp kernel)

To boot:
  1. Format SD card as FAT32
  2. Copy all these files to the root of the SD card
  3. Insert SD card into Raspberry Pi 3+
  4. Connect USB-to-TTL serial cable to GPIO pins 6, 8, 10
  5. Open serial terminal at 115200 baud
  6. Power on the Pi

Serial connection:
  Pin 6  (GND)    -> Cable GND   (Black)
  Pin 8  (GPIO14) -> Cable RX    (Green)
  Pin 10 (GPIO15) -> Cable TX    (White)

Terminal command:
  screen /dev/tty.usbserial-* 115200

For more information, see RPI_BOOT_GUIDE.md
EOF

echo "SD Card Files Ready."
echo ""
echo "Files prepared in: $SD_PREP_DIR"
echo ""
ls -lh "$SD_PREP_DIR"
echo ""
echo "Next steps:"
echo "  1. Format SD card as FAT32:"
echo "     diskutil eraseDisk FAT32 BOOT /dev/diskX"
echo ""
echo "  2. Copy files to SD card:"
echo "     cp $SD_PREP_DIR/* /Volumes/BOOT/"
echo ""
echo "  3. Eject SD card:"
echo "     diskutil eject /dev/diskX"
echo ""
echo "  4. Insert into Pi and boot!"
echo ""
