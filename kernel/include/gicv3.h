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
 * QEMU virt (gic-version=3) addresses; each CPU its own redistributor.
 */

#ifndef GICV3_H
#define GICV3_H

#define GICV3_PPI_HYP_TIMER   26
#define GICV3_PPI_VIRT_TIMER  27
#define GICV3_PPI_PHYS_TIMER  30   /* EL1 physical timer: the Linux-ABI scheduler tick */

/* Returns 0 on success, <0 if there's no GICv3 at the virt address. */
#define GICV3_SGI_WAKE        1    /* SGI: "something is runnable" to an idle CPU (SMP) */

int  gicv3_init(void);
void gicv3_cpu_init(void);              /* this CPU's redistributor and CPU interface */
void gicv3_sgi_others(void);            /* the wake-up SGI to every other CPU */
unsigned gicv3_drain(void);             /* pending interrupts taken and ended (after a masked WFI) */
int  gicv3_available(void);
void gicv3_ppi_enable(unsigned int intid, int enable);
/* Route a PPI to this CPU as a Group 1 IRQ and enable or disable it,
 * bringing the GIC up first if nobody has (used outside the hypervisor). */
void gicv3_ppi_setup(unsigned int intid, int enable);

#endif /* GICV3_H */
