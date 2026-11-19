#!/bin/bash
# run_qemu.sh — OS/2 Warp 4.52 ARM64 QEMU Launcher
#
# Launches OS/2 Warp in QEMU AArch64 virtual machine
# Compatible with Linux, macOS, and ODROID devices

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
MAGENTA='\033[0;35m'
NC='\033[0m'

# Detect OS
OS_TYPE="$(uname -s)"
case "$OS_TYPE" in
    Linux*)     OS="Linux";;
    Darwin*)    OS="macOS";;
    *)          OS="Unknown";;
esac

# Detect host architecture
HOST_ARCH="$(uname -m)"

# Detect if running on ODROID
ODROID_BOARD=""
ODROID_SOC=""
if [ "$OS" = "Linux" ] && [ -f /proc/device-tree/model ]; then
    BOARD_MODEL=$(cat /proc/device-tree/model 2>/dev/null | tr -d '\0')
    case "$BOARD_MODEL" in
        *"ODROID-N2 Plus"*)
            ODROID_BOARD="ODROID-N2+"
            ODROID_SOC="Amlogic S922X (4×A73 + 2×A53)"
            ;;
        *"ODROID-N2"*)
            ODROID_BOARD="ODROID-N2"
            ODROID_SOC="Amlogic S922X (4×A73 + 2×A53)"
            ;;
        *"ODROID-C4"*)
            ODROID_BOARD="ODROID-C4"
            ODROID_SOC="Amlogic S905X3 (4×A55)"
            ;;
        *"ODROID-M1"*)
            ODROID_BOARD="ODROID-M1"
            ODROID_SOC="Rockchip RK3568 (4×A55)"
            ;;
        *"ODROID-XU4"*)
            ODROID_BOARD="ODROID-XU4"
            ODROID_SOC="Exynos 5422 (4×A15 + 4×A7)"
            ;;
        *"ODROID"*)
            ODROID_BOARD="ODROID (Unknown model)"
            ODROID_SOC="Unknown"
            ;;
    esac
fi

# Print header
echo -e "${CYAN}  OS/2 Warp 4.52 ARM64 — QEMU Launcher${NC}"
echo -e "${CYAN}  Platform: $OS ($HOST_ARCH)${NC}"
if [ -n "$ODROID_BOARD" ]; then
    echo -e "${MAGENTA}  Hardware: $ODROID_BOARD${NC}"
    echo -e "${MAGENTA}  SoC:      $ODROID_SOC${NC}"
fi
echo

# Check arguments
if [ $# -lt 1 ]; then
    echo -e "${RED}Usage: $0 <disk_image> [options]${NC}"
    echo
    echo -e "${CYAN}Options:${NC}"
    echo -e "  -m, --memory <size>    Memory size (default: 2G)"
    echo -e "  -c, --cpus <count>     CPU count (default: 4)"
    echo -e "  -g, --gdb              Enable GDB server on port 1234"
    echo -e "  -d, --debug            Enable QEMU debug output"
    echo -e "  -n, --no-display       Run headless (no graphics)"
    echo -e "  -s, --serial           Serial output only (no graphics)"
    echo -e "  -a, --accel <type>     Accelerator: hvf (macOS), kvm (Linux), tcg (none)"
    echo -e "  --cpu <model>          CPU model (default: auto-detect)"
    echo
    echo -e "${YELLOW}Example:${NC}"
    echo -e "  $0 os2warp452.img"
    echo -e "  $0 os2warp452.img -m 4G -c 8"
    echo -e "  $0 os2warp452.img --gdb"
    if [ "$OS" = "macOS" ]; then
        echo -e "  $0 os2warp452.img --accel hvf  ${CYAN}# Use macOS Hypervisor (faster)${NC}"
    elif [ "$OS" = "Linux" ]; then
        echo -e "  $0 os2warp452.img --accel kvm  ${CYAN}# Use KVM (faster)${NC}"
    fi
    if [ -n "$ODROID_BOARD" ]; then
        echo
        echo -e "${MAGENTA}ODROID detected: Recommended settings${NC}"
        case "$ODROID_BOARD" in
            "ODROID-N2"|"ODROID-N2+")
                echo -e "  $0 os2warp452.img -m 4G -c 6 --accel kvm  ${MAGENTA}# N2/N2+ (6 cores, 4GB RAM)${NC}"
                ;;
            "ODROID-C4")
                echo -e "  $0 os2warp452.img -m 4G -c 4 --accel kvm  ${MAGENTA}# C4 (4 cores, 4GB RAM)${NC}"
                ;;
            "ODROID-M1")
                echo -e "  $0 os2warp452.img -m 4G -c 4 --accel kvm  ${MAGENTA}# M1 (4 cores, up to 8GB RAM)${NC}"
                ;;
        esac
    fi
    exit 1
