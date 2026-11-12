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

/* ── User (EL0) address spaces ───────────────────────────────────────────
 * Every process sees [MMU_USER_BASE, MMU_USER_BASE + MMU_USER_SIZE) mapped
 * with 4 KB pages to frames of its own, with per-page EL0 permissions;
 * the kernel's own tables leave that window unmapped. Frames come from
 * [MMU_POOL_BASE, MMU_POOL_END), which the kernel keeps identity mapped.
 * Used by the Linux-ABI user mode (kernel/src/linux). */
#define MMU_USER_BASE  0x60000000UL
#define MMU_USER_SIZE  (64UL * 1024 * 1024)
#define MMU_POOL_BASE  0x64000000UL
#define MMU_POOL_END   0x80000000UL

#define MMU_PROT_READ  1
#define MMU_PROT_WRITE 2
#define MMU_PROT_EXEC  4

typedef struct mmu_as mmu_as_t;

mmu_as_t *mmu_as_create(void);              /* empty user window */
mmu_as_t *mmu_as_fork(mmu_as_t *parent);    /* copy of every mapped page */
void      mmu_as_destroy(mmu_as_t *as);     /* frees its frames */
void      mmu_as_activate(mmu_as_t *as);    /* TTBR0; NULL = kernel tables */
mmu_as_t *mmu_as_current(void);

/* [va, va+len), page-rounded: map (new pages zeroed; existing ones keep
 * their contents and get prot), change permissions, unmap (frees). Writable
 * pages are never executable. 0 on success. */
int  mmu_as_map(mmu_as_t *as, unsigned long va, unsigned long len, int prot);
int  mmu_as_protect(mmu_as_t *as, unsigned long va, unsigned long len, int prot);
void mmu_as_unmap(mmu_as_t *as, unsigned long va, unsigned long len);

/* In the active address space: is every page mapped and allowing prot? */
int  mmu_user_mapped(unsigned long va, unsigned long len, int prot);

/* Physical frames (zeroed), e.g. for kernel stacks. */
void         *mmu_frames_alloc(unsigned long n);
void          mmu_frames_free(void *p, unsigned long n);
unsigned long mmu_frames_free_count(void);

#endif /* MMU_H */
