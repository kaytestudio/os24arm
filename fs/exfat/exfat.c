/* ─────────────────────────────────────────────────────────────
 * exfat.c  –  exFAT read/write driver (C11)
 *
 * exFAT is Microsoft's "Extended File Allocation Table" used on
 * large SDXC/USB drives and is mandatory for SDXC compliance.
 *
 * Key differences from FAT32:
 *   • No 4 GiB file size limit
 *   • No root directory entry count limit
 *   • Unicode filenames natively (UTF-16)
 *   • Allocation bitmap instead of FAT scan for free clusters
 *   • Cluster-based data regions (no root sector offset)
 * ───────────────────────────────────────────────────────────── */
#include "exfat.h"
#include "../../include/string.h"
#include "../../include/terminal.h"
#include "../../include/blkdev.h"

/* ── I/O primitives ──────────────────────────────────────────── */
static int exfat_read_sector(vfs_mount_t *mnt, uint64_t lba, void *buf)
{
    return blkdev_read(mnt->dev, lba, 1, buf) == BLKERR_OK
           ? VFS_OK : VFS_ERR_IO;
}
static int exfat_write_sector(vfs_mount_t *mnt, uint64_t lba, const void *buf)
{
    return blkdev_write(mnt->dev, lba, 1, buf) == BLKERR_OK
           ? VFS_OK : VFS_ERR_IO;
}

static int exfat_read_cluster(vfs_mount_t *mnt, uint32_t cluster, void *buf)
{
    exfat_fs_t *fs = mnt->fs_priv;
    uint64_t lba   = exfat_cluster_lba(fs, cluster);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; ++s) {
        if (blkdev_read(mnt->dev, lba + s, 1,
                        (uint8_t*)buf + s * fs->bytes_per_sector) != BLKERR_OK)
            return VFS_ERR_IO;
    }
    return VFS_OK;
}

static int exfat_write_cluster(vfs_mount_t *mnt, uint32_t cluster, const void *buf)
{
    exfat_fs_t *fs = mnt->fs_priv;
    uint64_t lba   = exfat_cluster_lba(fs, cluster);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; ++s) {
        if (blkdev_write(mnt->dev, lba + s, 1,
                         (const uint8_t*)buf + s * fs->bytes_per_sector) != BLKERR_OK)
            return VFS_ERR_IO;
    }
    return VFS_OK;
}

/* ── FAT operations ──────────────────────────────────────────── */
static uint32_t exfat_fat_get(vfs_mount_t *mnt, uint32_t cluster)
{
    exfat_fs_t *fs     = mnt->fs_priv;
    uint32_t byte_off  = cluster * 4;
    uint32_t sector    = (uint32_t)(fs->part_lba + fs->fat_offset)
                         + byte_off / fs->bytes_per_sector;
    uint32_t off_in    = byte_off % fs->bytes_per_sector;
    uint8_t  sec[512];
    if (blkdev_read(mnt->dev, sector, 1, sec) != BLKERR_OK) return EXFAT_CLUSTER_BAD;
    return *((uint32_t*)(sec + off_in));
}

static int exfat_fat_set(vfs_mount_t *mnt, uint32_t cluster, uint32_t value)
{
    exfat_fs_t *fs    = mnt->fs_priv;
    uint32_t byte_off = cluster * 4;
    uint32_t sector   = (uint32_t)(fs->part_lba + fs->fat_offset)
                        + byte_off / fs->bytes_per_sector;
    uint32_t off_in   = byte_off % fs->bytes_per_sector;
    uint8_t  sec[512];
    if (blkdev_read(mnt->dev, sector, 1, sec) != BLKERR_OK) return VFS_ERR_IO;
    *((uint32_t*)(sec + off_in)) = value;
    return blkdev_write(mnt->dev, sector, 1, sec) == BLKERR_OK ? VFS_OK : VFS_ERR_IO;
}

static uint32_t exfat_chain_nth(vfs_mount_t *mnt, uint32_t start, uint32_t n)
{
    uint32_t cur = start;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t next = exfat_fat_get(mnt, cur);
        if (next == EXFAT_CLUSTER_EOC || next == EXFAT_CLUSTER_FREE) return 0;
        cur = next;
    }
    return cur;
}

