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

#endif /* MMU_H */
