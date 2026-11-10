/* ============================================================================
 * kernel/src/blkdev.c  —  Block Device Management
 * ========================================================================== */

#include "blkdev.h"
#include "kio.h"
#include "uart.h"
#include "gpt.h"

/* External functions (should be in your headers) */
/* kmemcpy, kstrcmp, kstrcpy should be in kio.h */

/* Constants */
#define MAX_BLKDEVS     16
#define SECTOR_SIZE     512

/* Block device table */
static blkdev_t *blkdevs[MAX_BLKDEVS];
static int blkdev_count = 0;

int blkdev_register(blkdev_t *dev)
{
    uart_puts("DEBUG: Entering blkdev_register\r\n");
    if (blkdev_count >= MAX_BLKDEVS) return -1;
    blkdevs[blkdev_count++] = dev;
    kprintf("[BLKDEV] Registered: %s (%u sectors)\n",
            dev->name, (unsigned)dev->num_sectors);
    uart_puts("DEBUG: blkdev_register complete\r\n");
    return 0;
}

blkdev_t *blkdev_get_by_index(int index)
{
    if (index < 0 || index >= blkdev_count) return NULL;
    return blkdevs[index];
}

blkdev_t *blkdev_get_by_name(const char *name)
{
    for (int i = 0; i < blkdev_count; i++) {
        if (kstrcmp(blkdevs[i]->name, name) == 0) {
            return blkdevs[i];
        }
    }
    return NULL;
}

void blkdev_list(void)
{
    kprintf("\n=== Block Devices ===\n");
    for (int i = 0; i < blkdev_count; i++) {
        blkdev_t *d = blkdevs[i];
        int64_t mb = ((int64_t)d->num_sectors * (int64_t)d->sector_size) / (1024 * 1024);
        kprintf("  %s: %lu MB (%u sectors x %u bytes)\n",
                d->name, (unsigned long)mb, d->num_sectors, d->sector_size);
    }
}

/* ── Ramdisk Implementation ────────────────────────────────────────────── */

typedef struct {
    uint8_t     *base;
    uint32_t    num_sectors;
} ramdisk_priv_t;

static ramdisk_priv_t ramdisk_priv;
static blkdev_t       ramdisk_dev;

static int ramdisk_read(blkdev_t *dev, int64_t lba, uint32_t count, void *buf)
{
    ramdisk_priv_t *p = (ramdisk_priv_t *)dev->driver_data;
    if (!p || !p->base) return -1;
    if (lba + count > p->num_sectors) return -1;

    kmemcpy(buf, p->base + lba * SECTOR_SIZE, count * SECTOR_SIZE);
    return 0;
}

static int ramdisk_write(blkdev_t *dev, int64_t lba, uint32_t count, const void *buf)
{
    ramdisk_priv_t *p = (ramdisk_priv_t *)dev->driver_data;
    if (!p || !p->base) return -1;
    if (lba + count > p->num_sectors) return -1;

    kmemcpy(p->base + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE);
    return 0;
}

void ramdisk_create(const char *name, void *base, size_t size)
{
    ramdisk_priv.base        = (uint8_t *)base;
    ramdisk_priv.num_sectors = size / SECTOR_SIZE;

    kstrcpy(ramdisk_dev.name, name);
    ramdisk_dev.num_sectors  = ramdisk_priv.num_sectors;
    ramdisk_dev.sector_size  = SECTOR_SIZE;
    ramdisk_dev.driver_data  = &ramdisk_priv;
    ramdisk_dev.read         = ramdisk_read;
    ramdisk_dev.write        = ramdisk_write;
    ramdisk_dev.flags        = 0;
    ramdisk_dev.parent       = NULL;
    ramdisk_dev.start_sector = 0;

    blkdev_register(&ramdisk_dev);
    kprintf("[RAMDISK] Created: %s (%u KB)\n", name, (unsigned)(size / 1024));
}

/* ── VirtIO Block Device (stub for now) ──────────────────────────────────── */

/*
 * VirtIO implementation would go here.
 * For now, this is a placeholder.
 */

