/* ============================================================================
 * kernel/src/fat32.c  —  FAT16 / FAT32 filesystem driver
 *
 * The FAT variant is decided by cluster count, as the Microsoft FAT spec
 * requires (not by the BPB's informational "FAT16   "/"FAT32   " label):
 * fewer than 4085 clusters is FAT12 (not supported), fewer than 65525 is
 * FAT16, anything larger FAT32. The two differ in three places, all
 * contained here:
 *   - FAT entries are 16 bits on FAT16; fat_get()/fat_set() translate
 *     them to and from the FAT32 values (FAT32_EOC, FAT32_BAD, ...) that
 *     the rest of this file compares against.
 *   - The FAT16 root directory is a fixed region between the FATs and
 *     cluster 2, not a cluster chain. It is addressed as "cluster 0"
 *     (m->root_cluster == 0), the same value a subdirectory's ".." entry
 *     holds when its parent is the root.
 *   - A FAT16 directory entry's high cluster word is not part of the
 *     cluster number - OS/2 keeps its extended-attribute handle there - so
 *     it is ignored on read and preserved on write (dirent_cluster()).
 *
 * Supports:
 *   - Mount / volume read
 *   - VFAT long file names (UTF-8 here, UCS-2 on disk), read and written:
 *     a name that doesn't fit 8.3 gets long-name entries and a unique
 *     BASIS~N alias; all-lower-case 8.3 names use the NT_RES case bits
 *   - File open / read / write (O_APPEND) / seek / close; empty files
 *     have no cluster
 *   - Directory enumeration
 *   - File create / delete / rename and move between directories
 *   - mkdir, rmdir (unlink of an empty directory)
 *   - FAT32 FSInfo: marked unknown on the first change, recounted on
 *     unmount
 * ========================================================================== */

#include "fat32.h"
#include "vfs.h"
#include "kio.h"


/* ── Byte-order helpers (all FAT structures are little-endian) ────────────── */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24)); }
static void put_le16(uint8_t *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }

/* Wall clock (kernel/src/main.c: the PL031 RTC); absent in LX builds. */
extern uint32_t kernel_time_seconds(void) __attribute__((weak));

/* Sets a directory entry's write (and, if create, creation/access) time. */
static void fat_stamp(fat32_dirent_t *de, int create)
{
    if (!kernel_time_seconds) return;
    uint16_t d, t;
    vfs_unix_to_dos(kernel_time_seconds(), &d, &t);
    /* (packed: byte stores, since alignment checking is on) */
    put_le16((uint8_t *)&de->wrt_date, d);
    put_le16((uint8_t *)&de->wrt_time, t);
    put_le16((uint8_t *)&de->acc_date, d);
    if (create) {
        put_le16((uint8_t *)&de->crt_date, d);
        put_le16((uint8_t *)&de->crt_time, t);
    }
}
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

/* ── Cluster / LBA helpers ────────────────────────────────────────────────── */

static uint32_t cluster_to_lba(fat32_mount_t *m, uint32_t cluster)
{
    return m->data_start_lba + (cluster - 2) * m->sectors_per_cluster;
}

static int read_sector(fat32_mount_t *m, uint32_t lba, uint8_t *buf)
{
    return blk_read(m->dev, lba, 1, buf);
}

static int write_sector(fat32_mount_t *m, uint32_t lba, const uint8_t *buf)
{
    return blk_write(m->dev, lba, 1, (void *)buf);
}

/* FAT16 end-of-chain / bad-cluster markers */
#define FAT16_EOC_MIN  0xFFF8u
#define FAT16_BAD      0xFFF7u

static uint32_t fat_entry_bytes(const fat32_mount_t *m)
{
    return m->fat_bits == 16 ? 2 : 4;
}

/* Read FAT entry for a cluster. FAT16 values are widened to their FAT32
 * equivalents so callers only ever compare against FAT32_* constants. */
static uint32_t fat_get(fat32_mount_t *m, uint32_t cluster)
{
    uint32_t fat_offset  = cluster * fat_entry_bytes(m);
    uint32_t fat_sector  = m->fat_start_lba + fat_offset / SECTOR_SIZE;
    uint32_t fat_off_in  = fat_offset % SECTOR_SIZE;

    static uint8_t fat_buf[SECTOR_SIZE];
    if (read_sector(m, fat_sector, fat_buf) != 0) return FAT32_BAD;
    if (m->fat_bits == 16) {
        uint32_t v = le16(fat_buf + fat_off_in);
        if (v >= FAT16_EOC_MIN) return 0x0FFFFFFFU;
        if (v == FAT16_BAD)     return FAT32_BAD;
        return v;
    }
    return le32(fat_buf + fat_off_in) & 0x0FFFFFFFU;
}

/* Write FAT entry (value in FAT32 terms - narrowed for FAT16) */
/* FAT32's FSInfo sector caches the free-cluster count; rather than keep
 * it up to date, mark it unknown (0xFFFFFFFF, as the spec allows) on the
 * first change after mounting - Windows, macOS and Linux then count. */
static void fsinfo_invalidate(fat32_mount_t *m)
{
    if (m->fsinfo_stale || !m->fsinfo_lba) return;
    m->fsinfo_stale = 1;
    static uint8_t fsi[SECTOR_SIZE];
    if (read_sector(m, m->fsinfo_lba, fsi) != 0) return;
    if (le32(fsi) != 0x41615252u || le32(fsi + 484) != 0x61417272u) return;
    put_le32(fsi + 488, 0xFFFFFFFFu);                  /* free count: unknown */
    put_le32(fsi + 492, 0xFFFFFFFFu);                  /* next free: no hint */
    write_sector(m, m->fsinfo_lba, fsi);
}

static int fat_set(fat32_mount_t *m, uint32_t cluster, uint32_t value)
{
    fsinfo_invalidate(m);
    uint32_t fat_offset = cluster * fat_entry_bytes(m);
    uint32_t fat_sector = m->fat_start_lba + fat_offset / SECTOR_SIZE;
    uint32_t fat_off_in = fat_offset % SECTOR_SIZE;

    static uint8_t fat_buf[SECTOR_SIZE];
    if (read_sector(m, fat_sector, fat_buf) != 0) return -1;
    if (m->fat_bits == 16) {
        uint16_t v16 = value >= FAT32_EOC ? 0xFFFFu :
                       value == FAT32_BAD ? (uint16_t)FAT16_BAD : (uint16_t)value;
        put_le16(fat_buf + fat_off_in, v16);
    } else {
        uint32_t existing = le32(fat_buf + fat_off_in) & 0xF0000000U;
        put_le32(fat_buf + fat_off_in, existing | (value & 0x0FFFFFFFU));
    }
    /* Write to both FAT copies */
    write_sector(m, fat_sector, fat_buf);
    write_sector(m, fat_sector + m->fat_size_sectors, fat_buf);
    return 0;
}

