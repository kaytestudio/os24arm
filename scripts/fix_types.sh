#!/bin/bash
# One-liner fix for type errors in kernel stubs

echo "Fixing kernel_stubs.c type errors..."

# Replace the problematic file
cat > kernel/src/kernel_stubs.c << 'EOF'
#include "os2.h"

static ULONG heap_current = 0;
static ULONG heap_end = 0;

void mem_init(ULONG start, ULONG size)
{
    heap_current = start;
    heap_end = start + size;
}

void* mem_alloc(ULONG size)
{
    if (heap_current == 0) return NULL;
    size = (size + 15) & ~15;
    if (heap_current + size > heap_end) return NULL;
    void *ptr = (void*)heap_current;
    heap_current += size;
    return ptr;
}

void mem_free(void *ptr) { (void)ptr; }
void scheduler_init(void) { }
void blk_init(void) { }
EOF

# Remove old cli_stubs.c if exists
rm -f kernel/src/cli_stubs.c

# Fix CMakeLists.txt to reference kernel_stubs.c
sed -i.bak 's/cli_stubs\.c/kernel_stubs.c/g' kernel/CMakeLists.txt

echo "Fixed! Now rebuild:"
echo "   cd build"
echo "   cmake .."
echo "   make"
