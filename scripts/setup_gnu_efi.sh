#!/bin/bash
# setup_gnu_efi.sh — Clone and Build gnu-efi from GitHub
#
# This script:
# - Clones gnu-efi from GitHub
# - Builds it for ARM64
# - Installs it locally in the project

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configuration
GNUEFI_REPO="https://github.com/ncroxon/gnu-efi.git"
GNUEFI_DIR="external/gnu-efi"
INSTALL_PREFIX="$(pwd)/external/gnu-efi-install"

echo -e "${CYAN}  Setting up gnu-efi from GitHub${NC}"
echo

# Check prerequisites
echo -e "${CYAN}[1/6]${NC} Checking prerequisites..."

MISSING=()
if ! command -v git &> /dev/null; then
    MISSING+=("git")
fi
if ! command -v make &> /dev/null; then
    MISSING+=("make")
fi
if ! command -v clang &> /dev/null && ! command -v gcc &> /dev/null; then
    MISSING+=("clang or gcc")
fi

if [ ${#MISSING[@]} -ne 0 ]; then
    echo -e "${RED}Error: Missing tools: ${MISSING[*]}${NC}"
    echo -e "${YELLOW}Install with:${NC}"
    if [ "$(uname -s)" = "Darwin" ]; then
        echo -e "  ${GREEN}brew install git llvm${NC}"
    else
        echo -e "  ${GREEN}sudo apt install git build-essential${NC}"
    fi
    exit 1
fi

echo -e "${GREEN}${NC} All prerequisites found"

# Clone gnu-efi
echo
echo -e "${CYAN}[2/6]${NC} Cloning gnu-efi from GitHub..."

if [ -d "$GNUEFI_DIR" ]; then
    echo -e "${YELLOW}Warning: ${NC} Directory $GNUEFI_DIR already exists"
    read -p "Delete and re-clone? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$GNUEFI_DIR"
    else
        echo -e "${YELLOW}Using existing directory${NC}"
    fi
fi

if [ ! -d "$GNUEFI_DIR" ]; then
    mkdir -p external
    git clone "$GNUEFI_REPO" "$GNUEFI_DIR"
    echo -e "${GREEN}${NC} Cloned gnu-efi"
else
    echo -e "${GREEN}${NC} Using existing gnu-efi"
fi

# Get version
cd "$GNUEFI_DIR"
GNUEFI_VERSION=$(git describe --tags 2>/dev/null || echo "unknown")
echo -e "${GREEN}${NC} Version: $GNUEFI_VERSION"
cd - > /dev/null

# Detect platform and set compiler
echo
echo -e "${CYAN}[3/6]${NC} Detecting platform and setting up compiler..."

OS_TYPE="$(uname -s)"
case "$OS_TYPE" in
    Darwin*)
        OS="macOS"

        # On macOS, we need an ELF cross-compiler, not Mach-O
        echo -e "${YELLOW}Warning: ${NC} macOS detected - checking for ELF cross-compiler..."

        # Try to find aarch64 ELF cross-compiler
        if command -v aarch64-elf-gcc &> /dev/null; then
            CC="aarch64-elf-gcc"
            OBJCOPY="aarch64-elf-objcopy"
            echo -e "${GREEN}${NC} Found aarch64-elf-gcc"
        elif command -v aarch64-linux-gnu-gcc &> /dev/null; then
            CC="aarch64-linux-gnu-gcc"
            OBJCOPY="aarch64-linux-gnu-objcopy"
            echo -e "${GREEN}${NC} Found aarch64-linux-gnu-gcc"
        elif command -v aarch64-unknown-linux-gnu-gcc &> /dev/null; then
            CC="aarch64-unknown-linux-gnu-gcc"
            OBJCOPY="aarch64-unknown-linux-gnu-objcopy"
            echo -e "${GREEN}${NC} Found aarch64-unknown-linux-gnu-gcc"
        else
            echo -e "${RED}Error: ELF cross-compiler not found${NC}"
            echo
            echo -e "${YELLOW}gnu-efi requires an ELF toolchain on macOS (not Mach-O).${NC}"
            echo
            echo -e "${CYAN}Install with (RECOMMENDED - fast):${NC}"
            echo -e "  ${GREEN}brew tap messense/macos-cross-toolchains${NC}"
            echo -e "  ${GREEN}brew install aarch64-unknown-linux-gnu${NC}"
            echo
            echo -e "${CYAN}Or (slower, builds from source):${NC}"
            echo -e "  ${GREEN}brew tap osx-cross/arm${NC}"
            echo -e "  ${GREEN}brew install aarch64-elf-gcc${NC}"
            echo
            echo -e "${CYAN}Then run this script again.${NC}"
            exit 1
        fi
        ;;
    Linux*)
        OS="Linux"
        if command -v clang &> /dev/null; then
            CC=clang
            OBJCOPY=objcopy
        else
            CC=gcc
            OBJCOPY=objcopy
        fi
        ;;
    *)
        echo -e "${RED}Unsupported OS: $OS_TYPE${NC}"
        exit 1
        ;;
esac

echo -e "${GREEN}${NC} Platform: $OS"
echo -e "${GREEN}${NC} Compiler: $CC"
if [ "$OS" = "macOS" ]; then
    echo -e "${GREEN}${NC} Objcopy: $OBJCOPY"
