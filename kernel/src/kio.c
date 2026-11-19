/**
* @file kio.c
 * @brief Kernel I/O functions implementation
 */

#include "kio.h"
#include "uart.h"
#include "string.h"

/**
 * @brief Convert string to integer (atoi)
 */
int32_t katoi(const char *s)
{
    int32_t result = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }

    return sign * result;
}

/**
 * @brief Read a line from UART (gets)
 */
void kgets(char *buf, size_t len)
{
    size_t pos = 0;

    while (pos < len - 1) {
        char c = uart_getc();

        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                uart_putc('\b');
                uart_putc(' ');
                uart_putc('\b');
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            buf[pos] = '\0';
            uart_putc('\r');
            uart_putc('\n');
            return;
        }

        if (c >= 32 && c < 127) {
            buf[pos++] = c;
            uart_putc(c);
        }
    }

    buf[pos] = '\0';
}
