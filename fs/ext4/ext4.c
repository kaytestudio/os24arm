/**
re * @file ext4.c
 * @brief ext4 Filesystem Driver - mount, read, and directory/inode helpers
 *
 * Supports: superblock/group-descriptor parsing, path resolution, directory
 * listing, and file reads via both extent trees (the modern, default inode
 * layout) and classic direct/indirect block pointers.
 *
 * Write support (mkdir/create/write/truncate/unlink/rename) lives in
 * ext4_write.c, sharing this file's mount state and a few helpers via
 * ext4_internal.h - see that file's header comment for what it does and
 * does not maintain. Whether a given mounted image is actually writable
 * (ext4_mnt->writable) is decided once, at mount time, by
 * ext4_write_init_allocators(); every write entry point below falls back to
 * VFS_ERR_ROFS when it isn't.
 */

#include "ext4.h"
#include "ext4_internal.h"
#include "string.h"
#include "terminal.h"
#include "uart.h"
#include "blkdev.h"

/* Configuration */
static uint32_t cache_size_kb = 1024;

/* Forward declarations */
static int ext4_mount_impl(vfs_mount_t *mnt);
static int ext4_unmount_impl(vfs_mount_t *mnt);
static int ext4_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv);
static int ext4_close(vfs_mount_t *mnt, void *fpriv);
static int ext4_read(vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual);
static int ext4_write(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual);
static int ext4_opendir(vfs_mount_t *mnt, const char *path, void **dpriv);
static int ext4_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent);
static int ext4_closedir(vfs_mount_t *mnt, void *dpriv);
static int ext4_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st);
static int ext4_unlink(vfs_mount_t *mnt, const char *path);
static int ext4_mkdir(vfs_mount_t *mnt, const char *path);
static int ext4_rename(vfs_mount_t *mnt, const char *old, const char *nw);

/* VFS Operations */
static vfs_fs_t ext4_fs_ops = {
    .name     = "ext4",
    .mount    = ext4_mount_impl,
    .unmount  = ext4_unmount_impl,
    .open     = ext4_open,
    .close    = ext4_close,
    .read     = ext4_read,
    .write    = ext4_write,
    .opendir  = ext4_opendir,
    .readdir  = ext4_readdir,
    .closedir = ext4_closedir,
    .stat     = ext4_stat,
    .unlink   = ext4_unlink,
    .mkdir    = ext4_mkdir,
    .rename   = ext4_rename,
};

/* Index-node entry for extent-tree interior nodes (not exposed in ext4.h,
 * since it's an implementation detail of extent traversal). */
typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed)) ext4_extent_idx_t;

/* ── Block I/O ───────────────────────────────────────────────────────────── */

static int ext4_read_block_raw(blkdev_t *dev, uint32_t block_size, uint64_t block_num, void *buf)
{
    uint32_t sectors_per_block = block_size / dev->sector_size;
    if (sectors_per_block == 0) sectors_per_block = 1;
    int64_t sector = (int64_t)(block_num * sectors_per_block);
    return blk_read(dev, sector, sectors_per_block, buf);
}

int ext4_read_block(ext4_mount_t *mnt, uint64_t block_num, void *buf)
{
    return ext4_read_block_raw((blkdev_t *)mnt->dev, mnt->block_size, block_num, buf);
}

/* ── Inode I/O ───────────────────────────────────────────────────────────── */

int ext4_read_inode(ext4_mount_t *mnt, uint32_t ino, ext4_inode_t *out)
{
    if (ino == 0) return VFS_ERR_INVAL;

    uint32_t group = (ino - 1) / mnt->inodes_per_group;
    uint32_t index = (ino - 1) % mnt->inodes_per_group;
    if (group >= mnt->group_count) return VFS_ERR_INVAL;

    uint64_t inode_table_block = mnt->group_desc[group].bg_inode_table_lo |
                                  ((uint64_t)mnt->group_desc[group].bg_inode_table_hi << 32);
    uint64_t byte_off = inode_table_block * mnt->block_size + (uint64_t)index * mnt->inode_size;
    uint64_t block_num = byte_off / mnt->block_size;
    uint32_t block_off = (uint32_t)(byte_off % mnt->block_size);

    if (ext4_read_block(mnt, block_num, mnt->block_buffer) != 0) return VFS_ERR_IO;

    memset(out, 0, sizeof(*out));
    size_t copy_len = sizeof(ext4_inode_t);
    if (copy_len > mnt->inode_size) copy_len = mnt->inode_size;
    memcpy(out, mnt->block_buffer + block_off, copy_len);
    return VFS_OK;
}

