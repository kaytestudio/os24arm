/**
 * @file mmu.c
 * @brief W^X-hardened ARM64 MMU bring-up for the QEMU virt target
 *
 * With the MMU disabled (SCTLR_EL1.M = 0, this kernel's state before this
 * file existed - see boot.S), the ARMv8-A architecture forces every
 * single memory access to behave as Device-nGnRnE: strictly ordered, no
 * caching, no gathering - for RAM just as much as for a UART register.
 * So this kernel used to run with its data cache permanently off, even
 * for its own code/heap/stack.
 *
 * This sets up identity-mapped translation tables (VA == PA everywhere -
 * there is still no virtual memory, no address-space separation, no
 * per-process isolation; every address means exactly what it did with
 * the MMU off) with two levels: a level-1 table of 1GB blocks for the
 * coarse map, and one level-2 table of 2MB blocks for the [1GB, 2GB)
 * region that actually holds code, so each known code region can be
 * marked read-execute (RX) and everything else read-write,
 * never-execute (RW+NX) - real W^X, not one flat RWX region.
 *
 *   L1[0]  [0x00000000, 0x40000000)  Device-nGnRnE, XN   - low peripherals
 *                                                           (GICv3, PL011
 *                                                           UART at
 *                                                           0x09000000)
 *   L1[1]  [0x40000000, 0x80000000)  -> L2 table (below) - kernel + LX
 *                                                           apps + DLL
 *   L1[2]  [0x80000000, 0xC0000000)  Normal, WB, RW+NX   - unused RAM
 *                                       headroom (blocks 1+2 = 2GB,
 *                                       matching this project's `-m 2G`
 *                                       QEMU invocation)
 *
 *   L1[256] [0x40_0000_0000, +1GB) Device-nGnRnE, XN  - high PCIe ECAM
 *
 * Everything else from 0xC0000000 up is left unmapped - not backed by RAM
 * under `-m 2G`, so an access there should fault, not silently succeed.
 *
 * L2 (2MB blocks, [0x40000000, 0x80000000)) defaults every entry to
 * RW+NX, then carves out the three regions this project's own toolchain
 * actually places code in as RX:
 *   - kernel .text+.rodata      [0x40000000, +2MB)     (kernel/linker.ld
 *                                                        ASSERTs it fits)
 *   - LX app code (CMD.EXE etc) [LX_LOAD_BASE, +LX_CODE_REGION_SIZE)
 *   - DOSCALLS.DLL code         [LX_DLL_LOAD_BASE, +LX_DLL_CODE_REGION_SIZE)
 * Everything else in that 1GB - kernel .data/.bss/heap/stack, LX app
 * data, the LX app's own private heap (KAYTEVM.EXE), DOSCALLS.DLL's data
 * and its own private heap, and any unused gaps between them - is RW+NX.
 * 2MB is coarser than a real OS would use (can't separate, say, the LX
 * app's data from its private heap), but every *code* region is now
 * genuinely never writable, and everything writable is genuinely never
 * executable.
 */

#include "smp.h"
#include "mmu.h"
#include "types.h"
#include "lx_loader.h"
#include "lx_dll_loader.h"

/* types.h leaves uint64_t undefined under this kernel's -nostdinc build
 * (same workaround as fs/btrfs/btrfs_disk.h, kernel/src/lx_loader.c,
 * kernel/src/lx_dll_loader.c). */
#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#define MMU_BLOCK_1GB (1UL << 30)
#define MMU_BLOCK_2MB (1UL << 21)

/* Single level-1 table: with TCR_EL1.T0SZ = 25 (39-bit VA space) and a
 * 4KB granule, the walk starts at level 1, where each of the 512 entries
 * covers 1GB - so this table alone can describe up to 512GB with no
 * level-0 table needed. We only ever populate the first few entries. */
#define MMU_L1_ENTRIES 512
__attribute__((aligned(4096)))
static uint64_t mmu_l1_table[MMU_L1_ENTRIES];

/* Level-2 table for L1[1] = [0x40000000, 0x80000000) - the one 1GB
 * region that needs mixed (code vs data) permissions. */
