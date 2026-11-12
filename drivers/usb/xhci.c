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
 *   3. Enumerate every connected root-hub port (up to MAX_SLOTS): port
 *      reset (USB2), Enable Slot, Address Device, fix up EP0's max packet
 *      size (full speed), GET_DESCRIPTOR(Device).
 *   4. The first HID boot keyboard found is configured (SET_CONFIGURATION,
 *      SET_PROTOCOL(boot), Configure Endpoint for its interrupt-IN pipe)
 *      and its reports are turned into console bytes by xhci_kbd_getc().
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
/* Bits that keep their value when written back (PLS, PP, PIC,
 * WCE/WDE/WOE). Everything else is RO or RW1C/RW1S and is written as 0
 * to avoid side effects (PED is RW1C: writing it back as 1 would disable
 * the port). */
#define PORTSC_PRESERVE    0x0E00C3E0u

#define SPEED_FULL         1
#define SPEED_LOW          2
#define SPEED_HIGH         3
#define SPEED_SUPER        4

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
#define TRB_DIR_IN         (1u << 16)         /* data / status stage */
#define TRB_TRT_NONE       (0u << 16)         /* setup stage */
#define TRB_TRT_IN         (3u << 16)

#define TRB_NORMAL         1
#define TRB_SETUP          2
#define TRB_DATA           3
#define TRB_STATUS         4
#define TRB_LINK           6
#define TRB_ENABLE_SLOT    9
#define TRB_ADDRESS_DEV    11
#define TRB_CONFIGURE_EP   12
#define TRB_EVALUATE_CTX   13
#define TRB_NOOP_CMD       23
#define TRB_EV_TRANSFER    32
#define TRB_EV_CMD_DONE    33

#define EV_SLOT(c)         ((c) >> 24)
#define EV_EP(c)           (((c) >> 16) & 0x1F)

#define CC_SUCCESS         1
#define CC_SHORT_PACKET    13

#define RING_TRBS          32                 /* last one is the link TRB */
#define EVENT_TRBS         64
#define MAX_SLOTS          8
#define MAX_SCRATCHPAD     8
#define CTX_MAX_SIZE       64                 /* context size, CSZ = 1 */

/* ── USB / HID ────────────────────────────────────────────────────────── */
#define USB_REQ_GET_DESCRIPTOR  6
#define USB_REQ_SET_CONFIG      9
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIG         2
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5
#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B

#define KBD_REPORTS        4                  /* interrupt TRBs in flight */

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

typedef struct {
    uint32_t slot;       /* 0 = unused */
    uint32_t port;
    uint32_t speed;
    ring_t   ep0;
} usb_dev_t;

/* ── Controller-visible memory (all static, see file comment) ─────────── */
static u64          dcbaa[MAX_SLOTS + 1]          __attribute__((aligned(64)));
static u64          scratch_array[MAX_SCRATCHPAD] __attribute__((aligned(64)));
static uint8_t      scratch_pages[MAX_SCRATCHPAD][4096] __attribute__((aligned(4096)));
static trb_t        cmd_trbs[RING_TRBS]           __attribute__((aligned(64)));
static trb_t        event_trbs[EVENT_TRBS]        __attribute__((aligned(64)));
static erst_entry_t erst[1]                       __attribute__((aligned(64)));
static trb_t        ep0_trbs[MAX_SLOTS][RING_TRBS] __attribute__((aligned(64)));
static trb_t        kbd_trbs[RING_TRBS]           __attribute__((aligned(64)));
static uint8_t      in_ctx[33 * CTX_MAX_SIZE]     __attribute__((aligned(64)));
static uint8_t      dev_ctx[MAX_SLOTS][32 * CTX_MAX_SIZE] __attribute__((aligned(64)));
static uint8_t      desc_buf[256]                 __attribute__((aligned(64)));
static uint8_t      kbd_bufs[KBD_REPORTS][8]      __attribute__((aligned(64)));

/* ── Driver state ─────────────────────────────────────────────────────── */
static uintptr_t mmio, op, rt, db;
static uint32_t  ctx_size;
static ring_t    cmd_ring;
static uint32_t  ev_deq, ev_cycle;
static usb_dev_t devs[MAX_SLOTS];

/* Keyboard */
static usb_dev_t *kbd;            /* NULL until a boot keyboard is set up */
static uint32_t   kbd_dci;
static ring_t     kbd_ring;
static uint8_t    kbd_prev[8];
static int        kbd_caps;
static char       kbd_q[64];      /* translated bytes for xhci_kbd_getc() */
static uint32_t   kbd_qh, kbd_qt;

