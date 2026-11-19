/* ============================================================================
 * kernel/src/gpt.c  —  GPT partition table parser
 *
 * Reads the protective MBR + GPT header + partition entry array at the
 * start of a disk and registers each partition as a child blkdev_t (via
 * the existing parent/start_sector partition-offset mechanism already
 * built into blk_read/blk_write), named after the partition's GPT name.
 * ========================================================================== */

#include "gpt.h"
#include "string.h"
#include "uart.h"

/* types.h leaves uint64_t undefined outside os2.h (see fs/ext4/ext4.h for
 * the same workaround); both use the identical "unsigned long" typedef,
 * so redeclaring it here is legal even if os2.h is also in this TU. */
#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

extern void kprintf(const char *fmt, ...);

extern void *mem_alloc(size_t size);

#define GPT_HEADER_LBA        1
#define GPT_SIGNATURE         "EFI PART"
#define GPT_MAX_PARTITIONS    16
#define GPT_MAX_TABLE_ENTRIES 4096

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36]; /* UTF-16LE, not necessarily NUL-terminated */
} gpt_entry_t;

/* CRC32 (IEEE 802.3), used to validate the GPT header */

static uint32_t crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    if (!crc32_ready) crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t crc32_update(uint32_t c, const uint8_t *data, uint32_t len)
{
    if (!crc32_ready) crc32_init();
    for (uint32_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void utf16le_to_ascii(const uint16_t *src, int max, char *dst, int dst_max)
{
    int i = 0;
    for (; i < max && i < dst_max - 1; i++) {
        uint16_t c = src[i];
        if (c == 0) break;
        dst[i] = (c < 128) ? (char)c : '?';
    }
    dst[i] = '\0';
}

static void put_uint(char *dst, unsigned val)
{
    char tmp[12];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) dst[j++] = tmp[--n];
    dst[j] = '\0';
}

void gpt_probe(blkdev_t *disk)
{
    uint8_t sector[512];

    if (!disk || disk->sector_size != sizeof(sector) || disk->num_sectors < 2) return;

    /* Check the protective MBR signature at LBA 0. */
    if (blk_read(disk, 0, 1, sector) != 0) return;
    if (sector[510] != 0x55 || sector[511] != 0xAA) return;

    /* GPT header at LBA 1. */
    if (blk_read(disk, GPT_HEADER_LBA, 1, sector) != 0) return;
    gpt_header_t hdr;
    memcpy(&hdr, sector, sizeof(hdr));
    if (memcmp(hdr.signature, GPT_SIGNATURE, 8) != 0) return;
    if (hdr.header_size < sizeof(hdr) || hdr.header_size > sizeof(sector)) return;

    uint8_t hdr_copy[512];
    memcpy(hdr_copy, sector, sizeof(hdr_copy));
    memset(hdr_copy + 16, 0, 4); /* zero header_crc32 field before recomputing */
    uint32_t calc_crc = crc32_calc(hdr_copy, hdr.header_size);
    if (calc_crc != hdr.header_crc32) return;
    if (hdr.my_lba != GPT_HEADER_LBA || hdr.alternate_lba == hdr.my_lba ||
        hdr.alternate_lba >= (uint64_t)disk->num_sectors ||
        hdr.first_usable_lba < 2 || hdr.first_usable_lba > hdr.last_usable_lba ||
        hdr.last_usable_lba >= (uint64_t)disk->num_sectors ||
        hdr.partition_entry_lba >= (uint64_t)disk->num_sectors) return;

    /* GPT images commonly declare 128 entries, even when only a few are
     * populated. Bound the table size, but don't confuse that count with
     * the smaller number of partitions this driver registers. */
    if (hdr.num_partition_entries == 0 || hdr.num_partition_entries > GPT_MAX_TABLE_ENTRIES)
        return;
    uint32_t num_entries = hdr.num_partition_entries;
    if (num_entries > GPT_MAX_PARTITIONS) num_entries = GPT_MAX_PARTITIONS;
    uint32_t entry_size = hdr.partition_entry_size;
    if (entry_size < sizeof(gpt_entry_t) || entry_size > sizeof(sector) ||
        sizeof(sector) % entry_size != 0) return;
    uint64_t array_bytes = (uint64_t)hdr.num_partition_entries * entry_size;
    uint64_t array_sectors = (array_bytes + sizeof(sector) - 1) / sizeof(sector);
    if (array_sectors == 0 || array_sectors > (uint64_t)disk->num_sectors - hdr.partition_entry_lba)
        return;

    uint32_t array_crc = 0xFFFFFFFFu;
    uint64_t remaining = array_bytes;
    for (uint64_t s = 0; s < array_sectors; s++) {
        uint8_t array_sector[512];
        if (blk_read(disk, (int64_t)(hdr.partition_entry_lba + s), 1, array_sector) != 0)
            return;
        uint32_t bytes = remaining > sizeof(array_sector) ? sizeof(array_sector) : (uint32_t)remaining;
        array_crc = crc32_update(array_crc, array_sector, bytes);
        remaining -= bytes;
    }
    if ((array_crc ^ 0xFFFFFFFFu) != hdr.partition_array_crc32) return;

    uint32_t entries_per_sector = disk->sector_size / entry_size;
    if (entries_per_sector == 0) return;

    int found = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t sector_off = i / entries_per_sector;
        uint32_t in_sector_off = (i % entries_per_sector) * entry_size;

        uint8_t entry_sector[512];
        if (blk_read(disk, (int64_t)(hdr.partition_entry_lba + sector_off), 1, entry_sector) != 0) break;

        gpt_entry_t e;
        memcpy(&e, entry_sector + in_sector_off, sizeof(e));

        int all_zero = 1;
        for (int b = 0; b < 16; b++) {
            if (e.type_guid[b] != 0) { all_zero = 0; break; }
        }
        if (all_zero) continue;
        if (e.first_lba < hdr.first_usable_lba || e.last_lba > hdr.last_usable_lba ||
            e.last_lba < e.first_lba || e.last_lba >= (uint64_t)disk->num_sectors ||
            e.first_lba > 0x7FFFFFFFFFFFFFFFUL || e.last_lba > 0x7FFFFFFFFFFFFFFFUL) continue;

        char name[32];
        utf16le_to_ascii(e.name, 36, name, sizeof(name));
        if (name[0] == '\0') {
            strcpy(name, "part");
            put_uint(name + 4, (unsigned)i);
        }

        blkdev_t *part = (blkdev_t *)mem_alloc(sizeof(blkdev_t));
        if (!part) break;
        memset(part, 0, sizeof(*part));

        strncpy(part->name, name, sizeof(part->name) - 1);
        part->start_sector = (int64_t)e.first_lba;
        part->num_sectors  = (int64_t)(e.last_lba - e.first_lba + 1);
        part->sector_size  = disk->sector_size;
        part->read         = disk->read;
        part->write        = disk->write;
        part->driver_data  = disk->driver_data;
        part->parent       = disk;
        part->flags        = BLKDEV_PARTITION;

        blkdev_register(part);
        found++;
    }

    kprintf("[GPT] Found %d partition(s) on %s\n", found, disk->name);
}
