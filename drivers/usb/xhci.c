/**
 * @file xhci.c
 * @brief xHCI host controller driver for the AArch64 QEMU "virt" machine
 *
 * QEMU's virt machine only offers xHCI as a PCI device (-device qemu-xhci),
 * and with direct -kernel boot nothing enumerates PCI or assigns BARs, so
 * this does the minimum by hand (same approach as kernel/src/qemu_pci.c on
 * RISC-V):
 *
 *   1. PCI: scan bus 0 through virt's high ECAM window (0x40_1000_0000,
 *      used by default with highmem=on; kernel/src/mmu.c maps it as
 *      Device memory), size BAR0 and place it in the 32-bit PCI MMIO
 *      window at 0x10000000, enable memory decoding + bus mastering.
 *   2. xHCI: halt, reset, program DCBAA / command ring / one event ring
 *      segment, start, and check the command ring with a No Op command.
 *   3. Enumerate the first connected root-hub port: port reset (USB2),
 *      Enable Slot, Address Device, then GET_DESCRIPTOR(Device) on EP0.
 *
 * Everything is polled (no MSI/INTx). All controller-visible structures
 * are static, suitably aligned, in normal cacheable RAM; the kernel is
 * identity-mapped (VA == PA) so their addresses are DMA addresses as-is.
 * TCG doesn't model caches so this is coherent under QEMU; real hardware
 * would need cache maintenance or a non-cacheable mapping for them.
 */

#include "xhci.h"
#include "kprintf.h"

typedef __UINT64_TYPE__ u64;   /* types.h leaves uint64_t undefined */

/* ── QEMU virt memory map ─────────────────────────────────────────────── */
#define VIRT_PCIE_ECAM_HIGH   0x4010000000UL  /* bus 0, 1MB per bus */
#define VIRT_PCIE_MMIO_BASE   0x10000000UL    /* 32-bit BAR window */
#define VIRT_PCIE_MMIO_SIZE   0x2EFF0000UL

/* ── PCI config space ─────────────────────────────────────────────────── */
#define PCI_VENDOR_DEVICE  0x00
#define PCI_COMMAND        0x04
#define PCI_CLASS_REV      0x08
#define PCI_BAR0           0x10
#define PCI_BAR1           0x14

#define PCI_CMD_MEMORY     (1u << 1)
#define PCI_CMD_MASTER     (1u << 2)
#define PCI_BAR_TYPE_64    (2u << 1)

#define PCI_CLASS_XHCI     0x0C0330u          /* serial bus / USB / xHCI */

/* ── xHCI registers ───────────────────────────────────────────────────── */
#define CAP_CAPLENGTH      0x00
#define CAP_HCSPARAMS1     0x04
#define CAP_HCSPARAMS2     0x08
#define CAP_HCCPARAMS1     0x10
#define CAP_DBOFF          0x14
#define CAP_RTSOFF         0x18

#define OP_USBCMD          0x00
#define OP_USBSTS          0x04
#define OP_PAGESIZE        0x08
#define OP_CRCR            0x18
#define OP_DCBAAP          0x30
#define OP_CONFIG          0x38
#define OP_PORTSC(n)       (0x400 + 0x10 * ((n) - 1))   /* n is 1-based */

#define USBCMD_RS          (1u << 0)
#define USBCMD_HCRST       (1u << 1)
#define USBSTS_HCH         (1u << 0)
#define USBSTS_HSE         (1u << 2)
#define USBSTS_CNR         (1u << 11)

#define PORTSC_CCS         (1u << 0)
#define PORTSC_PED         (1u << 1)
#define PORTSC_PR          (1u << 4)
#define PORTSC_PRC         (1u << 21)
#define PORTSC_SPEED(v)    (((v) >> 10) & 0xF)
/* Bits that keep their value when written back (RO + RWS). Everything
 * else is RW1C/RW1S and must be written as 0 to avoid side effects
 * (PED is RW1C: writing it back as 1 would disable the port). */
#define PORTSC_PRESERVE    0x0E00C3E0u   /* PLS, PP, PIC, WCE/WDE/WOE */

/* Interrupter 0 registers, relative to the runtime base */
#define IR0_IMAN           0x20
#define IR0_ERSTSZ         0x28
#define IR0_ERSTBA         0x30
#define IR0_ERDP           0x38
#define ERDP_EHB           (1u << 3)

/* ── TRBs ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t p0, p1;   /* parameter (64-bit, little endian halves) */
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) trb_t;

