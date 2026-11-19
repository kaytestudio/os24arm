#!/bin/bash
# Writes a stub EFI file for image and installer tests; it cannot boot the OS.

set -e

GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${CYAN}  Creating Placeholder Bootloader${NC}"
echo

if [ ! -d "bootloader" ]; then
    mkdir -p bootloader
    echo -e "${GREEN}${NC} Created bootloader directory"
fi

cat > bootloader/BOOTAA64.EFI << 'EOFBOOT'
MZ PLACEHOLDER BOOTLOADER - REPLACE WITH REAL UEFI APPLICATION
This is a placeholder file to allow the build system to work.

To create a real bootloader, you need to:
1. Install gnu-efi development files
2. Write UEFI boot code in C
3. Compile with the UEFI toolchain
4. Sign the binary (optional)

For now, this allows you to:
- Test disk image creation
- Test installation scripts
- Verify the build system works

The real bootloader should:
- Initialize UEFI services
- Load the kernel from disk
- Parse CONFIG.SYS
- Set up memory mappings
- Transfer control to the kernel

See bootloader_stub.c for a minimal example.
EOFBOOT

chmod 644 bootloader/BOOTAA64.EFI

echo -e "${GREEN}${NC} Created placeholder: bootloader/BOOTAA64.EFI"
echo
echo -e "${YELLOW}Warning: NOTE: This is a PLACEHOLDER!${NC}"
echo -e "${YELLOW}   It will not actually boot OS/2.${NC}"
echo -e "${YELLOW}   It's just here to unblock the build system.${NC}"
echo
echo -e "${CYAN}Next steps:${NC}"
echo -e "  1. ${GREEN}cd build && cmake ..${NC}"
echo -e "  2. ${GREEN}make${NC}"
echo -e "  3. ${GREEN}make install-bootloader${NC} ${YELLOW}(should work now!)${NC}"
echo
echo -e "${CYAN}To create a real bootloader:${NC}"
echo -e "  • Install gnu-efi: ${GREEN}brew install gnu-efi${NC} (macOS)"
echo -e "  • Or: ${GREEN}sudo apt install gnu-efi${NC} (Linux)"
echo -e "  • Implement bootloader code in C"
echo -e "  • Compile to UEFI PE32+ format"
echo
