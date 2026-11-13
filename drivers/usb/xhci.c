/**
 * @file xhci.c
 * @brief USBXHCD - xHCI host controller driver (HCD layer of the OS/2-style
 *        USB stack, see usbd.h) for the AArch64 QEMU "virt" machine
 *
 * BASEDEVINIT: find the controller on PCI and register with USBD ("USBD$"),
 * retrying at INITIALIZATION COMPLETE if USBD wasn't loaded yet. After
 * that USBD drives everything through this driver's IDC entry
 * (Category 0x90):
 *   RSTHOST  reset + start the controller (rings, event ring, DCBAA)
 *   ACCIO    queue a transfer; completion is reported later, from PRCIRQ,
 *            by calling the request's usbIDC with Function PRCIRQ
 *   CANCEL   drop a device's (or one endpoint's) pending requests
 *   PRCIRQ   polled "interrupt": drain the event ring, then deliver
 *            completions
 *
 * Root hub: as the stack requires, the HCD presents a software root hub,
 * enumerated by USBD like any hub: device/config/hub descriptors, port
 * GET_STATUS / SET_FEATURE / CLEAR_FEATURE mapped onto PORTSC, and a
 * status-change interrupt pipe (EP1 IN). After a controller reset the
 * root hub answers at address 0 until USBD gives it an address.
 *
 * xHCI addresses devices itself, so for a real device:
 *   - GET_DESCRIPTOR(Device) to address 0 on a root port (how USBD starts
 *     every enumeration) creates a slot in the Default state (Address
 *     Device with BSR = 1); later address-0 requests reuse it,
 *   - SET_ADDRESS is not sent on the wire: it becomes Address Device
 *     (BSR = 0), and USBD's chosen address maps to the slot,
 *   - EP0's max packet size follows USBRb.maxPacketSize (Evaluate Context),
 *   - other endpoints are configured on first use from the USBRb (flags,
 *     maxPacketSize, serviceTime) with Configure Endpoint.
 *
 * Bring-up details: virt's PCIe ECAM - the high one (0x40_1000_0000,
 * mapped as Device memory by kernel/src/mmu.c) when the physical address
 * space reaches it, else the low one (0x3F00_0000; e.g. under HVF, whose
 * VMs have a 36-bit IPA space) - BAR0 placed at 0x10000000, all
 * controller structures static and identity-mapped, polled, no cache
 * maintenance (TCG doesn't model caches; real hardware would need it).
 */

#include "xhci.h"
#include "usbd.h"
#include "usb_util.h"
#include "kprintf.h"

/* ── QEMU virt memory map ─────────────────────────────────────────────── */
#define VIRT_PCIE_ECAM_HIGH   0x4010000000UL  /* bus 0, 1MB per bus */
#define VIRT_PCIE_ECAM_LOW    0x3F000000UL    /* when there's no high one (16 buses) */
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
#define PORTSC_OCA         (1u << 3)
#define PORTSC_PR          (1u << 4)
#define PORTSC_PP          (1u << 9)
#define PORTSC_SPEED(v)    (((v) >> 10) & 0xF)
#define PORTSC_CSC         (1u << 17)
#define PORTSC_PEC         (1u << 18)
#define PORTSC_WRC         (1u << 19)
#define PORTSC_OCC         (1u << 20)
#define PORTSC_PRC         (1u << 21)
#define PORTSC_PLC         (1u << 22)
#define PORTSC_CEC         (1u << 23)
#define PORTSC_CHANGES     (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | PORTSC_OCC | PORTSC_PRC)
/* Bits that keep their value when written back (PLS, PP, PIC,
 * WCE/WDE/WOE). Everything else is RO or RW1C/RW1S and is written as 0
 * to avoid side effects (PED is RW1C: writing it as 1 disables the port). */
#define PORTSC_PRESERVE    0x0E00C3E0u

#define XSPEED_FULL        1
#define XSPEED_LOW         2
#define XSPEED_HIGH        3
#define XSPEED_SUPER       4

/* Interrupter 0 registers, relative to the runtime base */
#define IR0_IMAN           0x20
#define IR0_ERSTSZ         0x28
#define IR0_ERSTBA         0x30
#define IR0_ERDP           0x38
#define ERDP_EHB           (1u << 3)

/* ── TRBs ─────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t p0, p1;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) trb_t;

#define TRB_CYCLE          (1u << 0)
#define TRB_TC             (1u << 1)
#define TRB_ISP            (1u << 2)
#define TRB_CH             (1u << 4)          /* chain to the next TRB */
#define TRB_IOC            (1u << 5)
#define TRB_IDT            (1u << 6)
#define TRB_BSR            (1u << 9)          /* Address Device */
#define TRB_TYPE(t)        ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)    (((c) >> 10) & 0x3F)
#define TRB_DIR_IN         (1u << 16)
#define TRB_TRT_NONE       (0u << 16)
#define TRB_TRT_OUT        (2u << 16)
#define TRB_TRT_IN         (3u << 16)
#define TRB_SLOT(s)        ((uint32_t)(s) << 24)
#define TRB_EP(e)          ((uint32_t)(e) << 16)

#define TRB_NORMAL         1
#define TRB_SETUP          2
#define TRB_DATA           3
#define TRB_STATUS         4
#define TRB_LINK           6
#define TRB_ENABLE_SLOT    9
#define TRB_DISABLE_SLOT   10
#define TRB_ADDRESS_DEV    11
#define TRB_CONFIGURE_EP   12
#define TRB_EVALUATE_CTX   13
#define TRB_RESET_EP       14
#define TRB_STOP_EP        15
#define TRB_SET_TR_DEQ     16
#define TRB_NOOP_CMD       23
#define TRB_EV_TRANSFER    32
#define TRB_EV_CMD_DONE    33

#define EV_SLOT(c)         ((c) >> 24)
#define EV_EP(c)           (((c) >> 16) & 0x1F)

#define CC_SUCCESS         1
#define CC_DATA_BUFFER     2
#define CC_BABBLE          3
#define CC_STALL           6
#define CC_TRANSACTION     4
#define CC_SPLIT_TXN       36
#define CC_SHORT_PACKET    13

#define RING_TRBS          32                 /* last one is the link TRB */
#define EVENT_TRBS         256
#define MAX_SLOTS          8
#define MAX_PORTS          15
#define MAX_SCRATCHPAD     8
#define MAX_EP_RINGS       16
#define MAX_XFERS          32
#define CTX_MAX_SIZE       64

#define REQ_IS(s, type, req) ((s)[0] == (type) && (s)[1] == (req))

typedef struct {
    trb_t   *trbs;
    uint32_t enq;
    uint32_t cycle;
    uint32_t pending;         /* TRBs queued and not yet completed/dropped */
} ring_t;

/* Usable TRBs per ring: one is the link TRB, and one stays free so a full
 * ring can never look empty. */
#define RING_ROOM          (RING_TRBS - 2)

typedef struct {
    u64      base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed)) erst_entry_t;

