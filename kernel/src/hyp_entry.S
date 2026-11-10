/**
 * @file hyp_entry.S
 * @brief EL2 vector table and guest/host world switch (see hyp.h)
 *
 * Everything that runs at EL2 is in this file. boot.S points VBAR_EL2 at
 * hyp_vectors before dropping to EL1, so from then on EL2 is only ever
 * entered through an exception:
 *
 *   - hvc #0 from the host kernel (TPIDR_EL2 == 0): HYP_CALL_* in x0.
 *     The host-side wrapper in hyp.c declares x0-x17 clobbered, the same
 *     as an SMCCC call, but HYP_CALL_RUN_VCPU saves all host registers
 *     anyway because it swaps the whole EL1 context out.
 *   - any trap or interrupt while a guest runs (TPIDR_EL2 == the
 *     running struct hyp_vcpu): save the guest, restore the host, and
 *     return from the host's hvc with x0 = HYP_EXIT_*.
 *
 * SP_EL2 (hyp_stack, set by boot.S) only ever holds the two registers
 * each vector entry needs to free up before it can look at TPIDR_EL2.
 */

#include "hyp.h"

/* Every EL1 system register a guest can change and the host relies on,
 * in CTX_SYS_* order (hyp.h). Scratch: x2, x3. */
.macro save_sysregs base
    mrs     x2, sctlr_el1
    mrs     x3, cpacr_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_SCTLR)]
    mrs     x2, ttbr0_el1
    mrs     x3, ttbr1_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_TTBR0)]
    mrs     x2, tcr_el1
    mrs     x3, mair_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_TCR)]
    mrs     x2, amair_el1
    mrs     x3, vbar_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_AMAIR)]
    mrs     x2, contextidr_el1
    mrs     x3, tpidr_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_CONTEXTIDR)]
    mrs     x2, tpidr_el0
    mrs     x3, tpidrro_el0
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_TPIDR_EL0)]
    mrs     x2, sp_el0
    mrs     x3, sp_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_SP_EL0)]
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_ELR)]
    mrs     x2, esr_el1
    mrs     x3, far_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_ESR)]
    mrs     x2, afsr0_el1
    mrs     x3, afsr1_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_AFSR0)]
    mrs     x2, par_el1
    mrs     x3, cntkctl_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_PAR)]
    mrs     x2, csselr_el1
    mrs     x3, mdscr_el1
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_CSSELR)]
    mrs     x2, cntv_ctl_el0
    mrs     x3, cntv_cval_el0
    stp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_CNTV_CTL)]
.endm

