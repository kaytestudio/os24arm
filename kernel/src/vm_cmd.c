/**
 * @file vm_cmd.c
 * @brief VM shell command: the hypervisor's "userspace" (see hyp.h)
 *
 * Plays the part QEMU plays for KVM: sets up the VM, attaches device
 * models (here just the PL011), and loops on vcpu_run(), handling each
 * exit and pumping host console input into the guest's UART between
 * runs. Ctrl-] typed on the console stops the guest and returns to the
 * shell, the same escape as telnet.
 */

#include "hyp.h"
#include "gicv3.h"
#include "vm_pl011.h"
#include "string.h"
#include "uart.h"
#include "kprintf.h"

/* Built-in test guest (vm_guest_hello.S) */
extern const uint8_t vm_guest_hello_start[];
extern const uint8_t vm_guest_hello_end[];

#define TEST_RAM_IPA   0x40000000UL
#define TEST_RAM_SIZE  (2UL * 1024 * 1024)
#define GUEST_UART     0x09000000UL   /* same spot as on the virt board */
#define ESCAPE_CHAR    0x1D           /* Ctrl-] */

static struct vm test_vm;
static struct vm_pl011 test_uart;

static void put_hex64(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        uart_putc(hex[(v >> shift) & 0xF]);
}

static void vm_report_error(struct vm *vm)
{
    uart_puts("\r\n[VM] guest stopped: unhandled trap, ESR_EL2=");
    put_hex64(vm->vcpu.run.esr);
    kprintf(" (EC 0x%x) at guest PC ", (unsigned)(vm->vcpu.run.esr >> 26));
    put_hex64(vm->vcpu.hv.guest.pc);
    uart_puts("\r\n");
}

/* Host console -> guest UART. Returns 1 if the escape key was typed. */
static int pump_console(struct vm_pl011 *uart)
{
    while (uart_ready()) {
        char c = uart_getc();
        if (c == ESCAPE_CHAR)
            return 1;
        vm_pl011_receive(uart, (uint8_t)c);   /* full FIFO: dropped, like an overrun */
    }
    return 0;
}

struct vm_stats {
    uint32_t mmio, intr, idle;
};

static void vm_loop(struct vm *vm, struct vm_pl011 *uart)
{
    struct vm_stats st = {0};
    for (;;) {
        if (pump_console(uart)) {
            uart_puts("\r\n[VM] stopped by Ctrl-]\r\n");
            break;
        }
        vcpu_run(&vm->vcpu);
        struct vm_run *run = &vm->vcpu.run;
        switch (run->reason) {
        case VM_EXIT_MMIO:
            st.mmio++;
            if (vm_pl011_mmio(uart, run) == 0)
                continue;
            uart_puts("\r\n[VM] guest stopped: access to unmapped address ");
            put_hex64(run->mmio.phys_addr);
            uart_puts(" at guest PC ");
            put_hex64(vm->vcpu.hv.guest.pc);
            uart_puts("\r\n");
            goto out;
        case VM_EXIT_INTR:
            st.intr++;
            continue;
        case VM_EXIT_IDLE:
            st.idle++;
            continue;
        case VM_EXIT_SHUTDOWN:
            uart_puts("[VM] guest powered off via PSCI\r\n");
            goto out;
        case VM_EXIT_HALT:
            uart_puts("\r\n[VM] guest halted (WFI with no interrupt source armed)\r\n");
            goto out;
        case VM_EXIT_INTERNAL_ERROR:
            vm_report_error(vm);
            goto out;
        }
    }
out:
    kprintf("[VM] exits: %u MMIO (UART), %u host IRQ, %u WFI idle\r\n",
            st.mmio, st.intr, st.idle);
}

static void cmd_vm_test(void)
{
    struct vm *vm = &test_vm;
    size_t len = (size_t)(vm_guest_hello_end - vm_guest_hello_start);

    if (vm_create(vm, TEST_RAM_IPA, TEST_RAM_SIZE) != 0 ||
        vm_load(vm, TEST_RAM_IPA, vm_guest_hello_start, len) != 0) {
        uart_puts("[VM] could not set up the test guest\r\n");
        return;
    }
    vm_set_entry(vm, TEST_RAM_IPA);
    vm_pl011_init(&test_uart, vm, GUEST_UART, VGIC_IRQ_UART);

    uart_puts("[VM] test guest: 2MB RAM at IPA ");
    put_hex64(TEST_RAM_IPA);
    uart_puts(" (host PA ");
    put_hex64(vm->ram_pa);
    uart_puts(")\r\n");
    kprintf("[VM] devices: PL011 @ 0x%x (SPI %u), GICv2 @ 0x%x/0x%x, virtual timer (PPI %u)\r\n",
            (unsigned)GUEST_UART, VGIC_IRQ_UART - 32,
            (unsigned)VGIC_GICD_BASE, (unsigned)VGIC_GICC_BASE, VGIC_IRQ_VTIMER - 16);
    uart_puts("[VM] Ctrl-] stops the guest\r\n");

    vm_loop(vm, &test_uart);
}

void vm_print_status(void)
{
    uart_puts("Hypervisor: ");
    if (hyp_available()) {
        kprintf("ready (EL2, stage-2 translation, %u-bit PA)\r\n", (unsigned)hyp_pa_range_bits());
        kprintf("  Preemption: %s\r\n", gicv3_available()
                ? "yes (EL2 timer time slice via host GICv3)"
                : "no (no GICv3 - guests run until they trap)");
        kprintf("  Counter:    %u Hz\r\n", (unsigned)vm_counter_freq());
    } else if (hyp_boot_el != 2) {
        uart_puts("unavailable - kernel was entered at EL1.\r\n");
        uart_puts("  Start QEMU with -machine virt,virtualization=on (scripts/run-vm.sh).\r\n");
    } else {
        uart_puts("unavailable - EL2 bring-up failed (see boot log)\r\n");
    }
}

/* argv[1] arrives upper-cased (main.c) */
void cmd_vm(int argc, char **argv)
{
    uart_puts("\r\n");
    if (argc < 2 || strcmp(argv[1], "STATUS") == 0) {
        vm_print_status();
    } else if (strcmp(argv[1], "TEST") == 0) {
        if (!hyp_available()) {
            vm_print_status();
            return;
        }
        cmd_vm_test();
    } else {
        uart_puts("Usage: VM [STATUS]  - show hypervisor state\r\n");
        uart_puts("       VM TEST      - boot the built-in test guest (Ctrl-] stops it)\r\n");
    }
}
