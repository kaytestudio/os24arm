/**
 * @file usbmouse.c
 * @brief USBMOUSE - HID mouse / tablet class driver (class layer of the
 *        OS/2-style USB stack, see usbd.h)
 *
 * Accepts (CHKSERV) the first HID interface that is not a keyboard and has
 * an interrupt-IN endpoint, then, as a chain of asynchronous requests:
 *
 *   SETCONF (via USBD) -> GET_DESCRIPTOR(HID report) -> SET_IDLE(0)
 *     -> interrupt-IN reads, each resubmitted on completion
 *
 * The device stays in report protocol (the default after configuration)
 * and its report descriptor is parsed for X, Y, wheel and buttons 1-3, so
 * both relative mice (usb-mouse) and absolute tablets (usb-tablet) work.
 * Reports become usbmouse_event_t's for the GUI (kernel_shell_idle).
 */

#include "usbmouse.h"
#include "usbd.h"
#include "usb_util.h"
#include "kprintf.h"

#define REPORT_MAX      64
#define RDESC_MAX       512
#define READS           2
#define EVQ             16
#define MAX_ERRORS      16

#define HID_REQ_SET_IDLE 0x0A

enum { RQ_SETCONF = 1, RQ_RDESC, RQ_IDLE, RQ_REPORT };

typedef struct {
    int      present;
    uint8_t  report_id;
    uint16_t bit;              /* offset after the report ID byte */
    uint8_t  size;
    int32_t  lmin, lmax;
    int      relative;
} field_t;

static int      verbose, registered, active, use_ids;
static uint16_t ctrl;
static uint8_t  addr, iface, ep, ival;
static uint16_t mps, rdesc_len, errors;
static field_t  fx, fy, fwheel, fbtn[3];

static uint8_t  setup_conf[8] __attribute__((aligned(8)));
static uint8_t  setup_req[8]  __attribute__((aligned(8)));
static uint8_t  rdesc[RDESC_MAX] __attribute__((aligned(64)));
static uint8_t  reports[READS][REPORT_MAX] __attribute__((aligned(64)));

static usbmouse_event_t evq[EVQ];
static uint32_t qh, qt;

#define V(...) do { if (verbose) kprintf(__VA_ARGS__); } while (0)

static void usbmouse_idc(RP_GENIOCTL *rp);

/* ── HID report descriptor parsing ────────────────────────────────────── */
static void parse_rdesc(const uint8_t *d, uint32_t len)
{
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    int32_t  lmin = 0, lmax = 0;
    uint32_t usages[16], nusages = 0, umin = 0, umax = 0;
    uint16_t bit = 0;

    usb_zero(&fx, sizeof fx); usb_zero(&fy, sizeof fy);
    usb_zero(&fwheel, sizeof fwheel); usb_zero(fbtn, sizeof fbtn);
    use_ids = 0;

    for (uint32_t i = 0; i < len; ) {
        uint8_t  b = d[i++];
        if (b == 0xFE) {                              /* long item: skip */
            if (i + 1 >= len) break;
            i += 2u + d[i];
            continue;
        }
        uint32_t n = (b & 3) == 3 ? 4 : (b & 3);
        if (i + n > len) break;
        uint32_t u = 0;
        for (uint32_t k = 0; k < n; k++) u |= (uint32_t)d[i + k] << (8 * k);
        int32_t sv = n == 1 ? (int8_t)u : n == 2 ? (int16_t)u : (int32_t)u;
        i += n;
        uint32_t type = (b >> 2) & 3, tag = b >> 4;

        if (type == 1) {                              /* global */
            switch (tag) {
            case 0: page = u; break;
            case 1: lmin = sv; break;
            case 2: lmax = (lmin >= 0 && n < 4) ? (int32_t)u : sv; break;
            case 7: rsize = u; break;
            case 8: rid = u; bit = 0; use_ids = 1; break;
            case 9: rcount = u; break;
            default: break;
            }
        } else if (type == 2) {                       /* local */
            uint32_t full = n == 4 ? u : (page << 16) | u;
            if (tag == 0 && nusages < 16) usages[nusages++] = full;
            else if (tag == 1) umin = full;
            else if (tag == 2) umax = full;
        } else if (type == 0) {                       /* main */
            if (tag == 8) {                           /* Input */
                int constant = u & 1, variable = (u >> 1) & 1, rel = (u >> 2) & 1;
                for (uint32_t f = 0; f < rcount; f++) {
                    uint32_t usage = nusages ? usages[f < nusages ? f : nusages - 1]
                                   : (umin && umin + f <= umax) ? umin + f : 0;
                    if (!constant && variable && usage) {
                        field_t fl = { 1, (uint8_t)rid, bit, (uint8_t)rsize,
                                       lmin, lmax, rel };
                        if (usage == 0x10030 && !fx.present) fx = fl;
                        else if (usage == 0x10031 && !fy.present) fy = fl;
                        else if (usage == 0x10038 && !fwheel.present) fwheel = fl;
                        else if (usage >= 0x90001 && usage <= 0x90003 &&
                                 !fbtn[usage - 0x90001].present)
                            fbtn[usage - 0x90001] = fl;
                    }
                    bit = (uint16_t)(bit + rsize);
                }
            }
            nusages = 0; umin = umax = 0;             /* locals reset */
        }
    }
    /* Everything must come from the same report. */
    if (fy.report_id != fx.report_id) fy.present = 0;
}