.macro restore_sysregs base
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_SCTLR)]
    msr     sctlr_el1, x2
    msr     cpacr_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_TTBR0)]
    msr     ttbr0_el1, x2
    msr     ttbr1_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_TCR)]
    msr     tcr_el1, x2
    msr     mair_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_AMAIR)]
    msr     amair_el1, x2
    msr     vbar_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_CONTEXTIDR)]
    msr     contextidr_el1, x2
    msr     tpidr_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_TPIDR_EL0)]
    msr     tpidr_el0, x2
    msr     tpidrro_el0, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_SP_EL0)]
    msr     sp_el0, x2
    msr     sp_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_ELR)]
    msr     elr_el1, x2
    msr     spsr_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_ESR)]
    msr     esr_el1, x2
    msr     far_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_AFSR0)]
    msr     afsr0_el1, x2
    msr     afsr1_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_PAR)]
    msr     par_el1, x2
    msr     cntkctl_el1, x3
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_CSSELR)]
    msr     csselr_el1, x2
    msr     mdscr_el1, x3
    /* CVAL before CTL, so an enabled timer never briefly compares
     * against the other context's deadline */
    ldp     x2, x3, [\base, #(CTX_SYS + 8 * CTX_SYS_CNTV_CTL)]
    msr     cntv_cval_el0, x3
    msr     cntv_ctl_el0, x2
.endm

/* x2..x30 <-> [base]; base must not be one of them. */
.macro save_gprs_2_30 base
    stp     x2,  x3,  [\base, #(CTX_X + 8 * 2)]
    stp     x4,  x5,  [\base, #(CTX_X + 8 * 4)]
    stp     x6,  x7,  [\base, #(CTX_X + 8 * 6)]
    stp     x8,  x9,  [\base, #(CTX_X + 8 * 8)]
    stp     x10, x11, [\base, #(CTX_X + 8 * 10)]
    stp     x12, x13, [\base, #(CTX_X + 8 * 12)]
    stp     x14, x15, [\base, #(CTX_X + 8 * 14)]
    stp     x16, x17, [\base, #(CTX_X + 8 * 16)]
    stp     x18, x19, [\base, #(CTX_X + 8 * 18)]
    stp     x20, x21, [\base, #(CTX_X + 8 * 20)]
    stp     x22, x23, [\base, #(CTX_X + 8 * 22)]
    stp     x24, x25, [\base, #(CTX_X + 8 * 24)]
    stp     x26, x27, [\base, #(CTX_X + 8 * 26)]
    stp     x28, x29, [\base, #(CTX_X + 8 * 28)]
    str     x30,      [\base, #(CTX_X + 8 * 30)]
.endm

.macro restore_gprs_2_30 base
    ldp     x2,  x3,  [\base, #(CTX_X + 8 * 2)]
    ldp     x4,  x5,  [\base, #(CTX_X + 8 * 4)]
    ldp     x6,  x7,  [\base, #(CTX_X + 8 * 6)]
    ldp     x8,  x9,  [\base, #(CTX_X + 8 * 8)]
    ldp     x10, x11, [\base, #(CTX_X + 8 * 10)]
    ldp     x12, x13, [\base, #(CTX_X + 8 * 12)]
    ldp     x14, x15, [\base, #(CTX_X + 8 * 14)]
    ldp     x16, x17, [\base, #(CTX_X + 8 * 16)]
    ldp     x18, x19, [\base, #(CTX_X + 8 * 18)]
    ldp     x20, x21, [\base, #(CTX_X + 8 * 20)]
    ldp     x22, x23, [\base, #(CTX_X + 8 * 22)]
    ldp     x24, x25, [\base, #(CTX_X + 8 * 24)]
    ldp     x26, x27, [\base, #(CTX_X + 8 * 26)]
    ldp     x28, x29, [\base, #(CTX_X + 8 * 28)]
    ldr     x30,      [\base, #(CTX_X + 8 * 30)]
.endm

/* A lower-EL vector slot: free x0/x1, x1 = exit reason, x0 = running
 * vCPU (or 0 if it was the host). */
.macro lower_el_entry reason
    .balign 128
    stp     x0, x1, [sp, #-16]!
    mov     x1, #\reason
    mrs     x0, tpidr_el2
    cbnz    x0, hyp_guest_exit
    b       hyp_host_trap
.endm

.macro bad_entry
    .balign 128
    b       hyp_panic
.endm

.section ".text"

.balign 2048
.global hyp_vectors
hyp_vectors:
    /* Current EL (EL2) with SP0 / SPx: a bug in this file */
    bad_entry
    bad_entry
    bad_entry
    bad_entry
    bad_entry
    bad_entry
    bad_entry
    bad_entry

    /* Lower EL, AArch64: the host's hvc, or a guest exit */
    lower_el_entry HYP_EXIT_TRAP
    lower_el_entry HYP_EXIT_IRQ
    lower_el_entry HYP_EXIT_IRQ      /* FIQ: same thing as far as we care */
    lower_el_entry HYP_EXIT_SERROR

    /* Lower EL, AArch32: never set up (HCR_EL2.RW = 1) */
    bad_entry
    bad_entry
    bad_entry
    bad_entry

/* ── Host -> EL2 ─────────────────────────────────────────────────────── */

hyp_host_trap:
    /* Only a synchronous hvc is expected from the host; an IRQ here
     * would mean HCR_EL2.IMO was left set outside a guest. */
    cmp     x1, #HYP_EXIT_TRAP
    b.ne    hyp_panic
    mrs     x0, esr_el2
    lsr     x0, x0, #26
    cmp     x0, #0x16                   /* EC = HVC64 */
    b.ne    hyp_panic
    ldp     x0, x1, [sp], #16           /* host's own x0/x1: the call */

    cmp     x0, #HYP_CALL_RUN_VCPU
    b.eq    hyp_run_vcpu
    cmp     x0, #HYP_CALL_INIT_MMU
    b.eq    hyp_init_mmu
    cmp     x0, #HYP_CALL_PROBE
    b.eq    hyp_probe
    mov     x0, #-1
    eret

hyp_probe:
    ldr     x0, =HYP_MAGIC
    eret

/* x1 = TTBR0_EL2, x2 = TCR_EL2, x3 = MAIR_EL2 (built by hyp.c). Runs
 * with EL2's MMU off until the final SCTLR_EL2 write, and touches no
 * memory, so there's nothing to keep coherent across the switch. */
hyp_init_mmu:
    msr     mair_el2, x3
    msr     tcr_el2, x2
    msr     ttbr0_el2, x1
    isb
    tlbi    alle2
    dsb     ish
    isb
    mrs     x4, sctlr_el2
    orr     x4, x4, #(1 << 0)           /* M: MMU */
    orr     x4, x4, #(1 << 2)           /* C: data cache */
    orr     x4, x4, #(1 << 12)          /* I: instruction cache */
    msr     sctlr_el2, x4
    isb
    mov     x0, #0
    eret

/* x1 = struct hyp_vcpu *. Save the host, load the guest, eret into it. */
hyp_run_vcpu:
    add     x9, x1, #VCPU_HOST
    stp     x0, x1, [x9, #(CTX_X + 8 * 0)]
    save_gprs_2_30 x9
    mrs     x10, elr_el2                /* host resumes after its hvc */
    mrs     x11, spsr_el2
    stp     x10, x11, [x9, #CTX_PC]
    save_sysregs x9

    /* The guest's virtual counter/timer, before its timer registers
     * come back: CNTVOFF_EL2 shifts what CNTV_CVAL is compared against. */
    ldr     x10, [x1, #VCPU_CNTVOFF]
    msr     cntvoff_el2, x10
    mov     x10, #CNTHCTL_EL2_GUEST
    msr     cnthctl_el2, x10

    add     x9, x1, #VCPU_GUEST
    restore_sysregs x9
    ldp     x10, x11, [x9, #CTX_PC]
    msr     elr_el2, x10
    msr     spsr_el2, x11

    ldr     x10, [x1, #VCPU_VTCR]
    msr     vtcr_el2, x10
    ldr     x10, [x1, #VCPU_VTTBR]
    msr     vttbr_el2, x10
    ldr     x10, [x1, #VCPU_HCR]
    msr     hcr_el2, x10
    mov     x10, #CPTR_EL2_GUEST
    msr     cptr_el2, x10
    isb

    /* New stage-2 tables (or a reused VMID): drop anything cached for
     * this VMID - needs VTTBR_EL2 (and so the VMID) already in place. */
    ldr     x10, [x1, #VCPU_FLAGS]
    tbz     x10, #0, 1f
    tlbi    vmalls12e1
    dsb     nsh
    isb
    bic     x10, x10, #HYP_VCPU_F_FLUSH_TLB
    str     x10, [x1, #VCPU_FLAGS]
1:
    /* Host time slice: the EL2 physical timer (PPI 26) fires while the
     * guest runs, and HCR_EL2.IMO brings that IRQ here - so the host
     * gets the CPU back even from a guest that never traps. */
    ldr     x10, [x1, #VCPU_SLICE]
    cbz     x10, 2f
    msr     cnthp_tval_el2, x10
    mov     x10, #1                     /* ENABLE, not IMASK */
    msr     cnthp_ctl_el2, x10
2:
    isb
    msr     tpidr_el2, x1
    mov     x0, x9
    restore_gprs_2_30 x0
    ldp     x0, x1, [x0, #(CTX_X + 8 * 0)]
    eret

/* ── Guest -> EL2 -> host ────────────────────────────────────────────── */

/* x0 = struct hyp_vcpu * (guest context at offset 0), x1 = HYP_EXIT_*,
 * guest x0/x1 on the EL2 stack. */
hyp_guest_exit:
    str     x1, [x0, #VCPU_EXIT_REASON]
    save_gprs_2_30 x0
    ldp     x2, x3, [sp], #16
    stp     x2, x3, [x0, #(CTX_X + 8 * 0)]
    mrs     x2, elr_el2
    mrs     x3, spsr_el2
    stp     x2, x3, [x0, #CTX_PC]
    save_sysregs x0

    mrs     x2, esr_el2
    str     x2, [x0, #VCPU_ESR]
    mrs     x2, far_el2
    str     x2, [x0, #VCPU_FAR]
    mrs     x2, hpfar_el2
    str     x2, [x0, #VCPU_HPFAR]

    mov     x1, x0
    msr     cnthp_ctl_el2, xzr          /* slice over: drop the PPI 26 line */
    msr     tpidr_el2, xzr
    mov     x10, #HCR_EL2_HOST
    msr     hcr_el2, x10
    mov     x10, #CPTR_EL2_HOST
    msr     cptr_el2, x10
    msr     vttbr_el2, xzr

    add     x9, x1, #VCPU_HOST
    restore_sysregs x9                  /* host CNTV_CTL: its timer (if any) back */
    msr     cntvoff_el2, xzr
    mov     x10, #CNTHCTL_EL2_HOST
    msr     cnthctl_el2, x10
    ldp     x10, x11, [x9, #CTX_PC]
    msr     elr_el2, x10
    msr     spsr_el2, x11
    isb

    ldr     x0, [x1, #VCPU_EXIT_REASON] /* host's hvc returns this */
    mov     x1, x9
    restore_gprs_2_30 x1
    ldr     x1, [x1, #(CTX_X + 8 * 1)]
    eret

/* ── Can't happen ────────────────────────────────────────────────────── */

hyp_panic:
    adr     x0, hyp_panic_msg
    bl      uart_puts
    mrs     x0, esr_el2
    mrs     x1, elr_el2
    mrs     x2, far_el2
    bl      report_sync_exception
1:  wfe
    b       1b

hyp_panic_msg:
    .asciz  "\r\n*** EL2 (hypervisor) exception ***\r\n"
    .balign 4

/* ── EL2 stack ───────────────────────────────────────────────────────── */

.section ".bss"
.balign 16
hyp_stack:
    .space  4096
.global hyp_stack_top
hyp_stack_top:
