/**
 * @file ext4.h
 * @brief ext4 Filesystem Driver for OS/2 Warp ARM64
 * @defgroup ext4 ext4 Filesystem
 * @ingroup filesystems
 */

#ifndef EXT4_H
#define EXT4_H

#include "types.h"
#include "vfs.h"

/* ── Define uint64_t if not available ───────────────────────────────────── */
/* Guards on UINT64_MAX (a macro, so actually visible to the preprocessor -
 * unlike a bare `#ifndef uint64_t`, which can never detect an existing
 * typedef and would always fire) rather than assuming freestanding. Never
 * fires on the ARM64 cross build (-nostdinc, no real <stdint.h> ever
 * pulled in); on a host build with the real <stdint.h> already in scope
 * (e.g. fs/ext4/tests/host_harness.c) it correctly skips, avoiding a
 * typedef-redefinition error against the platform's real uint64_t. */
#ifndef UINT64_MAX
/* The compiler's own 64-bit type: unsigned long on AArch64/RV64, but
 * unsigned long long on 32-bit ARM (BeagleBone Black), where a plain
 * unsigned long is only 32 bits and would truncate every 64-bit ext4
 * field (block numbers, file sizes). */
typedef __UINT64_TYPE__ uint64_t;
#endif

/* ── ext4 Magic Numbers ──────────────────────────────────────────────────── */

#define EXT4_SUPER_MAGIC      0xEF53
#define EXT4_SUPER_OFFSET     1024
#define EXT4_MIN_BLOCK_SIZE   1024
#define EXT4_MAX_BLOCK_SIZE   65536
#define EXT4_EXTENT_MAGIC     0xF30A

/* ── ext4 Feature Flags ──────────────────────────────────────────────────── */

#define EXT4_FEATURE_INCOMPAT_FILETYPE  0x0002
#define EXT4_FEATURE_INCOMPAT_EXTENTS   0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT     0x0080
#define EXT4_FEATURE_INCOMPAT_FLEX_BG   0x0200
#define EXT4_FEATURE_INCOMPAT_META_BG   0x0010

/* Write support is refused (falls back to read-only) when any of these are
 * set - see ext4_write_init_allocators() in ext4_write.c for why. */
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM      0x0010  /* a.k.a. "uninit_bg" */
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400

/* ── ext4 Inode Flags ────────────────────────────────────────────────────── */

#define EXT4_EXTENTS_FL      0x00080000

/* ── ext4 File Types ─────────────────────────────────────────────────────── */

#define EXT4_FT_UNKNOWN   0
#define EXT4_FT_REG_FILE  1
#define EXT4_FT_DIR       2
#define EXT4_FT_CHRDEV    3
#define EXT4_FT_BLKDEV    4
#define EXT4_FT_FIFO      5
#define EXT4_FT_SOCK      6
#define EXT4_FT_SYMLINK   7

/* ── ext4 Constants ──────────────────────────────────────────────────────── */

#define EXT4_NAME_LEN         255
#define EXT4_ROOT_INO         2
#define EXT4_NDIR_BLOCKS     12
#define EXT4_IND_BLOCK       12
#define EXT4_DIND_BLOCK      13
#define EXT4_TIND_BLOCK      14
#define EXT4_N_BLOCKS        15

/* ── ext4 Inode Mode Bits ────────────────────────────────────────────────── */

#define EXT4_S_IFMT    0xF000u
#define EXT4_S_IFREG   0x8000u
#define EXT4_S_IFDIR   0x4000u
#define EXT4_DEFM_DIR  0x01EDu   /* 0755 */
#define EXT4_DEFM_REG  0x01A4u   /* 0644 */

/* ── ext4 Directory Entry Sizing ─────────────────────────────────────────── */

/* Fixed on-disk dirent header is 8 bytes (inode:4 + rec_len:2 + name_len:1 +
 * file_type:1); name follows, not NUL-terminated, padded to a 4-byte
 * boundary. */
#define EXT4_DIR_REC_LEN(name_len)  ((8u + (uint32_t)(name_len) + 3u) & ~3u)

/* ── ext4 Superblock ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint32_t s_mmp_block_lo;
    uint32_t s_mmp_block_hi;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint8_t  s_reserved_pad[2];
    uint32_t s_kbytes_written_lo;
    uint32_t s_kbytes_written_hi;
    uint32_t s_reserved[160];
} __attribute__((packed)) ext4_superblock_t;

/* ── ext4 Group Descriptor ───────────────────────────────────────────────── */

typedef struct {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed)) ext4_group_desc_t;

/* ── ext4 Inode ──────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed)) ext4_inode_t;

/* ── ext4 Extent Header ──────────────────────────────────────────────────── */

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed)) ext4_extent_header_t;

/* ── ext4 Extent ─────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed)) ext4_extent_t;

/* ── ext4 Directory Entry ────────────────────────────────────────────────── */

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[EXT4_NAME_LEN];
} __attribute__((packed)) ext4_dirent_t;

/* ── ext4 Mount Data ─────────────────────────────────────────────────────── */

typedef struct {
    void              *dev;
    ext4_superblock_t  sb;
    uint32_t           block_size;
    uint32_t           inode_size;
    uint32_t           inodes_per_group;
    uint32_t           blocks_per_group;
    uint32_t           group_count;
    uint32_t           desc_per_block;
    uint32_t           desc_size;      /* bytes per group descriptor (32 or 64) */
    uint8_t           *block_buffer;   /* one fs block, general I/O scratch */
    uint8_t           *extent_buffer;  /* one fs block, extent/indirect-block scratch */
    uint8_t           *bitmap_buffer;  /* one fs block, block/inode bitmap I/O scratch -
                                         * kept separate from the above two so bitmap
                                         * I/O never aliases directory/extent scratch
                                         * across nested write-engine calls */
    ext4_group_desc_t *group_desc;
    int                writable;       /* set by ext4_write_init_allocators() */
} ext4_mount_t;

/* ── ext4 File Handle ────────────────────────────────────────────────────── */

typedef struct {
    ext4_mount_t *mnt;
    uint32_t      inode_num;
    ext4_inode_t  inode;
    uint32_t      pos;
    int           flags;
} ext4_file_t;

/* ── ext4 Directory Handle ───────────────────────────────────────────────── */

typedef struct {
    ext4_mount_t *mnt;
    ext4_file_t  *dir_file;
    uint32_t      pos;
} ext4_dir_t;

/* ── ext4 Public API ─────────────────────────────────────────────────────── */

int ext4_init(const char *params);
vfs_fs_t* ext4_get_fs(void);

#endif /* EXT4_H */



