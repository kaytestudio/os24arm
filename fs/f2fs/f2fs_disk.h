/**
 * @file f2fs_disk.h
 * @brief f2fs on-disk structures, transcribed byte-accurately from the
 *        real Linux kernel header (include/linux/f2fs_fs.h) - not
 *        reconstructed from memory. Field order/types/sizes match; only
 *        the C spelling changes (uint8/16/32/64_t instead of
 *        __u8/__le16/__le32/__le64, no byte-swap wrappers). No byte-
 *        swapping is performed anywhere in this driver, matching this
 *        project's existing little-endian-host assumption (fs/ext4 and
 *        fs/btrfs don't byte-swap either).
 *
 * This driver only ever formats and mounts images with:
 *   - F2FS_BLKSIZE == 4096 (the universal default; block size must equal
 *     host page size in real f2fs, so this is not a real restriction)
 *   - log_blocks_per_seg == 9 (512 blocks/segment, 2MB segments) - not a
 *     free choice: f2fs_sit_entry's valid_map is a fixed 64-byte bitmap,
 *     which covers exactly 512 blocks, so this is the only value that
 *     keeps one SIT entry describing exactly one segment.
 *   - cp_payload == 0 (the checkpoint's SIT/NAT version bitmaps always
 *     fit in the one fixed checkpoint block's tail space - true for any
 *     ordinary-sized filesystem)
 * See fs/f2fs/f2fs_write.c's header comment for the full list of
 * intentionally unimplemented subsystems (GC/SSR, compression,
 * encryption, xattr, ACL, quota, verity, roll-forward recovery, inline
 * data/dentry, SIT/NAT journal, multi-level hash-bucket directories).
 */

#ifndef F2FS_DISK_H
#define F2FS_DISK_H

#include "types.h"

/* kernel/include/types.h deliberately leaves uint64_t undefined (nothing
 * needed it before this driver). Guard on UINT64_MAX (a macro, visible to
 * the preprocessor - unlike a bare typedef) rather than assuming
 * freestanding, so a host build with a real <stdint.h> already in scope
 * (fs/f2fs/tests/host_harness.c) doesn't get a conflicting redefinition -
 * same fix already applied to fs/ext4/ext4.h for the same reason. */
#ifndef UINT64_MAX
typedef unsigned long uint64_t;
#endif

#define F2FS_SUPER_MAGIC        0xF2F52010u
#define F2FS_SUPER_OFFSET       1024        /* byte offset of superblock copy 1 */
#define F2FS_BLKSIZE            4096u
#define F2FS_BLKSIZE_BITS       12
#define F2FS_MAX_EXTENSION      64
#define F2FS_EXTENSION_LEN      8
#define F2FS_MAX_VOLUME_NAME    512
#define F2FS_MAX_PATH_LEN       64
#define F2FS_MAX_DEVICES        8
#define F2FS_MAX_QUOTAS         3
#define F2FS_VERSION_LEN        256
#define F2FS_MAX_STOP_REASON    32
#define F2FS_MAX_ERRORS         16

#define NULL_ADDR   ((uint32_t)0)
#define NEW_ADDR    ((uint32_t)0xFFFFFFFFu)

/* Reserved node ids: 0 unused, 1 = node_ino, 2 = meta_ino (both real-Linux
 * page-cache bookkeeping concepts this driver has no use for - it never
 * allocates or reads them), 3 = the conventional first free nid, normally
 * root_ino. This driver always reads root_ino/node_ino/meta_ino from the
 * superblock fields rather than assuming these values. */
#define F2FS_RESERVED_NODE_NUM  3

/* ── Superblock (byte offset 1024, two copies: blocks 0 and 1) ──────────── */

struct f2fs_device {
    uint8_t  path[F2FS_MAX_PATH_LEN];
    uint32_t total_segments;
} __attribute__((packed));

struct f2fs_super_block {
    uint32_t magic;
    uint16_t major_ver;
    uint16_t minor_ver;
    uint32_t log_sectorsize;
    uint32_t log_sectors_per_block;
    uint32_t log_blocksize;
    uint32_t log_blocks_per_seg;
    uint32_t segs_per_sec;
    uint32_t secs_per_zone;
    uint32_t checksum_offset;
    uint64_t block_count;
    uint32_t section_count;
    uint32_t segment_count;
    uint32_t segment_count_ckpt;
    uint32_t segment_count_sit;
    uint32_t segment_count_nat;
    uint32_t segment_count_ssa;
    uint32_t segment_count_main;
    uint32_t segment0_blkaddr;
    uint32_t cp_blkaddr;
    uint32_t sit_blkaddr;
    uint32_t nat_blkaddr;
    uint32_t ssa_blkaddr;
    uint32_t main_blkaddr;
    uint32_t root_ino;
    uint32_t node_ino;
    uint32_t meta_ino;
    uint8_t  uuid[16];
    uint16_t volume_name[F2FS_MAX_VOLUME_NAME];
    uint32_t extension_count;
    uint8_t  extension_list[F2FS_MAX_EXTENSION][F2FS_EXTENSION_LEN];
    uint32_t cp_payload;
    uint8_t  version[F2FS_VERSION_LEN];
    uint8_t  init_version[F2FS_VERSION_LEN];
    uint32_t feature;
    uint8_t  encryption_level;
    uint8_t  encrypt_pw_salt[16];
    struct f2fs_device devs[F2FS_MAX_DEVICES];
    uint32_t qf_ino[F2FS_MAX_QUOTAS];
    uint8_t  hot_ext_count;
    uint16_t s_encoding;
    uint16_t s_encoding_flags;
    uint8_t  s_stop_reason[F2FS_MAX_STOP_REASON];
    uint8_t  s_errors[F2FS_MAX_ERRORS];
    uint8_t  reserved[258];
    uint32_t crc;
} __attribute__((packed));

