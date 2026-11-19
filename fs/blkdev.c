/* ============================================================================
 * fs/blkdev.c — Block Device Management
 *
 * ========================================================================== */

#include "fs/blkdev.h"
#include "fs/virtio_blk.h"
#include "kio.h"
#include "string.h"

/* Block device table */
#define MAX_BLOCK_DEVICES 16
static blkdev_t g_devices[MAX_BLOCK_DEVICES];
static int g_device_count = 0;

void blk_init(void)
{
    kprintf("[BLK] Initializing block device subsystem...\n");
    
    kmemset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    
    kprintf("[BLK] Initializing VirtIO block driver...\n");
    virtio_blk_init();
    
/* The VirtIO driver registers devices and partitions through blk_register(). */
    
    kprintf("[BLK] Block device initialization complete (%d devices)\n", g_device_count);
    
    for (int i = 0; i < g_device_count; i++) {
        kprintf("[BLK]   - %s (%lu MB)\n", 
                g_devices[i].name,
                blk_bytes(&g_devices[i]) / (1024 * 1024));
    }
}

/* Register a block device (called by VirtIO driver) */
int blk_register(blkdev_t *dev)
{
    if (g_device_count >= MAX_BLOCK_DEVICES) {
        kprintf("[BLK] device table full!\n");
        return -1;
    }
    
    /* Copy device structure to table */
    g_devices[g_device_count] = *dev;
    g_device_count++;
    
    kprintf("[BLK] registered device: %s\n", dev->name);
    return 0;
}

/* Get block device by name */
blkdev_t *blk_get(const char *name)
{
    for (int i = 0; i < g_device_count; i++) {
        if (kstrcmp(g_devices[i].name, name) == 0) {
            return &g_devices[i];
        }
    }
    
    kprintf("[BLK] device not found: %s\n", name);
    return NULL;
}

/* Get device size in bytes */
uint64_t blk_bytes(blkdev_t *dev)
{
    if (!dev) return 0;
    return (uint64_t)dev->num_sectors * (uint64_t)dev->sector_size;
}
