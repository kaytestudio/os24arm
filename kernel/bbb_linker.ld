/* ============================================================================
 * bbb_linker.ld - BeagleBone Black (TI AM335x) kernel linker script
 * ============================================================================
 * Linked to run at 0x82000000 - the stock BeagleBone ${loadaddr}, inside
 * the 512 MB of DDR at 0x80000000 and clear of U-Boot (which relocates to
 * the top of RAM). Must match KERNEL_LOAD_ADDR in CMakeLists.txt and the
 * load address in bootloader/bbb_uEnv.txt.in: bootz runs the image where
 * it was loaded, and nothing here is position-independent.
 */

OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
    . = 0x82000000;

    .text : {
        KEEP(*(.text.boot))     /* zImage header + reset code must be first */
        *(.text*)
    }

    .rodata : ALIGN(8) { *(.rodata*) }

    .data : ALIGN(8) { *(.data*) }

    /* Everything objcopy -O binary emits ends here; the zImage header's
     * "end" field is the size of that raw image. */
    _image_end = .;
    _image_size = _image_end - _start;

    .bss (NOLOAD) : ALIGN(8) {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    }

    /* 64 KB kernel stack right after .bss, then the heap (see beagle_main.c) */
    . = ALIGN(16);
    . += 64K;
    _stack_top = .;

    . = ALIGN(4096);
    _heap_start = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
