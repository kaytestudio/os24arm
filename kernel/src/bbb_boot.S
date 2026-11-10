/* ============================================================================
 * bbb_boot.S - BeagleBone Black (TI AM335x, ARM Cortex-A8) boot code
 * ============================================================================
 * The image starts with an ARM Linux zImage header so U-Boot's `bootz`
 * accepts it (see bootloader/bbb_uEnv.txt.in). bootz cleans the caches and
 * enters here in SVC mode, ARM state, MMU and D-cache off, with
 *   r0 = 0, r1 = machine type, r2 = device tree address.
 *
 * bootz does not relocate a zImage - the image runs where it was loaded,
 * so it must be loaded at the address bbb_linker.ld links it at.
 */

    .syntax unified
    .arm

    .section ".text.boot", "ax"
    .global _start
_start:
    /* zImage header: 8 NOPs, a branch, then the magic at offset 0x24
     * (U-Boot's struct zimage_header: code[9], magic, start, end). */
    .rept 8
    mov     r0, r0
    .endr
    b       reset
    .word   0x016f2818          /* zImage magic                          */
    .word   0                   /* start: 0 = run from the load address  */
    .word   _image_size         /* end: image size in bytes              */

reset:
    cpsid   aif                 /* mask IRQ/FIQ/async aborts             */
    cps     #0x13               /* make sure we're in SVC mode           */

    /* The AM335x is single-core, but park any secondary core should this
     * run on an MP part. Only trust MPIDR.Aff0 when bit 31 says it has the
     * Multiprocessing Extensions format - the Cortex-A8 has no MP
     * extensions and its MPIDR isn't guaranteed to read as 0. */
    mrc     p15, 0, r3, c0, c0, 5
    tst     r3, #(1 << 31)
    beq     1f
    ands    r3, r3, #0xff
    bne     park
1:
    ldr     sp, =_stack_top

    mov     r4, r2              /* keep the device tree address          */

    /* Point VBAR at our vectors and make sure they're used: clear
     * SCTLR.V (high vectors) and SCTLR.TE (Thumb exceptions), and turn
     * the I-cache on - safe with the MMU off, and much faster. */
    ldr     r0, =_vectors
    mcr     p15, 0, r0, c12, c0, 0
    mrc     p15, 0, r0, c1, c0, 0
    bic     r0, r0, #(1 << 13)
    bic     r0, r0, #(1 << 30)
    orr     r0, r0, #(1 << 12)
    mcr     p15, 0, r0, c1, c0, 0
    isb

    /* Clear .bss (both ends 4-byte aligned by bbb_linker.ld) */
    ldr     r0, =_bss_start
    ldr     r1, =_bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    mov     r0, #0              /* kernel_main(hart_id = 0, dtb) */
    mov     r1, r4
    bl      kernel_main

hang:
park:
    wfi
    b       hang

/* ── Exception vectors ──────────────────────────────────────────────────────
 * Nothing is expected to trap yet, so every vector reports and halts.
 * The abort/undef modes have no stack of their own; the handlers grab
 * what they need into r0-r2 (unbanked), then switch back to SVC mode
 * and borrow its stack to call report_sync_exception(). */
    .balign 32
_vectors:
    b       reset               /* 0x00 reset          */
    b       exc_undef           /* 0x04 undefined      */
    b       exc_svc             /* 0x08 SVC            */
    b       exc_pabt            /* 0x0C prefetch abort */
    b       exc_dabt            /* 0x10 data abort     */
    b       .                   /* 0x14 reserved       */
    b       exc_irq             /* 0x18 IRQ            */
    b       exc_fiq             /* 0x1C FIQ            */

exc_undef:
    mov     r0, #1
    sub     r1, lr, #4
    mov     r2, #0
    b       exc_report
exc_svc:
    mov     r0, #2
    sub     r1, lr, #4
    mov     r2, #0
    b       exc_report
exc_pabt:
    mov     r0, #3
    sub     r1, lr, #4
    mrc     p15, 0, r2, c6, c0, 2   /* IFAR */
    b       exc_report
exc_dabt:
    mov     r0, #4
    sub     r1, lr, #8
    mrc     p15, 0, r2, c6, c0, 0   /* DFAR */
    b       exc_report
exc_irq:
    mov     r0, #6
    sub     r1, lr, #4
    mov     r2, #0
    b       exc_report
exc_fiq:
    mov     r0, #7
    sub     r1, lr, #4
    mov     r2, #0

exc_report:
    cps     #0x13
    bl      report_sync_exception
    b       hang