typedef struct {
    uint32_t slot;            /* 0 = unused */
    uint32_t port;            /* root hub port it is (ultimately) behind */
    uint32_t hub_slot;        /* parent hub's slot, 0 = root hub */
    uint32_t hub_port;        /* port on the parent hub */
    uint32_t route;           /* xHCI route string */
    uint32_t depth;           /* hubs between it and the root hub */
    uint32_t tt_slot;         /* LS/FS behind a HS hub: that hub's slot */
    uint32_t tt_port;         /* ... and the port on it */
    uint32_t is_hub;          /* snooped from its device descriptor */
    uint32_t hub_mtt;         /* multi-TT high-speed hub */
    uint32_t hub_nports;      /* snooped from its hub descriptor */
    uint32_t speed;           /* XSPEED_* */
    uint32_t addr;            /* USBD's address, 0 = still default */
    uint32_t ep0_mps;
    ring_t   ep0;
    ring_t  *ep[32];          /* by DCI, endpoints > 0 */
    uint32_t halted;          /* DCI bitmap, needs Reset Endpoint */
} hdev_t;

/* A data buffer may cross at most one 64 KB boundary (lengths are 16-bit),
 * so a data stage / Normal TD is at most 2 TRBs. */
#define MAX_SEGS           2

typedef struct {
    int      used, done;
    USBRb    rb;              /* the caller's request (copied) */
    uint32_t slot, dci;
    ring_t  *ring;            /* NULL for root hub requests */
    uint32_t ntrbs;           /* TRBs it holds on that ring */
    int      control;         /* control TD: completes on the Status TRB */
    trb_t   *first, *last;
    uint32_t nseg;
    trb_t   *seg[MAX_SEGS];   /* data TRBs, with their byte offset/length */
    uint32_t seg_off[MAX_SEGS], seg_len[MAX_SEGS];
    uint32_t len, actual;
    int      short_seen;
    uint8_t  status;
} xfer_t;

/* ── Controller-visible memory ────────────────────────────────────────── */
static u64          dcbaa[MAX_SLOTS + 1]          __attribute__((aligned(64)));
static u64          scratch_array[MAX_SCRATCHPAD] __attribute__((aligned(64)));
static uint8_t      scratch_pages[MAX_SCRATCHPAD][4096] __attribute__((aligned(4096)));
static trb_t        cmd_trbs[RING_TRBS]           __attribute__((aligned(64)));
static trb_t        event_trbs[EVENT_TRBS]        __attribute__((aligned(64)));
static erst_entry_t erst[1]                       __attribute__((aligned(64)));
static trb_t        ep0_trbs[MAX_SLOTS][RING_TRBS] __attribute__((aligned(64)));
static trb_t        ep_trbs[MAX_EP_RINGS][RING_TRBS] __attribute__((aligned(64)));
static uint8_t      in_ctx[33 * CTX_MAX_SIZE]     __attribute__((aligned(64)));
static uint8_t      dev_ctx[MAX_SLOTS][32 * CTX_MAX_SIZE] __attribute__((aligned(64)));

/* ── Driver state ─────────────────────────────────────────────────────── */
static int       verbose, found, registered, running;
static uintptr_t mmio, op, rt, db;
static uint16_t  pci_vendor, pci_device;
static uint32_t  ctx_size, max_slots, max_ports;
static uint16_t  ctrl_id;                     /* assigned by USBD */
static ring_t    cmd_ring;
static uint32_t  ev_deq, ev_cycle;
static volatile int cmd_got;
static trb_t     cmd_ev;
static const trb_t *cmd_wait;                 /* the command TRB being waited on */
static hdev_t    hdevs[MAX_SLOTS];
static uint8_t   addr2slot[128];
static ring_t    ep_rings[MAX_EP_RINGS];
static uint8_t   ep_ring_used[MAX_EP_RINGS];
static xfer_t    xfers[MAX_XFERS];
static uint8_t   cq[MAX_XFERS + 1];           /* completed xfer indices */
static uint32_t  cq_h, cq_t;

/* Root hub */
static uint32_t  rh_addr, rh_addressed, rh_config;
static int       rh_int = -1;                 /* pending status-change xfer */

#define V(...) do { if (verbose) kprintf(__VA_ARGS__); } while (0)

static void    free_slot(hdev_t *d);
static hdev_t *find_default(uint32_t hub_slot, uint32_t port);

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

static int wait_reg(uintptr_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    u64 end = usb_ticks() + usb_ms(ms);
    while ((rd32(reg) & mask) != want)
        if (usb_ticks() >= end) return -1;
    return 0;
}

/* QEMU places the ECAM high only if the PA space covers it (40 bits). */
static uintptr_t ecam_base(void)
{
    u64 mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    return (mmfr0 & 0xF) >= 2 ? VIRT_PCIE_ECAM_HIGH : VIRT_PCIE_ECAM_LOW;
}

static volatile uint32_t *pci_cfg(uint32_t dev, uint32_t reg)
{
    return (volatile uint32_t *)(ecam_base() + ((uintptr_t)dev << 15) + reg);
}

/* ── Rings ────────────────────────────────────────────────────────────── */
static void ring_init(ring_t *r, trb_t *trbs)
{
    usb_zero(trbs, RING_TRBS * sizeof(trb_t));
    r->trbs    = trbs;
    r->enq     = 0;
    r->cycle   = 1;
    r->pending = 0;
    trb_t *link = &trbs[RING_TRBS - 1];
    link->p0      = (uint32_t)dma(trbs);
    link->p1      = (uint32_t)(dma(trbs) >> 32);
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;
}

static trb_t *ring_push(ring_t *r, u64 param, uint32_t status, uint32_t control)
{
    trb_t *t = &r->trbs[r->enq];
    t->p0     = (uint32_t)param;
    t->p1     = (uint32_t)(param >> 32);
    t->status = status;
    dma_barrier();
    t->control = (control & ~TRB_CYCLE) | r->cycle;   /* hands it over */

    if (++r->enq == RING_TRBS - 1) {
        trb_t *link = &r->trbs[RING_TRBS - 1];
        dma_barrier();
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        r->enq   = 0;
        r->cycle ^= 1;
    }
    return t;
}

static u64 ring_deq_ptr(const ring_t *r) { return dma(&r->trbs[r->enq]) | r->cycle; }

/* Split [buf, buf+len) at 64 KB boundaries (an xHCI TRB rule). */
static uint32_t split_64k(u64 buf, uint32_t len, uint32_t *seglen)
{
    uint32_t n = 0;
    while (len && n < MAX_SEGS) {
        uint32_t room = 0x10000u - (uint32_t)(buf & 0xFFFF);
        uint32_t k = len < room ? len : room;
        seglen[n++] = k;
        buf += k;
        len -= k;
    }
    return len ? 0 : n;                               /* 0 = doesn't fit */
}

/* TD Size field: packets left after this TRB (xHCI 4.11.2.4), max 31. */
static uint32_t td_size(uint32_t remaining, uint32_t mps)
{
    if (!mps) mps = 512;
    uint32_t p = (remaining + mps - 1) / mps;
    return (p > 31 ? 31 : p) << 17;
}

/* Queue a data buffer as chained TRBs; first_type is the first TRB's type
 * (Data for a control data stage, Normal otherwise), later ones Normal. */
