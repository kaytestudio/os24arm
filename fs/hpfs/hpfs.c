/**
 * @file hpfs.c
 * @brief HPFS (OS/2 High Performance File System) VFS driver
 *
 * On-disk structures (512-byte sectors, little endian; layouts as in the
 * Linux HPFS driver, fs/hpfs/hpfs.h):
 *
 *   LSN 16  superblock  F995E849 FA53E9C5; +12 root fnode, +16 sectors
 *   fnode   F7E40AAE    one per file/dir: +12 name length, +13 first 15
 *                       name bytes, +28 parent, +55 dirflag (bit 0),
 *                       +56 B+ tree header, +64 8 extents / 12 pointers,
 *                       +160 file size
 *   anode   37E40AAE    B+ tree node: +12 header, +20 40 extents / 60
 *                       pointers
 *   dnode   77E40AAE    2 KB directory B-tree node: +4 first free, +8 bit
 *                       0 root, +12 up, +16 self, +20 directory entries
 *
 *   B+ tree header: [0] flags (0x80 internal), [5] nodes used. Leaf
 *   extent: file sector, length, disk sector. Internal pointer: file
 *   sector bound (subtree maps sectors below it), anode LSN.
 *
 *   Directory entry: +0 length, +2 flags (01 first = "..", 04 down
 *   pointer in the last 4 bytes, 08 last = end-of-dnode dummy), +3 DOS
 *   attributes, +4 fnode, +8 mtime (seconds since 1970), +12 size, +30
 *   name length, +31 name. Entries are sorted; a down pointer leads to
 *   the dnode with the names before that entry, so an in-order walk
 *   lists the directory.
 *
 * Lookups descend the dnode B-tree, comparing names upper-cased through
 * the volume's code page table (ASCII-only if it can't be loaded), and
 * fall back to a full in-order walk when the tree is damaged. Every
 * structure is checked (signatures, bounds, depth) before use.
 */

#include "hpfs.h"
#include "kio.h"

#define SB_LSN           16
#define SB_MAGIC0        0xF995E849u
#define SB_MAGIC1        0xFA53E9C5u
#define FNODE_MAGIC      0xF7E40AAEu
#define ANODE_MAGIC      0x37E40AAEu
#define DNODE_MAGIC      0x77E40AAEu
#define SP_MAGIC0        0xF9911849u           /* spareblock */
#define SP_MAGIC1        0xFA5229C5u
#define CPDIR_MAGIC      0x494521F7u           /* code page directory */
#define CPDATA_MAGIC     0x894521F7u           /* code page data */

#define DE_FIRST         0x01
#define DE_DOWN          0x04
#define DE_LAST          0x08
#define DE_HDR           31
#define DNODE_HDR        20

#define BP_INTERNAL      0x80

#define MAX_FILES        8
#define MAX_DIRS         8
#define MAX_DEPTH        16
#define MAX_STEPS        65536             /* walk guard against loops */
#define MAX_TREE_LEVELS  8

typedef struct {
    uint32_t fnode;
    uint32_t size;
    uint32_t mtime;
    uint8_t  attr;
    char     name[VFS_NAME_MAX + 1];
} hpfs_info_t;

typedef struct {
    int      depth;                         /* -1 = finished */
    uint32_t lsn[MAX_DEPTH];
    uint16_t off[MAX_DEPTH];
    uint8_t  descended[MAX_DEPTH];
    uint32_t steps;
    int      warned;
} dwalk_t;

typedef struct {
    hpfs_mount_t *m;                        /* NULL = free slot */
    uint32_t      size;
    uint32_t      pos;
    uint8_t       fnode[512];
    uint32_t      fnode_lsn;
    int           writable, append, changed;
    uint32_t      dir_fnode;                /* its directory, for the entry */
    char          name[VFS_NAME_MAX + 1];
} hpfs_file_t;

typedef struct {
    hpfs_mount_t *m;
    dwalk_t       w;
    uint32_t      dir_fnode;
    uint32_t      gen;                      /* m->dir_gen the walk is valid for */
    uint8_t       last[256];                /* last name returned (to resume) */
    uint32_t      lastlen;
    int           has_last;
} hpfs_dir_t;

static hpfs_file_t files[MAX_FILES];
static hpfs_dir_t  dirs[MAX_DIRS];

/* ── Helpers ──────────────────────────────────────────────────────────── */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Read sectors, honouring the hotfix map: a sector HPFS found bad was
 * copied to a spare, and every read of it must go to the spare instead
 * (as Linux's hpfs_search_hotfix_map does for metadata and data alike). */
static int rd(hpfs_mount_t *m, uint32_t lsn, uint32_t count, void *buf)
{
    if (lsn >= m->n_sectors || count > m->n_sectors - lsn) return -1;
    uint8_t *out = buf;
    while (count) {
        uint32_t run = count, to = 0;
        for (uint32_t i = 0; i < m->n_hotfix; i++) {
            uint32_t f = m->hotfix_from[i];
            if (f == lsn) { run = 1; to = m->hotfix_to[i]; break; }
            if (f > lsn && f - lsn < run) run = f - lsn;   /* stop before it */
        }
        if (to) {
            if (to >= m->n_sectors || blk_read(m->dev, to, 1, out) != 0) return -1;
        } else if (blk_read(m->dev, lsn, run, out) != 0) {
            return -1;
        }
        out += run * 512;
        lsn += run;
        count -= run;
    }
    return 0;
}

/* Upper case as HPFS does it: ASCII letters, and 80h..FFh through the
 * volume's code page table (identity if it has none we could load). */
static uint8_t upc(const hpfs_mount_t *m, uint8_t c)
{
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 32);
    if (c >= 0x80 && c != 0xFF && m->codepage) return m->upcase[c - 0x80];
    return c;
}

static int name_eq(const hpfs_mount_t *m, const uint8_t *name, uint32_t len,
                   const char *s, uint32_t slen)
{
    if (len != slen) return 0;
    for (uint32_t i = 0; i < len; i++)
        if (upc(m, name[i]) != upc(m, (uint8_t)s[i])) return 0;
    return 1;
}

/* HPFS collation: upper-cased bytes, then length (as Linux's
 * hpfs_compare_names). */
static int name_cmp(const hpfs_mount_t *m, const char *s, uint32_t slen,
                    const uint8_t *name, uint32_t len)
{
    uint32_t n = slen < len ? slen : len;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t a = upc(m, (uint8_t)s[i]), b = upc(m, name[i]);
        if (a != b) return a < b ? -1 : 1;
    }
    return slen < len ? -1 : slen > len ? 1 : 0;
}

