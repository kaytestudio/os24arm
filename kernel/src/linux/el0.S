/*
 * kernel/src/linux/el0.S - entering and leaving EL0 for the Linux-ABI user
 * mode (see linux.c).
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

/* long el0_run(uint64_t entry, uint64_t user_sp, uint64_t kctx[13])
 * Saves the callee-saved registers and the kernel stack pointer in kctx,
 * then drops to EL0 at entry with sp_el0 = user_sp and every register
 * cleared. Returns (via el0_exit) with the program's exit status. */
    .global el0_run
el0_run:
    stp     x19, x20, [x2, #0]
    stp     x21, x22, [x2, #16]
    stp     x23, x24, [x2, #32]
    stp     x25, x26, [x2, #48]
    stp     x27, x28, [x2, #64]
    stp     x29, x30, [x2, #80]
    mov     x9, sp
    str     x9, [x2, #96]

    msr     elr_el1, x0
    msr     sp_el0, x1
    mov     x9, #0x3c0                  /* EL0t, DAIF masked (no preemption yet) */
    msr     spsr_el1, x9
    msr     fpsr, xzr
    msr     fpcr, xzr
    msr     tpidr_el0, xzr
    align_check 0

    /* No kernel values leak into the program. */
    movi    v0.2d, #0
    movi    v1.2d, #0
    movi    v2.2d, #0
    movi    v3.2d, #0
    movi    v4.2d, #0
    movi    v5.2d, #0
    movi    v6.2d, #0
    movi    v7.2d, #0
    movi    v8.2d, #0
    movi    v9.2d, #0
    movi    v10.2d, #0
    movi    v11.2d, #0
    movi    v12.2d, #0
    movi    v13.2d, #0
    movi    v14.2d, #0
    movi    v15.2d, #0
    movi    v16.2d, #0
    movi    v17.2d, #0
    movi    v18.2d, #0
    movi    v19.2d, #0
    movi    v20.2d, #0
    movi    v21.2d, #0
    movi    v22.2d, #0
    movi    v23.2d, #0
    movi    v24.2d, #0
    movi    v25.2d, #0
    movi    v26.2d, #0
    movi    v27.2d, #0
    movi    v28.2d, #0
    movi    v29.2d, #0
    movi    v30.2d, #0
    movi    v31.2d, #0
    mov     x0, #0
    mov     x1, #0
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0
    eret

/* void el0_exit(uint64_t kctx[13], long status) - from a syscall or fault
 * handler: abandon the trap frame(s) and return from el0_run(). */
    .global el0_exit
el0_exit:
    align_check 1
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x9, [x0, #96]
    mov     sp, x9
    mov     x0, x1
    ret

/* Synchronous exception from EL0 (vector table: lower EL, AArch64). */
    .global el0_sync_entry
el0_sync_entry:
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

    align_check 1
    mov     x0, sp
    bl      linux_el0_trap
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