fi

IMAGE="$1"
shift

# Check if image exists
if [ ! -f "$IMAGE" ]; then
    echo -e "${RED}Error: Disk image not found: $IMAGE${NC}"
    echo -e "${YELLOW}Create it with: ./scripts/mkdisk.sh $IMAGE${NC}"
    if [ -n "$ODROID_BOARD" ]; then
        echo -e "${MAGENTA}For ODROID: ./scripts/mkdisk.sh $IMAGE --odroid${NC}"
    fi
    exit 1
fi

# Default options (adjust for ODROID)
if [ -n "$ODROID_BOARD" ]; then
    # ODROID defaults: more RAM, match CPU count to hardware
    case "$ODROID_BOARD" in
        "ODROID-N2"|"ODROID-N2+")
            MEMORY="4G"
            CPUS="6"  # 4×A73 + 2×A53
            ;;
        "ODROID-C4")
            MEMORY="4G"
            CPUS="4"  # 4×A55
            ;;
        "ODROID-M1")
            MEMORY="4G"
            CPUS="4"  # 4×A55
            ;;
        *)
            MEMORY="2G"
            CPUS="4"
            ;;
    esac
else
    MEMORY="2G"
    CPUS="4"
fi

GDB_OPT=""
DEBUG_OPT=""
DISPLAY_OPT="-device virtio-gpu-device -device ramfb"
SERIAL_MODE=""
ACCEL=""
ACCEL_OPT=""
CPU_MODEL=""

# Parse options
while [ $# -gt 0 ]; do
    case "$1" in
        -m|--memory)
            MEMORY="$2"
            shift 2
            ;;
        -c|--cpus)
            CPUS="$2"
            shift 2
            ;;
        -g|--gdb)
            GDB_OPT="-s -S"
            shift
            ;;
        -d|--debug)
            DEBUG_OPT="-d int,cpu_reset"
            shift
            ;;
        -n|--no-display)
            DISPLAY_OPT="-nographic"
            shift
            ;;
        -s|--serial)
            SERIAL_MODE="-serial stdio"
            DISPLAY_OPT=""
            shift
            ;;
        -a|--accel)
            ACCEL="$2"
            shift 2
            ;;
        --cpu)
            CPU_MODEL="$2"
            shift 2
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Check for QEMU
echo -e "${CYAN}[1/5]${NC} Checking for QEMU..."
if ! command -v qemu-system-aarch64 &> /dev/null; then
    echo -e "${RED}Error: qemu-system-aarch64 not found${NC}"
    echo -e "${YELLOW}Install it with:${NC}"
    if [ "$OS" = "macOS" ]; then
        echo -e "  macOS (Homebrew): brew install qemu"
    elif [ -n "$ODROID_BOARD" ]; then
        echo -e "  ODROID/Ubuntu:    sudo apt install qemu-system-arm qemu-efi-aarch64"
    else
        echo -e "  Ubuntu/Debian:    sudo apt install qemu-system-arm"
        echo -e "  Fedora/RHEL:      sudo dnf install qemu-system-aarch64"
        echo -e "  Arch:             sudo pacman -S qemu-system-aarch64"
    fi
    exit 1
fi

QEMU_VERSION=$(qemu-system-aarch64 --version | head -n1)
echo -e "${GREEN}${NC} QEMU found: $QEMU_VERSION"

