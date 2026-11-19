/**
 * @file terminal.h
 * @brief Terminal/Console I/O Functions for Kernel Drivers
 * @defgroup terminal Terminal I/O
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include "types.h"

/* Forward declare kprintf if not already available */
#ifndef __KPRINTF_DECLARED
extern void kprintf(const char *fmt, ...);
#define __KPRINTF_DECLARED
#endif

/* Terminal output wrapper - maps to kprintf */
static inline void terminal_writestring(const char *str)
{
    kprintf("%s", str);
}

static inline void terminal_putchar(char c)
{
    kprintf("%c", c);
}

static inline void terminal_writenum(uint32_t num)
{
    kprintf("%u", num);
}

static inline void terminal_writehex(uint32_t num)
{
    kprintf("0x%x", num);
}

static inline void terminal_newline(void)
{
    kprintf("\n");
}

/* Clear screen (if supported) */
static inline void terminal_clear(void)
{
    kprintf("\033[2J\033[H");
}

#endif /* TERMINAL_H */

