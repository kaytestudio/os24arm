#ifndef BTRFS_DISK_H
#define BTRFS_DISK_H

/* ============================================================================
 * fs/btrfs/btrfs_disk.h  —  btrfs on-disk structures (subset needed for a
 * read-only driver: single device, no compression, no RAID, uncompressed
 * regular/inline extents only).
 *
 * btrfs stores almost everything as items in copy-on-write B-trees, keyed
 * by (objectid, type, offset) triples. Unlike ext4's flat block/inode
 * tables, addresses inside these trees are *logical* and must be resolved
 * to a physical (device, offset) through the chunk tree - which itself is
 * a B-tree, bootstrapped via a small embedded "system chunk array" in the
 * superblock so the chunk tree's own root can be located before any chunk
 * mapping exists yet.
 * ========================================================================== */

#include "types.h"

/* types.h leaves uint64_t undefined (see fs/ext4/ext4.h for the same
 * workaround) since it's only ever used via this typedef, not #define'd. */
#ifndef uint64_t
typedef unsigned long uint64_t;
#endif

#define BTRFS_MAGIC_STR         "_BHRfS_M"
#define BTRFS_SUPER_OFFSET      0x10000ULL   /* 64 KiB - primary superblock */
#define BTRFS_CSUM_SIZE         32
#define BTRFS_FSID_SIZE         16
#define BTRFS_UUID_SIZE         16
#define BTRFS_LABEL_SIZE        256
#define BTRFS_SYSTEM_CHUNK_ARRAY_SIZE 2048

/* Well-known object IDs */
#define BTRFS_ROOT_TREE_OBJECTID     1ULL
#define BTRFS_EXTENT_TREE_OBJECTID   2ULL
#define BTRFS_CHUNK_TREE_OBJECTID    3ULL
#define BTRFS_DEV_TREE_OBJECTID      4ULL
#define BTRFS_FS_TREE_OBJECTID       5ULL
#define BTRFS_FIRST_FREE_OBJECTID    256ULL

/* Item types (the ones this driver needs) */
#define BTRFS_INODE_ITEM_KEY    1
#define BTRFS_INODE_REF_KEY     12
#define BTRFS_DIR_ITEM_KEY      84
#define BTRFS_DIR_INDEX_KEY     96
#define BTRFS_EXTENT_DATA_KEY   108
#define BTRFS_ROOT_ITEM_KEY     132
#define BTRFS_CHUNK_ITEM_KEY    228

/* Chunk / block-group flags */
#define BTRFS_BLOCK_GROUP_DATA      (1ULL << 0)
#define BTRFS_BLOCK_GROUP_SYSTEM    (1ULL << 1)
#define BTRFS_BLOCK_GROUP_METADATA  (1ULL << 2)

/* Inode mode bits (POSIX, same as everywhere else) */
#define BTRFS_S_IFMT   0xF000u
#define BTRFS_S_IFDIR  0x4000u
#define BTRFS_S_IFREG  0x8000u

/* Directory entry "file type" byte (btrfs_dir_item.type) */
#define BTRFS_FT_UNKNOWN  0
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR      2

/* Extent data types (btrfs_file_extent_item.type) */
#define BTRFS_FILE_EXTENT_INLINE 0
#define BTRFS_FILE_EXTENT_REG    1
#define BTRFS_FILE_EXTENT_PREALLOC 2

/* Extent compression (only "none" is supported by this driver) */
#define BTRFS_COMPRESS_NONE 0

/* B-tree key: sorts items as (objectid, type, offset) */
typedef struct __attribute__((packed)) {
    uint64_t objectid;
    uint8_t  type;
    uint64_t offset;
} btrfs_key_t;

/* Common header for every tree node (internal or leaf) */
typedef struct __attribute__((packed)) {
    uint8_t  csum[BTRFS_CSUM_SIZE];
    uint8_t  fsid[BTRFS_FSID_SIZE];
    uint64_t bytenr;       /* logical address of this node itself */
    uint64_t flags;        /* Unused here; level is read from the explicit field below. */
    uint8_t  chunk_tree_uuid[BTRFS_UUID_SIZE];
    uint64_t generation;
    uint64_t owner;        /* tree objectid this node belongs to */
    uint32_t nritems;      /* number of items (leaf) or key pointers (internal) */
    uint8_t  level;        /* 0 = leaf, >0 = internal node */
} btrfs_header_t;

/* Leaf item: (key, data offset/size within the leaf) */
typedef struct __attribute__((packed)) {
    btrfs_key_t key;
    uint32_t offset;  /* byte offset of the item's data, from the end of the
                          item array (i.e. from data_start = node + nodesize) */
    uint32_t size;
} btrfs_item_t;

/* Internal node key pointer */
typedef struct __attribute__((packed)) {
    btrfs_key_t key;
    uint64_t blockptr;  /* logical address of child node */
    uint64_t generation;
} btrfs_key_ptr_t;

/* Chunk mapping (logical block group -> physical device extent) */
typedef struct __attribute__((packed)) {
    uint64_t devid;
    uint64_t offset;   /* physical byte offset on the device */
    uint8_t  dev_uuid[BTRFS_UUID_SIZE];
} btrfs_stripe_t;

