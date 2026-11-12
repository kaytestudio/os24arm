/* drivers/usb/usb.c - USB stack loader: CONFIG.SYS BASEDEV=/DEVICE= lines */
#include "usb.h"
#include "usbd.h"
#include "xhci.h"
#include "usbkbd.h"
#include "usb_util.h"
#include "kprintf.h"

enum { DRV_USBD, DRV_XHCD, DRV_KBD };

static const char *const drv_names[] = { "USBD.SYS", "USBXHCD.SYS", "USBKBD.SYS" };

typedef struct {
    int drv;
    int basedev;
    int verbose;
} entry_t;

#define MAX_ENTRIES 8

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Case-insensitive prefix match; returns the rest of s, or 0. */
static const char *prefix(const char *s, const char *p)
{
    while (*p) {
        if (up(*s) != *p) return 0;
        s++, p++;
    }
    return s;
}

/* "[path\]NAME.SYS /V ..." up to end of line -> driver and /V flag. */
static int parse_driver(const char *s, int *verbose)
{
    const char *name = s, *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') {
        if (*e == '\\' || *e == '/' || *e == ':') name = e + 1;
        e++;
    }
    int drv = -1;
    for (int i = 0; i < 3; i++) {
        const char *rest = prefix(name, drv_names[i]);
        if (rest == e) { drv = i; break; }
    }
    *verbose = 0;
    for (const char *p = e; *p && *p != '\n'; p++)
        if (*p == '/' && up(p[1]) == 'V') *verbose = 1;
    return drv;
}

static void run(const entry_t *en, int phase)
{
    switch (en->drv) {
    case DRV_USBD:
        if (phase == 0) usbd_basedevinit(en->verbose);
        else            usbd_init_complete();
        break;
    case DRV_XHCD:
        if (phase == 0) xhci_basedevinit(en->verbose);
        else            xhci_init_complete();
        break;
    case DRV_KBD:
        if (phase == 0) usbkbd_devinit(en->verbose);
        else            usbkbd_init_complete();
        break;
    }
}

void usb_init(const char *cfg)
{
    entry_t en[MAX_ENTRIES];
    int n = 0;

    if (!cfg) {
        en[n++] = (entry_t){ DRV_XHCD, 1, 0 };
        en[n++] = (entry_t){ DRV_USBD, 1, 0 };
        en[n++] = (entry_t){ DRV_KBD,  0, 0 };
    } else {
        for (const char *l = cfg; *l && n < MAX_ENTRIES; ) {
            while (*l == ' ' || *l == '\t') l++;
            const char *rest;
            int basedev = 0;
            if ((rest = prefix(l, "BASEDEV="))) basedev = 1;
            else rest = prefix(l, "DEVICE=");
            if (rest) {
                int v, drv = parse_driver(rest, &v);
                if (drv >= 0) en[n++] = (entry_t){ drv, basedev, v };
            }
            while (*l && *l != '\n') l++;
            if (*l) l++;
        }
        if (!n) {
            kprintf("[USB]    no USB drivers in CONFIG.SYS\n");
            return;
        }
    }

    for (int i = 0; i < n; i++) if (en[i].basedev)  run(&en[i], 0);  /* BASEDEVINIT */
    for (int i = 0; i < n; i++) if (!en[i].basedev) run(&en[i], 0);  /* DEVINIT */
    for (int i = 0; i < n; i++) run(&en[i], 1);                      /* INIT COMPLETE */

    if (usbd_loaded()) {
        /* Let class drivers' asynchronous setup (SETCONF -> ...) finish
         * so the boot log is in order. */
        u64 end = usb_ticks() + usb_ms(100);
        while (usb_ticks() < end) usbd_daemon();
        usbd_summary();
    }
}
