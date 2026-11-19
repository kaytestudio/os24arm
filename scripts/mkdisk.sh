#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# mkdisk.sh — OS/2 Warp 4.52 ARM64 Disk Image Creator
#
# Creates a 6 GiB GPT disk image with 4 partitions:
#   P1: EFI (FAT32, 100 MiB)     → /boot/efi
#   P2: OS2ROOT (EXT4, 4 GiB)    → /
#   P3: FAT16DATA (FAT16, 512 MiB) → /mnt/fat16
#   P4: EXFATUSB (exFAT, 1 GiB)  → /mnt/exfat
#
# Compatible with Linux, macOS, and ODROID devices
# ═══════════════════════════════════════════════════════════════

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
MAGENTA='\033[0;35m'
NC='\033[0m' # No Color

# Detect OS
OS_TYPE="$(uname -s)"
case "$OS_TYPE" in
    Linux*)     OS="Linux";;
    Darwin*)    OS="macOS";;
    *)          OS="Unknown";;
esac

# Detect if running on ODROID
ODROID_BOARD=""
if [ "$OS" = "Linux" ] && [ -f /proc/device-tree/model ]; then
    BOARD_MODEL=$(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')
    case "$BOARD_MODEL" in
        *"ODROID-N2"*)   ODROID_BOARD="ODROID-N2/N2+";;
        *"ODROID-C4"*)   ODROID_BOARD="ODROID-C4";;
        *"ODROID-M1"*)   ODROID_BOARD="ODROID-M1";;
        *"ODROID"*)      ODROID_BOARD="ODROID (Unknown model)";;
    esac
fi

# Print header
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  OS/2 Warp 4.52 ARM64 — Disk Image Creator${NC}"
echo -e "${CYAN}  Platform: $OS${NC}"
if [ -n "$ODROID_BOARD" ]; then
    echo -e "${MAGENTA}  Hardware: $ODROID_BOARD${NC}"
fi
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo

# Check arguments
ODROID_MODE=0
if [ $# -eq 2 ] && [ "$2" = "--odroid" ]; then
    ODROID_MODE=1
    echo -e "${MAGENTA}ODROID mode enabled${NC}"
    echo
fi

if [ $# -lt 1 ]; then
    echo -e "${RED}Usage: $0 <output_image> [--odroid]${NC}"
    echo -e "${YELLOW}Example: $0 os2warp452.img${NC}"
    echo -e "${YELLOW}ODROID:  $0 os2warp452.img --odroid${NC}"
    echo
    echo -e "${CYAN}ODROID mode:${NC}"
    echo -e "  • Adds U-Boot boot partition (16 MiB before P1)"
    echo -e "  • Optimizes for Amlogic/Rockchip boot sequence"
    echo -e "  • Recommended for ODROID-N2/N2+/C4/M1"
    exit 1
fi

IMAGE="$1"

# Check if image already exists
if [ -f "$IMAGE" ]; then
    echo -e "${YELLOW}Warning: $IMAGE already exists!${NC}"
    read -p "Overwrite? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${RED}Aborted.${NC}"
        exit 1
    fi
    rm -f "$IMAGE"
fi

# Check dependencies based on OS
echo -e "${CYAN}[1/10]${NC} Checking dependencies..."
MISSING=()

if [ "$OS" = "Linux" ]; then
    DEPS=("dd" "sgdisk" "mkfs.fat" "mkfs.ext4" "mkexfatfs" "losetup")
    for cmd in "${DEPS[@]}"; do
        if ! command -v "$cmd" &> /dev/null; then
            MISSING+=("$cmd")
        fi
    done

    if [ ${#MISSING[@]} -ne 0 ]; then
        echo -e "${RED}Error: Missing dependencies: ${MISSING[*]}${NC}"
        echo -e "${YELLOW}Install them with:${NC}"
        echo -e "  Ubuntu/Debian: sudo apt install gdisk dosfstools e2fsprogs exfatprogs"
        echo -e "  Fedora/RHEL:   sudo dnf install gdisk dosfstools e2fsprogs exfatprogs"
        echo -e "  Arch:          sudo pacman -S gptfdisk dosfstools e2fsprogs exfatprogs"
        exit 1
    fi
    MKFS_EXT4="mkfs.ext4"

elif [ "$OS" = "macOS" ]; then
    DEPS=("dd" "sgdisk" "newfs_msdos" "newfs_exfat" "hdiutil")
    BREW_DEPS=()

    # Check for sgdisk (from gptfdisk)
    if ! command -v sgdisk &> /dev/null; then
        MISSING+=("sgdisk")
        BREW_DEPS+=("gptfdisk")
    fi

    # Check for native macOS tools
    for cmd in "dd" "newfs_msdos" "newfs_exfat" "hdiutil" "diskutil"; do
        if ! command -v "$cmd" &> /dev/null; then
            MISSING+=("$cmd")
        fi
    done

    # Check for Homebrew e2fsprogs (EXT4 support)
    MKFS_EXT4=""

    if command -v brew &> /dev/null; then
        # Check if e2fsprogs is installed
        if brew list e2fsprogs &> /dev/null 2>&1; then
            # Get the installation prefix
            E2FSPROGS_PREFIX=$(brew --prefix e2fsprogs 2>/dev/null)

            if [ -n "$E2FSPROGS_PREFIX" ]; then
                # Check all possible locations
                if [ -f "$E2FSPROGS_PREFIX/sbin/mkfs.ext4" ]; then
                    MKFS_EXT4="$E2FSPROGS_PREFIX/sbin/mkfs.ext4"
                    echo -e "${GREEN}${NC} Found e2fsprogs: $MKFS_EXT4"
                elif [ -f "$E2FSPROGS_PREFIX/bin/mkfs.ext4" ]; then
                    MKFS_EXT4="$E2FSPROGS_PREFIX/bin/mkfs.ext4"
                    echo -e "${GREEN}${NC} Found e2fsprogs: $MKFS_EXT4"
                fi
            fi
        fi

        # Fallback: check if mkfs.ext4 is in PATH
        if [ -z "$MKFS_EXT4" ] && command -v mkfs.ext4 &> /dev/null; then
            MKFS_EXT4="mkfs.ext4"
            echo -e "${GREEN}${NC} Found mkfs.ext4 in PATH"
        fi

        # If still not found, show installation message
        if [ -z "$MKFS_EXT4" ]; then
            echo -e "${YELLOW}Note: e2fsprogs not found (EXT4 support will be limited)${NC}"
            echo -e "${YELLOW}      Install with: ${GREEN}brew install e2fsprogs${NC}"
            echo -e "${YELLOW}      Then run: ${GREEN}brew link e2fsprogs${NC}"
            BREW_DEPS+=("e2fsprogs")
        fi
    else
        echo -e "${YELLOW}Note: Homebrew not found - EXT4 support unavailable${NC}"
    fi

    if [ ${#MISSING[@]} -ne 0 ]; then
        echo -e "${RED}Error: Missing dependencies: ${MISSING[*]}${NC}"
        echo -e "${YELLOW}Install with Homebrew:${NC}"
        if [ ${#BREW_DEPS[@]} -ne 0 ]; then
            echo -e "  brew install ${BREW_DEPS[*]}"
        fi
        exit 1
    fi
else
    echo -e "${RED}Error: Unsupported OS: $OS_TYPE${NC}"
    exit 1
fi

if [ -z "$MKFS_EXT4" ]; then
    echo -e "${YELLOW}Warning: ${NC} EXT4 formatting will be skipped (e2fsprogs not available)"
else
    echo -e "${GREEN}${NC} All dependencies found (including EXT4 support)"
fi

# Disk size: 6 GiB
DISK_SIZE=$((6 * 1024 * 1024 * 1024))

# Create sparse image
echo -e "${CYAN}[2/10]${NC} Creating 6 GiB disk image..."
dd if=/dev/zero of="$IMAGE" bs=1M count=0 seek=6144 status=none 2>/dev/null || \
    dd if=/dev/zero of="$IMAGE" bs=1m count=0 seek=6144 2>/dev/null
echo -e "${GREEN}${NC} Created $IMAGE (6 GiB)"

# Create GPT partition table
echo -e "${CYAN}[3/10]${NC} Creating GPT partition table..."
sgdisk -Z "$IMAGE" > /dev/null 2>&1
sgdisk -o "$IMAGE" > /dev/null 2>&1
echo -e "${GREEN}${NC} GPT initialized"

# Create partitions
echo -e "${CYAN}[4/10]${NC} Creating partitions..."

if [ $ODROID_MODE -eq 1 ]; then
    # ODROID mode: Reserve space for U-Boot (before partition table)
    # U-Boot is at sector 512 (256 KiB), reserve 16 MiB total
    # Then create partitions starting at 32 MiB to be safe

    echo -e "${MAGENTA}  ODROID: Reserving 32 MiB for U-Boot${NC}"

    # P1: EFI (FAT32, 100 MiB) - starts at 32 MiB
    sgdisk -n 1:65536:+100M -t 1:EF00 -c 1:"EFI" "$IMAGE" > /dev/null 2>&1
    echo -e "${GREEN}  ${NC} P1: EFI (FAT32, 100 MiB) @ 32 MiB offset"
else
    # Standard mode: Normal partition layout
    # P1: EFI (FAT32, 100 MiB)
    sgdisk -n 1:2048:+100M -t 1:EF00 -c 1:"EFI" "$IMAGE" > /dev/null 2>&1
    echo -e "${GREEN}  ${NC} P1: EFI (FAT32, 100 MiB)"
fi

# P2: OS2ROOT (EXT4, 4 GiB)
sgdisk -n 2:0:+4G -t 2:8300 -c 2:"OS2ROOT" "$IMAGE" > /dev/null 2>&1
echo -e "${GREEN}  ${NC} P2: OS2ROOT (EXT4, 4 GiB)"

# P3: FAT16DATA (FAT16, 512 MiB)
sgdisk -n 3:0:+512M -t 3:0700 -c 3:"FAT16DATA" "$IMAGE" > /dev/null 2>&1
echo -e "${GREEN}  ${NC} P3: FAT16DATA (FAT16, 512 MiB)"

# P4: EXFATUSB (exFAT, remaining space)
sgdisk -n 4:0:0 -t 4:0700 -c 4:"EXFATUSB" "$IMAGE" > /dev/null 2>&1
echo -e "${GREEN}  ${NC} P4: EXFATUSB (exFAT, remaining)"

# Print partition table
echo -e "${CYAN}[5/10]${NC} Partition table:"
sgdisk -p "$IMAGE" 2>/dev/null | grep -E "^   [0-9]" || true

# Set up disk device (OS-specific)
echo -e "${CYAN}[6/10]${NC} Setting up disk device..."

# Check if we need sudo
if [ "$EUID" -ne 0 ]; then
    SUDO="sudo"
else
    SUDO=""
fi

if [ "$OS" = "Linux" ]; then
    # Linux: use losetup
    LOOP=$($SUDO losetup -fP --show "$IMAGE")
    if [ -z "$LOOP" ]; then
        echo -e "${RED}Error: Failed to create loop device${NC}"
        exit 1
    fi
    echo -e "${GREEN}${NC} Loop device: $LOOP"

    # Ensure partitions are detected
    sleep 1
    $SUDO partprobe "$LOOP" 2>/dev/null || true
    sleep 1

    # Partition naming: /dev/loop0p1, /dev/loop0p2, etc.
    PART1="${LOOP}p1"
    PART2="${LOOP}p2"
    PART3="${LOOP}p3"
    PART4="${LOOP}p4"

elif [ "$OS" = "macOS" ]; then
    # macOS: use hdiutil
    DISK_DEV=$($SUDO hdiutil attach -nomount "$IMAGE" | head -n1 | awk '{print $1}')
    if [ -z "$DISK_DEV" ]; then
        echo -e "${RED}Error: Failed to attach disk image${NC}"
        exit 1
    fi
    echo -e "${GREEN}${NC} Disk device: $DISK_DEV"

    # Wait for partitions to appear
    sleep 2

    # Partition naming: /dev/disk3s1, /dev/disk3s2, etc.
    PART1="${DISK_DEV}s1"
    PART2="${DISK_DEV}s2"
    PART3="${DISK_DEV}s3"
    PART4="${DISK_DEV}s4"
fi

# Format partitions (OS-specific)
echo -e "${CYAN}[7/10]${NC} Formatting P1: EFI (FAT32)..."
if [ -e "$PART1" ]; then
    if [ "$OS" = "Linux" ]; then
        $SUDO mkfs.fat -F 32 -n EFI "$PART1" > /dev/null 2>&1
    elif [ "$OS" = "macOS" ]; then
        $SUDO newfs_msdos -F 32 -v EFI "$PART1" > /dev/null 2>&1
    fi
    echo -e "${GREEN}${NC} FAT32 filesystem created"
else
    echo -e "${RED}Error: $PART1 not found${NC}"
fi

echo -e "${CYAN}[8/10]${NC} Formatting P2: OS2ROOT (EXT4)..."
if [ -e "$PART2" ]; then
    if [ -n "$MKFS_EXT4" ]; then
        # EXT4 support available
        $SUDO "$MKFS_EXT4" -q -L OS2ROOT -O ^has_journal "$PART2" 2>&1 | grep -v "^mke2fs" || true
        echo -e "${GREEN}${NC} EXT4 filesystem created"
    else
        # No EXT4 support
        echo -e "${YELLOW}Warning: ${NC} EXT4 skipped (install e2fsprogs: brew install e2fsprogs)"
        echo -e "${YELLOW}  You can format this partition later on Linux${NC}"
    fi
else
    echo -e "${RED}Error: $PART2 not found${NC}"
fi

echo -e "${CYAN}[9/10]${NC} Formatting P3: FAT16DATA (FAT16)..."
if [ -e "$PART3" ]; then
    if [ "$OS" = "Linux" ]; then
        $SUDO mkfs.fat -F 16 -n FAT16DATA "$PART3" > /dev/null 2>&1
    elif [ "$OS" = "macOS" ]; then
        $SUDO newfs_msdos -F 16 -v FAT16DATA "$PART3" > /dev/null 2>&1
    fi
    echo -e "${GREEN}${NC} FAT16 filesystem created"
else
    echo -e "${RED}Error: $PART3 not found${NC}"
fi

echo -e "${CYAN}[10/10]${NC} Formatting P4: EXFATUSB (exFAT)..."
if [ -e "$PART4" ]; then
    if [ "$OS" = "Linux" ]; then
        $SUDO mkexfatfs -n EXFATUSB "$PART4" > /dev/null 2>&1
    elif [ "$OS" = "macOS" ]; then
        $SUDO newfs_exfat -v EXFATUSB "$PART4" > /dev/null 2>&1
    fi
    echo -e "${GREEN}${NC} exFAT filesystem created"
else
    echo -e "${RED}Error: $PART4 not found${NC}"
fi

# Copy bootloader and kernel (if they exist)
if [ -f "build/bootloader/BOOTAA64.EFI" ] || [ -f "bootloader/BOOTAA64.EFI" ]; then
    echo -e "${CYAN}[BONUS]${NC} Installing bootloader..."
    MOUNT_POINT=$(mktemp -d)

    if [ "$OS" = "Linux" ]; then
        $SUDO mount "$PART1" "$MOUNT_POINT"
    elif [ "$OS" = "macOS" ]; then
        $SUDO mount -t msdos "$PART1" "$MOUNT_POINT"
    fi

    $SUDO mkdir -p "$MOUNT_POINT/EFI/BOOT"

    if [ -f "build/bootloader/BOOTAA64.EFI" ]; then
        $SUDO cp build/bootloader/BOOTAA64.EFI "$MOUNT_POINT/EFI/BOOT/"
        echo -e "${GREEN}  ${NC} Copied BOOTAA64.EFI"
    elif [ -f "bootloader/BOOTAA64.EFI" ]; then
        $SUDO cp bootloader/BOOTAA64.EFI "$MOUNT_POINT/EFI/BOOT/"
        echo -e "${GREEN}  ${NC} Copied BOOTAA64.EFI"
    fi

    if [ -f "build/kernel/kernel8.img" ]; then
        $SUDO cp build/kernel/kernel8.img "$MOUNT_POINT/"
        echo -e "${GREEN}  ${NC} Copied kernel8.img"
    elif [ -f "kernel/kernel8.img" ]; then
        $SUDO cp kernel/kernel8.img "$MOUNT_POINT/"
        echo -e "${GREEN}  ${NC} Copied kernel8.img"
    fi

    $SUDO umount "$MOUNT_POINT"
    rmdir "$MOUNT_POINT"
fi

# ODROID: Install U-Boot if available
if [ $ODROID_MODE -eq 1 ] && [ "$OS" = "Linux" ]; then
    # Check for ODROID U-Boot files
    if [ -f "boot/u-boot-odroid-n2.bin" ] || [ -f "/boot/u-boot.bin" ]; then
        echo -e "${MAGENTA}[ODROID]${NC} Installing U-Boot bootloader..."

        UBOOT_FILE=""
        if [ -f "boot/u-boot-odroid-n2.bin" ]; then
            UBOOT_FILE="boot/u-boot-odroid-n2.bin"
        elif [ -f "/boot/u-boot.bin" ]; then
            UBOOT_FILE="/boot/u-boot.bin"
        fi

        if [ -n "$UBOOT_FILE" ]; then
            # Write U-Boot to sector 512 (256 KiB offset) for Amlogic
            $SUDO dd if="$UBOOT_FILE" of="$IMAGE" bs=512 seek=512 conv=notrunc status=none
            echo -e "${MAGENTA}  ${NC} U-Boot written to sector 512"
        fi
    else
        echo -e "${YELLOW}Note: U-Boot not found. You may need to install it manually${NC}"
        echo -e "${YELLOW}      for ODROID: sudo dd if=/boot/u-boot.bin of=$IMAGE bs=512 seek=512 conv=notrunc${NC}"
    fi
fi

# Cleanup (OS-specific)
echo -e "${CYAN}[CLEANUP]${NC} Detaching disk device..."
if [ "$OS" = "Linux" ]; then
    $SUDO losetup -d "$LOOP"
    echo -e "${GREEN}${NC} Loop device detached"
elif [ "$OS" = "macOS" ]; then
    $SUDO hdiutil detach "$DISK_DEV" > /dev/null 2>&1
    echo -e "${GREEN}${NC} Disk device detached"
fi

# Final summary
echo
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}Disk image created successfully!${NC}"
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
echo
echo -e "${CYAN}Image:${NC}      $IMAGE"
echo -e "${CYAN}Size:${NC}       6 GiB (6,442,450,944 bytes)"
echo -e "${CYAN}Format:${NC}     GPT"
echo -e "${CYAN}Platform:${NC}   $OS"
if [ $ODROID_MODE -eq 1 ]; then
    echo -e "${MAGENTA}Mode:${NC}       ODROID (U-Boot compatible)"
fi
if [ -n "$MKFS_EXT4" ]; then
    echo -e "${GREEN}EXT4:${NC}       Formatted successfully"
else
    echo -e "${YELLOW}EXT4:${NC}       Skipped (install e2fsprogs for EXT4 support)"
fi
echo
echo -e "${CYAN}Partitions:${NC}"
if [ $ODROID_MODE -eq 1 ]; then
    echo -e "  ${MAGENTA}U-Boot:${NC} Reserved (16 MiB)     → Bootloader area"
fi
echo -e "  ${GREEN}P1:${NC} EFI       (FAT32,  100 MiB)  → /boot/efi"
if [ -n "$MKFS_EXT4" ]; then
    echo -e "  ${GREEN}P2:${NC} OS2ROOT   (EXT4,   4 GiB)    → / ${GREEN}[formatted]${NC}"
else
    echo -e "  ${YELLOW}P2:${NC} OS2ROOT   (EXT4,   4 GiB)    → / ${YELLOW}[not formatted]${NC}"
fi
echo -e "  ${GREEN}P3:${NC} FAT16DATA (FAT16,  512 MiB)  → /mnt/fat16"
echo -e "  ${GREEN}P4:${NC} EXFATUSB  (exFAT,  remaining) → /mnt/exfat"
echo
echo -e "${CYAN}Next steps:${NC}"
if [ -z "$MKFS_EXT4" ]; then
    echo -e "  ${GREEN}0. Install EXT4: brew install e2fsprogs${NC}"
fi
echo -e "  1. Build OS/2 Warp: ${YELLOW}cmake --build build -j\$(nproc)${NC}"
echo -e "  2. Run in QEMU:     ${YELLOW}./scripts/run_qemu.sh $IMAGE${NC}"
if [ "$OS" = "Linux" ]; then
    echo -e "  3. Or write to SD:  ${YELLOW}sudo dd if=$IMAGE of=/dev/sdX bs=4M status=progress${NC}"
    if [ $ODROID_MODE -eq 1 ]; then
        echo -e "     ${MAGENTA}ODROID eMMC:${NC}     ${YELLOW}sudo dd if=$IMAGE of=/dev/mmcblk1 bs=4M status=progress${NC}"
    fi
elif [ "$OS" = "macOS" ]; then
    echo -e "  3. Or write to SD:  ${YELLOW}sudo dd if=$IMAGE of=/dev/rdiskX bs=4m${NC}"
fi
if [ -n "$ODROID_BOARD" ]; then
    echo
    echo -e "${MAGENTA}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${MAGENTA}  ODROID Hardware Detected: $ODROID_BOARD${NC}"
    echo -e "${MAGENTA}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${MAGENTA}To write to eMMC:${NC}"
    echo -e "  ${YELLOW}sudo dd if=$IMAGE of=/dev/mmcblk1 bs=4M status=progress${NC}"
    echo -e "${MAGENTA}To write to SD card:${NC}"
    echo -e "  ${YELLOW}sudo dd if=$IMAGE of=/dev/mmcblk0 bs=4M status=progress${NC}"
fi
echo
echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"