/* Allocate a free cluster (first-fit) */
static uint32_t fat_alloc(fat32_mount_t *m)
{
    static uint8_t fat_buf[SECTOR_SIZE];
    uint32_t esz = fat_entry_bytes(m);
    for (uint32_t s = 0; s < m->fat_size_sectors; s++) {
        if (read_sector(m, m->fat_start_lba + s, fat_buf) != 0) continue;
        for (uint32_t off = 0; off + esz <= SECTOR_SIZE; off += esz) {
            uint32_t val = esz == 2 ? le16(fat_buf + off)
                                    : le32(fat_buf + off) & 0x0FFFFFFFU;
            if (val == FAT32_FREE) {
                uint32_t cluster = (s * SECTOR_SIZE + off) / esz;
                if (cluster < 2 || cluster >= m->total_clusters + 2) continue;
                fsinfo_invalidate(m);
                if (esz == 2) put_le16(fat_buf + off, 0xFFFF);      /* EOC */
                else          put_le32(fat_buf + off, 0x0FFFFFFF);  /* EOC */
                write_sector(m, m->fat_start_lba + s, fat_buf);
                write_sector(m, m->fat_start_lba + s + m->fat_size_sectors, fat_buf);
                /* Zero the cluster */
                static uint8_t zero[SECTOR_SIZE];
                kmemset(zero, 0, SECTOR_SIZE);
                uint32_t lba = cluster_to_lba(m, cluster);
                for (uint32_t i = 0; i < m->sectors_per_cluster; i++)
                    write_sector(m, lba + i, zero);
                return cluster;
            }
        }
    }
    return 0; /* no free cluster */
}

/* Free clusters (and the first one), a FAT sector at a time. */
static uint32_t fat_count_free(fat32_mount_t *m, uint32_t *first_free)
{
    static uint8_t fb[SECTOR_SIZE];
    uint32_t esz = fat_entry_bytes(m), free = 0, per = SECTOR_SIZE / esz;
    uint32_t last = m->total_clusters + 2;              /* clusters 2 .. last-1 */
    if (first_free) *first_free = 0;
    for (uint32_t s = 0; s * per < last && s < m->fat_size_sectors; s++) {
        if (read_sector(m, m->fat_start_lba + s, fb) != 0) continue;
        for (uint32_t k = 0; k < per; k++) {
            uint32_t c = s * per + k;
            if (c < 2) continue;
            if (c >= last) break;
            uint32_t v = esz == 2 ? le16(fb + k * 2) : le32(fb + k * 4) & 0x0FFFFFFFu;
            if (v == FAT32_FREE) {
                if (!free && first_free) *first_free = c;
                free++;
            }
        }
    }
    return free;
}

/* Follow cluster chain to nth cluster (0-based) */
static uint32_t fat_nth(fat32_mount_t *m, uint32_t first, uint32_t n)
{
    uint32_t c = first;
    for (uint32_t i = 0; i < n; i++) {
        c = fat_get(m, c);
        if (c >= FAT32_EOC || c == FAT32_BAD || c < 2) return 0;
    }
    return c;
}

/* ══════════════════════════════════════════════════════════════════════════
   MOUNT
   ══════════════════════════════════════════════════════════════════════════ */