#define MMU_L2_ENTRIES 512
#define MMU_L2_BASE 0x40000000UL
__attribute__((aligned(4096)))
static uint64_t mmu_l2_ram[MMU_L2_ENTRIES];

/* ── MAIR_EL1 attribute encodings ─────────────────────────────────────── */
#define MAIR_ATTR_DEVICE_NGNRNE 0x00UL /* index 0: strictly-ordered, uncached */
#define MAIR_ATTR_NORMAL_WB     0xFFUL /* index 1: Normal, Inner+Outer WB RWA cacheable */
#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1
#define MAIR_VALUE ((MAIR_ATTR_NORMAL_WB << (8 * MAIR_IDX_NORMAL)) | \
                    (MAIR_ATTR_DEVICE_NGNRNE << (8 * MAIR_IDX_DEVICE)))

/* ── Block/table descriptor bits (stage 1, 4KB granule) ────────────────── */
#define MMU_DESC_BLOCK    (1UL << 0)          /* [1:0] = 0b01: block (leaf, level<3) */
#define MMU_DESC_TABLE    (3UL << 0)          /* [1:0] = 0b11: table (points to next level) */
#define MMU_DESC_AF       (1UL << 10)         /* access flag - must be set, we don't manage it in software */
#define MMU_DESC_SH_INNER (3UL << 8)          /* SH[1:0] = 0b11: inner shareable */
#define MMU_DESC_PXN      (1UL << 53)         /* privileged execute-never */
#define MMU_DESC_UXN      (1UL << 54)         /* unprivileged execute-never */
#define MMU_ATTRINDX(i)   ((uint64_t)(i) << 2) /* [4:2] = MAIR index */

/* Device memory: never executable, no shareability domain (SH bits left
 * 0 - the conventional "outer shareable" default for Device memory isn't
 * meaningful here with a single core, but SH=00 is the safe/simple
 * choice). */
#define MMU_ATTRS_DEVICE (MMU_DESC_BLOCK | MMU_ATTRINDX(MAIR_IDX_DEVICE) | \
                          MMU_DESC_AF | MMU_DESC_PXN | MMU_DESC_UXN)

/* Normal RAM, writable, never executable - the default for basically
 * everything (data, bss, heaps, stack): W^X's "not X" half. */
#define MMU_ATTRS_NORMAL_RW_NX (MMU_DESC_BLOCK | MMU_ATTRINDX(MAIR_IDX_NORMAL) | \
                                MMU_DESC_AF | MMU_DESC_SH_INNER | \
                                MMU_DESC_PXN | MMU_DESC_UXN)

/* Normal RAM, executable, never separately writable at this same
 * mapping - W^X's "not W" half (there is no explicit read-only bit set
 * here; AP stays at its default RW-at-EL1 encoding (0b00), since this
 * kernel never runs anything at EL0 - but it is never *also* granted
 * execute permission alongside a data region, which is the property that
 * actually matters for W^X). */
#define MMU_ATTRS_NORMAL_RX (MMU_DESC_BLOCK | MMU_ATTRINDX(MAIR_IDX_NORMAL) | \
                             MMU_DESC_AF | MMU_DESC_SH_INNER)

/* ── User address spaces (see mmu.h) ──────────────────────────────────────
 * Each address space has its own level-1 and level-2 tables - copies of
 * the kernel's - whose user-window entries point to its own level-3
 * tables. Those map user pages to frames from the physical pool, which
 * the kernel keeps identity-mapped (so it can reach any frame by its
 * physical address, e.g. to copy a parent's pages for fork). The kernel's
 * own tables leave the user window unmapped. */
