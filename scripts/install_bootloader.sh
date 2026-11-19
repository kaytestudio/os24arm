#!/bin/bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

if [ $# -ne 2 ]; then
    echo -e "${RED}Usage: $0 <disk_image> <bootloader_efi>${NC}"
    echo -e "${YELLOW}Example: $0 os2warp452.img build/bootloader/BOOTAA64.EFI${NC}"
    exit 1
fi

IMAGE="$1"
BOOTLOADER="$2"

if [ ! -f "$IMAGE" ]; then
    echo -e "${RED}Error: Disk image not found: $IMAGE${NC}"
    exit 1
fi

if [ ! -f "$BOOTLOADER" ]; then
    echo -e "${RED}Error: Bootloader not found: $BOOTLOADER${NC}"
    echo -e "${YELLOW}Build it first with: make${NC}"
    exit 1
fi

OS_TYPE="$(uname -s)"
case "$OS_TYPE" in
    Linux*)     OS="Linux";;
    Darwin*)    OS="macOS";;
    *)          OS="Unknown";;
esac

echo -e "${CYAN}  Installing OS/2 Bootloader to Disk Image${NC}"
echo -e "${CYAN}  Platform: $OS${NC}"
echo

if [ "$EUID" -ne 0 ]; then
    SUDO="sudo"
else
    SUDO=""
fi

MOUNT_POINT=$(mktemp -d)
echo -e "${CYAN}[1/4]${NC} Created temporary mount point: $MOUNT_POINT"

echo -e "${CYAN}[2/4]${NC} Mounting EFI partition (P1)..."

if [ "$OS" = "Linux" ]; then
    # P1 starts at sector 2048, sector size = 512 bytes
    OFFSET=$((2048 * 512))
    $SUDO mount -o loop,offset=$OFFSET "$IMAGE" "$MOUNT_POINT"
    echo -e "${GREEN}${NC} EFI partition mounted"

elif [ "$OS" = "macOS" ]; then
    DISK_DEV=$($SUDO hdiutil attach -nomount "$IMAGE" | head -n1 | awk '{print $1}')
    if [ -z "$DISK_DEV" ]; then
        echo -e "${RED}Error: Failed to attach disk image${NC}"
        rmdir "$MOUNT_POINT"
        exit 1
    fi

    PART="${DISK_DEV}s1"
    $SUDO mount -t msdos "$PART" "$MOUNT_POINT"
    echo -e "${GREEN}${NC} EFI partition mounted"
else
    echo -e "${RED}Error: Unsupported OS: $OS_TYPE${NC}"
    rmdir "$MOUNT_POINT"
    exit 1
fi

echo -e "${CYAN}[3/4]${NC} Creating EFI directory structure..."
$SUDO mkdir -p "$MOUNT_POINT/EFI/BOOT"
echo -e "${GREEN}${NC} Directory created: /EFI/BOOT"

echo -e "${CYAN}[4/4]${NC} Installing BOOTAA64.EFI..."
$SUDO cp "$BOOTLOADER" "$MOUNT_POINT/EFI/BOOT/BOOTAA64.EFI"
$SUDO chmod 755 "$MOUNT_POINT/EFI/BOOT/BOOTAA64.EFI"

$SUDO cp "$BOOTLOADER" "$MOUNT_POINT/BOOTAA64.EFI"

echo -e "${GREEN}${NC} Bootloader installed to:"
echo -e "    /EFI/BOOT/BOOTAA64.EFI"
echo -e "    /BOOTAA64.EFI (fallback)"

echo -e "${CYAN}[CLEANUP]${NC} Syncing and unmounting..."
sync

if [ "$OS" = "Linux" ]; then
    $SUDO umount "$MOUNT_POINT"
elif [ "$OS" = "macOS" ]; then
    $SUDO umount "$MOUNT_POINT"
    $SUDO hdiutil detach "$DISK_DEV" > /dev/null 2>&1
fi

rmdir "$MOUNT_POINT"
echo -e "${GREEN}${NC} Unmounted and cleaned up"

echo
echo -e "${GREEN}Bootloader successfully installed!${NC}"
echo
echo -e "${CYAN}Disk image:${NC}   $IMAGE"
echo -e "${CYAN}Bootloader:${NC}   $BOOTLOADER"
echo -e "${CYAN}Installed to:${NC} /EFI/BOOT/BOOTAA64.EFI"
echo
echo -e "${CYAN}Next step:${NC}"
echo -e "  Run in QEMU: ${YELLOW}make run${NC}"
echo -e "  or:          ${YELLOW}./scripts/run_qemu.sh $IMAGE${NC}"
echo
echo -e "${YELLOW}Note: The bootloader will look for the kernel on the EFI partition.${NC}"
echo -e "${YELLOW}      Make sure to also install the kernel image if needed.${NC}"
echo
