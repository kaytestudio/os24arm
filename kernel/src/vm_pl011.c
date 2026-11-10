/**
 * @file vm_pl011.c
 * @brief Emulated ARM PL011 UART for guests (see vm_pl011.h)
 */

#include "vm_pl011.h"
#include "uart.h"

#define UARTDR      0x000
#define UARTRSR     0x004   /* write: UARTECR */
#define UARTFR      0x018
#define UARTILPR    0x020
#define UARTIBRD    0x024
#define UARTFBRD    0x028
#define UARTLCR_H   0x02C
#define UARTCR      0x030
#define UARTIFLS    0x034
#define UARTIMSC    0x038
#define UARTRIS     0x03C
#define UARTMIS     0x040
#define UARTICR     0x044
#define UARTDMACR   0x048
#define UARTPID0    0xFE0

#define FR_RXFE     (1u << 4)
#define FR_TXFF     (1u << 5)
#define FR_RXFF     (1u << 6)
#define FR_TXFE     (1u << 7)

#define INT_RX      (1u << 4)
#define INT_TX      (1u << 5)
#define INT_RT      (1u << 6)
#define INT_ALL     0x7FFu

/* PeriphID0-3 + CellID0-3: an ARM PL011 r1p5, as QEMU reports it */
static const uint8_t pl011_id[8] = { 0x11, 0x10, 0x14, 0x00, 0x0D, 0xF0, 0x05, 0xB1 };

/* RX FIFO level at which RXRIS fires (UARTIFLS.RXIFLSEL: 1/8 .. 7/8) */
static uint32_t rx_trigger(const struct vm_pl011 *u)
{
    static const uint8_t level[] = { 2, 4, 8, 12, 14 };
    uint32_t sel = (u->ifls >> 3) & 7;
    return sel < sizeof(level) ? level[sel] : 8;
}

/* Raw interrupt status, all level-modelled: transmit is instant, so the
 * TX FIFO is always below its trigger; receive timeout is "immediately"
 * (there's no line idle time to wait out). */
static uint32_t ris(const struct vm_pl011 *u)
{
    uint32_t r = INT_TX;
    if (u->rx_count >= rx_trigger(u))
        r |= INT_RX;
    if (u->rx_count > 0)
        r |= INT_RT;
    return r;
}

static void update_irq(struct vm_pl011 *u)
{
    int level = (ris(u) & u->imsc) != 0;
    if (level != u->irq_level) {
        u->irq_level = level;
        vm_irq_line(u->vm, u->intid, level);
    }
}

void vm_pl011_init(struct vm_pl011 *u, struct vm *vm, uint64_t base, uint32_t intid)
{
    *u = (struct vm_pl011){0};
    u->vm    = vm;
    u->base  = base;
    u->intid = intid;
    u->cr    = 0x300;    /* TXE | RXE */
    u->ifls  = 0x12;     /* 1/2 full both ways */
    vm_irq_line(vm, intid, 0);
}

int vm_pl011_receive(struct vm_pl011 *u, uint8_t c)
{
    if (u->rx_count == VM_PL011_FIFO)
        return -1;
    u->rx[(u->rx_head + u->rx_count) % VM_PL011_FIFO] = c;
    u->rx_count++;
    update_irq(u);
    return 0;
}

static uint32_t rx_pop(struct vm_pl011 *u)
{
    if (u->rx_count == 0)
        return 0;
    uint32_t c = u->rx[u->rx_head];
    u->rx_head = (u->rx_head + 1) % VM_PL011_FIFO;
    u->rx_count--;
    return c;
}

int vm_pl011_mmio(struct vm_pl011 *u, struct vm_run *run)
{
    if (run->mmio.phys_addr < u->base || run->mmio.phys_addr >= u->base + 0x1000)
        return -1;
    uint64_t off = (run->mmio.phys_addr - u->base) & ~3UL;
    uint32_t v = (uint32_t)run->mmio.data;

    if (run->mmio.is_write) {
        switch (off) {
        case UARTDR:    uart_putc((char)(v & 0xFF)); break;
        case UARTRSR:   break;                     /* ECR: clear errors (none kept) */
        case UARTILPR:  u->ilpr = v & 0xFF; break;
        case UARTIBRD:  u->ibrd = v & 0xFFFF; break;
        case UARTFBRD:  u->fbrd = v & 0x3F; break;
        case UARTLCR_H: u->lcr_h = v & 0xFF; break;
        case UARTCR:    u->cr = v & 0xFFFF; break;
        case UARTIFLS:  u->ifls = v & 0x3F; break;
        case UARTIMSC:  u->imsc = v & INT_ALL; break;
        case UARTICR:   break;                     /* every source is level: nothing latched */
        case UARTDMACR: u->dmacr = v & 7; break;
        }
    } else {
        uint32_t r = 0;
        switch (off) {
        case UARTDR:    r = rx_pop(u); break;
        case UARTFR:    r = FR_TXFE
                          | (u->rx_count == 0 ? FR_RXFE : 0)
                          | (u->rx_count == VM_PL011_FIFO ? FR_RXFF : 0); break;
        case UARTILPR:  r = u->ilpr; break;
        case UARTIBRD:  r = u->ibrd; break;
        case UARTFBRD:  r = u->fbrd; break;
        case UARTLCR_H: r = u->lcr_h; break;
        case UARTCR:    r = u->cr; break;
        case UARTIFLS:  r = u->ifls; break;
        case UARTIMSC:  r = u->imsc; break;
        case UARTRIS:   r = ris(u); break;
        case UARTMIS:   r = ris(u) & u->imsc; break;
        case UARTDMACR: r = u->dmacr; break;
        default:
            if (off >= UARTPID0 && off < UARTPID0 + 32)
                r = pl011_id[(off - UARTPID0) / 4];
            break;
        }
        run->mmio.data = r;
    }
    update_irq(u);
    return 0;
}