int fat32_mount_init(fat32_mount_t *m, blkdev_t *dev)
{
    m->dev = dev;
    m->sector_buf_lba = 0xFFFFFFFF;

    uint8_t boot[SECTOR_SIZE];
    if (blk_read(dev, 0, 1, boot) != 0) return VFS_EIO;

    /* Validate boot sector */
    if (le16(boot + 510) != FAT32_SIGNATURE) {
        kprintf("[FAT] Bad signature\n"); return VFS_EIO;
    }

    uint16_t bytes_per_sector  = le16(boot + 11);
    uint8_t  spc               = boot[13];
    uint16_t reserved_sectors  = le16(boot + 14);
    uint8_t  fat_count         = boot[16];
    uint16_t root_entry_count  = le16(boot + 17);
    uint16_t fat_size_16       = le16(boot + 22);
    uint32_t fat_size_32       = le32(boot + 36);

    if (bytes_per_sector != SECTOR_SIZE) {
        kprintf("[FAT] Sector size %u unsupported\n", bytes_per_sector);
        return VFS_EINVAL;
    }
    if (spc == 0 || fat_count == 0 || reserved_sectors == 0) {
        kprintf("[FAT] Invalid BPB\n");
        return VFS_EINVAL;
    }

    /* Geometry, per the FAT spec: 16-bit fields win when non-zero. */
    uint32_t fat_size      = fat_size_16 ? fat_size_16 : fat_size_32;
    uint32_t total_sectors = le16(boot + 19);
    if (!total_sectors) total_sectors = le32(boot + 32);
    uint32_t root_dir_sectors = ((uint32_t)root_entry_count * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t meta_sectors     = reserved_sectors + fat_count * fat_size + root_dir_sectors;
    if (fat_size == 0 || total_sectors <= meta_sectors) {
        kprintf("[FAT] Invalid BPB\n");
        return VFS_EINVAL;
    }
    uint32_t clusters = (total_sectors - meta_sectors) / spc;

    if (clusters < 4085) {
        kprintf("[FAT] FAT12 volume (%u clusters) not supported\n", clusters);
        return VFS_EINVAL;
    }
    m->fat_bits = clusters < 65525 ? 16 : 32;
    if (m->fat_bits == 16 && root_entry_count == 0) {
        kprintf("[FAT] FAT16 volume without a root directory\n");
        return VFS_EINVAL;
    }

    m->sectors_per_cluster = spc;
    m->bytes_per_cluster   = (uint32_t)spc * SECTOR_SIZE;
    m->fat_size_sectors    = fat_size;
    m->fat_start_lba       = reserved_sectors;
    m->root_dir_lba        = reserved_sectors + fat_count * fat_size;
    m->root_entry_count    = m->fat_bits == 16 ? root_entry_count : 0;
    m->data_start_lba      = meta_sectors;
    m->root_cluster        = m->fat_bits == 32 ? le32(boot + 44) : 0;
    m->total_clusters      = clusters;
    uint16_t fsi = m->fat_bits == 32 ? le16(boot + 48) : 0;
    m->fsinfo_lba          = (fsi && fsi != 0xFFFF && fsi < reserved_sectors) ? fsi : 0;
    m->fsinfo_stale        = 0;

    kprintf("[FAT] Mounted FAT%u: clusters=%u  root=%u  data_lba=%u\n",
            m->fat_bits, m->total_clusters, m->root_cluster, m->data_start_lba);
    return VFS_OK;
}

/* First cluster of a directory entry. FAT16 only has the low word: the
 * high word is OS/2's extended-attribute handle there, not cluster bits. */
static uint32_t dirent_cluster(const fat32_mount_t *m, const fat32_dirent_t *de)
{
    uint32_t lo = le16((const uint8_t *)&de->cluster_lo);
    if (m->fat_bits == 16) return lo;
    return ((uint32_t)le16((const uint8_t *)&de->cluster_hi) << 16) | lo;
}

static void set_dirent_cluster(const fat32_mount_t *m, fat32_dirent_t *de, uint32_t cluster)
{
    if (m->fat_bits != 16)
        put_le16((uint8_t *)&de->cluster_hi, (uint16_t)(cluster >> 16));
    put_le16((uint8_t *)&de->cluster_lo, (uint16_t)(cluster & 0xFFFF));
}

/* ══════════════════════════════════════════════════════════════════════════
   PATH / NAME HELPERS
   ══════════════════════════════════════════════════════════════════════════ */

/* Convert 8.3 directory entry name to normal string (e.g. "FILE    TXT" → "FILE.TXT") */
static void fat32_name_to_str(const fat32_dirent_t *de, char *out)
{
    /* NT_RES bits 3 and 4: the base / extension is really lower case
     * (how Windows and Linux store "readme.txt" without a long name) */
    int lb = (de->nt_res & 0x08) != 0, le = (de->nt_res & 0x10) != 0;
    int i, j = 0;
    for (i = 0; i < 8 && de->name[i] != ' '; i++) {
        char c = (char)de->name[i];
        out[j++] = (lb && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (de->ext[0] != ' ') {
        out[j++] = '.';
        for (i = 0; i < 3 && de->ext[i] != ' '; i++) {
            char c = (char)de->ext[i];
            out[j++] = (le && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    out[j] = '\0';
}

/* Split path into first component and rest.
   "/foo/bar/baz" → component="foo", rest="/bar/baz"
   Returns pointer to rest (or NULL if none). */
static const char *path_split(const char *path, char *component)
{
    while (*path == '/') path++;
    int i = 0;
    while (*path && *path != '/' && i < VFS_NAME_MAX - 1)
        component[i++] = *path++;
    component[i] = '\0';
    if (!*path) return NULL;
    return path;
}

/* ══════════════════════════════════════════════════════════════════════════
   DIRECTORY TRAVERSAL
   ══════════════════════════════════════════════════════════════════════════ */

/* Locate directory entry (cluster, entry_idx) on disk: the sector's LBA
 * and the entry's byte offset in it. Cluster 0 on FAT16 is the fixed root
 * region, which ends after root_entry_count entries and can't grow.
 * Returns 0, or -1 past the end of the directory. */
static int dirent_location(fat32_mount_t *m, uint32_t cluster, uint32_t entry_idx,
                           uint32_t *lba, uint32_t *offset)
{
    uint32_t entries_per_sector = SECTOR_SIZE / sizeof(fat32_dirent_t);

    if (cluster == 0 && m->fat_bits == 16) {
        if (entry_idx >= m->root_entry_count) return -1;
        *lba    = m->root_dir_lba + entry_idx / entries_per_sector;
        *offset = (entry_idx % entries_per_sector) * sizeof(fat32_dirent_t);
        return 0;
    }

    uint32_t entries_per_cluster = m->bytes_per_cluster / sizeof(fat32_dirent_t);
    uint32_t cluster_idx         = entry_idx / entries_per_cluster;
    uint32_t entry_in_cluster    = entry_idx % entries_per_cluster;

    /* Follow chain to correct cluster */
    uint32_t c = fat_nth(m, cluster, cluster_idx);
    if (c == 0 && cluster_idx > 0) return -1;
    if (cluster_idx == 0) c = cluster;

    *lba    = cluster_to_lba(m, c) + (entry_in_cluster * sizeof(fat32_dirent_t)) / SECTOR_SIZE;
    *offset = (entry_in_cluster % entries_per_sector) * sizeof(fat32_dirent_t);
    return 0;
}

/* Read directory entry at (cluster, entry_idx).
   Returns 0 on success, -1 on end/error. */
static int fat32_read_dirent(fat32_mount_t *m, uint32_t cluster,
                              uint32_t entry_idx, fat32_dirent_t *de)
{
    uint32_t lba, off;
    if (dirent_location(m, cluster, entry_idx, &lba, &off) != 0) return -1;

    uint8_t buf[SECTOR_SIZE];
    if (read_sector(m, lba, buf) != 0) return -1;
    kmemcpy(de, buf + off, sizeof(fat32_dirent_t));
    return 0;
}

/* Write directory entry at (cluster, entry_idx) */
static int fat32_write_dirent(fat32_mount_t *m, uint32_t cluster,
                               uint32_t entry_idx, const fat32_dirent_t *de)
{
    uint32_t lba, off;
    if (dirent_location(m, cluster, entry_idx, &lba, &off) != 0) return -1;

    uint8_t buf[SECTOR_SIZE];
    if (read_sector(m, lba, buf) != 0) return -1;
    kmemcpy(buf + off, de, sizeof(fat32_dirent_t));
    return write_sector(m, lba, buf);
}

/* ── VFAT long file names ─────────────────────────────────────────────────
 * A long name is stored as up to 20 extra entries (attribute 0x0F) just
 * before its 8.3 entry, last part first: 13 UCS-2 characters each, a
 * sequence number (0x40 on the first one stored), and the checksum of
 * the 8.3 name they belong to. Names here are UTF-8. */

#define LFN_MAX_ENTRIES 20

static int in_set(const char *set, char c)
{
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}
static const uint8_t lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static uint8_t sfn_checksum(const uint8_t *n11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + n11[i]);
    return sum;
}

/* UTF-8 -> UCS-2 (the BMP); -1 if invalid or longer than max. */
static int utf8_to_ucs2(const char *s, uint16_t *u, int max)
{
    const uint8_t *p = (const uint8_t *)s;
    int n = 0;
    while (*p) {
        uint32_t c;
        if (*p < 0x80) c = *p++;
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            c = ((uint32_t)(*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            c = ((uint32_t)(*p & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else return -1;
        if (n >= max) return -1;
        u[n++] = (uint16_t)c;
    }
    return n;
}

static void ucs2_to_utf8(const uint16_t *u, int n, char *out, int size)
{
    int k = 0;
    for (int i = 0; i < n && u[i] && u[i] != 0xFFFF; i++) {
        uint16_t c = u[i];
        if (c >= 0xD800 && c < 0xE000) c = '?';         /* no surrogate pairs */
        if (c < 0x80) { if (k + 1 >= size) break; out[k++] = (char)c; }
        else if (c < 0x800) {
            if (k + 2 >= size) break;
            out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (k + 3 >= size) break;
            out[k++] = (char)(0xE0 | (c >> 12)); out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[k] = '\0';
}

static int name_eq(const char *a, const char *b)       /* ASCII case-insensitive */
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* The next live entry from *idx on: its 8.3 entry in *de, its name in
 * name (the long one when a valid one precedes it), where it is (*at)
 * and where its long-name entries start (*first; == *at without one).
 * *idx moves past it. Returns 0, or -1 at the end of the directory. */
static int dir_next(fat32_mount_t *m, uint32_t dc, uint32_t *idx, fat32_dirent_t *de,
                    char *name, int name_size, uint32_t *first, uint32_t *at)
{
    static uint16_t lfn[LFN_MAX_ENTRIES * 13];
    int lfn_ok = 0, lfn_count = 0, expect = 0;
    uint8_t lfn_sum = 0;
    uint32_t lfn_first = 0;
    for (;;) {
        uint32_t i = *idx;
        if (fat32_read_dirent(m, dc, i, de) != 0) return -1;
        const uint8_t *r = (const uint8_t *)de;
        if (r[0] == 0x00) return -1;                   /* the end */
        (*idx)++;
        if (r[0] == 0xE5) { lfn_ok = 0; continue; }    /* deleted */
        if (de->attr == FAT32_ATTR_LFN) {
            int ord = r[0] & 0x1F;
            if (r[0] & 0x40) {
                if (ord < 1 || ord > LFN_MAX_ENTRIES) { lfn_ok = 0; continue; }
                lfn_ok = 1; lfn_count = ord; lfn_sum = r[13]; lfn_first = i;
            } else if (!lfn_ok || ord != expect || r[13] != lfn_sum) {
                lfn_ok = 0;
                continue;
            }
            for (int j = 0; j < 13; j++) lfn[(ord - 1) * 13 + j] = le16(r + lfn_off[j]);
            expect = ord - 1;
            continue;
        }
        if (de->attr & FAT32_ATTR_VOLID) { lfn_ok = 0; continue; }
        if (lfn_ok && expect == 0 && sfn_checksum(r) == lfn_sum) {
            ucs2_to_utf8(lfn, lfn_count * 13, name, name_size);
            *first = lfn_first;
        } else {
            fat32_name_to_str(de, name);
            *first = i;
        }
        *at = i;
        return 0;
    }
}

/* Find name in a directory: by its long name or its 8.3 one. */
static int fat32_find_in_dir(fat32_mount_t *m, uint32_t dir_cluster, const char *name,
                             fat32_dirent_t *out_de, char *out_name,
                             uint32_t *out_entry_idx, uint32_t *out_first)
{
    fat32_dirent_t de;
    char found[VFS_NAME_MAX + 1], short_name[16];
    uint32_t idx = 0, first, at;
    while (dir_next(m, dir_cluster, &idx, &de, found, sizeof found, &first, &at) == 0) {
        fat32_name_to_str(&de, short_name);
        if (name_eq(found, name) || name_eq(short_name, name)) {
            if (out_de) kmemcpy(out_de, &de, sizeof de);
            if (out_name) { int k = 0; while (found[k] && k < VFS_NAME_MAX) { out_name[k] = found[k]; k++; } out_name[k] = 0; }
            if (out_entry_idx) *out_entry_idx = at;
            if (out_first) *out_first = first;
            return (int)at;
        }
    }
    return -1;
}

/* Resolve a path to its directory entry: the entry, the directory it is
 * in (*dir_cluster_out), its index there (and of its long-name entries,
 * *first_out), and its name as stored (name_out). */
static int fat32_resolve_ex(fat32_mount_t *m, const char *path,
                            fat32_dirent_t *de_out, uint32_t *dir_cluster_out,
                            uint32_t *entry_idx_out, uint32_t *first_out, char *name_out)
{
    uint32_t cur_cluster = m->root_cluster;
    char component[VFS_NAME_MAX];
    const char *rest = path;

    while (*rest == '/') rest++;
    if (*rest == '\0') {                               /* the root itself */
        if (de_out) {
            kmemset(de_out, 0, sizeof(*de_out));
            kmemset(de_out->name, ' ', 8);
            kmemset(de_out->ext, ' ', 3);
            de_out->attr = FAT32_ATTR_DIR;
            set_dirent_cluster(m, de_out, m->root_cluster);
        }
        if (dir_cluster_out) *dir_cluster_out = m->root_cluster;
        if (entry_idx_out)   *entry_idx_out   = 0;
        if (first_out)       *first_out       = 0;
        if (name_out)        name_out[0]      = '\0';
        return VFS_OK;
    }

    while (*rest) {
        rest = path_split(rest, component);
        if (!component[0]) break;

        fat32_dirent_t de;
        uint32_t found_idx, found_first;
        if (fat32_find_in_dir(m, cur_cluster, component, &de, name_out, &found_idx, &found_first) < 0)
            return VFS_ENOENT;

        if (rest && *rest) {                           /* a directory on the way */
            if (!(de.attr & FAT32_ATTR_DIR)) return VFS_ENOTDIR;
            cur_cluster = dirent_cluster(m, &de);
            if (cur_cluster < 2) cur_cluster = m->root_cluster;
        } else {
            if (de_out)          kmemcpy(de_out, &de, sizeof(de));
            if (dir_cluster_out) *dir_cluster_out = cur_cluster;
            if (entry_idx_out)   *entry_idx_out   = found_idx;
            if (first_out)       *first_out       = found_first;
            return VFS_OK;
        }
        if (!rest) break;
    }
    return VFS_ENOENT;
}

static int fat32_resolve(fat32_mount_t *m, const char *path, fat32_dirent_t *de_out,
                         uint32_t *dir_cluster_out, uint32_t *entry_idx_out,
                         uint32_t *parent_cluster_out)
{
    int r = fat32_resolve_ex(m, path, de_out, dir_cluster_out, entry_idx_out, 0, 0);
    if (r == VFS_OK && parent_cluster_out && dir_cluster_out) *parent_cluster_out = *dir_cluster_out;
    return r;
}

/* The directory cluster a path's parent is (root for "/x"); -1 if none. */
static int parent_dir(fat32_mount_t *m, const char *path, uint32_t *dc, const char **leaf)
{
    const char *last = path;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    *dc = m->root_cluster;
    if (last != path) {
        char parent_path[VFS_PATH_MAX];
        int plen = (int)(last - path);
        if (plen >= (int)sizeof parent_path) return -1;
        kmemcpy(parent_path, path, (size_t)plen);
        parent_path[plen] = '\0';
        fat32_dirent_t par;
        if (fat32_resolve(m, parent_path, &par, NULL, NULL, NULL) != VFS_OK) return -1;
        if (!(par.attr & FAT32_ATTR_DIR)) return -1;
        *dc = dirent_cluster(m, &par);
        if (*dc < 2) *dc = m->root_cluster;
    }
    while (*last == '/') last++;
    *leaf = last;
    return 0;
}

/* count free entries in a row (deleted, or past the end marker); the
 * directory grows by a zeroed cluster when it runs out (not the FAT16
 * root, which is fixed). */
static int alloc_run(fat32_mount_t *m, uint32_t dc, uint32_t count, uint32_t *start)
{
    uint32_t run = 0, run_start = 0;
    for (uint32_t idx = 0; ; idx++) {
        fat32_dirent_t e;
        if (fat32_read_dirent(m, dc, idx, &e) != 0) {
            if (dc == 0 && m->fat_bits == 16) return VFS_ENOSPC;
            uint32_t tail = dc;
            for (uint32_t next = fat_get(m, tail); next >= 2 && next < FAT32_EOC; next = fat_get(m, tail))
                tail = next;
            uint32_t nc = fat_alloc(m);                /* zeroed: all "end" entries */
            if (!nc) return VFS_ENOSPC;
            fat_set(m, tail, nc);
            fat_set(m, nc, FAT32_EOC);
            idx--;                                     /* read it again */
            continue;
        }
        uint8_t c0 = (uint8_t)e.name[0];
        if (c0 == 0x00 || c0 == 0xE5) {
            if (!run) run_start = idx;
            if (++run == count) { *start = run_start; return VFS_OK; }
        } else {
            run = 0;
        }
    }
}

/* name fits 8.3 as it is (one case per part)? Then its 11 bytes and the
 * NT_RES lower-case bits. */
static int fits_83(const char *name, uint8_t *n11, uint8_t *nt)
{
    static const char ok[] = "!#$%&'()-@^_`{}~";
    kmemset(n11, ' ', 11);
    *nt = 0;
    int bl = 0, el = 0, in_ext = 0, upper[2] = { 0, 0 }, lower[2] = { 0, 0 };
    if (name[0] == '.') return 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') { if (in_ext) return 0; in_ext = 1; continue; }
        int good = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        for (const char *q = ok; !good && *q; q++) good = c == *q;
        if (!good) return 0;
        if (c >= 'a' && c <= 'z') { lower[in_ext] = 1; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') upper[in_ext] = 1;
        if (!in_ext) { if (bl == 8) return 0; n11[bl++] = (uint8_t)c; }
        else         { if (el == 3) return 0; n11[8 + el++] = (uint8_t)c; }
    }
    if (!bl || (in_ext && !el)) return 0;
    if ((upper[0] && lower[0]) || (upper[1] && lower[1])) return 0;   /* mixed: keep it long */
    *nt = (uint8_t)((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
    return 1;
}

static int sfn_taken(fat32_mount_t *m, uint32_t dc, const uint8_t *n11)
{
    fat32_dirent_t e;
    for (uint32_t idx = 0; fat32_read_dirent(m, dc, idx, &e) == 0 && e.name[0]; idx++) {
        if ((uint8_t)e.name[0] == 0xE5 || e.attr == FAT32_ATTR_LFN) continue;
        int same = 1;
        for (int i = 0; i < 11 && same; i++) same = ((const uint8_t *)&e)[i] == n11[i];
        if (same) return 1;
    }
    return 0;
}

/* A unique 8.3 alias for a long name: "My Long File.html" -> MYLONG~1.HTM */
static int make_sfn(fat32_mount_t *m, uint32_t dc, const char *name, uint8_t *n11)
{
    uint8_t base[8], ext[3];
    int bl = 0, el = 0;
    const char *dot = 0;
    for (const char *p = name + 1; *p; p++) if (*p == '.') dot = p;
    for (const char *p = name; *p && p != dot; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == ' ' || c == '.' || (c >= 0x80 && c < 0xC0)) continue;   /* (UTF-8 tails) */
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 0x80 || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 in_set("!#$%&'()-@^_`{}~", (char)c))) c = '_';
        if (bl < 8) base[bl++] = c;
    }
    for (const char *p = dot ? dot + 1 : ""; *p && el < 3; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == ' ' || (c >= 0x80 && c < 0xC0)) continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 0x80 || !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 in_set("!#$%&'()-@^_`{}~", (char)c))) c = '_';
        ext[el++] = c;
    }
    if (!bl) base[bl++] = '_';
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[8];
        int tl = 0;
        uint32_t v = n;
        char digits[8]; int dl = 0;
        do { digits[dl++] = (char)('0' + v % 10); v /= 10; } while (v);
        tail[tl++] = '~';
        while (dl) tail[tl++] = digits[--dl];
        int keep = bl < 8 - tl ? bl : 8 - tl;
        kmemset(n11, ' ', 11);
        for (int i = 0; i < keep; i++) n11[i] = base[i];
        for (int i = 0; i < tl; i++) n11[keep + i] = (uint8_t)tail[i];
        for (int i = 0; i < el; i++) n11[8 + i] = ext[i];
        if (!sfn_taken(m, dc, n11)) return 0;
    }
    return -1;
}

/* Adds an entry for name to directory dc: *de carries everything but the
 * name (attributes, cluster, size, times). Long-name entries go first
 * when name doesn't fit 8.3. Returns VFS_OK with the 8.3 entry's index. */
static int dir_add(fat32_mount_t *m, uint32_t dc, const char *name, fat32_dirent_t *de,
                   uint32_t *at_out)
{
    static uint16_t u[LFN_MAX_ENTRIES * 13];
    uint8_t n11[11], nt = 0;
    int ulen = utf8_to_ucs2(name, u, 255);
    if (ulen <= 0) return VFS_EINVAL;
    for (int i = 0; i < ulen; i++)
        if (u[i] < 32 || (u[i] < 128 && in_set("\\/:*?\"<>|", (char)u[i]))) return VFS_EINVAL;
    int nl = 0;
    if (!fits_83(name, n11, &nt)) {
        if (make_sfn(m, dc, name, n11) != 0) return VFS_ENOSPC;
        nl = (ulen + 12) / 13;
        nt = 0;
    } else if (sfn_taken(m, dc, n11)) {
        return VFS_EINVAL;                             /* exists */
    }

    uint32_t start;
    int r = alloc_run(m, dc, (uint32_t)nl + 1, &start);
    if (r != VFS_OK) return r;

    uint8_t sum = sfn_checksum(n11);
    for (int k = nl; k >= 1; k--) {                    /* the last part is stored first */
        uint8_t raw[32];
        kmemset(raw, 0, sizeof raw);
        raw[0] = (uint8_t)(k | (k == nl ? 0x40 : 0));
        raw[11] = FAT32_ATTR_LFN;
        raw[13] = sum;
        for (int j = 0; j < 13; j++) {
            int pos = (k - 1) * 13 + j;
            put_le16(raw + lfn_off[j], pos < ulen ? u[pos] : pos == ulen ? 0x0000 : 0xFFFF);
        }
        if (fat32_write_dirent(m, dc, start + (uint32_t)(nl - k), (const fat32_dirent_t *)raw) != 0)
            return VFS_EIO;
    }
    kmemcpy(de->name, n11, 8);
    kmemcpy(de->ext, n11 + 8, 3);
    de->nt_res = nt;
    if (fat32_write_dirent(m, dc, start + (uint32_t)nl, de) != 0) return VFS_EIO;
    *at_out = start + (uint32_t)nl;
    return VFS_OK;
}

/* Marks an entry and its long-name entries deleted. */
static int dir_remove(fat32_mount_t *m, uint32_t dc, uint32_t first, uint32_t at)
{
    for (uint32_t i = first; i <= at; i++) {
        fat32_dirent_t e;
        if (fat32_read_dirent(m, dc, i, &e) != 0) return VFS_EIO;
        e.name[0] = 0xE5;
        if (fat32_write_dirent(m, dc, i, &e) != 0) return VFS_EIO;
    }
    return VFS_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
   VFS DRIVER OPS
   ══════════════════════════════════════════════════════════════════════════ */

static fat32_file_t open_files[FAT32_MAX_OPEN_FILES];
static fat32_dir_t  open_dirs [FAT32_MAX_OPEN_DIRS];

/* open_files/open_dirs are shared by every FAT volume; each entry
 * records its own volume (->mnt), so mounting another volume must not
 * touch them (it used to clear both tables, breaking the open files of
 * a volume already mounted). */
static int fat32_vfs_mount(vfs_mount_t *mnt)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;
    return fat32_mount_init(m, m->dev);
}

static int fat32_vfs_unmount(vfs_mount_t *mnt)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++)
        if (open_files[i].mnt == m) open_files[i].mnt = NULL;
    for (int i = 0; i < FAT32_MAX_OPEN_DIRS; i++)
        if (open_dirs[i].mnt == m) open_dirs[i].mnt = NULL;
    /* Changed since mounting: count the free clusters into FSInfo again
     * (pulled out without UMOUNT, it says "unknown", which is allowed) */
    if (m->fsinfo_stale && m->fsinfo_lba) {
        static uint8_t fsi[SECTOR_SIZE];
        uint32_t first_free;
        uint32_t free = fat_count_free(m, &first_free);
        if (read_sector(m, m->fsinfo_lba, fsi) == 0 &&
            le32(fsi) == 0x41615252u && le32(fsi + 484) == 0x61417272u) {
            put_le32(fsi + 488, free);
            put_le32(fsi + 492, first_free ? first_free : 0xFFFFFFFFu);
            write_sector(m, m->fsinfo_lba, fsi);
        }
        m->fsinfo_stale = 0;
    }
    return VFS_OK;
}

static int fat32_vfs_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;

    /* Find a free slot */
    fat32_file_t *f = NULL;
    for (int i = 0; i < FAT32_MAX_OPEN_FILES; i++) {
        if (!open_files[i].mnt) { f = &open_files[i]; break; }
    }
    if (!f) return VFS_ENOSPC;

    fat32_dirent_t de;
    uint32_t dir_cluster, entry_idx, parent_cluster;
    int r = fat32_resolve(m, path, &de, &dir_cluster, &entry_idx, &parent_cluster);

    if (r == VFS_ENOENT && (flags & O_CREAT)) {
        /* Create the file in its parent directory */
        uint32_t par_cluster;
        const char *leaf;
        if (parent_dir(m, path, &par_cluster, &leaf) != 0) return VFS_ENOENT;

        /* An empty file has no cluster (cluster 0), as the spec says;
         * the first write allocates one. */
        kmemset(&de, 0, sizeof(de));
        de.attr = FAT32_ATTR_ARCHIVE;
        de.file_size = 0;
        fat_stamp(&de, 1);
        r = dir_add(m, par_cluster, leaf, &de, &entry_idx);
        if (r != VFS_OK) return r;

        dir_cluster = par_cluster;
        f->first_cluster = 0;
        f->file_size     = 0;
    } else if (r != VFS_OK) {
        return r;
    } else {
        if (de.attr & FAT32_ATTR_DIR) return VFS_EISDIR;
        f->first_cluster = dirent_cluster(m, &de);
        f->file_size = de.file_size;
        if (flags & O_TRUNC) {
            /* Free chain and reset */
            uint32_t c = f->first_cluster;
            while (c >= 2 && c < FAT32_EOC) {
                uint32_t next = fat_get(m, c);
                fat_set(m, c, FAT32_FREE);
                c = next;
            }
            f->first_cluster = 0;                      /* empty: no cluster */
            f->file_size = 0;
            /* Update dirent */
            set_dirent_cluster(m, &de, f->first_cluster);
            de.file_size = 0;
            fat32_write_dirent(m, dir_cluster, entry_idx, &de);
        }
    }

    f->mnt          = m;
    f->cur_cluster  = f->first_cluster;
    f->pos          = 0;
    f->cluster_seq  = 0;
    f->dir_cluster  = dir_cluster;
    f->dir_entry_idx = entry_idx;
    f->flags        = flags;
    *fpriv          = f;
    return VFS_OK;
}

static int fat32_vfs_close(vfs_mount_t *mnt, void *fpriv)
{
    (void)mnt;
    fat32_file_t *f = (fat32_file_t *)fpriv;
    /* Flush file size back to directory entry */
    fat32_dirent_t de;
    fat32_read_dirent(f->mnt, f->dir_cluster, f->dir_entry_idx, &de);
    de.file_size = f->file_size;
    set_dirent_cluster(f->mnt, &de, f->first_cluster);
    if (f->written) {
        fat_stamp(&de, 0);
        de.attr |= FAT32_ATTR_ARCHIVE;
    }
    fat32_write_dirent(f->mnt, f->dir_cluster, f->dir_entry_idx, &de);
    kmemset(f, 0, sizeof(*f));
    return VFS_OK;
}

static int fat32_vfs_read(vfs_mount_t *mnt, void *fpriv,
                           void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    fat32_file_t *f = (fat32_file_t *)fpriv;
    fat32_mount_t *m = f->mnt;

    if (f->pos >= f->file_size) { *actual = 0; return VFS_OK; }
    if (f->pos + len > f->file_size) len = f->file_size - f->pos;

    uint32_t done = 0;
    static uint8_t sbuf[SECTOR_SIZE];

    while (done < len) {
        if (!f->cur_cluster || f->cur_cluster >= FAT32_EOC)
            break;

        uint32_t off_in_cluster = f->pos % m->bytes_per_cluster;
        uint32_t remain_in_cluster = m->bytes_per_cluster - off_in_cluster;
        uint32_t to_read = len - done;
        if (to_read > remain_in_cluster) to_read = remain_in_cluster;

        uint32_t lba = cluster_to_lba(m, f->cur_cluster);
        uint32_t sector_off = off_in_cluster / SECTOR_SIZE;
        uint32_t off_in_sector = off_in_cluster % SECTOR_SIZE;

        while (to_read > 0) {
            read_sector(m, lba + sector_off, sbuf);
            uint32_t can = SECTOR_SIZE - off_in_sector;
            if (can > to_read) can = to_read;
            kmemcpy((uint8_t *)buf + done, sbuf + off_in_sector, can);
            done    += can;
            f->pos  += can;
            to_read -= can;
            off_in_sector = 0;
            sector_off++;
        }

        if (f->pos % m->bytes_per_cluster == 0) {
            f->cur_cluster = fat_get(m, f->cur_cluster);
            f->cluster_seq++;
        }
    }

    *actual = done;
    return VFS_OK;
}

static int fat32_vfs_seek(vfs_mount_t *mnt, void *fpriv, int32_t offset, int whence, uint32_t *newpos);

static int fat32_vfs_write(vfs_mount_t *mnt, void *fpriv,
                            const void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    fat32_file_t *f = (fat32_file_t *)fpriv;
    fat32_mount_t *m = f->mnt;
    f->written = 1;

    if (!(f->flags & (O_WRONLY | O_RDWR))) return VFS_EROFS;
    if ((f->flags & O_APPEND) && f->pos != f->file_size)   /* every write at the end */
        fat32_vfs_seek(mnt, f, 0, SEEK_END, 0);

    uint32_t done = 0;
    static uint8_t sbuf[SECTOR_SIZE];

    while (done < len) {
        /* Ensure we have a current cluster */
        if (!f->cur_cluster || f->cur_cluster >= FAT32_EOC) {
            /* Extend file */
            uint32_t nc = fat_alloc(m);
            if (!nc) break;
            if (f->file_size == 0 && f->first_cluster < 2) {
                f->first_cluster = nc;
            } else {
                /* find tail */
                uint32_t tail = f->first_cluster;
                while (fat_get(m, tail) < FAT32_EOC) tail = fat_get(m, tail);
                fat_set(m, tail, nc);
            }
            fat_set(m, nc, FAT32_EOC);
            f->cur_cluster = nc;
        }

        uint32_t off_in_cluster = f->pos % m->bytes_per_cluster;
        uint32_t remain_in_cluster = m->bytes_per_cluster - off_in_cluster;
        uint32_t to_write = len - done;
        if (to_write > remain_in_cluster) to_write = remain_in_cluster;

        uint32_t lba = cluster_to_lba(m, f->cur_cluster);
        uint32_t sector_off = off_in_cluster / SECTOR_SIZE;
        uint32_t off_in_sector = off_in_cluster % SECTOR_SIZE;

        while (to_write > 0) {
            uint32_t can = SECTOR_SIZE - off_in_sector;
            if (can > to_write) can = to_write;
            if (can < SECTOR_SIZE) read_sector(m, lba + sector_off, sbuf);
            kmemcpy(sbuf + off_in_sector, (const uint8_t *)buf + done, can);
            write_sector(m, lba + sector_off, sbuf);
            done     += can;
            f->pos   += can;
            to_write -= can;
            off_in_sector = 0;
            sector_off++;
        }

        if (f->pos > f->file_size) f->file_size = f->pos;

        if (f->pos % m->bytes_per_cluster == 0) {
            uint32_t next = fat_get(m, f->cur_cluster);
            if (next >= FAT32_EOC) {
                /* Will be allocated on next iteration */
                f->cur_cluster = 0;
            } else {
                f->cur_cluster = next;
            }
            f->cluster_seq++;
        }
    }

    *actual = done;
    return VFS_OK;
}

static int fat32_vfs_seek(vfs_mount_t *mnt, void *fpriv,
                           int32_t offset, int whence, uint32_t *newpos)
{
    (void)mnt;
    fat32_file_t *f = (fat32_file_t *)fpriv;
    fat32_mount_t *m = f->mnt;

    uint32_t target;
    if (whence == SEEK_SET)      target = (uint32_t)offset;
    else if (whence == SEEK_CUR) target = f->pos + (uint32_t)offset;
    else                             target = f->file_size + (uint32_t)offset;

    /* Re-walk cluster chain from start */
    uint32_t bpc = m->bytes_per_cluster;
    uint32_t target_seq = target / bpc;
    f->cur_cluster = f->first_cluster;
    for (uint32_t i = 0; i < target_seq; i++) {
        uint32_t next = fat_get(m, f->cur_cluster);
        if (next >= FAT32_EOC || next < 2) { f->cur_cluster = 0; break; }
        f->cur_cluster = next;
    }
    f->pos = target;
    f->cluster_seq = target_seq;
    if (newpos) *newpos = target;
    return VFS_OK;
}

static int fat32_vfs_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;
    fat32_dirent_t de;
    char name[VFS_NAME_MAX + 1];
    if (fat32_resolve_ex(m, path, &de, NULL, NULL, NULL, name) != VFS_OK)
        return VFS_ENOENT;
    if (name[0]) kstrncpy(st->name, name, sizeof st->name);
    else fat32_name_to_str(&de, st->name);
    st->size  = de.file_size;
    st->attrs = (de.attr & FAT32_ATTR_DIR)    ? VFS_ATTR_DIR    : 0;
    st->attrs|= (de.attr & FAT32_ATTR_RDONLY) ? VFS_ATTR_RDONLY : 0;
    st->attrs|= (de.attr & FAT32_ATTR_HIDDEN) ? VFS_ATTR_HIDDEN : 0;
    st->attrs|= (de.attr & FAT32_ATTR_SYSTEM) ? VFS_ATTR_SYSTEM : 0;
    st->attrs|= (de.attr & FAT32_ATTR_ARCHIVE) ? VFS_ATTR_ARCHIVE : 0;
    st->mdate = de.wrt_date;
    st->mtime = de.wrt_time;
    return VFS_OK;
}

static int dc_is_root(fat32_mount_t *m, const char *path)
{
    (void)m;
    while (*path == '/') path++;
    return !*path;
}

static int fat32_vfs_unlink(vfs_mount_t *mnt, const char *path)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;
    fat32_dirent_t de;
    uint32_t dir_cluster, entry_idx, first;
    if (fat32_resolve_ex(m, path, &de, &dir_cluster, &entry_idx, &first, NULL) != VFS_OK)
        return VFS_ENOENT;
    if (dc_is_root(m, path)) return VFS_EISDIR;
    if (de.attr & FAT32_ATTR_DIR) {
        /* A directory goes only when empty (VFS_EISDIR otherwise, as the
         * other drivers report it): nothing but ".", ".." and deleted or
         * long-name entries. The root has no entry of its own. */
        uint32_t dc = dirent_cluster(m, &de);
        if (dc < 2) return VFS_EISDIR;
        for (uint32_t i = 0; ; i++) {
            fat32_dirent_t e;
            if (fat32_read_dirent(m, dc, i, &e) != 0 || e.name[0] == 0x00) break;
            if ((uint8_t)e.name[0] == 0xE5 || e.attr == FAT32_ATTR_LFN || e.name[0] == '.') continue;
            return VFS_EISDIR;
        }
    }

    /* Free cluster chain */
    uint32_t c = dirent_cluster(m, &de);
    while (c >= 2 && c < FAT32_EOC) {
        uint32_t next = fat_get(m, c);
        fat_set(m, c, FAT32_FREE);
        c = next;
    }
    /* Mark it (and its long name) deleted */
    return dir_remove(m, dir_cluster, first, entry_idx);
}

/* Rename or move within the volume: new entries (long name and all) in
 * the new directory, the old ones deleted; a moved directory's ".."
 * follows it. */
static int fat32_vfs_rename(vfs_mount_t *mnt, const char *old, const char *nw)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;
    fat32_dirent_t de;
    uint32_t dir_cluster, entry_idx, first;
    if (dc_is_root(m, old)) return VFS_EINVAL;
    if (fat32_resolve_ex(m, old, &de, &dir_cluster, &entry_idx, &first, NULL) != VFS_OK)
        return VFS_ENOENT;

    uint32_t new_dir;
    const char *leaf;
    if (parent_dir(m, nw, &new_dir, &leaf) != 0 || !*leaf) return VFS_ENOENT;

    fat32_dirent_t other;
    uint32_t other_dir, other_idx;
    if (fat32_resolve_ex(m, nw, &other, &other_dir, &other_idx, NULL, NULL) == VFS_OK &&
        !(other_dir == dir_cluster && other_idx == entry_idx))
        return VFS_EINVAL;                             /* the target exists */

    uint32_t own = dirent_cluster(m, &de);
    if ((de.attr & FAT32_ATTR_DIR) && own >= 2) {      /* not into itself */
        for (uint32_t c = new_dir; c >= 2 && c != m->root_cluster; ) {
            if (c == own) return VFS_EINVAL;
            fat32_dirent_t dd;
            if (fat32_read_dirent(m, c, 1, &dd) != 0) break;   /* its ".." */
            c = dirent_cluster(m, &dd);
        }
    }

    /* the old entries go first, so a new name differing only in case
     * (or the same 8.3 alias) can be written */
    int r = dir_remove(m, dir_cluster, first, entry_idx);
    if (r != VFS_OK) return r;
    uint32_t at;
    r = dir_add(m, new_dir, leaf, &de, &at);
    if (r != VFS_OK) {
        /* put the old name back: nothing changed */
        char oldleaf[VFS_NAME_MAX + 1];
        const char *ol = old;
        for (const char *p = old; *p; p++) if (*p == '/') ol = p + 1;
        kstrncpy(oldleaf, ol, sizeof oldleaf);
        dir_add(m, dir_cluster, oldleaf, &de, &at);
        return r;
    }
    if ((de.attr & FAT32_ATTR_DIR) && own >= 2 && new_dir != dir_cluster) {
        fat32_dirent_t dd;
        if (fat32_read_dirent(m, own, 1, &dd) == 0 && dd.name[0] == '.' && dd.name[1] == '.') {
            put_le16((uint8_t *)&dd.cluster_hi, 0);
            set_dirent_cluster(m, &dd, new_dir == m->root_cluster ? 0 : new_dir);
            fat32_write_dirent(m, own, 1, &dd);
        }
    }
    return VFS_OK;
}