#define MMU_PAGE          4096UL
#define MMU_DESC_PAGE     (3UL << 0)          /* level 3: [1:0] = 0b11 page */
#define MMU_AP_EL0_RW     (1UL << 6)          /* AP[2:1] = 01: EL1+EL0 read/write */
#define MMU_AP_RO_ALL     (3UL << 6)          /* AP[2:1] = 11: EL1+EL0 read-only */
#define MMU_PA_MASK       0x0000FFFFFFFFF000UL
#define MMU_WIN_L3S       (MMU_USER_SIZE / MMU_BLOCK_2MB)      /* 32 */
#define MMU_HI_L3S        (MMU_HI_SIZE / MMU_BLOCK_2MB)        /* 2048 */
#define MMU_HI_L1S        (MMU_HI_SIZE / MMU_BLOCK_1GB)        /* 4 */
#define MMU_HI_L1_FIRST   (MMU_HI_BASE / MMU_BLOCK_1GB)        /* L1[4..7] */
#define MMU_LX_L3S        (MMU_LX_SIZE / MMU_BLOCK_2MB)        /* 32 */
#define MMU_LX_FIRST      (MMU_WIN_L3S + MMU_HI_L3S)           /* its slots, after the others */
#define MMU_USER_L3S      (MMU_WIN_L3S + MMU_HI_L3S + MMU_LX_L3S)
#define MMU_LX_L2_FIRST   ((MMU_LX_BASE - MMU_L2_BASE) / MMU_BLOCK_2MB)
#define MMU_USER_L2_FIRST ((MMU_USER_BASE - MMU_L2_BASE) / MMU_BLOCK_2MB)
#define POOL_PAGES        ((MMU_POOL_END - MMU_POOL_BASE) / MMU_PAGE)
#define MAX_AS            32

#define MMU_SW_SHARED     (1UL << 55)         /* software bit: frame shared, not copied by fork */

static uint8_t pool_bitmap[POOL_PAGES / 8];   /* bit set = frame in use */
static uint16_t frame_refs[POOL_PAGES];       /* references to single frames (mmu_frame_get/put) */
static unsigned long pool_free = POOL_PAGES, pool_hint;

static void zero_page(void *p)
{
    uint64_t *q = p;
    for (unsigned i = 0; i < MMU_PAGE / 8; i++) q[i] = 0;
}

void *mmu_frames_alloc(unsigned long n)
{
    if (!n || n > pool_free) return 0;
    for (unsigned long pass = 0; pass < 2; pass++)
        for (unsigned long i = pass ? 0 : pool_hint; i + n <= POOL_PAGES; i++) {
            unsigned long k = 0;
            while (k < n && !(pool_bitmap[(i + k) / 8] & (1u << ((i + k) % 8)))) k++;
            if (k < n) { i += k; continue; }
            for (k = 0; k < n; k++) pool_bitmap[(i + k) / 8] |= (uint8_t)(1u << ((i + k) % 8));
            pool_free -= n;
            pool_hint = i + n;
            void *p = (void *)(MMU_POOL_BASE + i * MMU_PAGE);
            for (k = 0; k < n; k++) { zero_page((uint8_t *)p + k * MMU_PAGE); frame_refs[i + k] = 1; }
            return p;
        }
    return 0;
}

void mmu_frames_free(void *p, unsigned long n)
{
    unsigned long i = ((unsigned long)(uintptr_t)p - MMU_POOL_BASE) / MMU_PAGE;
    if ((unsigned long)(uintptr_t)p < MMU_POOL_BASE || i + n > POOL_PAGES) return;
    for (unsigned long k = 0; k < n; k++)
        if (pool_bitmap[(i + k) / 8] & (1u << ((i + k) % 8))) {
            pool_bitmap[(i + k) / 8] &= (uint8_t)~(1u << ((i + k) % 8));
            frame_refs[i + k] = 0;
            pool_free++;
        }
    if (i < pool_hint) pool_hint = i;
}

unsigned long mmu_frames_free_count(void) { return pool_free; }
unsigned long mmu_frames_total_count(void) { return POOL_PAGES; }

static long frame_index(unsigned long pa)
{
    if (pa < MMU_POOL_BASE || pa >= MMU_POOL_END) return -1;
    return (long)((pa - MMU_POOL_BASE) / MMU_PAGE);
}

void mmu_frame_get(void *frame)
{
    long i = frame_index((unsigned long)(uintptr_t)frame);
    if (i >= 0 && frame_refs[i] && frame_refs[i] < 0xFFFF) frame_refs[i]++;
}