typedef struct {
    uint32_t device_id;
    uint32_t num_sectors;
    void     *mmio_base;
} virtio_blk_priv_t;

static virtio_blk_priv_t virtio_priv;
static blkdev_t          virtio_dev;
static uint8_t           vio_sector_buf[SECTOR_SIZE] __attribute__((aligned(512)));

static int virtio_blk_read(blkdev_t *dev, int64_t lba, uint32_t count, void *buf)
{
    /* TODO: Implement VirtIO block read */
    /* This would use virtqueues to communicate with QEMU */
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;  /* Not implemented */
}

static int virtio_blk_write(blkdev_t *dev, int64_t lba, uint32_t count, const void *buf)
{
    /* TODO: Implement VirtIO block write */
    (void)dev;
    (void)lba;
    (void)count;
    (void)buf;
    return -1;  /* Not implemented */
}

void virtio_blk_init(void)
{
    /* TODO: Probe VirtIO MMIO region for block devices */
    /* For now, just print a message */
    kprintf("[VIRTIO-BLK] Driver initialized (stub)\n");
    kprintf("[VIRTIO-BLK] TODO: Implement VirtIO block device detection\n");

    /* Example of how to register when implemented:
     *
     * kstrcpy(virtio_dev.name, "vda");
     * virtio_dev.num_sectors  = virtio_priv.num_sectors;
     * virtio_dev.sector_size  = SECTOR_SIZE;
     * virtio_dev.driver_data  = &virtio_priv;
     * virtio_dev.read         = virtio_blk_read;
     * virtio_dev.write        = virtio_blk_write;
     * virtio_dev.flags        = 0;
     * virtio_dev.parent       = NULL;
     * virtio_dev.start_sector = 0;
     *
     * blkdev_register(&virtio_dev);
     */
}

/* ── Block Device Subsystem Init ───────────────────────────────────────── */

/* No VirtIO block driver exists yet (see virtio_blk_read/write stubs
 * above), so there's no real disk to mount a filesystem against. Register
 * a block device backed by a fixed physical RAM address instead: a real
 * disk image can be tested by injecting it there before boot, e.g.
 *   qemu-system-aarch64 ... -device loader,file=test.img,addr=0x44000000
 * With nothing loaded there, RAM reads as zero and filesystem mounts
 * correctly fail (bad superblock magic) rather than misbehaving. */
#define PRELOADED_DISK_ADDR  0x44000000UL
#define PRELOADED_DISK_SIZE  (24u * 1024u * 1024u)

void blk_init(void)
{
    uart_puts("DEBUG: Entering blk_init\r\n");

    // Arrays are already zeroed by boot.S BSS clearing
    blkdev_count = 0;

    ramdisk_create("disk0", (void *)PRELOADED_DISK_ADDR, PRELOADED_DISK_SIZE);

    /* If a GPT-partitioned image (BOOT + C:, or anything else) was loaded
     * into disk0, register its partitions as child devices. No-op if disk0
     * doesn't contain a valid GPT (e.g. a flat, unpartitioned test image),
     * leaving disk0 itself directly usable as before. */
    gpt_probe(blkdev_get_by_name("disk0"));

    uart_puts("DEBUG: blk_init complete\r\n");
}
void blkdev_init(void)
{
    kprintf("[BLKDEV] Initializing block device subsystem...\n");

    /* Initialize VirtIO block devices */
    virtio_blk_init();

    /* Create a ramdisk if needed (requires mem_alloc) */
    /* Uncomment this if you have mem_alloc() available:
     *
     * void *ramdisk_mem = mem_alloc(1024 * 1024);
     * if (ramdisk_mem) {
     *     ramdisk_create("ramdisk0", ramdisk_mem, 1024 * 1024);
     * }
     */

    /* List all registered devices */
    if (blkdev_count > 0) {
        blkdev_list();
    } else {
        kprintf("[BLKDEV] No block devices registered yet\n");
    }

    kprintf("[BLKDEV] Initialization complete (%d devices)\n", blkdev_count);
}