static int fat32_vfs_mkdir(vfs_mount_t *mnt, const char *path)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;

    uint32_t par_cluster;
    const char *dname;
    if (parent_dir(m, path, &par_cluster, &dname) != 0) return VFS_ENOENT;
    if (!*dname) return VFS_EINVAL;
    if (fat32_resolve(m, path, NULL, NULL, NULL, NULL) == VFS_OK) return VFS_EINVAL;

    /* Allocate new cluster for the directory */
    uint32_t nc = fat_alloc(m);
    if (!nc) return VFS_ENOSPC;
    fat_set(m, nc, FAT32_EOC);

    /* Create the dirent in parent */
    fat32_dirent_t de;
    kmemset(&de, 0, sizeof(de));
    de.attr = FAT32_ATTR_DIR;
    set_dirent_cluster(m, &de, nc);
    fat_stamp(&de, 1);
    uint32_t at;
    int r = dir_add(m, par_cluster, dname, &de, &at);
    if (r != VFS_OK) { fat_set(m, nc, FAT32_FREE); return r; }

    /* Write . and .. entries */
    fat32_dirent_t dot;
    kmemset(&dot, 0, sizeof(dot));
    kmemset(dot.name, ' ', 8); kmemset(dot.ext, ' ', 3);
    dot.name[0] = '.'; dot.attr = FAT32_ATTR_DIR;
    set_dirent_cluster(m, &dot, nc);
    fat32_write_dirent(m, nc, 0, &dot);

    /* ".." points at the parent - or at cluster 0 when the parent is the
     * root, as the FAT spec requires (FAT16 and FAT32 alike). */
    dot.name[1] = '.';
    uint32_t dotdot = par_cluster == m->root_cluster ? 0 : par_cluster;
    put_le16((uint8_t *)&dot.cluster_hi, 0);
    set_dirent_cluster(m, &dot, dotdot);
    fat32_write_dirent(m, nc, 1, &dot);

    return VFS_OK;
}

