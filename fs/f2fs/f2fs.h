/**
 * @file f2fs.h
 * @brief f2fs Filesystem Driver for OS/2 Warp ARM64 - public interface
 * @defgroup f2fs f2fs Filesystem
 * @ingroup filesystems
 *
 * See fs/f2fs/f2fs_disk.h for the on-disk format this driver understands
 * (a deliberately reduced subset of real f2fs) and fs/f2fs/f2fs_write.c
 * for exactly what write support does and does not maintain.
 */

#ifndef F2FS_H
#define F2FS_H

#include "types.h"
#include "vfs.h"
#include "f2fs_disk.h"

/* ── Mount data ───────────────────────────────────────────────────────────── */

typedef struct {
    void                    *dev;
    struct f2fs_super_block  sb;
    struct f2fs_checkpoint   ckpt;
    uint32_t                 blocks_per_seg;   /* always 512 - see f2fs_disk.h */
    uint32_t                 main_segments;    /* sb.segment_count_main */
    uint8_t                 *block_buffer;     /* one block, directory/data content scratch */
    uint8_t                 *node_buffer;      /* one block, node/dnode scratch */
    uint8_t                 *bitmap_buffer;    /* one block, SIT/NAT block I/O scratch */
    int                      cur_cp_pack;      /* 1 or 2: which pack is the currently valid checkpoint */
    int                      writable;         /* set by f2fs_write_init_allocators() */
} f2fs_mount_t;

/* ── File / directory handles ────────────────────────────────────────────── */

typedef struct {
    f2fs_mount_t     *mnt;
    uint32_t          ino;     /* == nid of this file's own inode node block */
    struct f2fs_inode inode;
    uint32_t          pos;
    int               flags;
} f2fs_file_t;

typedef struct {
    f2fs_mount_t *mnt;
    f2fs_file_t  *dir_file;
    uint32_t      pos;         /* byte offset into the directory's data */
} f2fs_dir_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

int       f2fs_init(const char *params);
vfs_fs_t *f2fs_get_fs(void);

#endif /* F2FS_H */
