/* ============================================================================
 * boot.S - Raspberry Pi 3+ ARM64 Boot Code
 * ============================================================================
 * Boots OS/2 Warp kernel on Raspberry Pi 3/3+ in 64-bit mode
 */

.section ".text.boot"
.global _start

_start:
    // Read CPU ID - only CPU 0 should continue, others park
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, cpu0_boot
    
    // CPU 1-3: Park in WFI loop
cpu_park:
    wfe
    b       cpu_park

cpu0_boot:
    // CPU 0: Initialize and boot kernel
    
    // Set stack pointer before clearing BSS
    // RPi loads kernel at 0x80000, give it 64KB stack
    ldr     x0, =_start
    mov     sp, x0
    
    // Clear BSS section
    ldr     x0, =_bss_start
    ldr     x1, =_bss_end
    sub     x1, x1, x0
    cbz     x1, bss_done
    
bss_loop:
    str     xzr, [x0], #8
    sub     x1, x1, #8
    cbnz    x1, bss_loop
    
bss_done:
    // Enable FPU/NEON
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    
    // Set up exception level (we're in EL2 or EL3)
    // Drop to EL1 if needed
    mrs     x0, CurrentEL
    and     x0, x0, #0xC
    cmp     x0, #0x8
    beq     el2_entry
    cmp     x0, #0x4
    beq     el1_entry
    
    // EL3 -> EL2
    mov     x0, #0x5b1      // RES1 bits
    msr     scr_el3, x0
    mov     x0, #0x3c9      // EL2h with SP_EL2
    msr     spsr_el3, x0
    adr     x0, el2_entry
    msr     elr_el3, x0
    eret
    
el2_entry:
    // EL2 -> EL1
    mov     x0, #(1 << 31)  // AArch64 EL1
    msr     hcr_el2, x0
    mov     x0, #0x3c5      // EL1h with SP_EL1
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret
    
el1_entry:
    bl      kernel_main
    
    // If kernel returns, hang
hang:
    wfe
    b       hang

// Expose BSS symbols
.section ".bss"
.global _bss_start
.global _bss_end
_bss_start:
_bss_end:
