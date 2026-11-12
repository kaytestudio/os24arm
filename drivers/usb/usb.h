/* drivers/usb/usb.h - USB stack loader (CONFIG.SYS driven) */
#ifndef DRIVERS_USB_H
#define DRIVERS_USB_H

/*
 * Load the USB stack the way OS/2 loads its drivers from CONFIG.SYS:
 *
 *   BASEDEV=USBXHCD.SYS [/V]           xHCI host controller driver
 *   BASEDEV=USBD.SYS [/V]              USB root layer (the daemon)
 *   DEVICE=[path\]USBKBD.SYS [/V]      HID keyboard class driver
 *   DEVICE=[path\]USBMOUSE.SYS [/V]    HID mouse / tablet class driver
 *
 * BASEDEV drivers get BASEDEVINIT in file order, then DEVICE drivers get
 * DEVINIT, then every driver gets INITIALIZATION COMPLETE in file order
 * (where late registration is retried and USBD enumerates). /V makes a
 * driver report what it found; without it drivers are silent.
 *
 * cfg_text is CONFIG.SYS's contents, or NULL when there is none, in
 * which case all four drivers load without /V.
 */
void usb_init(const char *cfg_text);

#endif /* DRIVERS_USB_H */
