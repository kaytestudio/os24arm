/**
 * @file usbd.c
 * @brief USBD - the USB stack's root layer, run as a polled daemon
 *
 * What the OS/2 USBD.SYS does, per the EDM2 stack description (see
 * usbd.h): accept host controller (HCD) and class/client driver
 * registration; reset each controller and enumerate it starting from the
 * root hub's device descriptor; handle hubs itself (port power, reset,
 * status-change pipe, attach/detach); offer each non-hub device to the
 * class drivers (CHKSERV - a driver accepts by setting a non-zero
 * bConfigurationValue); route ACCIO/CANCEL to the right HCD; process
 * SETCONF/SETINTF; send DETDEV on unplug and IDLE once initial
 * enumeration is done.
 *
 * There are no threads in this kernel, so "daemon" means usbd_daemon(),
 * called from the console's idle loops: it polls each HCD (PRCIRQ), which
 * delivers queued completions, then services hub port changes. USBD's own
 * enumeration requests are synchronous: they pump the HCDs until their
 * completion arrives.
 *
 * Hubs - the HCDs' software root hubs and external hubs alike - are
 * handled here with the same hub-class requests.
 */

#include "usbd.h"
#include "usb_util.h"
#include "kprintf.h"

#define MAX_HCDS       2
#define MAX_CLASSES    4
#define MAX_DEVS       16
#define MAX_HUB_PORTS  15

/* Standard requests / descriptors */
#define REQ_GET_STATUS      0
#define REQ_CLEAR_FEATURE   1
#define REQ_SET_FEATURE     3
#define REQ_SET_ADDRESS     5
#define REQ_GET_DESCRIPTOR  6
#define REQ_SET_CONFIG      9
#define REQ_SET_INTERFACE   11
#define DESC_DEVICE         1
#define DESC_CONFIG         2
#define DESC_ENDPOINT       5
#define DESC_HUB            0x29
#define CLASS_HUB           9

/* Hub port features / status bits (USB 2.0 ch. 11) */
#define PORT_ENABLE         1
#define PORT_RESET          4
#define PORT_POWER          8
#define C_PORT_CONNECTION   16
#define C_PORT_ENABLE       17
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET        20
#define PS_CONNECTION       (1u << 0)
#define PS_ENABLE           (1u << 1)
#define PS_LOW_SPEED        (1u << 9)
#define PS_HIGH_SPEED       (1u << 10)
#define PS_SUPER_SPEED      (1u << 15)   /* local: xHCI root hub only */
#define PC_CONNECTION       (1u << 0)
#define PC_ENABLE           (1u << 1)
#define PC_OVER_CURRENT     (1u << 3)
#define PC_RESET            (1u << 4)

/* requestData1 tags for USBD's own requests */
#define RQ_SYNC     1
#define RQ_HUB_INT  2

typedef struct {
    usb_idc_fn idc;
    uint16_t   first, count;
} hcd_t;

typedef struct {
    usb_idc_fn idc;
    void      *ds;
    uint8_t    category;
    int        idle_sent;
} cls_t;

typedef struct {
    int      used;
    uint16_t ctrl;
    uint8_t  addr;
    uint8_t  speed;          /* USB_SPEED_* */
    uint16_t mps0;
    int8_t   parent;         /* index of parent hub, -1 = this is a root hub */
    uint8_t  port;
    int8_t   owner;          /* class driver index, -1 = unclaimed */
    uint8_t  is_hub;
    uint8_t  nports;
    uint8_t  hub_ep;         /* status-change endpoint */
    uint8_t  hub_ival;
    uint16_t hub_mps;
    volatile int hub_change; /* status-change pipe fired */
    int      hub_armed;
    uint8_t  hub_bitmap[4] __attribute__((aligned(8)));
    DeviceInfo info;
} usbd_dev_t;

/* Completion slots for USBD's synchronous requests. They are static and
 * generation-tagged (requestData2 = slot | generation << 8), so a
 * completion arriving after its request timed out - CANCEL can't reach a
 * device still at address 0 - is recognised and ignored instead of
 * writing into a stack frame that no longer exists. */
#define SYNC_SLOTS 8
typedef struct {
    volatile int done;
    int          busy;
    uint32_t     gen;
    uint8_t      status;
    uint16_t     actual;
} sync_t;
static sync_t   syncs[SYNC_SLOTS];
static uint32_t sync_gen;