static void push_data(xfer_t *x, ring_t *r, u64 buf, uint32_t len, uint32_t nseg,
                      const uint32_t *seglen, uint32_t first_type, uint32_t flags,
                      uint32_t last_flags, uint32_t mps)
{
    uint32_t off = 0;
    x->nseg = nseg;
    for (uint32_t k = 0; k < x->nseg; k++) {
        int last = k + 1 == x->nseg;
        uint32_t ctl = TRB_TYPE(k ? TRB_NORMAL : first_type) | flags |
                       (last ? last_flags : TRB_CH);
        x->seg[k]     = ring_push(r, buf + off, seglen[k] |
                                  (last ? 0 : td_size(len - off - seglen[k], mps)), ctl);
        x->seg_off[k] = off;
        x->seg_len[k] = seglen[k];
        off += seglen[k];
    }
}
static void doorbell(uint32_t slot, uint32_t target)
{
    dma_barrier();
    wr32(db + 4 * slot, target);
}

/* ── Requests in flight ───────────────────────────────────────────────── */
static int xfer_alloc(const USBRb *rb)
{
    for (int i = 0; i < MAX_XFERS; i++)
        if (!xfers[i].used) {
            usb_zero(&xfers[i], sizeof xfers[i]);
            xfers[i].used = 1;
            usb_copy(&xfers[i].rb, rb, sizeof *rb);
            return i;
        }
    return -1;
}

/* Free a request slot and give its TRBs back to the ring's budget. */
static void xfer_free(int i)
{
    xfer_t *x = &xfers[i];
    if (x->ring) x->ring->pending = x->ring->pending >= x->ntrbs
                                    ? x->ring->pending - x->ntrbs : 0;
    x->ring = 0;
    x->used = 0;
}

static void xfer_complete(int i, uint8_t status)
{
    xfer_t *x = &xfers[i];
    if (!x->used || x->done) return;
    x->done   = 1;
    x->status = status;
    cq[cq_t] = (uint8_t)i;
    cq_t = (cq_t + 1) % (MAX_XFERS + 1);
}

static uint8_t cc_status(uint32_t cc)
{
    switch (cc) {
    case CC_SUCCESS:
    case CC_SHORT_PACKET: return 0;
    case CC_STALL:        return USRB_STATUS_STALLED;
    case CC_BABBLE:       return USRB_STATUS_BABBLE;
    case CC_DATA_BUFFER:  return USRB_STATUS_DATABUF;
    default:              return USRB_STATUS_CRC;
    }
}

/* Forget requests without completing them (CANCEL). */
static void xfer_drop(uint32_t slot, uint32_t dci_or_all)
{
    for (int i = 0; i < MAX_XFERS; i++)
        if (xfers[i].used && xfers[i].slot == slot &&
            (dci_or_all == 0xff || xfers[i].dci == dci_or_all))
            xfer_free(i);
    /* Compact the completion queue around them. */
    uint8_t keep[MAX_XFERS + 1];
    uint32_t n = 0;
    for (uint32_t h = cq_h; h != cq_t; h = (h + 1) % (MAX_XFERS + 1))
        if (xfers[cq[h]].used) keep[n++] = cq[h];
    for (uint32_t k = 0; k < n; k++) cq[k] = keep[k];
    cq_h = 0;
    cq_t = n;
}

/* ── Events ───────────────────────────────────────────────────────────── */
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

static int trb_in(const trb_t *t, const trb_t *first, const trb_t *last)
{
    if (first <= last) return t >= first && t <= last;
    return t >= first || t <= last;                   /* wrapped */
}

/* Never calls back into other drivers: completions are only queued. */
static void handle_event(const trb_t *ev)
{
    uint32_t type = TRB_GET_TYPE(ev->control);
    if (type == TRB_EV_CMD_DONE) {
        /* Only the command being waited for: a late completion of one
         * that timed out must not be taken for the next command's. */
        const trb_t *t = (const trb_t *)(uintptr_t)(((u64)ev->p1 << 32) | ev->p0);
        if (cmd_wait && t == cmd_wait) {
            cmd_ev  = *ev;
            cmd_got = 1;
        }
        return;
    }
    if (type != TRB_EV_TRANSFER) return;              /* port changes: polled */

    uint32_t slot = EV_SLOT(ev->control), dci = EV_EP(ev->control);
    uint32_t cc   = ev->status >> 24, residual = ev->status & 0xFFFFFF;
    const trb_t *t = (const trb_t *)(uintptr_t)(((u64)ev->p1 << 32) | ev->p0);

    for (int i = 0; i < MAX_XFERS; i++) {
        xfer_t *x = &xfers[i];
        if (!x->used || x->done || x->slot != slot || x->dci != dci) continue;
        if (!trb_in(t, x->first, x->last)) continue;
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
            /* xHCI halts the endpoint on these (not only on STALL): it
             * needs Reset Endpoint + Set TR Dequeue before it runs again. */
            if ((cc == CC_STALL || cc == CC_BABBLE || cc == CC_TRANSACTION ||
                 cc == CC_SPLIT_TXN) && slot && slot <= MAX_SLOTS)
                hdevs[slot - 1].halted |= 1u << dci;
            xfer_complete(i, cc_status(cc));
            return;
        }
        for (uint32_t k = 0; k < x->nseg; k++) {      /* an event on a data TRB */
            if (t != x->seg[k]) continue;
            uint32_t bytes = x->seg_off[k] + x->seg_len[k] - residual;
            if (x->control) {                         /* short data stage: the */
                x->actual = bytes;                    /* Status TRB still runs */
                x->short_seen = 1;
            } else if (cc == CC_SHORT_PACKET || t == x->last) {
                x->actual = bytes;                    /* short ends a Normal TD */
                xfer_complete(i, 0);
            }
            return;
        }
        if (t == x->last) {                           /* control Status TRB */
            if (!x->short_seen) x->actual = x->len;
            xfer_complete(i, 0);
        }
        return;
    }
}

static int command(u64 param, uint32_t control, trb_t *done)
{
    trb_t ev;
    cmd_got  = 0;
    cmd_wait = ring_push(&cmd_ring, param, 0, control);
    doorbell(0, 0);
    u64 end = usb_ticks() + usb_ms(500);
    int r = -1;
    while (!cmd_got) {
        if (event_poll(&ev)) { handle_event(&ev); continue; }
        if (rd32(op + OP_USBSTS) & USBSTS_HSE) break;
        if (usb_ticks() >= end) break;
    }
    if (cmd_got) {
        if (done) *done = cmd_ev;
        r = (int)(cmd_ev.status >> 24);
    }
    cmd_wait = 0;
    return r;
}

/* ── PCI ──────────────────────────────────────────────────────────────── */
static int pci_find_xhci(void)
{
    for (uint32_t dev = 0; dev < 32; dev++) {
        uint32_t id = *pci_cfg(dev, PCI_VENDOR_DEVICE);
        if (id == 0xFFFFFFFFu) continue;
        if ((*pci_cfg(dev, PCI_CLASS_REV) >> 8) != PCI_CLASS_XHCI) continue;

        *pci_cfg(dev, PCI_COMMAND) &= ~(PCI_CMD_MEMORY | PCI_CMD_MASTER);
        uint32_t orig = *pci_cfg(dev, PCI_BAR0);
        int is64 = (orig & (3u << 1)) == PCI_BAR_TYPE_64;
        *pci_cfg(dev, PCI_BAR0) = 0xFFFFFFFFu;
        uint32_t size = ~(*pci_cfg(dev, PCI_BAR0) & ~0xFu) + 1;
        if (size == 0 || size > VIRT_PCIE_MMIO_SIZE) {
            V("[XHCD]   00:%02x.0 bad BAR0 size 0x%x\n", dev, size);
            return -1;
        }
        /* Only device on the bus we place: BAR at the window base
         * (aligned to its size; the base is 256MB-aligned). */
        *pci_cfg(dev, PCI_BAR0) = (uint32_t)VIRT_PCIE_MMIO_BASE;
        if (is64) *pci_cfg(dev, PCI_BAR1) = 0;
        *pci_cfg(dev, PCI_COMMAND) |= PCI_CMD_MEMORY | PCI_CMD_MASTER;

        mmio = VIRT_PCIE_MMIO_BASE;
        pci_vendor = (uint16_t)id;
        pci_device = (uint16_t)(id >> 16);
        V("[XHCD]   00:%02x.0 xHCI %04x:%04x, BAR0 -> 0x%x (%u KB)\n",
          dev, pci_vendor, pci_device, (unsigned)VIRT_PCIE_MMIO_BASE, size / 1024);
        return 0;
    }
    return -1;
}

