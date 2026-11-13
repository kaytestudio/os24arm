/* ============================================================================
 * kernel/src/vfs.c  —  Virtual File System layer
 *
 * Sits between userspace (CLI, BASIC) and filesystem drivers (FAT32).
 * Manages mount points, file descriptors, and dispatches to fs ops.
 * ========================================================================== */

#include "vfs.h"
#include "kio.h"
#include "uart.h"  /*** @brief uart.h **/

/* ── Mount table ─────────────────────────────────────────────────────────── */

static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static int         mount_count = 0;

/* ── File / dir descriptor tables ───────────────────────────────────────── */

static vfs_file_t files[VFS_MAX_FILES];
static vfs_dir_t  dirs [VFS_MAX_DIRS];

/* ══════════════════════════════════════════════════════════════════════════
   INTERNAL HELPERS
   ══════════════════════════════════════════════════════════════════════════ */

/* Mountpoints match case-insensitively, as OS/2 paths do ("\usb" and
 * "\USB" are the same drive); what's below the mountpoint is passed to
 * the filesystem unchanged. */
static int mp_prefix(const char *path, const char *mp, int n)
{
    for (int i = 0; i < n; i++) {
        char a = path[i], b = mp[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 1;
    }
    return 0;
}

/* Find the best-matching mount for a path.
   "C:/FOO" matches mount "C:", "/" matches mount "/".
   Returns NULL if none. */
static vfs_mount_t *find_mount(const char *path)
{
    vfs_mount_t *best = NULL;
    int          best_len = -1;

    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].fs) continue;                  /* free slot */
        const char *mp  = mounts[i].mountpoint;
        int         mpl = kstrlen(mp);

        /* Match the mountpoint prefix */
        if (mp_prefix(path, mp, mpl) != 0) continue;

        /* Root mount "/" matches any absolute path: the leading slash IS
         * the separator, so there's no boundary character left to check
         * (unlike "C:", where the next char must still be '/' or ':'). */
        if (!(mpl == 1 && mp[0] == '/')) {
            char next = path[mpl];
            if (next != '\0' && next != '/' && next != ':' && next != '\\')
                continue;
        }

        if (mpl > best_len) { best = &mounts[i]; best_len = mpl; }
    }
    return best;
}

/* Strip the mountpoint prefix from a path, returning the relative path.
   "C:/FOO/BAR" with mount "C:" → "/FOO/BAR" */
static const char *relative_path(vfs_mount_t *mnt, const char *path)
{
    int mpl = kstrlen(mnt->mountpoint);
    path += mpl;
    /* skip a trailing ':' used in DOS-style paths */
    if (*path == ':') path++;
    if (*path == '\0') return "/";
    return path;
}

/* Allocate a file descriptor */
static int alloc_fd(void)
{
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!files[i].used) return i;
    }
    return VFS_ERR_NOSPACE;
}

/* Allocate a directory descriptor (offset into dirs[], returned as dfd = fd + VFS_MAX_FILES) */
static int alloc_dfd(void)
{
    for (int i = 0; i < VFS_MAX_DIRS; i++) {
        if (!dirs[i].used) return i;
    }
    return VFS_ERR_NOSPACE;
}

/* ══════════════════════════════════════════════════════════════════════════
   PUBLIC API
   ══════════════════════════════════════════════════════════════════════════ */

void vfs_init(void)
{
    uart_puts("DEBUG: vfs_init starting\r\n");

    // Arrays (mounts, files, dirs) are already zeroed by boot.S BSS clearing
    // Just initialize the global counter
    mount_count = 0;

    uart_puts("DEBUG: vfs_init complete\r\n");
}
/* ── Mount management ────────────────────────────────────────────────────── */

/* Slots never move once used: open files and directories keep pointers to
 * their mount (files[].mnt, dirs[].mnt), so unmounting frees a slot in
 * place and mounting reuses the first free one. */
int vfs_mount(const char *mountpoint, vfs_fs_t *fs, void *fs_priv)
{
    int slot = -1;
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].fs) { if (slot < 0) slot = i; continue; }
        if (kstrcmp(mounts[i].mountpoint, mountpoint) == 0) return VFS_ERR_INVAL;
    }
    if (slot < 0) {
        if (mount_count >= VFS_MAX_MOUNTS) return VFS_ERR_NOSPACE;
        slot = mount_count;
    }

    vfs_mount_t *mnt = &mounts[slot];
    kstrncpy(mnt->mountpoint, mountpoint, VFS_PATH_MAX);
    mnt->fs   = fs;
    mnt->priv = fs_priv;

    int r = fs->mount(mnt);
    if (r != VFS_OK) {
        kmemset(mnt, 0, sizeof(*mnt));
        kprintf("[VFS]  Mount '%s' failed: %d\n", mountpoint, r);
        return r;
    }

    if (slot == mount_count) mount_count++;
    kprintf("[VFS]  Mounted '%s' as '%s'\n", fs->name, mountpoint);
    return VFS_OK;
}

