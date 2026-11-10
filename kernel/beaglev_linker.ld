/* ============================================================================
 * beaglev_linker.ld - RV64 kernel linker script (BeagleV-Ahead, QEMU_RISCV)
 * ============================================================================
 * Linked at KERNEL_LOAD_ADDR, which kernel/CMakeLists.txt passes in with
 * --defsym from the platform's KERNEL_LOAD_ADDR in CMakeLists.txt. Both
 * platforms put the kernel 2 MB into DRAM, right after OpenSBI - the slot
 * the Image header's text_offset asks for (see beaglev_boot.S):
 *   - BeagleV-Ahead: TH1520 DRAM starts at 0x0, so 0x200000 (must also
 *     match bootloader/beaglev_ahead_uEnv.txt.in)
 *   - QEMU_RISCV:    virt DRAM starts at 0x80000000, so 0x80200000
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    .text : {
        KEEP(*(.text.boot))     /* Image header + reset code must be first */
        *(.text*)
    }

    .rodata : ALIGN(8) {
        *(.rodata*)
        *(.srodata*)
    }

    .data : ALIGN(8) {
        *(.data*)
        *(.sdata*)
    }

    .bss (NOLOAD) : ALIGN(8) {
        _bss_start = .;
        *(.sbss*)
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    }

    /* The Image header's image_size covers the whole loaded footprint,
     * .bss included, so booti reserves (and never overlaps) all of it. */
    _image_size = _bss_end - _start;

    /* 64 KB kernel stack right after .bss, then the heap (see beagle_main.c) */
    . = ALIGN(16);
    . += 64K;
    _stack_top = .;

    . = ALIGN(4096);
    _heap_start = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}