/* ── Directory iteration ──────────────────────────────────────────────────── */

static int fat32_vfs_opendir(vfs_mount_t *mnt, const char *path, void **dpriv)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;

    fat32_dir_t *d = NULL;
    for (int i = 0; i < FAT32_MAX_OPEN_DIRS; i++) {
        if (!open_dirs[i].mnt) { d = &open_dirs[i]; break; }
    }
    if (!d) return VFS_ENOSPC;

    uint32_t cluster = m->root_cluster;
    if (kstrcmp(path, "/") != 0 && *path) {
        fat32_dirent_t de;
        if (fat32_resolve(m, path, &de, NULL, NULL, NULL) != VFS_OK)
            return VFS_ENOENT;
        if (!(de.attr & FAT32_ATTR_DIR)) return VFS_ENOTDIR;
        cluster = dirent_cluster(m, &de);
        if (cluster < 2) cluster = m->root_cluster;
    }

    d->mnt       = m;
    d->cluster   = cluster;
    d->entry_idx = 0;
    *dpriv       = d;
    return VFS_OK;
}

static int fat32_vfs_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent)
{
    (void)mnt;
    fat32_dir_t *d = (fat32_dir_t *)dpriv;
    fat32_dirent_t de;
    uint32_t first, at;
    /* vfs_dirent_t only carries the name - callers that need size or
     * attributes (e.g. os2api's DosFindFirst) vfs_stat() each entry. */
    while (dir_next(d->mnt, d->cluster, &d->entry_idx, &de, ent->name, VFS_NAME_MAX + 1,
                    &first, &at) == 0) {
        if (!kstrcmp(ent->name, ".") || !kstrcmp(ent->name, "..")) continue;
        return VFS_OK;
    }
    return VFS_ENOENT;
}