static int        loaded, verbose, started, in_daemon;
static hcd_t      hcds[MAX_HCDS];
static int        nhcds;
static uint16_t   next_ctrl = 1;
static cls_t      classes[MAX_CLASSES];
static int        nclasses;
static usbd_dev_t devs[MAX_DEVS];
static uint8_t    xbuf[MAX_CONFIG_LENGTH] __attribute__((aligned(64)));
static uint8_t    sbuf[64] __attribute__((aligned(64)));  /* small replies */

#define V(...) do { if (verbose) kprintf(__VA_ARGS__); } while (0)

static const char *speed_name(uint8_t s)
{
    switch (s) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    default:              return "full";
    }
}

/* ── IDC plumbing ─────────────────────────────────────────────────────── */
uint16_t usb_idc_call(usb_idc_fn idc, uint8_t category, uint8_t function,
                      void *parm, void *data)
{
    RP_GENIOCTL rp;
    usb_zero(&rp, sizeof rp);
    rp.Len = sizeof rp;
    rp.Cmd = CMDGenIOCTL;
    rp.Category = category;
    rp.Function = function;
    rp.ParmPacket = parm;
    rp.DataPacket = data;
    if (!idc) return STATUS_DONE | STERR | USB_IDC_ADDRINV;
    idc(&rp);
    return rp.Status;
}

static hcd_t *hcd_of(uint16_t ctrl)
{
    for (int i = 0; i < nhcds; i++)
        if (ctrl >= hcds[i].first && ctrl < hcds[i].first + hcds[i].count)
            return &hcds[i];
    return 0;
}

static uint16_t hcd_call(uint16_t ctrl, uint8_t function, void *parm)
{
    hcd_t *h = hcd_of(ctrl);
    if (!h) return STATUS_DONE | STERR | USB_IDC_ADDRINV;
    return usb_idc_call(h->idc, USB_IDC_CATEGORY_HOST, function, parm, 0);
}

/* Poll every controller: delivers completed requests (PRCIRQ). */
static void pump(void)
{
    for (int i = 0; i < nhcds; i++)
        for (uint16_t c = 0; c < hcds[i].count; c++) {
            uint16_t id = (uint16_t)(hcds[i].first + c);
            usb_idc_call(hcds[i].idc, USB_IDC_CATEGORY_HOST,
                         USB_IDC_FUNCTION_PRCIRQ, &id, 0);
        }
}

/* ── Device table ─────────────────────────────────────────────────────── */
static usbd_dev_t *dev_alloc(void)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (!devs[i].used) {
            usb_zero(&devs[i], sizeof devs[i]);
            devs[i].used = 1;
            devs[i].owner = -1;
            return &devs[i];
        }
    return 0;
}

static int dev_index(const usbd_dev_t *d) { return (int)(d - devs); }

static usbd_dev_t *dev_by_addr(uint16_t ctrl, uint8_t addr)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (devs[i].used && devs[i].ctrl == ctrl && devs[i].addr == addr && addr)
            return &devs[i];
    return 0;
}

static uint8_t addr_alloc(uint16_t ctrl)
{
    for (uint8_t a = 1; a < 128; a++)
        if (!dev_by_addr(ctrl, a)) return a;
    return 0;
}

/* ── Synchronous requests (enumeration) ───────────────────────────────── */
static void fill_routing(USBRb *rb, const usbd_dev_t *d)
{
    if (d->speed == USB_SPEED_LOW)  rb->flags |= USRB_FLAGS_DET_LOWSPEED;
    if (d->speed == USB_SPEED_HIGH) rb->flags |= USRB_FLAGS_DET_HIGHSPEED;
    if (d->parent >= 0) {
        rb->HubAddr  = devs[d->parent].addr;
        rb->PortNumb = d->port;
    }
}