#define TRB_CYCLE          (1u << 0)
#define TRB_TC             (1u << 1)          /* link: toggle cycle */
#define TRB_IOC            (1u << 5)
#define TRB_IDT            (1u << 6)
#define TRB_TYPE(t)        ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)    (((c) >> 10) & 0x3F)

#define TRB_SETUP          2
#define TRB_DATA           3
#define TRB_STATUS         4
#define TRB_LINK           6
#define TRB_ENABLE_SLOT    9
#define TRB_ADDRESS_DEV    11
#define TRB_NOOP_CMD       23
#define TRB_EV_TRANSFER    32
#define TRB_EV_CMD_DONE    33
#define TRB_EV_PORT        34

#define CC_SUCCESS         1
#define CC_SHORT_PACKET    13

#define RING_TRBS          32                 /* last one is the link TRB */
#define EVENT_TRBS         64
#define MAX_SLOTS          8
#define MAX_SCRATCHPAD     8
#define CTX_MAX_SIZE       64                 /* context size, CSZ = 1 */

typedef struct {
    trb_t   *trbs;
    uint32_t enq;
    uint32_t cycle;
} ring_t;

typedef struct {
    u64      base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed)) erst_entry_t;

/* ── Controller-visible memory (all static, see file comment) ─────────── */
static u64          dcbaa[MAX_SLOTS + 1]          __attribute__((aligned(64)));
static u64          scratch_array[MAX_SCRATCHPAD] __attribute__((aligned(64)));
static uint8_t      scratch_pages[MAX_SCRATCHPAD][4096] __attribute__((aligned(4096)));
static trb_t        cmd_trbs[RING_TRBS]           __attribute__((aligned(64)));
static trb_t        event_trbs[EVENT_TRBS]        __attribute__((aligned(64)));
static erst_entry_t erst[1]                       __attribute__((aligned(64)));
static trb_t        ep0_trbs[RING_TRBS]           __attribute__((aligned(64)));
static uint8_t      in_ctx[33 * CTX_MAX_SIZE]     __attribute__((aligned(64)));
static uint8_t      dev_ctx[32 * CTX_MAX_SIZE]    __attribute__((aligned(64)));
static uint8_t      desc_buf[64]                  __attribute__((aligned(64)));

/* ── Driver state ─────────────────────────────────────────────────────── */
static uintptr_t mmio, op, rt, db;
static uint32_t  ctx_size;
static ring_t    cmd_ring, ep0_ring;
static uint32_t  ev_deq, ev_cycle;

/* ── Helpers ──────────────────────────────────────────────────────────── */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr64(uintptr_t a, u64 v)
{
    wr32(a, (uint32_t)v);
    wr32(a + 4, (uint32_t)(v >> 32));
}
static inline void dma_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}

