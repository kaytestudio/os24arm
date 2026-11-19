/**
 * @file btrfs_internal.h
 * @brief Shared declarations between btrfs.c (mount/read) and
 *        btrfs_write.c (the CoW write engine) - not part of the public
 *        driver interface (that's btrfs.h).
 */

#ifndef BTRFS_INTERNAL_H
#define BTRFS_INTERNAL_H

#include "btrfs.h"
#include "blkdev.h"

/* On-disk fixed sizes for the packed structs in btrfs_disk.h - btrfs.c
 * uses these instead of sizeof() since the structs are read/written as
 * raw byte layouts, not via direct struct assignment. */
#define BTRFS_HEADER_SIZE  101
#define BTRFS_ITEM_SIZE    25
#define BTRFS_KEY_PTR_SIZE 33

extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);

/* Key helpers (btrfs.c) */
int key_cmp(const btrfs_key_t *a, const btrfs_key_t *b);
btrfs_key_t key_successor(btrfs_key_t k);

/* Physical / logical block I/O (btrfs.c) */
int btrfs_read_phys(blkdev_t *dev, uint64_t phys_offset, void *buf, uint32_t len);
int btrfs_write_phys(blkdev_t *dev, uint64_t phys_offset, const void *buf, uint32_t len);
int btrfs_logical_to_phys(btrfs_mount_t *mnt, uint64_t logical, uint64_t *out_phys);
int btrfs_read_logical(btrfs_mount_t *mnt, uint64_t logical, void *buf, uint32_t len);
int btrfs_write_logical(btrfs_mount_t *mnt, uint64_t logical, const void *buf, uint32_t len);
int btrfs_read_node(btrfs_mount_t *mnt, uint64_t logical, uint8_t *buf);
int btrfs_write_node(btrfs_mount_t *mnt, uint64_t logical, const uint8_t *buf);

/* Chunk map (btrfs.c) */
void add_chunk_map(btrfs_mount_t *mnt, uint64_t logical, uint64_t length, uint64_t phys, uint64_t type);

/* B-tree search (btrfs.c) */
int btrfs_search(btrfs_mount_t *mnt, uint64_t root_logical, uint8_t root_level,
                  btrfs_key_t target, uint8_t *leaf_out, uint32_t *index_out);
int item_at(const uint8_t *leaf, uint32_t index, btrfs_item_t *out_item, const uint8_t **out_data);

/* Inode / directory (btrfs.c) */
int btrfs_read_inode(btrfs_mount_t *mnt, uint64_t ino, btrfs_inode_item_t *out);
int btrfs_dir_step(btrfs_mount_t *mnt, uint64_t dir_ino, uint64_t *pos,
                    const char *want_name, int want_len,
                    uint64_t *out_child_ino, uint8_t *out_type,
                    char *out_name, int *out_name_len);
uint64_t btrfs_dir_lookup(btrfs_mount_t *mnt, uint64_t dir_ino, const char *name, int name_len, uint8_t *out_type);
int btrfs_resolve_path(btrfs_mount_t *mnt, const char *path, uint64_t *out_ino, btrfs_inode_item_t *out_inode);
int btrfs_inode_pread(btrfs_mount_t *mnt, uint64_t ino, btrfs_inode_item_t *inode,
                       uint64_t offset, void *buf, uint32_t len, uint32_t *out_actual);

/* Write engine (btrfs_write.c) */

/* Called once at the end of a successful mount to bootstrap the bump
 * allocator and inode-number counter by scanning the existing trees.
 * Sets mnt->writable = 0 (falls back to read-only) if anything about the
 * fs looks unsupported for writing. */
void btrfs_write_init_allocators(btrfs_mount_t *mnt);

int btrfs_write_open(btrfs_mount_t *mnt, const char *path, int flags, btrfs_file_t **out_file);
int btrfs_write_data(btrfs_file_t *f, const void *buf, uint32_t len, uint32_t *actual);
int btrfs_do_unlink(btrfs_mount_t *mnt, const char *path);
int btrfs_do_mkdir(btrfs_mount_t *mnt, const char *path);
int btrfs_do_rename(btrfs_mount_t *mnt, const char *old_path, const char *new_path);

#endif /* BTRFS_INTERNAL_H */