/* ── Cluster allocation (FAT + allocation bitmap) ────────────── */
static uint32_t exfat_alloc_cluster(vfs_mount_t *mnt, uint32_t prev)
{
    exfat_fs_t *fs = mnt->fs_priv;

    for (uint32_t c = 2; c < fs->cluster_count + 2; ++c) {
        if (exfat_fat_get(mnt, c) == EXFAT_CLUSTER_FREE) {
            exfat_fat_set(mnt, c, EXFAT_CLUSTER_EOC);
            if (prev) exfat_fat_set(mnt, prev, c);
            /* Zero cluster */
            uint8_t zero[512]; kmemset(zero, 0, 512);
            uint64_t lba = exfat_cluster_lba(fs, c);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; ++s)
                blkdev_write(mnt->dev, lba + s, 1, zero);
            return c;
        }
    }
    return 0;
}

static void exfat_free_chain(vfs_mount_t *mnt, uint32_t start)
{
    uint32_t cur = start;
    while (cur && cur != EXFAT_CLUSTER_EOC) {
        uint32_t next = exfat_fat_get(mnt, cur);
        exfat_fat_set(mnt, cur, EXFAT_CLUSTER_FREE);
        cur = next;
    }
}

/* ── UTF-16 → ASCII helper ───────────────────────────────────── */
static void utf16_to_ascii(const uint16_t *src, int count, char *dst, int dst_sz)
{
    int i;
    for (i = 0; i < count && i < dst_sz - 1; ++i) {
        uint16_t c = src[i];
        if (c == 0) break;
        dst[i] = (c < 128) ? (char)c : '?';
    }
    dst[i] = '\0';
}

/* ASCII → UTF-16 */
static void ascii_to_utf16(const char *src, uint16_t *dst, int dst_words)
{
    int i;
    for (i = 0; src[i] && i < dst_words - 1; ++i) dst[i] = (uint16_t)(uint8_t)src[i];
    dst[i] = 0;
}

/* ── Directory scan ──────────────────────────────────────────── */
typedef struct {
    uint32_t cluster;
    uint32_t entry_idx;    /* index within cluster (32-byte units) */
    uint32_t first_cluster;
    uint32_t cluster_idx;  /* which cluster in chain               */
} exfat_dirpos_t;

/* Walk directory to find 'name'. Returns 0=found, VFS_ERR_NOENT, etc. */
static int exfat_find_entry(vfs_mount_t *mnt, uint32_t dir_cluster,
                             const char *name,
                             exfat_file_entry_t *out_fe,
                             exfat_stream_ext_t *out_se,
                             exfat_dirpos_t     *out_pos)
{
    exfat_fs_t *fs       = mnt->fs_priv;
    uint32_t    eps      = fs->bytes_per_cluster / 32;
    uint32_t    cluster  = dir_cluster;
    uint32_t    ci       = 0;

    /* Temp buffer for one cluster */
    uint8_t *buf = (uint8_t*)_Alignas(4) (uint8_t[4096]){0};
    /* Stack VLA limit — use static buffer */
    static uint8_t _cluster_buf[65536];  /* up to 64K cluster */
    buf = _cluster_buf;

    while (cluster && cluster != EXFAT_CLUSTER_EOC) {
        if (exfat_read_cluster(mnt, cluster, buf) != VFS_OK) return VFS_ERR_IO;

        for (uint32_t ei = 0; ei < eps; ++ei) {
            exfat_entry_t *e = (exfat_entry_t*)(buf + ei * 32);
            if (e->type == 0x00) return VFS_ERR_NOENT;  /* end of dir */
            if (e->type != EXFAT_ET_FILE) continue;

            exfat_file_entry_t *fe = (exfat_file_entry_t*)e;
            if (fe->secondary_count < 2) continue;

            /* Next entry must be stream ext */
            if (ei + 1 >= eps) continue;  /* Entry sets crossing a cluster boundary are not supported. */
            exfat_stream_ext_t *se = (exfat_stream_ext_t*)(buf + (ei+1)*32);
            if (se->type != EXFAT_ET_STREAM_EXT) continue;

            /* Gather name from file name entries */
            char fname[256]; fname[0] = '\0';
            int name_chars = se->name_length;
            int name_pos   = 0;
            for (int k = 2; k < fe->secondary_count && name_chars > 0; ++k) {
                if (ei + k >= eps) break;
                exfat_name_entry_t *ne = (exfat_name_entry_t*)(buf + (ei+k)*32);
                if (ne->type != EXFAT_ET_FILE_NAME) break;
                int take = (name_chars > 15) ? 15 : name_chars;
                utf16_to_ascii(ne->name, take, fname + name_pos, 256 - name_pos);
                name_pos   += take;
                name_chars -= take;
            }

            if (kstrcmp(fname, name) == 0) {
                if (out_fe)  *out_fe = *fe;
                if (out_se)  *out_se = *se;
                if (out_pos) {
                    out_pos->cluster       = cluster;
                    out_pos->entry_idx     = ei;
                    out_pos->first_cluster = dir_cluster;
                    out_pos->cluster_idx   = ci;
                }
                return VFS_OK;
            }
        }

        cluster = exfat_fat_get(mnt, cluster);
        ++ci;
    }
    return VFS_ERR_NOENT;
}

