/**
 * @file ramfb.c
 * @brief QEMU "ramfb" boot framebuffer configuration (fw_cfg DMA) — see
 * kernel/include/gui/ramfb.h.
 *
 * Register layout and protocol cross-checked against QEMU's own source
 * (include/standard-headers/linux/qemu_fw_cfg.h, hw/display/ramfb.c,
 * hw/arm/virt.c's memmap) and U-Boot's ARM fw_cfg driver
 * (drivers/misc/qfw_mmio.c) for the MMIO offsets and DMA trigger sequence;
 * mistakes here can hang QEMU or leave the framebuffer blank without an error.
 *
 * fw_cfg MMIO on the QEMU "virt" machine is fixed at 0x09020000 (the
 * same memmap this project's own UART_BASE=0x09000000 comes from):
 *   +0x00  data (8/16/32/64-bit, plain PIO access - unused here)
 *   +0x08  selector (16-bit - unused here, DMA SELECT does this instead)
 *   +0x10  dma address (64-bit, big-endian; writing it triggers the op)
 *
 * All multi-byte fields on the wire (the trigger struct itself, the file
 * directory, and whatever payload is read/written) are big-endian,
 * regardless of the guest CPU's own endianness.
 */

#include "gui/ramfb.h"
#include "uart.h"
#include "kprintf.h"
#include "string.h"

/* types.h leaves uint64_t undefined under this kernel's -nostdinc build
 * (see fs/btrfs/btrfs_disk.h / kernel/src/lx_loader.c for the same
 * workaround); needed here for the DMA address and framebuffer pointer
 * fields of the wire structs below. */
#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#define FW_CFG_BASE      0x09020000UL
#define FW_CFG_DMA_REG   (FW_CFG_BASE + 0x10)

#define FW_CFG_FILE_DIR       0x19
#define FW_CFG_DMA_CTL_ERROR  0x01
#define FW_CFG_DMA_CTL_READ   0x02
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE  0x10

#define FW_CFG_MAX_FILE_PATH  56
#define FW_CFG_MAX_FILES      64

#define DRM_FOURCC_XR24  0x34325258u  /* XRGB8888: 'X','R','2','4' */

#pragma pack(push, 1)
typedef struct {
    uint32_t control; /* BE */
    uint32_t length;  /* BE */
    uint64_t address; /* BE */
} fw_cfg_dma_t;

typedef struct {
    uint32_t size;     /* BE */
    uint16_t select;   /* BE */
    uint16_t reserved;
    char     name[FW_CFG_MAX_FILE_PATH];
} fw_cfg_file_t;

typedef struct {
    uint64_t addr;   /* BE */
    uint32_t fourcc; /* BE */
    uint32_t flags;  /* BE */
    uint32_t width;  /* BE */
    uint32_t height; /* BE */
    uint32_t stride; /* BE */
} ramfb_cfg_t;
#pragma pack(pop)

/* Trigger one fw_cfg DMA operation and wait for the device to finish it.
 * `control` should already include FW_CFG_DMA_CTL_SELECT/READ/WRITE (and
 * the target selector shifted into bits [31:16]) as needed. Returns false
 * on a device-reported error or if it never completed (no fw_cfg DMA
 * interface present - shouldn't happen on the "virt" machine, but a
 * bounded retry beats an unbootable hang if this ever runs somewhere
 * that lacks it). */
static bool fw_cfg_dma_transfer(uint32_t control, uint32_t length, void *addr)
{
    volatile fw_cfg_dma_t dma;
    dma.control = __builtin_bswap32(control);
    dma.length  = __builtin_bswap32(length);
    dma.address = __builtin_bswap64((uint64_t)(uintptr_t)addr);

    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint64_t *)FW_CFG_DMA_REG = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t spins = 0;
    while ((__builtin_bswap32(dma.control) & ~(uint32_t)FW_CFG_DMA_CTL_ERROR) != 0) {
        if (++spins > 10000000UL) {
            uart_puts("[FB] fw_cfg DMA timed out\r\n");
            return false;
        }
        __asm__ volatile("nop");
    }
    return (__builtin_bswap32(dma.control) & FW_CFG_DMA_CTL_ERROR) == 0;
}

/* Look up a named fw_cfg file's selector key via the file directory
 * (selector 0x19: a BE32 count followed by that many fixed-size
 * fw_cfg_file_t entries). Returns 0 (never a valid selector for a real
 * file) if not found. */
static uint16_t fw_cfg_find_file(const char *name)
{
    static uint8_t dirbuf[4 + FW_CFG_MAX_FILES * sizeof(fw_cfg_file_t)]
        __attribute__((aligned(8)));

    uint32_t control = ((uint32_t)FW_CFG_FILE_DIR << 16) |
                        FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ;
    if (!fw_cfg_dma_transfer(control, sizeof(dirbuf), dirbuf)) {
        uart_puts("[FB] fw_cfg file directory read failed\r\n");
        return 0;
    }

    uint32_t count = __builtin_bswap32(*(uint32_t *)dirbuf);
    if (count > FW_CFG_MAX_FILES) count = FW_CFG_MAX_FILES;

    for (uint32_t i = 0; i < count; i++) {
        const fw_cfg_file_t *f =
            (const fw_cfg_file_t *)(dirbuf + 4 + i * sizeof(fw_cfg_file_t));
        if (strncmp(f->name, name, sizeof(f->name)) == 0) {
            return __builtin_bswap16(f->select);
        }
    }
    return 0;
}

static bool ramfb_on;

bool ramfb_available(void) { return ramfb_on; }

bool ramfb_init(void *fb_addr, uint32_t width, uint32_t height, uint32_t stride)
{
    uint16_t sel = fw_cfg_find_file("etc/ramfb");
    if (sel == 0) {
        uart_puts("[FB] \"etc/ramfb\" not found - boot with -device ramfb to see graphics\r\n");
        return false;
    }

    static ramfb_cfg_t cfg __attribute__((aligned(8)));
    cfg.addr   = __builtin_bswap64((uint64_t)(uintptr_t)fb_addr);
    cfg.fourcc = __builtin_bswap32(DRM_FOURCC_XR24);
    cfg.flags  = 0;
    cfg.width  = __builtin_bswap32(width);
    cfg.height = __builtin_bswap32(height);
    cfg.stride = __builtin_bswap32(stride);

    uint32_t control = ((uint32_t)sel << 16) |
                        FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE;
    if (!fw_cfg_dma_transfer(control, sizeof(cfg), &cfg)) {
        uart_puts("[FB] ramfb configuration write failed\r\n");
        return false;
    }

    kprintf("[FB] ramfb configured: %ux%u @ 0x%p (QEMU will now display this buffer)\r\n",
            width, height, fb_addr);
    ramfb_on = true;
    return true;
}
