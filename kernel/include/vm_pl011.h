/**
 * @file vm_pl011.h
 * @brief Emulated ARM PL011 UART for guests (kernel/src/vm_pl011.c)
 *
 * A device model in the KVM sense: it sits outside vcpu_run(), handles
 * the VM_EXIT_MMIO exits for its 4KB window, and raises its interrupt
 * through vm_irq_line(). Transmit goes straight to the host console;
 * receive is a 16-entry FIFO the VM loop fills from host console input.
 *
 * Register-level: DR, RSR/ECR, FR, ILPR, IBRD/FBRD, LCR_H, CR, IFLS,
 * IMSC/RIS/MIS/ICR, DMACR and the PeriphID/CellID block (what Linux's
 * amba-pl011 probes). Not modelled: baud rate timing, break/framing
 * errors, DMA, modem lines, loopback; CR's enable bits are stored but -
 * as in QEMU's own PL011 - don't gate transfers.
 */

#ifndef VM_PL011_H
#define VM_PL011_H

#include "hyp.h"

#define VM_PL011_FIFO 16

struct vm_pl011 {
    struct vm *vm;
    uint64_t   base;
    uint32_t   intid;
    uint32_t   cr, lcr_h, ibrd, fbrd, ilpr, ifls, imsc, dmacr;
    uint8_t    rx[VM_PL011_FIFO];
    uint32_t   rx_head, rx_count;
    int        irq_level;
};

void vm_pl011_init(struct vm_pl011 *u, struct vm *vm, uint64_t base, uint32_t intid);

/* 0 if the exit was in this UART's window (and has been handled), -1 if not. */
int  vm_pl011_mmio(struct vm_pl011 *u, struct vm_run *run);

/* Queues a received byte; -1 if the FIFO is full (the byte is dropped). */
int  vm_pl011_receive(struct vm_pl011 *u, uint8_t c);

#endif /* VM_PL011_H */
