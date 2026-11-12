/**
 * @file usbkbd.c
 * @brief USBKBD - HID boot keyboard class driver (class layer of the
 *        OS/2-style USB stack, see usbd.h)
 *
 * Registers with USBD as a class driver (Category 0x92). USBD offers
 * every new device with CHKSERV; this driver accepts the first one with
 * a HID / boot / keyboard interface by setting bConfigurationValue, then
 * runs the rest as a chain of asynchronous requests, each completion
 * (PRCIRQ, told apart by requestData1) starting the next:
 *
 *   SETCONF (via USBD) -> SET_PROTOCOL(boot) -> SET_IDLE(0)
 *     -> KBD_REPORTS interrupt-IN requests, each resubmitted on completion
 *
 * 8-byte boot reports become console bytes (US layout, Shift/Ctrl/Caps
 * Lock, VT100 sequences for cursor keys) with typematic repeat, and Caps
 * Lock drives the keyboard's LED (SET_REPORT). DETDEV drops the keyboard;
 * the next one plugged in is accepted the same way.
 */

#include "usbkbd.h"
#include "usbd.h"
#include "usb_util.h"
#include "kprintf.h"

#define KBD_REPORTS        4
#define KBD_REPEAT_DELAY   500                /* ms before a held key repeats */
#define KBD_REPEAT_RATE    33                 /* ms between repeats (~30/s) */
#define KBD_MAX_ERRORS     16

#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_LED_CAPS_LOCK    (1u << 1)

/* requestData1 values ("IRQ switch") */
enum { RQ_SETCONF = 1, RQ_PROTOCOL, RQ_IDLE, RQ_REPORT, RQ_LED };

static int      verbose, registered, active;
static uint16_t ctrl;
static uint8_t  addr, iface, ep, ival;
static uint16_t mps, errors;

static uint8_t  setup_conf[8]  __attribute__((aligned(8)));
static uint8_t  setup_req[8]   __attribute__((aligned(8)));
static uint8_t  setup_led[8]   __attribute__((aligned(8)));
static uint8_t  led_data[8]    __attribute__((aligned(64)));
static uint8_t  reports[KBD_REPORTS][8] __attribute__((aligned(64)));

static uint8_t  prev[8];
static int      caps, led_busy, led_pending;
static uint8_t  rep_key, rep_mods;
static u64      rep_at;
static char     q[64];
static uint32_t qh, qt;

#define V(...) do { if (verbose) kprintf(__VA_ARGS__); } while (0)

static void usbkbd_idc(RP_GENIOCTL *rp);

/* ── Requests ─────────────────────────────────────────────────────────── */
static uint16_t submit(USBRb *rb)
{
    rb->controllerId  = ctrl;
    rb->deviceAddress = addr;
    rb->usbIDC        = usbkbd_idc;
    rb->category      = USB_IDC_CATEGORY_CLASS;
    return usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD,
                        USB_IDC_FUNCTION_ACCIO, rb, 0);
}

static void class_request(uint8_t *setup, uint8_t req, uint16_t value,
                          void *data, uint16_t len, uintptr_t tag)
{
    USBRb rb;
    usb_zero(&rb, sizeof rb);
    usb_setup(setup, 0x21, req, value, iface, len);
    rb.flags         = USRB_FLAGS_TTYPE_SETUP;
    rb.buffer1       = setup;
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
    rb.buffer1Length = 8;
    rb.serviceTime   = ival;
    rb.maxPacketSize = mps;
    rb.requestData1  = RQ_REPORT;
    rb.requestData2  = i;
    submit(&rb);
}

static void send_leds(void)
{
    if (!active) return;
    if (led_busy) { led_pending = 1; return; }
    led_busy = 1;
    led_pending = 0;
    led_data[0] = caps ? HID_LED_CAPS_LOCK : 0;
    class_request(setup_led, HID_REQ_SET_REPORT, 0x0200, led_data, 1, RQ_LED);
}

/* ── Key translation ──────────────────────────────────────────────────── */
static void put(char c)
{
    uint32_t next = (qt + 1) % sizeof q;
    if (next == qh) return;                           /* full: drop */
    q[qt] = c;
    qt = next;
}

