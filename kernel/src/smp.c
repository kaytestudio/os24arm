/**
 * @file smp.c
 * @brief The other CPUs: PSCI CPU_ON, their bring-up, the big kernel lock
 * (see smp.h).
 *
 * Each comes up in boot.S's secondary_start (EL2 set up as the boot CPU's
 * when there is one, then EL1: its vectors, the boot CPU's MMU settings
 * from smp_boot_args, its stack), then smp_secondary_main: its GIC
 * redistributor and CPU interface, and linux.c's idle loop - which runs
 * programs' threads while the Linux-ABI user mode is up, and waits
 * otherwise. Device interrupts stay routed to the boot CPU (they're
 * polled anyway); the others take their own timer tick and the wake-up
 * SGI only.
 */
#include "smp.h"
#include "gicv3.h"
#include "hyp.h"
#include "kprintf.h"
#include <stdint.h>

int smp_ncpus = 1;

/* For secondary_start, read with the MMU off (so cleaned to the point of
 * coherency): MAIR, TCR, TTBR0, SCTLR as the boot CPU has them. */
struct { unsigned long mair, tcr, ttbr0, sctlr; } smp_boot_args __attribute__((aligned(64)));
uint8_t smp_stacks[SMP_MAX_CPUS][32768] __attribute__((aligned(16)));
uint8_t smp_hyp_stacks[SMP_MAX_CPUS][4096] __attribute__((aligned(16)));
static volatile int online[SMP_MAX_CPUS];

extern void secondary_start(void);

#define PSCI_CPU_ON64      0xC4000003UL
#define PSCI_INVALID_PARAM (-2L)

/* QEMU's PSCI: by smc when this kernel booted at EL2 (QEMU's conduit with
 * EL2 and no EL3), by hvc otherwise (QEMU or HVF owning EL2). */
static long psci(unsigned long fn, unsigned long a1, unsigned long a2, unsigned long a3)
{
    register unsigned long x0 __asm__("x0") = fn, x1 __asm__("x1") = a1,
                           x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
    if (hyp_boot_el == 2)
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (long)x0;
}

void smp_secondary_main(unsigned long idx)
{
    gicv3_cpu_init();
    __atomic_store_n(&online[idx], 1, __ATOMIC_RELEASE);
    linux_cpu_idle();
}

void smp_init(void)
{
    if (!gicv3_available() && gicv3_init() != 0) return;   /* (GICv2: one CPU) */
    __asm__ volatile("mrs %0, mair_el1\n\tmrs %1, tcr_el1\n\tmrs %2, ttbr0_el1\n\tmrs %3, sctlr_el1"
                     : "=r"(smp_boot_args.mair), "=r"(smp_boot_args.tcr),
                       "=r"(smp_boot_args.ttbr0), "=r"(smp_boot_args.sctlr));
    __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"(&smp_boot_args) : "memory");
    for (unsigned i = 1; i < SMP_MAX_CPUS; i++) {
        /* QEMU virt: CPU i's MPIDR affinity is i (clusters of 16 with a GICv3) */
        long r = psci(PSCI_CPU_ON64, i, (unsigned long)(uintptr_t)secondary_start, i);
        if (r != 0) {
            if (r != PSCI_INVALID_PARAM) kprintf("[SMP]    CPU %u: PSCI CPU_ON failed (%ld)\n", i, r);
            break;
        }
        unsigned long f, t0, t;
        __asm__ volatile("mrs %0, cntfrq_el0\n\tmrs %1, cntvct_el0" : "=r"(f), "=r"(t0));
        do __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(t));
        while (!__atomic_load_n(&online[i], __ATOMIC_ACQUIRE) && t - t0 < f);   /* (a second) */
        if (!online[i]) { kprintf("[SMP]    CPU %u didn't come up\n", i); break; }
        smp_ncpus = (int)i + 1;
    }
}

/* ── The big kernel lock: a ticket lock (FIFO - the boot CPU's idle loop
 * isn't starved). Waiters sleep in WFE; the unlock's SEV wakes them. ── */
static volatile unsigned bkl_next, bkl_serving;
static volatile int bkl_owner = -1;

void bkl_lock(void)
{
    unsigned t = __atomic_fetch_add(&bkl_next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&bkl_serving, __ATOMIC_ACQUIRE) != t)
        __asm__ volatile("wfe");
    bkl_owner = (int)smp_cpu();
}

void bkl_unlock(void)
{
    if (bkl_owner != (int)smp_cpu()) {
        kprintf("[SMP]    CPU %u: big kernel lock released without holding it (owner %d)\n", smp_cpu(), bkl_owner);
        for (;;) { }
    }
    bkl_owner = -1;
    __atomic_store_n(&bkl_serving, bkl_serving + 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\n\tsev" ::: "memory");
}

int bkl_mine(void) { return bkl_owner == (int)smp_cpu(); }
