/* ============================================================================
 * fs/blkdev.h  —  Block Device Interface
 *
 * Defines block device structure and operations
 * ========================================================================== */

#ifndef FS_BLKDEV_H
#define FS_BLKDEV_H

#include "types.h"

/* Forward declarations */
struct blkdev;
typedef struct blkdev blkdev_t;

/* Block device read/write function types */
typedef int (*blkdev_read_fn)(blkdev_t *dev, int64_t sector, uint32_t count, void *buf);
typedef int (*blkdev_write_fn)(blkdev_t *dev, int64_t sector, uint32_t count, const void *buf);

/* Block device structure */
struct blkdev {
    char name[32];                /* Device name (e.g., "vda", "vda1", "vda2") */
    
    /* Device geometry */
    int64_t start_sector;        /* First sector (for partitions) */
    int64_t num_sectors;         /* Number of sectors */
    uint32_t sector_size;         /* Sector size in bytes (usually 512) */
    
    /* Operations */
    blkdev_read_fn  read;         /* Read sectors */
    blkdev_write_fn write;        /* Write sectors */
    
    /* Device hierarchy */
    blkdev_t *parent;             /* Parent device (NULL for whole disk) */
    
    /* Driver-specific data */
    void *driver_data;            /* Pointer to driver-specific structure */
    
    /* Flags */
    uint32_t flags;               /* Device flags (read-only, removable, etc.) */
};

/* Device flags */
#define BLKDEV_READONLY   (1 << 0)
#define BLKDEV_REMOVABLE  (1 << 1)
#define BLKDEV_PARTITION  (1 << 2)
#define BLKDEV_BOOT       (1 << 3)   /* MBR "active" partition (boot flag 0x80) */

/* Block device management */

/**
 * Initialize block device subsystem
 */
void blk_init(void);

/**
 * Register a block device
 */
int blkdev_register(blkdev_t *dev);

int blkdev_unregister(blkdev_t *dev);   /* + its partitions; 0 if found */

/* Called for each device blkdev_unregister() removes (e.g. to unmount
 * filesystems on a USB stick that was pulled). One hook; NULL clears it. */
typedef void (*blkdev_remove_fn)(blkdev_t *dev);
void blkdev_set_remove_hook(blkdev_remove_fn fn);

/**
 * Get block device by name
 */
blkdev_t *blkdev_get_by_name(const char *name);

/**
 * Get the index'th registered block device (registration order), or NULL
 * past the end - for walking every device, e.g. a disk's partitions
 */
blkdev_t *blkdev_get_by_index(int index);

/**
 * List all registered block devices (to kprintf)
 */
void blkdev_list(void);

/**
 * Get device size in bytes
 */
int64_t blk_bytes(blkdev_t *dev);

/**
 * Read sectors from block device
 */
static inline int blk_read(blkdev_t *dev, int64_t sector, uint32_t count, void *buf)
{
    if (!dev || !dev->read) return -1;
    
    /* Add the partition's starting sector before reading the device. */
    if (dev->parent) {
        sector += dev->start_sector;
    }
    
    return dev->read(dev, sector, count, buf);
}

/**
 * Write sectors to block device
 */
static inline int blk_write(blkdev_t *dev, int64_t sector, uint32_t count, const void *buf)
{
    if (!dev || !dev->write) return -1;
    if (dev->flags & BLKDEV_READONLY) return -1;
    
    /* Translate partition-relative sectors to the parent device. */
    if (dev->parent) {
        sector += dev->start_sector;
    }
    
    return dev->write(dev, sector, count, buf);
}

#endif /* FS_BLKDEV_H */