/* ── Checkpoint ───────────────────────────────────────────────────────────
 * Fixed-size header only (192 bytes) - the trailing sit_nat_version_bitmap
 * is variable-length and this driver never interprets its content (see
 * f2fs_disk.h's header comment: no SIT/NAT journal, so there's never a
 * stale-vs-current block to disambiguate via this bitmap); it's written
 * zeroed and never read back. */

#define F2FS_CP_PACKS           2
#define MAX_ACTIVE_LOGS         16
#define MAX_ACTIVE_NODE_LOGS    8
#define MAX_ACTIVE_DATA_LOGS    8

#define CP_UMOUNT_FLAG          0x00000001u
#define CP_ORPHAN_PRESENT_FLAG  0x00000002u
#define CP_FSCK_FLAG            0x00000010u

struct f2fs_checkpoint {
    uint64_t checkpoint_ver;
    uint64_t user_block_count;
    uint64_t valid_block_count;
    uint32_t rsvd_segment_count;
    uint32_t overprov_segment_count;
    uint32_t free_segment_count;

    uint32_t cur_node_segno[MAX_ACTIVE_NODE_LOGS];
    uint16_t cur_node_blkoff[MAX_ACTIVE_NODE_LOGS];
    uint32_t cur_data_segno[MAX_ACTIVE_DATA_LOGS];
    uint16_t cur_data_blkoff[MAX_ACTIVE_DATA_LOGS];

    uint32_t ckpt_flags;
    uint32_t cp_pack_total_block_count;
    uint32_t cp_pack_start_sum;
    uint32_t valid_node_count;
    uint32_t valid_inode_count;
    uint32_t next_free_nid;
    uint32_t sit_ver_bitmap_bytesize;
    uint32_t nat_ver_bitmap_bytesize;
    uint32_t checksum_offset;
    uint64_t elapsed_time;
    uint8_t  alloc_type[MAX_ACTIVE_LOGS];
    /* sit_nat_version_bitmap[] follows, variable length - see above */
} __attribute__((packed));

#define CP_CHKSUM_OFFSET      (F2FS_BLKSIZE - sizeof(uint32_t))
#define CP_MIN_CHKSUM_OFFSET  (sizeof(struct f2fs_checkpoint))

/* ── Node structure (inode / direct / indirect, all one block) ──────────── */

struct f2fs_extent {
    uint32_t fofs;
    uint32_t blk;
    uint32_t len;
} __attribute__((packed));

#define F2FS_NAME_LEN               255
#define OFFSET_OF_END_OF_I_EXT      360
#define SIZE_OF_I_NID               20
#define DEF_NIDS_PER_INODE          5

struct node_footer {
    uint32_t nid;
    uint32_t ino;
    uint32_t flag;
    uint64_t cp_ver;
    uint32_t next_blkaddr;
} __attribute__((packed));

#define DEF_ADDRS_PER_INODE  ((F2FS_BLKSIZE - OFFSET_OF_END_OF_I_EXT - SIZE_OF_I_NID - sizeof(struct node_footer)) / sizeof(uint32_t))
#define DEF_ADDRS_PER_BLOCK  ((F2FS_BLKSIZE - sizeof(struct node_footer)) / sizeof(uint32_t))
#define NIDS_PER_BLOCK       ((F2FS_BLKSIZE - sizeof(struct node_footer)) / sizeof(uint32_t))

/* Node-tree offsets within an inode's i_nid[5] - see get_node_path() in
 * the real driver's node.c. This driver only ever populates
 * NODE_DIR1/DIR2 (depth-1 direct node blocks); NODE_IND1/IND2/NODE_DIND
 * (depth-2/3) are a documented non-goal (fs/f2fs/f2fs_write.c). */
#define NODE_DIR1_BLOCK  (DEF_ADDRS_PER_INODE + 1)
#define NODE_DIR2_BLOCK  (DEF_ADDRS_PER_INODE + 2)
#define NODE_IND1_BLOCK  (DEF_ADDRS_PER_INODE + 3)
#define NODE_IND2_BLOCK  (DEF_ADDRS_PER_INODE + 4)
#define NODE_DIND_BLOCK  (DEF_ADDRS_PER_INODE + 5)

