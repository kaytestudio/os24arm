#include "uart.h"
/* In the kernel the USB keyboard (drivers/usb/usbkbd.c) feeds the same
 * console input. LX apps/DLLs compile this file too, without USB. */
#ifdef HAVE_USB
#include "usbkbd.h"
#else
/* LX programs: the kernel passes its keyboard in an lx_services table. */
#include "lx_services.h"
static const struct lx_services *input_svc;
void uart_set_input_services(const struct lx_services *svc)
{
    input_svc = (svc && svc->version >= 1) ? svc : 0;
}
static inline int usbkbd_ready(void)
{
    return input_svc && input_svc->kbd_ready && input_svc->kbd_ready();
}
static inline int usbkbd_getc(void)
{
    return (input_svc && input_svc->kbd_getc) ? input_svc->kbd_getc() : -1;
}
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

/* While waiting for a key, stop the CPU between polls: the generic
 * timer's event stream (CNTKCTL_EL1.EVNTEN) wakes WFE about every
 * 2^(EVNTI+1) counter ticks - 16384, under a millisecond at the usual
 * 24-62.5 MHz - so polling continues without spinning the host's CPU
 * under a hypervisor. */
static void event_stream_on(void)
{
    static int on;
    if (on) return;
    unsigned long k;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(k));
    k = (k & ~(0xFUL << 4)) | (13UL << 4) | (1UL << 2);   /* EVNTI = 13, EVNTEN */
    __asm__ volatile("msr cntkctl_el1, %0\n\tisb" :: "r"(k));
    on = 1;
}

/* For polling loops elsewhere (the shell's line reader): stop the CPU
 * for about 5 ms. With a GICv3 that's WFI on the EL1 virtual
 * timer (PPI 27): interrupts stay masked here - the pending timer just
 * ends the WFI - and hypervisors sleep the vCPU until it fires (QEMU's
 * HVF only honours the virtual timer for that). Otherwise WFE on the
 * event stream. */
extern void gicv3_ppi_setup(unsigned int intid, int enable) __attribute__((weak));

void uart_idle_wait(void)
{
    if (!gicv3_ppi_setup) {
        event_stream_on();
        __asm__ volatile("wfe");
        return;
    }
    gicv3_ppi_setup(27, 1);                    /* (the hypervisor may have turned it off) */
    unsigned long f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("msr cntv_tval_el0, %0\n\tmsr cntv_ctl_el0, %1\n\tisb"
                     :: "r"(f / 200), "r"(1UL));   /* 5 ms */
    __asm__ volatile("dsb sy\n\twfi");
    __asm__ volatile("msr cntv_ctl_el0, %0\n\tisb" :: "r"(0UL));   /* line down again */
}

char uart_getc(void)
{
    event_stream_on();
    for (;;) {
        if (!(UART_FR & UART_FR_RXFE))
            return (char)(UART_DR & 0xFF);
        int c = usbkbd_getc();
        if (c >= 0)
            return (char)c;
        __asm__ volatile("wfe");
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