/* Control transfer to `d` at `addr` (0 while unaddressed); waits. */
static int control_at(usbd_dev_t *d, uint8_t addr, uint8_t type, uint8_t req,
                      uint16_t value, uint16_t index, void *data, uint16_t len,
                      uint16_t *actual)
{
    /* The controller DMAs into `data`, possibly late (see sync_t), so it
     * must be static: xbuf itself, or bounced through sbuf. */
    void *dma = data;
    if (data && data != xbuf) {
        if (len > sizeof sbuf) return -1;
        dma = sbuf;
        if (!(type & 0x80)) usb_copy(sbuf, data, len);
    }
    int si = -1;
    for (int i = 0; i < SYNC_SLOTS; i++)
        if (!syncs[i].busy) { si = i; break; }
    if (si < 0) return -1;
    sync_t *s = &syncs[si];
    s->busy = 1;
    s->done = 0;
    s->gen  = ++sync_gen & 0xFFFFFF;

    uint8_t setup[8];                                 /* copied by the HCD */
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    usb_setup(setup, type, req, value, index, len);
    rb.controllerId  = d->ctrl;
    rb.deviceAddress = addr;
    rb.flags         = USRB_FLAGS_TTYPE_SETUP;
    rb.buffer1       = setup;
    rb.buffer1Length = 8;
    rb.buffer2       = dma;
    rb.buffer2Length = len;
    rb.maxPacketSize = d->mps0;
    rb.usbIDC        = usbd_idc;
    rb.category      = USB_IDC_CATEGORY_USBD;
    rb.requestData1  = RQ_SYNC;
    rb.requestData2  = (uintptr_t)si | ((uintptr_t)s->gen << 8);
    rb.maxErrorCount = 3;
    fill_routing(&rb, d);

    int ok = -1;
    if (!(hcd_call(d->ctrl, USB_IDC_FUNCTION_ACCIO, &rb) & STERR)) {
        u64 end = usb_ticks() + usb_ms(1000);
        while (!s->done && usb_ticks() < end) pump();
        if (!s->done) {
            USBCancel c = { d->ctrl, addr, 0 };       /* best effort */
            hcd_call(d->ctrl, USB_IDC_FUNCTION_CANCEL, &c);
        } else {
            if (actual) *actual = s->actual;
            ok = s->status == 0 ? 0 : -1;
            if (ok == 0 && dma == sbuf && (type & 0x80))
                usb_copy(data, sbuf, s->actual < len ? s->actual : len);
        }
    }
    s->busy = 0;                    /* a late completion now fails the gen check */
    return ok;
}

static int control(usbd_dev_t *d, uint8_t type, uint8_t req, uint16_t value,
                   uint16_t index, void *data, uint16_t len)
{
    return control_at(d, d->addr, type, req, value, index, data, len, 0);
}

/* ── Hub helpers ──────────────────────────────────────────────────────── */
static int port_status(usbd_dev_t *hub, uint8_t port, uint16_t *st, uint16_t *ch)
{
    uint8_t b[4] __attribute__((aligned(8)));
    if (control(hub, 0xA3, REQ_GET_STATUS, 0, port, b, 4) != 0) return -1;
    *st = (uint16_t)(b[0] | (b[1] << 8));
    *ch = (uint16_t)(b[2] | (b[3] << 8));
    return 0;
}

static int port_feature(usbd_dev_t *hub, uint8_t port, int set, uint16_t f)
{
    return control(hub, 0x23, set ? REQ_SET_FEATURE : REQ_CLEAR_FEATURE,
                   f, port, 0, 0);
}

static void hub_arm(usbd_dev_t *hub)
{
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    rb.controllerId  = hub->ctrl;
    rb.deviceAddress = hub->addr;
    rb.endPointId    = hub->hub_ep;
    rb.flags         = USRB_FLAGS_TTYPE_IN | USRB_FLAGS_DET_INTRPT;
    rb.buffer1       = hub->hub_bitmap;
    rb.buffer1Length = (uint16_t)((hub->nports + 8) / 8);
    rb.serviceTime   = hub->hub_ival;
    rb.maxPacketSize = hub->hub_mps;
    rb.usbIDC        = usbd_idc;
    rb.category      = USB_IDC_CATEGORY_USBD;
    rb.requestData1  = RQ_HUB_INT;
    rb.requestData2  = (uintptr_t)dev_index(hub);
    rb.maxErrorCount = 3;
    fill_routing(&rb, hub);
    hub->hub_armed = !(hcd_call(hub->ctrl, USB_IDC_FUNCTION_ACCIO, &rb) & STERR);
}

/* ── Enumeration ──────────────────────────────────────────────────────── */
static void port_attach(usbd_dev_t *hub, uint8_t port);
static void dev_detach(usbd_dev_t *d);

