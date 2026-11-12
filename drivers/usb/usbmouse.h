/* drivers/usb/usbmouse.h - USBMOUSE: HID mouse / tablet class driver */
#ifndef DRIVERS_USB_USBMOUSE_H
#define DRIVERS_USB_USBMOUSE_H

#define USBMOUSE_ABS_MAX 32767        /* absolute coordinates: 0..32767 */

typedef struct {
    int      absolute;  /* 1: x/y are positions (tablet), 0: dx/dy (mouse) */
    int      x, y;      /* absolute, scaled to 0..USBMOUSE_ABS_MAX */
    int      dx, dy;    /* relative motion */
    int      wheel;     /* relative, positive = away from the user */
    unsigned buttons;   /* bit 0 left, 1 right, 2 middle */
} usbmouse_event_t;

int  usbmouse_devinit(int verbose);   /* DEVINIT (00h): register with USBD */
void usbmouse_init_complete(void);    /* INITIALIZATION COMPLETE (1Fh) */

/* Runs the USBD daemon; returns 1 and fills *ev with the next report, or
 * 0 if there is none. Safe to call when nothing is loaded. */
int  usbmouse_poll(usbmouse_event_t *ev);

#endif /* DRIVERS_USB_USBMOUSE_H */