# Determine accelerator
echo -e "${CYAN}[2/5]${NC} Checking acceleration support..."
if [ -z "$ACCEL" ]; then
    # Auto-detect best accelerator
    if [ "$OS" = "macOS" ]; then
        # Check if HVF is available (macOS 10.13+, native on Apple Silicon)
        if qemu-system-aarch64 -accel help 2>&1 | grep -q hvf; then
            ACCEL="hvf"
            ACCEL_OPT="-accel hvf"
            echo -e "${GREEN}${NC} Using macOS Hypervisor.framework (HVF) acceleration"
        else
            ACCEL="tcg"
            echo -e "${YELLOW}Warning: ${NC} HVF not available, using TCG (software emulation)"
        fi
    elif [ "$OS" = "Linux" ]; then
        # Check if KVM is available
        if [ -e /dev/kvm ] && qemu-system-aarch64 -accel help 2>&1 | grep -q kvm; then
            ACCEL="kvm"
            ACCEL_OPT="-accel kvm"
            echo -e "${GREEN}${NC} Using KVM acceleration"
            if [ -n "$ODROID_BOARD" ]; then
                echo -e "${MAGENTA}  ODROID: Native ARM64 + KVM = excellent performance!${NC}"
            fi
        else
            ACCEL="tcg"
            if [ ! -e /dev/kvm ]; then
                echo -e "${YELLOW}Warning: ${NC} /dev/kvm not found, using TCG (software emulation)"
                if [ -n "$ODROID_BOARD" ]; then
                    echo -e "${MAGENTA}  ODROID: To enable KVM: sudo modprobe kvm${NC}"
                else
                    echo -e "${YELLOW}  To enable KVM: modprobe kvm kvm_intel (or kvm_amd)${NC}"
                fi
            else
                echo -e "${YELLOW}Warning: ${NC} KVM not available, using TCG (software emulation)"
            fi
        fi
    else
        ACCEL="tcg"
        echo -e "${YELLOW}Warning: ${NC} Unknown OS, using TCG (software emulation)"
    fi
else
    # User specified accelerator
    case "$ACCEL" in
        hvf)
            if [ "$OS" != "macOS" ]; then
                echo -e "${RED}Error: HVF is only available on macOS${NC}"
                exit 1
            fi
            ACCEL_OPT="-accel hvf"
            echo -e "${GREEN}${NC} Using macOS Hypervisor.framework (HVF) acceleration"
            ;;
        kvm)
            if [ "$OS" != "Linux" ]; then
                echo -e "${RED}Error: KVM is only available on Linux${NC}"
                exit 1
            fi
            if [ ! -e /dev/kvm ]; then
                echo -e "${RED}Error: /dev/kvm not found${NC}"
                if [ -n "$ODROID_BOARD" ]; then
                    echo -e "${MAGENTA}Enable KVM on ODROID: sudo modprobe kvm${NC}"
                else
                    echo -e "${YELLOW}Enable KVM: sudo modprobe kvm kvm_intel (or kvm_amd)${NC}"
                fi
                exit 1
            fi
            ACCEL_OPT="-accel kvm"
            echo -e "${GREEN}${NC} Using KVM acceleration"
            ;;
        tcg)
            ACCEL_OPT=""
            echo -e "${YELLOW}Warning: ${NC} Using TCG (software emulation, slower)"
            ;;
        *)
            echo -e "${RED}Error: Unknown accelerator: $ACCEL${NC}"
            echo -e "${YELLOW}Valid options: hvf (macOS), kvm (Linux), tcg (software)${NC}"
            exit 1
            ;;
    esac
fi

# Determine CPU model if not specified
if [ -z "$CPU_MODEL" ]; then
    # Auto-detect best CPU model
    if [ "$HOST_ARCH" = "arm64" ] || [ "$HOST_ARCH" = "aarch64" ]; then
        # ARM64 host
        if [ "$ACCEL" = "hvf" ] || [ "$ACCEL" = "kvm" ]; then
            # Use 'max' with hardware acceleration
            CPU_MODEL="max"
            echo -e "${GREEN}${NC} CPU model: max (ARM64 native with $ACCEL)"
        else
            # TCG on ARM64 host
            CPU_MODEL="cortex-a57"
            echo -e "${GREEN}${NC} CPU model: cortex-a57 (ARM64 TCG)"
        fi
    else
        # x86 or other host - use compatible ARM model
        CPU_MODEL="cortex-a57"
        echo -e "${GREEN}${NC} CPU model: cortex-a57 (cross-architecture emulation)"
    fi
else
    echo -e "${GREEN}${NC} CPU model: $CPU_MODEL (user-specified)"
fi

# Find UEFI firmware
echo -e "${CYAN}[3/5]${NC} Looking for UEFI firmware..."
UEFI_PATHS=(
    # Linux paths
    "/usr/share/AAVMF/AAVMF_CODE.fd"
    "/usr/share/qemu-efi-aarch64/QEMU_EFI.fd"
    "/usr/share/edk2/aarch64/QEMU_EFI.fd"
    "/usr/share/edk2-arm/aarch64/QEMU_EFI.fd"
    "/usr/share/edk2-armvirt/aarch64/QEMU_EFI.fd"
    # macOS paths (Homebrew)
    "/opt/homebrew/share/qemu/edk2-aarch64-code.fd"
    "/opt/homebrew/Cellar/qemu/*/share/qemu/edk2-aarch64-code.fd"
    "/usr/local/share/qemu/edk2-aarch64-code.fd"
    "/usr/local/Cellar/qemu/*/share/qemu/edk2-aarch64-code.fd"
    # Current directory
    "AAVMF_CODE.fd"
    "QEMU_EFI.fd"
    "edk2-aarch64-code.fd"
)

