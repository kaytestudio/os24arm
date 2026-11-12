/* drivers/usb/usb.h - USB driver interface (stub) */
#ifndef DRIVERS_USB_H
#define DRIVERS_USB_H

#include "types.h"   /* kernel/include: APIRET, PVOID, ULONG, NO_ERROR */

#ifndef ERROR_NOT_SUPPORTED
#define ERROR_NOT_SUPPORTED     50
#endif
#ifndef ERROR_INVALID_PARAMETER
#define ERROR_INVALID_PARAMETER 87
#endif

/*
 * Read up to `length` bytes from the USB device into `buffer`.
 * On return *bytes_read holds the number of bytes copied.
 * Stub: no host controller support yet, always ERROR_NOT_SUPPORTED.
 */
APIRET usb_reader(PVOID buffer, ULONG length, PULONG bytes_read);

#endif /* DRIVERS_USB_H */