fi

# Build gnu-efi for ARM64
echo
echo -e "${CYAN}[4/6]${NC} Building gnu-efi for ARM64..."

cd "$GNUEFI_DIR"

# Clean previous build
make clean > /dev/null 2>&1 || true

# Build for aarch64 with the correct compiler
echo -e "  Building for aarch64..."
if [ "$OS" = "macOS" ]; then
    # macOS: Use cross-compiler with explicit settings
    make ARCH=aarch64 \
         CC=$CC \
         OBJCOPY=$OBJCOPY \
         CROSS_COMPILE=${CC%-gcc}- \
         -j$(sysctl -n hw.ncpu)
else
    # Linux: Use native compiler
    make ARCH=aarch64 CC=$CC -j$(nproc)
fi

echo -e "${GREEN}${NC} Build complete"

# Install locally
echo
echo -e "${CYAN}[5/6]${NC} Installing to project directory..."

cd - > /dev/null
mkdir -p "$INSTALL_PREFIX"

# Copy headers
echo -e "  Copying headers..."
mkdir -p "$INSTALL_PREFIX/include/efi"
cp -r "$GNUEFI_DIR/inc/"* "$INSTALL_PREFIX/include/efi/"

# Copy libraries
echo -e "  Copying libraries..."
mkdir -p "$INSTALL_PREFIX/lib"
cp "$GNUEFI_DIR/aarch64/lib/"*.a "$INSTALL_PREFIX/lib/" 2>/dev/null || true
cp "$GNUEFI_DIR/aarch64/gnuefi/"*.a "$INSTALL_PREFIX/lib/" 2>/dev/null || true
cp "$GNUEFI_DIR/aarch64/gnuefi/"*.o "$INSTALL_PREFIX/lib/" 2>/dev/null || true

# Copy linker scripts
cp "$GNUEFI_DIR/gnuefi/"*.lds "$INSTALL_PREFIX/lib/" 2>/dev/null || true

echo -e "${GREEN}${NC} Installed to: $INSTALL_PREFIX"

# Create pkg-config file
echo
echo -e "${CYAN}[6/6]${NC} Creating configuration..."

mkdir -p "$INSTALL_PREFIX/lib/pkgconfig"

cat > "$INSTALL_PREFIX/lib/pkgconfig/gnu-efi.pc" << EOF
prefix=$INSTALL_PREFIX
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: gnu-efi
Description: GNU EFI development library
Version: $GNUEFI_VERSION
Libs: -L\${libdir} -lefi -lgnuefi
Cflags: -I\${includedir}/efi -I\${includedir}/efi/aarch64
EOF

echo -e "${GREEN}${NC} Created pkg-config file"

# Create environment script
cat > external/gnu-efi-env.sh << EOF
# Source this file to use project-local gnu-efi
export PKG_CONFIG_PATH="$INSTALL_PREFIX/lib/pkgconfig:\$PKG_CONFIG_PATH"
export GNUEFI_PREFIX="$INSTALL_PREFIX"
export EFI_INC="$INSTALL_PREFIX/include/efi"
export EFI_LIB="$INSTALL_PREFIX/lib"
export PATH="$INSTALL_PREFIX/bin:\$PATH"

echo "gnu-efi environment configured:"
echo "  Prefix: $INSTALL_PREFIX"
echo "  Include: \$EFI_INC"
echo "  Library: \$EFI_LIB"
EOF

chmod +x external/gnu-efi-env.sh

# Summary
echo
echo -e "${GREEN}gnu-efi setup complete!${NC}"
echo
echo -e "${CYAN}Installation:${NC}"
echo -e "  Location: ${GREEN}$INSTALL_PREFIX${NC}"
echo -e "  Version:  ${GREEN}$GNUEFI_VERSION${NC}"
echo
echo -e "${CYAN}Files installed:${NC}"
ls -lh "$INSTALL_PREFIX/lib/" | grep -E "\.(a|o|lds)$" | wc -l | xargs echo -e "  Libraries: ${GREEN}"{}"${NC}"
find "$INSTALL_PREFIX/include" -name "*.h" | wc -l | xargs echo -e "  Headers:   ${GREEN}"{}"${NC}"
echo
echo -e "${CYAN}Project structure:${NC}"
echo -e "  ${YELLOW}external/${NC}"
echo -e "  ├── ${GREEN}gnu-efi/${NC}              ← Source repository"
echo -e "  ├── ${GREEN}gnu-efi-install/${NC}      ← Installation"
echo -e "  └── ${GREEN}gnu-efi-env.sh${NC}        ← Environment script"
echo
echo -e "${CYAN}To use in your shell session:${NC}"
echo -e "  ${YELLOW}source external/gnu-efi-env.sh${NC}"
echo
echo -e "${CYAN}To build the bootloader:${NC}"
echo -e "  ${YELLOW}source external/gnu-efi-env.sh${NC}"
echo -e "  ${YELLOW}cd build${NC}"
echo -e "  ${YELLOW}cmake ..${NC}"
echo -e "  ${YELLOW}make${NC}"
echo
echo -e "${CYAN}The bootloader build system will automatically detect and use${NC}"
echo -e "${CYAN}the local gnu-efi installation.${NC}"
echo