/* Seconds since 1970 -> DOS date/time (0 before 1980). */
static void dos_datetime(uint32_t t, uint16_t *date, uint16_t *time)
{
    uint32_t days = t / 86400, secs = t % 86400;
    uint32_t y = 1970;
    for (;;) {
        uint32_t yd = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t mo = 0;
    for (; mo < 12; mo++) {
        uint32_t md = mdays[mo] + (mo == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
        if (days < md) break;
        days -= md;
    }
    if (y < 1980 || y > 2107) { *date = 0; *time = 0; return; }
    *date = (uint16_t)(((y - 1980) << 9) | ((mo + 1) << 5) | (days + 1));
    *time = (uint16_t)(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2));
}

/* ── Structures ───────────────────────────────────────────────────────── */
static int read_fnode(hpfs_mount_t *m, uint32_t lsn, uint8_t *fn)
{
    if (rd(m, lsn, 1, fn) != 0) return VFS_EIO;
    return le32(fn) == FNODE_MAGIC ? VFS_OK : VFS_EIO;
}

/* Directory fnode -> its root dnode LSN (first extent), 0 if not a dir. */
static uint32_t dir_root_dnode(hpfs_mount_t *m, uint32_t fnode_lsn)
{
    uint8_t fn[512];
    if (read_fnode(m, fnode_lsn, fn) != VFS_OK || !(fn[55] & 1)) return 0;
    if ((fn[56] & BP_INTERNAL) || fn[61] < 1) return 0;   /* leaf, 1 extent */
    return le32(fn + 72);                       /* external[0].disk_secno */
}

static const uint8_t *load_dnode(hpfs_mount_t *m, uint32_t lsn)
{
    if (m->dnode_lsn == lsn && lsn) return m->dnode;
    m->dnode_lsn = 0;
    if (lsn % 4 || rd(m, lsn, HPFS_DNODE_SIZE / 512, m->dnode) != 0) return 0;
    if (le32(m->dnode) != DNODE_MAGIC || le32(m->dnode + 16) != lsn) return 0;
    m->dnode_lsn = lsn;
    return m->dnode;
}

/* ── Directory walk (in order) ────────────────────────────────────────── */
static void walk_start(dwalk_t *w, uint32_t root)
{
    w->depth = 0;
    w->lsn[0] = root;
    w->off[0] = DNODE_HDR;
    w->descended[0] = 0;
    w->steps = 0;
    w->warned = 0;
}

/* Next directory entry (".." and end markers skipped), pointing into the
 * mount's dnode cache - valid until the next dnode load. NULL at the end.
 * A damaged dnode below the root is reported once and its subtree
 * skipped, so the rest of the directory still lists; a damaged root
 * dnode (or a loop) ends the walk. */
static const uint8_t *walk_next(hpfs_mount_t *m, dwalk_t *w)
{
    while (w->depth >= 0) {
        int t = w->depth;
        if (++w->steps > MAX_STEPS) goto damaged;
        const uint8_t *d = load_dnode(m, w->lsn[t]);
        if (!d) goto damaged;
        /* Entries end at first_free, not at the end of the 2 KB block. */
        uint32_t end = le32(d + 4), off = w->off[t];
        if (end > HPFS_DNODE_SIZE) end = HPFS_DNODE_SIZE;
        if (off < DNODE_HDR || off + DE_HDR + 1 > end) goto damaged;
        const uint8_t *p = d + off;
        uint32_t len = le16(p);
        if (len < DE_HDR + 1 || off + len > end || DE_HDR + p[30] > len) goto damaged;

        if ((p[2] & DE_DOWN) && !w->descended[t]) {
            /* the down pointer is the entry's last 4 bytes, after the name */
            if (len < DE_HDR + p[30] + 4u) goto damaged;
            w->descended[t] = 1;
            if (t + 1 >= MAX_DEPTH) {            /* too deep: skip subtree */
                if (!w->warned++)
                    kprintf("[HPFS]   %s: directory tree too deep (dnode %u), "
                            "some entries skipped\n", m->dev->name, w->lsn[t]);
                continue;                       /* emits this entry next */
            }
            w->depth = t + 1;
            w->lsn[t + 1] = le32(p + len - 4);
            w->off[t + 1] = DNODE_HDR;
            w->descended[t + 1] = 0;
            continue;
        }
        if (p[2] & DE_LAST) {                   /* end of this dnode */
            w->depth = t - 1;
            continue;
        }
        w->off[t] = (uint16_t)(off + len);
        w->descended[t] = 0;
        if (p[2] & DE_FIRST) continue;          /* ".." */
        return p;

    damaged:
        if (!w->warned++ && m->bad_dnode != w->lsn[t]) {
            m->bad_dnode = w->lsn[t];            /* every lookup walks it again */
            kprintf("[HPFS]   %s: damaged directory (dnode %u), some entries skipped\n",
                    m->dev->name, w->lsn[t]);
        }
        if (w->steps > MAX_STEPS || t == 0) break;
        w->depth = t - 1;                       /* back to the parent's entry */
    }
    w->depth = -1;
    return 0;
}

static void entry_info(const uint8_t *p, hpfs_info_t *out)
{
    out->fnode = le32(p + 4);
    out->mtime = le32(p + 8);
    out->size  = le32(p + 12);
    out->attr  = p[3];
    uint32_t n = p[30] > VFS_NAME_MAX ? VFS_NAME_MAX : p[30];
    for (uint32_t i = 0; i < n; i++) out->name[i] = (char)p[31 + i];
    out->name[n] = 0;
}

/* Look `s` up by descending the dnode B-tree: 1 found, 0 not there,
 * -1 if the tree is damaged (the caller then falls back to a full walk). */
static int btree_find(hpfs_mount_t *m, uint32_t root, const char *s, uint32_t slen,
                      hpfs_info_t *out, uint32_t *at_lsn, uint32_t *at_off)
{
    uint32_t lsn = root;
    for (int depth = 0; depth < MAX_DEPTH; depth++) {
        const uint8_t *d = load_dnode(m, lsn);
        if (!d) return -1;
        uint32_t end = le32(d + 4), off = DNODE_HDR;
        if (end > HPFS_DNODE_SIZE) end = HPFS_DNODE_SIZE;
        uint32_t down = 0;
        for (;;) {
            if (off + DE_HDR + 1 > end) return -1;
            const uint8_t *p = d + off;
            uint32_t len = le16(p);
            if (len < DE_HDR + 1 || off + len > end || DE_HDR + p[30] > len) return -1;
            int has_down = (p[2] & DE_DOWN) && len >= DE_HDR + p[30] + 4u;
            if (!(p[2] & DE_FIRST)) {
                int c = (p[2] & DE_LAST) ? -1 : name_cmp(m, s, slen, p + 31, p[30]);
                if (c == 0) {
                    entry_info(p, out);
                    if (at_lsn) { *at_lsn = lsn; *at_off = off; }
                    return 1;
                }
                if (c < 0) {                      /* belongs before this entry */
                    if (!has_down) return 0;
                    down = le32(p + len - 4);
                    break;
                }
            }
            if (p[2] & DE_LAST) return 0;
            off += len;
        }
        lsn = down;
    }
    return -1;
}

static int ascii(const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if ((uint8_t)s[i] >= 0x80) return 0;
    return 1;
}

/* One path component in directory fnode `dir`. B-tree descent first; a
 * full in-order walk if the tree is damaged, or for a non-ASCII name when
 * the volume's code page table couldn't be loaded (its order is then
 * unknown). */
static int lookup(hpfs_mount_t *m, uint32_t dir, const char *s, uint32_t slen,
                  hpfs_info_t *out)
{
    uint32_t root = dir_root_dnode(m, dir);
    if (!root) return VFS_EIO;
    int r = btree_find(m, root, s, slen, out, 0, 0);
    if (r == 1) return VFS_OK;
    if (r == 0 && (ascii(s, slen) || m->codepage)) return VFS_ENOENT;

    dwalk_t w;
    walk_start(&w, root);
    const uint8_t *p;
    while ((p = walk_next(m, &w)) != 0)
        if (name_eq(m, p + 31, p[30], s, slen)) { entry_info(p, out); return VFS_OK; }
    return VFS_ENOENT;
}

static void dir_info(uint32_t fnode, hpfs_info_t *out)
{
    out->fnode = fnode;
    out->size = out->mtime = 0;
    out->attr = VFS_ATTR_DIR;
}

/* Path ("/A/B", relative to the mount; "." and ".." allowed) -> entry info. */
static int resolve(hpfs_mount_t *m, const char *path, hpfs_info_t *out)
{
    dir_info(m->root_fnode, out);
    out->name[0] = '/'; out->name[1] = 0;

    const char *s = path;
    for (;;) {
        while (*s == '/' || *s == '\\') s++;
        if (!*s) return VFS_OK;
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        uint32_t n = (uint32_t)(e - s);
        if (!(out->attr & VFS_ATTR_DIR)) return VFS_ENOTDIR;

        if (n == 1 && s[0] == '.') {
            /* stays in this directory */
        } else if (n == 2 && s[0] == '.' && s[1] == '.') {
            uint8_t fn[512];                      /* parent = fnode's "up" */
            if (out->fnode != m->root_fnode) {
                if (read_fnode(m, out->fnode, fn) != VFS_OK) return VFS_EIO;
                dir_info(le32(fn + 28), out);
            }
            kstrncpy(out->name, "..", sizeof out->name);
        } else {
            int r = lookup(m, out->fnode, s, n, out);
            if (r != VFS_OK) return r;
        }
        s = e;
    }
}

/* File sector -> disk sector and how many sectors follow contiguously. */
static int map_sector(hpfs_mount_t *m, const uint8_t *fnode, uint32_t fsec,
                      uint32_t *disk, uint32_t *run)
{
    uint8_t node[512];
    const uint8_t *bt = fnode + 56;
    uint32_t cap_leaf = 8, cap_int = 12;
    for (int level = 0; level < MAX_TREE_LEVELS; level++) {
        uint32_t n = bt[5];
        const uint8_t *nodes = bt + 8;
        if (bt[0] & BP_INTERNAL) {
            if (n > cap_int) return -1;
            uint32_t i = 0;
            for (; i < n; i++)
                if (fsec < le32(nodes + 8 * i)) break;
            if (i == n) return -1;
            uint32_t a = le32(nodes + 8 * i + 4);
            if (rd(m, a, 1, node) != 0 || le32(node) != ANODE_MAGIC ||
                le32(node + 4) != a)                  /* anode's own LSN */
                return -1;
            bt = node + 12;
            cap_leaf = 40;
            cap_int = 60;
            continue;
        }
        if (n > cap_leaf) return -1;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t s = le32(nodes + 12 * i), len = le32(nodes + 12 * i + 4);
            if (fsec >= s && fsec - s < len) {
                *disk = le32(nodes + 12 * i + 8) + (fsec - s);
                *run  = len - (fsec - s);
                return 0;
            }
        }
        return -1;                              /* hole / not mapped */
    }
    return -1;
}

/* Code page upper-case table: spareblock (LSN 17) +32 code page directory
 * (magic 494521F7, entry 0 at +16: +8 data sector, +12 index) -> code
 * page data (magic 894521F7, +20 offsets[3]) -> entry: +2 code page
 * number, +6 128-byte map for 80h..FFh (layouts as Linux hpfs.h /
 * hpfs_load_code_page). Anything unexpected: leave ASCII-only. */
static void load_codepage(hpfs_mount_t *m)
{
    uint8_t b[512];
    m->codepage = 0;
    if (rd(m, 17, 1, b) != 0 || le32(b) != SP_MAGIC0 || le32(b + 4) != SP_MAGIC1) return;
    uint32_t dir = le32(b + 32);
    if (!dir || rd(m, dir, 1, b) != 0 || le32(b) != CPDIR_MAGIC || !le32(b + 4)) return;
    uint32_t data = le32(b + 16 + 8), idx = le16(b + 16 + 12);
    if (idx >= 3 || !data || rd(m, data, 1, b) != 0 || le32(b) != CPDATA_MAGIC) return;
    uint32_t off = le16(b + 20 + 2 * idx);
    if (off < 26 || off + 6 + 128 > 512) return;
    for (int i = 0; i < 128; i++) m->upcase[i] = b[off + 6 + i];
    uint16_t num = le16(b + off + 2);
    m->codepage = num ? num : 1;
}

/* Spareblock (LSN 17, layout as Linux hpfs.h): +8 flags (bit 0 dirty),
 * +12 hotfix map (4 sectors: n_spares "from" LSNs, then n_spares "to"),
 * +16 hotfixes used, +20 spares. Returns the dirty flag, -1 if absent. */