/* Address device `d` (currently at address 0) and read its descriptors. */
static int dev_setup(usbd_dev_t *d)
{
    d->mps0 = d->speed == USB_SPEED_SUPER ? 512 : d->speed == USB_SPEED_HIGH ? 64 : 8;
    usb_zero(xbuf, 18);
    if (control_at(d, 0, 0x80, REQ_GET_DESCRIPTOR, DESC_DEVICE << 8, 0, xbuf, 8, 0))
        return -1;
    d->mps0 = d->speed == USB_SPEED_SUPER ? (uint16_t)(1u << (xbuf[7] & 15)) : xbuf[7];
    if (!d->mps0) d->mps0 = 8;

    uint8_t addr = addr_alloc(d->ctrl);
    if (!addr) return -1;
    if (control_at(d, 0, 0x00, REQ_SET_ADDRESS, addr, 0, 0, 0, 0)) return -1;
    d->addr = addr;
    usb_delay_ms(2);                                  /* SET_ADDRESS recovery */

    DeviceInfo *di = &d->info;
    if (control(d, 0x80, REQ_GET_DESCRIPTOR, DESC_DEVICE << 8, 0, xbuf, 18))
        return -1;
    usb_copy(&di->descriptor, xbuf, sizeof di->descriptor);

    if (control(d, 0x80, REQ_GET_DESCRIPTOR, DESC_CONFIG << 8, 0, xbuf, 9))
        return -1;
    uint16_t total = (uint16_t)(xbuf[2] | (xbuf[3] << 8));
    if (total > MAX_CONFIG_LENGTH) total = MAX_CONFIG_LENGTH;
    if (total < 9) total = 9;
    uint16_t got = 0;
    if (control_at(d, d->addr, 0x80, REQ_GET_DESCRIPTOR, DESC_CONFIG << 8, 0,
                   xbuf, total, &got))
        return -1;
    usb_copy(di->configurationData, xbuf, got);
    di->configLength        = got;
    di->ctrlID              = d->ctrl;
    di->deviceAddress       = d->addr;
    di->bConfigurationValue = 0;
    di->lowSpeedDevice      = d->speed;
    di->portNum             = d->port;
    di->parentHubIndex      = (d->parent >= 0 && devs[d->parent].parent >= 0)
                              ? (int16_t)d->parent : -1;
    d->is_hub = di->descriptor.bDeviceClass == CLASS_HUB;
    return 0;
}

/* Configure a hub, power its ports, scan them and arm its status pipe. */
static int hub_start(usbd_dev_t *hub)
{
    const uint8_t *cfg = hub->info.configurationData;
    for (uint16_t i = 0; i + 2 <= hub->info.configLength && cfg[i] >= 2; i += cfg[i])
        if (cfg[i + 1] == DESC_ENDPOINT && (cfg[i + 2] & 0x80) && (cfg[i + 3] & 3) == 3) {
            hub->hub_ep   = cfg[i + 2] & 0xF;
            hub->hub_mps  = (uint16_t)(cfg[i + 4] | (cfg[i + 5] << 8));
            hub->hub_ival = cfg[i + 6];
            break;
        }
    if (!hub->hub_ep) return -1;

    if (control(hub, 0x00, REQ_SET_CONFIG, cfg[5], 0, 0, 0)) return -1;
    hub->info.bConfigurationValue = cfg[5];
    if (control(hub, 0xA0, REQ_GET_DESCRIPTOR, DESC_HUB << 8, 0, xbuf, 9)) return -1;
    hub->nports = xbuf[2] > MAX_HUB_PORTS ? MAX_HUB_PORTS : xbuf[2];
    uint32_t pgood = xbuf[5] * 2u;

    for (uint8_t p = 1; p <= hub->nports; p++)
        port_feature(hub, p, 1, PORT_POWER);
    usb_delay_ms(pgood > 20 ? pgood : 20);

    V("[USBD]   controller %u: %s hub, %u ports, address %u\n", hub->ctrl,
      hub->parent < 0 ? "root" : "external", hub->nports, hub->addr);

    for (uint8_t p = 1; p <= hub->nports; p++) {
        uint16_t st, ch;
        if (port_status(hub, p, &st, &ch)) continue;
        if (ch & PC_CONNECTION) port_feature(hub, p, 0, C_PORT_CONNECTION);
        if (st & PS_CONNECTION) port_attach(hub, p);
    }
    hub_arm(hub);
    return 0;
}

