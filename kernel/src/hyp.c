/**
 * @file hyp.c
 * @brief EL1 half of the hypervisor: page tables, vCPU run loop, trap
 *        decoding, and the VM shell command (see hyp.h)
 *
 * Memory, all identity-mapped on the host side as everywhere else in this
 * kernel (VA == PA):
 *
 *   EL2 stage 1 (hyp_l1/hyp_l2) - EL2's own view, so its loads/stores of
 *   struct hyp_vcpu are cacheable like the host's (with EL2's MMU off they
 *   would be Device accesses, bypassing the data cache EL1 wrote through).
 *   Same layout as kernel/src/mmu.c, but stricter: the kernel's first 2MB
 *   (.text/.rodata, which includes hyp_entry.S) is genuinely read-only +
 *   executable, everything else read-write + never-execute.
 *
 *   Stage 2 (s2_l1/s2_l2) - the guest's physical address space: one run of
 *   2MB blocks mapping guest RAM at ram_ipa onto host RAM at VM_RAM_PA.
 *   Nothing else is mapped, so every other guest access (devices) faults
 *   to EL2 and comes back to vcpu_run() as an MMIO exit.
 *
 * VM_RAM_PA is [0x80000000, 0xC0000000) - RAM under the `-m 2G` every run
 * target here uses, and outside everything the host kernel, LX loader and
 * preloaded test disk occupy (all below 0x50000000).
 */

#include "hyp.h"
#include "string.h"
#include "gicv3.h"

unsigned long hyp_boot_el;   /* written by boot.S before kernel_main */

static int hyp_ready;

#define offsetof(t, m) __builtin_offsetof(t, m)
_Static_assert(offsetof(struct hyp_ctx, pc) == CTX_PC, "CTX_PC");
_Static_assert(offsetof(struct hyp_ctx, pstate) == CTX_PSTATE, "CTX_PSTATE");
_Static_assert(offsetof(struct hyp_ctx, sys) == CTX_SYS, "CTX_SYS");
_Static_assert(sizeof(struct hyp_ctx) == CTX_SIZE, "CTX_SIZE");
_Static_assert(offsetof(struct hyp_vcpu, host) == VCPU_HOST, "VCPU_HOST");
_Static_assert(offsetof(struct hyp_vcpu, hcr) == VCPU_HCR, "VCPU_HCR");
_Static_assert(offsetof(struct hyp_vcpu, vttbr) == VCPU_VTTBR, "VCPU_VTTBR");
_Static_assert(offsetof(struct hyp_vcpu, vtcr) == VCPU_VTCR, "VCPU_VTCR");
_Static_assert(offsetof(struct hyp_vcpu, flags) == VCPU_FLAGS, "VCPU_FLAGS");
_Static_assert(offsetof(struct hyp_vcpu, exit_reason) == VCPU_EXIT_REASON, "VCPU_EXIT_REASON");
_Static_assert(offsetof(struct hyp_vcpu, esr) == VCPU_ESR, "VCPU_ESR");
_Static_assert(offsetof(struct hyp_vcpu, far) == VCPU_FAR, "VCPU_FAR");
_Static_assert(offsetof(struct hyp_vcpu, hpfar) == VCPU_HPFAR, "VCPU_HPFAR");
_Static_assert(offsetof(struct hyp_vcpu, cntvoff) == VCPU_CNTVOFF, "VCPU_CNTVOFF");
_Static_assert(offsetof(struct hyp_vcpu, slice) == VCPU_SLICE, "VCPU_SLICE");
_Static_assert(sizeof(struct hyp_vcpu) == VCPU_SIZE, "VCPU_SIZE");
_Static_assert(offsetof(struct vcpu, hv) == 0, "struct vcpu must start with hv");

#define SZ_2MB (1UL << 21)
#define SZ_1GB (1UL << 30)

#define VM_RAM_PA   0x80000000UL
#define VM_RAM_MAX  (64UL * 1024 * 1024)
#define VM_VMID     1UL

/* ── hvc into hyp_entry.S ────────────────────────────────────────────── */

static uint64_t hyp_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("hvc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                       "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return x0;
}

uint64_t hyp_pa_range_bits(void)
{
    static const uint8_t bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t r = mmfr0 & 0xF;
    return r < sizeof(bits) ? bits[r] : 48;
}

static uint64_t pa_range_field(void)
{
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t r = mmfr0 & 0xF;
    return r > 6 ? 5 : r;
}

