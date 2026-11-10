/**
 * @file ext4_internal.h
 * @brief Shared declarations between ext4.c (mount/read) and ext4_write.c
 *        (the bitmap-allocator write engine) - not part of the public
 *        driver interface (that's ext4.h).
 */

#ifndef EXT4_INTERNAL_H
#define EXT4_INTERNAL_H

#include "ext4.h"

extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);

/* ── Block / inode I/O (ext4.c) ──────────────────────────────────────────── */
int      ext4_read_block(ext4_mount_t *mnt, uint64_t block_num, void *buf);
int      ext4_read_inode(ext4_mount_t *mnt, uint32_t ino, ext4_inode_t *out);
int      ext4_get_file_block(ext4_mount_t *mnt, ext4_inode_t *inode, uint32_t logical_block, uint8_t *buf);
int      ext4_map_block(ext4_mount_t *mnt, ext4_inode_t *inode, uint32_t logical_block, uint64_t *out_phys);
uint64_t ext4_inode_size(const ext4_inode_t *inode);

/* ── Path / directory lookup (ext4.c) ────────────────────────────────────── */
int      ext4_resolve_path(ext4_mount_t *mnt, const char *path, uint32_t *out_ino, ext4_inode_t *out_inode);
uint32_t ext4_dir_lookup(ext4_mount_t *mnt, ext4_inode_t *dir_inode, const char *name, int name_len);

/* ── Write engine (ext4_write.c) ─────────────────────────────────────────── */

/* Called once at the end of a successful mount. Sets mnt->writable = 1 only
 * if the on-disk feature flags are within what this driver knows how to
 * write to safely (see ext4_write.c's header comment) - otherwise mnt stays
 * read-only, same as any other "layout not supported for writing" case. */
void ext4_write_init_allocators(ext4_mount_t *mnt);

int ext4_write_open(ext4_mount_t *mnt, const char *path, int flags, ext4_file_t **out_file);
int ext4_write_data(ext4_file_t *f, const void *buf, uint32_t len, uint32_t *actual);
int ext4_do_mkdir(ext4_mount_t *mnt, const char *path);
int ext4_do_unlink(ext4_mount_t *mnt, const char *path);
int ext4_do_rename(ext4_mount_t *mnt, const char *old_path, const char *new_path);

#endif /* EXT4_INTERNAL_H */