/* ── VFS callbacks ────────────────────────────────────────────── */
static bool exfat_probe(blkdev_t *dev, uint64_t part_lba)
{
    uint8_t boot[512];
    if (blkdev_read(dev, part_lba, 1, boot) != BLKERR_OK) return false;
    return kmemcmp(boot + 3, "EXFAT   ", 8) == 0;
}

static int exfat_mount(vfs_mount_t *mnt)
{
    static exfat_fs_t fs_pool[4];
    static bool       fs_used[4] = {false};
    exfat_fs_t *fs = NULL;
    for (int i = 0; i < 4; ++i)
        if (!fs_used[i]) { fs = &fs_pool[i]; fs_used[i] = true; break; }
    if (!fs) return VFS_ERR_NOMEM;

    uint8_t boot[512];
    if (blkdev_read(mnt->dev, mnt->part_lba, 1, boot) != BLKERR_OK) return VFS_ERR_IO;
    exfat_boot_t *b = (exfat_boot_t*)boot;

    kmemset(fs, 0, sizeof(*fs));
    fs->part_lba            = mnt->part_lba;
    fs->bytes_per_sector    = 1u << b->bytes_per_sector_shift;
    fs->sectors_per_cluster = 1u << b->sectors_per_cluster_shift;
    fs->bytes_per_cluster   = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->fat_offset          = b->fat_offset;
    fs->cluster_heap_offset = b->cluster_heap_offset;
    fs->cluster_count       = b->cluster_count;
    fs->root_dir_cluster    = b->root_dir_cluster;

    mnt->fs_priv   = fs;
    mnt->root.inode = fs->root_dir_cluster;
    mnt->root.mode  = VFS_S_IFDIR | 0755;
    mnt->root.mount = mnt;

    term_printf("[exFAT] Mounted. bps=%u spc=%u clusters=%u rootclus=%u\n",
                fs->bytes_per_sector, fs->sectors_per_cluster,
                fs->cluster_count, fs->root_dir_cluster);
    return VFS_OK;
}

static int exfat_lookup(vfs_mount_t *mnt, vfs_node_t *dir,
                        const char *name, vfs_node_t *out)
{
    exfat_file_entry_t fe;
    exfat_stream_ext_t se;
    int rc = exfat_find_entry(mnt, (uint32_t)dir->inode, name, &fe, &se, NULL);
    if (rc != VFS_OK) return rc;

    out->inode = se.first_cluster;
    out->size  = se.data_length;
    out->mount = mnt;
    out->mode  = (fe.file_attributes & EXFAT_FA_DIR)
                 ? (VFS_S_IFDIR | 0755)
                 : (VFS_S_IFREG | ((fe.file_attributes & EXFAT_FA_READ_ONLY) ? 0444 : 0644));
    return VFS_OK;
}

