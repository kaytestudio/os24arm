/**
 * @file ns16550.c
 * @brief 16550-compatible UART driver (BeagleBone Black, BeagleV-Ahead)
 *
 * Implements the same uart.h API as uart.c (PL011, QEMU virt) for boards
 * whose console UART is a 16550 clone:
 *
 *   - BeagleBone Black: TI AM335x UART0 @ 0x44E09000
 *   - BeagleV-Ahead:    T-Head TH1520 UART0 (Synopsys DW APB) @ 0xFFE7014000
 *
 * Both space their 8-bit registers 4 bytes apart (UART_REG_SHIFT=2) and
 * take 32-bit accesses. UART_BASE / UART_REG_SHIFT come from the
 * platform's PLATFORM_DEFINES in the top-level CMakeLists.txt.
 *
 * U-Boot has already programmed the divisor for 115200 8N1 from the
 * board's UART clock (48 MHz on AM335x, 100 MHz on TH1520) before handing
 * over, so this driver deliberately leaves the line settings alone.
 */

#include "ns16550.h"

#ifndef UART_BASE
#error "UART_BASE must be defined by the platform (see CMakeLists.txt)"
#endif
#ifndef UART_REG_SHIFT
#define UART_REG_SHIFT 2
#endif

/* Registers packed 1 byte apart can only take byte accesses. */
#if UART_REG_SHIFT == 0
typedef uint8_t  uart_reg_t;
#else
typedef uint32_t uart_reg_t;
#endif

#define UART_REG(n)  (*(volatile uart_reg_t *)((uintptr_t)UART_BASE + ((uintptr_t)(n) << UART_REG_SHIFT)))
#define UART_RBR     UART_REG(0)   /* receive buffer (read)      */
#define UART_THR     UART_REG(0)   /* transmit holding (write)   */
#define UART_IER     UART_REG(1)   /* interrupt enable           */
#define UART_LSR     UART_REG(5)   /* line status                */

#define UART_LSR_DR   (1u << 0)    /* receive data ready         */
#define UART_LSR_THRE (1u << 5)    /* transmit holding empty     */

void uart_init(void)
{
    /* Polled I/O only - make sure U-Boot left no UART interrupts armed. */
    UART_IER = 0;
}

void uart_putc(char c)
{
    while (!(UART_LSR & UART_LSR_THRE))
        ;
    UART_THR = (uart_reg_t)(unsigned char)c;
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
    while (!(UART_LSR & UART_LSR_DR))
        ;
    return (char)(UART_RBR & 0xFF);
}

int uart_ready(void)
{
    return (UART_LSR & UART_LSR_DR) != 0;
}

void uart_puthex(unsigned long val)
{
    uart_puts("0x");
    for (int shift = (int)(sizeof(val) * 8) - 4; shift >= 0; shift -= 4) {
        unsigned int nibble = (unsigned int)((val >> shift) & 0xF);
        uart_putc((char)(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10)));
    }
}

/**
 * @brief Report a fatal exception to UART, called from the boot code
 *
 * Same contract as uart.c's AArch64 version, with the three arguments
 * being whatever that architecture's trap entry captured:
 *   - ARMv7 (bbb_boot.S):     vector number, faulting PC, DFAR/IFAR
 *   - RV64  (beaglev_boot.S): scause, sepc, stval
 */
void report_sync_exception(unsigned long cause, unsigned long pc, unsigned long addr)
{
#if defined(__riscv)
    static const char *const labels[3] = { "scause", "sepc  ", "stval " };
#else
    static const char *const labels[3] = { "vector", "pc    ", "far   " };
#endif
    uart_puts("\r\n[EXC] Unhandled exception!\r\n");
    uart_puts("[EXC] "); uart_puts(labels[0]); uart_puts(": "); uart_puthex(cause); uart_puts("\r\n");
    uart_puts("[EXC] "); uart_puts(labels[1]); uart_puts(": "); uart_puthex(pc);    uart_puts("\r\n");
    uart_puts("[EXC] "); uart_puts(labels[2]); uart_puts(": "); uart_puthex(addr);  uart_puts("\r\n");
}
