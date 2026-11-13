#include "uart.h"
/* In the kernel the USB keyboard (drivers/usb/usbkbd.c) feeds the same
 * console input. LX apps/DLLs compile this file too, without USB. */
#ifdef HAVE_USB
#include "usbkbd.h"
#else
static inline int usbkbd_ready(void) { return 0; }
static inline int usbkbd_getc(void)  { return -1; }
#endif

#define UART_BASE   0x09000000UL
#define UART_DR     (*(volatile uint32_t *)(UART_BASE + 0x000))
#define UART_FR     (*(volatile uint32_t *)(UART_BASE + 0x018))
#define UART_FR_RXFE (1u << 4)
#define UART_FR_TXFF (1u << 5)

void uart_putc(char c)
{
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

char uart_getc(void)
{
    for (;;) {
        if (!(UART_FR & UART_FR_RXFE))
            return (char)(UART_DR & 0xFF);
        int c = usbkbd_getc();
        if (c >= 0)
            return (char)c;
    }
}

int uart_ready(void)
{
    return !(UART_FR & UART_FR_RXFE) || usbkbd_ready();
}

static void uart_puthex64(unsigned long val)
{
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int nibble = (unsigned int)((val >> shift) & 0xF);
        uart_putc((char)(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10)));
    }
}

/**
 * @brief Report a synchronous exception to UART, called from boot.S
 *
 * Uses only uart_putc/uart_puts (no kprintf, no varargs) so it stays usable
 * even if the fault is inside variadic-argument handling itself.
 */
void report_sync_exception(unsigned long esr, unsigned long elr, unsigned long far,
                           unsigned long lr)
{
    uart_puts("\r\n[EXC] Synchronous exception!\r\n");
    uart_puts("[EXC] ESR_EL1: "); uart_puthex64(esr); uart_puts("\r\n");
    uart_puts("[EXC] ELR_EL1: "); uart_puthex64(elr); uart_puts("\r\n");
    uart_puts("[EXC] FAR_EL1: "); uart_puthex64(far); uart_puts("\r\n");
    uart_puts("[EXC] LR:      "); uart_puthex64(lr); uart_puts("\r\n");
}
