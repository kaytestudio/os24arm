/**
 * @file btrfs.h
 * @brief btrfs Filesystem Driver for OS/2 Warp ARM64 (read/write)
 *
 * Write support (fs/btrfs/btrfs_write.c) maintains the chunk/root/fs trees
 * with real copy-on-write semantics, but deliberately does NOT maintain the
 * extent tree or checksum tree - see the header comment in btrfs_write.c
 * for why, and for the resulting real-world-interop caveat.
 */

#ifndef BTRFS_H
#define BTRFS_H

#include "types.h"
#include "vfs.h"
#include "btrfs_disk.h"
#include "blkdev.h"

#define BTRFS_MAX_CHUNKS 32
#define BTRFS_MAX_TREE_HEIGHT 8

/* One bootstrapped logical->physical chunk mapping. Single stripe only
 * (single device, no RAID) - stripe[0]'s device offset is used directly. */
typedef struct {
    uint64_t logical;
    uint64_t length;
    uint64_t phys;
    uint64_t type;   /* BTRFS_BLOCK_GROUP_* flags, from btrfs_chunk_t.type */
} btrfs_chunk_map_t;

/* Root-to-leaf search path, recorded so a leaf edit knows which ancestor
 * key-pointers (and possibly keys) need rewriting after a CoW commit.
 * levels[0] is the leaf; levels[height-1] is the tree root. */
typedef struct {
    struct {
        uint64_t logical;  /* this level's node, as read during the search */
        uint32_t index;    /* child index followed (internal) / item index landed on (leaf) */
    } levels[BTRFS_MAX_TREE_HEIGHT];
    uint8_t height;
} btrfs_path_t;

typedef struct {
    void              *dev;
    uint32_t           sectorsize;
    uint32_t           nodesize;

    uint64_t           chunk_root;
    uint8_t            chunk_root_level;
    uint64_t           root_tree_root;
    uint8_t            root_tree_level;
    uint64_t           fs_tree_root;
    uint8_t            fs_tree_level;

    btrfs_chunk_map_t  chunks[BTRFS_MAX_CHUNKS];
    int                chunk_count;

    uint8_t           *node_buffer;   /* nodesize bytes: tree node I/O scratch */
    uint8_t           *leaf_buffer;   /* nodesize bytes: search-result leaf scratch */
    uint8_t           *io_buffer;     /* nodesize bytes: file-data read scratch */

    /* Write support: recomputed at every mount by scanning the existing
     * trees (never persisted) - same "recompute, don't persist" bump
     * allocator philosophy as kernel/src/mem.c's heap. */
    uint64_t           generation;       /* superblock generation at mount; bumped per commit */
    uint64_t           next_alloc;       /* next free logical addr for new blocks/extents */
    uint64_t           alloc_chunk_end;  /* end of the chunk backing next_alloc */
    uint64_t           next_ino;         /* next free inode number */
    int                writable;         /* 0 => mount succeeded but writes are refused */
} btrfs_mount_t;

typedef struct {
    btrfs_mount_t     *mnt;
    uint64_t           ino;
    btrfs_inode_item_t inode;
    uint64_t           pos;
    int                flags;
} btrfs_file_t;

typedef struct {
    btrfs_mount_t *mnt;
    btrfs_file_t  *dir_file;
    uint64_t       next_offset;  /* next DIR_INDEX key offset to search for */
} btrfs_dir_t;

int btrfs_init(const char *params);
int btrfs_probe(blkdev_t *dev);        /* 1: a btrfs superblock */
vfs_fs_t *btrfs_get_fs(void);

#endif /* BTRFS_H */
