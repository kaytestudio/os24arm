/* lx/lx_app.ld — linker script for LX applications built by this project
 *
 * Every program wrapped into an OS/2 LX .EXE/.DLL by this project's build
 * (see the top-level CMakeLists.txt's add_lx_executable()/add_lx_library()
 * and the CMD.EXE target) is linked here at LX_LOAD_BASE, which must match
 * the kernel's built-in loader (kernel/include/lx_loader.h). Since link
 * address == load address by construction, the loader (kernel/src/lx_loader.c)
 * can copy each object verbatim with no relocation/fixup processing.
 *
 * Code (.text+.rodata) and data (.data+.bss) are split into two separate
 * objects at a fixed 2MB boundary (LX_CODE_REGION_SIZE) rather than one
 * flat blob, so kernel/src/mmu.c can map them with real, different W^X
 * permissions - RX for code, RW+NX for data - instead of one region that
 * has to be both writable and executable. makeexe (lx/makeexe.c) slices
 * the `objcopy -O binary` output at that same fixed offset to turn it
 * back into the two LX objects.
 */

ENTRY(main)

SECTIONS
{
    . = 0x48000000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    /* Pad up to the next 2MB boundary (must match LX_CODE_REGION_SIZE,
     * kernel/include/lx_loader.h, and the split offset makeexe is invoked
     * with - see the top-level CMakeLists.txt) before .data begins. Uses
     * ALIGN() (monotonic - never moves `.` backward) rather than a
     * hardcoded jump, so the ASSERT below can actually catch code/rodata
     * overflowing the region instead of silently overlapping the data
     * object that would follow. */
    . = ALIGN(0x200000);
    /* Bare ASSERT(...) as a standalone statement confuses lld's linker-
     * script parser (it stops recognizing the .data section that
     * follows) - the "assign to `.`" expression form works with both
     * lld and GNU ld. */
    . = ASSERT((. - 0x48000000) <= 0x200000,
               "LX app .text+.rodata exceeds the 2MB code region (lx/lx_app.ld / LX_CODE_REGION_SIZE)");

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