static u64 ticks(void)
{
    u64 v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

static u64 ms_to_ticks(uint32_t ms)
{
    u64 f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f / 1000 * ms;
}

static void delay_ms(uint32_t ms)
{
    u64 end = ticks() + ms_to_ticks(ms);
    while (ticks() < end) { }
}

/* Wait until (reg & mask) == want; 0 on success, -1 on timeout. */
static int wait_reg(uintptr_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    u64 end = ticks() + ms_to_ticks(ms);
    while ((rd32(reg) & mask) != want)
        if (ticks() >= end) return -1;
    return 0;
}

static volatile uint32_t *pci_cfg(uint32_t dev, uint32_t reg)
{
    return (volatile uint32_t *)(VIRT_PCIE_ECAM_HIGH + ((uintptr_t)dev << 15) + reg);
}

/* ── Rings ────────────────────────────────────────────────────────────── */
static void ring_init(ring_t *r, trb_t *trbs)
{
    zero(trbs, RING_TRBS * sizeof(trb_t));
    r->trbs  = trbs;
    r->enq   = 0;
    r->cycle = 1;
    trb_t *link = &trbs[RING_TRBS - 1];
    link->p0      = (uint32_t)(uintptr_t)trbs;
    link->p1      = (uint32_t)((u64)(uintptr_t)trbs >> 32);
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;
}

static void ring_push(ring_t *r, u64 param, uint32_t status, uint32_t control)
{
    trb_t *t = &r->trbs[r->enq];
    t->p0     = (uint32_t)param;
    t->p1     = (uint32_t)(param >> 32);
    t->status = status;
    dma_barrier();
    /* Cycle bit last: it hands the TRB to the controller. */
    t->control = (control & ~TRB_CYCLE) | r->cycle;

    if (++r->enq == RING_TRBS - 1) {
        trb_t *link = &r->trbs[RING_TRBS - 1];
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        r->enq   = 0;
        r->cycle ^= 1;
    }
}

static void ring_doorbell(uint32_t slot, uint32_t target)
{
    dma_barrier();
    wr32(db + 4 * slot, target);
}

/*
 * Poll the event ring for an event of `type`; other events (e.g. port
 * status changes) are consumed and dropped. Copies the event to *out.
 */
static int wait_event(uint32_t type, trb_t *out, uint32_t ms)
{
    u64 end = ticks() + ms_to_ticks(ms);
    for (;;) {
        volatile trb_t *e = &event_trbs[ev_deq];
        if ((e->control & TRB_CYCLE) == ev_cycle) {
            trb_t ev = { e->p0, e->p1, e->status, e->control };
            if (++ev_deq == EVENT_TRBS) { ev_deq = 0; ev_cycle ^= 1; }
            wr64(rt + IR0_ERDP, (u64)(uintptr_t)&event_trbs[ev_deq] | ERDP_EHB);
            if (TRB_GET_TYPE(ev.control) == type) {
                if (out) *out = ev;
                return 0;
            }
            continue;
        }
        if (rd32(op + OP_USBSTS) & USBSTS_HSE) {
            kprintf("[XHCI]   host system error\n");
            return -1;
        }
        if (ticks() >= end) return -1;
    }
}

/* Issue a command and wait for its completion; returns completion code. */
static int command(u64 param, uint32_t control, trb_t *done)
{
    trb_t ev;
    ring_push(&cmd_ring, param, 0, control);
    ring_doorbell(0, 0);
    if (wait_event(TRB_EV_CMD_DONE, &ev, 500) != 0) return -1;
    if (done) *done = ev;
    return (int)(ev.status >> 24);
}

/* ── PCI ──────────────────────────────────────────────────────────────── */
static int pci_find_xhci(xhci_info_t *info)
{
    for (uint32_t dev = 0; dev < 32; dev++) {
        uint32_t id = *pci_cfg(dev, PCI_VENDOR_DEVICE);
        if (id == 0xFFFFFFFFu) continue;
        if ((*pci_cfg(dev, PCI_CLASS_REV) >> 8) != PCI_CLASS_XHCI) continue;

        *pci_cfg(dev, PCI_COMMAND) &= ~(PCI_CMD_MEMORY | PCI_CMD_MASTER);

        /* Size BAR0 (memory BAR; xHCI's is normally 64-bit). */
        uint32_t orig = *pci_cfg(dev, PCI_BAR0);
        int is64 = (orig & (3u << 1)) == PCI_BAR_TYPE_64;
        *pci_cfg(dev, PCI_BAR0) = 0xFFFFFFFFu;
        uint32_t size = ~(*pci_cfg(dev, PCI_BAR0) & ~0xFu) + 1;
        if (size == 0 || size > VIRT_PCIE_MMIO_SIZE) {
            kprintf("[XHCI]   00:%02x.0 bad BAR0 size 0x%x\n", dev, size);
            return -1;
        }

        /* Only device on the bus we place: put it at the window base
         * (BAR must be aligned to its size; the base is 256MB-aligned). */
        *pci_cfg(dev, PCI_BAR0) = (uint32_t)VIRT_PCIE_MMIO_BASE;
        if (is64) *pci_cfg(dev, PCI_BAR1) = 0;
        *pci_cfg(dev, PCI_COMMAND) |= PCI_CMD_MEMORY | PCI_CMD_MASTER;

        mmio = VIRT_PCIE_MMIO_BASE;
        info->vendor_id = (uint16_t)id;
        info->device_id = (uint16_t)(id >> 16);
        kprintf("[XHCI]   00:%02x.0 xHCI %04x:%04x, BAR0 -> 0x%x (%u KB)\n",
                dev, info->vendor_id, info->device_id,
                (unsigned)VIRT_PCIE_MMIO_BASE, size / 1024);
        return 0;
    }
    return -1;
}

/* ── Controller bring-up ──────────────────────────────────────────────── */
static int hc_start(xhci_info_t *info)
{
    uint32_t caplen  = rd32(mmio + CAP_CAPLENGTH) & 0xFF;
    uint32_t hcs1    = rd32(mmio + CAP_HCSPARAMS1);
    uint32_t hcs2    = rd32(mmio + CAP_HCSPARAMS2);
    uint32_t hcc1    = rd32(mmio + CAP_HCCPARAMS1);
    op = mmio + caplen;
    rt = mmio + (rd32(mmio + CAP_RTSOFF) & ~0x1Fu);
    db = mmio + (rd32(mmio + CAP_DBOFF) & ~0x3u);
    ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;

    uint32_t slots = hcs1 & 0xFF;
    if (slots > MAX_SLOTS) slots = MAX_SLOTS;
    info->max_slots = slots;
    info->max_ports = hcs1 >> 24;

    if (!(rd32(op + OP_PAGESIZE) & 1)) {
        kprintf("[XHCI]   4KB pages not supported\n");
        return -1;
    }

    /* Halt, then reset. */
    wr32(op + OP_USBCMD, rd32(op + OP_USBCMD) & ~USBCMD_RS);
    if (wait_reg(op + OP_USBSTS, USBSTS_HCH, USBSTS_HCH, 100)) return -1;
    wr32(op + OP_USBCMD, USBCMD_HCRST);
    if (wait_reg(op + OP_USBCMD, USBCMD_HCRST, 0, 1000)) return -1;
    if (wait_reg(op + OP_USBSTS, USBSTS_CNR, 0, 1000)) return -1;

    wr32(op + OP_CONFIG, slots);

    /* Device context base address array (+ scratchpad buffers in [0]). */
    zero(dcbaa, sizeof dcbaa);
    uint32_t nscratch = (((hcs2 >> 21) & 0x1F) << 5) | (hcs2 >> 27);
    if (nscratch > MAX_SCRATCHPAD) {
        kprintf("[XHCI]   needs %u scratchpad pages (max %u)\n",
                nscratch, MAX_SCRATCHPAD);
        return -1;
    }
    if (nscratch) {
        for (uint32_t i = 0; i < nscratch; i++) {
            zero(scratch_pages[i], 4096);
            scratch_array[i] = (u64)(uintptr_t)scratch_pages[i];
        }
        dcbaa[0] = (u64)(uintptr_t)scratch_array;
    }
    dma_barrier();
    wr64(op + OP_DCBAAP, (u64)(uintptr_t)dcbaa);

    /* Command ring. */
    ring_init(&cmd_ring, cmd_trbs);
    wr64(op + OP_CRCR, (u64)(uintptr_t)cmd_trbs | 1 /* RCS */);

    /* Event ring: one segment. ERSTBA is written last (it arms it). */
    zero(event_trbs, sizeof event_trbs);
    ev_deq = 0;
    ev_cycle = 1;
    erst[0].base = (u64)(uintptr_t)event_trbs;
    erst[0].size = EVENT_TRBS;
    erst[0].rsvd = 0;
    dma_barrier();
    wr32(rt + IR0_ERSTSZ, 1);
    wr64(rt + IR0_ERDP, (u64)(uintptr_t)event_trbs);
    wr64(rt + IR0_ERSTBA, (u64)(uintptr_t)erst);
    wr32(rt + IR0_IMAN, 0);                      /* polled: interrupts off */

    wr32(op + OP_USBCMD, USBCMD_RS);
    if (wait_reg(op + OP_USBSTS, USBSTS_HCH, 0, 100)) return -1;

    int cc = command(0, TRB_TYPE(TRB_NOOP_CMD), 0);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI]   No Op command failed (cc=%d)\n", cc);
        return -1;
    }
    kprintf("[XHCI]   running: %u slots, %u ports, %u-byte contexts\n",
            slots, info->max_ports, ctx_size);
    return 0;
}

