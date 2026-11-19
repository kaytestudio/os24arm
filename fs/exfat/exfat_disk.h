#ifndef EXFAT_DISK_H
#define EXFAT_DISK_H

/* ============================================================================
 * kernel/fs/exfat/exfat_disk.h  —  exFAT on-disk structures
 *
 * Reference: Microsoft exFAT File System Specification v1.00 (August 2019)
 * All structures are __attribute__((packed)).
 * ========================================================================== */

#include "types.h"

/* ── Magic / constants ───────────────────────────────────────────────────── */

#define EXFAT_OEM_NAME         "EXFAT   "   /* 8 bytes, space-padded        */
#define EXFAT_BOOT_SIG         0xAA55u
#define EXFAT_FS_REVISION      0x0100u      /* version 1.00                 */

/* File attributes */
#define EXFAT_ATTR_READ_ONLY   0x0001u
#define EXFAT_ATTR_HIDDEN      0x0002u
#define EXFAT_ATTR_SYSTEM      0x0004u
#define EXFAT_ATTR_DIRECTORY   0x0010u
#define EXFAT_ATTR_ARCHIVE     0x0020u

/* Directory entry types */
#define EXFAT_TYPE_END         0x00u    /* End of directory                 */
#define EXFAT_TYPE_ALLOC_BMP   0x81u    /* Allocation Bitmap                */
#define EXFAT_TYPE_UPCASE      0x82u    /* Up-case Table                    */
#define EXFAT_TYPE_VOLUME_LABEL 0x83u   /* Volume Label                     */
#define EXFAT_TYPE_FILE        0x85u    /* File                             */
#define EXFAT_TYPE_STREAM_EXT  0xC0u    /* Stream Extension (critical secondary) */
#define EXFAT_TYPE_FILE_NAME   0xC1u    /* File Name (critical secondary)   */

/* InUse / valid flags */
#define EXFAT_INUSE            0x80u    /* bit 7 of type byte              */

/* GeneralSecondaryFlags */
#define EXFAT_FLAG_ALLOC_POSSIBLE 0x01u /* data may be allocated           */
#define EXFAT_FLAG_NO_FAT_CHAIN   0x02u /* contiguous; no FAT chain needed */

/* FAT cluster sentinels */
#define EXFAT_CLUSTER_FREE    0x00000000u
#define EXFAT_CLUSTER_BAD     0xFFFFFFF7u
#define EXFAT_CLUSTER_EOC     0xFFFFFFFFu
#define EXFAT_CLUSTER_MEDIA   0xFFFFFFF8u

/* ── Boot Sector (512 bytes) ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];               /* EB 76 90                        */
    char     oem_name[8];               /* "EXFAT   "                      */
    uint8_t  must_be_zero[53];
    uint64_t partition_offset;          /* LBA of partition start          */
    uint64_t volume_length;             /* volume size in sectors          */
    uint32_t fat_offset;                /* sectors from volume start       */
    uint32_t fat_length;                /* sectors in one FAT              */
    uint32_t cluster_heap_offset;       /* sectors from volume start       */
    uint32_t cluster_count;
    uint32_t first_cluster_of_root_dir;
    uint32_t volume_serial_number;
    uint16_t filesystem_revision;       /* 0x0100                          */
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;    /* log2(bytes_per_sector), e.g. 9  */
    uint8_t  sectors_per_cluster_shift; /* log2(sectors_per_cluster)       */
    uint8_t  number_of_fats;            /* 1 or 2; 1 for no TexFAT        */
    uint8_t  drive_select;              /* 0x80                            */
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
    uint8_t  boot_code[390];
    uint16_t boot_signature;            /* 0xAA55                          */
} exfat_boot_t;

_Static_assert(sizeof(exfat_boot_t) == 512, "exfat_boot_t must be 512 bytes");

