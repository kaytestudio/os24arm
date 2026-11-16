/*
 * kernel/src/debug_entry.S - DEBUG's G, P and T (dos/dos_commands.c): run
 * code at EL1 with its own registers until a hardware breakpoint, a single
 * step, a BRK (its return, onto debug_return_trap) or any other
 * synchronous exception brings it back.
 *
 * A context is x0-x30 (0), sp (248), pc (256), pstate (264), esr (272),
 * far (280). debug_go(ctx, step) saves the caller's callee-saved registers
 * and stack (debug_jmp), unlocks the OS lock and enables debug exceptions
 * at EL1 (MDSCR_EL1.KDE, MDE; SS for a step), and erets into ctx: EL1h,
 * A/I/F masked, D clear (so the breakpoints and the step can fire), the
 * context's NZCV. boot.S's EL1 synchronous vector sees debug_trap_active
 * and comes to debug_trap_entry, which stores the registers back into
 * ctx, turns stepping off, and returns from debug_go with the ESR.
 */
    .bss
    .balign 8
    .global debug_trap_active
debug_trap_active:  .word 0
    .balign 8
debug_jmp:          .skip 13 * 8
debug_ctx_ptr:      .skip 8

    .text
    .balign 4
    .global debug_go
debug_go:
    ldr     x2, =debug_jmp
    stp     x19, x20, [x2, #0]
    stp     x21, x22, [x2, #16]
    stp     x23, x24, [x2, #32]
    stp     x25, x26, [x2, #48]
    stp     x27, x28, [x2, #64]
    stp     x29, x30, [x2, #80]
    mov     x3, sp
    str     x3, [x2, #96]
    ldr     x2, =debug_ctx_ptr
    str     x0, [x2]
    msr     oslar_el1, xzr              /* (the OS lock: off, or no debug exceptions) */
    isb
    mrs     x3, mdscr_el1
    orr     x3, x3, #(1 << 13)          /* KDE: debug exceptions at EL1 */
    orr     x3, x3, #(1 << 15)          /* MDE: the breakpoint registers */
    bic     x3, x3, #1
    cbz     x1, 1f
    orr     x3, x3, #1                  /* SS: one instruction */
1:  msr     mdscr_el1, x3
    ldr     x5, [x0, #264]
    and     x5, x5, #0xf0000000         /* (its NZCV) */
    mov     x3, #0x1c5                  /* EL1h, A I F masked, D clear */
    orr     x3, x3, x5
    cbz     x1, 2f
    orr     x3, x3, #(1 << 21)          /* SPSR.SS */
2:  msr     spsr_el1, x3
    ldr     x3, [x0, #256]
    msr     elr_el1, x3
    ldr     x3, =debug_trap_active
    mov     w4, #1
    str     w4, [x3]
    isb
    ldr     x3, [x0, #248]
    mov     sp, x3
    ldp     x2, x3, [x0, #16]
    ldp     x4, x5, [x0, #32]
    ldp     x6, x7, [x0, #48]
    ldp     x8, x9, [x0, #64]
    ldp     x10, x11, [x0, #80]
    ldp     x12, x13, [x0, #96]
    ldp     x14, x15, [x0, #112]
    ldp     x16, x17, [x0, #128]
    ldp     x18, x19, [x0, #144]
    ldp     x20, x21, [x0, #160]
    ldp     x22, x23, [x0, #176]
    ldp     x24, x25, [x0, #192]
    ldp     x26, x27, [x0, #208]
    ldp     x28, x29, [x0, #224]
    ldr     x30, [x0, #240]
    ldp     x0, x1, [x0]
    eret

/* From boot.S's EL1 synchronous vector, x0 and x1 pushed (16 bytes). */
    .global debug_trap_entry
debug_trap_entry:
    ldr     x0, =debug_ctx_ptr
    ldr     x0, [x0]
    stp     x2, x3, [x0, #16]
    stp     x4, x5, [x0, #32]
    stp     x6, x7, [x0, #48]
    stp     x8, x9, [x0, #64]
    stp     x10, x11, [x0, #80]
    stp     x12, x13, [x0, #96]
    stp     x14, x15, [x0, #112]
    stp     x16, x17, [x0, #128]
    stp     x18, x19, [x0, #144]
    stp     x20, x21, [x0, #160]
    stp     x22, x23, [x0, #176]
    stp     x24, x25, [x0, #192]
    stp     x26, x27, [x0, #208]
    stp     x28, x29, [x0, #224]
    str     x30, [x0, #240]
    ldp     x2, x3, [sp]
    stp     x2, x3, [x0]
    add     x2, sp, #16
    str     x2, [x0, #248]
    mrs     x2, elr_el1
    str     x2, [x0, #256]
    mrs     x2, spsr_el1
    str     x2, [x0, #264]
    mrs     x2, esr_el1
    str     x2, [x0, #272]
    mrs     x2, far_el1
    str     x2, [x0, #280]
    ldr     x3, =debug_trap_active
    str     wzr, [x3]
    mrs     x3, mdscr_el1
    bic     x3, x3, #1
    msr     mdscr_el1, x3
    isb
    ldr     x3, =debug_jmp
    ldp     x19, x20, [x3, #0]
    ldp     x21, x22, [x3, #16]
    ldp     x23, x24, [x3, #32]
    ldp     x25, x26, [x3, #48]
    ldp     x27, x28, [x3, #64]
    ldp     x29, x30, [x3, #80]
    ldr     x4, [x3, #96]
    mov     sp, x4
    ldr     x0, [x0, #272]              /* debug_go's result: the ESR */
    ret

/* Where code G runs returns to (its x30 at the start): back to DEBUG. */
    .global debug_return_trap
debug_return_trap:
    brk     #0xdebe