/* ── EL2 stage-1 translation (identity map) ──────────────────────────── */

#define S1_BLOCK     (1UL << 0)
#define S1_TABLE     (3UL << 0)
#define S1_ATTR(i)   ((uint64_t)(i) << 2)
#define S1_AP_RW     (1UL << 6)    /* AP[1] is RES1 in the EL2 regime */
#define S1_AP_RO     (3UL << 6)
#define S1_SH_INNER  (3UL << 8)
#define S1_AF        (1UL << 10)
#define S1_XN        (1UL << 54)

#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1
#define HYP_MAIR        (0xFFUL << (8 * MAIR_IDX_NORMAL))   /* idx 0 = Device-nGnRnE (0x00) */

#define S1_DEVICE    (S1_BLOCK | S1_ATTR(MAIR_IDX_DEVICE) | S1_AF | S1_AP_RW | S1_XN)
#define S1_NORMAL_RW (S1_BLOCK | S1_ATTR(MAIR_IDX_NORMAL) | S1_AF | S1_SH_INNER | S1_AP_RW | S1_XN)
#define S1_NORMAL_RX (S1_BLOCK | S1_ATTR(MAIR_IDX_NORMAL) | S1_AF | S1_SH_INNER | S1_AP_RO)

__attribute__((aligned(4096))) static uint64_t hyp_l1[512];
__attribute__((aligned(4096))) static uint64_t hyp_l2[512];

static void hyp_build_el2_tables(void)
{
    /* [0x40000000, +1GB) in 2MB blocks: the first holds .text/.rodata
     * (kernel/linker.ld ASSERTs they fit), the rest is data. */
    for (int i = 0; i < 512; i++)
        hyp_l2[i] = (SZ_1GB + (uint64_t)i * SZ_2MB) | S1_NORMAL_RW;
    hyp_l2[0] = SZ_1GB | S1_NORMAL_RX;

    hyp_l1[0] = 0 | S1_DEVICE;                       /* PL011 etc., for hyp_panic */
    hyp_l1[1] = (uint64_t)(uintptr_t)hyp_l2 | S1_TABLE;
    hyp_l1[2] = (2 * SZ_1GB) | S1_NORMAL_RW;
    __asm__ volatile("dsb ish" ::: "memory");
}

/* ── Stage-2 translation (guest IPA -> host PA) ──────────────────────── */

#define S2_BLOCK      (1UL << 0)
#define S2_TABLE      (3UL << 0)
#define S2_NORMAL_WB  (0xFUL << 2)   /* MemAttr: Normal, Inner+Outer Write-Back */
#define S2_AP_RW      (3UL << 6)
#define S2_SH_INNER   (3UL << 8)
#define S2_AF         (1UL << 10)
#define S2_RAM        (S2_BLOCK | S2_NORMAL_WB | S2_AP_RW | S2_SH_INNER | S2_AF)

/* One VM at a time, so one set of tables. 39-bit IPA, starting at
 * level 1 (VTCR_EL2.T0SZ = 25, SL0 = 1): no concatenated tables needed. */
__attribute__((aligned(4096))) static uint64_t s2_l1[512];
__attribute__((aligned(4096))) static uint64_t s2_l2[512];

#define HCR_VM    (1UL << 0)
#define HCR_SWIO  (1UL << 1)
#define HCR_FMO   (1UL << 3)
#define HCR_IMO   (1UL << 4)
#define HCR_AMO   (1UL << 5)
#define HCR_VI    (1UL << 7)    /* virtual IRQ line into the guest */
#define HCR_FB    (1UL << 9)
#define HCR_BSU_IS (1UL << 10)
#define HCR_TWI   (1UL << 13)
#define HCR_TSC   (1UL << 19)
#define HCR_RW    (1UL << 31)
#define HCR_GUEST (HCR_VM | HCR_SWIO | HCR_FMO | HCR_IMO | HCR_AMO | \
                   HCR_FB | HCR_BSU_IS | HCR_TWI | HCR_TSC | HCR_RW)

#define SCTLR_EL1_RES1  0x30D00800UL   /* MMU and caches off */
#define PSTATE_EL1H_DAIF 0x3C5UL

#define CNTV_CTL_ENABLE 1UL
#define CNTV_CTL_IMASK  2UL

/* Host time slice: how long a guest runs before the host gets the CPU
 * back to poll its devices (console input, Ctrl-]). */