/* ── Generic directory entry (32 bytes) ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t entry_type;
    uint8_t custom_defined[19];
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_entry_t;

/* ── Allocation Bitmap entry ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  entry_type;        /* 0x81                                     */
    uint8_t  bitmap_flags;      /* bit 0: 0=first bitmap, 1=second          */
    uint8_t  reserved[18];
    uint32_t first_cluster;
    uint64_t data_length;       /* bytes in bitmap = ceil(cluster_count/8)  */
} exfat_alloc_bmp_t;

/* ── Up-case Table entry ─────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  entry_type;        /* 0x82                                     */
    uint8_t  reserved1[3];
    uint32_t table_checksum;
    uint8_t  reserved2[12];
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_upcase_t;

/* ── Volume Label entry ──────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  entry_type;        /* 0x83                                     */
    uint8_t  character_count;   /* 0–11                                    */
    uint16_t volume_label[11];  /* UTF-16LE; unused chars = 0              */
    uint8_t  reserved[8];
} exfat_vol_label_t;

/* ── File entry set (3 entries: File + Stream Extension + File Name) ──────── */

/* Primary: File */
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;        /* 0x85                                     */
    uint8_t  secondary_count;   /* number of secondary entries (at least 2) */
    uint16_t set_checksum;
    uint16_t file_attributes;
    uint8_t  reserved1[2];
    uint32_t create_time;       /* DOS packed time                          */
    uint32_t last_modified_time;
    uint32_t last_accessed_time;
    uint8_t  create_10ms;
    uint8_t  last_modified_10ms;
    uint8_t  create_utc_offset;
    uint8_t  last_modified_utc_offset;
    uint8_t  last_accessed_utc_offset;
    uint8_t  reserved2[7];
} exfat_file_t;

/* Secondary: Stream Extension */
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;        /* 0xC0                                     */
    uint8_t  general_secondary_flags;
    uint8_t  reserved1;
    uint8_t  name_length;       /* characters in file name                  */
    uint16_t name_hash;
    uint8_t  reserved2[2];
    uint64_t valid_data_length;
    uint8_t  reserved3[4];
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_stream_ext_t;

/* Secondary: File Name */
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;        /* 0xC1                                     */
    uint8_t  general_secondary_flags;
    uint16_t file_name[15];     /* UTF-16LE (15 chars per entry)            */
} exfat_file_name_t;

/* ── DOS packed date/time ─────────────────────────────────────────────────── */

/* exFAT uses the same DOS-time format as FAT, but stored in 32 bits:
 * [31:16] date: bits 15-9=year-1980, 8-5=month, 4-0=day
 * [15:0]  time: bits 15-11=hour, 10-5=min, 4-0=sec/2                        */
#define EXFAT_DATETIME(y,mo,d,h,mi,s) \
    ((uint32_t)((((y)-1980)&0x7F)<<25|(((mo)&0xF)<<21)|(((d)&0x1F)<<16) | \
                (((h)&0x1F)<<11)|(((mi)&0x3F)<<5)|(((s)/2)&0x1F)))

#define EXFAT_DEFAULT_DATETIME EXFAT_DATETIME(2026,2,15,0,0,0)

/* ── Name hash ───────────────────────────────────────────────────────────── */

static inline uint16_t exfat_name_hash(const uint16_t *name, uint8_t len)
{
    uint16_t hash = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint16_t c = name[i];
        /* The up-case table currently handles ASCII lowercase letters only. */
        if (c >= 'a' && c <= 'z') c -= 32;
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (uint8_t)c);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (uint8_t)(c >> 8));
    }
    return hash;
}

/* ── Entry set checksum ──────────────────────────────────────────────────── */

static inline uint16_t exfat_entry_set_checksum(const uint8_t *entries,
                                                  uint8_t num_secondary)
{
    uint16_t csum = 0;
    uint32_t total_bytes = (uint32_t)(num_secondary + 1) * 32;
    for (uint32_t i = 0; i < total_bytes; i++) {
        if (i == 2 || i == 3) continue;  /* skip checksum field itself     */
        csum = (uint16_t)(((csum << 15) | (csum >> 1)) + entries[i]);
    }
    return csum;
}

#endif /* EXFAT_DISK_H */
