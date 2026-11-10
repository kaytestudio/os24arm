#!/bin/bash
# build_iso.sh - Simple ISO builder for OS/2 Warp ARM64

set -e

BUILD_DIR="$1"
ISO_BUILD_DIR="$BUILD_DIR/iso_build"
ISO_OUTPUT="$BUILD_DIR/os2warp-arm64.iso"

echo "Building ISO..."

# Clean and create directories
rm -rf "$ISO_BUILD_DIR"
mkdir -p "$ISO_BUILD_DIR/EFI/BOOT"
mkdir -p "$ISO_BUILD_DIR/OS2"
mkdir -p "$ISO_BUILD_DIR/COMMANDS"

# Copy kernel
echo "  Copying kernel..."
cp "$BUILD_DIR/os2warp.img" "$ISO_BUILD_DIR/OS2/"

# Copy commands
echo "  Copying commands..."
if [ -d "$BUILD_DIR/kernel/commands" ]; then
    cp "$BUILD_DIR/kernel/commands"/*.EXE "$ISO_BUILD_DIR/COMMANDS/" 2>/dev/null || true
fi

# Copy bootloader if exists
echo "  Copying bootloader..."
if [ -f "$BUILD_DIR/bootloader/BOOTAA64.EFI" ]; then
    cp "$BUILD_DIR/bootloader/BOOTAA64.EFI" "$ISO_BUILD_DIR/EFI/BOOT/"
    echo "    Bootloader included"
else
    echo "    Bootloader not found - ISO will use direct kernel boot"
fi

# Create README
cat > "$ISO_BUILD_DIR/README.TXT" << 'EOF'
OS/2 Warp 4.52 for ARM64

This ISO contains:
  /OS2/os2warp.img       - Kernel image
  /COMMANDS/*.EXE        - Command executables
  /EFI/BOOT/BOOTAA64.EFI - UEFI bootloader (if present)
EOF

# Build ISO
echo "  Building ISO image..."
if command -v xorriso >/dev/null 2>&1; then
    # Simple ISO without boot options (most compatible)
    xorriso -as mkisofs \
        -o "$ISO_OUTPUT" \
        -V "OS2WARP_ARM64" \
        -J -R \
        "$ISO_BUILD_DIR"
    echo "    ISO created with xorriso"
elif command -v mkisofs >/dev/null 2>&1; then
    mkisofs \
        -o "$ISO_OUTPUT" \
        -V "OS2WARP_ARM64" \
        -J -R \
        "$ISO_BUILD_DIR"
    echo "    ISO created with mkisofs"
else
    echo "Error: No ISO creation tool found"
    exit 1
fi

# Show results
echo ""
echo "ISO Build Complete!"
ls -lh "$ISO_OUTPUT"
echo ""