static int64_t exfat_read(vfs_mount_t *mnt, vfs_node_t *node,
                          uint64_t offset, uint64_t size, void *buf)
{
    exfat_fs_t *fs = mnt->fs_priv;
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    uint32_t skip     = (uint32_t)(offset / fs->bytes_per_cluster);
    uint32_t cluster  = exfat_chain_nth(mnt, (uint32_t)node->inode, skip);
    if (!cluster) return 0;

    uint32_t off_in = (uint32_t)(offset % fs->bytes_per_cluster);
    uint64_t done   = 0;
    static uint8_t _cbuf[65536];

    while (done < size && cluster && cluster != EXFAT_CLUSTER_EOC) {
        if (exfat_read_cluster(mnt, cluster, _cbuf) != VFS_OK) break;
        uint64_t to_copy = fs->bytes_per_cluster - off_in;
        if (to_copy > size - done) to_copy = size - done;
        kmemcpy((uint8_t*)buf + done, _cbuf + off_in, (size_t)to_copy);
        done   += to_copy;
        off_in  = 0;
        cluster = exfat_fat_get(mnt, cluster);
    }
    return (int64_t)done;
}

static int64_t exfat_write(vfs_mount_t *mnt, vfs_node_t *node,
                           uint64_t offset, uint64_t size, const void *buf)
{
    if (!buf && size == 0) {
        exfat_free_chain(mnt, (uint32_t)node->inode);
        node->size = 0; return 0;
    }

    exfat_fs_t *fs    = mnt->fs_priv;
    uint32_t cluster  = (uint32_t)node->inode;
    uint64_t done     = 0;

    /* Navigate to / extend to starting cluster */
    uint32_t skip = (uint32_t)(offset / fs->bytes_per_cluster);
    uint32_t cur = cluster, prev = 0;
    for (uint32_t i = 0; i <= skip; ++i) {
        if (!cur || cur == EXFAT_CLUSTER_EOC) {
            cur = exfat_alloc_cluster(mnt, prev);
            if (!cur) return done > 0 ? (int64_t)done : VFS_ERR_NOSPC;
            if (i == 0) { node->inode = cur; cluster = cur; }
        }
        if (i < skip) { prev = cur; cur = exfat_fat_get(mnt, cur); }
    }

    uint32_t off_in  = (uint32_t)(offset % fs->bytes_per_cluster);
    uint32_t wcl     = cur;
    static uint8_t _cbuf[65536];

    while (done < size) {
        if (!wcl || wcl == EXFAT_CLUSTER_EOC)
            wcl = exfat_alloc_cluster(mnt, prev);
        if (!wcl) break;

        uint64_t to_copy = fs->bytes_per_cluster - off_in;
        if (to_copy > size - done) to_copy = size - done;

        if (off_in || to_copy < fs->bytes_per_cluster)
            exfat_read_cluster(mnt, wcl, _cbuf);
        kmemcpy(_cbuf + off_in, (const uint8_t*)buf + done, (size_t)to_copy);
        exfat_write_cluster(mnt, wcl, _cbuf);

        done  += to_copy;
        off_in = 0;
        prev   = wcl;
        wcl    = exfat_fat_get(mnt, wcl);
    }

    if (offset + done > node->size) node->size = offset + done;
    return (int64_t)done;
}