/* ── RSTHOST: controller reset + start ────────────────────────────────── */
static int hc_start(void)
{
    uint32_t caplen = rd32(mmio + CAP_CAPLENGTH) & 0xFF;
    uint32_t hcs1   = rd32(mmio + CAP_HCSPARAMS1);
    uint32_t hcs2   = rd32(mmio + CAP_HCSPARAMS2);
    uint32_t hcc1   = rd32(mmio + CAP_HCCPARAMS1);
    op = mmio + caplen;
    rt = mmio + (rd32(mmio + CAP_RTSOFF) & ~0x1Fu);
    db = mmio + (rd32(mmio + CAP_DBOFF) & ~0x3u);
    ctx_size  = (hcc1 & (1u << 2)) ? 64 : 32;
    max_slots = hcs1 & 0xFF;
    if (max_slots > MAX_SLOTS) max_slots = MAX_SLOTS;
    max_ports = hcs1 >> 24;
    if (max_ports > MAX_PORTS) max_ports = MAX_PORTS;

    if (!(rd32(op + OP_PAGESIZE) & 1)) return -1;    /* need 4KB pages */

    running = 0;
    wr32(op + OP_USBCMD, rd32(op + OP_USBCMD) & ~USBCMD_RS);
    if (wait_reg(op + OP_USBSTS, USBSTS_HCH, USBSTS_HCH, 100)) return -1;
    wr32(op + OP_USBCMD, USBCMD_HCRST);
    if (wait_reg(op + OP_USBCMD, USBCMD_HCRST, 0, 1000)) return -1;
    if (wait_reg(op + OP_USBSTS, USBSTS_CNR, 0, 1000)) return -1;

    /* Forget all software state from before the reset. */
    usb_zero(hdevs, sizeof hdevs);
    usb_zero(addr2slot, sizeof addr2slot);
    usb_zero(ep_ring_used, sizeof ep_ring_used);
    usb_zero(xfers, sizeof xfers);
    cq_h = cq_t = 0;
    rh_addr = rh_addressed = rh_config = 0;
    rh_int = -1;

    wr32(op + OP_CONFIG, max_slots);

    usb_zero(dcbaa, sizeof dcbaa);
    uint32_t nscratch = (((hcs2 >> 21) & 0x1F) << 5) | (hcs2 >> 27);
    if (nscratch > MAX_SCRATCHPAD) return -1;
    for (uint32_t i = 0; i < nscratch; i++) {
        usb_zero(scratch_pages[i], 4096);
        scratch_array[i] = dma(scratch_pages[i]);
    }
    if (nscratch) dcbaa[0] = dma(scratch_array);
    dma_barrier();
    wr64(op + OP_DCBAAP, dma(dcbaa));

    ring_init(&cmd_ring, cmd_trbs);
    wr64(op + OP_CRCR, dma(cmd_trbs) | 1 /* RCS */);

    usb_zero(event_trbs, sizeof event_trbs);
    ev_deq = 0;
    ev_cycle = 1;
    erst[0].base = dma(event_trbs);
    erst[0].size = EVENT_TRBS;
    erst[0].rsvd = 0;
    dma_barrier();
    wr32(rt + IR0_ERSTSZ, 1);
    wr64(rt + IR0_ERDP, dma(event_trbs));
    wr64(rt + IR0_ERSTBA, dma(erst));                 /* arms the event ring */
    wr32(rt + IR0_IMAN, 0);                           /* polled */

    wr32(op + OP_USBCMD, USBCMD_RS);
    if (wait_reg(op + OP_USBSTS, USBSTS_HCH, 0, 100)) return -1;
    if (command(0, TRB_TYPE(TRB_NOOP_CMD), 0) != CC_SUCCESS) return -1;
    running = 1;
    V("[XHCD]   controller %u running: %u slots, %u ports, %u-byte contexts\n",
      ctrl_id, max_slots, max_ports, ctx_size);
    return 0;
}

/* ── Software root hub ────────────────────────────────────────────────── */
static const uint8_t rh_dev_desc[18] = {
    18, 1, 0x00, 0x02,          /* USB 2.0 */
    9, 0, 1, 64,                /* hub class, single TT, EP0 64 bytes */
    0x00, 0x00, 0x03, 0x00,     /* VID 0000 (none), PID 0003 */
    0x00, 0x01, 0, 0, 0, 1
};
static const uint8_t rh_cfg_desc[25] = {
    9, 2, 25, 0, 1, 1, 0, 0xE0, 0,      /* config 1, self powered */
    9, 4, 0, 0, 1, 9, 0, 0, 0,          /* interface: hub */
    7, 5, 0x81, 3, 2, 0, 12             /* EP1 IN interrupt, 2 bytes */
};

static uint32_t portsc(uint32_t p) { return rd32(op + OP_PORTSC(p)); }

static void portsc_set(uint32_t p, uint32_t bits)
{
    wr32(op + OP_PORTSC(p), (portsc(p) & PORTSC_PRESERVE) | bits);
}

static void rh_port_status(uint32_t p, uint8_t *out)
{
    uint32_t v = portsc(p), s = 0, c = 0;
    if (v & PORTSC_CCS) s |= 1u << 0;
    if (v & PORTSC_PED) s |= 1u << 1;
    if (v & PORTSC_OCA) s |= 1u << 3;
    if (v & PORTSC_PR)  s |= 1u << 4;
    if (v & PORTSC_PP)  s |= 1u << 8;
    uint32_t sp = PORTSC_SPEED(v);
    if (v & PORTSC_CCS) {
        if (sp == XSPEED_LOW)   s |= 1u << 9;
        if (sp == XSPEED_HIGH)  s |= 1u << 10;
        if (sp >= XSPEED_SUPER) s |= (1u << 10) | (1u << 15);  /* 15: local */
    }
    if (v & PORTSC_CSC) c |= 1u << 0;
    if (v & PORTSC_PEC) c |= 1u << 1;
    if (v & PORTSC_OCC) c |= 1u << 3;
    if (v & (PORTSC_PRC | PORTSC_WRC)) c |= 1u << 4;
    out[0] = (uint8_t)s; out[1] = (uint8_t)(s >> 8);
    out[2] = (uint8_t)c; out[3] = (uint8_t)(c >> 8);
}