/* ── Logical -> physical block mapping ──────────────────────────────────── */

/* Walk the inode's extent tree for the physical block backing logical_block.
 * *out_phys is set to 0 for a hole (sparse region with no allocated block). */
static int ext4_extent_lookup(ext4_mount_t *mnt, ext4_inode_t *inode, uint32_t logical_block, uint64_t *out_phys)
{
    uint8_t *node = (uint8_t *)inode->i_block;

    for (;;) {
        ext4_extent_header_t *h = (ext4_extent_header_t *)node;
        if (h->eh_magic != EXT4_EXTENT_MAGIC) return VFS_ERR_IO;

        if (h->eh_depth == 0) {
            ext4_extent_t *ext = (ext4_extent_t *)(node + sizeof(ext4_extent_header_t));
            for (uint16_t i = 0; i < h->eh_entries; i++) {
                uint32_t first = ext[i].ee_block;
                uint32_t count = ext[i].ee_len;
                if (count > 32768) count -= 32768; /* unwritten-extent marker bit */
                if (logical_block >= first && logical_block < first + count) {
                    uint64_t phys_start = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
                    *out_phys = phys_start + (logical_block - first);
                    return VFS_OK;
                }
            }
            *out_phys = 0;
            return VFS_OK;
        }

        ext4_extent_idx_t *idx = (ext4_extent_idx_t *)(node + sizeof(ext4_extent_header_t));
        int chosen = -1;
        for (uint16_t i = 0; i < h->eh_entries; i++) {
            if (idx[i].ei_block <= logical_block) chosen = i;
            else break;
        }
        if (chosen < 0) {
            *out_phys = 0;
            return VFS_OK;
        }

        uint64_t child_block = ((uint64_t)idx[chosen].ei_leaf_hi << 32) | idx[chosen].ei_leaf_lo;
        if (ext4_read_block(mnt, child_block, mnt->extent_buffer) != 0) return VFS_ERR_IO;
        node = mnt->extent_buffer;
    }
}

/* Classic (pre-extent) direct/indirect/double-indirect/triple-indirect
 * block mapping, for inodes without EXT4_EXTENTS_FL. */
static int ext4_classic_lookup(ext4_mount_t *mnt, ext4_inode_t *inode, uint32_t logical_block, uint64_t *out_phys)
{
    uint32_t ptrs_per_block = mnt->block_size / 4;

    if (logical_block < EXT4_NDIR_BLOCKS) {
        *out_phys = inode->i_block[logical_block];
        return VFS_OK;
    }
    logical_block -= EXT4_NDIR_BLOCKS;

    if (logical_block < ptrs_per_block) {
        uint32_t ind = inode->i_block[EXT4_IND_BLOCK];
        if (ind == 0) { *out_phys = 0; return VFS_OK; }
        if (ext4_read_block(mnt, ind, mnt->extent_buffer) != 0) return VFS_ERR_IO;
        *out_phys = ((uint32_t *)mnt->extent_buffer)[logical_block];
        return VFS_OK;
    }
    logical_block -= ptrs_per_block;

    if (logical_block < ptrs_per_block * ptrs_per_block) {
        uint32_t dind = inode->i_block[EXT4_DIND_BLOCK];
        if (dind == 0) { *out_phys = 0; return VFS_OK; }
        if (ext4_read_block(mnt, dind, mnt->extent_buffer) != 0) return VFS_ERR_IO;
        uint32_t idx1 = logical_block / ptrs_per_block;
        uint32_t idx2 = logical_block % ptrs_per_block;
        uint32_t ind = ((uint32_t *)mnt->extent_buffer)[idx1];
        if (ind == 0) { *out_phys = 0; return VFS_OK; }
        if (ext4_read_block(mnt, ind, mnt->extent_buffer) != 0) return VFS_ERR_IO;
        *out_phys = ((uint32_t *)mnt->extent_buffer)[idx2];
        return VFS_OK;
    }
    logical_block -= ptrs_per_block * ptrs_per_block;

    uint32_t tind = inode->i_block[EXT4_TIND_BLOCK];
    if (tind == 0) { *out_phys = 0; return VFS_OK; }
    if (ext4_read_block(mnt, tind, mnt->extent_buffer) != 0) return VFS_ERR_IO;
    uint32_t idx0 = logical_block / (ptrs_per_block * ptrs_per_block);
    uint32_t rem = logical_block % (ptrs_per_block * ptrs_per_block);
    uint32_t dind = ((uint32_t *)mnt->extent_buffer)[idx0];
    if (dind == 0) { *out_phys = 0; return VFS_OK; }
    if (ext4_read_block(mnt, dind, mnt->extent_buffer) != 0) return VFS_ERR_IO;
    uint32_t idx1 = rem / ptrs_per_block;
    uint32_t idx2 = rem % ptrs_per_block;
    uint32_t ind = ((uint32_t *)mnt->extent_buffer)[idx1];
    if (ind == 0) { *out_phys = 0; return VFS_OK; }
    if (ext4_read_block(mnt, ind, mnt->extent_buffer) != 0) return VFS_ERR_IO;
    *out_phys = ((uint32_t *)mnt->extent_buffer)[idx2];
    return VFS_OK;
}

