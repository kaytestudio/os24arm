/**
 * @file vgic.h
 * @brief Software-emulated GICv2 for guests (kernel/src/vgic.c)
 *
 * Trap-and-emulate, not hardware GIC virtualization: the guest's GICD/GICC
 * pages are left unmapped at stage 2, every access is an MMIO exit, and
 * the "is an interrupt deliverable" answer is fed to the guest through
 * the single HCR_EL2.VI line. Slow, but needs nothing from the host GIC -
 * hardware vGIC (GICv3 list registers) can replace it later behind the
 * same vm_irq_line()-style interface.
 *
 * Scope: one CPU interface, 64 interrupt IDs (SGIs/PPIs 0-31 + 32 SPIs),
 * every input level-sensitive, one active interrupt at a time (no
 * preemption between guest interrupts, so BPR/RPR are cosmetic).
 * Addresses follow QEMU's virt board with gic-version=2, which is where a
 * virt-style guest expects them.
 */

#ifndef VGIC_H
#define VGIC_H

#include "types.h"

#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#define VGIC_GICD_BASE   0x08000000UL
#define VGIC_GICD_SIZE   0x10000UL
#define VGIC_GICC_BASE   0x08010000UL
#define VGIC_GICC_SIZE   0x10000UL

#define VGIC_NR_IRQS     64
#define VGIC_SPURIOUS    1023

#define VGIC_IRQ_VTIMER  27   /* PPI 11: EL1 virtual timer */
#define VGIC_IRQ_UART    33   /* SPI 1: PL011, as on virt */

struct vgic {
    uint32_t gicd_ctlr;
    uint32_t gicc_ctlr;
    uint32_t gicc_pmr;
    uint32_t gicc_bpr;
    uint64_t enabled;            /* bit n: GICD_ISENABLER */
    uint64_t line;               /* bit n: input level from the device */
    uint64_t soft_pending;       /* bit n: set via GICD_ISPENDR */
    uint64_t active;             /* bit n: acknowledged, not yet EOI'd */
    uint8_t  priority[VGIC_NR_IRQS];
    uint32_t icfgr[VGIC_NR_IRQS / 16];
};

void vgic_init(struct vgic *g);
void vgic_set_line(struct vgic *g, uint32_t intid, int level);

/* Should HCR_EL2.VI be asserted? (an enabled, pending, unmasked interrupt
 * with nothing active) */
int  vgic_irq_ready(const struct vgic *g);

/* Is there an enabled pending interrupt at all, ignoring the CPU
 * interface's masking - what wakes WFI. */
int  vgic_any_pending(const struct vgic *g);

/* Handles an access if addr is in the GICD/GICC window: returns 0 and
 * fills *data for reads, or -1 if addr isn't ours. */
int  vgic_mmio(struct vgic *g, uint64_t addr, int is_write, uint32_t len, uint64_t *data);

#endif /* VGIC_H */