static int rh_port_feature(uint32_t p, int set, uint32_t f)
{
    if (p < 1 || p > max_ports) return -1;
    if (set) {
        switch (f) {
        case 4:  portsc_set(p, PORTSC_PR); return 0;               /* RESET */
        case 8:  portsc_set(p, PORTSC_PP); return 0;               /* POWER */
        case 2:  return 0;                                         /* SUSPEND */
        default: return -1;
        }
    }
    switch (f) {
    case 1:  portsc_set(p, PORTSC_PED); return 0;                  /* ENABLE */
    case 8:  wr32(op + OP_PORTSC(p), portsc(p) & PORTSC_PRESERVE & ~PORTSC_PP);
             return 0;                                             /* POWER */
    case 16: portsc_set(p, PORTSC_CSC); return 0;                  /* C_CONNECTION */
    case 17: portsc_set(p, PORTSC_PEC); return 0;                  /* C_ENABLE */
    case 19: portsc_set(p, PORTSC_OCC); return 0;                  /* C_OVER_CURRENT */
    case 20: portsc_set(p, PORTSC_PRC | PORTSC_WRC); return 0;     /* C_RESET */
    case 2: case 18: return 0;                                     /* suspend */
    default: return -1;
    }
}

static uint32_t rh_bitmap(void)
{
    uint32_t bm = 0;
    for (uint32_t p = 1; p <= max_ports; p++) {
        uint32_t v = portsc(p);
        if (v & (PORTSC_PLC | PORTSC_CEC))            /* not reported: ack */
            portsc_set(p, v & (PORTSC_PLC | PORTSC_CEC));
        if ((v & PORTSC_CSC) && !(v & PORTSC_CCS)) {  /* unplugged: drop any */
            hdev_t *d = find_default(0, p);           /* half-enumerated slot */
            if (d) free_slot(d);
        }
        if (v & PORTSC_CHANGES) bm |= 1u << p;
    }
    return bm;
}

static uint16_t rh_request(int xi)
{
    xfer_t *x = &xfers[xi];
    USBRb *rb = &x->rb;

    if (rb->endPointId == 1) {                        /* status-change pipe */
        if (rh_int >= 0) return USB_IDC_PARMERR;
        rh_int = xi;
        return 0;
    }
    if (rb->endPointId != 0 || !rb->buffer1 || rb->buffer1Length < 8)
        return USB_IDC_PARMERR;

    const uint8_t *s = rb->buffer1;
    uint16_t value = (uint16_t)(s[2] | (s[3] << 8));
    uint16_t index = (uint16_t)(s[4] | (s[5] << 8));
    uint16_t wlen  = (uint16_t)(s[6] | (s[7] << 8));
    uint8_t  reply[32];
    uint32_t n = 0;
    int ok = 1;

    switch ((s[0] << 8) | s[1]) {
    case 0x8006:                                      /* GET_DESCRIPTOR */
        if ((value >> 8) == 1)      { usb_copy(reply, rh_dev_desc, 18); n = 18; }
        else if ((value >> 8) == 2) { usb_copy(reply, rh_cfg_desc, 25); n = 25; }
        else ok = 0;
        break;
    case 0x0005:                                      /* SET_ADDRESS */
        rh_addr = value & 0x7F; rh_addressed = 1; break;
    case 0x0009: rh_config = value & 0xFF; break;     /* SET_CONFIGURATION */
    case 0x8008: reply[0] = (uint8_t)rh_config; n = 1; break;
    case 0x8000: reply[0] = 1; reply[1] = 0; n = 2; break;  /* self powered */
    case 0xA006: {                                    /* GET hub descriptor */
        uint32_t bytes = (max_ports + 8) / 8;
        reply[0] = (uint8_t)(7 + 2 * bytes);
        reply[1] = 0x29;
        reply[2] = (uint8_t)max_ports;
        reply[3] = 0x01; reply[4] = 0;                /* per-port power */
        reply[5] = 10;                                /* 20 ms power good */
        reply[6] = 0;
        for (uint32_t i = 0; i < bytes; i++) {
            reply[7 + i] = 0;                         /* removable */
            reply[7 + bytes + i] = 0xFF;              /* power control mask */
        }
        n = reply[0];
        break;
    }
    case 0xA000: usb_zero(reply, 4); n = 4; break;    /* hub status */
    case 0xA300:                                      /* port status */
        if (index < 1 || index > max_ports) { ok = 0; break; }
        rh_port_status(index, reply); n = 4;
        break;
    case 0x2001: case 0x2003: break;                  /* hub features */
    case 0x2303: ok = rh_port_feature(index, 1, value) == 0; break;
    case 0x2301: ok = rh_port_feature(index, 0, value) == 0; break;
    default: ok = 0; break;
    }

    if (n > wlen) n = wlen;
    if (n > rb->buffer2Length) n = rb->buffer2Length;
    if (ok && n) usb_copy(rb->buffer2, reply, n);
    x->actual = ok ? n : 0;
    xfer_complete(xi, ok ? 0 : USRB_STATUS_STALLED);
    return 0;
}

/* ── Devices ──────────────────────────────────────────────────────────── */
static uint32_t *ictx(uint32_t i) { return (uint32_t *)(in_ctx + i * ctx_size); }
static uint8_t *dctx(const hdev_t *d) { return dev_ctx[d->slot - 1]; }

static void free_slot(hdev_t *d)
{
    /* Devices still at address 0 behind this hub are USBD-invisible: free
     * them with it (USBD cancels the addressed ones itself, first). */
    for (uint32_t i = 0; i < MAX_SLOTS; i++)
        if (hdevs[i].slot && !hdevs[i].addr && hdevs[i].hub_slot == d->slot &&
            &hdevs[i] != d)
            free_slot(&hdevs[i]);
    for (uint32_t i = 0; i < 32; i++)
        if (d->ep[i]) ep_ring_used[d->ep[i] - ep_rings] = 0;
    xfer_drop(d->slot, 0xff);
    command(0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(d->slot), 0);
    dcbaa[d->slot] = 0;
    if (d->addr) addr2slot[d->addr] = 0;
    usb_zero(d, sizeof *d);
}

static void fill_ep0_ctx(hdev_t *d)
{
    uint32_t *ep0 = ictx(2);
    ep0[1] = (3u << 1) | (4u << 3) | (d->ep0_mps << 16);   /* CErr 3, control */
    u64 deq = ring_deq_ptr(&d->ep0);
    ep0[2] = (uint32_t)deq;
    ep0[3] = (uint32_t)(deq >> 32);
    ep0[4] = 8;
}

static int address_device(hdev_t *d, int bsr)
{
    usb_zero(in_ctx, sizeof in_ctx);
    ictx(0)[1] = (1u << 0) | (1u << 1);
    ictx(1)[0] = d->route | (d->speed << 20) | (1u << 27);
    ictx(1)[1] = d->port << 16;                       /* root hub port */
    ictx(1)[2] = d->tt_slot | (d->tt_port << 8);
    fill_ep0_ctx(d);
    dma_barrier();
    return command(dma(in_ctx), TRB_TYPE(TRB_ADDRESS_DEV) | TRB_SLOT(d->slot) |
                   (bsr ? TRB_BSR : 0), 0) == CC_SUCCESS ? 0 : -1;
}

/* The not-yet-addressed device on port `port` of hub `hub_slot` (0 = the
 * root hub), if there is one. */
static hdev_t *find_default(uint32_t hub_slot, uint32_t port)
{
    for (uint32_t i = 0; i < MAX_SLOTS; i++)
        if (hdevs[i].slot && !hdevs[i].addr &&
            hdevs[i].hub_slot == hub_slot && hdevs[i].hub_port == port)
            return &hdevs[i];
    return 0;
}

