/**
 * @file vgic.c
 * @brief Software-emulated GICv2 distributor + CPU interface (see vgic.h)
 */

#include "vgic.h"
#include "string.h"

/* GICD */
#define GICD_CTLR         0x000
#define GICD_TYPER        0x004
#define GICD_IIDR         0x008
#define GICD_IGROUPR      0x080
#define GICD_ISENABLER    0x100
#define GICD_ICENABLER    0x180
#define GICD_ISPENDR      0x200
#define GICD_ICPENDR      0x280
#define GICD_ISACTIVER    0x300
#define GICD_ICACTIVER    0x380
#define GICD_IPRIORITYR   0x400
#define GICD_ITARGETSR    0x800
#define GICD_ICFGR        0xC00
#define GICD_SGIR         0xF00
#define GICD_PIDR2        0xFE8

/* GICC */
#define GICC_CTLR         0x000
#define GICC_PMR          0x004
#define GICC_BPR          0x008
#define GICC_IAR          0x00C
#define GICC_EOIR         0x010
#define GICC_RPR          0x014
#define GICC_HPPIR        0x018
#define GICC_IIDR         0x0FC
#define GICC_DIR          0x1000

#define BANK_BYTES        ((VGIC_NR_IRQS + 7) / 8)   /* bytes of 1-bit-per-IRQ registers */

void vgic_init(struct vgic *g)
{
    memset(g, 0, sizeof(*g));
    /* SGIs/PPIs are always enabled-capable; SGIs read as edge (0b10) */
    g->icfgr[0] = 0xAAAAAAAAu;
}

void vgic_set_line(struct vgic *g, uint32_t intid, int level)
{
    if (intid >= VGIC_NR_IRQS)
        return;
    if (level)
        g->line |= 1UL << intid;
    else
        g->line &= ~(1UL << intid);
}

static uint64_t pending(const struct vgic *g)
{
    return g->line | g->soft_pending;
}

/* Highest-priority (lowest value) enabled, pending, not-active IRQ that
 * the priority mask lets through - or VGIC_SPURIOUS. */
static uint32_t best_irq(const struct vgic *g)
{
    uint64_t cand = pending(g) & g->enabled & ~g->active;
    uint32_t best = VGIC_SPURIOUS;
    uint32_t best_prio = 0x100;
    for (uint32_t i = 0; i < VGIC_NR_IRQS; i++) {
        if (!((cand >> i) & 1))
            continue;
        if (g->priority[i] >= g->gicc_pmr)
            continue;
        if (g->priority[i] < best_prio) {
            best_prio = g->priority[i];
            best = i;
        }
    }
    return best;
}

int vgic_irq_ready(const struct vgic *g)
{
    if (!(g->gicd_ctlr & 1) || !(g->gicc_ctlr & 1) || g->active)
        return 0;
    return best_irq(g) != VGIC_SPURIOUS;
}

int vgic_any_pending(const struct vgic *g)
{
    return (pending(g) & g->enabled) != 0;
}

/* 1-bit-per-IRQ set/clear registers: GICD_IS*R / IC*R */
static uint64_t bank_mask(uint64_t off, uint64_t base, uint64_t val32)
{
    uint64_t word = (off - base) / 4;
    return word < 2 ? val32 << (32 * word) : 0;
}

static uint32_t bank_read(uint64_t off, uint64_t base, uint64_t bits)
{
    uint64_t word = (off - base) / 4;
    return word < 2 ? (uint32_t)(bits >> (32 * word)) : 0;
}

static int in(uint64_t off, uint64_t base, uint64_t size)
{
    return off >= base && off < base + size;
}

