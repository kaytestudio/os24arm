/**
 * @file hyp.h
 * @brief Minimal AArch64 EL2 hypervisor - KVM-shaped vCPU run loop
 *
 * Not a port of illumos-kvm/Linux KVM (those are x86 VT-x and arm64 KVM
 * respectively, built on kernels this one doesn't have) - this borrows
 * KVM's *shape*: a vCPU you run until it exits back to the host with a
 * reason (MMIO, shutdown, error), and the host's device model handles the
 * exit before running it again (see vcpu_run()).
 *
 * The split between the two exception levels follows arm64 KVM's nVHE
 * mode: everything at EL2 (kernel/src/hyp_entry.S) is a small assembly
 * world switch - save one context, load the other, eret - and every
 * decision (decoding a trap, emulating a device) is made back at EL1 in
 * ordinary kernel C (kernel/src/hyp.c).
 *
 * Only usable when the kernel was entered at EL2 (QEMU: `-machine
 * virt,virtualization=on`, i.e. `make run-vm`); boot.S then installs
 * hyp_vectors at EL2 before dropping to EL1 as usual. Entered at EL1
 * (`make run`, HVF), hyp_init() reports the hypervisor as unavailable
 * and nothing else changes.
 *
 * This header is also included by hyp_entry.S, so the context layout is
 * spelled out as byte offsets that both sides share; hyp.c
 * _Static_asserts the C structs against them.
 */

#ifndef HYP_H
#define HYP_H

/* ── hvc #0 calls from the host kernel (x0 = call, x1.. = args) ──────── */
#define HYP_CALL_PROBE     0   /* -> x0 = HYP_MAGIC */
#define HYP_CALL_INIT_MMU  1   /* x1 = TTBR0_EL2, x2 = TCR_EL2, x3 = MAIR_EL2 */
#define HYP_CALL_RUN_VCPU  2   /* x1 = struct hyp_vcpu * -> x0 = HYP_EXIT_* */
#define HYP_MAGIC          0x48595021  /* "HYP!" */

/* ── Why the guest stopped running (which EL2 vector it came in on) ──── */
#define HYP_EXIT_TRAP      0   /* synchronous: ESR_EL2 says what */
#define HYP_EXIT_IRQ       1   /* physical IRQ/FIQ while the guest ran */
#define HYP_EXIT_SERROR    2

/* ── struct hyp_ctx: one full EL1 CPU context (guest or host) ────────── */
#define CTX_X        0     /* x0..x30 */
#define CTX_PC       248   /* ELR_EL2 */
#define CTX_PSTATE   256   /* SPSR_EL2 */
#define CTX_SYS      264   /* EL1 system registers, CTX_SYS_* order below */
#define CTX_NR_SYS   26
#define CTX_SIZE     (CTX_SYS + CTX_NR_SYS * 8)   /* 472 */

#define CTX_SYS_SCTLR       0
#define CTX_SYS_CPACR       1
#define CTX_SYS_TTBR0       2
#define CTX_SYS_TTBR1       3
#define CTX_SYS_TCR         4
#define CTX_SYS_MAIR        5
#define CTX_SYS_AMAIR       6
#define CTX_SYS_VBAR        7
#define CTX_SYS_CONTEXTIDR  8
#define CTX_SYS_TPIDR_EL1   9
#define CTX_SYS_TPIDR_EL0   10
#define CTX_SYS_TPIDRRO_EL0 11
#define CTX_SYS_SP_EL0      12
#define CTX_SYS_SP_EL1      13
#define CTX_SYS_ELR         14
#define CTX_SYS_SPSR        15
#define CTX_SYS_ESR         16
#define CTX_SYS_FAR         17
#define CTX_SYS_AFSR0       18
#define CTX_SYS_AFSR1       19
#define CTX_SYS_PAR         20
#define CTX_SYS_CNTKCTL     21
#define CTX_SYS_CSSELR      22
#define CTX_SYS_MDSCR       23
#define CTX_SYS_CNTV_CTL    24   /* EL1 virtual timer: the guest's own */
#define CTX_SYS_CNTV_CVAL   25

/* ── struct hyp_vcpu: what EL2 needs to run one vCPU ─────────────────── */
#define VCPU_GUEST       0
#define VCPU_HOST        CTX_SIZE
#define VCPU_HCR         (2 * CTX_SIZE)        /* HCR_EL2 while guest runs */
#define VCPU_VTTBR       (VCPU_HCR + 8)
#define VCPU_VTCR        (VCPU_HCR + 16)
#define VCPU_FLAGS       (VCPU_HCR + 24)       /* HYP_VCPU_F_* */
#define VCPU_EXIT_REASON (VCPU_HCR + 32)       /* HYP_EXIT_* */
#define VCPU_ESR         (VCPU_HCR + 40)
#define VCPU_FAR         (VCPU_HCR + 48)
#define VCPU_HPFAR       (VCPU_HCR + 56)
#define VCPU_CNTVOFF     (VCPU_HCR + 64)       /* guest virtual counter = physical - this */
#define VCPU_SLICE       (VCPU_HCR + 72)       /* EL2 timer ticks per entry; 0 = no preemption */
#define VCPU_SIZE        (VCPU_HCR + 80)

