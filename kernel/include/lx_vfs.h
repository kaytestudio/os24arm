/* The kernel's VFS calls, handed to DOSCALLS.DLL (via lx_services) so
 * LX programs work on the kernel's own mounts - USB sticks MOUNTed in the
 * shell included - instead of a private second copy of the boot disk. */
#ifndef LX_VFS_H
#define LX_VFS_H

#include "vfs.h"

struct lx_vfs_ops {
    int (*open)(const char *path, int flags);
    int (*close)(int fd);
    int (*read)(int fd, void *buf, uint32_t len);
    int (*write)(int fd, const void *buf, uint32_t len);
    int (*seek)(int fd, int32_t offset, int whence);
    int (*stat)(const char *path, vfs_stat_t *st);
    int (*statfs)(const char *path, vfs_statfs_t *st, const char **fsname);
    int (*unlink)(const char *path);
    int (*rename)(const char *old, const char *nw);
    int (*mkdir)(const char *path);
    int (*opendir)(const char *path);
    int (*readdir)(int dfd, vfs_dirent_t *ent);
    int (*closedir)(int dfd);
    int (*mount_info)(int index, const char **mountpoint, const char **fsname);
    int (*mount_quiet)(int index);
};

#endif
