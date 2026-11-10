#ifndef NS16550_H
#define NS16550_H

/* 16550 UART console (kernel/src/ns16550.c) - the uart.h API plus the
 * extras the BeagleBone Black / BeagleV-Ahead kernel uses. Kept out of
 * uart.h because main.c / rpi_main.c define their own static versions. */

#include "uart.h"

void uart_init(void);
void uart_puthex(unsigned long val);

#endif /* NS16550_H */
