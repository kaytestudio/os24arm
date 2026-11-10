/**
 * @file kprintf.c
 * @brief Kernel printf implementation
 */

#include "types.h"
#include "uart.h"

/* Variadic argument support using compiler builtins */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

static void put_spaces(int n)
{
    while (n-- > 0)
        uart_putc(' ');
}

/* Helper to print a number. A negative pad left-justifies it in -pad
 * columns (the '-' flag), always with spaces. */
static void print_num(unsigned long num, int base, int pad, char pad_char)
{
    int left = pad < 0;
    if (left)
        pad = -pad;
    if (pad > 31)
        pad = 31;

    char buf[32];
    int i = 0;

    if (num == 0) {
        buf[i++] = '0';
    } else {
        while (num > 0) {
            int digit = num % base;
            buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
            num /= base;
        }
    }

    int digits = i;

    /* Add padding */
    while (!left && i < pad) {
        buf[i++] = pad_char;
    }

    /* Print in reverse */
    while (i > 0) {
        uart_putc(buf[--i]);
    }

    if (left)
        put_spaces(pad - digits);
}

/* Simple printf implementation */
void kprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;

            /* Handle padding */
            int pad = 0;
            int left = 0;
            char pad_char = ' ';
            if (*fmt == '-') {
                left = 1;
                fmt++;
            }
            if (*fmt == '0') {
                pad_char = '0';
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                pad = pad * 10 + (*fmt - '0');
                fmt++;
            }
            if (left) {
                pad = -pad;      /* print_num's left-justify convention */
                pad_char = ' ';  /* "%-05d" pads with spaces, as in C */
            }

            switch (*fmt) {
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    if (val < 0) {
                        uart_putc('-');
                        val = -val;
                    }
                    print_num(val, 10, pad, pad_char);
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    print_num(val, 10, pad, pad_char);
                    break;
                }
                case 'x':
                case 'X': {
                    unsigned int val = va_arg(args, unsigned int);
                    print_num(val, 16, pad, pad_char);
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void*);
                    uart_puts("0x");
                    print_num((unsigned long)ptr, 16, sizeof(void*) * 2, '0');
                    break;
                }
                case 's': {
                    const char *s = va_arg(args, const char*);
                    if (!s)
                        s = "(null)";
                    int len = 0;
                    while (s[len])
                        len++;
                    int width = pad < 0 ? -pad : pad;
                    if (pad > 0)
                        put_spaces(width - len);
                    uart_puts(s);
                    if (pad < 0)
                        put_spaces(width - len);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    uart_putc(c);
                    break;
                }
                case '%': {
                    uart_putc('%');
                    break;
                }
                default: {
                    uart_putc('%');
                    uart_putc(*fmt);
                    break;
                }
            }
            fmt++;
        } else {
            uart_putc(*fmt++);
        }
    }

    va_end(args);
}