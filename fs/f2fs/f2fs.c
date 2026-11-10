/**
 * @file f2fs.c
 * @brief f2fs Filesystem Driver - mount, checkpoint validation, node/NAT
 *        traversal, path resolution, read, and directory listing.
 *
 * See fs/f2fs/f2fs_disk.h for the on-disk subset this driver understands
 * and fs/f2fs/f2fs_write.c for what write support (mkdir/create/write/
 * truncate/unlink/rename, in that file) does and does not maintain.
 * Whether a mounted image is actually writable (f2fs_mnt->writable) is
 * decided once, at mount time, by f2fs_write_init_allocators(); every
 * write entry point below falls back to VFS_ERR_ROFS when it isn't.
 *
 * f2fs directories do not store "." or ".." entries on disk (real f2fs
 * calls this the "implicit dot dentries" scheme - parent linkage lives in
 * i_pino instead), so unlike fs/ext4/ext4.c this driver's directory scan
 * never needs to special-case skipping them.
 */

#include "f2fs.h"
#include "f2fs_internal.h"
#include "string.h"
#include "terminal.h"
#include "blkdev.h"

/* Forward declarations */
static int f2fs_mount_impl(vfs_mount_t *mnt);
static int f2fs_unmount_impl(vfs_mount_t *mnt);
static int f2fs_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv);
static int f2fs_close(vfs_mount_t *mnt, void *fpriv);
static int f2fs_read(vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual);
static int f2fs_write(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual);
static int f2fs_opendir(vfs_mount_t *mnt, const char *path, void **dpriv);
static int f2fs_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent);
static int f2fs_closedir(vfs_mount_t *mnt, void *dpriv);
static int f2fs_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st);
static int f2fs_unlink(vfs_mount_t *mnt, const char *path);
static int f2fs_mkdir(vfs_mount_t *mnt, const char *path);
static int f2fs_rename(vfs_mount_t *mnt, const char *old, const char *nw);

static vfs_fs_t f2fs_fs_ops = {
    .name     = "f2fs",
    .mount    = f2fs_mount_impl,
    .unmount  = f2fs_unmount_impl,
    .open     = f2fs_open,
    .close    = f2fs_close,
    .read     = f2fs_read,
    .write    = f2fs_write,
    .opendir  = f2fs_opendir,
    .readdir  = f2fs_readdir,
    .closedir = f2fs_closedir,
    .stat     = f2fs_stat,
    .unlink   = f2fs_unlink,
    .mkdir    = f2fs_mkdir,
    .rename   = f2fs_rename,
};

/* ── CRC32 ───────────────────────────────────────────────────────────────── */
/* Standard reflected CRC-32/ISO-HDLC table (the ubiquitous zlib/PNG/gzip
 * polynomial 0xEDB88320) - matches real f2fs's f2fs_crc32(), which wraps
 * the Linux kernel's generic crc32_le(): caller-supplied seed, no implicit
 * pre/post complement. */

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