static void puts_q(const char *s) { while (*s) put(*s++); }

/* HID usage -> ASCII (US layout), unshifted / shifted, from 0x04. */
static const char hid_plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\x7f\t -=[]\\#;'`,./";
static const char hid_shift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\x7f\t _+{}|~:\"~<>?";

static void key(uint8_t usage, uint8_t mods)
{
    int shift = (mods & 0x22) != 0;
    int ctrlk = (mods & 0x11) != 0;

    if (usage >= 0x04 && usage < 0x04 + sizeof hid_plain - 1) {
        char c = (shift ? hid_shift : hid_plain)[usage - 0x04];
        if (usage <= 0x1D && caps)                    /* letters only */
            c = (c >= 'a') ? (char)(c - 32) : (char)(c + 32);
        if (ctrlk && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c = (char)(c & 0x1F);
        put(c);
        return;
    }
    switch (usage) {                                  /* VT100 sequences */
    case 0x39: caps = !caps; send_leds(); break;      /* Caps Lock */
    case 0x4A: puts_q("\x1b[H");  break;              /* Home */
    case 0x4C: puts_q("\x1b[3~"); break;              /* Delete */
    case 0x4D: puts_q("\x1b[F");  break;              /* End */
    case 0x4F: puts_q("\x1b[C");  break;              /* Right */
    case 0x50: puts_q("\x1b[D");  break;              /* Left */
    case 0x51: puts_q("\x1b[B");  break;              /* Down */
    case 0x52: puts_q("\x1b[A");  break;              /* Up */
    default: break;
    }
}

/* Boot report: [0] modifiers, [1] reserved, [2..7] pressed key usages. */
static void report(const uint8_t *r)
{
    if (r[2] == 0x01) return;                         /* rollover error */
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k < 0x04) continue;
        int held = 0;
        for (int j = 2; j < 8; j++)
            if (prev[j] == k) held = 1;
        if (!held) {
            key(k, r[0]);
            rep_key = k == 0x39 ? 0 : k;              /* newest key repeats */
            rep_at  = usb_ticks() + usb_ms(KBD_REPEAT_DELAY);
        }
    }
    int still = 0;
    for (int i = 2; i < 8; i++)
        if (rep_key && r[i] == rep_key) still = 1;
    if (!still) rep_key = 0;
    rep_mods = r[0];
    usb_copy(prev, r, 8);
}

/* ── CHKSERV: accept a HID boot keyboard ──────────────────────────────── */
static int chkserv(USBCDServe *s)
{
    DeviceInfo *di = s ? s->pDeviceInfo : 0;
    if (!di || active) return 0;

    const uint8_t *c = di->configurationData;
    uint32_t total = di->configLength;
    int in_kbd = 0, found_if = -1;
    uint8_t ep_addr = 0;
    for (uint32_t i = 0; i + 2 <= total && c[i] >= 2; i += c[i]) {
        const uint8_t *p = &c[i];
        if (i + p[0] > total) break;
        if (p[1] == 4 && p[0] >= 9) {                 /* interface */
            in_kbd = p[5] == 3 && p[6] == 1 && p[7] == 1;
            if (in_kbd && found_if < 0) found_if = p[2];
        } else if (p[1] == 5 && p[0] >= 7 && in_kbd && !ep_addr &&
                   (p[2] & 0x80) && (p[3] & 3) == 3) {
            ep_addr = p[2];
            mps  = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
            ival = p[6];
        }
    }
    if (found_if < 0 || !ep_addr) return 0;           /* not ours */

    ctrl  = di->ctrlID;
    addr  = di->deviceAddress;
    iface = (uint8_t)found_if;
    ep    = ep_addr & 0xF;
    di->bConfigurationValue = c[5];                   /* accept */
    di->bInterfaceNumber    = iface;
    active = 1;
    errors = 0;
    caps = led_busy = led_pending = 0;
    rep_key = 0;
    usb_zero(prev, sizeof prev);

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
        return 0;
    }
    V("[USBKBD] keyboard at address %u: interface %u, EP 0x%02x\n",
      addr, iface, ep_addr);
    return 1;
}