int vfs_unmount(const char *mountpoint)
{
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].fs || kstrcmp(mounts[i].mountpoint, mountpoint) != 0)
            continue;
        for (int f = 0; f < VFS_MAX_FILES; f++)
            if (files[f].used && files[f].mnt == &mounts[i]) return VFS_ERR_BUSY;
        for (int d = 0; d < VFS_MAX_DIRS; d++)
            if (dirs[d].used && dirs[d].mnt == &mounts[i]) return VFS_ERR_BUSY;
        if (mounts[i].fs->unmount) mounts[i].fs->unmount(&mounts[i]);
        kmemset(&mounts[i], 0, sizeof(mounts[i]));    /* free in place */
        return VFS_OK;
    }
    return VFS_ERR_NOMOUNT;
}

int vfs_mount_info(int index, const char **mountpoint, const char **fsname)
{
    int n = 0;
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].fs) continue;
        if (n++ == index) {
            if (mountpoint) *mountpoint = mounts[i].mountpoint;
            if (fsname)     *fsname     = mounts[i].fs->name;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOMOUNT;
}

/* ── File operations ─────────────────────────────────────────────────────── */

int vfs_open(const char *path, int flags)
{
    vfs_mount_t *mnt = find_mount(path);
    if (!mnt) return VFS_ERR_NOMOUNT;

    int fd = alloc_fd();
    if (fd < 0) return fd;

    void *fpriv = NULL;
    int r = mnt->fs->open(mnt, relative_path(mnt, path), flags, &fpriv);
    if (r != VFS_OK) return r;

    files[fd].used  = true;
    files[fd].mnt   = mnt;
    files[fd].fpriv = fpriv;
    files[fd].flags = flags;
    files[fd].pos   = 0;
    return fd;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used) return VFS_ERR_BADFD;
    int r = files[fd].mnt->fs->close(files[fd].mnt, files[fd].fpriv);
    files[fd].used = false;
    return r;
}

int vfs_read(int fd, void *buf, uint32_t len)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used) return VFS_ERR_BADFD;
    uint32_t actual = 0;
    int r = files[fd].mnt->fs->read(files[fd].mnt, files[fd].fpriv, buf, len, &actual);
    if (r != VFS_OK) return r;
    return (int)actual;
}

int vfs_write(int fd, const void *buf, uint32_t len)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used) return VFS_ERR_BADFD;
    uint32_t actual = 0;
    int r = files[fd].mnt->fs->write(files[fd].mnt, files[fd].fpriv, buf, len, &actual);
    if (r != VFS_OK) return r;
    return (int)actual;
}

int vfs_seek(int fd, int32_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used) return VFS_ERR_BADFD;
    uint32_t newpos = 0;
    if (!files[fd].mnt->fs->seek) return VFS_ERR_INVAL;          /* driver can't seek */
    int r = files[fd].mnt->fs->seek(files[fd].mnt, files[fd].fpriv, offset, whence, &newpos);
    if (r != VFS_OK) return r;
    files[fd].pos = newpos;
    return (int)newpos;
}

int vfs_stat(const char *path, vfs_stat_t *st)
{
    vfs_mount_t *mnt = find_mount(path);
    if (!mnt) return VFS_ERR_NOMOUNT;
    return mnt->fs->stat(mnt, relative_path(mnt, path), st);
}

int vfs_unlink(const char *path)
{
    vfs_mount_t *mnt = find_mount(path);
    if (!mnt) return VFS_ERR_NOMOUNT;
    return mnt->fs->unlink(mnt, relative_path(mnt, path));
}

int vfs_rename(const char *old, const char *nw)
{
    vfs_mount_t *mnt = find_mount(old);
    if (!mnt) return VFS_ERR_NOMOUNT;
    /* Cross-mount rename not supported */
    vfs_mount_t *mnt2 = find_mount(nw);
    if (mnt != mnt2) return VFS_ERR_INVAL;
    if (!mnt->fs->rename) return VFS_ERR_ROFS;
    return mnt->fs->rename(mnt, relative_path(mnt, old), relative_path(mnt, nw));
}

int vfs_mkdir(const char *path)
{
    vfs_mount_t *mnt = find_mount(path);
    if (!mnt) return VFS_ERR_NOMOUNT;
    return mnt->fs->mkdir(mnt, relative_path(mnt, path));
}

/* ── Directory operations ─────────────────────────────────────────────────── */

int vfs_opendir(const char *path)
{
    vfs_mount_t *mnt = find_mount(path);
    if (!mnt) return VFS_ERR_NOMOUNT;

    int di = alloc_dfd();
    if (di < 0) return di;

    void *dpriv = NULL;
    int r = mnt->fs->opendir(mnt, relative_path(mnt, path), &dpriv);
    if (r != VFS_OK) return r;

    dirs[di].used  = true;
    dirs[di].mnt   = mnt;
    dirs[di].dpriv = dpriv;

    /* Return as offset to distinguish from file fds */
    return di + VFS_MAX_FILES;
}

int vfs_readdir(int dfd, vfs_dirent_t *ent)
{
    int di = dfd - VFS_MAX_FILES;
    if (di < 0 || di >= VFS_MAX_DIRS || !dirs[di].used) return VFS_ERR_BADFD;
    return dirs[di].mnt->fs->readdir(dirs[di].mnt, dirs[di].dpriv, ent);
}

int vfs_closedir(int dfd)
{
    int di = dfd - VFS_MAX_FILES;
    if (di < 0 || di >= VFS_MAX_DIRS || !dirs[di].used) return VFS_ERR_BADFD;
    int r = dirs[di].mnt->fs->closedir(dirs[di].mnt, dirs[di].dpriv);
    dirs[di].used = false;
    return r;
}