static int load_spareblock(hpfs_mount_t *m)
{
    uint8_t b[512];
    m->n_hotfix = 0;
    if (blk_read(m->dev, 17, 1, b) != 0 || le32(b) != SP_MAGIC0 || le32(b + 4) != SP_MAGIC1)
        return -1;
    uint32_t map = le32(b + 12), used = le32(b + 16), spares = le32(b + 20);
    if (used && map && spares <= 256 && used <= spares) {
        static uint8_t hm[2048];
        if (map < m->n_sectors - 4 && blk_read(m->dev, map, 4, hm) == 0) {
            for (uint32_t i = 0; i < used; i++) {
                m->hotfix_from[i] = le32(hm + 4 * i);
                m->hotfix_to[i]   = le32(hm + 4 * (spares + i));
            }
            m->n_hotfix = used;
        }
    } else if (used) {
        kprintf("[HPFS]   %s: invalid hotfix counts (%u of %u), ignored\n",
                m->dev->name, used, spares);
    }
    return b[8] & 1;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Writing
 *
 *  Only on a read-write mount (hpfs_mount_op: writable device, version 2,
 *  clean volume, bitmaps readable). The spareblock dirty bit is set
 *  before the first change and cleared at unmount, so a volume pulled or
 *  crashed mid-way mounts read-only next time and asks for CHKDSK.
 *
 *  Free space: one 2 KB bitmap per 8 MB band (bit set = free), listed
 *  from superblock +24; dnodes come from the directory band (superblock
 *  +48 size, +52/+56 first/last sector, +60 its bitmap: bit per dnode)
 *  and, when it is full, from free space on a 4-sector boundary.
 *
 *  Files: the extent list is rebuilt into the fnode (<= 8 extents) or
 *  into anodes (40 each, up to 12 -> 480 extents) whenever it changes.
 *  Directories: an entry goes into its leaf dnode when it fits; when it
 *  doesn't (or a removal would leave an internal hole) the directory's
 *  dnode tree is rebuilt, balanced, from its sorted entries.
 * ══════════════════════════════════════════════════════════════════════ */

#define BAND_SECTORS     16384
#define MAX_EXTENTS      480
#define RB_BYTES         262144            /* directory rebuild limits (deletes) */
#define RB_ITEMS         8192
#define RB_DNODES        4096

typedef struct { uint32_t fsec, len, disk; } extent_t;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Wall clock for timestamps: QEMU virt's PL031 via the kernel, if it
 * provides one; else a fixed time. */
extern uint32_t kernel_time_seconds(void) __attribute__((weak));
static uint32_t now(void)
{
    return kernel_time_seconds ? kernel_time_seconds() : 1767225600u; /* 2026-01-01 */
}

static int set_dirty(hpfs_mount_t *m, int dirty);

/* Write sectors (hotfix-aware like rd()). Any failure makes the mount
 * read-only rather than carry on with half-written structures. */
static int wr(hpfs_mount_t *m, uint32_t lsn, uint32_t count, const void *buf)
{
    if (!m->rw || lsn >= m->n_sectors || count > m->n_sectors - lsn) return -1;
    if (!m->marked_dirty && lsn != 17 && set_dirty(m, 1) != 0) return -1;
    if (m->dnode_lsn && lsn < m->dnode_lsn + 4 && lsn + count > m->dnode_lsn)
        m->dnode_lsn = 0;                     /* keep the dnode cache honest */
    const uint8_t *in = buf;
    while (count) {
        uint32_t run = count, to = 0;
        for (uint32_t i = 0; i < m->n_hotfix; i++) {
            uint32_t f = m->hotfix_from[i];
            if (f == lsn) { run = 1; to = m->hotfix_to[i]; break; }
            if (f > lsn && f - lsn < run) run = f - lsn;
        }
        if (blk_write(m->dev, to ? to : lsn, run, in) != 0) {
            kprintf("[HPFS]   %s: write failed at sector %u - now read-only\n",
                    m->dev->name, to ? to : lsn);
            m->rw = 0;
            return -1;
        }
        in += run * 512;
        lsn += run;
        count -= run;
        m->wcount += run;
    }
    return 0;
}

static int set_dirty(hpfs_mount_t *m, int dirty)
{
    uint8_t b[512];
    if (rd(m, 17, 1, b) != 0) return -1;
    b[8] = (uint8_t)((b[8] & ~1u) | (dirty ? 1 : 0));
    m->marked_dirty = dirty;                  /* before wr(): no recursion */
    if (wr(m, 17, 1, b) != 0) return -1;
    return 0;
}

/* ── Free-space bitmaps ───────────────────────────────────────────────── */
static int band_load(hpfs_mount_t *m, uint32_t b)
{
    if (m->band == b) return 0;
    if (b >= m->nbands || rd(m, m->bitmaps[b], 4, m->bandbuf) != 0) return -1;
    m->band = b;
    return 0;
}

static int sector_free(hpfs_mount_t *m, uint32_t s)
{
    if (s >= m->n_sectors || band_load(m, s / BAND_SECTORS) != 0) return 0;
    uint32_t bit = s % BAND_SECTORS;
    return (m->bandbuf[bit / 8] >> (bit % 8)) & 1;
}

/* Mark [s, s+n) used or free; the run stays within one band. */
static int mark(hpfs_mount_t *m, uint32_t s, uint32_t n, int used)
{
    while (n) {
        if (band_load(m, s / BAND_SECTORS) != 0) return -1;
        uint32_t bit = s % BAND_SECTORS, k = BAND_SECTORS - bit;
        if (k > n) k = n;
        for (uint32_t i = 0; i < k; i++, bit++) {
            if (used) m->bandbuf[bit / 8] &= (uint8_t)~(1u << (bit % 8));
            else      m->bandbuf[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
        if (wr(m, m->bitmaps[m->band], 4, m->bandbuf) != 0) return -1;
        s += k;
        n -= k;
    }
    return 0;
}

/* First free run of up to `want` sectors at or after `hint` (wrapping),
 * starting on an `align` boundary; exactly `want` when align > 1.
 * Returns its start (0 = none) and the length in *got. */
static uint32_t alloc_run(hpfs_mount_t *m, uint32_t hint, uint32_t want, uint32_t align,
                          uint32_t *got)
{
    if (!want) return 0;
    if (hint >= m->n_sectors) hint = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t s = pass ? 0 : hint, end = pass ? hint : m->n_sectors;
        while (s < end) {
            if (s % align) { s += align - s % align; continue; }
            if (!sector_free(m, s)) { s++; continue; }
            uint32_t n = 1;
            while (n < want && s + n < m->n_sectors && (s + n) % BAND_SECTORS &&
                   sector_free(m, s + n))
                n++;
            if (align > 1 && n < want) { s += n; continue; }
            if (mark(m, s, n, 1) != 0) return 0;
            *got = n;
            m->alloc_hint = s + n;
            return s;
        }
    }
    return 0;
}

static uint32_t alloc_sector(hpfs_mount_t *m, uint32_t hint)
{
    uint32_t got;
    return alloc_run(m, hint, 1, 1, &got);
}

static uint32_t alloc_dnode(hpfs_mount_t *m)
{
    for (uint32_t i = 0; i < m->dband_n; i++)
        if (m->dband_bm[i / 8] & (1u << (i % 8))) {
            m->dband_bm[i / 8] &= (uint8_t)~(1u << (i % 8));
            if (wr(m, m->dband_bitmap, 4, m->dband_bm) != 0) return 0;
            return m->dband_start + 4 * i;
        }
    uint32_t got;
    return alloc_run(m, m->alloc_hint, 4, 4, &got);
}

static void free_dnode(hpfs_mount_t *m, uint32_t lsn)
{
    if (m->dband_n && lsn >= m->dband_start && lsn + 3 <= m->dband_end) {
        uint32_t i = (lsn - m->dband_start) / 4;
        m->dband_bm[i / 8] |= (uint8_t)(1u << (i % 8));
        wr(m, m->dband_bitmap, 4, m->dband_bm);
    } else {
        mark(m, lsn, 4, 0);
    }
}

/* ── Extent trees ─────────────────────────────────────────────────────── */
/* All leaf extents of a file, in order, and the anodes holding them. */
static int collect_extents(hpfs_mount_t *m, const uint8_t *fnode, extent_t *ext,
                           uint32_t *next, uint32_t *anodes, uint32_t *nanodes)
{
    *next = 0;
    *nanodes = 0;
    const uint8_t *bt = fnode + 56;
    if (!(bt[0] & BP_INTERNAL)) {                     /* extents in the fnode */
        for (uint32_t i = 0; i < bt[5] && i < 8; i++) {
            const uint8_t *x = bt + 8 + 12 * i;
            ext[(*next)++] = (extent_t){ le32(x), le32(x + 4), le32(x + 8) };
        }
        return 0;
    }
    /* Internal: walk the anode tree depth-first with an explicit stack. */
    uint32_t stack[64], sp = 0;
    for (int i = (int)bt[5] - 1; i >= 0 && i < 12; i--) stack[sp++] = le32(bt + 8 + 8 * i + 4);
    uint8_t node[512];
    while (sp) {
        uint32_t a = stack[--sp];
        if (*nanodes >= 64 || rd(m, a, 1, node) != 0 || le32(node) != ANODE_MAGIC ||
            le32(node + 4) != a)
            return -1;
        anodes[(*nanodes)++] = a;
        const uint8_t *nb = node + 12;
        if (nb[0] & BP_INTERNAL) {
            if (nb[5] > 60 || sp + nb[5] > 64) return -1;
            for (int i = (int)nb[5] - 1; i >= 0; i--) stack[sp++] = le32(nb + 8 + 8 * i + 4);
        } else {
            for (uint32_t i = 0; i < nb[5] && i < 40; i++) {
                if (*next >= MAX_EXTENTS) return -1;
                const uint8_t *x = nb + 8 + 12 * i;
                ext[(*next)++] = (extent_t){ le32(x), le32(x + 4), le32(x + 8) };
            }
        }
    }
    return 0;
}

static void bplus_header(uint8_t *bt, uint8_t flags, uint32_t used, uint32_t cap, uint32_t size)
{
    bt[0] = flags; bt[1] = bt[2] = bt[3] = 0;
    bt[4] = (uint8_t)(cap - used);
    bt[5] = (uint8_t)used;
    put16(bt + 6, (uint16_t)(8 + size * used));
}

/* Write a file's extent list back: into the fnode, or anodes + pointers.
 * The file's existing anode sectors are reused in place (new ones would
 * otherwise land right after the data and stop it growing contiguously);
 * any left over are freed once the new tree is on disk. */
static int write_extents(hpfs_mount_t *m, uint32_t fnode_lsn, uint8_t *fnode,
                         const extent_t *ext, uint32_t n, const uint32_t *old, uint32_t nold)
{
    if (n > MAX_EXTENTS) return VFS_ENOSPC;
    uint8_t *bt = fnode + 56;
    kmemset(bt, 0, 8 + 96);
    if (n <= 8) {
        bplus_header(bt, 0, n, 8, 12);
        for (uint32_t i = 0; i < n; i++) {
            put32(bt + 8 + 12 * i, ext[i].fsec);
            put32(bt + 8 + 12 * i + 4, ext[i].len);
            put32(bt + 8 + 12 * i + 8, ext[i].disk);
        }
    } else {
        uint32_t leaves = (n + 39) / 40;
        bplus_header(bt, BP_INTERNAL, leaves, 12, 8);
        uint8_t node[512];
        for (uint32_t l = 0; l < leaves; l++) {
            uint32_t a = l < nold ? old[l] : alloc_sector(m, m->dband_end + 1);
            if (!a) return VFS_ENOSPC;
            uint32_t first = l * 40, cnt = n - first > 40 ? 40 : n - first;
            kmemset(node, 0, sizeof node);
            put32(node, ANODE_MAGIC);
            put32(node + 4, a);
            put32(node + 8, fnode_lsn);
            bplus_header(node + 12, 0x20 /* parent is fnode */, cnt, 40, 12);
            for (uint32_t i = 0; i < cnt; i++) {
                put32(node + 20 + 12 * i, ext[first + i].fsec);
                put32(node + 20 + 12 * i + 4, ext[first + i].len);
                put32(node + 20 + 12 * i + 8, ext[first + i].disk);
            }
            if (wr(m, a, 1, node) != 0) return VFS_EIO;
            put32(bt + 8 + 8 * l, l + 1 < leaves ? ext[first + cnt].fsec : 0xFFFFFFFFu);
            put32(bt + 8 + 8 * l + 4, a);
        }
    }
    if (wr(m, fnode_lsn, 1, fnode) != 0) return VFS_EIO;
    uint32_t reused = n <= 8 ? 0 : (n + 39) / 40;
    for (uint32_t i = reused; i < nold; i++) mark(m, old[i], 1, 0);
    return VFS_OK;
}

/* Free extended-attribute / ACL storage hung off an fnode: either a run
 * of sectors, or (anode flag) an anode tree of extents. */
static void free_ext_storage(hpfs_mount_t *m, uint32_t secno, uint32_t size, int anode)
{
    if (!secno || !size) return;
    if (!anode) { mark(m, secno, (size + 511) / 512, 0); return; }
    uint32_t stack[64], sp = 0;
    uint8_t node[512];
    stack[sp++] = secno;
    while (sp) {
        uint32_t a = stack[--sp];
        if (rd(m, a, 1, node) != 0 || le32(node) != ANODE_MAGIC || le32(node + 4) != a) return;
        const uint8_t *bt = node + 12;
        if (bt[0] & BP_INTERNAL) {
            for (uint32_t i = 0; i < bt[5] && i < 60 && sp < 64; i++)
                stack[sp++] = le32(bt + 8 + 8 * i + 4);
        } else {
            for (uint32_t i = 0; i < bt[5] && i < 40; i++)
                mark(m, le32(bt + 8 + 12 * i + 8), le32(bt + 8 + 12 * i + 4), 0);
        }
        mark(m, a, 1, 0);
    }
}

/* An fnode's EAs (+44 size, +48 sector, flag +54 bit 1 = anode) and ACL
 * (+32 size, +36 sector, +42 anode). */
static void free_fnode_extras(hpfs_mount_t *m, const uint8_t *fn)
{
    free_ext_storage(m, le32(fn + 48), le32(fn + 44), fn[54] & 2);
    free_ext_storage(m, le32(fn + 36), le32(fn + 32), fn[42]);
}

/* Free a file's data and anodes, leaving an empty extent list. */
static int truncate_fnode(hpfs_mount_t *m, uint32_t fnode_lsn, uint8_t *fnode)
{
    static extent_t ext[MAX_EXTENTS];
    uint32_t n, an[64], nan;
    if (collect_extents(m, fnode, ext, &n, an, &nan) != 0) return VFS_EIO;
    put32(fnode + 160, 0);
    int r = write_extents(m, fnode_lsn, fnode, ext, 0, an, nan);
    if (r != VFS_OK) return r;
    for (uint32_t i = 0; i < n; i++) mark(m, ext[i].disk, ext[i].len, 0);
    return VFS_OK;
}

/* Make a file at least `need` sectors long (new sectors are not cleared). */
static int grow_fnode(hpfs_mount_t *m, uint32_t fnode_lsn, uint8_t *fnode, uint32_t need)
{
    static extent_t ext[MAX_EXTENTS];
    uint32_t n, an[64], nan, have = 0;
    if (collect_extents(m, fnode, ext, &n, an, &nan) != 0) return VFS_EIO;
    for (uint32_t i = 0; i < n; i++) have += ext[i].len;
    if (have >= need) return VFS_OK;
    int r = VFS_OK;
    while (have < need) {
        uint32_t hint = n ? ext[n - 1].disk + ext[n - 1].len : fnode_lsn + 1, got = 0;
        uint32_t s = alloc_run(m, hint, need - have, 1, &got);
        if (!s) { r = VFS_ENOSPC; break; }
        if (n && s == ext[n - 1].disk + ext[n - 1].len) {
            ext[n - 1].len += got;                    /* contiguous: extend */
        } else if (n < MAX_EXTENTS) {
            ext[n++] = (extent_t){ have, got, s };
        } else {
            mark(m, s, got, 0);
            r = VFS_ENOSPC;
            break;
        }
        have += got;
    }
    int w = write_extents(m, fnode_lsn, fnode, ext, n, an, nan);
    return w != VFS_OK ? w : r;
}

/* ── Fnodes and directory entries ─────────────────────────────────────── */
static void init_fnode(uint8_t *f, const char *name, uint32_t nlen, uint32_t up, int dir)
{
    kmemset(f, 0, 512);
    put32(f, FNODE_MAGIC);
    f[12] = (uint8_t)nlen;
    for (uint32_t i = 0; i < nlen && i < 15; i++) f[13 + i] = (uint8_t)name[i];
    put32(f + 28, up);
    f[55] = dir ? 1 : 0;
    bplus_header(f + 56, 0, 0, 8, 12);
    put16(f + 184, 0xC4);                         /* ea_offs */
}

static int valid_name(const char *s, uint32_t n)
{
    if (n == 0 || n > 254) return 0;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            return 0;
    }
    return !(s[n - 1] == ' ' || s[n - 1] == '.');  /* OS/2 drops these */
}

static int is_83(const char *s, uint32_t n)
{
    uint32_t base = 0, ext = 0, dots = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') { if (++dots > 1) return 0; continue; }
        if (c >= 'a' && c <= 'z') return 0;
        if (c == ' ' || (uint8_t)c >= 0x80) return 0;
        if (dots) ext++; else base++;
    }
    return base >= 1 && base <= 8 && ext <= 3;
}

