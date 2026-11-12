/* drivers/usb/xhci.h - xHCI (USB 3) host controller driver, QEMU virt */
#ifndef DRIVERS_USB_XHCI_H
#define DRIVERS_USB_XHCI_H

#include "types.h"

/* Result of xhci_init(), for callers that want to log or branch on it. */
typedef struct {
    uint16_t vendor_id;       /* PCI IDs of the controller */
    uint16_t device_id;
    uint32_t max_slots;       /* device slots enabled */
    uint32_t max_ports;       /* root hub ports */
    uint32_t connected;       /* ports with a device attached at init */
    uint32_t enumerated;      /* devices addressed + descriptor read */
    uint32_t keyboard;        /* 1 if a HID boot keyboard is active */
    uint16_t dev_vendor;      /* first enumerated device (0 if none) */
    uint16_t dev_product;
    uint8_t  dev_class;
} xhci_info_t;

/*
 * Find the first xHCI controller (PCI class 0C/03/30) on bus 0, assign its
 * BAR, reset and start it, then enumerate every connected root-hub port
 * (Enable Slot, Address Device, GET_DESCRIPTOR) and set up the first HID
 * boot keyboard as console input.
 *
 * Needs QEMU started with e.g. `-device qemu-xhci -device usb-kbd`.
 * Polled only (no interrupts), single controller, root-hub devices only
 * (no hubs).
 *
 * Returns 0 on success (controller running), -1 if there is no xHCI
 * controller, -2 if it failed to come up. `info` may be NULL.
 */
int xhci_init(xhci_info_t *info);

/*
 * USB keyboard input, already translated to console bytes (ASCII, CR for
 * Enter, 0x7F for Backspace, VT100 escape sequences for arrows/Home/End/
 * Delete). Both poll the controller; safe to call before xhci_init() or
 * without a keyboard (they just report nothing).
 */
int xhci_kbd_ready(void);   /* nonzero if xhci_kbd_getc() has a byte */
int xhci_kbd_getc(void);    /* next byte, or -1 if none */

#endif /* DRIVERS_USB_XHCI_H */
