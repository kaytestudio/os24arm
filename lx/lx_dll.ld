/* lx/lx_dll.ld — linker script for LX DLLs built by this project
 *
 * DLLs need their own fixed load address, distinct from lx_app.ld's
 * LX_LOAD_BASE: an EXE (CMD.EXE, KAYTEVM.EXE, ...) that embeds and loads
 * a DLL at runtime (see kernel/src/lx_dll_loader.c) is itself already
 * resident at LX_LOAD_BASE, and this project's flat address space has no
 * way for two images to share the same address. Since link address ==
 * load address by construction (same reasoning as lx_app.ld), the DLL
 * loader can copy each object verbatim to its fixed address with no
 * relocation/fixup processing.
 *
 * Like lx_app.ld, code (.text+.rodata) and data (.data+.bss) are split
 * into two separate objects at a fixed 2MB boundary
 * (LX_DLL_CODE_REGION_SIZE) rather than one flat blob, so
 * kernel/src/mmu.c can map them with real, different W^X permissions.
 *
 * Must match LX_DLL_LOAD_BASE/LX_DLL_CODE_REGION_SIZE in
 * kernel/include/lx_dll_loader.h.
 */

SECTIONS
{
    . = 0x4A000000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    /* Pad up to the next 2MB boundary (must match LX_DLL_CODE_REGION_SIZE
     * and the split offset makedll is invoked with - see the top-level
     * CMakeLists.txt) before .data begins. Uses ALIGN() (monotonic -
     * never moves `.` backward) rather than a hardcoded jump, so the
     * ASSERT below can actually catch code/rodata overflowing the region
     * instead of silently overlapping the data object that would follow. */
    . = ALIGN(0x200000);
    /* Bare ASSERT(...) as a standalone statement confuses lld's linker-
     * script parser (it stops recognizing the .data section that
     * follows) - the "assign to `.`" expression form works with both
     * lld and GNU ld. */
    . = ASSERT((. - 0x4A000000) <= 0x200000,
               "LX DLL .text+.rodata exceeds the 2MB code region (lx/lx_dll.ld / LX_DLL_CODE_REGION_SIZE)");

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