void mmu_frame_put(void *frame)
{
    long i = frame_index((unsigned long)(uintptr_t)frame);
    if (i < 0 || !frame_refs[i]) return;
    if (--frame_refs[i] == 0) mmu_frames_free(frame, 1);
}

/* An address space: its own L1 and L2 (copies of the kernel's), whose
 * user entries point to its own tables: the 64 MB window through the L2
 * that covers [1 GB, 2 GB), the high region (MMU_HI_BASE, 4 GB, L1[4..7]
 * - above the 2 GB of RAM, unmapped in the kernel's tables) through L2s of
 * its own. Level-3 tables come and go per 2 MB slot: l3[0..31] the
 * window, l3[32..] the high region. */
struct mmu_as {
    int       used;
    uint64_t *l1, *l2;                        /* frames */
    uint64_t *hl2[MMU_HI_L1S];                /* high region L2s, NULL = nothing there */
    uint64_t *l3[MMU_USER_L3S];               /* frames, NULL = nothing mapped there */
};
static mmu_as_t as_pool[MAX_AS];
/* Each CPU's active address space (SMP: kernel/include/smp.h) */
static mmu_as_t *as_active_cpu[SMP_MAX_CPUS];
#define as_active (as_active_cpu[smp_cpu()])

static void mmu_tlb_flush(void)                  /* this CPU's */
{
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
}

/* as's mappings changed: every CPU running it forgets the old ones (the
 * inner-shareable TLBI reaches them all - another thread of the process
 * may be running on one). */
static void mmu_tlb_flush_as(mmu_as_t *as)
{
    for (int i = 0; i < SMP_MAX_CPUS; i++)
        if (as_active_cpu[i] == as) {
            __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
            return;
        }
}

static uint64_t page_attrs(int prot)
{
    uint64_t a = MMU_DESC_PAGE | MMU_ATTRINDX(MAIR_IDX_NORMAL) | MMU_DESC_AF |
                 MMU_DESC_SH_INNER | MMU_DESC_PXN;               /* never EL1-executable */
    if (prot & MMU_PROT_WRITE) a |= MMU_AP_EL0_RW | MMU_DESC_UXN; /* W^X */
    else a |= MMU_AP_RO_ALL | ((prot & MMU_PROT_EXEC) ? 0 : MMU_DESC_UXN);
    return a;
}

int mmu_user_range(unsigned long start, unsigned long end)
{
    if (end < start) return 0;
    return (start >= MMU_USER_BASE && end <= MMU_USER_BASE + MMU_USER_SIZE) ||
           (start >= MMU_HI_BASE && end <= MMU_HI_BASE + MMU_HI_SIZE) ||
           (start >= MMU_LX_BASE && end <= MMU_LX_BASE + MMU_LX_SIZE);
}

/* 2 MB slot of a user address, or -1 */
static long slot_of(unsigned long va)
{
    if (va >= MMU_USER_BASE && va < MMU_USER_BASE + MMU_USER_SIZE)
        return (long)((va - MMU_USER_BASE) / MMU_BLOCK_2MB);
    if (va >= MMU_HI_BASE && va < MMU_HI_BASE + MMU_HI_SIZE)
        return (long)(MMU_WIN_L3S + (va - MMU_HI_BASE) / MMU_BLOCK_2MB);
    if (va >= MMU_LX_BASE && va < MMU_LX_BASE + MMU_LX_SIZE)
        return (long)(MMU_LX_FIRST + (va - MMU_LX_BASE) / MMU_BLOCK_2MB);
    return -1;
}

static unsigned long slot_va(unsigned long t)
{
    if (t >= MMU_LX_FIRST) return MMU_LX_BASE + (t - MMU_LX_FIRST) * MMU_BLOCK_2MB;
    return t < MMU_WIN_L3S ? MMU_USER_BASE + t * MMU_BLOCK_2MB
                           : MMU_HI_BASE + (t - MMU_WIN_L3S) * MMU_BLOCK_2MB;
}

