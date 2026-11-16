/**
 * @file smp.h
 * @brief The other CPUs (kernel/src/smp.c): starting them, which one this
 * is, and the big kernel lock.
 *
 * Programs (EL0) run on every CPU at once; the kernel itself runs on one
 * at a time - the big kernel lock, taken on every way in from EL0 and in
 * the idle loops' work, given up on every way back to EL0 and while a CPU
 * waits for work. Kernel code runs with interrupts masked and never
 * sleeps holding anything else, so this one lock is all it takes.
 */
#ifndef SMP_H
#define SMP_H

#define SMP_MAX_CPUS 8

/* This CPU's index (0: the boot CPU) - TPIDR_EL1, set at boot. */
static inline unsigned smp_cpu(void)
{
    unsigned long v;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(v));
    return (unsigned)v;
}

extern int smp_ncpus;                 /* CPUs running (1 until smp_init) */

void smp_init(void);                  /* the boot CPU: start the others */
void bkl_lock(void);
void bkl_unlock(void);
int  bkl_mine(void);                  /* held by this CPU */

/* The others' idle loop (kernel/src/linux/linux.c): never returns. */
void linux_cpu_idle(void);

#endif