static void gicd_access(struct vgic *g, uint64_t off, int is_write, uint32_t len, uint64_t *data)
{
    uint64_t v = *data;

    /* Byte-array registers: priorities and targets accept any width */
    if (in(off, GICD_IPRIORITYR, VGIC_NR_IRQS)) {
        uint32_t first = (uint32_t)(off - GICD_IPRIORITYR);
        if (is_write) {
            for (uint32_t i = 0; i < len && first + i < VGIC_NR_IRQS; i++)
                g->priority[first + i] = (uint8_t)(v >> (8 * i));
        } else {
            *data = 0;
            for (uint32_t i = 0; i < len && first + i < VGIC_NR_IRQS; i++)
                *data |= (uint64_t)g->priority[first + i] << (8 * i);
        }
        return;
    }
    if (in(off, GICD_ITARGETSR, VGIC_NR_IRQS)) {
        if (!is_write) {   /* one CPU interface: everything targets CPU 0 */
            *data = 0;
            for (uint32_t i = 0; i < len; i++)
                *data |= 1UL << (8 * i);
        }
        return;
    }

    off &= ~3UL;
    if (is_write) {
        if (off == GICD_CTLR)
            g->gicd_ctlr = v & 1;
        else if (in(off, GICD_ISENABLER, BANK_BYTES))
            g->enabled |= bank_mask(off, GICD_ISENABLER, v);
        else if (in(off, GICD_ICENABLER, BANK_BYTES))
            g->enabled &= ~bank_mask(off, GICD_ICENABLER, v);
        else if (in(off, GICD_ISPENDR, BANK_BYTES))
            g->soft_pending |= bank_mask(off, GICD_ISPENDR, v);
        else if (in(off, GICD_ICPENDR, BANK_BYTES))
            g->soft_pending &= ~bank_mask(off, GICD_ICPENDR, v);
        else if (in(off, GICD_ISACTIVER, BANK_BYTES))
            g->active |= bank_mask(off, GICD_ISACTIVER, v);
        else if (in(off, GICD_ICACTIVER, BANK_BYTES))
            g->active &= ~bank_mask(off, GICD_ICACTIVER, v);
        else if (in(off, GICD_ICFGR, VGIC_NR_IRQS / 4) && off >= GICD_ICFGR + 8)
            g->icfgr[(off - GICD_ICFGR) / 4] = (uint32_t)v;   /* SPIs only; inputs stay level */
        /* IGROUPR, SGIR, everything else: write-ignored */
        return;
    }

    switch (off) {
    case GICD_CTLR:  *data = g->gicd_ctlr; return;
    case GICD_TYPER: *data = (VGIC_NR_IRQS / 32) - 1; return;   /* ITLinesNumber, 1 CPU */
    case GICD_IIDR:  *data = 0x0200043B; return;                /* ARM, GIC-400-like */
    case GICD_PIDR2: *data = 0x20; return;                      /* ArchRev 2 */
    }
    if (in(off, GICD_ISENABLER, BANK_BYTES))      *data = bank_read(off, GICD_ISENABLER, g->enabled);
    else if (in(off, GICD_ICENABLER, BANK_BYTES)) *data = bank_read(off, GICD_ICENABLER, g->enabled);
    else if (in(off, GICD_ISPENDR, BANK_BYTES))   *data = bank_read(off, GICD_ISPENDR, pending(g));
    else if (in(off, GICD_ICPENDR, BANK_BYTES))   *data = bank_read(off, GICD_ICPENDR, pending(g));
    else if (in(off, GICD_ISACTIVER, BANK_BYTES)) *data = bank_read(off, GICD_ISACTIVER, g->active);
    else if (in(off, GICD_ICACTIVER, BANK_BYTES)) *data = bank_read(off, GICD_ICACTIVER, g->active);
    else if (in(off, GICD_ICFGR, VGIC_NR_IRQS / 4)) *data = g->icfgr[(off - GICD_ICFGR) / 4];
    else *data = 0;   /* IGROUPR (all group 0) and reserved space: RAZ */
}

static void gicc_access(struct vgic *g, uint64_t off, int is_write, uint64_t *data)
{
    off &= ~3UL;
    if (is_write) {
        uint32_t v = (uint32_t)*data;
        switch (off) {
        case GICC_CTLR: g->gicc_ctlr = v & 1; break;
        case GICC_PMR:  g->gicc_pmr = v & 0xFF; break;
        case GICC_BPR:  g->gicc_bpr = v & 7; break;
        case GICC_EOIR: /* EOImode 0: drop priority and deactivate together */
        case GICC_DIR:
            if ((v & 0x3FF) < VGIC_NR_IRQS)
                g->active &= ~(1UL << (v & 0x3FF));
            break;
        }
        return;
    }

    switch (off) {
    case GICC_CTLR: *data = g->gicc_ctlr; break;
    case GICC_PMR:  *data = g->gicc_pmr; break;
    case GICC_BPR:  *data = g->gicc_bpr; break;
    case GICC_IAR: {
        uint32_t irq = (g->gicc_ctlr & 1) && (g->gicd_ctlr & 1) && !g->active
                     ? best_irq(g) : VGIC_SPURIOUS;
        if (irq != VGIC_SPURIOUS) {
            g->active |= 1UL << irq;
            g->soft_pending &= ~(1UL << irq);   /* a level line stays pending while high */
        }
        *data = irq;
        break;
    }
    case GICC_RPR: {
        uint32_t prio = 0xFF;
        for (uint32_t i = 0; i < VGIC_NR_IRQS; i++)
            if (((g->active >> i) & 1) && g->priority[i] < prio)
                prio = g->priority[i];
        *data = prio;
        break;
    }
    case GICC_HPPIR: *data = best_irq(g); break;
    case GICC_IIDR:  *data = 0x0202043B; break;
    default:         *data = 0; break;
    }
}

int vgic_mmio(struct vgic *g, uint64_t addr, int is_write, uint32_t len, uint64_t *data)
{
    if (addr >= VGIC_GICD_BASE && addr < VGIC_GICD_BASE + VGIC_GICD_SIZE) {
        gicd_access(g, addr - VGIC_GICD_BASE, is_write, len, data);
        return 0;
    }
    if (addr >= VGIC_GICC_BASE && addr < VGIC_GICC_BASE + VGIC_GICC_SIZE) {
        gicc_access(g, addr - VGIC_GICC_BASE, is_write, data);
        return 0;
    }
    return -1;
}