/* New slot in the Default state for the device on a hub port. Behind an
 * external hub the speed comes from USBD (USBRb flags, i.e. the hub's
 * port status) and the route string / TT from the hub's own slot. */
static hdev_t *default_device(uint32_t hub_slot, uint32_t port, const USBRb *rb)
{
    uint32_t speed, root_port, route = 0, depth = 0, tt_slot = 0, tt_port = 0;
    if (!hub_slot) {
        if (port < 1 || port > max_ports || !(portsc(port) & PORTSC_PED)) return 0;
        speed = PORTSC_SPEED(portsc(port));
        root_port = port;
    } else {
        const hdev_t *hub = &hdevs[hub_slot - 1];
        if (!hub->slot || !hub->is_hub || hub->depth >= 4 || port < 1 || port > 15)
            return 0;
        speed = (rb->flags & USRB_FLAGS_DET_LOWSPEED)  ? XSPEED_LOW  :
                (rb->flags & USRB_FLAGS_DET_HIGHSPEED) ? XSPEED_HIGH : XSPEED_FULL;
        root_port = hub->port;
        depth     = hub->depth + 1;
        route     = hub->route | (port << (4 * hub->depth));
        if (hub->speed == XSPEED_HIGH && (speed == XSPEED_LOW || speed == XSPEED_FULL)) {
            tt_slot = hub->slot;                       /* this hub's TT */
            tt_port = port;
        } else {
            tt_slot = hub->tt_slot;                    /* inherited */
            tt_port = hub->tt_port;
        }
    }
    hdev_t *old = find_default(hub_slot, port);
    if (old) free_slot(old);                          /* stale: start over */

    trb_t done;
    if (command(0, TRB_TYPE(TRB_ENABLE_SLOT), &done) != CC_SUCCESS) return 0;
    uint32_t slot = EV_SLOT(done.control);
    if (!slot || slot > max_slots) return 0;

    hdev_t *d = &hdevs[slot - 1];
    usb_zero(d, sizeof *d);
    d->slot     = slot;
    d->port     = root_port;
    d->hub_slot = hub_slot;
    d->hub_port = port;
    d->route    = route;
    d->depth    = depth;
    d->tt_slot  = tt_slot;
    d->tt_port  = tt_port;
    d->speed    = speed;
    d->ep0_mps  = rb->maxPacketSize ? rb->maxPacketSize : 8;
    ring_init(&d->ep0, ep0_trbs[slot - 1]);
    usb_zero(dctx(d), sizeof dev_ctx[0]);
    dcbaa[slot] = dma(dctx(d));

    if (address_device(d, 1) != 0) {                  /* BSR: no SET_ADDRESS */
        free_slot(d);
        return 0;
    }
    return d;
}

static int set_ep0_mps(hdev_t *d, uint32_t mps)
{
    usb_zero(in_ctx, sizeof in_ctx);
    ictx(0)[1] = 1u << 1;
    usb_copy(ictx(2), dctx(d) + ctx_size, ctx_size);
    ictx(2)[1] = (ictx(2)[1] & 0xFFFFu) | (mps << 16);
    dma_barrier();
    if (command(dma(in_ctx), TRB_TYPE(TRB_EVALUATE_CTX) | TRB_SLOT(d->slot), 0)
        != CC_SUCCESS)
        return -1;
    d->ep0_mps = mps;
    return 0;
}

/* After a STALL the endpoint is Halted: reset it and move its dequeue
 * pointer past everything queued. Requests that were queued behind the
 * failed one would never run, so they complete now as stalled (their
 * owners resubmit, as for any error) instead of hanging. */
static void ep_recover(hdev_t *d, uint32_t dci)
{
    ring_t *r = dci == 1 ? &d->ep0 : d->ep[dci];
    d->halted &= ~(1u << dci);
    if (!r) return;
    for (int i = 0; i < MAX_XFERS; i++)
        if (xfers[i].used && !xfers[i].done && xfers[i].slot == d->slot &&
            xfers[i].dci == dci)
            xfer_complete(i, USRB_STATUS_STALLED);
    command(0, TRB_TYPE(TRB_RESET_EP) | TRB_SLOT(d->slot) | TRB_EP(dci), 0);
    command(ring_deq_ptr(r), TRB_TYPE(TRB_SET_TR_DEQ) | TRB_SLOT(d->slot) |
            TRB_EP(dci), 0);
}

/* xHCI endpoint interval (2^n x 125us) from bInterval. */
static uint32_t ep_interval(uint32_t speed, uint32_t binterval)
{
    uint32_t n;
    if (speed == XSPEED_HIGH || speed >= XSPEED_SUPER) {
        n = binterval ? binterval - 1 : 0;
    } else {
        uint32_t frames = (binterval ? binterval : 1) * 8;
        for (n = 0; (2u << n) <= frames; n++) { }
        if (n < 3) n = 3;
    }
    return n > 15 ? 15 : n;
}

static ring_t *ep_configure(hdev_t *d, uint32_t dci, const USBRb *rb)
{
    int in   = (rb->flags & USRB_FLAGS_TTYPE_MASK) == USRB_FLAGS_TTYPE_IN;
    int intr = (rb->flags & USRB_FLAGS_DET_INTRPT) != 0;
    if (!intr && !(rb->flags & USRB_FLAGS_DET_BULK)) return 0;   /* no isoch */

    int ri = -1;
    for (int i = 0; i < MAX_EP_RINGS; i++)
        if (!ep_ring_used[i]) { ri = i; break; }
    if (ri < 0) return 0;
    ring_t *r = &ep_rings[ri];
    ring_init(r, ep_trbs[ri]);

    uint32_t mps = rb->maxPacketSize & 0x7FF;
    usb_zero(in_ctx, sizeof in_ctx);
    ictx(0)[1] = (1u << 0) | (1u << dci);
    usb_copy(ictx(1), dctx(d), ctx_size);
    uint32_t entries = (ictx(1)[0] >> 27) & 0x1F;
    if (dci > entries) entries = dci;
    ictx(1)[0] = (ictx(1)[0] & 0x07FFFFFFu) | (entries << 27);
    if (d->is_hub) {                                  /* Hub, MTT, NumPorts */
        ictx(1)[0] |= 1u << 26;
        if (d->hub_mtt && d->speed == XSPEED_HIGH) ictx(1)[0] |= 1u << 25;
        ictx(1)[1] = (ictx(1)[1] & 0x00FFFFFFu) | (d->hub_nports << 24);
    }

    uint32_t type = in ? (intr ? 7 : 6) : (intr ? 3 : 2);
    uint32_t *ep = ictx(1 + dci);
    ep[0] = intr ? ep_interval(d->speed, rb->serviceTime) << 16 : 0;
    ep[1] = (3u << 1) | (type << 3) | (mps << 16);
    ep[2] = (uint32_t)dma(r->trbs) | 1;
    ep[3] = (uint32_t)(dma(r->trbs) >> 32);
    ep[4] = (intr ? 8 : mps) | ((intr ? mps : 0) << 16);
    dma_barrier();

    if (command(dma(in_ctx), TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(d->slot), 0)
        != CC_SUCCESS)
        return 0;
    ep_ring_used[ri] = 1;
    d->ep[dci] = r;
    return r;
}

