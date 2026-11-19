/* ─────────────────────────────────────────────────────────────
 * vfs.h  –  Virtual Filesystem Switch
 *
 * All filesystem drivers register a vfs_driver_t.
 * User code calls vfs_* functions; the VFS dispatches to
 * the correct driver based on the mount-point prefix.
 *
 * Mount table example:
 *   /       → ext4  on nvme0p2   (root)
 *   /boot   → fat32 on nvme0p1   (ESP)
 *   /media  → exfat on sda1      (USB)
 * ───────────────────────────────────────────────────────────── */
#pragma once
#include "types.h"
#include "blkdev.h"

/* ── Limits ──────────────────────────────────────────────────── */
#define VFS_MOUNT_MAX       16
#define VFS_PATH_MAX        512
#define VFS_NAME_MAX        255
#define VFS_FD_MAX          64
#define VFS_FSTYPE_LEN      16

/* ── Flags ───────────────────────────────────────────────────── */
#define VFS_O_RDONLY        0x0000
#define VFS_O_WRONLY        0x0001
#define VFS_O_RDWR          0x0002
#define VFS_O_CREAT         0x0100
#define VFS_O_TRUNC         0x0200
#define VFS_O_APPEND        0x0400
#define VFS_O_EXCL          0x0800
#define VFS_O_DIR           0x1000

#define VFS_SEEK_SET        0
#define VFS_SEEK_CUR        1
#define VFS_SEEK_END        2

/* File type flags (mode bits) */
#define VFS_S_IFREG         0x8000
#define VFS_S_IFDIR         0x4000
#define VFS_S_IFLNK         0xA000
#define VFS_ISREG(m)        (((m) & 0xF000) == VFS_S_IFREG)
#define VFS_ISDIR(m)        (((m) & 0xF000) == VFS_S_IFDIR)

/* ── Data structures ─────────────────────────────────────────── */
typedef struct {
    uint64_t  size;
    uint32_t  mode;        /* VFS_S_IF* | permissions */
    uint32_t  uid;
    uint32_t  gid;
    uint32_t  nlink;
    uint64_t  inode;
    uint64_t  atime;       /* Unix timestamps */
    uint64_t  mtime;
    uint64_t  ctime;
    uint32_t  blksize;
    uint64_t  blocks;
} vfs_stat_t;

typedef struct {
    uint64_t  inode;
    uint32_t  type;        /* DT_REG=8, DT_DIR=4, DT_LNK=10 */
    char      name[VFS_NAME_MAX + 1];
} vfs_dirent_t;

#define DT_UNKNOWN  0
#define DT_DIR      4
#define DT_REG      8
#define DT_LNK      10

/* Forward declarations */
typedef struct vfs_node   vfs_node_t;
typedef struct vfs_mount  vfs_mount_t;
typedef struct vfs_driver vfs_driver_t;
typedef struct vfs_file   vfs_file_t;

/* ── VFS node (inode abstraction) ───────────────────────────── */
struct vfs_node {
    uint64_t       inode;
    uint32_t       mode;
    uint64_t       size;
    uint64_t       mtime;
    vfs_mount_t   *mount;
    void          *priv;       /* driver-private inode data */
    uint32_t       refcount;
};

/* ── Open file handle ─────────────────────────────────────────── */
struct vfs_file {
    vfs_node_t    *node;
    uint64_t       offset;
    uint32_t       flags;
    bool           used;
};

/* ── Filesystem driver interface ─────────────────────────────── */
struct vfs_driver {
    char name[VFS_FSTYPE_LEN];      /* "ext4", "fat32", etc.        */

    /* Return true if dev contains this filesystem. */
    bool     (*probe)(blkdev_t *dev, uint64_t part_lba_start);

    /* Mount the filesystem; fill *mount->priv                     */
    int      (*mount)(vfs_mount_t *mount);

    /* Unmount, flush, free resources                              */
    int      (*umount)(vfs_mount_t *mount);

    /* Lookup name in directory node → fill *out_node              */
    int      (*lookup)(vfs_mount_t *mount, vfs_node_t *dir,
                       const char *name, vfs_node_t *out_node);

    /* Read bytes from a regular file                              */
    int64_t  (*read)(vfs_mount_t *mount, vfs_node_t *node,
                     uint64_t offset, uint64_t size, void *buf);