int ext4_map_block(ext4_mount_t *mnt, ext4_inode_t *inode, uint32_t logical_block, uint64_t *out_phys)
{
    if (inode->i_flags & EXT4_EXTENTS_FL) {
        return ext4_extent_lookup(mnt, inode, logical_block, out_phys);
    }
    return ext4_classic_lookup(mnt, inode, logical_block, out_phys);
}

/* Read one filesystem block's worth of an inode's data (by logical block
 * index) into buf. Holes read as zero. */
int ext4_get_file_block(ext4_mount_t *mnt, ext4_inode_t *inode, uint32_t logical_block, uint8_t *buf)
{
    uint64_t phys = 0;
    int r = ext4_map_block(mnt, inode, logical_block, &phys);
    if (r != VFS_OK) return r;
    if (phys == 0) {
        memset(buf, 0, mnt->block_size);
        return VFS_OK;
    }
    return (ext4_read_block(mnt, phys, buf) == 0) ? VFS_OK : VFS_ERR_IO;
}

uint64_t ext4_inode_size(const ext4_inode_t *inode)
{
    return ((uint64_t)inode->i_size_high << 32) | inode->i_size_lo;
}

/* ── File data read ──────────────────────────────────────────────────────── */

static int ext4_inode_pread(ext4_mount_t *mnt, ext4_inode_t *inode, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_actual)
{
    uint64_t file_size = ext4_inode_size(inode);
    if (offset >= file_size) {
        *out_actual = 0;
        return VFS_OK;
    }
    if (offset + len > file_size) len = (uint32_t)(file_size - offset);

    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (total < len) {
        uint64_t cur_off = offset + total;
        uint32_t logical_block = (uint32_t)(cur_off / mnt->block_size);
        uint32_t block_off = (uint32_t)(cur_off % mnt->block_size);
        uint32_t chunk = mnt->block_size - block_off;
        if (chunk > len - total) chunk = len - total;

        if (ext4_get_file_block(mnt, inode, logical_block, mnt->block_buffer) != VFS_OK) {
            *out_actual = total;
            return VFS_ERR_IO;
        }
        memcpy(dst + total, mnt->block_buffer + block_off, chunk);
        total += chunk;
    }
    *out_actual = total;
    return VFS_OK;
}

/* ── Directory lookup / iteration ───────────────────────────────────────── */

uint32_t ext4_dir_lookup(ext4_mount_t *mnt, ext4_inode_t *dir_inode, const char *name, int name_len)
{
    uint64_t file_size = ext4_inode_size(dir_inode);
    uint32_t num_blocks = (uint32_t)((file_size + mnt->block_size - 1) / mnt->block_size);

    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        if (ext4_get_file_block(mnt, dir_inode, lb, mnt->block_buffer) != VFS_OK) return 0;

        uint32_t off = 0;
        while (off + sizeof(uint32_t) + 4 <= mnt->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(mnt->block_buffer + off);
            if (de->rec_len < 8) break; /* corrupt entry; stop scanning this block */
            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, (size_t)name_len) == 0) {
                return de->inode;
            }
            off += de->rec_len;
        }
    }
    return 0;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