UEFI_FW=""
for path in "${UEFI_PATHS[@]}"; do
    # Handle glob patterns
    for file in $path; do
        if [ -f "$file" ]; then
            UEFI_FW="$file"
            break 2
        fi
    done
done

if [ -z "$UEFI_FW" ]; then
    echo -e "${RED}Error: UEFI firmware not found${NC}"
    echo -e "${YELLOW}Install it with:${NC}"
    if [ "$OS" = "macOS" ]; then
        echo -e "  macOS: UEFI firmware is included with QEMU from Homebrew"
        echo -e "         If missing, reinstall: brew reinstall qemu"
        echo -e "         Check: ls /opt/homebrew/share/qemu/edk2-*.fd"
    elif [ -n "$ODROID_BOARD" ]; then
        echo -e "  ODROID/Ubuntu: sudo apt install qemu-efi-aarch64"
    else
        echo -e "  Ubuntu/Debian: sudo apt install qemu-efi-aarch64"
        echo -e "  Fedora/RHEL:   sudo dnf install edk2-aarch64"
        echo -e "  Arch:          sudo pacman -S edk2-aarch64"
    fi
    exit 1
fi
echo -e "${GREEN}${NC} UEFI firmware: $UEFI_FW"

# Display configuration
echo -e "${CYAN}[4/5]${NC} Configuration:"
echo -e "  ${CYAN}Platform:${NC}   $OS ($HOST_ARCH)"
if [ -n "$ODROID_BOARD" ]; then
    echo -e "  ${MAGENTA}Hardware:${NC}   $ODROID_BOARD"
fi
echo -e "  ${CYAN}Disk:${NC}       $IMAGE"
echo -e "  ${CYAN}Memory:${NC}     $MEMORY"
echo -e "  ${CYAN}CPUs:${NC}       $CPUS"
echo -e "  ${CYAN}Machine:${NC}    virt,gic-version=3"
echo -e "  ${CYAN}CPU:${NC}        $CPU_MODEL"
echo -e "  ${CYAN}Accel:${NC}      $ACCEL"

if [ -n "$GDB_OPT" ]; then
    echo -e "  ${YELLOW}GDB:${NC}        Enabled on port 1234 (waiting for connection)"
fi

if [ -n "$DEBUG_OPT" ]; then
    echo -e "  ${YELLOW}Debug:${NC}      Enabled"
fi

# Launch QEMU
echo -e "${CYAN}[5/5]${NC} Launching QEMU..."
echo

if [ -n "$GDB_OPT" ]; then
    echo -e "${YELLOW}  GDB mode enabled - waiting for debugger connection${NC}"
    echo -e "${YELLOW}  Connect with: aarch64-elf-gdb build/kernel/kernel_elf${NC}"
    echo -e "${YELLOW}                (gdb) target remote :1234${NC}"
    echo -e "${YELLOW}                (gdb) continue${NC}"
    echo
fi

# Build QEMU command
QEMU_CMD=(
    qemu-system-aarch64
    -machine virt,gic-version=3
    -cpu "$CPU_MODEL"
    -m "$MEMORY"
    -smp "$CPUS"
    -bios "$UEFI_FW"
    -drive if=none,id=hd0,file="$IMAGE",format=raw
    -device virtio-blk-pci,drive=hd0
    -serial stdio
)

# Add accelerator if available
if [ -n "$ACCEL_OPT" ]; then
    QEMU_CMD+=($ACCEL_OPT)
fi

# Add optional features
if [ -n "$DISPLAY_OPT" ]; then
    QEMU_CMD+=($DISPLAY_OPT)
fi

if [ -n "$GDB_OPT" ]; then
    QEMU_CMD+=($GDB_OPT)
fi

if [ -n "$DEBUG_OPT" ]; then
    QEMU_CMD+=($DEBUG_OPT)
fi

if [ -n "$SERIAL_MODE" ]; then
    QEMU_CMD+=($SERIAL_MODE)
fi

# macOS-specific: Add USB support for better compatibility
if [ "$OS" = "macOS" ]; then
    QEMU_CMD+=(-device qemu-xhci -device usb-kbd -device usb-mouse)
fi

# Execute
exec "${QEMU_CMD[@]}"