#define SLICES_PER_SEC  100

static uint64_t read_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}

uint64_t vm_counter_freq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static void clean_dcache_to_poc(uint64_t start, uint64_t size)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4UL << ((ctr >> 16) & 0xF);
    for (uint64_t a = start & ~(line - 1); a < start + size; a += line)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tic ialluis\n\tdsb ish\n\tisb" ::: "memory");
}

int vm_create(struct vm *vm, uint64_t ram_ipa, uint64_t ram_size)
{
    if (!hyp_ready)
        return -1;
    if ((ram_ipa | ram_size) & (SZ_2MB - 1) || ram_size == 0 || ram_size > VM_RAM_MAX)
        return -2;
    if ((ram_ipa >> 30) != ((ram_ipa + ram_size - 1) >> 30) || (ram_ipa >> 30) >= 512)
        return -2;   /* must sit inside one 1GB level-1 slot */

    memset(vm, 0, sizeof(*vm));
    vm->ram_ipa  = ram_ipa;
    vm->ram_pa   = VM_RAM_PA;
    vm->ram_size = ram_size;

    memset(s2_l1, 0, sizeof(s2_l1));
    memset(s2_l2, 0, sizeof(s2_l2));
    s2_l1[ram_ipa >> 30] = (uint64_t)(uintptr_t)s2_l2 | S2_TABLE;
    uint64_t first = (ram_ipa & (SZ_1GB - 1)) / SZ_2MB;
    for (uint64_t i = 0; i < ram_size / SZ_2MB; i++)
        s2_l2[first + i] = (VM_RAM_PA + i * SZ_2MB) | S2_RAM;

    /* The guest starts with its MMU off, so its accesses are
     * non-cacheable and must find guest RAM's contents in memory, not
     * just in the host's data cache. */
    memset((void *)(uintptr_t)vm->ram_pa, 0, ram_size);
    clean_dcache_to_poc(vm->ram_pa, ram_size);
    __asm__ volatile("dsb ish" ::: "memory");

    struct hyp_vcpu *hv = &vm->vcpu.hv;
    hv->hcr   = HCR_GUEST;
    hv->vttbr = (uint64_t)(uintptr_t)s2_l1 | (VM_VMID << 48);
    hv->vtcr  = 25UL                   /* T0SZ: 39-bit IPA */
              | (1UL << 6)             /* SL0: start at level 1 */
              | (1UL << 8)             /* IRGN0: WB RA WA */
              | (1UL << 10)            /* ORGN0: WB RA WA */
              | (3UL << 12)            /* SH0: inner shareable */
              | (pa_range_field() << 16)  /* PS */
              | (1UL << 31);           /* RES1 */
    hv->flags = HYP_VCPU_F_FLUSH_TLB;
    hv->cntvoff = read_cntpct();              /* guest's virtual counter starts at ~0 */
    hv->slice = gicv3_available() ? vm_counter_freq() / SLICES_PER_SEC : 0;

    vgic_init(&vm->vcpu.vgic);
    gicv3_ppi_enable(GICV3_PPI_VIRT_TIMER, 0);
    vm->vcpu.timer_ppi_on = 0;
    hv->guest.sys[CTX_SYS_SCTLR] = SCTLR_EL1_RES1;
    hv->guest.pstate = PSTATE_EL1H_DAIF;
    hv->guest.pc = ram_ipa;
    return 0;
}

int vm_load(struct vm *vm, uint64_t ipa, const void *image, size_t len)
{
    if (ipa < vm->ram_ipa || ipa + len > vm->ram_ipa + vm->ram_size)
        return -1;
    uint64_t pa = vm->ram_pa + (ipa - vm->ram_ipa);
    memcpy((void *)(uintptr_t)pa, image, len);
    clean_dcache_to_poc(pa, len);
    return 0;
}

void vm_set_entry(struct vm *vm, uint64_t ipa)
{
    vm->vcpu.hv.guest.pc = ipa;
}

/* ── Trap handling ───────────────────────────────────────────────────── */

#define ESR_EC(esr)       ((esr) >> 26)
#define EC_WFX            0x01
#define EC_FP             0x07
#define EC_HVC64          0x16
#define EC_SMC64          0x17
#define EC_IABT_LOW       0x20
#define EC_DABT_LOW       0x24