/* ── Helpers ──────────────────────────────────────────────────────────── */
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr64(uintptr_t a, u64 v)
{
    wr32(a, (uint32_t)v);
    wr32(a + 4, (uint32_t)(v >> 32));
}
static inline void dma_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline u64 dma(const volatile void *p) { return (u64)(uintptr_t)p; }

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}

static void copy(void *dst, const void *src, uint32_t n)
{
    volatile uint8_t *d = dst;
    const volatile uint8_t *s = src;
    while (n--) *d++ = *s++;
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
    link->p0      = (uint32_t)dma(trbs);
    link->p1      = (uint32_t)(dma(trbs) >> 32);
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
        dma_barrier();
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

/* ── Event ring ───────────────────────────────────────────────────────── */
static void kbd_report(const uint8_t *r);

/* Take one event off the ring if there is one (1), else 0. */
static int event_poll(trb_t *out)
{
    volatile trb_t *e = &event_trbs[ev_deq];
    if ((e->control & TRB_CYCLE) != ev_cycle) return 0;
    out->p0 = e->p0; out->p1 = e->p1;
    out->status = e->status; out->control = e->control;
    if (++ev_deq == EVENT_TRBS) { ev_deq = 0; ev_cycle ^= 1; }
    wr64(rt + IR0_ERDP, dma(&event_trbs[ev_deq]) | ERDP_EHB);
    return 1;
}

/* Keyboard transfer event: decode the report and requeue its buffer. */
static int kbd_event(const trb_t *ev)
{
    if (!kbd || TRB_GET_TYPE(ev->control) != TRB_EV_TRANSFER ||
        EV_SLOT(ev->control) != kbd->slot || EV_EP(ev->control) != kbd_dci)
        return 0;

    /* The event points at the completed Normal TRB, whose parameter is
     * the report buffer it filled. */
    const trb_t *done = (const trb_t *)(uintptr_t)
        (((u64)ev->p1 << 32) | ev->p0);
    uint8_t *buf = (uint8_t *)(uintptr_t)(((u64)done->p1 << 32) | done->p0);
    uint32_t cc = ev->status >> 24;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET)
        kbd_report(buf);

    ring_push(&kbd_ring, dma(buf), 8, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(kbd->slot, kbd_dci);
    return 1;
}

/*
 * Wait for an event of `type` (for transfer events, from `slot`). Keyboard
 * reports arriving meanwhile are still handled; anything else (e.g. port
 * status changes) is dropped.
 */
static int wait_event(uint32_t type, uint32_t slot, trb_t *out, uint32_t ms)
{
    u64 end = ticks() + ms_to_ticks(ms);
    trb_t ev;
    for (;;) {
        if (event_poll(&ev)) {
            if (kbd_event(&ev)) continue;
            if (TRB_GET_TYPE(ev.control) == type &&
                (type != TRB_EV_TRANSFER || EV_SLOT(ev.control) == slot)) {
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
    if (wait_event(TRB_EV_CMD_DONE, 0, &ev, 500) != 0) return -1;
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
            scratch_array[i] = dma(scratch_pages[i]);
        }
        dcbaa[0] = dma(scratch_array);
    }
    dma_barrier();
    wr64(op + OP_DCBAAP, dma(dcbaa));

    /* Command ring. */
    ring_init(&cmd_ring, cmd_trbs);
    wr64(op + OP_CRCR, dma(cmd_trbs) | 1 /* RCS */);

    /* Event ring: one segment. ERSTBA is written last (it arms it). */
    zero(event_trbs, sizeof event_trbs);
    ev_deq = 0;
    ev_cycle = 1;
    erst[0].base = dma(event_trbs);
    erst[0].size = EVENT_TRBS;
    erst[0].rsvd = 0;
    dma_barrier();
    wr32(rt + IR0_ERSTSZ, 1);
    wr64(rt + IR0_ERDP, dma(event_trbs));
    wr64(rt + IR0_ERSTBA, dma(erst));
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

/* ── Control transfers ────────────────────────────────────────────────── */
/* IN data stage if len > 0, else no data stage. 0 or -(completion code). */
static int control(usb_dev_t *d, uint8_t req_type, uint8_t req,
                   uint16_t value, uint16_t index, void *buf, uint16_t len)
{
    u64 setup = (u64)req_type | ((u64)req << 8) | ((u64)value << 16) |
                ((u64)index << 32) | ((u64)len << 48);
    ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT |
              (len ? TRB_TRT_IN : TRB_TRT_NONE));
    if (len)
        ring_push(&d->ep0, dma(buf), len, TRB_TYPE(TRB_DATA) | TRB_DIR_IN);
    /* Status stage runs opposite to the data stage (IN when there is none). */
    ring_push(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
              (len ? 0 : TRB_DIR_IN));
    ring_doorbell(d->slot, 1);                       /* DCI 1 = EP0 */

    trb_t ev;
    /* A data stage completing short raises its own event first; the
     * status stage (IOC) is the one that ends the transfer. */
    for (;;) {
        if (wait_event(TRB_EV_TRANSFER, d->slot, &ev, 500) != 0) return -1;
        uint32_t cc = ev.status >> 24;
        if (cc == CC_SHORT_PACKET) continue;
        return cc == CC_SUCCESS ? 0 : -(int)cc;
    }
}

static int get_descriptor(usb_dev_t *d, uint8_t type, void *buf, uint16_t len)
{
    return control(d, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(type << 8),
                   0, buf, len);
}

/* ── Device enumeration ───────────────────────────────────────────────── */
static const char *speed_name(uint32_t s)
{
    switch (s) {
    case SPEED_FULL:  return "full";
    case SPEED_LOW:   return "low";
    case SPEED_HIGH:  return "high";
    case SPEED_SUPER: return "super";
    case 5:           return "super+";
    default:          return "?";
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

static uint8_t *dev_ctx_of(usb_dev_t *d) { return dev_ctx[d->slot - 1]; }

/* Input context entry i (0 = input control, 1 = slot, 1 + DCI = endpoint). */
static uint32_t *ictx(uint32_t i) { return (uint32_t *)(in_ctx + i * ctx_size); }

/* Full speed EP0 starts at 8 bytes; switch to the device's real size. */
static int fix_ep0_mps(usb_dev_t *d)
{
    zero(desc_buf, 8);
    if (get_descriptor(d, USB_DESC_DEVICE, desc_buf, 8) != 0) return -1;
    uint32_t mps = desc_buf[7];
    if (mps == 8 || (mps != 16 && mps != 32 && mps != 64)) return 0;

    zero(in_ctx, sizeof in_ctx);
    ictx(0)[1] = 1u << 1;                            /* evaluate EP0 */
    copy(ictx(2), dev_ctx_of(d) + ctx_size, ctx_size);
    ictx(2)[1] = (ictx(2)[1] & 0xFFFFu) | (mps << 16);
    dma_barrier();
    int cc = command(dma(in_ctx), TRB_TYPE(TRB_EVALUATE_CTX) | (d->slot << 24), 0);
    return cc == CC_SUCCESS ? 0 : -1;
}

static usb_dev_t *enumerate_port(uint32_t port, xhci_info_t *info)
{
    if (port_enable(port) != 0) {
        kprintf("[XHCI]   port %u: reset failed\n", port);
        return 0;
    }
    uint32_t speed = PORTSC_SPEED(rd32(op + OP_PORTSC(port)));

    trb_t done;
    int cc = command(0, TRB_TYPE(TRB_ENABLE_SLOT), &done);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI]   port %u: Enable Slot failed (cc=%d)\n", port, cc);
        return 0;
    }
    uint32_t slot = EV_SLOT(done.control);
    if (slot == 0 || slot > info->max_slots) return 0;

    usb_dev_t *d = &devs[slot - 1];
    d->slot  = slot;
    d->port  = port;
    d->speed = speed;

    zero(dev_ctx_of(d), sizeof dev_ctx[0]);
    dcbaa[slot] = dma(dev_ctx_of(d));

    zero(in_ctx, sizeof in_ctx);
    uint32_t *sctx = ictx(1), *ep0 = ictx(2);
    ictx(0)[1] = (1u << 0) | (1u << 1);              /* add slot + EP0 */
    sctx[0] = (speed << 20) | (1u << 27);            /* context entries = 1 */
    sctx[1] = port << 16;                            /* root hub port */

    uint32_t mps = speed >= SPEED_SUPER ? 512 : (speed == SPEED_HIGH ? 64 : 8);
    ring_init(&d->ep0, ep0_trbs[slot - 1]);
    ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);    /* CErr 3, control EP */
    ep0[2] = (uint32_t)dma(d->ep0.trbs) | 1;         /* TR dequeue | DCS */
    ep0[3] = (uint32_t)(dma(d->ep0.trbs) >> 32);
    ep0[4] = 8;                                      /* average TRB length */
    dma_barrier();

    cc = command(dma(in_ctx), TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24), 0);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI]   port %u: Address Device failed (cc=%d)\n", port, cc);
        d->slot = 0;
        return 0;
    }

    if (speed == SPEED_FULL && fix_ep0_mps(d) != 0)
        kprintf("[XHCI]   port %u: EP0 packet size fix-up failed\n", port);

    zero(desc_buf, 18);
    if (get_descriptor(d, USB_DESC_DEVICE, desc_buf, 18) != 0) {
        kprintf("[XHCI]   port %u: GET_DESCRIPTOR failed\n", port);
        d->slot = 0;
        return 0;
    }
    uint16_t vid = (uint16_t)(desc_buf[8]  | (desc_buf[9]  << 8));
    uint16_t pid = (uint16_t)(desc_buf[10] | (desc_buf[11] << 8));
    if (!info->dev_vendor) {
        info->dev_vendor  = vid;
        info->dev_product = pid;
        info->dev_class   = desc_buf[4];
    }
    info->enumerated++;
    kprintf("[XHCI]   port %u: %s-speed device %04x:%04x, class %02x, "
            "USB %x.%02x, slot %u\n",
            port, speed_name(speed), vid, pid, desc_buf[4],
            desc_buf[3], desc_buf[2], slot);
    return d;
}