/* Build a directory entry (no down pointer); returns its length. */
static uint32_t make_dirent(uint8_t *e, const char *name, uint32_t nlen, uint8_t flags,
                            uint8_t attr, uint32_t fnode, uint32_t size, uint32_t t)
{
    uint32_t len = (DE_HDR + nlen + 3) & ~3u;
    kmemset(e, 0, len);
    put16(e, (uint16_t)len);
    e[2] = flags;
    e[3] = attr;
    put32(e + 4, fnode);
    put32(e + 8, t);                              /* written */
    put32(e + 12, size);
    put32(e + 16, t);                             /* read */
    put32(e + 20, t);                             /* created */
    e[30] = (uint8_t)nlen;
    for (uint32_t i = 0; i < nlen; i++) e[31 + i] = (uint8_t)name[i];
    return len;
}

/* Split "/A/B/C" into the directory part (resolved to its fnode) and the
 * last component. */
static int resolve_parent(hpfs_mount_t *m, const char *path, uint32_t *dir_fnode,
                          const char **name, uint32_t *nlen)
{
    const char *end = path + kstrlen(path);
    while (end > path && (end[-1] == '/' || end[-1] == '\\')) end--;
    const char *base = end;
    while (base > path && base[-1] != '/' && base[-1] != '\\') base--;
    if (base == end) return VFS_EINVAL;           /* the root itself */
    char dir[VFS_PATH_MAX];
    uint32_t dl = (uint32_t)(base - path);
    if (dl >= sizeof dir) return VFS_EINVAL;
    for (uint32_t i = 0; i < dl; i++) dir[i] = path[i];
    dir[dl] = 0;
    hpfs_info_t info;
    int r = resolve(m, dir, &info);
    if (r != VFS_OK) return r;
    if (!(info.attr & VFS_ATTR_DIR)) return VFS_ENOTDIR;
    *dir_fnode = info.fnode;
    *name = base;
    *nlen = (uint32_t)(end - base);
    return VFS_OK;
}

/* Where an entry lives: its dnode (copied into `d`) and offset there. */
static int locate(hpfs_mount_t *m, uint32_t dir_fnode, const char *s, uint32_t n,
                  uint32_t *lsn, uint32_t *off, uint8_t *d)
{
    uint32_t root = dir_root_dnode(m, dir_fnode);
    if (!root) return VFS_EIO;
    hpfs_info_t info;
    int r = btree_find(m, root, s, n, &info, lsn, off);   /* B-tree descent */
    if (r == 1 && load_dnode(m, *lsn)) {
        kmemcpy(d, m->dnode, HPFS_DNODE_SIZE);
        return VFS_OK;
    }
    if (r == 0 && (ascii(s, n) || m->codepage)) return VFS_ENOENT;
    dwalk_t w;                                      /* damaged tree: full walk */
    walk_start(&w, root);
    const uint8_t *p;
    while ((p = walk_next(m, &w)) != 0)
        if (name_eq(m, p + 31, p[30], s, n)) {
            *lsn = w.lsn[w.depth];
            *off = (uint32_t)(p - m->dnode);
            kmemcpy(d, m->dnode, HPFS_DNODE_SIZE);
            return VFS_OK;
        }
    return VFS_ENOENT;
}

/* ── Directory rebuild ────────────────────────────────────────────────── */
typedef struct { uint32_t off, len, child; } rb_item_t;   /* entry in rb_ents */
static uint8_t   rb_ents[RB_BYTES];
static uint32_t  rb_used;
static rb_item_t rb_a[RB_ITEMS], rb_b[RB_ITEMS];
static uint8_t   rb_node[HPFS_DNODE_SIZE];

static uint32_t rb_size(const rb_item_t *it) { return it->len + (it->child ? 4 : 0); }

/* One dnode's worth of entries (each without its down pointer, plus the
 * child it should point down to) and the "last" entry's child. */
typedef struct { const uint8_t *p; uint32_t len, child; } dn_item_t;

static int node_write(hpfs_mount_t *m, uint32_t lsn, const dn_item_t *it, uint32_t n,
                      uint32_t tail, int root, uint32_t up)
{
    uint8_t *d = rb_node;
    kmemset(d, 0, HPFS_DNODE_SIZE);
    put32(d, DNODE_MAGIC);
    d[8] = root ? 1 : 0;
    put32(d + 12, up);
    put32(d + 16, lsn);
    /* Everything, including the end entry, must fit exactly. */
    uint32_t need = DNODE_HDR + 32 + (tail ? 4 : 0);
    for (uint32_t i = 0; i < n; i++) need += it[i].len + (it[i].child ? 4 : 0);
    if (need > HPFS_DNODE_SIZE) return VFS_EIO;
    uint32_t off = DNODE_HDR;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = it[i].len;
        kmemcpy(d + off, it[i].p, len);
        d[off + 2] &= (uint8_t)~DE_DOWN;
        if (it[i].child) {
            d[off + 2] |= DE_DOWN;
            put32(d + off + len, it[i].child);
            len += 4;
        }
        put16(d + off, (uint16_t)len);
        off += len;
    }
    uint32_t last = make_dirent(d + off, "\xFF", 1, DE_LAST, 0, 0, 0, 0);
    if (tail) {
        d[off + 2] |= DE_DOWN;
        put32(d + off + last, tail);
        last += 4;
        put16(d + off, (uint16_t)last);
    }
    off += last;
    put32(d + 4, off);
    return wr(m, lsn, 4, d) == 0 ? VFS_OK : VFS_EIO;
}

static int rb_write(hpfs_mount_t *m, const rb_item_t *it, uint32_t n, uint32_t tail,
                    int root, uint32_t up, uint32_t *out)
{
    static dn_item_t conv[128];
    if (n > 128) return VFS_EIO;
    uint32_t lsn = alloc_dnode(m);
    if (!lsn) return VFS_ENOSPC;
    for (uint32_t i = 0; i < n; i++)
        conv[i] = (dn_item_t){ rb_ents + it[i].off, it[i].len, it[i].child };
    int r = node_write(m, lsn, conv, n, tail, root, up);
    if (r == VFS_OK) *out = lsn;
    return r;
}

/* Point every dnode's "up" at its parent dnode (the root's stays at the
 * directory fnode). */
static int rb_fix_up(hpfs_mount_t *m, uint32_t root)
{
    static uint32_t stack[RB_DNODES];
    uint32_t sp = 0;
    stack[sp++] = root;
    while (sp) {
        uint32_t parent = stack[--sp];
        static uint8_t pd[HPFS_DNODE_SIZE], cd[HPFS_DNODE_SIZE];
        if (rd(m, parent, 4, pd) != 0) return VFS_EIO;
        uint32_t end = le32(pd + 4), off = DNODE_HDR;
        while (off + DE_HDR < end) {
            uint32_t len = le16(pd + off);
            if (len < DE_HDR + 1) return VFS_EIO;
            if (pd[off + 2] & DE_DOWN) {
                uint32_t c = le32(pd + off + len - 4);
                if (rd(m, c, 4, cd) != 0) return VFS_EIO;
                put32(cd + 12, parent);
                if (wr(m, c, 4, cd) != 0) return VFS_EIO;
                if (sp >= RB_DNODES) return VFS_ENOSPC;
                stack[sp++] = c;
            }
            if (pd[off + 2] & DE_LAST) break;
            off += len;
        }
    }
    return VFS_OK;
}

/* Every dnode of a directory tree (to free after a rebuild). */
static int rb_collect(hpfs_mount_t *m, uint32_t root, uint32_t *list, uint32_t *n)
{
    uint32_t sp = 0;
    static uint32_t stack[RB_DNODES];
    static uint8_t d[HPFS_DNODE_SIZE];
    *n = 0;
    stack[sp++] = root;
    while (sp) {
        uint32_t lsn = stack[--sp];
        if (*n >= RB_DNODES || rd(m, lsn, 4, d) != 0 || le32(d) != DNODE_MAGIC) return -1;
        list[(*n)++] = lsn;
        uint32_t end = le32(d + 4), off = DNODE_HDR;
        if (end > HPFS_DNODE_SIZE) return -1;
        while (off + DE_HDR < end) {
            uint32_t len = le16(d + off);
            if (len < DE_HDR + 1 || off + len > end) return -1;
            if (d[off + 2] & DE_DOWN) {
                if (sp >= RB_DNODES) return -1;
                stack[sp++] = le32(d + off + len - 4);
            }
            if (d[off + 2] & DE_LAST) break;
            off += len;
        }
    }
    return 0;
}

/* Rebuild directory `dir_fnode`'s dnode tree from its entries, adding
 * `add` (a dirent without down pointer) and/or dropping `drop`. */
static int dir_rebuild(hpfs_mount_t *m, uint32_t dir_fnode, const uint8_t *add,
                       const char *drop, uint32_t droplen)
{
    uint8_t fn[512];
    if (read_fnode(m, dir_fnode, fn) != VFS_OK || !(fn[55] & 1)) return VFS_EIO;
    uint32_t old_root = le32(fn + 72);
    static uint32_t old[RB_DNODES];
    uint32_t nold;
    if (rb_collect(m, old_root, old, &nold) != 0) return VFS_EIO;

    /* Sorted items: "..", then the entries in order with `add` merged in. */
    rb_used = 0;
    uint32_t n = 0;
    uint32_t up = le32(fn + 28);
    rb_a[n++] = (rb_item_t){ 0, make_dirent(rb_ents, "\x01\x01", 2, DE_FIRST, VFS_ATTR_DIR,
                                            up, 0, now()), 0 };
    rb_used = rb_a[0].len;
    int added = !add;
    dwalk_t w;
    walk_start(&w, old_root);
    const uint8_t *p;
    for (;;) {
        p = walk_next(m, &w);
        if (!added && (!p || name_cmp(m, (const char *)add + 31, add[30], p + 31, p[30]) < 0)) {
            uint32_t len = le16(add);
            if (n >= RB_ITEMS || rb_used + len > RB_BYTES) return VFS_ENOSPC;
            kmemcpy(rb_ents + rb_used, add, len);
            rb_a[n++] = (rb_item_t){ rb_used, len, 0 };
            rb_used += len;
            added = 1;
        }
        if (!p) break;
        if (drop && name_eq(m, p + 31, p[30], drop, droplen)) continue;
        uint32_t len = (DE_HDR + p[30] + 3) & ~3u;     /* without down ptr */
        if (n >= RB_ITEMS || rb_used + len > RB_BYTES) return VFS_ENOSPC;
        kmemcpy(rb_ents + rb_used, p, len);
        put16(rb_ents + rb_used, (uint16_t)len);
        rb_ents[rb_used + 2] &= (uint8_t)~DE_DOWN;
        rb_a[n++] = (rb_item_t){ rb_used, len, 0 };
        rb_used += len;
    }
    if (w.warned) return VFS_EIO;                    /* don't rebuild a damaged tree */

    /* Bottom-up: split each level into dnodes ~3/4 full, promoting one
     * item per boundary (its child = the dnode before it) to the next. */
    const uint32_t cap = HPFS_DNODE_SIZE - DNODE_HDR - 36, target = cap * 3 / 4;
    rb_item_t *cur = rb_a, *nxt = rb_b;
    uint32_t tail = 0, root = 0;
    for (;;) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < n; i++) total += rb_size(&cur[i]);
        if (total <= cap) {
            int r = rb_write(m, cur, n, tail, 1, dir_fnode, &root);
            if (r != VFS_OK) return r;
            break;
        }
        uint32_t nn = 0, i = 0, new_tail = 0;
        while (i < n) {
            uint32_t start = i, used = 0;
            while (i < n && (i == start || used + rb_size(&cur[i]) <= target))
                used += rb_size(&cur[i++]);
            if (i >= n - 1) {                         /* last group (absorbs a lone tail item) */
                if (i == n - 1) i++;
                int r = rb_write(m, cur + start, i - start, tail, 0, 0, &new_tail);
                if (r != VFS_OK) return r;
                break;
            }
            uint32_t lsn;
            int r = rb_write(m, cur + start, i - start, cur[i].child, 0, 0, &lsn);
            if (r != VFS_OK) return r;
            nxt[nn] = cur[i];
            nxt[nn++].child = lsn;
            i++;
        }
        tail = new_tail;
        rb_item_t *t = cur; cur = nxt; nxt = t;
        n = nn;
    }
    if (rb_fix_up(m, root) != VFS_OK) return VFS_EIO;
    put32(fn + 72, root);                            /* the new tree */
    if (wr(m, dir_fnode, 1, fn) != 0) return VFS_EIO;
    for (uint32_t i = 0; i < nold; i++) free_dnode(m, old[i]);
    m->dnode_lsn = 0;
    m->dir_gen++;
    return VFS_OK;
}