/* ── PRCIRQ: request completions ──────────────────────────────────────── */
static void completion(USBRb *rb)
{
    if (!active || rb->deviceAddress != addr || rb->controllerId != ctrl) return;
    switch (rb->requestData1) {
    case RQ_SETCONF:
        if (rb->status) {
            V("[USBKBD] SET_CONFIGURATION failed (status 0x%02x)\n", rb->status);
            active = 0; usb_release_device(ctrl, addr);
            break;
        }
        class_request(setup_req, HID_REQ_SET_PROTOCOL, 0, 0, 0, RQ_PROTOCOL);
        break;
    case RQ_PROTOCOL:                                 /* a stall is not fatal */
        class_request(setup_req, HID_REQ_SET_IDLE, 0, 0, 0, RQ_IDLE);
        break;
    case RQ_IDLE:
        for (uint32_t i = 0; i < KBD_REPORTS; i++) read_report(i);
        V("[USBKBD] keyboard ready -> console input\n");
        break;
    case RQ_REPORT:
        if (rb->status == 0) {
            errors = 0;
            if (rb->buffer1Length >= 8) report(reports[rb->requestData2]);
        } else if (++errors > KBD_MAX_ERRORS) {
            V("[USBKBD] too many errors, keyboard stopped\n");
            active = 0; usb_release_device(ctrl, addr);
            break;
        }
        read_report((uint32_t)rb->requestData2);
        break;
    case RQ_LED:
        led_busy = 0;
        if (rb->status) V("[USBKBD] LED SET_REPORT failed (status 0x%02x)\n", rb->status);
        if (led_pending) send_leds();
        break;
    default:
        break;
    }
}

/* ── IDC entry ────────────────────────────────────────────────────────── */
static void usbkbd_idc(RP_GENIOCTL *rp)
{
    uint16_t err = 0;
    if (rp->Category != USB_IDC_CATEGORY_CLASS) {
        err = USB_IDC_WRONGCAT;
    } else {
        switch (rp->Function) {
        case USB_IDC_FUNCTION_CHKSERV:
            chkserv(rp->ParmPacket);                  /* reject = leave 0 */
            break;
        case USB_IDC_FUNCTION_PRCIRQ:
            completion(rp->ParmPacket);
            break;
        case USB_IDC_FUNCTION_DETDEV: {
            USBDetach *d = rp->ParmPacket;
            if (d && active && d->controllerId == ctrl && d->deviceAddress == addr) {
                active = 0;
                rep_key = 0;
                V("[USBKBD] keyboard at address %u detached\n", addr);
            }
            break;
        }
        case USB_IDC_FUNCTION_IDLE:
            if (!active) V("[USBKBD] no USB keyboard attached\n");
            break;
        default:
            err = USB_IDC_WRONGFUNC;
            break;
        }
    }
    rp->Status = err ? (uint16_t)(STATUS_DONE | STERR | err) : STATUS_DONE;
}

/* ── Driver lifecycle ─────────────────────────────────────────────────── */
static void try_register(void)
{
    USBDClass c = { usbkbd_idc, 0 };
    uint8_t cat = USB_IDC_CATEGORY_CLASS;
    if (!(usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD,
                       USB_IDC_FUNCTION_REGISTER, &c, &cat) & STERR))
        registered = 1;
}

int usbkbd_devinit(int v)
{
    verbose = v;
    try_register();                    /* fails if USBD isn't loaded yet */
    return 0;
}

void usbkbd_init_complete(void)
{
    if (!registered) try_register();
    if (!registered) V("[USBKBD] USBD not loaded, driver inactive\n");
}

/* ── Console input ────────────────────────────────────────────────────── */
int usbkbd_ready(void)
{
    if (!registered) return 0;
    if (qh == qt) {
        usbd_daemon();
        if (qh == qt && active && rep_key && usb_ticks() >= rep_at) {
            key(rep_key, rep_mods);
            rep_at = usb_ticks() + usb_ms(KBD_REPEAT_RATE);
        }
    }
    return qh != qt;
}

int usbkbd_getc(void)
{
    if (!usbkbd_ready()) return -1;
    char c = q[qh];
    qh = (qh + 1) % sizeof q;
    return (unsigned char)c;
}