static int exfat_readdir(vfs_mount_t *mnt, vfs_node_t *dir,
                         uint64_t *cookie, vfs_dirent_t *out)
{
    exfat_fs_t *fs  = mnt->fs_priv;
    uint32_t    eps = fs->bytes_per_cluster / 32;
    uint32_t    cluster = exfat_chain_nth(mnt, (uint32_t)dir->inode,
                                          (uint32_t)(*cookie / eps));
    if (!cluster || cluster == EXFAT_CLUSTER_EOC) return 1;

    static uint8_t _cbuf[65536];
    if (exfat_read_cluster(mnt, cluster, _cbuf) != VFS_OK) return VFS_ERR_IO;

    for (uint32_t ei = (uint32_t)(*cookie % eps); ei < eps; ++ei) {
        exfat_entry_t *e = (exfat_entry_t*)(_cbuf + ei * 32);
        if (e->type == 0x00) return 1;  /* end */
        if (e->type != EXFAT_ET_FILE) { (*cookie)++; continue; }

        exfat_file_entry_t *fe = (exfat_file_entry_t*)e;
        if (ei + 1 >= eps || fe->secondary_count < 2) { (*cookie)++; continue; }
        exfat_stream_ext_t *se = (exfat_stream_ext_t*)(_cbuf + (ei+1)*32);
        if (se->type != EXFAT_ET_STREAM_EXT) { (*cookie)++; continue; }

        /* Collect filename */
        char fname[256]; fname[0] = '\0'; int pos = 0;
        int nleft = se->name_length;
        for (int k = 2; k < fe->secondary_count && nleft > 0; ++k) {
            if (ei + (uint32_t)k >= eps) break;
            exfat_name_entry_t *ne = (exfat_name_entry_t*)(_cbuf + (ei + k)*32);
            if (ne->type != EXFAT_ET_FILE_NAME) break;
            int take = nleft > 15 ? 15 : nleft;
            utf16_to_ascii(ne->name, take, fname + pos, 256 - pos);
            pos += take; nleft -= take;
        }

        *cookie = (uint64_t)((*cookie / eps) * eps + ei + 1);

        kstrncpy(out->name, fname, VFS_NAME_MAX);
        out->inode = se->first_cluster;
        out->type  = (fe->file_attributes & EXFAT_FA_DIR) ? DT_DIR : DT_REG;
        return VFS_OK;
    }
    return 1;
}

static int exfat_create(vfs_mount_t *mnt, vfs_node_t *dir,
                        const char *name, uint32_t mode, vfs_node_t *out)
{
    exfat_fs_t *fs    = mnt->fs_priv;
    bool        is_dir = VFS_ISDIR(mode);
    uint32_t    eps    = fs->bytes_per_cluster / 32;

    uint32_t new_cluster = exfat_alloc_cluster(mnt, 0);
    if (!new_cluster) return VFS_ERR_NOSPC;

    /* Count name entries needed (15 chars per entry) */
    int name_len   = (int)kstrlen(name);
    int name_entries = (name_len + 14) / 15;
    int total_secondary = 1 + name_entries;  /* 1 stream + N name */

    /* Find a run of (1+total_secondary) free slots in parent dir */
    uint32_t dir_cluster = (uint32_t)dir->inode;
    uint32_t cluster = dir_cluster, prev = 0;
    static uint8_t _cbuf[65536];

    bool inserted = false;
    for (uint32_t ci = 0; ; ++ci) {
        if (!cluster || cluster == EXFAT_CLUSTER_EOC) {
            cluster = exfat_alloc_cluster(mnt, prev);
            if (!cluster) { exfat_free_chain(mnt, new_cluster); return VFS_ERR_NOSPC; }
        }
        if (exfat_read_cluster(mnt, cluster, _cbuf) != VFS_OK) return VFS_ERR_IO;

        for (uint32_t ei = 0; ei + (uint32_t)total_secondary < eps; ++ei) {
            /* Check if all slots are free */
            bool free_run = true;
            for (int k = 0; k <= total_secondary; ++k) {
                exfat_entry_t *e = (exfat_entry_t*)(_cbuf + (ei + (uint32_t)k)*32);
                if (e->type != 0x00 && !(e->type & 0x80)) { free_run = false; break; }
            }
            if (!free_run && (((exfat_entry_t*)(_cbuf+ei*32))->type & 0x80)) continue;
            if (!free_run) continue;

            /* Write file entry */
            exfat_file_entry_t *fe = (exfat_file_entry_t*)(_cbuf + ei*32);
            kmemset(fe, 0, 32);
            fe->type            = EXFAT_ET_FILE;
            fe->secondary_count = (uint8_t)total_secondary;
            fe->file_attributes = is_dir ? EXFAT_FA_DIR : EXFAT_FA_ARCHIVE;

            /* Stream extension */
            exfat_stream_ext_t *se = (exfat_stream_ext_t*)(_cbuf + (ei+1)*32);
            kmemset(se, 0, 32);
            se->type            = EXFAT_ET_STREAM_EXT;
            se->general_flags   = 0x01;  /* allocation possible */
            se->name_length     = (uint8_t)name_len;
            se->first_cluster   = new_cluster;
            se->data_length     = 0;
            se->valid_data_length = 0;

            /* Name entries */
            int npos = 0, nrem = name_len;
            for (int k = 0; k < name_entries; ++k) {
                exfat_name_entry_t *ne =
                    (exfat_name_entry_t*)(_cbuf + (ei + 2 + k)*32);
                kmemset(ne, 0, 32);
                ne->type = EXFAT_ET_FILE_NAME;
                int take = nrem > 15 ? 15 : nrem;
                ascii_to_utf16(name + npos, ne->name, take + 1);
                npos += take; nrem -= take;
            }

            exfat_write_cluster(mnt, cluster, _cbuf);
            inserted = true;
            break;
        }
        if (inserted) break;
        prev    = cluster;
        cluster = exfat_fat_get(mnt, cluster);
    }

    if (!inserted) { exfat_free_chain(mnt, new_cluster); return VFS_ERR_IO; }

    out->inode = new_cluster;
    out->mode  = mode;
    out->size  = 0;
    out->mount = mnt;
    return VFS_OK;
}