static int32_t field(const uint8_t *r, uint32_t len, const field_t *f)
{
    uint32_t v = 0;
    for (uint32_t k = 0; k < f->size && k < 32; k++) {
        uint32_t b = f->bit + k;
        if (b / 8 >= len) break;
        v |= (uint32_t)((r[b / 8] >> (b % 8)) & 1) << k;
    }
    if (f->lmin < 0 && f->size && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size;                          /* sign-extend */
    return (int32_t)v;
}

static void report(const uint8_t *r, uint32_t len)
{
    if (use_ids) {
        if (!len || r[0] != fx.report_id) return;
        r++, len--;
    }
    usbmouse_event_t e;
    usb_zero(&e, sizeof e);
    for (int b = 0; b < 3; b++)
        if (fbtn[b].present && field(r, len, &fbtn[b])) e.buttons |= 1u << b;
    if (fwheel.present) e.wheel = field(r, len, &fwheel);
    int32_t x = field(r, len, &fx), y = field(r, len, &fy);
    if (fx.relative) {
        e.dx = x; e.dy = y;
    } else {
        e.absolute = 1;
        int32_t sx = fx.lmax > fx.lmin ? fx.lmax - fx.lmin : 1;
        int32_t sy = fy.lmax > fy.lmin ? fy.lmax - fy.lmin : 1;
        e.x = (int)(((int64_t)(x - fx.lmin) * USBMOUSE_ABS_MAX) / sx);
        e.y = (int)(((int64_t)(y - fy.lmin) * USBMOUSE_ABS_MAX) / sy);
    }
    uint32_t next = (qt + 1) % EVQ;
    if (next == qh) qh = (qh + 1) % EVQ;              /* full: drop oldest */
    evq[qt] = e;
    qt = next;
}

/* ── Requests ─────────────────────────────────────────────────────────── */
static void submit(USBRb *rb)
{
    rb->controllerId  = ctrl;
    rb->deviceAddress = addr;
    rb->usbIDC        = usbmouse_idc;
    rb->category      = USB_IDC_CATEGORY_CLASS;
    usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD, USB_IDC_FUNCTION_ACCIO, rb, 0);
}

static void control(uint8_t type, uint8_t req, uint16_t value, void *data,
                    uint16_t len, uintptr_t tag)
{
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    usb_setup(setup_req, type, req, value, iface, len);
    rb.flags         = USRB_FLAGS_TTYPE_SETUP;
    rb.buffer1       = setup_req;
    rb.buffer1Length = 8;
    rb.buffer2       = data;
    rb.buffer2Length = len;
    rb.requestData1  = tag;
    submit(&rb);
}

static void read_report(uint32_t i)
{
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    rb.endPointId    = ep;
    rb.flags         = USRB_FLAGS_TTYPE_IN | USRB_FLAGS_DET_INTRPT;
    rb.buffer1       = reports[i];
    rb.buffer1Length = mps < REPORT_MAX ? mps : REPORT_MAX;
    rb.serviceTime   = ival;
    rb.maxPacketSize = mps;
    rb.requestData1  = RQ_REPORT;
    rb.requestData2  = i;
    submit(&rb);
}

/* ── CHKSERV ──────────────────────────────────────────────────────────── */
static void chkserv(USBCDServe *s)
{
    DeviceInfo *di = s ? s->pDeviceInfo : 0;
    if (!di || active) return;

    const uint8_t *c = di->configurationData;
    uint32_t total = di->configLength;
    int in_hid = 0, found_if = -1;
    uint8_t ep_addr = 0;
    uint16_t rlen = 0;
    for (uint32_t i = 0; i + 2 <= total && c[i] >= 2; i += c[i]) {
        const uint8_t *p = &c[i];
        if (i + p[0] > total) break;
        if (p[1] == 4 && p[0] >= 9) {                 /* interface */
            if (found_if >= 0 && ep_addr) break;      /* first one wins */
            in_hid = p[5] == 3 && p[7] != 1;          /* HID, not keyboard */
            if (in_hid) { found_if = p[2]; ep_addr = 0; rlen = 0; }
        } else if (p[1] == 0x21 && p[0] >= 9 && in_hid && p[6] == 0x22) {
            rlen = (uint16_t)(p[7] | (p[8] << 8));    /* report desc length */
        } else if (p[1] == 5 && p[0] >= 7 && in_hid && !ep_addr &&
                   (p[2] & 0x80) && (p[3] & 3) == 3) {
            ep_addr = p[2];
            mps  = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
            ival = p[6];
        }
    }
    if (found_if < 0 || !ep_addr || !rlen) return;    /* not ours */

    ctrl      = di->ctrlID;
    addr      = di->deviceAddress;
    iface     = (uint8_t)found_if;
    ep        = ep_addr & 0xF;
    rdesc_len = rlen > RDESC_MAX ? RDESC_MAX : rlen;
    errors    = 0;
    qh = qt   = 0;
    di->bConfigurationValue = c[5];                   /* accept */
    di->bInterfaceNumber    = iface;
    active = 1;

    USBSetConf sc;
    usb_zero(&sc, sizeof sc);
    sc.controllerId       = ctrl;
    sc.deviceAddress      = addr;
    sc.configurationValue = c[5];
    sc.irqSwitchValue     = RQ_SETCONF;
    sc.category           = USB_IDC_CATEGORY_CLASS;
    sc.setConfiguration   = setup_conf;
    if (usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD, USB_IDC_FUNCTION_SETCONF,
                     &sc, 0) & STERR) {
        active = 0;
        di->bConfigurationValue = 0;
        return;
    }
    V("[USBMOUSE] HID device at address %u: interface %u, EP 0x%02x\n",
      addr, iface, ep_addr);
}