/* ── Device enumeration ───────────────────────────────────────────────── */
static const char *speed_name(uint32_t s)
{
    switch (s) {
    case 1: return "full";
    case 2: return "low";
    case 3: return "high";
    case 4: return "super";
    case 5: return "super+";
    default: return "?";
    }
}

/* Reset a USB2 port (USB3 ports train on their own) and wait for enable. */
static int port_enable(uint32_t port)
{
    uintptr_t reg = op + OP_PORTSC(port);
    uint32_t v = rd32(reg);
    if (v & PORTSC_PED) return 0;

    wr32(reg, (v & PORTSC_PRESERVE) | PORTSC_PR);
    if (wait_reg(reg, PORTSC_PRC, PORTSC_PRC, 500)) return -1;
    v = rd32(reg);
    wr32(reg, (v & PORTSC_PRESERVE) | PORTSC_PRC);   /* ack the change */
    delay_ms(10);                                    /* USB reset recovery */
    return (rd32(reg) & PORTSC_PED) ? 0 : -1;
}

static int control_in(uint32_t slot, uint8_t req_type, uint8_t req,
                      uint16_t value, uint16_t index, void *buf, uint16_t len)
{
    u64 setup = (u64)req_type | ((u64)req << 8) | ((u64)value << 16) |
                ((u64)index << 32) | ((u64)len << 48);
    ring_push(&ep0_ring, setup, 8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (3u << 16) /* TRT = IN */);
    ring_push(&ep0_ring, (u64)(uintptr_t)buf, len,
              TRB_TYPE(TRB_DATA) | (1u << 16) /* DIR = IN */);
    ring_push(&ep0_ring, 0, 0,
              TRB_TYPE(TRB_STATUS) | TRB_IOC /* DIR = OUT */);
    ring_doorbell(slot, 1);                          /* DCI 1 = EP0 */

    trb_t ev;
    if (wait_event(TRB_EV_TRANSFER, &ev, 500) != 0) return -1;
    uint32_t cc = ev.status >> 24;
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -(int)cc;
}

