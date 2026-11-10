/**
 * @file mbr.c
 * @brief Classic MBR partition table probe
 *
 * SD cards (BeagleBone Black, BeagleV-Ahead) are almost always
 * MBR-partitioned - a FAT boot partition first, often an ext4 root after
 * it - which gpt.c doesn't handle. Partitions are registered the same way
 * gpt.c does it: a child blkdev_t whose start_sector blk_read/blk_write
 * add on, sharing the parent's read/write ops and driver_data.
 */

#include "mbr.h"
#include "string.h"
#include "kprintf.h"

extern void *mem_alloc(size_t size);

#define MBR_TABLE_OFFSET   446
#define MBR_ENTRY_SIZE     16
#define MBR_TYPE_GPT       0xEE

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_extended(uint8_t type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

int mbr_probe(blkdev_t *disk)
{
    uint8_t sector[512] __attribute__((aligned(4)));

    if (!disk || disk->sector_size != 512) return 0;
    if (blk_read(disk, 0, 1, sector) != 0) return 0;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;

    /* A GPT disk's protective MBR is a single type-0xEE entry: leave it
     * to gpt_probe(). */
    for (int i = 0; i < 4; i++) {
        if (sector[MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE + 4] == MBR_TYPE_GPT)
            return 0;
    }

    int found = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        uint8_t  type  = e[4];
        uint32_t first = le32(e + 8);
        uint32_t count = le32(e + 12);

        if (type == 0 || count == 0 || is_extended(type)) continue;
        if ((int64_t)first + count > disk->num_sectors) {
            kprintf("[MBR] %s: partition %d runs past end of disk, skipped\n", disk->name, i + 1);
            continue;
        }

        blkdev_t *part = (blkdev_t *)mem_alloc(sizeof(blkdev_t));
        if (!part) break;
        memset(part, 0, sizeof(*part));

        /* "<disk>p<N>" - N is a single digit (1..4) */
        size_t n = strlen(disk->name);
        if (n > sizeof(part->name) - 3) n = sizeof(part->name) - 3;
        memcpy(part->name, disk->name, n);
        part->name[n]     = 'p';
        part->name[n + 1] = (char)('1' + i);
        part->name[n + 2] = '\0';

        part->start_sector = first;
        part->num_sectors  = count;
        part->sector_size  = disk->sector_size;
        part->read         = disk->read;
        part->write        = disk->write;
        part->driver_data  = disk->driver_data;
        part->parent       = disk;
        part->flags        = BLKDEV_PARTITION | (disk->flags & BLKDEV_READONLY);
        if (e[0] & 0x80)
            part->flags |= BLKDEV_BOOT;

        if (blkdev_register(part) != 0) break;
        kprintf("[MBR] %s: type 0x%x, start %u, %u sectors%s\n",
                part->name, (unsigned)type, (unsigned)first, (unsigned)count,
                (part->flags & BLKDEV_BOOT) ? " (boot)" : "");
        found++;
    }

    kprintf("[MBR] Found %d partition(s) on %s\n", found, disk->name);
    return found;
}
