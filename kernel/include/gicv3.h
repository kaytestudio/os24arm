/**
 * @file gicv3.h
 * @brief Minimal host GICv3 bring-up for the hypervisor (kernel/src/gicv3.c)
 *
 * The host kernel itself still runs with interrupts masked and handles
 * none; this only makes two private timer interrupts reach the CPU, so
 * that - with HCR_EL2.IMO set while a guest runs - they exit the guest to
 * EL2:
 *   PPI 26 (INTID 26): EL2 physical timer - the host's time slice
 *   PPI 27 (INTID 27): EL1 virtual timer - the guest's own timer firing
 * Nothing ever acknowledges them: both are level-sensitive, and the
 * world switch drops each line (timer disabled / guest timer swapped
 * out) before the host runs, so their pending state clears by itself.
 *
 * QEMU virt (gic-version=3) addresses, CPU 0's redistributor only.
 */

#ifndef GICV3_H
#define GICV3_H

#define GICV3_PPI_HYP_TIMER   26
#define GICV3_PPI_VIRT_TIMER  27

/* Returns 0 on success, <0 if there's no GICv3 at the virt address. */
int  gicv3_init(void);
int  gicv3_available(void);
void gicv3_ppi_enable(unsigned int intid, int enable);

#endif /* GICV3_H */