/* ── Directory insert / remove / update ───────────────────────────────── */
/* ── B-tree insertion ─────────────────────────────────────────────────── */
static uint8_t   sp_node[MAX_DEPTH][HPFS_DNODE_SIZE];  /* a level's dnode, as read */
static dn_item_t sp_items[MAX_DEPTH][80];

static int set_up(hpfs_mount_t *m, uint32_t child, uint32_t up)
{
    static uint8_t c[HPFS_DNODE_SIZE];
    if (rd(m, child, 4, c) != 0 || le32(c) != DNODE_MAGIC) return VFS_EIO;
    put32(c + 12, up);
    return wr(m, child, 4, c) == 0 ? VFS_OK : VFS_EIO;
}

/* Put entry `e` (elen bytes, no down pointer; `child` = the subtree before
 * it, if any) into dnode path[depth] at its place in the order. `after`
 * (from a split below) becomes the down pointer of the entry that follows
 * it. A full dnode splits around its middle: the left half stays where it
 * is, the right half moves to a new dnode, and the median goes up to the
 * parent - or to a new root. path[0] is the root, path[depth] the target. */
static int dnode_put(hpfs_mount_t *m, uint32_t dir_fnode, const uint32_t *path, int depth,
                     const uint8_t *e, uint32_t elen, uint32_t child, uint32_t after)
{
    uint8_t *d = sp_node[depth];
    dn_item_t *it = sp_items[depth];
    uint32_t lsn = path[depth];
    if (rd(m, lsn, 4, d) != 0 || le32(d) != DNODE_MAGIC || le32(d + 16) != lsn) return VFS_EIO;
    uint32_t end = le32(d + 4), off = DNODE_HDR, n = 0, pos = ~0u, tail = 0;
    if (end > HPFS_DNODE_SIZE) return VFS_EIO;
    for (;;) {
        if (off + DE_HDR + 1 > end) return VFS_EIO;
        uint32_t len = le16(d + off);
        uint8_t fl = d[off + 2];
        if (len < DE_HDR + 1 || off + len > end || DE_HDR + d[off + 30] > len) return VFS_EIO;
        int down = (fl & DE_DOWN) != 0;
        if (down && len < DE_HDR + d[off + 30] + 4u) return VFS_EIO;
        if (pos == ~0u && ((fl & DE_LAST) ||
                           (!(fl & DE_FIRST) &&
                            name_cmp(m, (const char *)e + 31, e[30], d + off + 31, d[off + 30]) < 0)))
            pos = n;                                   /* the new entry goes here */
        uint32_t dp = down ? le32(d + off + len - 4) : 0;
        if (fl & DE_LAST) { tail = dp; break; }
        if (n >= 78) return VFS_EIO;
        it[n++] = (dn_item_t){ d + off, down ? len - 4 : len, dp };
        off += len;
    }
    if (after) {
        if (pos < n) it[pos].child = after;
        else         tail = after;
    }
    for (uint32_t i = n; i > pos; i--) it[i] = it[i - 1];
    it[pos] = (dn_item_t){ e, elen, child };
    n++;
    m->dir_gen++;

    int root = d[8] & 1;
    uint32_t up = le32(d + 12), total = DNODE_HDR + 32 + (tail ? 4 : 0);
    for (uint32_t i = 0; i < n; i++) total += it[i].len + (it[i].child ? 4 : 0);
    if (total <= HPFS_DNODE_SIZE) return node_write(m, lsn, it, n, tail, root, up);

    /* Split: k = the median, by bytes, with entries on both sides. */
    uint32_t half = (total - DNODE_HDR) / 2, acc = 0, k = 0;
    while (k + 2 < n && acc + it[k].len + (it[k].child ? 4 : 0) < half) {
        acc += it[k].len + (it[k].child ? 4 : 0);
        k++;
    }
    if (k == 0) k = 1;
    dn_item_t med = it[k];
    uint32_t right = alloc_dnode(m), newroot = 0;
    if (!right) return VFS_ENOSPC;
    if (root && !(newroot = alloc_dnode(m))) { free_dnode(m, right); return VFS_ENOSPC; }
    uint32_t parent = root ? newroot : up;
    int r = node_write(m, right, it + k + 1, n - k - 1, tail, 0, parent);
    for (uint32_t i = k + 1; r == VFS_OK && i < n; i++)
        if (it[i].child) r = set_up(m, it[i].child, right);
    if (r == VFS_OK && tail) r = set_up(m, tail, right);
    if (r == VFS_OK) r = node_write(m, lsn, it, k, med.child, 0, parent);
    if (r != VFS_OK) return r;
    if (root) {                                        /* the tree grows a level */
        dn_item_t ri = { med.p, med.len, lsn };
        if ((r = node_write(m, newroot, &ri, 1, right, 1, dir_fnode)) != VFS_OK) return r;
        uint8_t fn[512];
        if (read_fnode(m, dir_fnode, fn) != VFS_OK) return VFS_EIO;
        put32(fn + 72, newroot);
        return wr(m, dir_fnode, 1, fn) == 0 ? VFS_OK : VFS_EIO;
    }
    if (depth == 0) return VFS_EIO;                    /* non-root dnode with no parent */
    return dnode_put(m, dir_fnode, path, depth - 1, med.p, med.len, lsn, right);
}

/* Insert `e` (no down pointer) into the directory's B-tree. */
static int dir_insert(hpfs_mount_t *m, uint32_t dir_fnode, const uint8_t *e)
{
    static uint8_t d[HPFS_DNODE_SIZE];
    uint32_t path[MAX_DEPTH];
    int depth = 0;
    if (!(path[0] = dir_root_dnode(m, dir_fnode))) return VFS_EIO;
    for (;;) {                                         /* down to the leaf it belongs in */
        uint32_t lsn = path[depth];
        if (rd(m, lsn, 4, d) != 0 || le32(d) != DNODE_MAGIC || le32(d + 16) != lsn)
            return VFS_EIO;
        uint32_t end = le32(d + 4), off = DNODE_HDR, down = 0;
        if (end > HPFS_DNODE_SIZE) return VFS_EIO;
        for (;;) {
            if (off + DE_HDR + 1 > end) return VFS_EIO;
            uint32_t len = le16(d + off);
            uint8_t fl = d[off + 2];
            if (len < DE_HDR + 1 || off + len > end) return VFS_EIO;
            if ((fl & DE_LAST) ||
                (!(fl & DE_FIRST) &&
                 name_cmp(m, (const char *)e + 31, e[30], d + off + 31, d[off + 30]) < 0)) {
                if (fl & DE_DOWN) down = le32(d + off + len - 4);
                break;
            }
            off += len;
        }
        if (!down) break;
        if (depth + 1 >= MAX_DEPTH) return VFS_EIO;
        path[++depth] = down;
    }
    return dnode_put(m, dir_fnode, path, depth, e, le16(e), 0, 0);
}

static int dir_remove(hpfs_mount_t *m, uint32_t dir_fnode, const char *s, uint32_t n)
{
    static uint8_t d[HPFS_DNODE_SIZE];
    uint32_t lsn, off;
    int r = locate(m, dir_fnode, s, n, &lsn, &off, d);
    if (r != VFS_OK) return r;
    uint32_t len = le16(d + off), end = le32(d + 4);
    /* In place only for a leaf entry whose dnode keeps an entry (or is the
     * root); otherwise rebuild, which also rebalances. */
    int others = 0;
    for (uint32_t o = DNODE_HDR; o + DE_HDR < end; o += le16(d + o)) {
        if (d[o + 2] & DE_LAST) break;
        if (o != off) others++;
        if (le16(d + o) < DE_HDR + 1) return VFS_EIO;
    }
    if ((d[off + 2] & DE_DOWN) || (!others && !(d[8] & 1)))
        return dir_rebuild(m, dir_fnode, 0, s, n);
    m->dir_gen++;
    for (uint32_t k = off; k + len < end; k++) d[k] = d[k + len];
    put32(d + 4, end - len);
    kmemset(d + end - len, 0, len);
    return wr(m, lsn, 4, d) == 0 ? VFS_OK : VFS_EIO;
}

/* Refresh a file's entry: size, and written/read times. */
static int dir_update(hpfs_mount_t *m, uint32_t dir_fnode, const char *s, uint32_t n,
                      uint32_t size)
{
    static uint8_t d[HPFS_DNODE_SIZE];
    uint32_t lsn, off;
    int r = locate(m, dir_fnode, s, n, &lsn, &off, d);
    if (r != VFS_OK) return r;
    uint32_t t = now();
    put32(d + off + 8, t);
    put32(d + off + 12, size);
    put32(d + off + 16, t);
    return wr(m, lsn, 4, d) == 0 ? VFS_OK : VFS_EIO;
}

/* Prepare a mount for writing: bitmap list, dir band; 0 if OK. */
static int write_setup(hpfs_mount_t *m, const uint8_t *sb)
{
    static uint8_t list[2048];
    m->band = ~0u;
    m->nbands = (m->n_sectors + BAND_SECTORS - 1) / BAND_SECTORS;
    if (m->nbands > 512 || rd(m, le32(sb + 24), 4, list) != 0) return -1;
    for (uint32_t b = 0; b < m->nbands; b++) {
        m->bitmaps[b] = le32(list + 4 * b);
        if (!m->bitmaps[b] || m->bitmaps[b] + 4 > m->n_sectors) return -1;
    }
    m->dband_n = le32(sb + 48) / 4;
    m->dband_start = le32(sb + 52);
    m->dband_end = le32(sb + 56);
    m->dband_bitmap = le32(sb + 60);
    if (m->dband_n > 16384) return -1;
    if (m->dband_n && (rd(m, m->dband_bitmap, 4, m->dband_bm) != 0 ||
                       m->dband_end < m->dband_start ||
                       m->dband_start + 4 * m->dband_n > m->dband_end + 1))
        return -1;
    if (band_load(m, 0) != 0) return -1;
    m->alloc_hint = m->dband_end + 1;
    return 0;
}