/* Offer a device to the class/client drivers until one claims it. */
static void dev_offer(usbd_dev_t *d)
{
    for (int i = 0; i < nclasses && d->owner < 0; i++) {
        if (!classes[i].idc) continue;
        USBCDServe s = { &d->info, 0 };
        d->info.bConfigurationValue = 0;
        /* Tentative owner, so the driver may SETCONF from inside CHKSERV. */
        d->owner = (int8_t)i;
        usb_idc_call(classes[i].idc, classes[i].category,
                     USB_IDC_FUNCTION_CHKSERV, &s, 0);
        if (!d->info.bConfigurationValue) d->owner = -1;
    }
}

static void port_attach(usbd_dev_t *hub, uint8_t port)
{
    if (port_feature(hub, port, 1, PORT_RESET)) return;
    uint16_t st = 0, ch = 0;
    u64 end = usb_ticks() + usb_ms(500);
    do {
        usb_delay_ms(10);
        if (port_status(hub, port, &st, &ch)) return;
    } while (!(ch & PC_RESET) && usb_ticks() < end);
    port_feature(hub, port, 0, C_PORT_RESET);
    if (ch & PC_ENABLE) port_feature(hub, port, 0, C_PORT_ENABLE);
    if (!(st & PS_ENABLE)) {
        V("[USBD]   hub %u port %u: reset failed\n", hub->addr, port);
        return;
    }
    usb_delay_ms(10);                                 /* reset recovery */

    usbd_dev_t *d = dev_alloc();
    if (!d) { V("[USBD]   hub %u port %u: device table full\n", hub->addr, port); return; }
    d->ctrl   = hub->ctrl;
    d->parent = (int8_t)dev_index(hub);
    d->port   = port;
    d->speed  = (st & PS_SUPER_SPEED) ? USB_SPEED_SUPER :
                (st & PS_HIGH_SPEED)  ? USB_SPEED_HIGH  :
                (st & PS_LOW_SPEED)   ? USB_SPEED_LOW   : USB_SPEED_FULL;

    if (dev_setup(d) != 0) {
        V("[USBD]   hub %u port %u: enumeration failed\n", hub->addr, port);
        dev_detach(d);
        return;
    }
    const DeviceDescriptor *dd = &d->info.descriptor;
    if (d->is_hub) {
        V("[USBD]   hub %u port %u: %s-speed hub %04x:%04x, address %u\n", hub->addr, port,
          speed_name(d->speed), dd->idVendor, dd->idProduct, d->addr);
        hub_start(d);
        return;
    }
    dev_offer(d);
    V("[USBD]   hub %u port %u: %s-speed device %04x:%04x class %02x, address %u, %s\n",
      hub->addr, port, speed_name(d->speed), dd->idVendor, dd->idProduct,
      dd->bDeviceClass, d->addr,
      d->owner >= 0 ? "claimed" : "no driver");
}

/* Detach a device (and anything behind it): DETDEV to its owner, cancel
 * its I/O at the HCD, free its address. */
static void dev_detach(usbd_dev_t *d)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (devs[i].used && devs[i].parent == dev_index(d))
            dev_detach(&devs[i]);

    if (d->addr) {
        if (d->owner >= 0 && classes[d->owner].idc) {
            USBDetach det = { d->ctrl, d->addr };
            usb_idc_call(classes[d->owner].idc, classes[d->owner].category,
                         USB_IDC_FUNCTION_DETDEV, &det, 0);
        }
        USBCancel c = { d->ctrl, d->addr, 0xff };
        hcd_call(d->ctrl, USB_IDC_FUNCTION_CANCEL, &c);
    }
    d->used = 0;
}

static void port_detach(usbd_dev_t *hub, uint8_t port)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (devs[i].used && devs[i].parent == dev_index(hub) && devs[i].port == port) {
            V("[USBD]   hub %u port %u: device %04x:%04x (address %u) detached\n", hub->addr, port,
              devs[i].info.descriptor.idVendor, devs[i].info.descriptor.idProduct,
              devs[i].addr);
            dev_detach(&devs[i]);
        }
}

