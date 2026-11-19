#include "keyboard.h"

/* PL011 UART registers on QEMU virt (base 0x09000000). */
#define UART_BASE   0x09000000UL
#define UART_DR     (*(volatile ULONG *)(UART_BASE + 0x000))
#define UART_FR     (*(volatile ULONG *)(UART_BASE + 0x018))
#define UART_IMSC   (*(volatile ULONG *)(UART_BASE + 0x038))

#define UART_FR_RXFE (1u << 4)
#define UART_FR_TXFF (1u << 5)

static void uart_putc_raw(char c)
{
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = (ULONG)(unsigned char)c;
}

static void uart_puts_raw(const char *s)
{
    while (*s) {
        if (*s == '\n') uart_putc_raw('\r');
        uart_putc_raw(*s++);
    }
}

void kbd_init(void)
{
    UART_IMSC = 0;
    uart_puts_raw("[KBD]  UART keyboard driver ready\r\n");
}

BOOL kbd_ready(void)
{
    return !(UART_FR & UART_FR_RXFE);
}

static ULONG uart_readc(void)
{
    while (!kbd_ready())
        ;
    return UART_DR & 0xFF;
}

static ULONG decode_escape(void)
{
    ULONG b = uart_readc();

    if (b == 'O') {
        ULONG c = uart_readc();
        switch (c) {
            case 'P': return KEY_F1;
            case 'Q': return KEY_F2;
            case 'R': return KEY_F3;
            case 'S': return KEY_F4;
            default:  return KEY_NONE;
        }
    }

    if (b == '[') {
        ULONG c = uart_readc();
        switch (c) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            case '1': {
                ULONG d = uart_readc();
                ULONG e = (d != '~') ? uart_readc() : 0;
                if (d >= '1' && d <= '5' && e == '~') {
                    const ULONG fmap[] = { KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5 };
                    return fmap[d - '1'];
                }
                return KEY_HOME;
            }
            case '2': { uart_readc(); return KEY_NONE; }
            case '3': { uart_readc(); return KEY_DEL;  }
            case '4': { uart_readc(); return KEY_END;  }
            case '5': { uart_readc(); return KEY_PGUP; }
            case '6': { uart_readc(); return KEY_PGDN; }
            case '7': { uart_readc(); return KEY_HOME; }
            case '8': { uart_readc(); return KEY_END;  }
            default:  return KEY_NONE;
        }
    }

    return KEY_ESC;
}

ULONG kbd_getc(void)
{
    ULONG c = uart_readc();
    if (c == 0x1B) return decode_escape();
    if (c == 0x08) c = KEY_BACKSPACE;
    return c;
}

ULONG kbd_getc_nb(void)
{
    if (!kbd_ready()) return KEY_NONE;
    return kbd_getc();
}
