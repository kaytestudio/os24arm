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
/* ... and a second, larger user region above RAM, mapped the same way:
 * the mmap area (demand paged by kernel/src/linux). */
#define MMU_HI_BASE    0x100000000UL
#define MMU_HI_SIZE    (4UL << 30)
/* ... and the LX window: where OS/2 programs (CMD.EXE, ...) are linked
 * (lx/lx_app.ld, lx/lx_dll.ld: 0x48000000 the program, 0x4A000000 its
 * DLL), mapped the same way when one runs as a process
 * (kernel/src/linux/lx_user.c). The kernel's own tables keep this range
 * for RUN at the console, which runs them in the kernel. */
#define MMU_LX_BASE    0x48000000UL
#define MMU_LX_SIZE    (64UL * 1024 * 1024)
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
 * their contents and get prot), change the permissions of the pages that
 * are mapped, unmap (frees). Writable pages are never executable. 0 on
 * success. */
int  mmu_as_map(mmu_as_t *as, unsigned long va, unsigned long len, int prot);
int  mmu_as_protect(mmu_as_t *as, unsigned long va, unsigned long len, int prot);
void mmu_as_unmap(mmu_as_t *as, unsigned long va, unsigned long len);

/* Map n given frames at va (page aligned) as shared pages: each gains a
 * reference (mmu_frame_get), fork shares rather than copies them, unmap
 * drops the reference. Used for MAP_SHARED (memfd, shared anonymous). */
int  mmu_as_map_shared(mmu_as_t *as, unsigned long va, void *const *frames, unsigned long n,
                       int prot);

/* Pages present in an address space (for /proc: RSS). */
unsigned long mmu_as_resident(mmu_as_t *as);

/* What's at va: -1 if nothing is mapped, else MMU_PROT_* bits. */
int  mmu_as_query(mmu_as_t *as, unsigned long va);

/* Is [start, end) inside the user regions (the window or the high region)? */
int  mmu_user_range(unsigned long start, unsigned long end);

/* In the active address space: is every page mapped and allowing prot? */
int  mmu_user_mapped(unsigned long va, unsigned long len, int prot);

/* Physical frames (zeroed), e.g. for kernel stacks. */
void         *mmu_frames_alloc(unsigned long n);
void          mmu_frames_free(void *p, unsigned long n);
unsigned long mmu_frames_free_count(void);
unsigned long mmu_frames_total_count(void);
/* Single frames are reference counted (mmu_frames_alloc returns them with
 * one reference; the last mmu_frame_put frees). */
void          mmu_frame_get(void *frame);
void          mmu_frame_put(void *frame);

#endif /* MMU_H */