/* Status-change pipe fired: look at each flagged port. */
static void hub_service(usbd_dev_t *hub)
{
    hub->hub_change = 0;
    if (hub->hub_bitmap[0] & 1) {                     /* the hub itself */
        uint8_t *b = sbuf;
        if (control(hub, 0xA0, REQ_GET_STATUS, 0, 0, b, 4) == 0) {
            uint16_t ch = (uint16_t)(b[2] | (b[3] << 8));
            if (ch & 1) control(hub, 0x20, REQ_CLEAR_FEATURE, 0, 0, 0, 0); /* C_HUB_LOCAL_POWER */
            if (ch & 2) {                                                  /* C_HUB_OVER_CURRENT */
                control(hub, 0x20, REQ_CLEAR_FEATURE, 1, 0, 0, 0);
                V("[USBD]   hub %u: over-current\n", hub->addr);
            }
        }
    }
    for (uint8_t p = 1; p <= hub->nports; p++) {
        if (!(hub->hub_bitmap[p / 8] & (1u << (p % 8)))) continue;
        uint16_t st, ch;
        if (port_status(hub, p, &st, &ch)) continue;
        if (ch & PC_ENABLE)       port_feature(hub, p, 0, C_PORT_ENABLE);
        if (ch & PC_OVER_CURRENT) port_feature(hub, p, 0, C_PORT_OVER_CURRENT);
        if (ch & PC_RESET)        port_feature(hub, p, 0, C_PORT_RESET);
        if (ch & PC_CONNECTION) {
            port_feature(hub, p, 0, C_PORT_CONNECTION);
            port_detach(hub, p);
            if (st & PS_CONNECTION) {
                usb_delay_ms(100);                    /* debounce */
                port_attach(hub, p);
            }
        }
    }
    usb_zero(hub->hub_bitmap, sizeof hub->hub_bitmap);
    hub_arm(hub);
}

/* Reset a controller through its HCD and enumerate from its root hub. */
static void ctrl_start(uint16_t ctrl)
{
    USBDetach r = { ctrl, 0 };
    if (hcd_call(ctrl, USB_IDC_FUNCTION_RSTHOST, &r) & STERR) {
        V("[USBD]   controller %u: reset failed\n", ctrl);
        return;
    }
    usbd_dev_t *rh = dev_alloc();
    if (!rh) return;
    rh->ctrl   = ctrl;
    rh->parent = -1;
    rh->speed  = USB_SPEED_HIGH;
    if (dev_setup(rh) != 0 || !rh->is_hub || hub_start(rh) != 0) {
        V("[USBD]   controller %u: root hub enumeration failed\n", ctrl);
        rh->used = 0;
    }
}

static void send_idle(void)
{
    for (int i = 0; i < nclasses; i++)
        if (classes[i].idc && !classes[i].idle_sent) {
            classes[i].idle_sent = 1;
            usb_idc_call(classes[i].idc, classes[i].category,
                         USB_IDC_FUNCTION_IDLE, 0, 0);
        }
}

/* ── USBD IDC entry ("USBD$") ─────────────────────────────────────────── */
static uint16_t do_register(RP_GENIOCTL *rp)
{
    if (!rp->ParmPacket) return USB_IDC_PARMERR;
    if (!rp->DataPacket) {                            /* host controller */
        USBHCD *h = rp->ParmPacket;
        if (nhcds >= MAX_HCDS || !h->usbIDC || !h->hcdCount) return USB_IDC_PARMERR;
        h->hcdID = next_ctrl;
        hcds[nhcds].idc   = h->usbIDC;
        hcds[nhcds].first = next_ctrl;
        hcds[nhcds].count = h->hcdCount;
        nhcds++;
        next_ctrl = (uint16_t)(next_ctrl + h->hcdCount);
        V("[USBD]   host controller driver registered: controller %u%s\n",
          h->hcdID, h->hcdCount > 1 ? "+" : "");
        if (started)                                  /* late registration */
            for (uint16_t c = 0; c < h->hcdCount; c++)
                ctrl_start((uint16_t)(h->hcdID + c));
        return 0;
    }
    USBDClass *c = rp->ParmPacket;                    /* class / client */
    uint8_t cat = *(uint8_t *)rp->DataPacket;
    if (nclasses >= MAX_CLASSES || !c->usbIDC ||
        (cat != USB_IDC_CATEGORY_CLASS && cat != USB_IDC_CATEGORY_CLIENT))
        return USB_IDC_PARMERR;
    classes[nclasses].idc      = c->usbIDC;
    classes[nclasses].ds       = c->usbDS;
    classes[nclasses].category = cat;
    nclasses++;
    if (started) {                       /* offer what nobody claimed yet */
        for (int i = 0; i < MAX_DEVS; i++)
            if (devs[i].used && !devs[i].is_hub && devs[i].owner < 0 && devs[i].addr)
                dev_offer(&devs[i]);
        send_idle();
    }
    return 0;
}

