/**
 * @file mbr.c
 * @brief Classic MBR partition table probe, with extended partitions
 *
 * SD cards (BeagleBone Black, BeagleV-Ahead) are almost always
 * MBR-partitioned, and so are OS/2 disks (HPFS is type 07h, often a
 * logical drive inside an extended partition) - gpt.c handles neither.
 * Partitions are registered the way gpt.c does it: a child blkdev_t whose
 * start_sector blk_read/blk_write add on, sharing the parent's read/write
 * ops and driver_data.
 *
 * Naming follows OS/2 / Linux numbering: primaries are <disk>p1..p4,
 * logical drives in the extended partition's EBR chain <disk>p5, p6, ...
 *
 * A whole-disk filesystem (FAT, HPFS) also ends sector 0 with 55AA; its
 * boot code must not be taken for a partition table, so the table is
 * rejected unless every status byte is 00/80 and the entries fit the disk
 * without overlapping.
 */

#include "mbr.h"
#include "string.h"
#include "kprintf.h"

extern void *mem_alloc(size_t size);

#define MBR_TABLE_OFFSET   446
#define MBR_ENTRY_SIZE     16
#define MBR_TYPE_GPT       0xEE
#define MAX_LOGICAL        24                 /* EBR chain guard */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_extended(uint8_t type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

static int add_part(blkdev_t *disk, int num, uint8_t type, uint32_t first,
                    uint32_t count, int boot)
{
    if (first == 0 || (int64_t)first + count > disk->num_sectors) {
        kprintf("[MBR] %s: partition %d outside the disk, skipped\n", disk->name, num);
        return 0;
    }
    blkdev_t *part = (blkdev_t *)mem_alloc(sizeof(blkdev_t));
    if (!part) return -1;
    memset(part, 0, sizeof(*part));

    /* "<disk>p<N>", N = 1..99 */
    size_t n = strlen(disk->name);
    if (n > sizeof(part->name) - 4) n = sizeof(part->name) - 4;
    memcpy(part->name, disk->name, n);
    part->name[n++] = 'p';
    if (num >= 10) part->name[n++] = (char)('0' + num / 10);
    part->name[n++] = (char)('0' + num % 10);
    part->name[n]   = '\0';

    part->start_sector = first;
    part->num_sectors  = count;
    part->sector_size  = disk->sector_size;
    part->read         = disk->read;
    part->write        = disk->write;
    part->driver_data  = disk->driver_data;
    part->parent       = disk;
    part->flags        = BLKDEV_PARTITION | (disk->flags & BLKDEV_READONLY) |
                         (boot ? BLKDEV_BOOT : 0);

    if (blkdev_register(part) != 0) return -1;
    kprintf("[MBR] %s: type 0x%x, start %u, %u sectors%s\n", part->name,
            (unsigned)type, (unsigned)first, (unsigned)count, boot ? " (boot)" : "");
    return 1;
}

/* Logical drives: a chain of EBRs. In each, entry 0 is the logical
 * partition (start relative to that EBR) and entry 1 links to the next
 * EBR (start relative to the extended partition). */
static int walk_ebr(blkdev_t *disk, uint32_t ext_start, uint32_t ext_count, int *num)
{
    uint8_t sector[512] __attribute__((aligned(4)));
    uint32_t ebr = ext_start;
    int found = 0;
    for (int k = 0; k < MAX_LOGICAL; k++) {
        if (blk_read(disk, ebr, 1, sector) != 0) break;
        if (sector[510] != 0x55 || sector[511] != 0xAA) break;
        const uint8_t *e0 = sector + MBR_TABLE_OFFSET;
        const uint8_t *e1 = e0 + MBR_ENTRY_SIZE;
        uint32_t first = le32(e0 + 8), count = le32(e0 + 12);
        if (e0[4] && count && !is_extended(e0[4])) {
            uint32_t abs = ebr + first;
            if (first && abs >= ext_start && (int64_t)abs + count <= (int64_t)ext_start + ext_count) {
                int r = add_part(disk, (*num)++, e0[4], abs, count, e0[0] & 0x80);
                if (r < 0) break;
                found += r;
            } else {
                kprintf("[MBR] %s: logical drive outside its extended partition, skipped\n",
                        disk->name);
            }
        }
        uint32_t next = le32(e1 + 8);
        if (!is_extended(e1[4]) || !next || next >= ext_count) break;
        ebr = ext_start + next;
    }
    return found;
}

int mbr_probe(blkdev_t *disk)
{
    uint8_t sector[512] __attribute__((aligned(4)));

    if (!disk || disk->sector_size != 512) return 0;
    if (blk_read(disk, 0, 1, sector) != 0) return 0;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;

    /* A GPT disk's protective MBR is a type-0xEE entry: leave it to
     * gpt_probe(). Then sanity-check the table (see file comment). */
    uint32_t s[4], c[4];
    int used = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        if (e[4] == MBR_TYPE_GPT) return 0;
        if (e[0] != 0x00 && e[0] != 0x80) return 0;
        s[i] = le32(e + 8);
        c[i] = e[4] ? le32(e + 12) : 0;
        if (!c[i]) continue;
        if (!s[i] || (int64_t)s[i] + c[i] > disk->num_sectors) return 0;
        for (int j = 0; j < i; j++)
            if (c[j] && s[i] < s[j] + c[j] && s[j] < s[i] + c[i]) return 0;
        used++;
    }
    if (!used) return 0;

    int found = 0, logical = 5;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sector + MBR_TABLE_OFFSET + i * MBR_ENTRY_SIZE;
        if (!c[i]) continue;
        int r;
        if (is_extended(e[4]))
            r = walk_ebr(disk, s[i], c[i], &logical);
        else
            r = add_part(disk, i + 1, e[4], s[i], c[i], e[0] & 0x80);
        if (r < 0) break;
        found += r;
    }

    kprintf("[MBR] Found %d partition(s) on %s\n", found, disk->name);
    return found;
}