static int fat32_vfs_closedir(vfs_mount_t *mnt, void *dpriv)
{
    (void)mnt;
    fat32_dir_t *d = (fat32_dir_t *)dpriv;
    kmemset(d, 0, sizeof(*d));
    return VFS_OK;
}

static int fat32_vfs_statfs(vfs_mount_t *mnt, vfs_statfs_t *st)
{
    fat32_mount_t *m = (fat32_mount_t *)mnt->priv;
    __UINT64_TYPE__ free = fat_count_free(m, 0);
    st->bsize = m->bytes_per_cluster;
    st->blocks = m->total_clusters;
    st->bfree = free;
    /* The label is in the boot sector's extended BPB (offset 43 on FAT16,
     * 71 on FAT32), space-padded; "NO NAME" means none. */
    static uint8_t boot[SECTOR_SIZE];
    if (blk_read(m->dev, 0, 1, boot) == 0 && boot[m->fat_bits == 16 ? 38 : 66] == 0x29) {
        const uint8_t *l = boot + (m->fat_bits == 16 ? 43 : 71);
        int n = 11;
        while (n > 0 && l[n - 1] == ' ') n--;
        for (int i = 0; i < n; i++) st->label[i] = (l[i] >= 32 && l[i] < 127) ? (char)l[i] : '?';
        st->label[n] = 0;
        {
            static const char none[] = "NO NAME";
            int same = n == 7;
            for (int i = 0; same && i < 7; i++) same = st->label[i] == none[i];
            if (same) st->label[0] = 0;
        }
    }
    return VFS_OK;
}

