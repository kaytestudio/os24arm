/* ============================================================================
 * beaglev_boot.S - BeagleV-Ahead (T-Head TH1520, XuanTie C910) boot code
 * ============================================================================
 * The image starts with a RISC-V Linux Image header so U-Boot's `booti`
 * accepts it (see bootloader/beaglev_ahead_uEnv.txt.in). booti flushes the
 * caches and enters here in S-mode (OpenSBI owns M-mode) with paging off,
 *   a0 = hart id, a1 = device tree address.
 *
 * Depending on the U-Boot version, booti either runs the image where it
 * was loaded or first moves it to DRAM base + text_offset. DRAM starts at
 * 0x0 on the TH1520 and the image is loaded at 0x200000, so both land at
 * the address beaglev_linker.ld links it at.
 *
 * A U-Boot built with SMP support starts every hart at the entry point,
 * so the first hart through the lottery below boots and the rest park.
 */

    .section ".text.boot", "ax"
    .global _start
_start:
    /* ── RISC-V Image header (Documentation/arch/riscv/boot-image-header.rst) */
    .option push
    .option norvc               /* code0 must be exactly one 4-byte insn */
    j       reset               /* code0                                 */
    .option pop
    .word   0                   /* code1                                 */
    .dword  0x200000            /* text_offset (from DRAM base)          */
    .dword  _image_size         /* image_size (includes .bss)            */
    .dword  0                   /* flags: little endian                  */
    .word   0x2                 /* version 0.2                           */
    .word   0                   /* res1                                  */
    .dword  0                   /* res2                                  */
    .ascii  "RISCV\0\0\0"       /* magic (deprecated, still checked)     */
    .ascii  "RSC\x05"           /* magic2                                */
    .word   0                   /* res3 (PE/COFF offset: none)           */

reset:
    csrw    sie, zero           /* no interrupts, and no traps from them */
    csrci   sstatus, 0x2        /* sstatus.SIE = 0                       */

    /* Hart lottery - boot_lottery lives in .data, so it starts at 0
     * without relying on .bss having been cleared. */
    la      t0, boot_lottery
    li      t1, 1
    amoadd.w t1, t1, (t0)
    bnez    t1, park

    la      sp, _stack_top

    la      t0, trap_entry
    csrw    stvec, t0

    /* Clear .bss (both ends 8-byte aligned by beaglev_linker.ld) */
    la      t0, _bss_start
    la      t1, _bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:
    fence.i

    /* kernel_main(hart_id = a0, dtb = a1) - both still as booti left them */
    call    kernel_main

hang:
    wfi
    j       hang

park:
    wfi
    j       park

/* ── Trap entry ─────────────────────────────────────────────────────────────
 * Nothing is expected to trap yet (interrupts are off), so any trap is an
 * exception: report scause/sepc/stval and halt. Reset sp in case the trap
 * came from a stack overflow. */
    .balign 4
trap_entry:
    csrr    a0, scause
    csrr    a1, sepc
    csrr    a2, stval
    la      sp, _stack_top
    call    report_sync_exception
    j       hang

    .section ".data"
    .balign 4
boot_lottery:
    .word   0
