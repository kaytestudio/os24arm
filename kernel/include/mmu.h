/**
 * @file mmu.h
 * @brief Identity-mapped ARM64 MMU bring-up for the QEMU virt target
 *
 * See mmu.c for the full picture: this enables the MMU (SCTLR_EL1.M) with
 * a single-level, 1GB-block identity map (VA == PA everywhere), purely to
 * get real page-table-backed memory attributes (cacheable Normal memory
 * for RAM, non-cacheable strictly-ordered Device memory for peripherals)
 * instead of running with the MMU off, where the architecture forces
 * every access - RAM included - to be treated as uncached Device memory.
 * It does not introduce virtual memory, address-space separation, or any
 * per-process isolation - addresses mean exactly what they did before.
 */

#ifndef MMU_H
#define MMU_H

/* Enables the MMU. Must run early in boot (see boot.S), after the
 * exception vector table is installed but before any code that benefits
 * from caching (i.e. essentially everything) - identity-mapped, so the
 * program counter keeps working across the transition with no trampoline
 * needed. */
void mmu_init(void);

/* ── User (EL0) window ────────────────────────────────────────────────────
 * [MMU_USER_BASE, MMU_USER_BASE + MMU_USER_SIZE) is mapped with 4KB pages
 * (level-3 tables) so each page gets its own EL0 permissions; every page
 * starts unmapped. Identity mapped like the rest of RAM: VA == PA. Used by
 * the Linux-ABI user mode (kernel/src/linux). */
#define MMU_USER_BASE  0x60000000UL
#define MMU_USER_SIZE  (64UL * 1024 * 1024)

#define MMU_PROT_READ  1
#define MMU_PROT_WRITE 2
#define MMU_PROT_EXEC  4

/* Map / unmap [va, va+len) (page-rounded, inside the window) for EL0 with
 * PROT_* bits. Writable pages are never executable. 0 on success. */
int  mmu_user_map(unsigned long va, unsigned long len, int prot);
void mmu_user_unmap(unsigned long va, unsigned long len);
void mmu_user_clear(void);                 /* unmap the whole window */
int  mmu_user_mapped(unsigned long va, unsigned long len, int prot); /* all pages allow prot? */

#endif /* MMU_H */