    /* Write bytes to a regular file (may be NULL for read-only)  */
    int64_t  (*write)(vfs_mount_t *mount, vfs_node_t *node,
                      uint64_t offset, uint64_t size, const void *buf);

    /* Create a file/directory entry                               */
    int      (*create)(vfs_mount_t *mount, vfs_node_t *dir,
                       const char *name, uint32_t mode,
                       vfs_node_t *out_node);

    /* Remove a name from a directory                              */
    int      (*unlink)(vfs_mount_t *mount, vfs_node_t *dir,
                       const char *name);

    /* Read directory entries; *cookie=0 starts iteration         */
    int      (*readdir)(vfs_mount_t *mount, vfs_node_t *dir,
                        uint64_t *cookie, vfs_dirent_t *out);

    /* Get file metadata                                           */
    int      (*stat)(vfs_mount_t *mount, vfs_node_t *node,
                     vfs_stat_t *out);

    /* Sync / flush to disk                                        */
    int      (*sync)(vfs_mount_t *mount);

    /* Filesystem info                                             */
    int      (*statfs)(vfs_mount_t *mount,
                       uint64_t *total_blocks, uint64_t *free_blocks,
                       uint32_t *block_size);
};

/* ── Mount entry ─────────────────────────────────────────────── */
struct vfs_mount {
    char          path[VFS_PATH_MAX];   /* mount point, e.g. "/"   */
    vfs_driver_t *driver;
    blkdev_t     *dev;
    uint64_t      part_lba;            /* partition start LBA      */
    bool          read_only;
    bool          used;
    vfs_node_t    root;                /* root inode of this mount */
    void         *fs_priv;            /* driver private state     */
};

/* ── VFS API ─────────────────────────────────────────────────── */
void  vfs_init(void);

/* Register a filesystem driver */
int   vfs_register_driver(vfs_driver_t *drv);

/* Mount dev (starting at part_lba) at path using filesystem type */
int   vfs_mount(blkdev_t *dev, uint64_t part_lba,
                const char *fstype, const char *path, bool ro);

/* Auto-detect filesystem on dev+part_lba and mount at path       */
int   vfs_automount(blkdev_t *dev, uint64_t part_lba,
                    const char *path, bool ro);

int   vfs_umount(const char *path);

/* POSIX-like file operations */
int       vfs_open  (const char *path, uint32_t flags);
int       vfs_close (int fd);
int64_t   vfs_read  (int fd, void *buf, uint64_t size);
int64_t   vfs_write (int fd, const void *buf, uint64_t size);
int64_t   vfs_seek  (int fd, int64_t offset, int whence);
int64_t   vfs_tell  (int fd);
int       vfs_stat  (const char *path, vfs_stat_t *out);
int       vfs_fstat (int fd, vfs_stat_t *out);
int       vfs_mkdir (const char *path, uint32_t mode);
int       vfs_unlink(const char *path);

/* Directory iteration */
typedef struct { vfs_node_t node; uint64_t cookie; } vfs_dir_t;
int   vfs_opendir (const char *path, vfs_dir_t *out);
int   vfs_readdir (vfs_dir_t *dir, vfs_dirent_t *out);
void  vfs_closedir(vfs_dir_t *dir);

/* Filesystem info */
int   vfs_statfs(const char *path,
                 uint64_t *total_blocks, uint64_t *free_blocks,
                 uint32_t *block_size);

/* Sync all mounted filesystems */
void  vfs_sync_all(void);

/* List mounted filesystems */
void  vfs_list_mounts(void);

/* Error codes */
#define VFS_OK          0
#define VFS_ERR_NOENT  -1   /* No such file or directory */
#define VFS_ERR_EXIST  -2   /* File already exists       */
#define VFS_ERR_ISDIR  -3   /* Is a directory            */
#define VFS_ERR_NOTDIR -4   /* Not a directory           */
#define VFS_ERR_PERM   -5   /* Permission denied         */
#define VFS_ERR_NOSPC  -6   /* No space left             */
#define VFS_ERR_IO     -7   /* I/O error                 */
#define VFS_ERR_INVAL  -8   /* Invalid argument          */
#define VFS_ERR_NOMEM  -9   /* Out of memory             */
#define VFS_ERR_NOTSUP -10  /* Not supported             */
#define VFS_ERR_BUSY   -11  /* Device or resource busy   */
#define VFS_ERR_BADF   -12  /* Bad file descriptor       */
#define VFS_ERR_NOFS   -13  /* No filesystem recognised  */