static uint64_t *pte(mmu_as_t *as, unsigned long va, int create)
{
    long t = as ? slot_of(va) : -1;
    if (t < 0) return 0;
    if (!as->l3[t]) {
        if (!create) return 0;
        uint64_t *l2;
        unsigned long idx;
        if (t < (long)MMU_WIN_L3S) {
            l2 = as->l2;
            idx = MMU_USER_L2_FIRST + (unsigned long)t;
        } else if (t >= (long)MMU_LX_FIRST) {          /* (replaces the kernel's own 2 MB block) */
            l2 = as->l2;
            idx = MMU_LX_L2_FIRST + (unsigned long)(t - MMU_LX_FIRST);
        } else {
            unsigned long h = (unsigned long)(t - MMU_WIN_L3S);
            unsigned long g = h / 512;                 /* which GB */
            if (!as->hl2[g]) {
                if (!(as->hl2[g] = mmu_frames_alloc(1))) return 0;
                as->l1[MMU_HI_L1_FIRST + g] = (uint64_t)(uintptr_t)as->hl2[g] | MMU_DESC_TABLE;
            }
            l2 = as->hl2[g];
            idx = h % 512;
        }
        if (!(as->l3[t] = mmu_frames_alloc(1))) return 0;
        l2[idx] = (uint64_t)(uintptr_t)as->l3[t] | MMU_DESC_TABLE;
    }
    return &as->l3[t][(va % MMU_BLOCK_2MB) / MMU_PAGE];
}

mmu_as_t *mmu_as_create(void)
{
    mmu_as_t *as = 0;
    for (int i = 0; i < MAX_AS; i++) if (!as_pool[i].used) { as = &as_pool[i]; break; }
    if (!as) return 0;
    for (unsigned i = 0; i < MMU_USER_L3S; i++) as->l3[i] = 0;
    for (unsigned i = 0; i < MMU_HI_L1S; i++) as->hl2[i] = 0;
    as->l1 = mmu_frames_alloc(1);
    as->l2 = mmu_frames_alloc(1);
    if (!as->l1 || !as->l2) {
        if (as->l1) mmu_frames_free(as->l1, 1);
        if (as->l2) mmu_frames_free(as->l2, 1);
        return 0;
    }
    for (int i = 0; i < MMU_L1_ENTRIES; i++) as->l1[i] = mmu_l1_table[i];
    for (int i = 0; i < MMU_L2_ENTRIES; i++) as->l2[i] = mmu_l2_ram[i];
    as->l1[1] = (uint64_t)(uintptr_t)as->l2 | MMU_DESC_TABLE;
    for (unsigned i = 0; i < MMU_LX_L3S; i++) as->l2[MMU_LX_L2_FIRST + i] = 0;   /* the LX window: empty */
    as->used = 1;
    return as;
}