/* ── ACCIO ────────────────────────────────────────────────────────────── */
static uint16_t accio_control(int xi, hdev_t *d)
{
    xfer_t *x = &xfers[xi];
    USBRb *rb = &x->rb;
    const uint8_t *s = rb->buffer1;
    uint16_t len = rb->buffer2 ? rb->buffer2Length : 0;
    int in = (s[0] & 0x80) != 0;

    if (rb->maxPacketSize && rb->maxPacketSize != d->ep0_mps &&
        set_ep0_mps(d, rb->maxPacketSize) != 0)
        return USB_IDC_IOFAILED;
    if (d->halted & (1u << 1)) ep_recover(d, 1);

    uint32_t seglen[MAX_SEGS];
    uint32_t nseg = len ? split_64k(dma(rb->buffer2), len, seglen) : 0;
    if (len && !nseg) return USB_IDC_PARMERR;
    uint32_t need = 2 + nseg;                         /* setup, data..., status */
    if (d->ep0.pending + need > RING_ROOM) return USB_IDC_NOBANDWIDTH;

    x->slot    = d->slot;
    x->dci     = 1;
    x->len     = len;
    x->control = 1;
    x->ring    = &d->ep0;
    x->ntrbs   = need;
    d->ep0.pending += need;
    u64 setup = 0;
    for (int i = 7; i >= 0; i--) setup = (setup << 8) | s[i];
    x->first = ring_push(&d->ep0, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT |
                         (!len ? TRB_TRT_NONE : in ? TRB_TRT_IN : TRB_TRT_OUT));
    if (len)
        push_data(x, &d->ep0, dma(rb->buffer2), len, nseg, seglen, TRB_DATA,
                  TRB_ISP | (in ? TRB_DIR_IN : 0), 0, d->ep0_mps);
    x->last = ring_push(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
                        (len && in ? 0 : TRB_DIR_IN));
    doorbell(d->slot, 1);
    return 0;
}

/* Control request to address 0: the not-yet-addressed device on hub
 * HubAddr's port PortNumb (USBD fills both in). */
static uint16_t accio_default(int xi)
{
    USBRb *rb = &xfers[xi].rb;
    const uint8_t *s = rb->buffer1;
    uint32_t port = rb->PortNumb, hub_slot = 0;
    if (!rh_addressed) return USB_IDC_ADDRINV;
    if (rb->HubAddr != rh_addr) {
        hub_slot = rb->HubAddr < 128 ? addr2slot[rb->HubAddr] : 0;
        if (!hub_slot) return USB_IDC_ADDRINV;
    }
    hdev_t *d = find_default(hub_slot, port);

    if (REQ_IS(s, 0x00, 5)) {                         /* SET_ADDRESS */
        uint32_t a = s[2] & 0x7F;
        if (!d || !a || addr2slot[a] || address_device(d, 0) != 0)
            return USB_IDC_IOFAILED;
        d->addr = a;
        addr2slot[a] = (uint8_t)d->slot;
        xfer_complete(xi, 0);
        return 0;
    }
    /* GET_DESCRIPTOR(Device) starts every enumeration: fresh slot. */
    if (!d || (REQ_IS(s, 0x80, 6) && s[3] == 1))
        d = default_device(hub_slot, port, rb);
    return d ? accio_control(xi, d) : USB_IDC_IOFAILED;
}

