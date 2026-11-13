/* Services the kernel hands an LX program's entry point (x0).
 *
 * LX programs (CMD.EXE, ...) run in the kernel's address space but link
 * their own copy of kernel/src/uart.c, which can only poll the PL011.
 * Console input that needs the kernel's drivers - the USB keyboard - comes
 * through this table instead. Fields may be NULL; programs started some
 * other way get a NULL table and fall back to the UART alone. */
#ifndef LX_SERVICES_H
#define LX_SERVICES_H

#define LX_SERVICES_VERSION 1

struct lx_services {
    unsigned int version;          /* LX_SERVICES_VERSION */
    int (*kbd_ready)(void);        /* nonzero if kbd_getc() has a byte */
    int (*kbd_getc)(void);         /* next byte, or -1 */
};

/* kernel/src/uart.c (non-kernel builds): extra console input source. */
void uart_set_input_services(const struct lx_services *svc);

#endif