#define PSCI_VERSION      0x84000000UL
#define PSCI_CPU_OFF      0x84000002UL
#define PSCI_SYSTEM_OFF   0x84000008UL
#define PSCI_SYSTEM_RESET 0x84000009UL
#define SMCCC_NOT_SUPPORTED ((uint64_t)-1)

static void complete_mmio(struct vcpu *v);

static int exit_internal(struct vcpu *v)
{
    v->run.reason = VM_EXIT_INTERNAL_ERROR;
    v->run.esr = v->hv.esr;
    return 1;
}

/* Guest hvc/smc: only the PSCI calls a single-vCPU guest needs to stop. */
static int handle_psci(struct vcpu *v)
{
    struct hyp_ctx *g = &v->hv.guest;
    switch (g->x[0] & 0xFFFFFFFFUL) {
    case PSCI_VERSION:
        g->x[0] = 0x00010000;   /* 1.0 */
        return 0;
    case PSCI_CPU_OFF:
    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
        v->run.reason = VM_EXIT_SHUTDOWN;
        return 1;
    default:
        g->x[0] = SMCCC_NOT_SUPPORTED;
        return 0;
    }
}

/* Stage-2 data abort outside guest RAM: decode it into an MMIO exit.
 * Needs ESR_EL2.ISV (the hardware decoded the instruction for us) - true
 * for plain single-register loads/stores, which is what device drivers
 * use; anything else (ldp, writeback forms) is reported as an error. */
static int handle_dabt(struct vcpu *v)
{
    uint64_t esr = v->hv.esr;
    if (!(esr & (1UL << 24)) || (esr & (1UL << 7)))   /* !ISV, or S1PTW */
        return exit_internal(v);

    uint32_t len = 1u << ((esr >> 22) & 3);
    v->mmio_rt          = (esr >> 16) & 31;
    v->mmio_sign_extend = (esr >> 21) & 1;
    v->mmio_64bit       = (esr >> 15) & 1;

    v->run.reason = VM_EXIT_MMIO;
    v->run.mmio.phys_addr = ((v->hv.hpfar >> 4) << 12) | (v->hv.far & 0xFFF);
    v->run.mmio.len = len;
    v->run.mmio.is_write = (esr >> 6) & 1;
    v->run.mmio.data = 0;
    if (v->run.mmio.is_write) {
        uint64_t val = v->mmio_rt == 31 ? 0 : v->hv.guest.x[v->mmio_rt];
        v->run.mmio.data = len == 8 ? val : val & ((1UL << (len * 8)) - 1);
    }
    v->mmio_pending = 1;

    /* The interrupt controller lives here, not in the caller's device
     * model, the same split as KVM's in-kernel vGIC. */
    if (vgic_mmio(&v->vgic, v->run.mmio.phys_addr, v->run.mmio.is_write,
                  len, &v->run.mmio.data) == 0) {
        complete_mmio(v);
        return 0;
    }
    return 1;
}

/* The guest's virtual timer as an interrupt line: asserted while it's
 * enabled, unmasked and past its compare value - from the guest's saved
 * registers, since it isn't running. */
static int vtimer_level(struct vcpu *v)
{
    struct hyp_ctx *g = &v->hv.guest;
    uint64_t ctl = g->sys[CTX_SYS_CNTV_CTL];
    if ((ctl & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK)) != CNTV_CTL_ENABLE)
        return 0;
    return read_cntpct() - v->hv.cntvoff >= g->sys[CTX_SYS_CNTV_CVAL];
}

/* Before every entry: refresh the timer line, decide whether the guest
 * sees an IRQ (HCR_EL2.VI), and whether the host needs to hear about the
 * guest's timer firing while it runs. */
static void sync_irqs(struct vcpu *v)
{
    int level = vtimer_level(v);
    vgic_set_line(&v->vgic, VGIC_IRQ_VTIMER, level);

    /* Once the line is high here it's already pending in the vGIC, and
     * the physical PPI would only keep exiting the guest until the guest
     * deals with it - so only listen for it while it's low. */
    if (v->timer_ppi_on != !level) {
        gicv3_ppi_enable(GICV3_PPI_VIRT_TIMER, !level);
        v->timer_ppi_on = !level;
    }

    if (vgic_irq_ready(&v->vgic))
        v->hv.hcr |= HCR_VI;
    else
        v->hv.hcr &= ~HCR_VI;
}