/* ── CHKDSK ───────────────────────────────────────────────────────────── */
int hpfs_is_dirty(hpfs_mount_t *m)
{
    uint8_t b[512];
    if (blk_read(m->dev, 17, 1, b) != 0 || le32(b) != SP_MAGIC0) return 0;
    return b[8] & 1;
}

/* Is [s, s+n) marked in use? A dnode in the directory band by its own
 * bitmap (bit set = free), anything else by the band bitmaps. */
static int chk_used(hpfs_mount_t *m, uint32_t s, uint32_t n, int dnode)
{
    if (s >= m->n_sectors || n > m->n_sectors - s) return 0;
    if (dnode && m->dband_n && s >= m->dband_start && s + 3 <= m->dband_end) {
        uint32_t i = (s - m->dband_start) / 4;
        return !(m->dband_bm[i / 8] & (1u << (i % 8)));
    }
    for (uint32_t k = 0; k < n; k++)
        if (sector_free(m, s + k)) return 0;
    return 1;
}

#define CHK_MAX_DIRS 4096

int hpfs_check(hpfs_mount_t *m, int fix, hpfs_check_t *r)
{
    static uint32_t queue[CHK_MAX_DIRS], dn[RB_DNODES], anodes[64];
    static extent_t ext[MAX_EXTENTS];
    static uint8_t sb[512], fn[512], d[HPFS_DNODE_SIZE];
    char name[VFS_NAME_MAX + 1];
    kmemset(r, 0, sizeof *r);
    r->was_dirty = hpfs_is_dirty(m);
    if (rd(m, SB_LSN, 1, sb) != 0 || le32(sb) != SB_MAGIC0) return -1;
    if (write_setup(m, sb) != 0) {                    /* (only reads) */
        kprintf("[CHKDSK] %s: the free-space bitmaps can't be read\n", m->dev->name);
        return -1;
    }
#define PROBLEM(...) do { if (r->problems++ < 12) kprintf("[CHKDSK] " __VA_ARGS__); } while (0)

    uint32_t head = 0, tail = 0;
    queue[tail++] = m->root_fnode;
    if (read_fnode(m, m->root_fnode, fn) != VFS_OK) PROBLEM("the root directory's fnode is damaged\n");
    else if (!chk_used(m, m->root_fnode, 1, 0)) PROBLEM("the root fnode is marked free\n");

    while (head < tail) {
        uint32_t dir = queue[head++];
        r->dirs++;
        uint32_t root = dir_root_dnode(m, dir), n = 0;
        if (!root) { PROBLEM("directory fnode %u has no directory tree\n", dir); continue; }
        if (rb_collect(m, root, dn, &n) != 0) {
            PROBLEM("the directory tree under fnode %u is damaged\n", dir);
            continue;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (rd(m, dn[i], 4, d) != 0 || le32(d + 16) != dn[i])
                PROBLEM("dnode %u: wrong sector number in it\n", dn[i]);
            else if (!chk_used(m, dn[i], 4, 1))
                PROBLEM("dnode %u is in use but marked free\n", dn[i]);
        }
        dwalk_t w;
        walk_start(&w, root);
        const uint8_t *p;
        while ((p = walk_next(m, &w)) != 0) {
            uint32_t f = le32(p + 4), size = le32(p + 12);
            uint32_t nl = p[30] > VFS_NAME_MAX ? VFS_NAME_MAX : p[30];
            for (uint32_t i = 0; i < nl; i++) name[i] = (char)p[31 + i];
            name[nl] = 0;
            if (read_fnode(m, f, fn) != VFS_OK) { PROBLEM("%s: its fnode %u is damaged\n", name, f); continue; }
            if (!chk_used(m, f, 1, 0)) PROBLEM("%s: its fnode %u is marked free\n", name, f);
            if (le32(fn + 28) != dir) PROBLEM("%s: its fnode points at the wrong directory\n", name);
            if (fn[55] & 1) {                         /* a directory */
                if (tail < CHK_MAX_DIRS) queue[tail++] = f;
                else PROBLEM("too many directories to check\n");
                continue;
            }
            r->files++;
            r->bytes += size;
            uint32_t next, na;
            if (collect_extents(m, fn, ext, &next, anodes, &na) != 0) {
                PROBLEM("%s: its allocation tree is damaged\n", name);
                continue;
            }
            for (uint32_t i = 0; i < na; i++)
                if (!chk_used(m, anodes[i], 1, 0)) PROBLEM("%s: anode %u is marked free\n", name, anodes[i]);
            unsigned long long have = 0;
            for (uint32_t i = 0; i < next; i++) {
                if (!chk_used(m, ext[i].disk, ext[i].len, 0))
                    PROBLEM("%s: sectors %u+%u are in use but marked free\n", name, ext[i].disk, ext[i].len);
                have += ext[i].len;
            }
            if (have * 512 < size) PROBLEM("%s: shorter on disk than its size\n", name);
        }
        if (w.warned) PROBLEM("a directory (fnode %u) has damaged entries\n", dir);
    }
#undef PROBLEM

    if (fix && !r->problems && r->was_dirty && !(m->dev->flags & BLKDEV_READONLY)) {
        uint8_t b[512];
        if (blk_read(m->dev, 17, 1, b) == 0 && le32(b) == SP_MAGIC0) {
            b[8] &= (uint8_t)~1u;
            if (blk_write(m->dev, 17, 1, b) == 0) r->cleared = 1;
        }
        /* read-only only because it was dirty: writable now */
        if (r->cleared && !m->rw && sb[8] == 2 && m->codepage && !m->inconsistent) {
            m->marked_dirty = 0;
            m->rw = 1;
            r->now_rw = 1;
        }
    }
    return (int)r->problems;
}

/* ── VFS operations ───────────────────────────────────────────────────── */
static int hpfs_mount_op(vfs_mount_t *mnt)
{
    hpfs_mount_t *m = mnt->priv;
    uint8_t sb[512];
    if (!m || !m->dev || m->dev->sector_size != 512) return VFS_EINVAL;
    m->n_sectors = (uint32_t)(m->dev->num_sectors > 0xFFFFFFFFLL ? 0xFFFFFFFFu
                                                                 : m->dev->num_sectors);
    m->dnode_lsn = 0;
    m->n_hotfix  = 0;
    m->bad_dnode = 0;
    if (rd(m, SB_LSN, 1, sb) != 0) return VFS_EIO;
    if (le32(sb) != SB_MAGIC0 || le32(sb + 4) != SB_MAGIC1) return VFS_EINVAL;
    uint32_t n = le32(sb + 16);
    if (n && n < m->n_sectors) m->n_sectors = n;
    m->root_fnode = le32(sb + 12);
    int dirty = load_spareblock(m);                  /* before any other read */
    /* The volume label lives in the boot block's extended BPB (the
     * superblock's volume_name field is unused). */
    uint8_t bb[512];
    m->label[0] = 0;
    if (rd(m, 0, 1, bb) == 0 && bb[38] == 0x29) {
        int n = 11;
        while (n > 0 && (bb[43 + n - 1] == ' ' || !bb[43 + n - 1])) n--;
        for (int i = 0; i < n; i++)
            m->label[i] = (bb[43 + i] >= 32 && bb[43 + i] < 127) ? (char)bb[43 + i] : '?';
        m->label[n] = 0;
    }
    if (!dir_root_dnode(m, m->root_fnode)) return VFS_EIO;
    load_codepage(m);
    /* Read-write only when it's safe: writable device, HPFS v2, a clean
     * volume (a dirty one needs CHKDSK first) and usable bitmaps. */
    m->rw = 0;
    m->marked_dirty = 0;
    m->inconsistent = 0;
    m->wcount = 0;
    m->dir_gen = 0;
    const char *ro_why = 0;
    if (m->dev->flags & BLKDEV_READONLY)  ro_why = "device is read-only";
    else if (sb[8] != 2)                  ro_why = "unsupported HPFS version";
    else if (dirty != 0)                  ro_why = dirty == 1 ? "volume is dirty" : "no spareblock";
    else if (!m->codepage)                ro_why = "code page table unreadable (names can't be sorted)";
    else if (write_setup(m, sb) != 0)     ro_why = "bitmaps unreadable";
    else                                  m->rw = 1;
    char cp[48] = "no code page table (ASCII case folding only)";
    if (m->codepage) {
        kstrncpy(cp, "code page ", sizeof cp);
        uint32_t n = m->codepage, k = 10, digits = 1;
        while (n / k && digits < 5) { k *= 10; digits++; }
        for (uint32_t i = 0; i < digits; i++) {
            k /= 10;
            cp[10 + i] = (char)('0' + (n / k) % 10);
        }
        cp[10 + digits] = 0;
    }
    kprintf("[HPFS]   %s: HPFS v%u, %u sectors, root fnode %u%s%s, %s, %s%s%s\n", m->dev->name,
            sb[8], m->n_sectors, m->root_fnode, m->label[0] ? ", label " : "", m->label, cp,
            m->rw ? "read-write" : "read-only", ro_why ? ": " : "", ro_why ? ro_why : "");
    if (m->n_hotfix)
        kprintf("[HPFS]   %s: %u bad sector(s) hotfixed to spares\n", m->dev->name, m->n_hotfix);
    if (dirty == 1)
        kprintf("[HPFS]   %s: volume was not shut down cleanly (CHKDSK /F checks it)\n",
                m->dev->name);
    return VFS_OK;
}

static int hpfs_unmount_op(vfs_mount_t *mnt)
{
    hpfs_mount_t *m = mnt->priv;
    for (int i = 0; i < MAX_FILES; i++) if (files[i].m == m) files[i].m = 0;
    for (int i = 0; i < MAX_DIRS; i++)  if (dirs[i].m == m)  dirs[i].m = 0;
    if (m->rw && m->marked_dirty && !m->inconsistent)
        set_dirty(m, 0);                             /* cleanly closed */
    else if (m->inconsistent)
        kprintf("[HPFS]   %s: left marked dirty - run CHKDSK on OS/2\n", m->dev->name);
    m->rw = 0;
    return VFS_OK;
}

/* Another handle on the same fnode? (a writer excludes everyone else) */
static int fnode_busy(hpfs_mount_t *m, uint32_t fnode, int writer)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].m == m && files[i].fnode_lsn == fnode && (writer || files[i].writable))
            return 1;
    return 0;
}

static int hpfs_open_do(vfs_mount_t *mnt, const char *path, int flags, void **fpriv)
{
    hpfs_mount_t *m = mnt->priv;
    int writer = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;
    if (writer && !m->rw) return VFS_EROFS;
    hpfs_file_t *f = 0;
    for (int i = 0; i < MAX_FILES; i++) if (!files[i].m) { f = &files[i]; break; }
    if (!f) return VFS_ENOSPC;

    uint32_t dir = 0, nlen = 0;
    const char *name = 0;
    if (writer) {
        int r = resolve_parent(m, path, &dir, &name, &nlen);
        if (r != VFS_OK) return r;
    }
    hpfs_info_t info;
    int r = resolve(m, path, &info);
    if (r == VFS_ENOENT && (flags & O_CREAT)) {
        /* New file: fnode near its directory, then the directory entry. */
        if (!valid_name(name, nlen)) return VFS_EINVAL;
        uint32_t fl = alloc_sector(m, dir + 1);
        if (!fl) return VFS_ENOSPC;
        init_fnode(f->fnode, name, nlen, dir, 0);
        static uint8_t e[DE_HDR + 256 + 4];
        make_dirent(e, name, nlen, 0, VFS_ATTR_ARCHIVE | (is_83(name, nlen) ? 0 : 0x40),
                    fl, 0, now());
        if (wr(m, fl, 1, f->fnode) != 0 || (r = dir_insert(m, dir, e)) != VFS_OK) {
            mark(m, fl, 1, 0);
            return r == VFS_OK ? VFS_EIO : r;
        }
        info.fnode = fl;
        info.attr = VFS_ATTR_ARCHIVE;
    } else if (r != VFS_OK) {
        return r;
    } else {
        if (info.attr & VFS_ATTR_DIR) return VFS_EISDIR;
        if (writer && (info.attr & VFS_ATTR_RDONLY)) return VFS_ERR_ISDIR; /* access denied */
        if (read_fnode(m, info.fnode, f->fnode) != VFS_OK) return VFS_EIO;
    }
    if (fnode_busy(m, info.fnode, writer)) return VFS_ERR_BUSY;

    f->fnode_lsn = info.fnode;
    f->size      = le32(f->fnode + 160);
    f->pos       = 0;
    f->writable  = writer;
    f->append    = (flags & O_APPEND) != 0;
    f->changed   = 0;
    if (writer) {
        f->dir_fnode = dir;
        for (uint32_t i = 0; i < nlen; i++) f->name[i] = name[i];
        f->name[nlen] = 0;
        if ((flags & O_TRUNC) && f->size) {
            if ((r = truncate_fnode(m, f->fnode_lsn, f->fnode)) != VFS_OK) return r;
            f->size = 0;
            f->changed = 1;
        }
    }
    f->m   = m;
    *fpriv = f;
    return VFS_OK;
}