#define F2FS_INLINE_XATTR    0x01
#define F2FS_INLINE_DATA     0x02
#define F2FS_INLINE_DENTRY   0x04
#define F2FS_DATA_EXIST      0x08
#define F2FS_EXTRA_ATTR      0x20

struct f2fs_inode {
    uint16_t i_mode;
    uint8_t  i_advise;
    uint8_t  i_inline;
    uint32_t i_uid;
    uint32_t i_gid;
    uint32_t i_links;
    uint64_t i_size;
    uint64_t i_blocks;
    uint64_t i_atime;
    uint64_t i_ctime;
    uint64_t i_mtime;
    uint32_t i_atime_nsec;
    uint32_t i_ctime_nsec;
    uint32_t i_mtime_nsec;
    uint32_t i_generation;
    uint32_t i_current_depth;   /* union with i_gc_failures in real f2fs; only depth used here */
    uint32_t i_xattr_nid;
    uint32_t i_flags;
    uint32_t i_pino;
    uint32_t i_namelen;
    uint8_t  i_name[F2FS_NAME_LEN];
    uint8_t  i_dir_level;
    struct f2fs_extent i_ext;
    uint32_t i_addr[DEF_ADDRS_PER_INODE];  /* union with extra_isize/compress fields in real f2fs; only i_addr used here */
    uint32_t i_nid[DEF_NIDS_PER_INODE];
} __attribute__((packed));

struct direct_node {
    uint32_t addr[DEF_ADDRS_PER_BLOCK];
} __attribute__((packed));

struct indirect_node {
    uint32_t nid[NIDS_PER_BLOCK];
} __attribute__((packed));

struct f2fs_node {
    union {
        struct f2fs_inode i;
        struct direct_node dn;
        struct indirect_node in;
    };
    struct node_footer footer;
} __attribute__((packed));

/* ── NAT (Node Address Table) ────────────────────────────────────────────── */

struct f2fs_nat_entry {
    uint8_t  version;
    uint32_t ino;
    uint32_t block_addr;
} __attribute__((packed));

#define NAT_ENTRY_PER_BLOCK (F2FS_BLKSIZE / sizeof(struct f2fs_nat_entry))

struct f2fs_nat_block {
    struct f2fs_nat_entry entries[NAT_ENTRY_PER_BLOCK];
} __attribute__((packed));

/* ── SIT (Segment Information Table) ─────────────────────────────────────── */

#define SIT_VBLOCK_MAP_SIZE 64  /* fixed - covers exactly 512 blocks/segment */

struct f2fs_sit_entry {
    uint16_t vblocks;                        /* [15:10] type, [9:0] valid count */
    uint8_t  valid_map[SIT_VBLOCK_MAP_SIZE];
    uint64_t mtime;
} __attribute__((packed));

#define SIT_ENTRY_PER_BLOCK (F2FS_BLKSIZE / sizeof(struct f2fs_sit_entry))

struct f2fs_sit_block {
    struct f2fs_sit_entry entries[SIT_ENTRY_PER_BLOCK];
} __attribute__((packed));

/* ── Directory entries ───────────────────────────────────────────────────── */

#define F2FS_SLOT_LEN            8
#define SIZE_OF_DIR_ENTRY        11
#define NR_DENTRY_IN_BLOCK       ((8u * F2FS_BLKSIZE) / ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * 8u + 1u))
#define SIZE_OF_DENTRY_BITMAP    ((NR_DENTRY_IN_BLOCK + 7u) / 8u)
#define SIZE_OF_RESERVED         (F2FS_BLKSIZE - ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * NR_DENTRY_IN_BLOCK + SIZE_OF_DENTRY_BITMAP))

typedef uint32_t f2fs_hash_t;

struct f2fs_dir_entry {
    uint32_t hash_code;
    uint32_t ino;
    uint16_t name_len;
    uint8_t  file_type;
} __attribute__((packed));

struct f2fs_dentry_block {
    uint8_t dentry_bitmap[SIZE_OF_DENTRY_BITMAP];
    uint8_t reserved[SIZE_OF_RESERVED];
    struct f2fs_dir_entry dentry[NR_DENTRY_IN_BLOCK];
    uint8_t filename[NR_DENTRY_IN_BLOCK][F2FS_SLOT_LEN];
} __attribute__((packed));

/* File types (f2fs_dir_entry.file_type / f2fs_inode.i_mode high bits) */
#define F2FS_FT_UNKNOWN   0
#define F2FS_FT_REG_FILE  1
#define F2FS_FT_DIR       2

#define F2FS_S_IFMT   0xF000u
#define F2FS_S_IFREG  0x8000u
#define F2FS_S_IFDIR  0x4000u
#define F2FS_DEF_DIR  0x41EDu  /* S_IFDIR | 0755 */
#define F2FS_DEF_REG  0x81A4u  /* S_IFREG | 0644 */

#endif /* F2FS_DISK_H */
