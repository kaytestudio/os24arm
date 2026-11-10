/**
 * @file vm_guest_hello.S
 * @brief Built-in test guest for `VM TEST` (kernel/src/vm_cmd.c)
 *
 * Not part of the kernel's own code: these bytes are copied into guest
 * RAM and run there, at guest EL1 with its MMU off - so it's kept in
 * .rodata (the host never executes it) and written position-independent
 * (adr only), since it runs at whatever IPA it's loaded at. The section
 * is 2KB-aligned so the vector table below keeps VBAR_EL1's alignment
 * once loaded at a 2MB-aligned IPA.
 *
 * Exercises every device the hypervisor gives it, in order:
 *   1. PL011 output (every store is an MMIO exit) and an hvc round trip
 *      (PSCI_VERSION)
 *   2. virtual timer + emulated GICv2: 3 ticks while spinning with no
 *      traps at all - only reachable if the host preempts the guest and
 *      injects the interrupt - then 3 more while sleeping in WFI
 *   3. PL011 receive interrupt: echoes what's typed until Ctrl-D
 * and finally powers off with PSCI SYSTEM_OFF.
 *
 * Register use (never touched by the IRQ handler except x21/x22):
 *   x20 PL011 base, x21 timer ticks, x22 Ctrl-D seen, x23 GICC base,
 *   x24 timer interval in counter ticks
 */

#define PL011_FR      0x18
#define PL011_IMSC    0x38
#define GICD_ISENABLER0 0x100
#define GICC_PMR      0x04
#define GICC_IAR      0x0C
#define GICC_EOIR     0x10
#define IRQ_VTIMER    27
#define IRQ_UART      33
#define TICKS_SPIN    3
#define TICKS_TOTAL   6

.section ".rodata.vm_guest", "a"
.balign 2048

.global vm_guest_hello_start
vm_guest_hello_start:
    b       guest_main

/* ── Vector table: only IRQ from the current EL (SPx) is expected ───── */
.balign 2048
guest_vectors:
    .rept 5
    .balign 128
    b       .                       /* sync/IRQ/FIQ/SError from SP0, sync from SPx */
    .endr
    .balign 128
    b       guest_irq               /* +0x280: IRQ, current EL, SPx */
    .rept 10
    .balign 128
    b       .
    .endr

