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
 * Everything from 0xC0000000 up is left unmapped - not backed by RAM
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

    /* L1 */
    for (int i = 0; i < MMU_L1_ENTRIES; i++)
        mmu_l1_table[i] = 0; /* invalid - faults if ever accessed */

    mmu_l1_table[0] = (0UL * MMU_BLOCK_1GB) | MMU_ATTRS_DEVICE;
    mmu_l1_table[1] = (uint64_t)(uintptr_t)mmu_l2_ram | MMU_DESC_TABLE;
    mmu_l1_table[2] = (2UL * MMU_BLOCK_1GB) | MMU_ATTRS_NORMAL_RW_NX;

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
