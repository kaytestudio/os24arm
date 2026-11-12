/**
 * @file hpfs.c
 * @brief HPFS (OS/2 High Performance File System) VFS driver, read-only
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
} hpfs_file_t;

typedef struct {
    hpfs_mount_t *m;
    dwalk_t       w;
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
                      hpfs_info_t *out)
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
                if (c == 0) { entry_info(p, out); return 1; }
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
    int r = btree_find(m, root, s, slen, out);
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
    kprintf("[HPFS]   %s: HPFS v%u, %u sectors, root fnode %u%s%s, %s\n", m->dev->name,
            sb[8], m->n_sectors, m->root_fnode, m->label[0] ? ", label " : "", m->label, cp);
    if (m->n_hotfix)
        kprintf("[HPFS]   %s: %u bad sector(s) hotfixed to spares\n", m->dev->name, m->n_hotfix);
    if (dirty == 1)
        kprintf("[HPFS]   %s: volume was not shut down cleanly - run CHKDSK on OS/2\n",
                m->dev->name);
    return VFS_OK;
}

static int hpfs_unmount_op(vfs_mount_t *mnt)
{
    hpfs_mount_t *m = mnt->priv;
    for (int i = 0; i < MAX_FILES; i++) if (files[i].m == m) files[i].m = 0;
    for (int i = 0; i < MAX_DIRS; i++)  if (dirs[i].m == m)  dirs[i].m = 0;
    return VFS_OK;
}

static int hpfs_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv)
{
    hpfs_mount_t *m = mnt->priv;
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) return VFS_EROFS;
    hpfs_file_t *f = 0;
    for (int i = 0; i < MAX_FILES; i++) if (!files[i].m) { f = &files[i]; break; }
    if (!f) return VFS_ENOSPC;

    hpfs_info_t info;
    int r = resolve(m, path, &info);
    if (r != VFS_OK) return r;
    if (info.attr & VFS_ATTR_DIR) return VFS_EISDIR;
    if (read_fnode(m, info.fnode, f->fnode) != VFS_OK) return VFS_EIO;
    f->size = le32(f->fnode + 160);
    f->pos  = 0;
    f->m    = m;
    *fpriv  = f;
    return VFS_OK;
}

static int hpfs_close(vfs_mount_t *mnt, void *fpriv)
{
    (void)mnt;
    ((hpfs_file_t *)fpriv)->m = 0;
    return VFS_OK;
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

static int hpfs_write(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len,
                      uint32_t *actual)
{
    (void)mnt; (void)fpriv; (void)buf; (void)len;
    if (actual) *actual = 0;
    return VFS_EROFS;
}

static int hpfs_seek(vfs_mount_t *mnt, void *fpriv, int32_t offset, int whence,
                     uint32_t *newpos)
{
    (void)mnt;
    hpfs_file_t *f = fpriv;
    int64_t base = whence == SEEK_CUR ? f->pos : whence == SEEK_END ? f->size : 0;
    int64_t p = base + offset;
    if (p < 0) return VFS_EINVAL;
    f->pos = p > f->size ? f->size : (uint32_t)p;
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

static int hpfs_rofs(vfs_mount_t *mnt, const char *path)
{
    (void)mnt; (void)path;
    return VFS_EROFS;
}

static int hpfs_rename(vfs_mount_t *mnt, const char *old, const char *nw)
{
    (void)mnt; (void)old; (void)nw;
    return VFS_EROFS;
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
    *dpriv = d;
    return VFS_OK;
}

static int hpfs_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent)
{
    (void)mnt;
    hpfs_dir_t *d = dpriv;
    if (!d || !d->m) return VFS_ENOENT;
    const uint8_t *p = walk_next(d->m, &d->w);
    if (!p) return VFS_ENOENT;
    hpfs_info_t info;
    entry_info(p, &info);
    kstrncpy(ent->name, info.name, sizeof ent->name);
    return VFS_OK;
}

static int hpfs_closedir(vfs_mount_t *mnt, void *dpriv)
{
    (void)mnt;
    ((hpfs_dir_t *)dpriv)->m = 0;
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
    .unlink   = hpfs_rofs,
    .rename   = hpfs_rename,
    .mkdir    = hpfs_rofs,
    .opendir  = hpfs_opendir,
    .readdir  = hpfs_readdir,
    .closedir = hpfs_closedir,
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
