/*
 * kernel/src/linux/el0.S - EL0 trap entry/return and kernel context switch
 * for the Linux-ABI user mode (see linux.c).
 *
 * Trap frame (linux_tf_t in linux_abi.h), built on the EL1 stack when an
 * exception is taken from EL0:
 *     0  x0 .. x30        (31 x 8)
 *   248  sp_el0
 *   256  elr_el1
 *   264  spsr_el1
 *   272  q0 .. q31        (32 x 16) - the kernel itself uses FP/SIMD
 *                          registers (kprintf varargs), and Linux promises
 *                          programs that svc preserves all of them
 *   784  fpsr
 *   792  fpcr
 *   800  (size, 16-byte aligned)
 */
    .equ TF_SIZE, 800

/* SCTLR_EL1.A (bit 1): the kernel runs with strict alignment checking as a
 * debugging aid (boot.S), but Linux programs - musl included - rely on
 * unaligned accesses to normal memory working, as they do on Linux. It is
 * cleared while EL0 runs and set again on every way back into the kernel. */
    .macro align_check on
    mrs     x9, sctlr_el1
    .if \on
    orr     x9, x9, #2
    .else
    bic     x9, x9, #2
    .endif
    msr     sctlr_el1, x9
    isb
    .endm

    .text
    .balign 4

/* void cpu_switch(uint64_t save[13], const uint64_t load[13])
 * Kernel context switch: saves x19-x30 and sp into save, loads them from
 * load, and returns into whatever context load describes. */
    .global cpu_switch
cpu_switch:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x9, sp
    str     x9, [x0, #96]
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x9, [x1, #96]
    mov     sp, x9
    ret

/* First return to EL0 of a new process: its kernel context was set up with
 * sp = its trap frame and x30 = here (see new_kctx in linux.c). */
    .global el0_start
el0_start:
    b       el0_restore

/* Build the trap frame on the EL1 stack (sp -> it). */
    .macro save_frame
    sub     sp, sp, #TF_SIZE
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x9, sp_el0
    str     x9, [sp, #248]
    mrs     x9, elr_el1
    mrs     x10, spsr_el1
    stp     x9, x10, [sp, #256]
    add     x9, sp, #272
    stp     q0, q1, [x9, #0]
    stp     q2, q3, [x9, #32]
    stp     q4, q5, [x9, #64]
    stp     q6, q7, [x9, #96]
    stp     q8, q9, [x9, #128]
    stp     q10, q11, [x9, #160]
    stp     q12, q13, [x9, #192]
    stp     q14, q15, [x9, #224]
    stp     q16, q17, [x9, #256]
    stp     q18, q19, [x9, #288]
    stp     q20, q21, [x9, #320]
    stp     q22, q23, [x9, #352]
    stp     q24, q25, [x9, #384]
    stp     q26, q27, [x9, #416]
    stp     q28, q29, [x9, #448]
    stp     q30, q31, [x9, #480]
    mrs     x10, fpsr
    mrs     x11, fpcr
    add     x9, sp, #784
    stp     x10, x11, [x9]

    .endm

/* Synchronous exception from EL0 (vector table: lower EL, AArch64). */
    .global el0_sync_entry
el0_sync_entry:
    save_frame
    align_check 1
    bl      linux_kernel_enter          /* the big kernel lock (SMP) */
    mov     x0, sp
    bl      linux_el0_trap
    b       el0_restore

/* IRQ from EL0 (lower EL, AArch64): the scheduler tick (linux.c). The
 * kernel itself runs with IRQs masked, so they only arrive here. */
    .global el0_irq_entry
el0_irq_entry:
    save_frame
    align_check 1
    bl      linux_kernel_enter
    mov     x0, sp
    bl      linux_el0_irq
el0_restore:                            /* sp = trap frame */
    bl      linux_kernel_exit           /* idle CPUs kicked, the lock given up: nothing in the kernel after this */
    align_check 0

    add     x9, sp, #784
    ldp     x10, x11, [x9]
    msr     fpsr, x10
    msr     fpcr, x11
    add     x9, sp, #272
    ldp     q0, q1, [x9, #0]
    ldp     q2, q3, [x9, #32]
    ldp     q4, q5, [x9, #64]
    ldp     q6, q7, [x9, #96]
    ldp     q8, q9, [x9, #128]
    ldp     q10, q11, [x9, #160]
    ldp     q12, q13, [x9, #192]
    ldp     q14, q15, [x9, #224]
    ldp     q16, q17, [x9, #256]
    ldp     q18, q19, [x9, #288]
    ldp     q20, q21, [x9, #320]
    ldp     q22, q23, [x9, #352]
    ldp     q24, q25, [x9, #384]
    ldp     q26, q27, [x9, #416]
    ldp     q28, q29, [x9, #448]
    ldp     q30, q31, [x9, #480]
    ldp     x9, x10, [sp, #256]
    msr     elr_el1, x9
    msr     spsr_el1, x10
    ldr     x9, [sp, #248]
    msr     sp_el0, x9
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    add     sp, sp, #TF_SIZE
    eret