static uint16_t do_unregister(RP_GENIOCTL *rp)
{
    for (int i = 0; i < nclasses; i++)
        if (rp->ParmPacket && classes[i].idc == ((USBDClass *)rp->ParmPacket)->usbIDC) {
            for (int d = 0; d < MAX_DEVS; d++)
                if (devs[d].used && devs[d].owner == i) devs[d].owner = -1;
            classes[i].idc = 0;
            return 0;
        }
    return USB_IDC_PARMERR;
}

static uint16_t do_accio(USBRb *rb)
{
    usbd_dev_t *d = dev_by_addr(rb->controllerId, rb->deviceAddress);
    if (!d) return USB_IDC_ADDRINV;
    if (rb->endPointId > 15) return USB_IDC_PARMERR;
    rb->flags &= (uint16_t)~(USRB_FLAGS_DET_LOWSPEED | USRB_FLAGS_DET_HIGHSPEED);
    fill_routing(rb, d);
    if (rb->endPointId == 0 && !rb->maxPacketSize) rb->maxPacketSize = d->mps0;
    if (!rb->maxErrorCount) rb->maxErrorCount = 3;
    uint16_t st = hcd_call(rb->controllerId, USB_IDC_FUNCTION_ACCIO, rb);
    return (st & STERR) ? (uint16_t)(st & 0x7F) : 0;
}

/* SETCONF / SETINTF: issue the request on the class driver's behalf; its
 * completion goes to the device's owner with requestData1 =
 * irqSwitchValue. */
static uint16_t do_setconf(USBSetConf *sc, int intf)
{
    if (!sc || !sc->setConfiguration) return USB_IDC_PARMERR;
    usbd_dev_t *d = dev_by_addr(sc->controllerId, sc->deviceAddress);
    if (!d) return USB_IDC_ADDRINV;
    if (d->owner < 0) return USB_IDC_SERVREJCTD;

    if (intf)
        usb_setup(sc->setConfiguration, 0x01, REQ_SET_INTERFACE,
                  sc->configurationValue, d->info.bInterfaceNumber, 0);
    else {
        usb_setup(sc->setConfiguration, 0x00, REQ_SET_CONFIG,
                  sc->configurationValue, 0, 0);
        d->info.bConfigurationValue = sc->configurationValue;
    }
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    rb.controllerId  = d->ctrl;
    rb.deviceAddress = d->addr;
    rb.flags         = USRB_FLAGS_TTYPE_SETUP;
    rb.buffer1       = sc->setConfiguration;
    rb.buffer1Length = 8;
    rb.usbIDC        = classes[d->owner].idc;
    rb.usbDS         = sc->classDriverDS;
    rb.category      = sc->category;
    rb.requestData1  = sc->irqSwitchValue;
    return do_accio(&rb);
}

static uint16_t do_reset_port(USBDetach *p)
{
    usbd_dev_t *d = p ? dev_by_addr(p->controllerId, p->deviceAddress) : 0;
    if (!d || d->parent < 0) return USB_IDC_ADDRINV;
    usbd_dev_t *hub = &devs[d->parent];
    uint8_t port = d->port;
    dev_detach(d);
    port_attach(hub, port);                           /* enumerate it again */
    return 0;
}

/* Completion of one of USBD's own requests. */
static void own_completion(USBRb *rb)
{
    if (rb->requestData1 == RQ_SYNC) {
        uint32_t si = (uint32_t)(rb->requestData2 & 0xFF);
        uint32_t gen = (uint32_t)(rb->requestData2 >> 8);
        if (si >= SYNC_SLOTS || !syncs[si].busy || syncs[si].gen != gen)
            return;                                   /* stale: timed out */
        syncs[si].status = rb->status;
        syncs[si].actual = rb->buffer2Length;
        syncs[si].done   = 1;
    } else if (rb->requestData1 == RQ_HUB_INT && rb->requestData2 < MAX_DEVS) {
        usbd_dev_t *hub = &devs[rb->requestData2];
        hub->hub_armed = 0;
        if (hub->used && hub->is_hub && rb->status == 0)
            hub->hub_change = 1;
    }
}