/* Returns 1 if the exit has to go back to vcpu_run()'s caller. */
static int handle_trap(struct vcpu *v)
{
    struct hyp_ctx *g = &v->hv.guest;
    switch (ESR_EC(v->hv.esr)) {
    case EC_HVC64:              /* ELR_EL2 already points past the hvc */
        return handle_psci(v);
    case EC_SMC64:              /* ...but past a trapped smc it doesn't */
        g->pc += 4;
        return handle_psci(v);
    case EC_WFX:
        g->pc += 4;
        /* An interrupt is already waiting: WFI completes at once. */
        if (vgic_any_pending(&v->vgic) || vtimer_level(v))
            return 0;
        /* No armed timer and no interrupt the guest could ever take:
         * this WFI never ends. */
        if ((g->sys[CTX_SYS_CNTV_CTL] & (CNTV_CTL_ENABLE | CNTV_CTL_IMASK)) != CNTV_CTL_ENABLE &&
            v->vgic.enabled == 0) {
            v->run.reason = VM_EXIT_HALT;
            return 1;
        }
        /* Otherwise the wakeup comes from a device or the timer: give the
         * caller the CPU until then (KVM would sleep the vCPU thread). */
        v->run.reason = VM_EXIT_IDLE;
        return 1;
    case EC_DABT_LOW:
        return handle_dabt(v);
    default:                    /* EC_FP, EC_IABT_LOW, ... */
        return exit_internal(v);
    }
}

/* Writes a completed MMIO read into the guest's register and steps past
 * the faulting instruction - KVM does the same at the top of KVM_RUN. */
static void complete_mmio(struct vcpu *v)
{
    struct hyp_ctx *g = &v->hv.guest;
    if (!v->run.mmio.is_write && v->mmio_rt != 31) {
        uint32_t bits = v->run.mmio.len * 8;
        uint64_t val = v->run.mmio.data;
        if (bits < 64) {
            val &= (1UL << bits) - 1;
            if (v->mmio_sign_extend && (val >> (bits - 1)) & 1)
                val |= ~0UL << bits;
        }
        if (!v->mmio_64bit)
            val &= 0xFFFFFFFFUL;
        g->x[v->mmio_rt] = val;
    }
    g->pc += 4;
    v->mmio_pending = 0;
}

void vcpu_run(struct vcpu *v)
{
    if (v->mmio_pending)
        complete_mmio(v);

    for (;;) {
        sync_irqs(v);
        uint64_t why = hyp_call(HYP_CALL_RUN_VCPU, (uint64_t)(uintptr_t)&v->hv, 0, 0);
        if (why == HYP_EXIT_IRQ) {
            /* Slice over, or the guest's timer fired (sync_irqs picks
             * that up on the way back in): either way the caller gets a
             * turn, the way a signal ends KVM_RUN. */
            v->run.reason = VM_EXIT_INTR;
            return;
        }
        if (why != HYP_EXIT_TRAP) {
            exit_internal(v);
            return;
        }
        if (handle_trap(v))
            return;
    }
}

/* ── Bring-up ────────────────────────────────────────────────────────── */

int hyp_init(void)
{
    if (hyp_boot_el != 2)
        return -1;
    if (hyp_call(HYP_CALL_PROBE, 0, 0, 0) != HYP_MAGIC)
        return -2;
    if (hyp_pa_range_bits() < 40)
        return -3;   /* the 39-bit IPA space below needs >= 40-bit PAs */

    hyp_build_el2_tables();
    uint64_t tcr = 25UL                       /* T0SZ: 39-bit VA */
                 | (1UL << 8) | (1UL << 10)   /* IRGN0/ORGN0: WB RA WA */
                 | (3UL << 12)                /* SH0: inner shareable */
                 | (pa_range_field() << 16)   /* PS */
                 | (1UL << 23) | (1UL << 31); /* RES1 */
    hyp_call(HYP_CALL_INIT_MMU, (uint64_t)(uintptr_t)hyp_l1, tcr, HYP_MAIR);

    /* Still answering with its MMU on? */
    if (hyp_call(HYP_CALL_PROBE, 0, 0, 0) != HYP_MAGIC)
        return -4;

    /* Without it guests still run, they just can't be preempted and only
     * see their timer fire when they next trap. */
    gicv3_init();
    hyp_ready = 1;
    return 0;
}

int hyp_available(void)
{
    return hyp_ready;
}

void vm_irq_line(struct vm *vm, uint32_t intid, int level)
{
    vgic_set_line(&vm->vcpu.vgic, intid, level);
}
