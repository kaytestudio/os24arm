/**
 * @file f2fs_internal.h
 * @brief Shared declarations between f2fs.c (mount/read) and
 *        f2fs_write.c (the LFS write engine) - not part of the public
 *        driver interface (that's f2fs.h).
 */

#ifndef F2FS_INTERNAL_H
#define F2FS_INTERNAL_H

#include "f2fs.h"

extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);

/* ── CRC32 (f2fs.c) - standard reflected CRC-32/ISO-HDLC table, seeded by
 * the caller (checkpoint/superblock checks seed with F2FS_SUPER_MAGIC, per
 * the real driver's f2fs_crc32()) with no pre/post complement. ─────────── */
uint32_t f2fs_crc32(uint32_t seed, const void *data, uint32_t len);

/* ── Block I/O (f2fs.c) ──────────────────────────────────────────────────── */
int f2fs_read_block(f2fs_mount_t *mnt, uint32_t blkaddr, void *buf);

/* ── NAT / node (f2fs.c) ─────────────────────────────────────────────────── */
int f2fs_get_node_blkaddr(f2fs_mount_t *mnt, uint32_t nid, uint32_t *out_blkaddr);
int f2fs_read_node(f2fs_mount_t *mnt, uint32_t nid, struct f2fs_node *out);
int f2fs_read_inode(f2fs_mount_t *mnt, uint32_t ino, struct f2fs_inode *out);
int f2fs_map_block(f2fs_mount_t *mnt, struct f2fs_inode *inode, uint32_t logical_block, uint32_t *out_phys);
int f2fs_get_file_block(f2fs_mount_t *mnt, struct f2fs_inode *inode, uint32_t logical_block, uint8_t *buf);
uint64_t f2fs_inode_size(const struct f2fs_inode *inode);

/* ── Path / directory (f2fs.c) ───────────────────────────────────────────── */
int f2fs_resolve_path(f2fs_mount_t *mnt, const char *path, uint32_t *out_ino, struct f2fs_inode *out_inode);
uint32_t f2fs_dir_lookup(f2fs_mount_t *mnt, struct f2fs_inode *dir_inode, const char *name, int name_len, uint8_t *out_type);

/* ── Write engine (f2fs_write.c) ─────────────────────────────────────────── */

/* Called once at the end of a successful mount. Sets mnt->writable = 1
 * only if the image is within this driver's fixed parameters (4096-byte
 * blocks, 512 blocks/segment, cp_payload == 0 - see f2fs_disk.h) and the
 * last checkpoint was cleanly unmounted (CP_UMOUNT_FLAG set, no orphans) -
 * this driver does no roll-forward recovery, so it only writes to images
 * it can be sure are already fully consistent. */
void f2fs_write_init_allocators(f2fs_mount_t *mnt);

int f2fs_write_open(f2fs_mount_t *mnt, const char *path, int flags, f2fs_file_t **out_file);
int f2fs_write_data(f2fs_file_t *f, const void *buf, uint32_t len, uint32_t *actual);
int f2fs_do_mkdir(f2fs_mount_t *mnt, const char *path);
int f2fs_do_unlink(f2fs_mount_t *mnt, const char *path);
int f2fs_do_rename(f2fs_mount_t *mnt, const char *old_path, const char *new_path);

#endif /* F2FS_INTERNAL_H */