void mmu_as_unmap(mmu_as_t *as, unsigned long va, unsigned long len)
{
    unsigned long start = va & ~(MMU_PAGE - 1), end = (va + len + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    for (unsigned long a = start; a < end && a >= start; ) {
        long t = slot_of(a);
        if (t < 0 || !as->l3[t]) {                     /* nothing in this 2 MB: skip it */
            unsigned long next = (a | (MMU_BLOCK_2MB - 1)) + 1;
            if (t < 0 && a < MMU_HI_BASE && end > MMU_HI_BASE && a >= MMU_USER_BASE + MMU_USER_SIZE)
                next = MMU_HI_BASE;
            a = next;
            continue;
        }
        uint64_t *p = &as->l3[t][(a % MMU_BLOCK_2MB) / MMU_PAGE];
        if (*p & 1) {
            mmu_frame_put((void *)(uintptr_t)(*p & MMU_PA_MASK));
            *p = 0;
        }
        a += MMU_PAGE;
    }
    mmu_tlb_flush_as(as);
}

void mmu_as_destroy(mmu_as_t *as)
{
    if (!as || !as->used) return;
    if (as == as_active) mmu_as_activate(0);
    for (unsigned t = 0; t < MMU_USER_L3S; t++) {
        if (!as->l3[t]) continue;
        for (int j = 0; j < 512; j++)
            if (as->l3[t][j] & 1) mmu_frame_put((void *)(uintptr_t)(as->l3[t][j] & MMU_PA_MASK));
        mmu_frames_free(as->l3[t], 1);
        as->l3[t] = 0;
    }
    for (unsigned g = 0; g < MMU_HI_L1S; g++) if (as->hl2[g]) { mmu_frames_free(as->hl2[g], 1); as->hl2[g] = 0; }
    mmu_frames_free(as->l1, 1);
    mmu_frames_free(as->l2, 1);
    as->used = 0;
}

int mmu_as_map(mmu_as_t *as, unsigned long va, unsigned long len, int prot)
{
    unsigned long start = va & ~(MMU_PAGE - 1), end = (va + len + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    if (!mmu_user_range(start, end)) return -1;
    uint64_t attrs = page_attrs(prot);
    for (unsigned long a = start; a < end; a += MMU_PAGE) {
        uint64_t *p = pte(as, a, 1);
        if (!p) return -1;
        uint64_t frame = *p & 1 ? (*p & (MMU_PA_MASK | MMU_SW_SHARED))
                                : (uint64_t)(uintptr_t)mmu_frames_alloc(1);
        if (!frame) { mmu_tlb_flush_as(as); return -1; }
        *p = frame | attrs;
    }
    mmu_tlb_flush_as(as);
    return 0;
}

int mmu_as_map_shared(mmu_as_t *as, unsigned long va, void *const *frames, unsigned long n, int prot)
{
    if ((va & (MMU_PAGE - 1)) || n > MMU_HI_SIZE / MMU_PAGE || !mmu_user_range(va, va + n * MMU_PAGE))
        return -1;
    uint64_t attrs = page_attrs(prot) | MMU_SW_SHARED;
    for (unsigned long k = 0; k < n; k++) {
        uint64_t *p = pte(as, va + k * MMU_PAGE, 1);
        if (!p) { mmu_as_unmap(as, va, k * MMU_PAGE); return -1; }
        if (*p & 1) mmu_frame_put((void *)(uintptr_t)(*p & MMU_PA_MASK));
        mmu_frame_get(frames[k]);
        *p = (uint64_t)(uintptr_t)frames[k] | attrs;
    }
    mmu_tlb_flush_as(as);
    return 0;
}

/* Pages that aren't mapped are skipped (with demand paging, a range is
 * usually only partly present). */
int mmu_as_protect(mmu_as_t *as, unsigned long va, unsigned long len, int prot)
{
    unsigned long start = va & ~(MMU_PAGE - 1), end = (va + len + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    uint64_t attrs = page_attrs(prot);
    for (unsigned long a = start; a < end && a >= start; ) {
        long t = slot_of(a);
        if (t < 0 || !as->l3[t]) { a = (a | (MMU_BLOCK_2MB - 1)) + 1; continue; }
        uint64_t *p = &as->l3[t][(a % MMU_BLOCK_2MB) / MMU_PAGE];
        if (*p & 1) *p = (*p & (MMU_PA_MASK | MMU_SW_SHARED)) | attrs;
        a += MMU_PAGE;
    }
    mmu_tlb_flush_as(as);
    return 0;
}

int mmu_as_query(mmu_as_t *as, unsigned long va)
{
    uint64_t *p = pte(as, va, 0);
    if (!p || !(*p & 1)) return -1;
    int prot = MMU_PROT_READ;
    if ((*p & (3UL << 6)) == MMU_AP_EL0_RW) prot |= MMU_PROT_WRITE;
    if (!(*p & MMU_DESC_UXN)) prot |= MMU_PROT_EXEC;
    return prot;
}

unsigned long mmu_as_resident(mmu_as_t *as)
{
    unsigned long n = 0;
    if (!as) return 0;
    for (unsigned t = 0; t < MMU_USER_L3S; t++)
        if (as->l3[t])
            for (int j = 0; j < 512; j++) if (as->l3[t][j] & 1) n++;
    return n;
}

mmu_as_t *mmu_as_fork(mmu_as_t *parent)
{
    mmu_as_t *as = mmu_as_create();
    if (!as) return 0;
    for (unsigned t = 0; t < MMU_USER_L3S; t++) {
        if (!parent->l3[t]) continue;
        for (int j = 0; j < 512; j++) {
            uint64_t e = parent->l3[t][j];
            if (!(e & 1)) continue;
            unsigned long va = slot_va(t) + (unsigned long)j * MMU_PAGE;
            uint64_t *p = pte(as, va, 1);
            if (p && (e & MMU_SW_SHARED)) {           /* MAP_SHARED: same frame */
                mmu_frame_get((void *)(uintptr_t)(e & MMU_PA_MASK));
                *p = e;
                continue;
            }
            void *frame = mmu_frames_alloc(1);
            if (!p || !frame) { if (frame) mmu_frames_free(frame, 1); mmu_as_destroy(as); return 0; }
            const uint64_t *src = (const uint64_t *)(uintptr_t)(e & MMU_PA_MASK);
            uint64_t *dst = frame;
            for (unsigned i = 0; i < MMU_PAGE / 8; i++) dst[i] = src[i];
            *p = (uint64_t)(uintptr_t)frame | (e & ~MMU_PA_MASK);
        }
    }
    return as;
}

void mmu_as_activate(mmu_as_t *as)
{
    uint64_t root = (uint64_t)(uintptr_t)(as ? as->l1 : mmu_l1_table);
    __asm__ volatile("dsb ish\n\tmsr ttbr0_el1, %0\n\tisb" :: "r"(root) : "memory");
    mmu_tlb_flush();
    as_active = as;
}

mmu_as_t *mmu_as_current(void) { return as_active; }

int mmu_user_mapped(unsigned long va, unsigned long len, int prot)
{
    if (!len) return 1;
    if (va + len < va || !as_active) return 0;
    for (unsigned long a = va & ~(MMU_PAGE - 1); a < va + len; a += MMU_PAGE) {
        uint64_t *p = pte(as_active, a, 0);
        if (!p || !(*p & 1)) return 0;
        if ((prot & MMU_PROT_WRITE) && (*p & (3UL << 6)) != MMU_AP_EL0_RW) return 0;
    }
    return 1;
}

/* Marks 2MB-aligned [start, end) within mmu_l2_ram as RX, overwriting the
 * RW+NX default. start/end must both be 2MB-aligned and within
 * [MMU_L2_BASE, MMU_L2_BASE + 1GB) - true for every region below by
 * construction (see the ASSERTs in kernel/linker.ld, lx/lx_app.ld,
 * lx/lx_dll.ld). */
static void mmu_mark_rx(uint64_t start, uint64_t end)
{
    uint32_t first = (uint32_t)((start - MMU_L2_BASE) / MMU_BLOCK_2MB);
    uint32_t last  = (uint32_t)((end   - MMU_L2_BASE) / MMU_BLOCK_2MB);
    for (uint32_t i = first; i < last && i < MMU_L2_ENTRIES; i++)
        mmu_l2_ram[i] = (MMU_L2_BASE + (uint64_t)i * MMU_BLOCK_2MB) | MMU_ATTRS_NORMAL_RX;
}

/* DEBUG's G and T: code in the 2 MB block holding addr may run (privileged execute-never cleared; it stays
 * writable). 1 if that changed it, 0 if it was executable already, -1 if it isn't RAM this table maps. */
int mmu_kernel_allow_exec(unsigned long addr)
{
    if (addr < MMU_L2_BASE || addr >= MMU_L2_BASE + (unsigned long)MMU_L2_ENTRIES * MMU_BLOCK_2MB) return -1;
    uint32_t i = (uint32_t)((addr - MMU_L2_BASE) / MMU_BLOCK_2MB);
    uint64_t e = mmu_l2_ram[i];
    if (!(e & 1) || (e & 3) != MMU_DESC_BLOCK) return -1;          /* (the user window: not the kernel's) */
    if (!(e & MMU_DESC_PXN)) return 0;
    mmu_l2_ram[i] = e & ~MMU_DESC_PXN;
    __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
    return 1;
}

void mmu_init(void)
{
    /* L2: default every 2MB block in [0x40000000, 0x80000000) to RW+NX,
     * then carve out the known code regions as RX. */
    for (int i = 0; i < MMU_L2_ENTRIES; i++)
        mmu_l2_ram[i] = (MMU_L2_BASE + (uint64_t)i * MMU_BLOCK_2MB) | MMU_ATTRS_NORMAL_RW_NX;

    /* Kernel .text+.rodata - kernel/linker.ld pads .data to start no
     * earlier than the next 2MB boundary and ASSERTs .text+.rodata fit
     * before it, so exactly one block here is always correct. */
    mmu_mark_rx(0x40000000UL, 0x40000000UL + MMU_BLOCK_2MB);

    /* LX app code (CMD.EXE, KAYTEVM.EXE, ...) - lx/lx_app.ld pads/ASSERTs
     * the same way. */
    mmu_mark_rx(LX_LOAD_BASE, LX_LOAD_BASE + LX_CODE_REGION_SIZE);

    /* DOSCALLS.DLL code - lx/lx_dll.ld pads/ASSERTs the same way. */
    mmu_mark_rx(LX_DLL_LOAD_BASE, LX_DLL_LOAD_BASE + LX_DLL_CODE_REGION_SIZE);

    /* User window: unmapped in the kernel's own tables; each address space
     * maps it to its own frames (mmu_as_*). */
    for (unsigned long i = 0; i < MMU_WIN_L3S; i++)
        mmu_l2_ram[MMU_USER_L2_FIRST + i] = 0;

    /* L1 */
    for (int i = 0; i < MMU_L1_ENTRIES; i++)
        mmu_l1_table[i] = 0; /* invalid - faults if ever accessed */

    mmu_l1_table[0] = (0UL * MMU_BLOCK_1GB) | MMU_ATTRS_DEVICE;
    mmu_l1_table[1] = (uint64_t)(uintptr_t)mmu_l2_ram | MMU_DESC_TABLE;
    mmu_l1_table[2] = (2UL * MMU_BLOCK_1GB) | MMU_ATTRS_NORMAL_RW_NX;
    /* [0x40_0000_0000, +1GB): virt's high PCIe ECAM (0x40_1000_0000,
     * the default with highmem=on) - drivers/usb/xhci.c scans it. */
    mmu_l1_table[256] = (256UL * MMU_BLOCK_1GB) | MMU_ATTRS_DEVICE;

    __asm__ volatile("dsb sy" ::: "memory");

    uint64_t mair = MAIR_VALUE;
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    /* PA range must match what this CPU actually supports (ID_AA64MMFR0_EL1
     * bits [3:0]) rather than a hardcoded guess, or TCR_EL1.IPS encodes an
     * invalid/mismatched size. */
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t pa_range = mmfr0 & 0xFUL;

    uint64_t tcr = 25UL              /* T0SZ: 39-bit VA space (512GB), level-1 start */
                 | (1UL << 8)        /* IRGN0 = Write-Back Read/Write-Allocate */
                 | (1UL << 10)       /* ORGN0 = Write-Back Read/Write-Allocate */
                 | (3UL << 12)       /* SH0   = Inner Shareable */
                 | (0UL << 14)       /* TG0   = 4KB granule */
                 | (1UL << 23)       /* EPD1  = 1: no TTBR1 walks (unused - no address-space split) */
                 | (pa_range << 32); /* IPS   = this CPU's real physical address range */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    uint64_t ttbr0 = (uint64_t)(uintptr_t)mmu_l1_table;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0));

    __asm__ volatile("isb");
    /* No prior translations to throw away (MMU has never been on), but a
     * stale TLB entry surviving from any earlier state would be exactly
     * the kind of bug this can't be debugged past - cheap insurance. */
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0)   /* M: MMU enable */
          |  (1UL << 2)   /* C: data cache enable */
          |  (1UL << 12); /* I: instruction cache enable */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");
}
