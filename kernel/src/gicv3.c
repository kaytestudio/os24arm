/**
 * @file gicv3.c
 * @brief Minimal host GICv3 bring-up for the hypervisor (see gicv3.h)
 */

#include "gicv3.h"
#include "types.h"

#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#define GICD_BASE        0x08000000UL
#define GICR_BASE        0x080A0000UL   /* the first redistributor; each CPU's: by its affinity (GICR_TYPER) */
#define GICR_SGI_OFF     0x10000UL
#define GICR_TYPER       0x0008
#define GICR_SGI_BASE    (rd_base() + GICR_SGI_OFF)        /* this CPU's SGI/PPI frame */
#define GICD_CTLR        0x0000
#define GICD_PIDR2       0xFFE8
#define GICD_CTLR_RWP    (1u << 31)
#define GICD_CTLR_ARE    (1u << 4)
#define GICD_CTLR_GRP1   (1u << 1)
#define GICD_CTLR_GRP0   (1u << 0)

#define GICR_WAKER       0x0014
#define GICR_WAKER_PSLEEP   (1u << 1)
#define GICR_WAKER_CASLEEP  (1u << 2)

#define GICR_IGROUPR0    0x0080
#define GICR_ISENABLER0  0x0100
#define GICR_ICENABLER0  0x0180
#define GICR_IPRIORITYR  0x0400

static int gic_ready;
static uint64_t rd_of[8];                /* each CPU's redistributor (by its index, TPIDR_EL1) */

static unsigned cpu_index(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(v));
    return (unsigned)v & 7;
}

/* This CPU's redistributor: the one whose GICR_TYPER affinity is MPIDR's. */
static uint64_t rd_base(void)
{
    unsigned me = cpu_index();
    if (rd_of[me]) return rd_of[me];
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t aff = (uint32_t)((mpidr & 0xFFFFFF) | ((mpidr >> 8) & 0xFF000000));
    uint64_t a = GICR_BASE;
    for (int k = 0; k < 64; k++) {
        uint64_t typer = *(volatile uint64_t *)(uintptr_t)(a + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff) return rd_of[me] = a;
        if (typer & (1u << 4)) break;                         /* Last */
        a += (typer & (1u << 1)) ? 0x40000 : 0x20000;          /* (VLPIs: two more frames) */
    }
    return rd_of[me] = GICR_BASE;
}


static volatile uint32_t *reg32(uint64_t addr)
{
    return (volatile uint32_t *)(uintptr_t)addr;
}

int gicv3_init(void)
{
    uint32_t arch = (*reg32(GICD_BASE + GICD_PIDR2) >> 4) & 0xF;
    if (arch != 3 && arch != 4)
        return -1;

    /* Affinity routing first (it can't change while groups are enabled),
     * then the groups. Under QEMU's single security state, bits 0/1 are
     * Group 0 / Group 1; with two states they're Group 1 NS / 1A - either
     * way, enabling both covers the Group 1 PPIs used here. */
    volatile uint32_t *ctlr = reg32(GICD_BASE + GICD_CTLR);
    *ctlr = *ctlr | GICD_CTLR_ARE;
    while (*ctlr & GICD_CTLR_RWP)
        ;
    *ctlr = *ctlr | GICD_CTLR_GRP0 | GICD_CTLR_GRP1;
    while (*ctlr & GICD_CTLR_RWP)
        ;

    gicv3_cpu_init();
    /* The slice timer is wanted whenever a guest runs; the virtual timer
     * PPI is switched per entry by hyp.c. */
    *reg32(GICR_SGI_BASE + GICR_ISENABLER0) = 1u << GICV3_PPI_HYP_TIMER;
    gic_ready = 1;
    return 0;
}

/* This CPU's part: its redistributor awake, the timer PPIs and the wake-up SGI in Group 1, its CPU interface on.
 * (Every CPU, the boot one from gicv3_init.) */
void gicv3_cpu_init(void)
{
    /* Wake this CPU's redistributor */
    volatile uint32_t *waker = reg32(rd_base() + GICR_WAKER);
    *waker = *waker & ~GICR_WAKER_PSLEEP;
    while (*waker & GICR_WAKER_CASLEEP)
        ;

    uint32_t ppis = (1u << GICV3_PPI_HYP_TIMER) | (1u << GICV3_PPI_VIRT_TIMER);
    *reg32(GICR_SGI_BASE + GICR_ICENABLER0) = ppis;
    *reg32(GICR_SGI_BASE + GICR_IGROUPR0) |= ppis;            /* Group 1 -> IRQ */
    volatile uint8_t *prio = (volatile uint8_t *)(uintptr_t)(GICR_SGI_BASE + GICR_IPRIORITYR);
    prio[GICV3_PPI_HYP_TIMER]  = 0x80;
    prio[GICV3_PPI_VIRT_TIMER] = 0x80;

    /* CPU interface: system registers (boot.S set ICC_SRE_EL2.Enable) */
    uint64_t sre;
    __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));    /* ICC_SRE_EL1 */
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(sre | 1));
    __asm__ volatile("isb");
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(0xFFUL));   /* ICC_PMR_EL1: let all through */
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"(1UL));    /* ICC_IGRPEN1_EL1 */
    __asm__ volatile("isb");

    *reg32(GICR_SGI_BASE + GICR_IGROUPR0) |= 1u << GICV3_SGI_WAKE;      /* the wake-up SGI */
    prio[GICV3_SGI_WAKE] = 0x80;
    *reg32(GICR_SGI_BASE + GICR_ISENABLER0) = 1u << GICV3_SGI_WAKE;
}

/* The wake-up SGI to every other CPU (ICC_SGI1R_EL1, IRM: all but this one). */
void gicv3_sgi_others(void)
{
    if (!gic_ready) return;
    uint64_t v = (1UL << 40) | ((uint64_t)GICV3_SGI_WAKE << 24);
    __asm__ volatile("dsb ish\n\tmsr S3_0_C12_C11_5, %0\n\tisb" :: "r"(v));
}

/* Interrupts that ended a WFI with them masked, taken and ended here: returns a mask of which (bit n: INTID n < 32). */
unsigned gicv3_drain(void)
{
    unsigned seen = 0;
    if (!gic_ready) return 0;
    for (int k = 0; k < 16; k++) {
        uint64_t intid;
        __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(intid));      /* ICC_IAR1_EL1 */
        intid &= 0xFFFFFF;
        if (intid >= 1020) break;
        if (intid < 32) seen |= 1u << intid;
        __asm__ volatile("msr S3_0_C12_C12_1, %0\n\tisb" :: "r"(intid)); /* ICC_EOIR1_EL1 */
    }
    return seen;
}

int gicv3_available(void)
{
    return gic_ready;
}

void gicv3_ppi_setup(unsigned int intid, int enable)
{
    if (!gic_ready && gicv3_init() != 0)
        return;
    if (intid >= 32)
        return;
    if (enable) {
        *reg32(GICR_SGI_BASE + GICR_IGROUPR0) |= 1u << intid;           /* Group 1 -> IRQ */
        ((volatile uint8_t *)(uintptr_t)(GICR_SGI_BASE + GICR_IPRIORITYR))[intid] = 0x80;
    }
    *reg32(GICR_SGI_BASE + (enable ? GICR_ISENABLER0 : GICR_ICENABLER0)) = 1u << intid;
}

void gicv3_ppi_enable(unsigned int intid, int enable)
{
    if (!gic_ready || intid >= 32)
        return;
    *reg32(GICR_SGI_BASE + (enable ? GICR_ISENABLER0 : GICR_ICENABLER0)) = 1u << intid;
}