static int hpfs_close_do(vfs_mount_t *mnt, void *fpriv)
{
    (void)mnt;
    hpfs_file_t *f = fpriv;
    int r = VFS_OK;
    if (f->m && f->writable && f->changed && f->m->rw)   /* size + times in its entry */
        r = dir_update(f->m, f->dir_fnode, f->name, kstrlen(f->name), f->size);
    f->m = 0;
    f->fnode_lsn = 0;
    return r;
}

static int hpfs_read(vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len,
                     uint32_t *actual)
{
    hpfs_mount_t *m = mnt->priv;
    hpfs_file_t *f = fpriv;
    uint8_t *out = buf, sec[512];
    uint32_t done = 0;
    if (f->pos >= f->size) len = 0;
    else if (len > f->size - f->pos) len = f->size - f->pos;

    while (done < len) {
        uint32_t fsec = f->pos / 512, off = f->pos % 512, disk, run;
        if (map_sector(m, f->fnode, fsec, &disk, &run) != 0) {
            if (actual) *actual = done;
            return done ? VFS_OK : VFS_EIO;
        }
        uint32_t left = len - done;
        if (off == 0 && left >= 512) {          /* whole sectors, direct */
            uint32_t n = left / 512;
            if (n > run) n = run;
            if (n > 64) n = 64;
            if (rd(m, disk, n, out + done) != 0) return VFS_EIO;
            done += n * 512;
            f->pos += n * 512;
        } else {                                /* partial sector */
            if (rd(m, disk, 1, sec) != 0) return VFS_EIO;
            uint32_t n = 512 - off;
            if (n > left) n = left;
            kmemcpy(out + done, sec + off, n);
            done += n;
            f->pos += n;
        }
    }
    if (actual) *actual = done;
    return VFS_OK;
}

static int hpfs_write_do(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len,
                      uint32_t *actual)
{
    hpfs_mount_t *m = mnt->priv;
    hpfs_file_t *f = fpriv;
    if (actual) *actual = 0;
    if (!f->writable || !m->rw) return VFS_EROFS;
    if (f->append) f->pos = f->size;
    if (!len) return VFS_OK;
    if ((int64_t)f->pos + len > 0xFFFFFFFFLL) return VFS_ENOSPC;

    /* Allocation: sectors up to the new end. Sectors that weren't part of
     * the file's data must never show stale disk contents: those wholly
     * before the write (a seek past the end) are zeroed, and partial
     * sectors past the old end start from zeros. */
    uint32_t old_size = f->size, end = f->pos + len;
    uint32_t old_secs = (old_size + 511) / 512, need = (end + 511) / 512;
    if (need > old_secs) {
        int r = grow_fnode(m, f->fnode_lsn, f->fnode, need);
        if (r != VFS_OK) return r;
    }
    static uint8_t sec[512];
    uint32_t disk, run;
    if (f->pos > old_size && old_size % 512 && f->pos / 512 > old_size / 512) {
        /* the old last sector's bytes past the old end are now inside */
        if (map_sector(m, f->fnode, old_size / 512, &disk, &run) != 0 || rd(m, disk, 1, sec) != 0)
            return VFS_EIO;
        kmemset(sec + old_size % 512, 0, 512 - old_size % 512);
        if (wr(m, disk, 1, sec) != 0) return VFS_EIO;
    }
    for (uint32_t s = old_secs; s < f->pos / 512; s++) {         /* the gap */
        kmemset(sec, 0, 512);
        if (map_sector(m, f->fnode, s, &disk, &run) != 0 || wr(m, disk, 1, sec) != 0)
            return VFS_EIO;
    }
    const uint8_t *in = buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t s = f->pos / 512, off = f->pos % 512, left = len - done;
        if (map_sector(m, f->fnode, s, &disk, &run) != 0) return VFS_EIO;
        if (off == 0 && left >= 512) {                           /* whole sectors */
            uint32_t n = left / 512;
            if (n > run) n = run;
            if (n > 64) n = 64;
            if (wr(m, disk, n, in + done) != 0) return VFS_EIO;
            done += n * 512;
            f->pos += n * 512;
        } else {                                                 /* partial */
            if ((int64_t)s * 512 < (int64_t)old_size) {
                if (rd(m, disk, 1, sec) != 0) return VFS_EIO;
                if (s == old_size / 512)                         /* tail of old data */
                    kmemset(sec + old_size % 512, 0, 512 - old_size % 512);
            } else {
                kmemset(sec, 0, 512);
            }
            uint32_t n = 512 - off;
            if (n > left) n = left;
            kmemcpy(sec + off, in + done, n);
            if (wr(m, disk, 1, sec) != 0) return VFS_EIO;
            done += n;
            f->pos += n;
        }
        if (actual) *actual = done;
    }
    if (f->pos > f->size) {
        f->size = f->pos;
        put32(f->fnode + 160, f->size);
        if (wr(m, f->fnode_lsn, 1, f->fnode) != 0) return VFS_EIO;
    }
    f->changed = 1;
    return VFS_OK;
}

static int hpfs_seek(vfs_mount_t *mnt, void *fpriv, int32_t offset, int whence,
                     uint32_t *newpos)
{
    (void)mnt;
    hpfs_file_t *f = fpriv;
    int64_t base = whence == SEEK_CUR ? f->pos : whence == SEEK_END ? f->size : 0;
    int64_t p = base + offset;
    if (p < 0 || p > 0xFFFFFFFFLL) return VFS_EINVAL;
    /* A writer may seek past the end (the gap reads as zeros once written);
     * a reader stops at it. */
    f->pos = (!f->writable && p > f->size) ? f->size : (uint32_t)p;
    if (newpos) *newpos = f->pos;
    return VFS_OK;
}

static int hpfs_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st)
{
    hpfs_info_t info;
    int r = resolve(mnt->priv, path, &info);
    if (r != VFS_OK) return r;
    kstrncpy(st->name, info.name, sizeof st->name);
    st->size  = (info.attr & VFS_ATTR_DIR) ? 0 : info.size;
    st->attrs = info.attr & 0x37;               /* drop not-8.3 etc. */
    dos_datetime(info.mtime, &st->mdate, &st->mtime);
    return VFS_OK;
}

/* Delete a file, or an empty directory (RMDIR comes here too). */
static int hpfs_unlink_do(vfs_mount_t *mnt, const char *path)
{
    hpfs_mount_t *m = mnt->priv;
    if (!m->rw) return VFS_EROFS;
    uint32_t dir, nlen;
    const char *name;
    int r = resolve_parent(m, path, &dir, &name, &nlen);
    if (r != VFS_OK) return r;
    hpfs_info_t info;
    if ((r = lookup(m, dir, name, nlen, &info)) != VFS_OK) return r;
    if (fnode_busy(m, info.fnode, 1)) return VFS_ERR_BUSY;
    if (info.attr & VFS_ATTR_RDONLY) return VFS_ERR_ISDIR;           /* access denied */
    for (int i = 0; i < MAX_DIRS; i++)                /* being listed? */
        if (dirs[i].m == m && (info.attr & VFS_ATTR_DIR) && dirs[i].dir_fnode == info.fnode)
            return VFS_ERR_BUSY;

    uint8_t fn[512];
    if (read_fnode(m, info.fnode, fn) != VFS_OK) return VFS_EIO;
    uint32_t root = 0;
    if (info.attr & VFS_ATTR_DIR) {
        if (!(root = dir_root_dnode(m, info.fnode))) return VFS_EIO;
        dwalk_t w;
        walk_start(&w, root);
        if (walk_next(m, &w)) return VFS_ERR_ISDIR;   /* not empty: access denied */
        static uint32_t dn[RB_DNODES];
        uint32_t n;
        if (rb_collect(m, root, dn, &n) != 0) return VFS_EIO;
        if ((r = dir_remove(m, dir, name, nlen)) != VFS_OK) return r;
        for (uint32_t i = 0; i < n; i++) free_dnode(m, dn[i]);
    } else {
        if ((r = dir_remove(m, dir, name, nlen)) != VFS_OK) return r;
        if ((r = truncate_fnode(m, info.fnode, fn)) != VFS_OK) return r;
    }
    free_fnode_extras(m, fn);                     /* EAs (.LONGNAME, WPS data), ACL */
    return mark(m, info.fnode, 1, 0) == 0 ? VFS_OK : VFS_EIO;
}

/* Rename and/or move (within this volume). */
static int hpfs_rename_do(vfs_mount_t *mnt, const char *old, const char *nw)
{
    hpfs_mount_t *m = mnt->priv;
    if (!m->rw) return VFS_EROFS;
    uint32_t od, nd, ol, nl;
    const char *on, *nn;
    int r = resolve_parent(m, old, &od, &on, &ol);
    if (r != VFS_OK) return r;
    if ((r = resolve_parent(m, nw, &nd, &nn, &nl)) != VFS_OK) return r;
    if (!valid_name(nn, nl)) return VFS_EINVAL;
    hpfs_info_t info, other;
    if ((r = lookup(m, od, on, ol, &info)) != VFS_OK) return r;
    /* (an open file may be renamed - as on Unix, where programs write a
     * temporary file and rename it into place: its handles follow below) */
    int same = lookup(m, nd, nn, nl, &other) == VFS_OK;
    if (same && other.fnode != info.fnode) return VFS_EINVAL;         /* exists */
    if (info.attr & VFS_ATTR_DIR) {                /* not into itself */
        uint32_t a = nd;
        for (int k = 0; k < 64 && a != m->root_fnode; k++) {
            if (a == info.fnode) return VFS_EINVAL;
            uint8_t fn[512];
            if (read_fnode(m, a, fn) != VFS_OK) return VFS_EIO;
            a = le32(fn + 28);
        }
    }
    /* Find the old entry's full fields, then insert the new name first. */
    static uint8_t d[HPFS_DNODE_SIZE], e[DE_HDR + 256 + 4];
    uint32_t lsn, off;
    if ((r = locate(m, od, on, ol, &lsn, &off, d)) != VFS_OK) return r;
    uint8_t attr = (uint8_t)((d[off + 3] & ~0x40) | (is_83(nn, nl) ? 0 : 0x40));
    make_dirent(e, nn, nl, 0, attr, info.fnode, le32(d + off + 12), le32(d + off + 8));
    put32(e + 16, le32(d + off + 16));
    put32(e + 20, le32(d + off + 20));
    if (same) {
        /* Only the case changes: same length, same place in the
         * (case-insensitive) order - rewrite the name in place. */
        for (uint32_t i = 0; i < nl; i++) d[off + 31 + i] = (uint8_t)nn[i];
        d[off + 3] = attr;
        if (wr(m, lsn, 4, d) != 0) return VFS_EIO;
    } else {
        if ((r = dir_insert(m, nd, e)) != VFS_OK) return r;
        if ((r = dir_remove(m, od, on, ol)) != VFS_OK) return r;
    }
    /* The fnode's own copy of the name and its parent; a directory's ".." */
    uint8_t fn[512];
    if (read_fnode(m, info.fnode, fn) != VFS_OK) return VFS_EIO;
    fn[12] = (uint8_t)nl;
    kmemset(fn + 13, 0, 15);
    for (uint32_t i = 0; i < nl && i < 15; i++) fn[13 + i] = (uint8_t)nn[i];
    put32(fn + 28, nd);
    if (wr(m, info.fnode, 1, fn) != 0) return VFS_EIO;
    /* open handles on it update their entry (size, time) at close by
     * directory and name: the new ones */
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].m == m && files[i].fnode_lsn == info.fnode) {
            files[i].dir_fnode = nd;
            uint32_t k = 0;
            for (; k < nl && k < VFS_NAME_MAX; k++) files[i].name[k] = nn[k];
            files[i].name[k] = 0;
        }
    if ((info.attr & VFS_ATTR_DIR) && od != nd) {
        uint32_t root = dir_root_dnode(m, info.fnode);
        static uint8_t rdn[HPFS_DNODE_SIZE];
        /* ".." is the first entry of the leftmost dnode */
        uint32_t lsn2 = root;
        for (int k = 0; k < MAX_DEPTH && lsn2; k++) {
            if (rd(m, lsn2, 4, rdn) != 0 || le32(rdn) != DNODE_MAGIC) return VFS_EIO;
            if (rdn[DNODE_HDR + 2] & DE_FIRST) {
                put32(rdn + DNODE_HDR + 4, nd);
                if (wr(m, lsn2, 4, rdn) != 0) return VFS_EIO;
                break;
            }
            uint32_t l0 = le16(rdn + DNODE_HDR);
            lsn2 = (rdn[DNODE_HDR + 2] & DE_DOWN) ? le32(rdn + DNODE_HDR + l0 - 4) : 0;
        }
    }
    return VFS_OK;
}