uint32_t f2fs_crc32(uint32_t seed, const void *data, uint32_t len)
{
    if (!crc32_table_ready) crc32_init_table();
    uint32_t crc = seed;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* ── Block I/O ───────────────────────────────────────────────────────────── */

int f2fs_read_block(f2fs_mount_t *mnt, uint32_t blkaddr, void *buf)
{
    blkdev_t *dev = (blkdev_t *)mnt->dev;
    uint32_t sectors_per_block = F2FS_BLKSIZE / dev->sector_size;
    if (sectors_per_block == 0) sectors_per_block = 1;
    int64_t sector = (int64_t)((uint64_t)blkaddr * sectors_per_block);
    return (blk_read(dev, sector, sectors_per_block, buf) == 0) ? VFS_OK : VFS_ERR_IO;
}

/* ── NAT / node ──────────────────────────────────────────────────────────── */

int f2fs_get_node_blkaddr(f2fs_mount_t *mnt, uint32_t nid, uint32_t *out_blkaddr)
{
    uint32_t nat_block = nid / NAT_ENTRY_PER_BLOCK;
    uint32_t index = nid % NAT_ENTRY_PER_BLOCK;

    if (f2fs_read_block(mnt, mnt->sb.nat_blkaddr + nat_block, mnt->bitmap_buffer) != VFS_OK)
        return VFS_ERR_IO;

    struct f2fs_nat_block *nb = (struct f2fs_nat_block *)mnt->bitmap_buffer;
    *out_blkaddr = nb->entries[index].block_addr;
    return VFS_OK;
}

int f2fs_read_node(f2fs_mount_t *mnt, uint32_t nid, struct f2fs_node *out)
{
    uint32_t blkaddr;
    if (f2fs_get_node_blkaddr(mnt, nid, &blkaddr) != VFS_OK) return VFS_ERR_IO;
    if (blkaddr == NULL_ADDR || blkaddr == NEW_ADDR) return VFS_ERR_NOTFOUND;
    if (f2fs_read_block(mnt, blkaddr, out) != VFS_OK) return VFS_ERR_IO;
    return VFS_OK;
}

int f2fs_read_inode(f2fs_mount_t *mnt, uint32_t ino, struct f2fs_inode *out)
{
    struct f2fs_node node;
    int r = f2fs_read_node(mnt, ino, &node);
    if (r != VFS_OK) return r;
    memcpy(out, &node.i, sizeof(*out));
    return VFS_OK;
}

uint64_t f2fs_inode_size(const struct f2fs_inode *inode)
{
    return inode->i_size;
}

/* Logical -> physical block mapping. Levels 0-1 only (inline i_addr[] plus
 * the two direct node blocks NODE_DIR1/DIR2) - see f2fs_disk.h's header
 * comment. *out_phys is NULL_ADDR (0) for a hole. */
int f2fs_map_block(f2fs_mount_t *mnt, struct f2fs_inode *inode, uint32_t logical_block, uint32_t *out_phys)
{
    if (logical_block < DEF_ADDRS_PER_INODE) {
        *out_phys = inode->i_addr[logical_block];
        return VFS_OK;
    }
    logical_block -= DEF_ADDRS_PER_INODE;

    for (int slot = 0; slot < 2; slot++) {
        if (logical_block < DEF_ADDRS_PER_BLOCK) {
            uint32_t nid = inode->i_nid[slot];
            if (nid == 0) { *out_phys = 0; return VFS_OK; }
            struct f2fs_node dnode;
            if (f2fs_read_node(mnt, nid, &dnode) != VFS_OK) return VFS_ERR_IO;
            *out_phys = dnode.dn.addr[logical_block];
            return VFS_OK;
        }
        logical_block -= DEF_ADDRS_PER_BLOCK;
    }

    /* Beyond NODE_DIR1/DIR2 - this driver doesn't support IND1/IND2/DIND. */
    return VFS_ERR_NOSPACE;
}

int f2fs_get_file_block(f2fs_mount_t *mnt, struct f2fs_inode *inode, uint32_t logical_block, uint8_t *buf)
{
    uint32_t phys = 0;
    int r = f2fs_map_block(mnt, inode, logical_block, &phys);
    if (r != VFS_OK) return r;
    if (phys == NULL_ADDR) {
        memset(buf, 0, F2FS_BLKSIZE);
        return VFS_OK;
    }
    return f2fs_read_block(mnt, phys, buf);
}

/* ── File data read ──────────────────────────────────────────────────────── */

static int f2fs_inode_pread(f2fs_mount_t *mnt, struct f2fs_inode *inode, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_actual)
{
    uint64_t file_size = f2fs_inode_size(inode);
    if (offset >= file_size) { *out_actual = 0; return VFS_OK; }
    if (offset + len > file_size) len = (uint32_t)(file_size - offset);

    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (total < len) {
        uint64_t cur_off = offset + total;
        uint32_t logical_block = (uint32_t)(cur_off / F2FS_BLKSIZE);
        uint32_t block_off = (uint32_t)(cur_off % F2FS_BLKSIZE);
        uint32_t chunk = F2FS_BLKSIZE - block_off;
        if (chunk > len - total) chunk = len - total;

        if (f2fs_get_file_block(mnt, inode, logical_block, mnt->block_buffer) != VFS_OK) {
            *out_actual = total;
            return VFS_ERR_IO;
        }
        memcpy(dst + total, mnt->block_buffer + block_off, chunk);
        total += chunk;
    }
    *out_actual = total;
    return VFS_OK;
}

/* ── Directory lookup / iteration (flat, level-0/bucket-0 only) ──────────── */

#define GET_DENTRY_SLOTS(name_len) (((name_len) + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN)

static int dentry_bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

uint32_t f2fs_dir_lookup(f2fs_mount_t *mnt, struct f2fs_inode *dir_inode, const char *name, int name_len, uint8_t *out_type)
{
    uint64_t file_size = f2fs_inode_size(dir_inode);
    uint32_t num_blocks = (uint32_t)((file_size + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE);

    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        if (f2fs_get_file_block(mnt, dir_inode, lb, mnt->block_buffer) != VFS_OK) return 0;
        struct f2fs_dentry_block *db = (struct f2fs_dentry_block *)mnt->block_buffer;

        uint32_t bit = 0;
        while (bit < NR_DENTRY_IN_BLOCK) {
            if (!dentry_bitmap_test(db->dentry_bitmap, bit)) { bit++; continue; }
            struct f2fs_dir_entry *de = &db->dentry[bit];
            uint32_t slots = GET_DENTRY_SLOTS(de->name_len);
            if (de->name_len == (uint16_t)name_len &&
                memcmp(db->filename[bit], name, (size_t)name_len) == 0) {
                if (out_type) *out_type = de->file_type;
                return de->ino;
            }
            bit += (slots > 0) ? slots : 1;
        }
    }
    return 0;
}

/* *pos encodes (block * NR_DENTRY_IN_BLOCK + slot) rather than a byte
 * offset - NR_DENTRY_IN_BLOCK never divides F2FS_BLKSIZE evenly, so a
 * byte-offset convention (like ext4_dir_t's) doesn't map cleanly onto
 * fixed-size dentry slots. */
static int f2fs_readdir_step(f2fs_mount_t *mnt, struct f2fs_inode *dir_inode, uint32_t *pos,
                              uint32_t *out_ino, uint8_t *out_type, char *out_name, int *out_name_len)
{
    uint64_t file_size = f2fs_inode_size(dir_inode);

    for (;;) {
        uint32_t lb = *pos / NR_DENTRY_IN_BLOCK;
        uint32_t bit = *pos % NR_DENTRY_IN_BLOCK;
        if ((uint64_t)lb * F2FS_BLKSIZE >= file_size) return VFS_ERR_NOTFOUND;

        if (f2fs_get_file_block(mnt, dir_inode, lb, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;
        struct f2fs_dentry_block *db = (struct f2fs_dentry_block *)mnt->block_buffer;

        while (bit < NR_DENTRY_IN_BLOCK) {
            if (!dentry_bitmap_test(db->dentry_bitmap, bit)) { bit++; continue; }
            struct f2fs_dir_entry *de = &db->dentry[bit];
            uint32_t slots = GET_DENTRY_SLOTS(de->name_len);

            *out_ino = de->ino;
            if (out_type) *out_type = de->file_type;
            int n = de->name_len;
            if (n > VFS_NAME_MAX) n = VFS_NAME_MAX;
            memcpy(out_name, db->filename[bit], (size_t)n);
            out_name[n] = '\0';
            *out_name_len = n;

            *pos = lb * NR_DENTRY_IN_BLOCK + bit + (slots > 0 ? slots : 1);
            return VFS_OK;
        }

        *pos = (lb + 1) * NR_DENTRY_IN_BLOCK;
    }
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

int f2fs_resolve_path(f2fs_mount_t *mnt, const char *path, uint32_t *out_ino, struct f2fs_inode *out_inode)
{
    uint32_t cur_ino = mnt->sb.root_ino;
    struct f2fs_inode cur_inode;
    if (f2fs_read_inode(mnt, cur_ino, &cur_inode) != VFS_OK) return VFS_ERR_IO;

    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        if (len > 0) {
            if ((cur_inode.i_mode & F2FS_S_IFMT) != F2FS_S_IFDIR) return VFS_ERR_NOTDIR;
            if (len > F2FS_NAME_LEN) return VFS_ERR_NOTFOUND;

            uint32_t next_ino = f2fs_dir_lookup(mnt, &cur_inode, start, len, NULL);
            if (next_ino == 0) return VFS_ERR_NOTFOUND;
            if (f2fs_read_inode(mnt, next_ino, &cur_inode) != VFS_OK) return VFS_ERR_IO;
            cur_ino = next_ino;
        }
        while (*p == '/') p++;
    }

    *out_ino = cur_ino;
    *out_inode = cur_inode;
    return VFS_OK;
}

/* ── Checkpoint pack selection ───────────────────────────────────────────── */

static int f2fs_ckpt_probe(f2fs_mount_t *mnt, uint32_t blkaddr, uint64_t *out_version, uint8_t *scratch)
{
    if (f2fs_read_block(mnt, blkaddr, scratch) != VFS_OK) return VFS_ERR_IO;

    struct f2fs_checkpoint *cp = (struct f2fs_checkpoint *)scratch;
    uint32_t crc_offset = cp->checksum_offset;
    if (crc_offset < CP_MIN_CHKSUM_OFFSET || crc_offset > CP_CHKSUM_OFFSET) return VFS_ERR_IO;

    uint32_t stored_crc;
    memcpy(&stored_crc, scratch + crc_offset, sizeof(stored_crc));
    uint32_t crc = f2fs_crc32(F2FS_SUPER_MAGIC, scratch, crc_offset);
    if (crc != stored_crc) return VFS_ERR_IO;

    *out_version = cp->checkpoint_ver;
    return VFS_OK;
}

/* Returns VFS_OK with *out_version and header bytes in header_out (one
 * full block) if the pack starting at pack_start is internally consistent
 * (header and duplicated tail block agree on version - see this driver's
 * plan/f2fs_write.c for why: it's the crash-safety trick that lets mount
 * tell a fully-written checkpoint from one interrupted mid-write). */
static int f2fs_validate_pack(f2fs_mount_t *mnt, uint32_t pack_start, uint64_t *out_version, uint8_t *header_out)
{
    uint64_t pre_version;
    if (f2fs_ckpt_probe(mnt, pack_start, &pre_version, header_out) != VFS_OK) return VFS_ERR_IO;

    struct f2fs_checkpoint *cp = (struct f2fs_checkpoint *)header_out;
    uint32_t cp_blocks = cp->cp_pack_total_block_count;
    if (cp_blocks <= F2FS_CP_PACKS || cp_blocks > mnt->blocks_per_seg) return VFS_ERR_IO;

    uint8_t tail_scratch[F2FS_BLKSIZE];
    uint64_t cur_version;
    if (f2fs_ckpt_probe(mnt, pack_start + cp_blocks - 1, &cur_version, tail_scratch) != VFS_OK) return VFS_ERR_IO;

    if (cur_version != pre_version) return VFS_ERR_IO;
    *out_version = pre_version;
    return VFS_OK;
}

/* ver_after(a, b): true if checkpoint version a is strictly newer than b -
 * plain unsigned comparison (this driver's versions are a simple counter,
 * unlike real f2fs's wrapping-aware comparison, since it always increments
 * by exactly 1 per commit and never wraps in any realistic session). */
static int ver_after(uint64_t a, uint64_t b) { return a > b; }

static int f2fs_get_valid_checkpoint(f2fs_mount_t *mnt)
{
    uint32_t cp1_addr = mnt->sb.cp_blkaddr;
    uint32_t cp2_addr = cp1_addr + mnt->blocks_per_seg;

    uint8_t buf1[F2FS_BLKSIZE], buf2[F2FS_BLKSIZE];
    uint64_t v1 = 0, v2 = 0;
    int ok1 = (f2fs_validate_pack(mnt, cp1_addr, &v1, buf1) == VFS_OK);
    int ok2 = (f2fs_validate_pack(mnt, cp2_addr, &v2, buf2) == VFS_OK);

    uint8_t *winner;
    if (ok1 && ok2) {
        if (ver_after(v2, v1)) { winner = buf2; mnt->cur_cp_pack = 2; }
        else { winner = buf1; mnt->cur_cp_pack = 1; }
    } else if (ok1) {
        winner = buf1; mnt->cur_cp_pack = 1;
    } else if (ok2) {
        winner = buf2; mnt->cur_cp_pack = 2;
    } else {
        return VFS_ERR_IO;
    }

    memcpy(&mnt->ckpt, winner, sizeof(mnt->ckpt));
    return VFS_OK;
}

/* ── Mount / unmount ─────────────────────────────────────────────────────── */

static int f2fs_read_superblock(f2fs_mount_t *mnt, blkdev_t *dev)
{
    if (dev->sector_size == 0) return VFS_ERR_INVAL;

    uint8_t sb_raw[F2FS_BLKSIZE];
    int64_t sb_sector = F2FS_SUPER_OFFSET / dev->sector_size;
    uint32_t sb_sectors = F2FS_BLKSIZE / dev->sector_size;
    if (sb_sectors < 1) sb_sectors = 1;
    if (blk_read(dev, sb_sector, sb_sectors, sb_raw) != 0) return VFS_ERR_IO;

    struct f2fs_super_block *sb = (struct f2fs_super_block *)sb_raw;
    if (sb->magic != F2FS_SUPER_MAGIC) return VFS_ERR_IO;
    if (sb->log_blocksize != F2FS_BLKSIZE_BITS) return VFS_ERR_IO;
    if (sb->log_blocks_per_seg != 9) return VFS_ERR_IO; /* 512 blocks/seg - see f2fs_disk.h */
    if (sb->cp_payload != 0) return VFS_ERR_IO;

    if (sb->checksum_offset != 0) {
        uint32_t crc = f2fs_crc32(F2FS_SUPER_MAGIC, sb_raw, sb->checksum_offset);
        if (crc != sb->crc) return VFS_ERR_IO;
    }

    memcpy(&mnt->sb, sb, sizeof(mnt->sb));
    return VFS_OK;
}

static int f2fs_mount_impl(vfs_mount_t *vmnt)
{
    if (!vmnt || !vmnt->priv) return VFS_ERR_INVAL;
    blkdev_t *dev = (blkdev_t *)vmnt->priv;

    terminal_writestring("[F2FS] Mounting filesystem...\n");

    f2fs_mount_t *mnt = (f2fs_mount_t *)mem_alloc(sizeof(f2fs_mount_t));
    if (!mnt) return VFS_ERR_NOSPACE;
    memset(mnt, 0, sizeof(*mnt));
    mnt->dev = dev;

    int r = f2fs_read_superblock(mnt, dev);
    if (r != VFS_OK) {
        terminal_writestring("[F2FS] ERROR: bad or unsupported superblock\n");
        mem_free(mnt);
        return r;
    }

    mnt->blocks_per_seg = 1u << mnt->sb.log_blocks_per_seg;
    mnt->main_segments = mnt->sb.segment_count_main;

    mnt->block_buffer  = (uint8_t *)mem_alloc(F2FS_BLKSIZE);
    mnt->node_buffer   = (uint8_t *)mem_alloc(F2FS_BLKSIZE);
    mnt->bitmap_buffer = (uint8_t *)mem_alloc(F2FS_BLKSIZE);
    if (!mnt->block_buffer || !mnt->node_buffer || !mnt->bitmap_buffer) {
        if (mnt->block_buffer) mem_free(mnt->block_buffer);
        if (mnt->node_buffer) mem_free(mnt->node_buffer);
        if (mnt->bitmap_buffer) mem_free(mnt->bitmap_buffer);
        mem_free(mnt);
        return VFS_ERR_NOSPACE;
    }

    r = f2fs_get_valid_checkpoint(mnt);
    if (r != VFS_OK) {
        terminal_writestring("[F2FS] ERROR: no valid checkpoint pack\n");
        mem_free(mnt->block_buffer); mem_free(mnt->node_buffer); mem_free(mnt->bitmap_buffer);
        mem_free(mnt);
        return r;
    }

    vmnt->priv = mnt;

    f2fs_write_init_allocators(mnt);

    kprintf("[F2FS] Mounted: cp pack %d, ver %llu, %s\n",
            mnt->cur_cp_pack, (unsigned long long)mnt->ckpt.checkpoint_ver,
            mnt->writable ? "read/write" : "read-only");
    return VFS_OK;
}

static int f2fs_unmount_impl(vfs_mount_t *vmnt)
{
    if (!vmnt || !vmnt->priv) return VFS_ERR_INVAL;
    f2fs_mount_t *mnt = (f2fs_mount_t *)vmnt->priv;

    mem_free(mnt->block_buffer);
    mem_free(mnt->node_buffer);
    mem_free(mnt->bitmap_buffer);
    mem_free(mnt);

    vmnt->priv = NULL;
    terminal_writestring("[F2FS] Unmounted\n");
    return VFS_OK;
}

/* ── File operations ─────────────────────────────────────────────────────── */

static int f2fs_open(vfs_mount_t *vmnt, const char *path, int flags, void **fpriv)
{
    f2fs_mount_t *mnt = (f2fs_mount_t *)vmnt->priv;

    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) {
        if (!mnt->writable) return VFS_ERR_ROFS;
        f2fs_file_t *f = NULL;
        int r = f2fs_write_open(mnt, path, flags, &f);
        if (r != VFS_OK) return r;
        *fpriv = f;
        return VFS_OK;
    }

    uint32_t ino;
    struct f2fs_inode inode;
    int r = f2fs_resolve_path(mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;
    if ((inode.i_mode & F2FS_S_IFMT) == F2FS_S_IFDIR) return VFS_ERR_ISDIR;

    f2fs_file_t *f = (f2fs_file_t *)mem_alloc(sizeof(f2fs_file_t));
    if (!f) return VFS_ERR_NOSPACE;
    f->mnt = mnt;
    f->ino = ino;
    f->inode = inode;
    f->pos = 0;
    f->flags = flags;

    *fpriv = f;
    return VFS_OK;
}

static int f2fs_close(vfs_mount_t *vmnt, void *fpriv)
{
    (void)vmnt;
    if (!fpriv) return VFS_ERR_BADFD;
    mem_free(fpriv);
    return VFS_OK;
}

static int f2fs_read(vfs_mount_t *vmnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual)
{
    (void)vmnt;
    f2fs_file_t *f = (f2fs_file_t *)fpriv;
    if (!f) { *actual = 0; return VFS_ERR_BADFD; }

    uint32_t got = 0;
    int r = f2fs_inode_pread(f->mnt, &f->inode, f->pos, buf, len, &got);
    f->pos += got;
    *actual = got;
    return r;
}

static int f2fs_write(vfs_mount_t *vmnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual)
{
    (void)vmnt;
    f2fs_file_t *f = (f2fs_file_t *)fpriv;
    if (!f) { *actual = 0; return VFS_ERR_BADFD; }
    return f2fs_write_data(f, buf, len, actual);
}

/* ── Directory operations ────────────────────────────────────────────────── */

static int f2fs_opendir(vfs_mount_t *vmnt, const char *path, void **dpriv)
{
    f2fs_mount_t *mnt = (f2fs_mount_t *)vmnt->priv;

    uint32_t ino;
    struct f2fs_inode inode;
    int r = f2fs_resolve_path(mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;
    if ((inode.i_mode & F2FS_S_IFMT) != F2FS_S_IFDIR) return VFS_ERR_NOTDIR;

    f2fs_file_t *file = (f2fs_file_t *)mem_alloc(sizeof(f2fs_file_t));
    if (!file) return VFS_ERR_NOSPACE;
    file->mnt = mnt;
    file->ino = ino;
    file->inode = inode;
    file->pos = 0;
    file->flags = O_RDONLY;

    f2fs_dir_t *d = (f2fs_dir_t *)mem_alloc(sizeof(f2fs_dir_t));
    if (!d) { mem_free(file); return VFS_ERR_NOSPACE; }
    d->mnt = mnt;
    d->dir_file = file;
    d->pos = 0;

    *dpriv = d;
    return VFS_OK;
}

static int f2fs_readdir(vfs_mount_t *vmnt, void *dpriv, vfs_dirent_t *ent)
{
    (void)vmnt;
    f2fs_dir_t *d = (f2fs_dir_t *)dpriv;
    if (!d) return VFS_ERR_BADFD;

    uint32_t ino;
    uint8_t type;
    int name_len;
    int r = f2fs_readdir_step(d->mnt, &d->dir_file->inode, &d->pos, &ino, &type, ent->name, &name_len);
    return r;
}

static int f2fs_closedir(vfs_mount_t *vmnt, void *dpriv)
{
    (void)vmnt;
    if (!dpriv) return VFS_ERR_BADFD;
    f2fs_dir_t *d = (f2fs_dir_t *)dpriv;
    if (d->dir_file) mem_free(d->dir_file);
    mem_free(d);
    return VFS_OK;
}

/* ── Metadata ─────────────────────────────────────────────────────────────── */

static int f2fs_stat(vfs_mount_t *vmnt, const char *path, vfs_stat_t *st)
{
    f2fs_mount_t *mnt = (f2fs_mount_t *)vmnt->priv;

    uint32_t ino;
    struct f2fs_inode inode;
    int r = f2fs_resolve_path(mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;

    memset(st, 0, sizeof(*st));

    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    int n = 0;
    while (base[n] && n < VFS_NAME_MAX) { st->name[n] = base[n]; n++; }
    st->name[n] = '\0';

    uint64_t size = f2fs_inode_size(&inode);
    st->size = (size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)size;
    st->attrs = ((inode.i_mode & F2FS_S_IFMT) == F2FS_S_IFDIR) ? VFS_ATTR_DIR : 0;
    return VFS_OK;
}

static int f2fs_unlink(vfs_mount_t *vmnt, const char *path)
{
    f2fs_mount_t *mnt = (f2fs_mount_t *)vmnt->priv;
    if (!mnt->writable) return VFS_ERR_ROFS;
    return f2fs_do_unlink(mnt, path);
}

static int f2fs_mkdir(vfs_mount_t *vmnt, const char *path)
{
    f2fs_mount_t *mnt = (f2fs_mount_t *)vmnt->priv;
    if (!mnt->writable) return VFS_ERR_ROFS;
    return f2fs_do_mkdir(mnt, path);
}

static int f2fs_rename(vfs_mount_t *vmnt, const char *old, const char *nw)
{
    f2fs_mount_t *mnt = (f2fs_mount_t *)vmnt->priv;
    if (!mnt->writable) return VFS_ERR_ROFS;
    return f2fs_do_rename(mnt, old, nw);
}

/* ── Driver init / VFS wiring ────────────────────────────────────────────── */

int f2fs_init(const char *params)
{
    (void)params;
    blkdev_t *dev = blkdev_get_by_name("disk0");
    if (!dev) {
        kprintf("[F2FS] No block device found; not mounting\n");
        return 0;
    }
    int r = vfs_mount("/", f2fs_get_fs(), dev);
    if (r != VFS_OK) {
        kprintf("[F2FS] Mount failed: %d\n", r);
        return 0;
    }
    return 0;
}

vfs_fs_t *f2fs_get_fs(void)
{
    return &f2fs_fs_ops;
}
