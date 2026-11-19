#!/bin/bash
# install_config.sh — Install CONFIG.SYS to OS/2 Disk Image
#
# Mounts the EXT4 root partition and copies CONFIG.SYS to /etc

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Check arguments
if [ $# -ne 2 ]; then
    echo -e "${RED}Usage: $0 <disk_image> <config.sys>${NC}"
    exit 1
fi

IMAGE="$1"
CONFIG_SYS="$2"

# Check if files exist
if [ ! -f "$IMAGE" ]; then
    echo -e "${RED}Error: Disk image not found: $IMAGE${NC}"
    exit 1
fi

if [ ! -f "$CONFIG_SYS" ]; then
    echo -e "${RED}Error: CONFIG.SYS not found: $CONFIG_SYS${NC}"
    exit 1
fi

# Detect OS
OS_TYPE="$(uname -s)"
case "$OS_TYPE" in
    Linux*)     OS="Linux";;
    Darwin*)    OS="macOS";;
    *)          OS="Unknown";;
esac

echo -e "${CYAN}  Installing CONFIG.SYS to OS/2 Disk Image${NC}"
echo -e "${CYAN}  Platform: $OS${NC}"
echo

# Check for sudo
if [ "$EUID" -ne 0 ]; then
    SUDO="sudo"
else
    SUDO=""
fi

# Create mount point
MOUNT_POINT=$(mktemp -d)
echo -e "${CYAN}[1/4]${NC} Created temporary mount point: $MOUNT_POINT"

# Mount the partition
echo -e "${CYAN}[2/4]${NC} Mounting EXT4 partition (P2)..."

if [ "$OS" = "Linux" ]; then
    # Linux: Mount partition 2 (EXT4 root) at offset 206848 sectors
    # P2 starts at sector 206848, sector size = 512 bytes
    OFFSET=$((206848 * 512))
    $SUDO mount -o loop,offset=$OFFSET "$IMAGE" "$MOUNT_POINT"
    echo -e "${GREEN}${NC} Partition mounted"

elif [ "$OS" = "macOS" ]; then
    # macOS: Use hdiutil
    DISK_DEV=$($SUDO hdiutil attach -nomount "$IMAGE" | head -n1 | awk '{print $1}')
    if [ -z "$DISK_DEV" ]; then
        echo -e "${RED}Error: Failed to attach disk image${NC}"
        rmdir "$MOUNT_POINT"
        exit 1
    fi

    # Mount partition 2 (s2)
    PART="${DISK_DEV}s2"

    # Check if e2fsprogs is available
    if command -v fuse-ext2 &> /dev/null; then
        # Using FUSE-EXT2 for macOS
        fuse-ext2 "$PART" "$MOUNT_POINT" -o rw+
        echo -e "${GREEN}${NC} Partition mounted (via FUSE-EXT2)"
    else
        echo -e "${YELLOW}Warning: ${NC} Cannot mount EXT4 on macOS without fuse-ext2"
        echo -e "${YELLOW}Install with: brew install fuse-ext2${NC}"
        $SUDO hdiutil detach "$DISK_DEV" > /dev/null 2>&1
        rmdir "$MOUNT_POINT"
        exit 1
    fi
else
    echo -e "${RED}Error: Unsupported OS: $OS_TYPE${NC}"
    rmdir "$MOUNT_POINT"
    exit 1
fi

# Create /etc directory if it doesn't exist
echo -e "${CYAN}[3/4]${NC} Creating /etc directory..."
$SUDO mkdir -p "$MOUNT_POINT/etc"
$SUDO mkdir -p "$MOUNT_POINT/os2"
$SUDO mkdir -p "$MOUNT_POINT/os2/system"
echo -e "${GREEN}${NC} Directories created"

# Copy CONFIG.SYS
echo -e "${CYAN}[4/4]${NC} Installing CONFIG.SYS..."
$SUDO cp "$CONFIG_SYS" "$MOUNT_POINT/etc/config.sys"
$SUDO cp "$CONFIG_SYS" "$MOUNT_POINT/config.sys"
$SUDO cp "$CONFIG_SYS" "$MOUNT_POINT/os2/config.sys"

# Set permissions
$SUDO chmod 644 "$MOUNT_POINT/etc/config.sys"
$SUDO chmod 644 "$MOUNT_POINT/config.sys"
$SUDO chmod 644 "$MOUNT_POINT/os2/config.sys"

echo -e "${GREEN}${NC} CONFIG.SYS installed to:"
echo -e "    /etc/config.sys"
echo -e "    /config.sys"
echo -e "    /os2/config.sys"

# Sync and unmount
echo -e "${CYAN}[CLEANUP]${NC} Syncing and unmounting..."
sync

if [ "$OS" = "Linux" ]; then
    $SUDO umount "$MOUNT_POINT"
elif [ "$OS" = "macOS" ]; then
    if command -v fuse-ext2 &> /dev/null; then
        umount "$MOUNT_POINT" 2>/dev/null || $SUDO umount "$MOUNT_POINT"
    fi
    $SUDO hdiutil detach "$DISK_DEV" > /dev/null 2>&1
fi

rmdir "$MOUNT_POINT"
echo -e "${GREEN}${NC} Unmounted and cleaned up"

# Summary
echo
echo -e "${GREEN}CONFIG.SYS successfully installed!${NC}"
echo
echo -e "${CYAN}Disk image:${NC}    $IMAGE"
echo -e "${CYAN}CONFIG.SYS:${NC}    $CONFIG_SYS"
echo -e "${CYAN}Installed to:${NC}  /etc/config.sys, /config.sys, /os2/config.sys"
echo
echo -e "${CYAN}Next step:${NC}"
echo -e "  Run in QEMU:  ${YELLOW}make run${NC}"
echo -e "  or:           ${YELLOW}./scripts/run_qemu.sh $IMAGE${NC}"
echo