#define HYP_VCPU_F_FLUSH_TLB  (1 << 0)   /* tlbi vmalls12e1 before entry */

/* ── EL2 control register values ─────────────────────────────────────── */
#define HCR_EL2_HOST   (1 << 31)             /* RW: EL1 is AArch64, nothing trapped */
#define CPTR_EL2_HOST  0x33ff                /* RES1 bits only: FP/SIMD not trapped */
#define CPTR_EL2_GUEST (CPTR_EL2_HOST | (1 << 10))  /* TFP: guest FP traps (not context-switched yet) */
#define CNTHCTL_EL2_HOST  3   /* EL1PCTEN | EL1PCEN: physical counter + timer */
#define CNTHCTL_EL2_GUEST 1   /* EL1PCTEN: counter only - the physical timer traps */

#ifndef __ASSEMBLER__

#include "types.h"

#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#include "vgic.h"

struct hyp_ctx {
    uint64_t x[31];
    uint64_t pc;
    uint64_t pstate;
    uint64_t sys[CTX_NR_SYS];
};

struct hyp_vcpu {
    struct hyp_ctx guest;
    struct hyp_ctx host;
    uint64_t hcr;
    uint64_t vttbr;
    uint64_t vtcr;
    uint64_t flags;
    uint64_t exit_reason;
    uint64_t esr;
    uint64_t far;
    uint64_t hpfar;
    uint64_t cntvoff;
    uint64_t slice;
};

/* ── KVM-style run API ───────────────────────────────────────────────── */

enum vm_exit_reason {
    VM_EXIT_MMIO,            /* guest touched unmapped IPA space: see run.mmio */
    VM_EXIT_SHUTDOWN,        /* PSCI SYSTEM_OFF / SYSTEM_RESET / CPU_OFF */
    VM_EXIT_HALT,            /* WFI with nothing that could ever wake it */
    VM_EXIT_INTERNAL_ERROR,  /* trap we can't handle: see run.esr / guest.pc */
    VM_EXIT_INTR,            /* host time slice ended - poll devices, run again */
    VM_EXIT_IDLE,            /* guest is in WFI waiting for an interrupt */
};

struct vm_run {
    enum vm_exit_reason reason;
    struct {
        uint64_t phys_addr;
        uint64_t data;       /* written value, or (for reads) filled by caller */
        uint32_t len;        /* 1, 2, 4 or 8 */
        int      is_write;
    } mmio;
    uint64_t esr;            /* INTERNAL_ERROR: the ESR_EL2 that caused it */
};

struct vcpu {
    struct hyp_vcpu hv;      /* must stay first: EL2 sees only this part */
    struct vm_run   run;
    int             mmio_pending;
    uint32_t        mmio_rt;
    int             mmio_sign_extend;
    int             mmio_64bit;
    struct vgic     vgic;    /* emulated GICv2 (vgic.c) */
    int             timer_ppi_on;  /* host GIC: physical vtimer PPI enabled */
};

struct vm {
    uint64_t    ram_ipa;     /* guest-physical base of guest RAM */
    uint64_t    ram_pa;      /* host-physical (== host VA: identity mapped) */
    uint64_t    ram_size;
    struct vcpu vcpu;
};

/* Set by boot.S: the exception level the kernel was entered at. */
extern unsigned long hyp_boot_el;

/* Brings up EL2's own MMU and checks the EL2 vectors answer. Returns 0 if
 * the hypervisor is usable, <0 if not (entered at EL1, or EL2 broken). */
int  hyp_init(void);
int  hyp_available(void);

/* One VM, one vCPU, guest RAM as a single stage-2 mapping. */
int  vm_create(struct vm *vm, uint64_t ram_ipa, uint64_t ram_size);
int  vm_load(struct vm *vm, uint64_t ipa, const void *image, size_t len);
void vm_set_entry(struct vm *vm, uint64_t ipa);

/* Runs the vCPU until an exit the caller has to handle (see vm_run), the
 * way KVM_RUN does. For VM_EXIT_MMIO reads, set run.mmio.data before the
 * next call - it's written to the guest's register on re-entry. */
void vcpu_run(struct vcpu *vcpu);

/* Sets an interrupt line into the guest's GIC, like KVM_IRQ_LINE - for
 * device models outside vcpu_run() (e.g. the PL011's SPI). */
void vm_irq_line(struct vm *vm, uint32_t intid, int level);

/* Guest virtual counter frequency (the same as the host's). */
uint64_t vm_counter_freq(void);

/* Shell command: VM [STATUS|TEST] (vm_cmd.c) */
void cmd_vm(int argc, char **argv);
void vm_print_status(void);
uint64_t hyp_pa_range_bits(void);

#endif /* __ASSEMBLER__ */

#endif /* HYP_H */
