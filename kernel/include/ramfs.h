/* kernel/include/ramfs.h - in-memory file system (kernel/src/ramfs.c) */
#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

extern vfs_fs_t vfs_ramfs;

/* Set the size of a file (path relative to the mount: "/a/b"); growing
 * adds zeros. VFS_OK or a VFS error. */
int ramfs_truncate(const char *path_in_mount, uint32_t size);

/* Page frames holding files and directories (files outlive programs). */
unsigned long ramfs_frames(void);

#endif /* RAMFS_H */
