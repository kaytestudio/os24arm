/* ============================================================================
 * fs/virtio_blk.h  —  VirtIO Block Device Driver
 * ========================================================================== */

#ifndef FS_VIRTIO_BLK_H
#define FS_VIRTIO_BLK_H

#include "types.h"
#include "fs/blkdev.h"

/* VirtIO device types */
#define VIRTIO_TYPE_BLOCK  2

/* VirtIO block request types */
#define VIRTIO_BLK_T_IN          0
#define VIRTIO_BLK_T_OUT         1
#define VIRTIO_BLK_T_FLUSH       4

/* VirtIO block request status */
#define VIRTIO_BLK_S_OK          0
#define VIRTIO_BLK_S_IOERR       1
#define VIRTIO_BLK_S_UNSUPP      2

/* Device state used by the current VirtIO block driver. */
typedef struct {
    uint32_t type;
    uint32_t status;
    uint64_t features;
    void *mmio_base;
    uint32_t queue_size;
    void *queue;
    uint64_t capacity;
} virtio_device_t;

/* VirtIO Block Driver Interface */

/**
 * Initialize VirtIO block device driver
 */
void virtio_blk_init(void);

/**
 * Read sectors from VirtIO block device
 */
int virtio_blk_read(blkdev_t *dev, uint64_t sector, uint32_t count, void *buf);

/**
 * Write sectors to VirtIO block device
 */
int virtio_blk_write(blkdev_t *dev, uint64_t sector, uint32_t count, const void *buf);

/**
 * Get device capacity in sectors
 */
uint64_t virtio_blk_capacity(virtio_device_t *vdev);

/* GPT Partition Table Support */

/* GPT partition entry */
typedef struct {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed)) gpt_partition_t;

/* GPT header */
typedef struct {
    char     signature[8];      /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t num_partitions;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} __attribute__((packed)) gpt_header_t;

/**
 * Read GPT partition table
 */
int gpt_read_table(blkdev_t *dev, gpt_partition_t *partitions, int max_parts);

#endif /* FS_VIRTIO_BLK_H */