void usbd_idc(RP_GENIOCTL *rp)
{
    uint16_t err = 0;
    if (rp->Cmd != CMDGenIOCTL) {
        err = USB_IDC_WRONGFUNC;
    } else if (rp->Category != USB_IDC_CATEGORY_USBD) {
        err = USB_IDC_WRONGCAT;
    } else if (!loaded) {
        err = USB_IDC_SERVREJCTD;
    } else {
        switch (rp->Function) {
        case USB_IDC_FUNCTION_REGISTER:   err = do_register(rp); break;
        case USB_IDC_FUNCTION_UNREGISTER: err = do_unregister(rp); break;
        case USB_IDC_FUNCTION_ACCIO:      err = do_accio(rp->ParmPacket); break;
        case USB_IDC_FUNCTION_CANCEL: {
            USBCancel *c = rp->ParmPacket;
            err = c ? (uint16_t)(hcd_call(c->controllerId, USB_IDC_FUNCTION_CANCEL, c) & 0x7F)
                    : USB_IDC_PARMERR;
            break;
        }
        case USB_IDC_FUNCTION_SETCONF:    err = do_setconf(rp->ParmPacket, 0); break;
        case USB_IDC_FUNCTION_SETINTF:    err = do_setconf(rp->ParmPacket, 1); break;
        case USB_IDC_FUNCTION_RESET_PORT: err = do_reset_port(rp->ParmPacket); break;
        case USB_IDC_FUNCTION_CMPL_INI:   usbd_init_complete(); break;
        case USB_IDC_FUNCTION_PRCIRQ:     own_completion(rp->ParmPacket); break;
        case USB_IDC_FUNCTION_CLRSTALL:   /* not implemented yet */
        default:                          err = USB_IDC_WRONGFUNC; break;
        }
    }
    rp->Status = err ? (uint16_t)(STATUS_DONE | STERR | err) : STATUS_DONE;
}

/* ── Driver lifecycle ─────────────────────────────────────────────────── */
int usbd_basedevinit(int v)
{
    verbose = v;
    loaded  = 1;
    V("[USBD]   USBD.SYS: USB root layer loaded (polled daemon)\n");
    return 0;
}

void usbd_init_complete(void)
{
    if (!loaded || started) return;
    started = 1;
    for (int i = 0; i < nhcds; i++)
        for (uint16_t c = 0; c < hcds[i].count; c++)
            ctrl_start((uint16_t)(hcds[i].first + c));
    send_idle();
}

int usbd_loaded(void) { return loaded; }

void usbd_pump(void) { if (loaded) pump(); }

void usbd_daemon(void)
{
    if (!loaded || in_daemon) return;
    in_daemon = 1;
    pump();
    for (int i = 0; i < MAX_DEVS; i++) {
        usbd_dev_t *d = &devs[i];
        if (!d->used || !d->is_hub) continue;
        if (d->hub_change) hub_service(d);
        else if (!d->hub_armed && d->nports) hub_arm(d);
    }
    in_daemon = 0;
}

void usbd_list(void)
{
    if (!loaded) { kprintf("USBD.SYS is not loaded (see CONFIG.SYS)\n"); return; }
    kprintf("Ctl Addr Hub Port Speed  VID:PID    Class  Driver\n");
    int n = 0;
    for (int i = 0; i < MAX_DEVS; i++) {
        usbd_dev_t *d = &devs[i];
        if (!d->used || !d->addr) continue;
        const DeviceDescriptor *dd = &d->info.descriptor;
        kprintf("%3u %4u %3u %4u %-6s %04x:%04x  %02x     %s\n",
                d->ctrl, d->addr, d->parent >= 0 ? devs[d->parent].addr : 0u,
                d->port, speed_name(d->speed),
                dd->idVendor, dd->idProduct, dd->bDeviceClass,
                d->parent < 0 ? "(root hub)" : d->is_hub ? "(hub)" :
                d->owner >= 0 ? "claimed" : "-");
        n++;
    }
    if (!n) kprintf("(no devices)\n");
}

void usbd_summary(void)
{
    int devices = 0, claimed = 0, ctrls = 0;
    for (int i = 0; i < nhcds; i++) ctrls += hcds[i].count;
    for (int i = 0; i < MAX_DEVS; i++)
        if (devs[i].used && devs[i].parent >= 0) {
            devices++;
            if (devs[i].owner >= 0) claimed++;
        }
    kprintf("[USB]    USBD: %d host controller(s), %d device(s), %d claimed by drivers\n",
            ctrls, devices, claimed);
}
