#ifndef UART_H
#define UART_H

#include "types.h"

void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);
int  uart_ready(void);

/* Called from boot.S's exception vectors; uses only uart_putc/uart_puts. */
/* x30 at the time of the fault: who called, when ELR is a bad branch target */
void report_sync_exception(unsigned long esr, unsigned long elr, unsigned long far,
                           unsigned long lr);

#endif /* UART_H */
