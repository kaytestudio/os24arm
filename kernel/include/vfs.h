/* ============================================================================
 * kernel/include/vfs.h  —  Virtual File System Interface
 *
 * This header matches the actual vfs.c implementation
 * ========================================================================== */

#ifndef VFS_H
#define VFS_H

#include "types.h"

/* ── Configuration ───────────────────────────────────────────────────────── */

#define VFS_MAX_MOUNTS   8
#define VFS_MAX_FILES    32
#define VFS_MAX_DIRS     16
#define VFS_PATH_MAX     260
#define VFS_NAME_MAX     255

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define VFS_OK           0
#define VFS_ERR_NOSPACE  -1
#define VFS_ERR_NOMOUNT  -2
#define VFS_ERR_BADFD    -3
#define VFS_ERR_INVAL    -4
#define VFS_ERR_IO       -5
#define VFS_ERR_NOTFOUND -6
#define VFS_ERR_NOTDIR   -7
#define VFS_ERR_ISDIR    -8
#define VFS_ERR_ROFS     -9
#define VFS_ERR_BUSY     -10   /* unmount: files/dirs still open */

/* Aliases for compatibility */
#define VFS_EIO       VFS_ERR_IO
#define VFS_EINVAL    VFS_ERR_INVAL
#define VFS_ENOENT    VFS_ERR_NOTFOUND
#define VFS_ENOTDIR   VFS_ERR_NOTDIR
#define VFS_EISDIR    VFS_ERR_ISDIR
#define VFS_ENOSPC    VFS_ERR_NOSPACE
#define VFS_EROFS     VFS_ERR_ROFS

/* ── Open flags ──────────────────────────────────────────────────────────── */

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

/* ── Seek whence ─────────────────────────────────────────────────────────── */

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* ── Forward declarations ────────────────────────────────────────────────── */

typedef struct vfs_mount    vfs_mount_t;
typedef struct vfs_file     vfs_file_t;
typedef struct vfs_dir      vfs_dir_t;
typedef struct vfs_fs       vfs_fs_t;
typedef struct vfs_dirent   vfs_dirent_t;
typedef struct vfs_stat     vfs_stat_t;

/* A volume's size and free space, in blocks of bsize bytes, and its
 * label ("" if the driver doesn't report one). */
typedef struct {
    __UINT64_TYPE__ bsize, blocks, bfree;
    char label[32];
} vfs_statfs_t;

/* ── Filesystem operations ───────────────────────────────────────────────── */

struct vfs_fs {
    const char *name;  /* "fat32", "ext4", etc. */
    
    /* Mount/unmount */
    int (*mount)   (vfs_mount_t *mnt);
    int (*unmount) (vfs_mount_t *mnt);
    
    /* File operations */
    int (*open)    (vfs_mount_t *mnt, const char *path, int flags, void **fpriv);
    int (*close)   (vfs_mount_t *mnt, void *fpriv);
    int (*read)    (vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual);
    int (*write)   (vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual);
    int (*seek)    (vfs_mount_t *mnt, void *fpriv, int32_t offset, int whence, uint32_t *newpos);
    
    /* Metadata */
    int (*stat)    (vfs_mount_t *mnt, const char *path, vfs_stat_t *st);
    int (*unlink)  (vfs_mount_t *mnt, const char *path);
    int (*rename)  (vfs_mount_t *mnt, const char *old, const char *nw);
    int (*mkdir)   (vfs_mount_t *mnt, const char *path);
    
    /* Directory operations */
    int (*opendir) (vfs_mount_t *mnt, const char *path, void **dpriv);
    int (*readdir) (vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent);
    int (*closedir)(vfs_mount_t *mnt, void *dpriv);

    /* Optional: size and free space (df) */
    int (*statfs)  (vfs_mount_t *mnt, vfs_statfs_t *st);
};

/* ── Mount point ─────────────────────────────────────────────────────────── */

struct vfs_mount {
    char        mountpoint[VFS_PATH_MAX];
    vfs_fs_t   *fs;
    void       *priv;  /* Filesystem-private data (e.g., fat32_mount_t*) */
};

/* ── File descriptor ─────────────────────────────────────────────────────── */

struct vfs_file {
    bool         used;
    vfs_mount_t *mnt;
    void        *fpriv;  /* Filesystem-private file handle */
    int          flags;
    uint32_t     pos;
};

/* ── Directory descriptor ────────────────────────────────────────────────── */

struct vfs_dir {
    bool         used;
    vfs_mount_t *mnt;
    void        *dpriv;  /* Filesystem-private dir handle */
};

/* ── Directory entry ─────────────────────────────────────────────────────── */

struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    /* Note: fat32.c uses .attrs field, but it's not in vfs.c */
    /* You may want to add: uint32_t attrs; */
};

/* ── Stat structure ──────────────────────────────────────────────────────── */

struct vfs_stat {
    /* Note: fat32.c uses .name, .size, .attrs, .mdate, .mtime */
    /* But vfs.c doesn't define these fields */
    /* Add the fields your fat32.c actually needs: */
    char     name[VFS_NAME_MAX + 1];
    uint32_t size;
    uint32_t attrs;
    uint16_t mdate;
    uint16_t mtime;
};

/* ── File attributes ─────────────────────────────────────────────────────── */

#define VFS_ATTR_RDONLY     0x01
#define VFS_ATTR_HIDDEN     0x02
#define VFS_ATTR_SYSTEM     0x04
#define VFS_ATTR_VOLUME     0x08
#define VFS_ATTR_DIR        0x10
#define VFS_ATTR_ARCHIVE    0x20

/* ── VFS API ─────────────────────────────────────────────────────────────── */

/* Initialize VFS */
void vfs_init(void);

/* Mount/unmount */
int vfs_mount(const char *mountpoint, vfs_fs_t *fs, void *fs_priv);
int vfs_unmount(const char *mountpoint);
/* index-th active mount (0-based): VFS_OK, or VFS_ERR_NOMOUNT past the end */
int vfs_mount_info(int index, const char **mountpoint, const char **fsname);

/* File operations (fd-based) */
int vfs_open(const char *path, int flags);
int vfs_close(int fd);
int vfs_read(int fd, void *buf, uint32_t len);
int vfs_write(int fd, const void *buf, uint32_t len);
int vfs_seek(int fd, int32_t offset, int whence);

/* Metadata operations */
int vfs_stat(const char *path, vfs_stat_t *st);
/* The volume path is on: size and free space, and its file system's name
 * (fsname may be NULL). VFS_ERR_INVAL if its driver can't tell. */
int vfs_statfs(const char *path, vfs_statfs_t *st, const char **fsname);
extern int vfs_quiet;   /* nonzero: vfs_mount() says nothing */
/* Seconds since 1970 -> DOS date/time words (both 0 outside 1980-2107). */
void vfs_unix_to_dos(unsigned long long t, uint16_t *date, uint16_t *time);
/* Which mount slot holds path (a stable number while mounted), or -1. */
int vfs_mount_index(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *old, const char *nw);
int vfs_mkdir(const char *path);

/* Directory operations (dfd-based, offset by VFS_MAX_FILES) */
int vfs_opendir(const char *path);
int vfs_readdir(int dfd, vfs_dirent_t *ent);
int vfs_closedir(int dfd);

#endif