/* ── VFS driver struct ────────────────────────────────────────────────────── */

vfs_fs_t vfs_fat32_fs = {
    .name     = "fat",
    .mount    = fat32_vfs_mount,
    .unmount  = fat32_vfs_unmount,
    .open     = fat32_vfs_open,
    .close    = fat32_vfs_close,
    .read     = fat32_vfs_read,
    .write    = fat32_vfs_write,
    .seek     = fat32_vfs_seek,
    .stat     = fat32_vfs_stat,
    .unlink   = fat32_vfs_unlink,
    .rename   = fat32_vfs_rename,
    .mkdir    = fat32_vfs_mkdir,
    .opendir  = fat32_vfs_opendir,
    .readdir  = fat32_vfs_readdir,
    .closedir = fat32_vfs_closedir,
    .statfs   = fat32_vfs_statfs,
};

/* ── Driver init ──────────────────────────────────────────────────────────── */

int fat32_init(const char *params)
{
    (void)params;

    /* A GPT-partitioned disk (see gpt.c) keeps a FAT "BOOT" partition as a
     * safety/recovery area mounted at "/BOOT" - FAT so firmware and any
     * host OS can read and repair it. The main "/" drive is partition 2,
     * owned by ext4_init() or btrfs_init(). More FAT volumes (e.g. USB
     * sticks) can be mounted later with the shell's MOUNT command. */
    static fat32_mount_t boot_mnt;

    blkdev_t *dev = blkdev_get_by_name("BOOT");
    if (!dev) {
        kprintf("[FAT] No BOOT partition found; not mounting\n");
        return 0;
    }

    kmemset(&boot_mnt, 0, sizeof(boot_mnt));
    boot_mnt.dev = dev;
    int r = vfs_mount("/BOOT", &vfs_fat32_fs, &boot_mnt);
    if (r != VFS_OK) {
        kprintf("[FAT] Mount failed: %d\n", r);
    }
    return 0; /* not fatal to boot */
}