typedef struct __attribute__((packed)) {
    uint64_t length;          /* size of this chunk, in bytes */
    uint64_t owner;
    uint64_t stripe_len;
    uint64_t type;            /* BTRFS_BLOCK_GROUP_* flags */
    uint32_t io_align;
    uint32_t io_width;
    uint32_t sector_size;
    uint16_t num_stripes;
    uint16_t sub_stripes;
    btrfs_stripe_t stripes[1]; /* variable length: num_stripes entries */
} btrfs_chunk_t;

/* Device item (embedded in the superblock) */
typedef struct __attribute__((packed)) {
    uint64_t devid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint32_t io_align;
    uint32_t io_width;
    uint32_t sector_size;
    uint64_t type;
    uint64_t generation;
    uint64_t start_offset;
    uint32_t dev_group;
    uint8_t  seek_speed;
    uint8_t  bandwidth;
    uint8_t  uuid[BTRFS_UUID_SIZE];
    uint8_t  fsid[BTRFS_FSID_SIZE];
} btrfs_dev_item_t;

/* Superblock (4096 bytes at BTRFS_SUPER_OFFSET) */
typedef struct __attribute__((packed)) {
    uint8_t  csum[BTRFS_CSUM_SIZE];
    uint8_t  fsid[BTRFS_FSID_SIZE];
    uint64_t bytenr;
    uint64_t flags;
    uint8_t  magic[8];
    uint64_t generation;
    uint64_t root;                 /* logical addr of root tree root node */
    uint64_t chunk_root;           /* logical addr of chunk tree root node */
    uint64_t log_root;
    uint64_t reserved0;            /* historically log_root_transid */
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t root_dir_objectid;
    uint64_t num_devices;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint32_t reserved1;            /* historically leafsize, == nodesize */
    uint32_t stripesize;
    uint32_t sys_chunk_array_size;
    uint64_t chunk_root_generation;
    uint64_t compat_flags;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint16_t csum_type;
    uint8_t  root_level;
    uint8_t  chunk_root_level;
    uint8_t  log_root_level;
    btrfs_dev_item_t dev_item;
    char     label[BTRFS_LABEL_SIZE];
    uint64_t cache_generation;
    uint64_t uuid_tree_generation;
    uint8_t  metadata_uuid[BTRFS_UUID_SIZE];
    uint8_t  reserved2[224];
    uint8_t  sys_chunk_array[BTRFS_SYSTEM_CHUNK_ARRAY_SIZE];
    uint8_t  reserved3[1237];       /* root backups + padding, unused here */
} btrfs_super_t;

/* Inode item (subset of fields this driver actually uses) */
typedef struct __attribute__((packed)) {
    uint64_t generation;
    uint64_t transid;
    uint64_t size;
    uint64_t nbytes;
    uint64_t block_group;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t rdev;
    uint64_t flags;
    uint64_t sequence;
    uint8_t  reserved[32];
    uint8_t  atime[12];   /* btrfs_timespec: sec(8) + nsec(4), unused by this driver */
    uint8_t  ctime[12];
    uint8_t  mtime[12];
    uint8_t  otime[12];
    /* Struct size (160 bytes) must match the real on-disk format exactly:
     * it's embedded inside btrfs_root_item_t, so getting this size wrong
     * shifts every field that follows it. */
} btrfs_inode_item_t;

/* Directory entry item (DIR_ITEM / DIR_INDEX both use this layout) */
typedef struct __attribute__((packed)) {
    btrfs_key_t location;   /* key of the target inode (INODE_ITEM) */
    uint64_t transid;
    uint16_t data_len;
    uint16_t name_len;
    uint8_t  type;
    /* name[name_len] then data[data_len] follow immediately */
} btrfs_dir_item_t;

/* Root item fields used to locate a subvolume's tree root */
typedef struct __attribute__((packed)) {
    btrfs_inode_item_t inode;
    uint64_t generation;
    uint64_t root_dirid;
    uint64_t bytenr;         /* logical addr of this root's tree root node */
    uint64_t byte_limit;
    uint64_t bytes_used;
    uint64_t last_snapshot;
    uint64_t flags;
    uint32_t refs;
    btrfs_key_t drop_progress;
    uint8_t  drop_level;
    uint8_t  level;
    /* generation_v2 + uuids + timestamps follow; unused here */
} btrfs_root_item_t;

/* File extent item (EXTENT_DATA) */
typedef struct __attribute__((packed)) {
    uint64_t generation;
    uint64_t ram_bytes;
    uint8_t  compression;
    uint8_t  encryption;
    uint16_t other_encoding;
    uint8_t  type;            /* BTRFS_FILE_EXTENT_* */
    /* For INLINE: raw file data follows immediately (ram_bytes long,
     *   possibly compressed - only COMPRESS_NONE is supported here).
     * For REG/PREALLOC: the fields below follow. */
    uint64_t disk_bytenr;     /* logical addr of the extent on disk, 0 = hole */
    uint64_t disk_num_bytes;
    uint64_t offset;          /* offset within the extent this file uses */
    uint64_t num_bytes;
} btrfs_file_extent_item_t;

#endif /* BTRFS_DISK_H */