static int hpfs_mkdir_do(vfs_mount_t *mnt, const char *path)
{
    hpfs_mount_t *m = mnt->priv;
    if (!m->rw) return VFS_EROFS;
    uint32_t dir, nlen;
    const char *name;
    int r = resolve_parent(m, path, &dir, &name, &nlen);
    if (r != VFS_OK) return r;
    if (!valid_name(name, nlen)) return VFS_EINVAL;
    hpfs_info_t info;
    if (lookup(m, dir, name, nlen, &info) == VFS_OK) return VFS_EINVAL;   /* exists */

    uint32_t fl = alloc_sector(m, dir + 1), dn = alloc_dnode(m);
    if (!fl || !dn) {
        if (fl) mark(m, fl, 1, 0);
        if (dn) free_dnode(m, dn);
        return VFS_ENOSPC;
    }
    uint32_t t = now();
    static uint8_t d[HPFS_DNODE_SIZE], e[DE_HDR + 256 + 4];
    kmemset(d, 0, sizeof d);
    put32(d, DNODE_MAGIC);
    d[8] = 1;                                      /* root dnode */
    put32(d + 12, fl);
    put32(d + 16, dn);
    uint32_t off = DNODE_HDR;
    off += make_dirent(d + off, "\x01\x01", 2, DE_FIRST, VFS_ATTR_DIR, dir, 0, t);
    off += make_dirent(d + off, "\xFF", 1, DE_LAST, 0, 0, 0, 0);
    put32(d + 4, off);
    uint8_t fn[512];
    init_fnode(fn, name, nlen, dir, 1);
    bplus_header(fn + 56, 0, 1, 8, 12);
    put32(fn + 64, 0);
    put32(fn + 68, 4);
    put32(fn + 72, dn);
    make_dirent(e, name, nlen, 0, VFS_ATTR_DIR | (is_83(name, nlen) ? 0 : 0x40), fl, 0, t);
    if (wr(m, dn, 4, d) != 0 || wr(m, fl, 1, fn) != 0) return VFS_EIO;
    if ((r = dir_insert(m, dir, e)) != VFS_OK) {
        mark(m, fl, 1, 0);
        free_dnode(m, dn);
        return r;
    }
    return VFS_OK;
}

static int hpfs_opendir(vfs_mount_t *mnt, const char *path, void **dpriv)
{
    hpfs_mount_t *m = mnt->priv;
    hpfs_dir_t *d = 0;
    for (int i = 0; i < MAX_DIRS; i++) if (!dirs[i].m) { d = &dirs[i]; break; }
    if (!d) return VFS_ENOSPC;

    hpfs_info_t info;
    int r = resolve(m, path, &info);
    if (r != VFS_OK) return r;
    if (!(info.attr & VFS_ATTR_DIR)) return VFS_ENOTDIR;
    uint32_t root = dir_root_dnode(m, info.fnode);
    if (!root) return VFS_EIO;
    walk_start(&d->w, root);
    d->m = m;
    d->dir_fnode = info.fnode;
    d->gen = m->dir_gen;
    d->has_last = 0;
    *dpriv = d;
    return VFS_OK;
}

static int hpfs_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent)
{
    (void)mnt;
    hpfs_dir_t *d = dpriv;
    if (!d || !d->m) return VFS_ENOENT;
    hpfs_mount_t *m = d->m;
    int skip = 0;
    if (d->gen != m->dir_gen) {
        /* The directory may have changed under us (DEL while listing):
         * start over and skip up to the last name returned. */
        uint32_t root = dir_root_dnode(m, d->dir_fnode);
        if (!root) return VFS_ENOENT;
        walk_start(&d->w, root);
        d->gen = m->dir_gen;
        skip = d->has_last;
    }
    const uint8_t *p;
    while ((p = walk_next(m, &d->w)) != 0) {
        if (skip && name_cmp(m, (const char *)d->last, d->lastlen, p + 31, p[30]) >= 0)
            continue;
        break;
    }
    if (!p) return VFS_ENOENT;
    hpfs_info_t info;
    entry_info(p, &info);
    kmemcpy(d->last, p + 31, p[30]);
    d->lastlen = p[30];
    d->has_last = 1;
    kstrncpy(ent->name, info.name, sizeof ent->name);
    return VFS_OK;
}

static int hpfs_closedir(vfs_mount_t *mnt, void *dpriv)
{
    (void)mnt;
    ((hpfs_dir_t *)dpriv)->m = 0;
    return VFS_OK;
}

/* An operation that fails after it has written something may have left
 * allocated-but-unreferenced sectors (or worse): keep the volume marked
 * dirty at unmount so CHKDSK gets to look at it. */
static int tracked(hpfs_mount_t *m, uint32_t w0, int r)
{
    if (r != VFS_OK && m->wcount != w0 && !m->inconsistent) {
        kprintf("[HPFS]   %s: an update failed part-way (%d) - the volume stays "
                "marked dirty; run CHKDSK on OS/2\n", m->dev->name, r);
        m->inconsistent = 1;
    }
    return r;
}

static int hpfs_write(vfs_mount_t *mnt, void *f, const void *b, uint32_t n, uint32_t *a)
{
    hpfs_mount_t *m = mnt->priv;
    uint32_t w0 = m->wcount;
    return tracked(m, w0, hpfs_write_do(mnt, f, b, n, a));
}

static int hpfs_unlink(vfs_mount_t *mnt, const char *path)
{
    hpfs_mount_t *m = mnt->priv;
    uint32_t w0 = m->wcount;
    return tracked(m, w0, hpfs_unlink_do(mnt, path));
}

static int hpfs_rename(vfs_mount_t *mnt, const char *old, const char *nw)
{
    hpfs_mount_t *m = mnt->priv;
    uint32_t w0 = m->wcount;
    return tracked(m, w0, hpfs_rename_do(mnt, old, nw));
}

static int hpfs_mkdir(vfs_mount_t *mnt, const char *path)
{
    hpfs_mount_t *m = mnt->priv;
    uint32_t w0 = m->wcount;
    return tracked(m, w0, hpfs_mkdir_do(mnt, path));
}

static int hpfs_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv)
{
    hpfs_mount_t *m = mnt->priv;
    uint32_t w0 = m->wcount;
    return tracked(m, w0, hpfs_open_do(mnt, path, flags, fpriv));
}

static int hpfs_close(vfs_mount_t *mnt, void *fpriv)
{
    hpfs_file_t *f = fpriv;
    hpfs_mount_t *m = f->m;
    uint32_t w0 = m ? m->wcount : 0;
    int r = hpfs_close_do(mnt, fpriv);
    return m ? tracked(m, w0, r) : r;
}

/* Free sectors: the bitmap bands (one 2 KB bitmap per 8 MB, listed by
 * the superblock), read now - works for read-only mounts too. */
static int hpfs_statfs(vfs_mount_t *mnt, vfs_statfs_t *st)
{
    hpfs_mount_t *m = mnt->priv;
    static uint8_t sb[512], list[2048], bm[2048];
    if (rd(m, SB_LSN, 1, sb) != 0) return VFS_EIO;
    uint32_t nb = (m->n_sectors + BAND_SECTORS - 1) / BAND_SECTORS;
    if (nb > 512 || rd(m, le32(sb + 24), 4, list) != 0) return VFS_EIO;
    __UINT64_TYPE__ free = 0;
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t lsn = le32(list + 4 * b);
        if (!lsn || lsn + 4 > m->n_sectors || rd(m, lsn, 4, bm) != 0) return VFS_EIO;
        for (uint32_t i = 0; i < BAND_SECTORS; i++) {
            if (b * BAND_SECTORS + i >= m->n_sectors) break;
            if ((bm[i / 8] >> (i % 8)) & 1) free++;
        }
    }
    st->bsize = 512;
    st->blocks = m->n_sectors;
    st->bfree = free;
    for (int i = 0; i < (int)sizeof st->label - 1 && m->label[i]; i++) {
        st->label[i] = m->label[i];
        st->label[i + 1] = 0;
    }
    return VFS_OK;
}

vfs_fs_t vfs_hpfs_fs = {
    .name     = "hpfs",
    .mount    = hpfs_mount_op,
    .unmount  = hpfs_unmount_op,
    .open     = hpfs_open,
    .close    = hpfs_close,
    .read     = hpfs_read,
    .write    = hpfs_write,
    .seek     = hpfs_seek,
    .stat     = hpfs_stat,
    .unlink   = hpfs_unlink,
    .rename   = hpfs_rename,
    .mkdir    = hpfs_mkdir,
    .opendir  = hpfs_opendir,
    .readdir  = hpfs_readdir,
    .closedir = hpfs_closedir,
    .statfs   = hpfs_statfs,
};

/* ── Probe / IFS init ─────────────────────────────────────────────────── */
int hpfs_probe(blkdev_t *dev)
{
    uint8_t sb[512];
    if (!dev || dev->sector_size != 512 || dev->num_sectors <= SB_LSN) return 0;
    if (blk_read(dev, SB_LSN, 1, sb) != 0) return 0;
    return le32(sb) == SB_MAGIC0 && le32(sb + 4) == SB_MAGIC1;
}

int hpfs_init(const char *params)
{
    (void)params;
    static hpfs_mount_t boot_mnt[2];
    static const char *const paths[2] = { "/HPFS", "/HPFS2" };
    int n = 0;
    for (int i = 0; n < 2; i++) {
        blkdev_t *dev = blkdev_get_by_index(i);
        if (!dev) break;
        if (!hpfs_probe(dev)) continue;
        kmemset(&boot_mnt[n], 0, sizeof boot_mnt[n]);
        boot_mnt[n].dev = dev;
        if (vfs_mount(paths[n], &vfs_hpfs_fs, &boot_mnt[n]) == VFS_OK) n++;
    }
    if (!n) kprintf("[HPFS]   No HPFS volume found; not mounting\n");
    return 0;                                   /* not fatal to boot */
}
