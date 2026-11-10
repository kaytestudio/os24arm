#ifndef QEMU_PCI_H
#define QEMU_PCI_H

/* Minimal PCI support for QEMU riscv64 "virt" (QEMU_RISCV) - just enough
 * to reach an sdhci-pci SD host controller (kernel/src/qemu_pci.c). */

/**
 * @brief Find the first SD host controller (class 08h, subclass 05h) on
 * PCI bus 0, map its BAR0 at SD_BASE and enable memory decoding.
 *
 * @return 0 on success, -1 if there is no such device (QEMU started
 *         without -device sdhci-pci)
 */
int qemu_pci_setup_sdhci(void);

#endif /* QEMU_PCI_H */