/* ── HID boot keyboard ────────────────────────────────────────────────── */
/* xHCI endpoint interval (2^n x 125us) from an endpoint's bInterval. */
static uint32_t ep_interval(uint32_t speed, uint32_t binterval)
{
    uint32_t n;
    if (speed == SPEED_HIGH || speed >= SPEED_SUPER) {
        n = binterval ? binterval - 1 : 0;           /* already 2^(b-1) */
    } else {
        uint32_t frames = (binterval ? binterval : 1) * 8;   /* ms -> 125us */
        for (n = 0; (2u << n) <= frames; n++) { }
        if (n < 3) n = 3;
    }
    return n > 15 ? 15 : n;
}

/* If `d` is a HID boot keyboard, configure it and start reading reports. */
static int kbd_setup(usb_dev_t *d)
{
    zero(desc_buf, sizeof desc_buf);
    if (get_descriptor(d, USB_DESC_CONFIG, desc_buf, 9) != 0) return -1;
    uint32_t total = desc_buf[2] | (desc_buf[3] << 8);
    if (total > sizeof desc_buf) total = sizeof desc_buf;
    if (get_descriptor(d, USB_DESC_CONFIG, desc_buf, (uint16_t)total) != 0)
        return -1;

    uint32_t config = desc_buf[5];
    int in_kbd_if = 0, iface = -1;
    uint32_t ep_addr = 0, ep_mps = 0, ep_ival = 0;
    for (uint32_t i = 0; i + 2 <= total && desc_buf[i] >= 2; i += desc_buf[i]) {
        const uint8_t *p = &desc_buf[i];
        if (i + p[0] > total) break;
        if (p[1] == USB_DESC_INTERFACE && p[0] >= 9) {
            in_kbd_if = p[5] == 3 && p[6] == 1 && p[7] == 1;  /* HID/boot/kbd */
            if (in_kbd_if) iface = p[2];
        } else if (p[1] == USB_DESC_ENDPOINT && p[0] >= 7 && in_kbd_if &&
                   !ep_addr && (p[2] & 0x80) && (p[3] & 3) == 3) {
            ep_addr = p[2];
            ep_mps  = (p[4] | (p[5] << 8)) & 0x7FF;
            ep_ival = p[6];
        }
    }
    if (iface < 0 || !ep_addr) return -1;            /* not a keyboard */

    if (control(d, 0x00, USB_REQ_SET_CONFIG, (uint16_t)config, 0, 0, 0) != 0) {
        kprintf("[XHCI]   keyboard: SET_CONFIGURATION failed\n");
        return -1;
    }
    /* Boot protocol: fixed 8-byte reports. SET_IDLE(0): report on change
     * only. Some devices stall these; neither is fatal. */
    control(d, 0x21, HID_REQ_SET_PROTOCOL, 0, (uint16_t)iface, 0, 0);
    control(d, 0x21, HID_REQ_SET_IDLE, 0, (uint16_t)iface, 0, 0);

    uint32_t dci = (ep_addr & 0xF) * 2 + 1;          /* IN endpoint */
    ring_init(&kbd_ring, kbd_trbs);

    zero(in_ctx, sizeof in_ctx);
    ictx(0)[1] = (1u << 0) | (1u << dci);
    copy(ictx(1), dev_ctx_of(d), ctx_size);          /* current slot ctx */
    uint32_t entries = (ictx(1)[0] >> 27) & 0x1F;
    if (dci > entries) entries = dci;
    ictx(1)[0] = (ictx(1)[0] & 0x07FFFFFFu) | (entries << 27);

    uint32_t *ep = ictx(1 + dci);
    ep[0] = ep_interval(d->speed, ep_ival) << 16;
    ep[1] = (3u << 1) | (7u << 3) | (ep_mps << 16);  /* CErr 3, interrupt IN */
    ep[2] = (uint32_t)dma(kbd_trbs) | 1;
    ep[3] = (uint32_t)(dma(kbd_trbs) >> 32);
    ep[4] = 8 | (ep_mps << 16);                      /* avg TRB len, max ESIT */
    dma_barrier();

    int cc = command(dma(in_ctx), TRB_TYPE(TRB_CONFIGURE_EP) | (d->slot << 24), 0);
    if (cc != CC_SUCCESS) {
        kprintf("[XHCI]   keyboard: Configure Endpoint failed (cc=%d)\n", cc);
        return -1;
    }

    zero(kbd_prev, sizeof kbd_prev);
    kbd_qh = kbd_qt = 0;
    kbd_dci = dci;
    kbd = d;
    for (uint32_t i = 0; i < KBD_REPORTS; i++)
        ring_push(&kbd_ring, dma(kbd_bufs[i]), 8, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_doorbell(d->slot, dci);

    kprintf("[XHCI]   port %u: HID boot keyboard on EP 0x%02x -> console input\n",
            d->port, ep_addr);
    return 0;
}

static void kbd_put(char c)
{
    uint32_t next = (kbd_qt + 1) % sizeof kbd_q;
    if (next == kbd_qh) return;                      /* full: drop */
    kbd_q[kbd_qt] = c;
    kbd_qt = next;
}

static void kbd_puts(const char *s) { while (*s) kbd_put(*s++); }

/* HID usage -> ASCII (US layout), unshifted / shifted, from 0x04. */
static const char hid_plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\x7f\t -=[]\\#;'`,./";
static const char hid_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\x7f\t _+{}|~:\"~<>?";

static void kbd_key(uint8_t usage, uint8_t mods)
{
    int shift = (mods & 0x22) != 0;
    int ctrl  = (mods & 0x11) != 0;

    if (usage >= 0x04 && usage < 0x04 + sizeof hid_plain - 1) {
        char c = (shift ? hid_shift : hid_plain)[usage - 0x04];
        if (usage <= 0x1D && kbd_caps)               /* letters only */
            c = (c >= 'a') ? (char)(c - 32) : (char)(c + 32);
        if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c = (char)(c & 0x1F);
        kbd_put(c);
        return;
    }
    switch (usage) {                                 /* VT100 sequences */
    case 0x39: kbd_caps = !kbd_caps;  break;         /* Caps Lock */
    case 0x4A: kbd_puts("\x1b[H");    break;         /* Home */
    case 0x4C: kbd_puts("\x1b[3~");   break;         /* Delete */
    case 0x4D: kbd_puts("\x1b[F");    break;         /* End */
    case 0x4F: kbd_puts("\x1b[C");    break;         /* Right */
    case 0x50: kbd_puts("\x1b[D");    break;         /* Left */
    case 0x51: kbd_puts("\x1b[B");    break;         /* Down */
    case 0x52: kbd_puts("\x1b[A");    break;         /* Up */
    default: break;
    }
}

/* Boot report: [0] modifiers, [1] reserved, [2..7] pressed key usages. */
static void kbd_report(const uint8_t *r)
{
    if (r[2] == 0x01) return;                        /* rollover error */
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k < 0x04) continue;
        int held = 0;
        for (int j = 2; j < 8; j++)
            if (kbd_prev[j] == k) held = 1;
        if (!held) kbd_key(k, r[0]);
    }
    copy(kbd_prev, r, 8);
}

int xhci_kbd_ready(void)
{
    if (kbd_qh == kbd_qt && kbd) {
        trb_t ev;
        while (event_poll(&ev))
            kbd_event(&ev);
    }
    return kbd_qh != kbd_qt;
}

int xhci_kbd_getc(void)
{
    if (!xhci_kbd_ready()) return -1;
    char c = kbd_q[kbd_qh];
    kbd_qh = (kbd_qh + 1) % sizeof kbd_q;
    return (unsigned char)c;
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

    for (uint32_t port = 1; port <= info->max_ports; port++) {
        if (!(rd32(op + OP_PORTSC(port)) & PORTSC_CCS)) continue;
        info->connected++;
        if (info->enumerated >= info->max_slots) {
            kprintf("[XHCI]   port %u: device present (no free slot)\n", port);
            continue;
        }
        usb_dev_t *d = enumerate_port(port, info);
        if (d && !kbd) kbd_setup(d);
    }
    if (!info->connected)
        kprintf("[XHCI]   no devices connected\n");
    info->keyboard = kbd != 0;
    return 0;
}