/* ── Main ─────────────────────────────────────────────────────────────── */
.balign 128
guest_main:
    movz    x20, #0x0900, lsl #16   /* PL011 */
    movz    x23, #0x0801, lsl #16   /* GICC */
    adr     x0, vm_guest_hello_start
    add     x0, x0, #0x200, lsl #12 /* stack at the top of 2MB of RAM */
    mov     sp, x0
    adr     x0, guest_vectors
    msr     vbar_el1, x0
    isb

    adr     x1, msg_hello
    bl      puts

    /* CurrentEL should read 1: we're a guest kernel, not the host. */
    mrs     x0, CurrentEL
    ubfx    x0, x0, #2, #2
    add     w0, w0, #'0'
    adr     x1, msg_el
    strb    w0, [x1, #(msg_el_digit - msg_el)]   /* RAM is writable at stage 2 */
    bl      puts

    /* hvc round trip: PSCI_VERSION should come back as 1.0 */
    movz    x0, #0x8400, lsl #16    /* PSCI_VERSION */
    hvc     #0
    adr     x1, msg_psci_ok
    movz    x2, #0x1, lsl #16
    cmp     x0, x2
    b.eq    1f
    adr     x1, msg_psci_bad
1:  bl      puts

    /* GIC: distributor + CPU interface on, timer and UART interrupts
     * enabled (priority 0, the reset value, beats PMR 0xFF) */
    movz    x9, #0x0800, lsl #16    /* GICD */
    mov     w0, #1
    str     w0, [x9]                /* GICD_CTLR */
    mov     w0, #(1 << IRQ_VTIMER)
    str     w0, [x9, #GICD_ISENABLER0]
    mov     w0, #(1 << (IRQ_UART - 32))
    str     w0, [x9, #(GICD_ISENABLER0 + 4)]
    mov     w0, #0xFF
    str     w0, [x23, #GICC_PMR]
    mov     w0, #1
    str     w0, [x23]               /* GICC_CTLR */

    /* Virtual timer: every 100ms */
    mrs     x24, cntfrq_el0
    mov     x0, #10
    udiv    x24, x24, x0
    mov     x21, #0
    mov     x22, #0
    adr     x1, msg_spin
    bl      puts
    msr     cntv_tval_el0, x24
    mov     x0, #1                  /* ENABLE, not IMASK */
    msr     cntv_ctl_el0, x0
    msr     daifclr, #2             /* take IRQs */

2:  cmp     x21, #TICKS_SPIN        /* no trap in here: only preemption ends it */
    b.lo    2b

    adr     x1, msg_wfi
    bl      puts
3:  wfi
    cmp     x21, #TICKS_TOTAL
    b.lo    3b
    msr     cntv_ctl_el0, xzr       /* timer off */

    adr     x1, msg_echo
    bl      puts
    mov     w0, #((1 << 4) | (1 << 6))   /* UARTIMSC: RXIM | RTIM */
    str     w0, [x20, #PL011_IMSC]
4:  wfi
    cbz     x22, 4b
    msr     daifset, #2

    adr     x1, msg_bye
    bl      puts
    movz    x0, #0x8400, lsl #16
    movk    x0, #0x0008             /* PSCI_SYSTEM_OFF */
    hvc     #0
5:  wfi                             /* not reached */
    b       5b

/* ── IRQ handler ──────────────────────────────────────────────────────── */
guest_irq:
    sub     sp, sp, #48
    stp     x0, x1, [sp]
    stp     x2, x3, [sp, #16]
    stp     x29, x30, [sp, #32]

    ldr     w2, [x23, #GICC_IAR]
    and     w0, w2, #0x3FF
    cmp     w0, #IRQ_VTIMER
    b.eq    irq_tick
    cmp     w0, #IRQ_UART
    b.eq    irq_rx
    b       irq_done

irq_tick:
    add     x21, x21, #1
    adr     x1, msg_tick
    bl      puts
    add     w3, w21, #'0'
    strb    w3, [x20]
    adr     x1, msg_crlf
    bl      puts
    msr     cntv_tval_el0, x24      /* next deadline; drops the line now */
    b       irq_eoi

irq_rx:
    ldr     w3, [x20, #PL011_FR]
    tbnz    w3, #4, irq_eoi         /* RXFE: drained */
    ldr     w3, [x20]               /* DR */
    and     w3, w3, #0xFF
    cmp     w3, #4                  /* Ctrl-D */
    b.ne    1f
    mov     x22, #1
    b       irq_rx
1:  cmp     w3, #'\r'
    b.ne    2f
    strb    w3, [x20]
    mov     w3, #'\n'
2:  strb    w3, [x20]
    b       irq_rx

irq_eoi:
    str     w2, [x23, #GICC_EOIR]
irq_done:                           /* spurious (1023): nothing to EOI */
    ldp     x29, x30, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp]
    add     sp, sp, #48
    eret

/* x1 = NUL-terminated string -> PL011 DR (x20). Clobbers x1, w3. */
puts:
    ldrb    w3, [x1], #1
    cbz     w3, 1f
    strb    w3, [x20]
    b       puts
1:  ret

msg_hello:
    .asciz  "  Hello from the guest!\r\n"
msg_el:
    .ascii  "  Guest is running at EL"
msg_el_digit:
    .asciz  "?\r\n"
msg_psci_ok:
    .asciz  "  PSCI_VERSION over hvc: 1.0\r\n"
msg_psci_bad:
    .asciz  "  PSCI_VERSION over hvc: unexpected\r\n"
msg_spin:
    .asciz  "  Timer: 100ms ticks while spinning (needs host preemption)...\r\n"
msg_wfi:
    .asciz  "  Timer: ticks while sleeping in WFI...\r\n"
msg_tick:
    .asciz  "    tick "
msg_crlf:
    .asciz  "\r\n"
msg_echo:
    .asciz  "  UART RX interrupt: type something, it echoes back. Ctrl-D powers off.\r\n"
msg_bye:
    .asciz  "\r\n  Powering off.\r\n"
    .balign 4
.global vm_guest_hello_end
vm_guest_hello_end:
