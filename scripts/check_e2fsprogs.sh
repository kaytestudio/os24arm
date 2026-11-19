#!/bin/bash
# Quick diagnostic for e2fsprogs on macOS

echo "  e2fsprogs Diagnostic Tool"
echo

# Check Homebrew
echo "1. Checking Homebrew..."
if command -v brew &> /dev/null; then
    echo "   Homebrew found: $(which brew)"
    BREW_VERSION=$(brew --version | head -n1)
    echo "   Version: $BREW_VERSION"
else
    echo "   Homebrew NOT found"
    exit 1
fi
echo

# Check e2fsprogs installation
echo "2. Checking e2fsprogs installation..."
if brew list e2fsprogs &> /dev/null 2>&1; then
    echo "   e2fsprogs is installed"
    E2FS_VERSION=$(brew list --versions e2fsprogs)
    echo "   Installed: $E2FS_VERSION"
else
    echo "   e2fsprogs is NOT installed"
    echo "   → Install with: brew install e2fsprogs"
    exit 1
fi
echo

# Check prefix
echo "3. Checking installation paths..."
E2FS_PREFIX=$(brew --prefix e2fsprogs 2>/dev/null)
if [ -n "$E2FS_PREFIX" ]; then
    echo "   Prefix: $E2FS_PREFIX"
else
    echo "   Could not determine prefix"
    exit 1
fi
echo

# Check for mkfs.ext4
echo "4. Checking for mkfs.ext4..."
FOUND=0

if [ -f "$E2FS_PREFIX/sbin/mkfs.ext4" ]; then
    echo "   Found: $E2FS_PREFIX/sbin/mkfs.ext4"
    MKFS_EXT4="$E2FS_PREFIX/sbin/mkfs.ext4"
    FOUND=1
elif [ -f "$E2FS_PREFIX/bin/mkfs.ext4" ]; then
    echo "   Found: $E2FS_PREFIX/bin/mkfs.ext4"
    MKFS_EXT4="$E2FS_PREFIX/bin/mkfs.ext4"
    FOUND=1
fi

if [ $FOUND -eq 0 ]; then
    echo "   mkfs.ext4 NOT found in expected locations"
    echo "   → Try: brew reinstall e2fsprogs"
    exit 1
fi
echo

# Check version
echo "5. Testing mkfs.ext4..."
if [ -n "$MKFS_EXT4" ]; then
    VERSION=$("$MKFS_EXT4" -V 2>&1 | head -n1)
    echo "   Version: $VERSION"
else
    echo "   Could not test (not found)"
    exit 1
fi
echo

# Check if linked
echo "6. Checking if e2fsprogs is linked..."
if brew list e2fsprogs | grep -q "not symlinked"; then
    echo "   Warning: e2fsprogs is installed but NOT linked"
    echo "   → To link: brew link e2fsprogs"
    echo "   → Then you can use: mkfs.ext4 (without full path)"
else
    echo "   e2fsprogs is properly linked"
fi
echo

# Summary
echo "  SUMMARY"
echo
echo "e2fsprogs path: $MKFS_EXT4"
echo
echo "To use in mkdisk.sh, the script should detect this automatically."
echo "If not, try:"
echo "  1. brew link e2fsprogs"
echo "  2. brew reinstall e2fsprogs"
echo "  3. Re-run make mkdisk"
echo