static int enumerate_port(uint32_t port, xhci_info_t *info)
{
    if (port_enable(port) != 0) {
        kprintf("[XHCI]   port %u: reset failed\n", port);
        return -1;
    }
    uint32_t speed = PORTSC_SPEED(rd32(op + OP_PORTSC(port)));

    trb_t done;
    int cc = command(0, TRB_TYPE(TRB_ENABLE_SLOT), &done);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI]   port %u: Enable Slot failed (cc=%d)\n", port, cc);
        return -1;
    }
    uint32_t slot = done.control >> 24;
    if (slot == 0 || slot > info->max_slots) return -1;

    /* Output device context for this slot. */
    zero(dev_ctx, sizeof dev_ctx);
    dcbaa[slot] = (u64)(uintptr_t)dev_ctx;

    /* Input context: control ctx, slot ctx, EP0 ctx (each ctx_size). */
    zero(in_ctx, sizeof in_ctx);
    uint32_t *icc  = (uint32_t *)(in_ctx);
    uint32_t *sctx = (uint32_t *)(in_ctx + ctx_size);
    uint32_t *ep0  = (uint32_t *)(in_ctx + 2 * ctx_size);
    icc[1] = (1u << 0) | (1u << 1);                  /* add slot + EP0 */
    sctx[0] = (speed << 20) | (1u << 27);            /* context entries = 1 */
    sctx[1] = port << 16;                            /* root hub port */

    uint32_t mps = (speed == 4 || speed == 5) ? 512 : (speed == 3 ? 64 : 8);
    ring_init(&ep0_ring, ep0_trbs);
    ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);    /* CErr 3, control EP */
    ep0[2] = (uint32_t)(uintptr_t)ep0_trbs | 1;      /* TR dequeue | DCS */
    ep0[3] = (uint32_t)((u64)(uintptr_t)ep0_trbs >> 32);
    ep0[4] = 8;                                      /* average TRB length */
    dma_barrier();

    cc = command((u64)(uintptr_t)in_ctx,
                 TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24), 0);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI]   port %u: Address Device failed (cc=%d)\n", port, cc);
        return -1;
    }

    /* GET_DESCRIPTOR(Device), 18 bytes. */
    zero(desc_buf, sizeof desc_buf);
    if (control_in(slot, 0x80, 6, 0x0100, 0, desc_buf, 18) != 0) {
        kprintf("[XHCI]   port %u: GET_DESCRIPTOR failed\n", port);
        return -1;
    }
    info->dev_vendor  = (uint16_t)(desc_buf[8]  | (desc_buf[9]  << 8));
    info->dev_product = (uint16_t)(desc_buf[10] | (desc_buf[11] << 8));
    info->dev_class   = desc_buf[4];
    kprintf("[XHCI]   port %u: %s-speed device %04x:%04x, class %02x, "
            "USB %x.%02x, slot %u\n",
            port, speed_name(speed), info->dev_vendor, info->dev_product,
            info->dev_class, desc_buf[3], desc_buf[2], slot);
    return 0;
}

/* ── Public entry ─────────────────────────────────────────────────────── */
int xhci_init(xhci_info_t *info)
{
    xhci_info_t local;
    if (!info) info = &local;
    zero(info, sizeof *info);

    if (pci_find_xhci(info) != 0) return -1;
    if (hc_start(info) != 0) {
        kprintf("[XHCI]   controller failed to start\n");
        return -2;
    }

    int enumerated = 0;
    for (uint32_t port = 1; port <= info->max_ports; port++) {
        uint32_t v = rd32(op + OP_PORTSC(port));
        if (!(v & PORTSC_CCS)) continue;
        info->connected++;
        /* One device for now: a single EP0 ring / device context. */
        if (!enumerated && enumerate_port(port, info) == 0)
            enumerated = 1;
        else if (enumerated)
            kprintf("[XHCI]   port %u: device present (not enumerated)\n", port);
    }
    if (!info->connected)
        kprintf("[XHCI]   no devices connected\n");
    return 0;
}