int ext4_resolve_path(ext4_mount_t *mnt, const char *path, uint32_t *out_ino, ext4_inode_t *out_inode)
{
    uint32_t cur_ino = EXT4_ROOT_INO;
    ext4_inode_t cur_inode;
    if (ext4_read_inode(mnt, cur_ino, &cur_inode) != VFS_OK) return VFS_ERR_IO;

    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        if (len > 0) {
            if ((cur_inode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;
            if (len > EXT4_NAME_LEN) return VFS_ERR_NOTFOUND;

            uint32_t next_ino = ext4_dir_lookup(mnt, &cur_inode, start, len);
            if (next_ino == 0) return VFS_ERR_NOTFOUND;
            if (ext4_read_inode(mnt, next_ino, &cur_inode) != VFS_OK) return VFS_ERR_IO;
            cur_ino = next_ino;
        }
        while (*p == '/') p++;
    }

    *out_ino = cur_ino;
    *out_inode = cur_inode;
    return VFS_OK;
}

/* ── Mount / unmount ─────────────────────────────────────────────────────── */

static int ext4_mount_impl(vfs_mount_t *mnt)
{
    if (!mnt || !mnt->priv) return VFS_ERR_INVAL;
    blkdev_t *dev = (blkdev_t *)mnt->priv;

    terminal_writestring("[EXT4] Mounting filesystem...\n");

    if (dev->sector_size == 0) return VFS_ERR_INVAL;

    uint8_t sb_raw[1024];
    int64_t sb_sector = EXT4_SUPER_OFFSET / dev->sector_size;
    uint32_t sb_sectors = 1024 / dev->sector_size;
    if (sb_sectors < 1) sb_sectors = 1;
    if (blk_read(dev, sb_sector, sb_sectors, sb_raw) != 0) {
        terminal_writestring("[EXT4] ERROR: Failed to read superblock\n");
        return VFS_ERR_IO;
    }

    ext4_superblock_t *sb = (ext4_superblock_t *)sb_raw;
    if (sb->s_magic != EXT4_SUPER_MAGIC) {
        terminal_writestring("[EXT4] ERROR: Bad superblock magic (not ext4)\n");
        return VFS_ERR_IO;
    }

    uint32_t block_size = 1024u << sb->s_log_block_size;
    if (block_size < EXT4_MIN_BLOCK_SIZE || block_size > EXT4_MAX_BLOCK_SIZE) {
        terminal_writestring("[EXT4] ERROR: Invalid block size\n");
        return VFS_ERR_IO;
    }

    uint64_t blocks_count = sb->s_blocks_count_lo | ((uint64_t)sb->s_blocks_count_hi << 32);
    uint32_t blocks_per_group = sb->s_blocks_per_group ? sb->s_blocks_per_group : 1;
    uint32_t group_count = (uint32_t)((blocks_count + blocks_per_group - 1) / blocks_per_group);
    if (group_count == 0) group_count = 1;

    uint32_t desc_size = 32;
    if ((sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) && sb->s_desc_size >= 64) {
        desc_size = sb->s_desc_size;
    }

    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mem_alloc(sizeof(ext4_mount_t));
    if (!ext4_mnt) return VFS_ERR_NOSPACE;
    memset(ext4_mnt, 0, sizeof(ext4_mount_t));

    ext4_mnt->dev = dev;
    memcpy(&ext4_mnt->sb, sb, sizeof(ext4_mnt->sb));
    ext4_mnt->block_size = block_size;
    ext4_mnt->inode_size = sb->s_inode_size ? sb->s_inode_size : 128;
    ext4_mnt->inodes_per_group = sb->s_inodes_per_group;
    ext4_mnt->blocks_per_group = blocks_per_group;
    ext4_mnt->group_count = group_count;
    ext4_mnt->desc_per_block = block_size / desc_size;
    ext4_mnt->desc_size = desc_size;

    ext4_mnt->block_buffer = (uint8_t *)mem_alloc(block_size);
    ext4_mnt->extent_buffer = (uint8_t *)mem_alloc(block_size);
    ext4_mnt->bitmap_buffer = (uint8_t *)mem_alloc(block_size);
    ext4_mnt->group_desc = (ext4_group_desc_t *)mem_alloc((size_t)group_count * sizeof(ext4_group_desc_t));
    if (!ext4_mnt->block_buffer || !ext4_mnt->extent_buffer || !ext4_mnt->bitmap_buffer || !ext4_mnt->group_desc) {
        if (ext4_mnt->block_buffer) mem_free(ext4_mnt->block_buffer);
        if (ext4_mnt->extent_buffer) mem_free(ext4_mnt->extent_buffer);
        if (ext4_mnt->bitmap_buffer) mem_free(ext4_mnt->bitmap_buffer);
        if (ext4_mnt->group_desc) mem_free(ext4_mnt->group_desc);
        mem_free(ext4_mnt);
        return VFS_ERR_NOSPACE;
    }
    memset(ext4_mnt->group_desc, 0, (size_t)group_count * sizeof(ext4_group_desc_t));

    uint64_t gdt_start_block = (uint64_t)sb->s_first_data_block + 1;
    for (uint32_t g = 0; g < group_count; g++) {
        uint64_t byte_off = gdt_start_block * block_size + (uint64_t)g * desc_size;
        uint64_t blk = byte_off / block_size;
        uint32_t off_in_blk = (uint32_t)(byte_off % block_size);

        if (ext4_read_block_raw(dev, block_size, blk, ext4_mnt->block_buffer) != 0) {
            terminal_writestring("[EXT4] ERROR: Failed to read group descriptors\n");
            mem_free(ext4_mnt->block_buffer);
            mem_free(ext4_mnt->extent_buffer);
            mem_free(ext4_mnt->bitmap_buffer);
            mem_free(ext4_mnt->group_desc);
            mem_free(ext4_mnt);
            return VFS_ERR_IO;
        }
        size_t copy_len = desc_size < sizeof(ext4_group_desc_t) ? desc_size : sizeof(ext4_group_desc_t);
        memcpy(&ext4_mnt->group_desc[g], ext4_mnt->block_buffer + off_in_blk, copy_len);
    }

    mnt->priv = ext4_mnt;

    /* Bootstrap the write engine (fs/ext4/ext4_write.c) - sets
     * ext4_mnt->writable, falling back to read-only when the on-disk
     * feature flags are outside what it knows how to write to safely. */
    ext4_write_init_allocators(ext4_mnt);

    kprintf("[EXT4] Mounted: %u KB blocks, %u groups, %u inodes, %s\n",
            block_size / 1024, group_count, ext4_mnt->sb.s_inodes_count,
            ext4_mnt->writable ? "read/write" : "read-only");
    return VFS_OK;
}

static int ext4_unmount_impl(vfs_mount_t *mnt)
{
    if (!mnt || !mnt->priv) return VFS_ERR_INVAL;

    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mnt->priv;

    if (ext4_mnt->group_desc) mem_free(ext4_mnt->group_desc);
    if (ext4_mnt->extent_buffer) mem_free(ext4_mnt->extent_buffer);
    if (ext4_mnt->bitmap_buffer) mem_free(ext4_mnt->bitmap_buffer);
    if (ext4_mnt->block_buffer) mem_free(ext4_mnt->block_buffer);
    mem_free(ext4_mnt);

    mnt->priv = NULL;
    terminal_writestring("[EXT4] Unmounted\n");
    return VFS_OK;
}

/* ── File operations ─────────────────────────────────────────────────────── */

static int ext4_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv)
{
    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mnt->priv;

    /* Write/create opens hand off to the write engine (ext4_write.c).
     * Checked before path resolution: a filesystem this driver decided not
     * to write to (ext4_mnt->writable == 0 - see ext4_write_init_allocators)
     * should report "read-only" rather than a misleading "not found" for
     * the common case of creating a brand new file. */
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) {
        if (!ext4_mnt->writable) return VFS_ERR_ROFS;
        ext4_file_t *f = NULL;
        int r = ext4_write_open(ext4_mnt, path, flags, &f);
        if (r != VFS_OK) return r;
        *fpriv = f;
        return VFS_OK;
    }

    uint32_t ino;
    ext4_inode_t inode;
    int r = ext4_resolve_path(ext4_mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;

    if ((inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;

    ext4_file_t *f = (ext4_file_t *)mem_alloc(sizeof(ext4_file_t));
    if (!f) return VFS_ERR_NOSPACE;

    f->mnt = ext4_mnt;
    f->inode_num = ino;
    f->inode = inode;
    f->pos = 0;
    f->flags = flags;

    *fpriv = f;
    return VFS_OK;
}

static int ext4_close(vfs_mount_t *mnt, void *fpriv)
{
    (void)mnt;
    if (!fpriv) return VFS_ERR_BADFD;
    mem_free(fpriv);
    return VFS_OK;
}

static int ext4_read(vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    ext4_file_t *f = (ext4_file_t *)fpriv;
    if (!f) { *actual = 0; return VFS_ERR_BADFD; }

    uint32_t got = 0;
    int r = ext4_inode_pread(f->mnt, &f->inode, f->pos, buf, len, &got);
    f->pos += got;
    *actual = got;
    return r;
}

static int ext4_write(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    ext4_file_t *f = (ext4_file_t *)fpriv;
    if (!f) { *actual = 0; return VFS_ERR_BADFD; }
    return ext4_write_data(f, buf, len, actual);
}

/* ── Directory operations ────────────────────────────────────────────────── */

static int ext4_opendir(vfs_mount_t *mnt, const char *path, void **dpriv)
{
    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mnt->priv;

    uint32_t ino;
    ext4_inode_t inode;
    int r = ext4_resolve_path(ext4_mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;
    if ((inode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    ext4_file_t *file = (ext4_file_t *)mem_alloc(sizeof(ext4_file_t));
    if (!file) return VFS_ERR_NOSPACE;
    file->mnt = ext4_mnt;
    file->inode_num = ino;
    file->inode = inode;
    file->pos = 0;
    file->flags = O_RDONLY;

    ext4_dir_t *d = (ext4_dir_t *)mem_alloc(sizeof(ext4_dir_t));
    if (!d) { mem_free(file); return VFS_ERR_NOSPACE; }
    d->mnt = ext4_mnt;
    d->dir_file = file;
    d->pos = 0;

    *dpriv = d;
    return VFS_OK;
}

static int ext4_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent)
{
    (void)mnt;
    ext4_dir_t *d = (ext4_dir_t *)dpriv;
    if (!d) return VFS_ERR_BADFD;

    ext4_inode_t *dir_inode = &d->dir_file->inode;
    uint64_t file_size = ext4_inode_size(dir_inode);

    for (;;) {
        if (d->pos >= file_size) return VFS_ERR_NOTFOUND;

        uint32_t lb = (uint32_t)(d->pos / d->mnt->block_size);
        uint32_t off = (uint32_t)(d->pos % d->mnt->block_size);

        if (ext4_get_file_block(d->mnt, dir_inode, lb, d->mnt->block_buffer) != VFS_OK)
            return VFS_ERR_IO;

        ext4_dirent_t *de = (ext4_dirent_t *)(d->mnt->block_buffer + off);
        if (de->rec_len < 8) {
            /* Corrupt entry; skip to the next block. */
            d->pos = (uint64_t)(lb + 1) * d->mnt->block_size;
            continue;
        }

        d->pos += de->rec_len;

        if (de->inode == 0 || de->name_len == 0) continue;
        if (de->name_len == 1 && de->name[0] == '.') continue;
        if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') continue;

        int n = de->name_len;
        if (n > VFS_NAME_MAX) n = VFS_NAME_MAX;
        memcpy(ent->name, de->name, (size_t)n);
        ent->name[n] = '\0';
        return VFS_OK;
    }
}

static int ext4_closedir(vfs_mount_t *mnt, void *dpriv)
{
    (void)mnt;
    if (!dpriv) return VFS_ERR_BADFD;
    ext4_dir_t *d = (ext4_dir_t *)dpriv;
    if (d->dir_file) mem_free(d->dir_file);
    mem_free(d);
    return VFS_OK;
}

/* ── Metadata ─────────────────────────────────────────────────────────────── */

static int ext4_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st)
{
    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mnt->priv;

    uint32_t ino;
    ext4_inode_t inode;
    int r = ext4_resolve_path(ext4_mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;

    memset(st, 0, sizeof(*st));

    /* Basename: last path component. */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    int n = 0;
    while (base[n] && n < VFS_NAME_MAX) { st->name[n] = base[n]; n++; }
    st->name[n] = '\0';

    uint64_t size = ext4_inode_size(&inode);
    st->size = (size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)size;
    st->attrs = ((inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) ? VFS_ATTR_DIR : 0;
    return VFS_OK;
}

static int ext4_unlink(vfs_mount_t *mnt, const char *path)
{
    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mnt->priv;
    if (!ext4_mnt->writable) return VFS_ERR_ROFS;
    return ext4_do_unlink(ext4_mnt, path);
}

static int ext4_mkdir(vfs_mount_t *mnt, const char *path)
{
    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mnt->priv;
    if (!ext4_mnt->writable) return VFS_ERR_ROFS;
    return ext4_do_mkdir(ext4_mnt, path);
}

static int ext4_rename(vfs_mount_t *mnt, const char *old, const char *nw)
{
    ext4_mount_t *ext4_mnt = (ext4_mount_t *)mnt->priv;
    if (!ext4_mnt->writable) return VFS_ERR_ROFS;
    return ext4_do_rename(ext4_mnt, old, nw);
}

/* ── Driver init / VFS wiring ────────────────────────────────────────────── */

/* Minimal substring search (string.h has no strstr in this freestanding build) */
static const char *find_substr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    for (const char *p = hay; *p; p++) {
        if (strncmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

/* Parse a decimal "/CACHE:NNNN" option out of an IFS parameter string, if
 * present. Not fatal if absent or malformed - just keeps the default. */
static void parse_cache_option(const char *params)
{
    const char *p = find_substr(params, "/CACHE:");
    if (!p) return;
    p += 7;
    uint32_t val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (uint32_t)(*p - '0');
        p++;
    }
    if (val > 0) cache_size_kb = val;
}

/* Returns nonzero if dev carries an ext2/3/4 superblock magic. */
static int ext4_probe(blkdev_t *dev)
{
    if (dev->sector_size == 0) return 0;

    uint8_t sb_raw[1024];
    int64_t sb_sector = EXT4_SUPER_OFFSET / dev->sector_size;
    uint32_t sb_sectors = 1024 / dev->sector_size;
    if (sb_sectors < 1) sb_sectors = 1;
    if (blk_read(dev, sb_sector, sb_sectors, sb_raw) != 0) return 0;

    return ((ext4_superblock_t *)sb_raw)->s_magic == EXT4_SUPER_MAGIC;
}

int ext4_init(const char *params)
{
    uart_puts("DEBUG: Entering ext4_init\r\n");

    if (params && params[0]) {
        uart_puts("DEBUG: ext4_init has params\r\n");
        parse_cache_option(params);
    }

    /* A GPT-partitioned disk (see gpt.c) has a FAT "BOOT" partition
     * (mounted at "/BOOT" by fat32_init()) and the main "/" drive on
     * partition "C", which may be ext4 or btrfs: try it here as ext4 -
     * the superblock magic check in the mount rejects a btrfs "C", which
     * btrfs_init() then mounts instead. Only without a "C" partition does
     * this fall back to the raw "disk0" device, so flat, unpartitioned
     * ext4-only test images keep working exactly as before. */
    blkdev_t *dev = blkdev_get_by_name("C");
    const char *mount_point = "/";
    if (!dev && !blkdev_get_by_name("BOOT")) {
        dev = blkdev_get_by_name("disk0");
    }
    if (!dev) {
        kprintf("[EXT4] No block device found; not mounting\n");
        uart_puts("DEBUG: ext4_init complete\r\n");
        return 0;
    }

    /* "C" may legitimately be btrfs - probe the magic first so that case
     * is a quiet skip rather than a "Bad superblock magic" error. */
    if (!ext4_probe(dev)) {
        kprintf("[EXT4] %s is not ext4; not mounting\n", dev->name);
        uart_puts("DEBUG: ext4_init complete\r\n");
        return 0;
    }

    int r = vfs_mount(mount_point, ext4_get_fs(), dev);
    if (r != VFS_OK) {
        kprintf("[EXT4] Mount failed: %d\n", r);
        uart_puts("DEBUG: ext4_init complete\r\n");
        return 0; /* not fatal to boot */
    }

    uart_puts("DEBUG: ext4_init complete\r\n");
    return 0;
}

vfs_fs_t* ext4_get_fs(void)
{
    return &ext4_fs_ops;
}