/* ── PRCIRQ ───────────────────────────────────────────────────────────── */
static void completion(USBRb *rb)
{
    if (!active || rb->deviceAddress != addr || rb->controllerId != ctrl) return;
    switch (rb->requestData1) {
    case RQ_SETCONF:
        if (rb->status) { active = 0; usb_release_device(ctrl, addr); break; }
        control(0x81, 6, 0x2200, rdesc, rdesc_len, RQ_RDESC);  /* report desc */
        break;
    case RQ_RDESC:
        if (rb->status) { V("[USBMOUSE] report descriptor read failed\n"); active = 0; usb_release_device(ctrl, addr); break; }
        parse_rdesc(rdesc, rb->buffer2Length);
        if (!fx.present || !fy.present) {
            V("[USBMOUSE] address %u has no X/Y axes, ignored\n", addr);
            active = 0; usb_release_device(ctrl, addr);
            break;
        }
        control(0x21, HID_REQ_SET_IDLE, 0, 0, 0, RQ_IDLE);
        break;
    case RQ_IDLE:                                     /* a stall is not fatal */
        for (uint32_t i = 0; i < READS; i++) read_report(i);
        V("[USBMOUSE] %s ready -> GUI pointer (%u-bit X/Y%s)\n",
          fx.relative ? "mouse" : "tablet", fx.size, fwheel.present ? ", wheel" : "");
        break;
    case RQ_REPORT:
        if (rb->status == 0) {
            errors = 0;
            report(reports[rb->requestData2], rb->buffer1Length);
        } else if (++errors > MAX_ERRORS) {
            V("[USBMOUSE] too many errors, pointer stopped\n");
            active = 0; usb_release_device(ctrl, addr);
            break;
        }
        read_report((uint32_t)rb->requestData2);
        break;
    default:
        break;
    }
}

/* ── IDC entry ────────────────────────────────────────────────────────── */
static void usbmouse_idc(RP_GENIOCTL *rp)
{
    uint16_t err = 0;
    if (rp->Category != USB_IDC_CATEGORY_CLASS) {
        err = USB_IDC_WRONGCAT;
    } else {
        switch (rp->Function) {
        case USB_IDC_FUNCTION_CHKSERV: chkserv(rp->ParmPacket); break;
        case USB_IDC_FUNCTION_PRCIRQ:  completion(rp->ParmPacket); break;
        case USB_IDC_FUNCTION_DETDEV: {
            USBDetach *d = rp->ParmPacket;
            if (d && active && d->controllerId == ctrl && d->deviceAddress == addr) {
                active = 0;
                V("[USBMOUSE] pointer at address %u detached\n", addr);
            }
            break;
        }
        case USB_IDC_FUNCTION_IDLE:
            if (!active) V("[USBMOUSE] no USB mouse/tablet attached\n");
            break;
        default:
            err = USB_IDC_WRONGFUNC;
            break;
        }
    }
    rp->Status = err ? (uint16_t)(STATUS_DONE | STERR | err) : STATUS_DONE;
}

/* ── Lifecycle / API ──────────────────────────────────────────────────── */
static void try_register(void)
{
    USBDClass c = { usbmouse_idc, 0 };
    uint8_t cat = USB_IDC_CATEGORY_CLASS;
    if (!(usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD,
                       USB_IDC_FUNCTION_REGISTER, &c, &cat) & STERR))
        registered = 1;
}

int usbmouse_devinit(int v)
{
    verbose = v;
    try_register();
    return 0;
}

void usbmouse_init_complete(void)
{
    if (!registered) try_register();
    if (!registered) V("[USBMOUSE] USBD not loaded, driver inactive\n");
}

int usbmouse_poll(usbmouse_event_t *ev)
{
    if (!registered) return 0;
    if (qh == qt) usbd_daemon();
    if (qh == qt) return 0;
    *ev = evq[qh];
    qh = (qh + 1) % EVQ;
    return 1;
}
