/* drivers/usb/usbmsd.h - USBMSD: USB mass storage class driver (BOT/SCSI) */
#ifndef DRIVERS_USB_USBMSD_H
#define DRIVERS_USB_USBMSD_H

int  usbmsd_basedevinit(int verbose); /* BASEDEVINIT (1Bh): register with USBD */
void usbmsd_init_complete(void);      /* INITIALIZATION COMPLETE (1Fh) */

#endif /* DRIVERS_USB_USBMSD_H */
