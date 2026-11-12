/* drivers/usb/xhci.h - USBXHCD: xHCI host controller driver (HCD layer) */
#ifndef DRIVERS_USB_XHCI_H
#define DRIVERS_USB_XHCI_H

/*
 * BASEDEVINIT (1Bh): find the xHCI controller (PCI class 0C/03/30) on
 * QEMU virt's PCI bus 0 and register with USBD. Returns -1 if there is
 * no controller (QEMU: -device qemu-xhci).
 */
int  xhci_basedevinit(int verbose);

/* INITIALIZATION COMPLETE (1Fh): retry registration if USBD loaded later. */
void xhci_init_complete(void);

#endif /* DRIVERS_USB_XHCI_H */