static int exfat_unlink(vfs_mount_t *mnt, vfs_node_t *dir, const char *name)
{
    exfat_file_entry_t fe;
    exfat_stream_ext_t se;
    exfat_dirpos_t     pos;

    int rc = exfat_find_entry(mnt, (uint32_t)dir->inode, name, &fe, &se, &pos);
    if (rc != VFS_OK) return rc;

    /* Free data chain */
    exfat_free_chain(mnt, se.first_cluster);

    /* Zero out the entire directory entry set */
    exfat_fs_t *fs = mnt->fs_priv;
    uint32_t    eps = fs->bytes_per_cluster / 32;
    static uint8_t _cbuf[65536];
    if (exfat_read_cluster(mnt, pos.cluster, _cbuf) != VFS_OK) return VFS_ERR_IO;
    int total = 1 + fe.secondary_count;
    for (int k = 0; k < total && pos.entry_idx + k < eps; ++k) {
        ((exfat_entry_t*)(_cbuf + (pos.entry_idx + k)*32))->type = 0x00;
    }
    return exfat_write_cluster(mnt, pos.cluster, _cbuf);
}

static int exfat_stat(vfs_mount_t *mnt, vfs_node_t *node, vfs_stat_t *out)
{
    exfat_fs_t *fs = mnt->fs_priv;
    out->inode   = node->inode;
    out->size    = node->size;
    out->mode    = node->mode;
    out->uid     = 0; out->gid = 0;
    out->nlink   = 1;
    out->blksize = fs->bytes_per_cluster;
    out->blocks  = (node->size + 511) / 512;
    return VFS_OK;
}

static int exfat_statfs(vfs_mount_t *mnt,
                        uint64_t *tot, uint64_t *fblks, uint32_t *bsz)
{
    exfat_fs_t *fs = mnt->fs_priv;
    *bsz = fs->bytes_per_cluster;
    *tot = fs->cluster_count;
    uint32_t free_cnt = 0;
    for (uint32_t c = 2; c < fs->cluster_count + 2; ++c)
        if (exfat_fat_get(mnt, c) == EXFAT_CLUSTER_FREE) free_cnt++;
    *fblks = free_cnt;
    return VFS_OK;
}

static vfs_driver_t exfat_driver = {
    .name    = "exfat",
    .probe   = exfat_probe,
    .mount   = exfat_mount,
    .umount  = NULL,
    .lookup  = exfat_lookup,
    .read    = exfat_read,
    .write   = exfat_write,
    .create  = exfat_create,
    .unlink  = exfat_unlink,
    .readdir = exfat_readdir,
    .stat    = exfat_stat,
    .sync    = NULL,
    .statfs  = exfat_statfs,
};

vfs_driver_t *exfat_get_driver(void) { return &exfat_driver; }
