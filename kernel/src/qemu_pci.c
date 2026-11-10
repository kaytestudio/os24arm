/**
 * @file qemu_pci.c
 * @brief Minimal PCI setup for QEMU riscv64 "virt" (QEMU_RISCV)
 *
 * QEMU's virt machine has no memory-mapped SD controller, only PCI ones
 * (-device sdhci-pci), and OpenSBI doesn't enumerate PCI or assign BARs.
 * This does the minimum by hand, through virt's ECAM window: find the SD
 * host controller on bus 0, place its 32-bit BAR0 at SD_BASE (inside
 * virt's 32-bit PCI MMIO window, 0x40000000-0x7FFFFFFF) and turn on memory
 * decoding. sdhci.c then drives it exactly like the Beagle boards'
 * memory-mapped controllers.
 */

#include "qemu_pci.h"
#include "types.h"
#include "kprintf.h"

#ifndef SD_BASE
#error "SD_BASE must be defined by the platform (see CMakeLists.txt)"
#endif

#define VIRT_PCIE_ECAM     0x30000000UL   /* bus 0 config space (1 MB/bus) */

#define PCI_VENDOR_DEVICE  0x00
#define PCI_COMMAND        0x04
#define PCI_CLASS_REV      0x08           /* class [31:24], subclass [23:16] */
#define PCI_BAR0           0x10

#define PCI_CMD_MEMORY     (1u << 1)
#define PCI_CMD_MASTER     (1u << 2)

#define PCI_CLASS_SYSTEM   0x08
#define PCI_SUBCLASS_SDHCI 0x05

static volatile uint32_t *cfg(uint32_t dev, uint32_t reg)
{
    /* ECAM: bus << 20 | device << 15 | function << 12 | register */
    return (volatile uint32_t *)(VIRT_PCIE_ECAM + ((uintptr_t)dev << 15) + reg);
}

int qemu_pci_setup_sdhci(void)
{
    for (uint32_t dev = 0; dev < 32; dev++) {
        uint32_t id = *cfg(dev, PCI_VENDOR_DEVICE);
        if (id == 0xFFFFFFFFu) continue;             /* empty slot */

        uint32_t class_rev = *cfg(dev, PCI_CLASS_REV);
        if ((class_rev >> 24) != PCI_CLASS_SYSTEM ||
            ((class_rev >> 16) & 0xFF) != PCI_SUBCLASS_SDHCI)
            continue;

        *cfg(dev, PCI_BAR0) = (uint32_t)SD_BASE;
        *cfg(dev, PCI_COMMAND) |= PCI_CMD_MEMORY | PCI_CMD_MASTER;

        kprintf("[PCI]    00:%02x.0 SD host controller %04x:%04x, BAR0 -> 0x%x\n",
                dev, id & 0xFFFF, id >> 16, (unsigned)SD_BASE);
        return 0;
    }
    return -1;
}
