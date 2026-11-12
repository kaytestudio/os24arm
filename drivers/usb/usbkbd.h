/* drivers/usb/usbkbd.h - USBKBD: HID boot keyboard class driver */
#ifndef DRIVERS_USB_USBKBD_H
#define DRIVERS_USB_USBKBD_H

int  usbkbd_devinit(int verbose);     /* DEVINIT (00h): register with USBD */
void usbkbd_init_complete(void);      /* INITIALIZATION COMPLETE (1Fh) */

/*
 * Console input from the USB keyboard, already translated to bytes
 * (ASCII, CR for Enter, 0x7F for Backspace, VT100 escape sequences for
 * arrows/Home/End/Delete). Both run the USBD daemon; safe to call when
 * nothing is loaded (they just report no input).
 */
int usbkbd_ready(void);               /* nonzero if usbkbd_getc() has a byte */
int usbkbd_getc(void);                /* next byte, or -1 */

/* Raw key events: 1 and (HID usage, pressed) if there is one, else 0.
 * usbkbd_grab(1): keys go only there, not to usbkbd_getc (and the queue
 * starts empty). Used by the Linux evdev device (/dev/input/event0). */
int  usbkbd_poll_raw(int *usage, int *down);
void usbkbd_grab(int on);

#endif /* DRIVERS_USB_USBKBD_H */
