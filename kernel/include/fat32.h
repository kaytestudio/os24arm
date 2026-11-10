#ifndef FAT32_H
#define FAT32_H

#include "types.h"
#include "blkdev.h"
#include "vfs.h"

/* ─────────────────────────────────────────────────────────────────────────────
   FAT32 on-disk structures  (all fields little-endian)
   ───────────────────────────────────────────────────────────────────────────── */

#define FAT32_SIGNATURE     0xAA55
#define FAT32_EOC           0x0FFFFFF8U
#define FAT32_FREE          0x00000000U
#define FAT32_BAD           0x0FFFFFF7U
#define FAT32_ATTR_RDONLY   0x01
#define FAT32_ATTR_HIDDEN   0x02
#define FAT32_ATTR_SYSTEM   0x04
#define FAT32_ATTR_VOLID    0x08
#define FAT32_ATTR_DIR      0x10
#define FAT32_ATTR_ARCHIVE  0x20
#define FAT32_ATTR_LFN      0x0F  /* all four low bits set */

#define FAT32_MAX_OPEN_FILES 16
#define FAT32_MAX_OPEN_DIRS   8

/* Constants */
#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif

/* Packed BPB (BIOS Parameter Block) + FAT32 extended BPB */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;    /* 0 if > 65535 */
    uint8_t  media;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} fat32_bpb_t;

/* 8.3 Directory entry */
typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t acc_date;
    uint16_t cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;
    uint32_t file_size;
} fat32_dirent_t;

/* LFN entry */
typedef struct __attribute__((packed)) {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;       /* 0x0F */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t cluster;    /* always 0 */
    uint16_t name3[2];
} fat32_lfn_t;

/* Mount private data */
typedef struct {
    blkdev_t *dev;
    uint32_t  fat_bits;          /* 16 or 32 (FAT12 is rejected)             */
    uint32_t  fat_start_lba;     /* LBA of FAT table 0       */
    uint32_t  data_start_lba;    /* LBA of cluster 2         */
    uint32_t  root_cluster;      /* FAT32: root dir's first cluster; FAT16: 0,
                                  * meaning the fixed root region below       */
    uint32_t  root_dir_lba;      /* FAT16 only: fixed root directory region   */
    uint32_t  root_entry_count;  /* FAT16 only: entries in that region        */
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  total_clusters;
    uint32_t  fat_size_sectors;
    /* scratch buffer (one sector) */
    uint8_t   sector_buf[SECTOR_SIZE];
    uint32_t  sector_buf_lba;    /* which sector is cached   */
} fat32_mount_t;

/* Open file private data */
typedef struct {
    fat32_mount_t *mnt;
    uint32_t  first_cluster;
    uint32_t  cur_cluster;
    uint32_t  file_size;
    uint32_t  pos;             /* byte offset from start    */
    uint32_t  cluster_seq;    /* which cluster# in chain    */
    /* directory location for write-back */
    uint32_t  dir_cluster;
    uint32_t  dir_entry_idx;
    int       flags;
} fat32_file_t;

/* Open dir private data */
typedef struct {
    fat32_mount_t *mnt;
    uint32_t  cluster;
    uint32_t  entry_idx;       /* next entry index to read  */
} fat32_dir_t;

/* Mount a FAT16 or FAT32 volume (type chosen by cluster count) */
int fat32_mount_init(fat32_mount_t *m, blkdev_t *dev);

/* VFS driver - vfs_mount() it with a fat32_mount_t whose ->dev is set. */
extern vfs_fs_t vfs_fat32_fs;

/* Mount the GPT "BOOT" partition (FAT16/FAT32) at "/BOOT", if present. */
int fat32_init(const char *params);

#endif /* FAT32_H */