static uint16_t accio_pipe(int xi)
{
    xfer_t *x = &xfers[xi];
    USBRb *rb = &x->rb;
    uint32_t slot = rb->deviceAddress < 128 ? addr2slot[rb->deviceAddress] : 0;
    if (!slot || rb->endPointId == 0) return USB_IDC_ADDRINV;
    hdev_t *d = &hdevs[slot - 1];
    int in = (rb->flags & USRB_FLAGS_TTYPE_MASK) == USRB_FLAGS_TTYPE_IN;
    uint32_t dci = rb->endPointId * 2u + (in ? 1 : 0);

    ring_t *r = d->ep[dci];
    if (!r && !(r = ep_configure(d, dci, rb))) return USB_IDC_IOFAILED;
    if (d->halted & (1u << dci)) ep_recover(d, dci);

    uint32_t len = rb->buffer1Length, seglen[MAX_SEGS];
    uint32_t nseg = len ? split_64k(dma(rb->buffer1), len, seglen) : 1;
    if (!nseg) return USB_IDC_PARMERR;
    if (r->pending + nseg > RING_ROOM) return USB_IDC_NOBANDWIDTH;

    x->slot  = slot;
    x->dci   = dci;
    x->len   = len;
    x->ring  = r;
    x->ntrbs = nseg;
    r->pending += nseg;
    if (len) {
        push_data(x, r, dma(rb->buffer1), len, nseg, seglen, TRB_NORMAL, TRB_ISP, TRB_IOC,
                  rb->maxPacketSize);
    } else {                                          /* zero-length packet */
        x->nseg = 1;
        x->seg[0] = ring_push(r, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
        x->seg_off[0] = x->seg_len[0] = 0;
    }
    x->first = x->seg[0];
    x->last  = x->seg[x->nseg - 1];
    doorbell(slot, dci);
    return 0;
}

static uint16_t accio(const USBRb *in_rb)
{
    if (!in_rb) return USB_IDC_PARMERR;
    if (!running) return USB_IDC_IOFAILED;
    if (in_rb->controllerId != ctrl_id) return USB_IDC_ADDRINV;

    int xi = xfer_alloc(in_rb);
    if (xi < 0) return USB_IDC_ALLOCERR;
    USBRb *rb = &xfers[xi].rb;
    int control = (rb->flags & USRB_FLAGS_TTYPE_MASK) == USRB_FLAGS_TTYPE_SETUP;
    uint16_t err;

    if ((rb->deviceAddress == 0 && !rh_addressed) ||
        (rh_addressed && rb->deviceAddress == rh_addr))
        err = rh_request(xi);
    else if (!control)
        err = accio_pipe(xi);
    else if (rb->endPointId != 0 || !rb->buffer1 || rb->buffer1Length < 8)
        err = USB_IDC_PARMERR;
    else if (rb->deviceAddress == 0)
        err = accio_default(xi);
    else {
        uint32_t slot = rb->deviceAddress < 128 ? addr2slot[rb->deviceAddress] : 0;
        err = slot ? accio_control(xi, &hdevs[slot - 1]) : USB_IDC_ADDRINV;
    }
    if (err) xfer_free(xi);
    return err;
}

/* ── CANCEL ───────────────────────────────────────────────────────────── */
static uint16_t cancel(const USBCancel *c)
{
    if (!c || c->controllerId != ctrl_id) return USB_IDC_ADDRINV;
    if (rh_addressed && c->deviceAddress == rh_addr) {
        if (rh_int >= 0) { xfer_free(rh_int); rh_int = -1; }
        return 0;
    }
    uint32_t slot = c->deviceAddress && c->deviceAddress < 128
                    ? addr2slot[c->deviceAddress] : 0;
    if (!slot) return 0;                              /* nothing to cancel */
    hdev_t *d = &hdevs[slot - 1];
    if (c->endPointId == 0xff) {                      /* device gone */
        free_slot(d);
        return 0;
    }
    /* One endpoint: stop it, drop its requests, skip the rest of its ring. */
    for (uint32_t dci = 1; dci < 32; dci++) {
        if ((dci >> 1) != c->endPointId) continue;
        ring_t *r = dci == 1 ? &d->ep0 : d->ep[dci];
        if (!r) continue;
        command(0, TRB_TYPE(TRB_STOP_EP) | TRB_SLOT(slot) | TRB_EP(dci), 0);
        xfer_drop(slot, dci);
        command(ring_deq_ptr(r), TRB_TYPE(TRB_SET_TR_DEQ) | TRB_SLOT(slot) |
                TRB_EP(dci), 0);
    }
    return 0;
}

/* ── PRCIRQ: poll, then notify ────────────────────────────────────────── */
/* The slot context must say whether a device is a hub (and how many
 * ports, multi-TT or not) - nothing in the IDC tells the HCD, so it reads
 * that off the descriptors USBD fetches during enumeration. */
/* Tell the controller a device is a hub (Hub, MTT, Number of Ports in the
 * slot context) as soon as that is known - before USBD addresses anything
 * behind it, since those devices' slots name it as their TT hub.
 * Configure Endpoint with only the slot context (as Linux does). */
static void hub_slot_update(hdev_t *d)
{
    usb_zero(in_ctx, sizeof in_ctx);
    ictx(0)[1] = 1u << 0;
    usb_copy(ictx(1), dctx(d), ctx_size);
    ictx(1)[0] |= 1u << 26;
    if (d->hub_mtt && d->speed == XSPEED_HIGH) ictx(1)[0] |= 1u << 25;
    else                                       ictx(1)[0] &= ~(1u << 25);
    ictx(1)[1] = (ictx(1)[1] & 0x00FFFFFFu) | (d->hub_nports << 24);
    dma_barrier();
    if (command(dma(in_ctx), TRB_TYPE(TRB_CONFIGURE_EP) | TRB_SLOT(d->slot), 0) != CC_SUCCESS)
        V("[XHCD]   slot %u: hub slot context update failed\n", d->slot);
}

static void snoop(hdev_t *d, const USBRb *rb)
{
    if ((rb->flags & USRB_FLAGS_TTYPE_MASK) != USRB_FLAGS_TTYPE_SETUP) return;
    const uint8_t *s = rb->buffer1, *b = rb->buffer2;
    if (!s) return;
    if (b && REQ_IS(s, 0x80, 6) && s[3] == 1 && rb->buffer2Length >= 7) {
        d->is_hub  = b[4] == 9;
        d->hub_mtt = 0;              /* until USBD selects the multi-TT setting */
    } else if (b && REQ_IS(s, 0xA0, 6) && s[3] == 0x29 && rb->buffer2Length >= 3) {
        d->hub_nports = b[2];
        if (d->is_hub && d->addr) hub_slot_update(d);
    } else if (REQ_IS(s, 0x01, 11) && d->is_hub) {
        d->hub_mtt = s[2] == 1;      /* SET_INTERFACE alt 1 = multiple TTs */
        if (d->hub_nports) hub_slot_update(d);
    } else if (b && REQ_IS(s, 0xA3, 0) && d->is_hub && rb->buffer2Length >= 1 && !(b[0] & 1)) {
        /* A port with nothing connected: drop a half-enumerated slot left
         * on it (as rh_bitmap does for root ports). */
        hdev_t *dd = find_default(d->slot, s[4]);
        if (dd) free_slot(dd);
    }
}

static void process(void)
{
    if (!running) return;
    trb_t ev;
    while (event_poll(&ev)) handle_event(&ev);

    /* Stalled endpoints: recover now, so requests queued behind the
     * failed one complete (as stalled) rather than wait for the next
     * submission on that endpoint. */
    for (uint32_t i = 0; i < MAX_SLOTS; i++)
        for (uint32_t dci = 1; hdevs[i].slot && hdevs[i].halted && dci < 32; dci++)
            if (hdevs[i].halted & (1u << dci)) ep_recover(&hdevs[i], dci);

    if (rh_int >= 0) {
        uint32_t bm = rh_bitmap();
        if (bm) {
            xfer_t *x = &xfers[rh_int];
            uint32_t n = x->rb.buffer1Length < 4 ? x->rb.buffer1Length : 4;
            usb_copy(x->rb.buffer1, &bm, n);
            x->actual = n;
            xfer_complete(rh_int, 0);
            rh_int = -1;
        }
    }

    /* Deliver completions. The callee may queue new requests. */
    while (cq_h != cq_t) {
        int i = cq[cq_h];
        cq_h = (cq_h + 1) % (MAX_XFERS + 1);
        if (!xfers[i].used) continue;
        USBRb rb;
        usb_copy(&rb, &xfers[i].rb, sizeof rb);
        rb.status = xfers[i].status;
        if ((rb.flags & USRB_FLAGS_TTYPE_MASK) == USRB_FLAGS_TTYPE_SETUP)
            rb.buffer2Length = (uint16_t)xfers[i].actual;
        else
            rb.buffer1Length = (uint16_t)xfers[i].actual;
        if (xfers[i].slot && !rb.status) snoop(&hdevs[xfers[i].slot - 1], &rb);
        xfer_free(i);
        if (rb.usbIDC)
            usb_idc_call(rb.usbIDC, rb.category, USB_IDC_FUNCTION_PRCIRQ, &rb, 0);
    }
}

/* ── IDC entry ────────────────────────────────────────────────────────── */
static void xhci_idc(RP_GENIOCTL *rp)
{
    uint16_t err = 0;
    if (rp->Category != USB_IDC_CATEGORY_HOST) {
        err = USB_IDC_WRONGCAT;
    } else {
        switch (rp->Function) {
        case USB_IDC_FUNCTION_RSTHOST: {
            USBDetach *p = rp->ParmPacket;
            if (!p || p->controllerId != ctrl_id) err = USB_IDC_ADDRINV;
            else if (hc_start() != 0) err = USB_IDC_IOFAILED;
            break;
        }
        case USB_IDC_FUNCTION_ACCIO:  err = accio(rp->ParmPacket); break;
        case USB_IDC_FUNCTION_CANCEL: err = cancel(rp->ParmPacket); break;
        case USB_IDC_FUNCTION_PRCIRQ: process(); break;
        default:                      err = USB_IDC_WRONGFUNC; break;
        }
    }
    rp->Status = err ? (uint16_t)(STATUS_DONE | STERR | err) : STATUS_DONE;
}

/* ── Driver lifecycle ─────────────────────────────────────────────────── */
static void try_register(void)
{
    USBHCD h;
    usb_zero(&h, sizeof h);
    h.usbIDC   = xhci_idc;
    h.hcdCount = 1;
    uint16_t st = usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD,
                               USB_IDC_FUNCTION_REGISTER, &h, 0);
    if (!(st & STERR)) {
        registered = 1;
        ctrl_id = h.hcdID;
    }
}

int xhci_basedevinit(int v)
{
    verbose = v;
    if (pci_find_xhci() != 0) {
        V("[XHCD]   USBXHCD.SYS: no xHCI controller (QEMU: -device qemu-xhci)\n");
        return -1;
    }
    found = 1;
    try_register();                    /* fails if USBD isn't loaded yet */
    return 0;
}

void xhci_init_complete(void)
{
    if (found && !registered) try_register();
    if (found && !registered)
        V("[XHCD]   USBXHCD.SYS: USBD not loaded, controller unused\n");
}
