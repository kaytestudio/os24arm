/* ============================================================================
 * bootloader/start.S  —  ARM64 bare-metal entry point
 * Executed by QEMU at the image load address (0x40080000 on virt machine).
 * Clears BSS, sets the stack, and calls kernel_main().
 * ========================================================================== */

.section ".text.boot"
.global _start
.type   _start, %function

_start:
    /* ── Only core 0 does work; park the rest ─────────────────────────── */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF          // affinity level 0
    cbz     x0, .Lprimary          // core 0 → continue
.Lspin:
    wfe
    b       .Lspin

.Lprimary:
    /* Drop to EL1 when QEMU starts us at EL2. */
    mrs     x0, currentel
    lsr     x0, x0, #2
    cmp     x0, #2
    beq     .Ldrop_to_el1
    b       .Lel1_ready

.Ldrop_to_el1:
    /* Minimal EL2→EL1 drop */
    mov     x0, #(1 << 31)         // AArch64 in EL1
    msr     hcr_el2,  x0
    mov     x0, #0x3C5             // DAIF masked, EL1h
    msr     spsr_el2, x0
    adr     x0, .Lel1_ready
    msr     elr_el2,  x0
    eret

.Lel1_ready:
    /* ── Set up stack pointer ─────────────────────────────────────────── */
    ldr     x0, =_stack_top
    mov     sp, x0

    /* ── Zero BSS section ─────────────────────────────────────────────── */
    ldr     x0, =_bss_start
    ldr     x1, =_bss_end
    mov     x2, xzr
.Lzero_bss:
    cmp     x0, x1
    b.ge    .Ldone_bss
    str     x2, [x0], #8
    b       .Lzero_bss
.Ldone_bss:

    /* ── Enable FP / SIMD ─────────────────────────────────────────────── */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    /* ── Jump to C kernel ─────────────────────────────────────────────── */
    bl      kernel_main

    /* ── Should never return — halt ───────────────────────────────────── */
.Lhalt:
    wfe
    b       .Lhalt

.size _start, . - _start
