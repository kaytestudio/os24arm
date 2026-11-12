/* kernel/include/linux_abi.h - Linux-compatible user mode (kernel/src/linux) */
#ifndef LINUX_ABI_H
#define LINUX_ABI_H

#include "types.h"

typedef __UINT64_TYPE__ lx_u64;

/* Trap frame built by el0_sync_entry (layout: kernel/src/linux/el0.S). */
typedef struct {
    lx_u64  x[31];
    lx_u64  sp;          /* sp_el0 */
    lx_u64  elr;
    lx_u64  spsr;
    uint8_t q[32][16];
    lx_u64  fpsr, fpcr;
} linux_tf_t;

/*
 * Run a static AArch64 Linux ELF executable (ET_EXEC, linked inside the
 * user window - see mmu.h, userland/README.md) at EL0 with argv[0..argc).
 * Blocks until it exits. Returns its exit status (0-255), 128 + signal
 * number if it was killed (e.g. 139 = SIGSEGV), or -1 if it couldn't be
 * loaded (the reason is printed).
 */
int linux_run(const char *path, int argc, char **argv);

/* Called from el0_sync_entry / el0_irq_entry. */
void linux_el0_trap(linux_tf_t *tf);
void linux_el0_irq(linux_tf_t *tf);

#endif /* LINUX_ABI_H */
