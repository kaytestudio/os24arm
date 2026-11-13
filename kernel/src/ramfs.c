/**
 * @file ramfs.c
 * @brief A small in-memory file system (VFS driver), mounted at /TMP
 *
 * Files and directories live in RAM only: page frames from the MMU pool
 * (mmu_frames_alloc), so the contents are lost at reboot. Used by the
 * Linux-ABI user mode for /tmp (Wayland's XDG_RUNTIME_DIR lock files,
 * temporary files); the DOS shell can use it too (DIR \TMP).
 *
 * Limits: RAMFS_NODES files + directories in all, names up to 63 bytes
 * (case-sensitive), files up to 512 pages (2 MB) each.
 */

#include "ramfs.h"
#include "mmu.h"
#include "kio.h"

#define RAMFS_NODES   128
#define RAMFS_NAME    64
#define PAGE          4096u
#define MAX_PAGES     512u                 /* one frame of page pointers */

typedef struct {
    int       used;
    int       is_dir;
    int       parent;                      /* node index; root = 0 */
    int       open;                        /* open handles */
    int       unlinked;                    /* removed while open */
    char      name[RAMFS_NAME];
    uint32_t  size;
    uint8_t **pages;                       /* MAX_PAGES entries, or NULL */
    uint16_t  mdate, mtime;
} rnode_t;

typedef struct {
    int      node;
    uint32_t pos;
    int      append;
    int      used;
    int      dir_next;                     /* readdir cursor (node index) */
} rhandle_t;

static int same(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static rnode_t   nodes[RAMFS_NODES];
static unsigned long frames_held;          /* for leak checks (ramfs_frames) */

static void *ramfs_alloc(void)
{
    void *p = mmu_frames_alloc(1);
    if (p) frames_held++;
    return p;
}

static void ramfs_free(void *p)
{
    mmu_frames_free(p, 1);
    frames_held--;
}

unsigned long ramfs_frames(void) { return frames_held; }
static rhandle_t handles[48];
static int       inited;

static void init(void)
{
    if (inited) return;
    inited = 1;
    nodes[0].used = 1;
    nodes[0].is_dir = 1;
}

/* path relative to the mount ("/", "/a/b") -> node, or -1; *parent and
 * *leaf get the parent directory and last component when the last one
 * doesn't exist (for create). */
static int lookup(const char *path, int *parent, const char **leaf)
{
    init();
    int cur = 0;
    const char *s = path;
    if (parent) *parent = -1;
    for (;;) {
        while (*s == '/' || *s == '\\') s++;
        if (!*s) return cur;
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        uint32_t len = (uint32_t)(e - s);
        int found = -1;
        if (len < RAMFS_NAME)
            for (int i = 1; i < RAMFS_NODES; i++)
                if (nodes[i].used && !nodes[i].unlinked && nodes[i].parent == cur &&
                    kstrlen(nodes[i].name) == len && same(nodes[i].name, s, len)) {
                    found = i;
                    break;
                }
        if (found < 0) {
            const char *rest = e;
            while (*rest == '/' || *rest == '\\') rest++;
            if (!*rest && parent && len && len < RAMFS_NAME) { *parent = cur; *leaf = s; }
            return -1;
        }
        if (*e && !nodes[found].is_dir) return -1;
        cur = found;
        s = e;
    }
}

static int new_node(int parent, const char *leaf, int is_dir)
{
    if (!nodes[parent].is_dir) return VFS_ERR_NOTDIR;
    for (int i = 1; i < RAMFS_NODES; i++)
        if (!nodes[i].used) {
            kmemset(&nodes[i], 0, sizeof nodes[i]);
            nodes[i].used = 1;
            nodes[i].is_dir = is_dir;
            nodes[i].parent = parent;
            uint32_t n = 0;
            while (leaf[n] && leaf[n] != '/' && leaf[n] != '\\' && n < RAMFS_NAME - 1) {
                nodes[i].name[n] = leaf[n];
                n++;
            }
            nodes[i].name[n] = 0;
            return i;
        }
    return VFS_ERR_NOSPACE;
}

static void truncate_to(rnode_t *n, uint32_t size)
{
    if (n->pages)
        for (uint32_t p = (size + PAGE - 1) / PAGE; p < MAX_PAGES; p++)
            if (n->pages[p]) { ramfs_free(n->pages[p]); n->pages[p] = 0; }
    if (size < n->size && n->pages && size % PAGE && n->pages[size / PAGE])
        kmemset(n->pages[size / PAGE] + size % PAGE, 0, PAGE - size % PAGE);
    n->size = size;
}

static void free_node(int i)
{
    truncate_to(&nodes[i], 0);
    if (nodes[i].pages) ramfs_free(nodes[i].pages);
    nodes[i].used = 0;
}

static int r_mount(vfs_mount_t *mnt)   { (void)mnt; init(); return VFS_OK; }
static int r_unmount(vfs_mount_t *mnt) { (void)mnt; return VFS_OK; }

static int r_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv)
{
    (void)mnt;
    int parent; const char *leaf;
    int n = lookup(path, &parent, &leaf);
    if (n < 0) {
        if (!(flags & O_CREAT) || parent < 0) return VFS_ERR_NOTFOUND;
        if ((n = new_node(parent, leaf, 0)) < 0) return n;
    } else if (nodes[n].is_dir) {
        return VFS_ERR_ISDIR;
    } else if (flags & O_TRUNC) {
        truncate_to(&nodes[n], 0);
    }
    for (int h = 0; h < (int)(sizeof handles / sizeof handles[0]); h++)
        if (!handles[h].used) {
            handles[h] = (rhandle_t){ .node = n, .used = 1, .append = (flags & O_APPEND) != 0 };
            nodes[n].open++;
            *fpriv = &handles[h];
            return VFS_OK;
        }
    return VFS_ERR_NOSPACE;
}

