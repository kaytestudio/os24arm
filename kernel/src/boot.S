/**
 * @file boot.S
 * @brief ARM64 Boot Code for OS/2 Warp Kernel (Fixed)
 */

#include "hyp.h"

.section ".text.boot"

.global _start

_start:
    /* We enter here on core 0, at EL1 - or at EL2 when QEMU runs with
     * -machine virt,virtualization=on (make run-vm), in which case EL2 is
     * set up to host guests (kernel/src/hyp_entry.S) before dropping to
     * EL1 below. The kernel itself always runs at EL1. */

    /* Read CPU ID, hang all cores except core 0 */
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, core0

    /* Cores 1-3: infinite loop */
hang:
    wfe
    b       hang

core0:
    /* Disable interrupts */
    msr     daifset, #0xF

    /* x19 = entry exception level, kept until BSS is cleared and it can
     * be stored in hyp_boot_el (kernel/src/hyp.c). */
    mrs     x19, CurrentEL
    lsr     x19, x19, #2
    and     x19, x19, #3
    cmp     x19, #2
    b.ne    el1_entry

    /* ── EL2: keep it for the hypervisor, then continue at EL1 ─────── */
    ldr     x0, =hyp_vectors
    msr     vbar_el2, x0
    ldr     x0, =hyp_stack_top
    mov     sp, x0                  /* SP_EL2 */

    mov     x0, #HCR_EL2_HOST       /* EL1 is AArch64; nothing trapped */
    msr     hcr_el2, x0
    mov     x0, #CPTR_EL2_HOST      /* no FP/SIMD trap to EL2 */
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    mov     x0, #CNTHCTL_EL2_HOST   /* EL1 physical counter/timer access */
    msr     cnthctl_el2, x0
    msr     cnthp_ctl_el2, xzr      /* EL2 timer off until a guest runs */
    msr     cntvoff_el2, xzr
    msr     vttbr_el2, xzr
    msr     tpidr_el2, xzr          /* 0 = host running (hyp_entry.S) */
    mrs     x0, midr_el1            /* EL1 reads of MIDR/MPIDR see these */
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0
    ldr     x0, =0x30c50830         /* SCTLR_EL2: RES1 bits, MMU/caches off */
    msr     sctlr_el2, x0

    /* GICv3 present (ID_AA64PFR0_EL1.GIC != 0): let EL1 use the
     * system-register CPU interface - otherwise ICC_* at EL1 is UNDEFINED. */
    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4
    cbz     x0, 1f
    mov     x0, #0xf                /* ICC_SRE_EL2: Enable | DIB | DFB | SRE */
    msr     S3_4_C12_C9_5, x0       /* ICC_SRE_EL2 */
1:
    isb

    mov     x0, #0x3c5              /* EL1h, DAIF masked */
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:

    /* Set up stack pointer (grows downward) */
    ldr     x1, =_stack_top
    mov     sp, x1

    /* Ensure stack is valid before proceeding */
    mov     x0, #0
    str     x0, [sp, #-16]!   /* Test stack write */
    ldr     x0, [sp], #16     /* Test stack read */

    /* Clear BSS section with safety checks */
    ldr     x1, =_bss_start
    ldr     x2, =_bss_end

    /* Safety: Check if BSS addresses are valid */
    cmp     x1, x2
    b.ge    bss_done          /* Skip if _bss_start >= _bss_end */

clear_bss:
    /* Clear 8 bytes at a time */
    str     xzr, [x1], #8
    cmp     x1, x2
    b.lt    clear_bss

bss_done:
    /* Ensure memory operations complete */
    dsb     sy
    isb

    ldr     x0, =hyp_boot_el
    str     x19, [x0]

    /* Set up exception vector table */
    ldr     x0, =vector_table
    msr     vbar_el1, x0
    isb

    /* Enable FP/SIMD access at EL1 (CPACR_EL1.FPEN = 0b11). Traps by
     * default at reset; kprintf's variadic prologue spills q0-q7 as part
     * of the AAPCS64 varargs register-save area, which needs this. */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    /* Enable alignment checking (optional, helps debug) */
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 1)  /* Enable alignment checking */
    msr     sctlr_el1, x0
    isb

    /* Enable the MMU (kernel/src/mmu.c) - identity-mapped, so this keeps
     * running from the same addresses afterwards with no trampoline
     * needed; see that file for why (real cacheable memory attributes,
     * not virtual memory or isolation). Must run before anything that
     * benefits from caching, i.e. everything from kernel_main() on. */
    bl      mmu_init

    /* Jump to C kernel entry point */
    bl      kernel_main

    /* If kernel_main returns, hang */
    b       hang

/* Exception vector table (required for ARM64) */
.balign 2048
vector_table:
    /* Current EL with SP0 */
    .balign 128
    b       sync_exception_sp0
    .balign 128
    b       irq_exception_sp0
    .balign 128
    b       fiq_exception_sp0
    .balign 128
    b       serror_exception_sp0

    /* Current EL with SPx */
    .balign 128
    b       sync_exception_spx
    .balign 128
    b       irq_exception_spx
    .balign 128
    b       fiq_exception_spx
    .balign 128
    b       serror_exception_spx

    /* Lower EL using AArch64 */
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang

    /* Lower EL using AArch32 */
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang
    .balign 128
    b       hang

/* Exception handlers: report to UART (esr/elr/far -> x0/x1/x2) then hang.
 * report_sync_exception() uses only uart_putc/uart_puts, so it stays usable
 * even if the fault happened inside variadic-argument handling. */
sync_exception_sp0:
    stp     x29, x30, [sp, #-16]!
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      report_sync_exception
    b       hang

irq_exception_sp0:
    b       hang

fiq_exception_sp0:
    b       hang

serror_exception_sp0:
    b       hang

sync_exception_spx:
    stp     x29, x30, [sp, #-16]!
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      report_sync_exception
    b       hang

irq_exception_spx:
    b       hang

fiq_exception_spx:
    b       hang

serror_exception_spx:
    b       hang