static int r_close(vfs_mount_t *mnt, void *fpriv)
{
    (void)mnt;
    rhandle_t *h = fpriv;
    rnode_t *n = &nodes[h->node];
    h->used = 0;
    if (--n->open == 0 && n->unlinked) free_node(h->node);
    return VFS_OK;
}

static int r_read(vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    rhandle_t *h = fpriv;
    rnode_t *n = &nodes[h->node];
    uint32_t done = 0;
    while (done < len && h->pos < n->size) {
        uint32_t off = h->pos % PAGE, k = PAGE - off;
        if (k > len - done) k = len - done;
        if (k > n->size - h->pos) k = n->size - h->pos;
        uint8_t *pg = n->pages ? n->pages[h->pos / PAGE] : 0;
        if (pg) kmemcpy((uint8_t *)buf + done, pg + off, k);
        else kmemset((uint8_t *)buf + done, 0, k);
        done += k;
        h->pos += k;
    }
    *actual = done;
    return VFS_OK;
}

static int r_write(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    rhandle_t *h = fpriv;
    rnode_t *n = &nodes[h->node];
    *actual = 0;
    if (h->append) h->pos = n->size;
    if (!n->pages && !(n->pages = ramfs_alloc())) return VFS_ERR_NOSPACE;
    uint32_t done = 0;
    while (done < len) {
        uint32_t pi = h->pos / PAGE, off = h->pos % PAGE, k = PAGE - off;
        if (pi >= MAX_PAGES) break;
        if (k > len - done) k = len - done;
        if (!n->pages[pi] && !(n->pages[pi] = ramfs_alloc())) break;
        kmemcpy(n->pages[pi] + off, (const uint8_t *)buf + done, k);
        done += k;
        h->pos += k;
        if (h->pos > n->size) n->size = h->pos;
    }
    *actual = done;
    return done || !len ? VFS_OK : VFS_ERR_NOSPACE;
}

static int r_seek(vfs_mount_t *mnt, void *fpriv, int32_t offset, int whence, uint32_t *newpos)
{
    (void)mnt;
    rhandle_t *h = fpriv;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? h->pos : nodes[h->node].size;
    int64_t p = base + offset;
    if (p < 0 || p > (int64_t)MAX_PAGES * PAGE) return VFS_ERR_INVAL;
    h->pos = (uint32_t)p;
    *newpos = h->pos;
    return VFS_OK;
}

static int r_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st)
{
    (void)mnt;
    int n = lookup(path, 0, 0);
    if (n < 0) return VFS_ERR_NOTFOUND;
    kmemset(st, 0, sizeof *st);
    kstrncpy(st->name, n ? nodes[n].name : "", sizeof st->name);
    st->size = nodes[n].size;
    st->attrs = nodes[n].is_dir ? VFS_ATTR_DIR : 0;
    st->mdate = nodes[n].mdate;
    st->mtime = nodes[n].mtime;
    return VFS_OK;
}

static int r_unlink(vfs_mount_t *mnt, const char *path)
{
    (void)mnt;
    int n = lookup(path, 0, 0);
    if (n <= 0) return n == 0 ? VFS_ERR_BUSY : VFS_ERR_NOTFOUND;
    if (nodes[n].is_dir)
        for (int i = 1; i < RAMFS_NODES; i++)
            if (nodes[i].used && !nodes[i].unlinked && nodes[i].parent == n) return VFS_ERR_ISDIR;
    if (nodes[n].open) { nodes[n].unlinked = 1; return VFS_OK; }
    free_node(n);
    return VFS_OK;
}

static int r_rename(vfs_mount_t *mnt, const char *old, const char *nw)
{
    (void)mnt;
    int n = lookup(old, 0, 0);
    if (n <= 0) return VFS_ERR_NOTFOUND;
    int parent; const char *leaf;
    int t = lookup(nw, &parent, &leaf);
    if (t == n) return VFS_OK;
    if (t >= 0) {                                        /* replace a file */
        if (nodes[t].is_dir || nodes[n].is_dir) return VFS_ERR_INVAL;
        parent = nodes[t].parent;
        leaf = nodes[t].name;
        char keep[RAMFS_NAME];
        kstrncpy(keep, leaf, sizeof keep);
        r_unlink(mnt, nw);
        nodes[n].parent = parent;
        kstrncpy(nodes[n].name, keep, RAMFS_NAME);
        return VFS_OK;
    }
    if (parent < 0) return VFS_ERR_NOTFOUND;
    for (int p = parent; p; p = nodes[p].parent)         /* not into itself */
        if (p == n) return VFS_ERR_INVAL;
    nodes[n].parent = parent;
    uint32_t k = 0;
    while (leaf[k] && leaf[k] != '/' && leaf[k] != '\\' && k < RAMFS_NAME - 1) {
        nodes[n].name[k] = leaf[k];
        k++;
    }
    nodes[n].name[k] = 0;
    return VFS_OK;
}

static int r_mkdir(vfs_mount_t *mnt, const char *path)
{
    (void)mnt;
    int parent; const char *leaf;
    if (lookup(path, &parent, &leaf) >= 0) return VFS_ERR_INVAL;
    if (parent < 0) return VFS_ERR_NOTFOUND;
    int n = new_node(parent, leaf, 1);
    return n < 0 ? n : VFS_OK;
}

static int r_opendir(vfs_mount_t *mnt, const char *path, void **dpriv)
{
    (void)mnt;
    int n = lookup(path, 0, 0);
    if (n < 0) return VFS_ERR_NOTFOUND;
    if (!nodes[n].is_dir) return VFS_ERR_NOTDIR;
    for (int h = 0; h < (int)(sizeof handles / sizeof handles[0]); h++)
        if (!handles[h].used) {
            handles[h] = (rhandle_t){ .node = n, .used = 1, .dir_next = 1 };
            *dpriv = &handles[h];
            return VFS_OK;
        }
    return VFS_ERR_NOSPACE;
}

static int r_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent)
{
    (void)mnt;
    rhandle_t *h = dpriv;
    for (; h->dir_next < RAMFS_NODES; h->dir_next++) {
        rnode_t *n = &nodes[h->dir_next];
        if (n->used && !n->unlinked && n->parent == h->node) {
            kstrncpy(ent->name, n->name, sizeof ent->name);
            h->dir_next++;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOTFOUND;
}

static int r_closedir(vfs_mount_t *mnt, void *dpriv)
{
    (void)mnt;
    ((rhandle_t *)dpriv)->used = 0;
    return VFS_OK;
}

vfs_fs_t vfs_ramfs = {
    .name = "ramfs",
    .mount = r_mount, .unmount = r_unmount,
    .open = r_open, .close = r_close, .read = r_read, .write = r_write, .seek = r_seek,
    .stat = r_stat, .unlink = r_unlink, .rename = r_rename, .mkdir = r_mkdir,
    .opendir = r_opendir, .readdir = r_readdir, .closedir = r_closedir,
};

int ramfs_truncate(const char *path_in_mount, uint32_t size)
{
    int n = lookup(path_in_mount, 0, 0);
    if (n < 0) return VFS_ERR_NOTFOUND;
    if (nodes[n].is_dir) return VFS_ERR_ISDIR;
    if (size > MAX_PAGES * PAGE) return VFS_ERR_NOSPACE;
    if (size < nodes[n].size) truncate_to(&nodes[n], size);
    else nodes[n].size = size;                           /* holes read as zeros */
    return VFS_OK;
}
