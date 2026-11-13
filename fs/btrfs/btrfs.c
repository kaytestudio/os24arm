/**
 * @file btrfs.c
 * @brief btrfs Filesystem Driver (read/write) - mount, read, and the
 *        physical/logical I/O + B-tree search primitives shared with the
 *        write engine in btrfs_write.c.
 *
 * Supports a single device, uncompressed inline/regular extents, and a
 * flat default subvolume (no snapshots/nested subvolumes). Write support
 * (btrfs_write.c) maintains the chunk/root/fs trees with real
 * copy-on-write semantics, but deliberately skips the extent tree and
 * checksum tree - see btrfs_write.c's header comment for why, and for the
 * resulting real-world-interop caveat.
 *
 * Unlike ext4's flat block/inode tables, btrfs stores everything as items
 * in copy-on-write B-trees keyed by (objectid, type, offset). Addresses
 * inside these trees are *logical* and must be resolved to a physical
 * (device, offset) through the chunk tree - itself a B-tree, bootstrapped
 * via a small "system chunk array" embedded in the superblock so the
 * chunk tree's own root can be located before any chunk mapping exists.
 */

#include "btrfs.h"
#include "btrfs_internal.h"
#include "string.h"
#include "terminal.h"
#include "uart.h"
#include "blkdev.h"

/* Forward declarations */
static int btrfs_mount_impl(vfs_mount_t *mnt);
static int btrfs_unmount_impl(vfs_mount_t *mnt);
static int btrfs_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv);
static int btrfs_close(vfs_mount_t *mnt, void *fpriv);
static int btrfs_read(vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual);
static int btrfs_write(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual);
static int btrfs_seek(vfs_mount_t *mnt, void *fpriv, int32_t offset, int whence, uint32_t *newpos);
static int btrfs_opendir(vfs_mount_t *mnt, const char *path, void **dpriv);
static int btrfs_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent);
static int btrfs_closedir(vfs_mount_t *mnt, void *dpriv);
static int btrfs_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st);
static int btrfs_unlink(vfs_mount_t *mnt, const char *path);
static int btrfs_mkdir(vfs_mount_t *mnt, const char *path);
static int btrfs_rename(vfs_mount_t *mnt, const char *old, const char *nw);
static int btrfs_statfs(vfs_mount_t *mnt, vfs_statfs_t *st);

static vfs_fs_t btrfs_fs_ops = {
    .name     = "btrfs",
    .mount    = btrfs_mount_impl,
    .unmount  = btrfs_unmount_impl,
    .open     = btrfs_open,
    .close    = btrfs_close,
    .read     = btrfs_read,
    .write    = btrfs_write,
    .seek     = btrfs_seek,
    .opendir  = btrfs_opendir,
    .readdir  = btrfs_readdir,
    .closedir = btrfs_closedir,
    .stat     = btrfs_stat,
    .unlink   = btrfs_unlink,
    .mkdir    = btrfs_mkdir,
    .rename   = btrfs_rename,
    .statfs   = btrfs_statfs,
};

/* ── Key helpers ─────────────────────────────────────────────────────────── */

int key_cmp(const btrfs_key_t *a, const btrfs_key_t *b)
{
    if (a->objectid != b->objectid) return (a->objectid < b->objectid) ? -1 : 1;
    if (a->type != b->type) return (a->type < b->type) ? -1 : 1;
    if (a->offset != b->offset) return (a->offset < b->offset) ? -1 : 1;
    return 0;
}

btrfs_key_t key_successor(btrfs_key_t k)
{
    /* Smallest key that sorts strictly after k, for range-iteration by
     * re-search rather than maintaining a full tree-walk path/stack. */
    if (k.offset != 0xFFFFFFFFFFFFFFFFULL) {
        k.offset++;
    } else if (k.type != 0xFF) {
        k.type++;
        k.offset = 0;
    } else {
        k.objectid++;
        k.type = 0;
        k.offset = 0;
    }
    return k;
}

/* ── Physical / logical block I/O ───────────────────────────────────────── */

int btrfs_read_phys(blkdev_t *dev, uint64_t phys_offset, void *buf, uint32_t len)
{
    uint32_t ssz = dev->sector_size;
    uint64_t start_sector = phys_offset / ssz;
    uint32_t start_off = (uint32_t)(phys_offset % ssz);
    uint32_t total = start_off + len;
    uint32_t sector_count = (total + ssz - 1) / ssz;

    /* Fast path: aligned read directly into caller's buffer. */
    if (start_off == 0 && (len % ssz) == 0) {
        return blk_read(dev, (int64_t)start_sector, sector_count, buf) == 0 ? VFS_OK : VFS_ERR_IO;
    }

    /* Slow path: read the containing sector range into a bounce buffer.
     * Bounded by nodesize (4KiB-64KiB) in every actual caller, so a small
     * fixed on-stack buffer is safe. */
    uint8_t bounce[8192];
    if (sector_count * ssz > sizeof(bounce)) return VFS_ERR_IO;
    if (blk_read(dev, (int64_t)start_sector, sector_count, bounce) != 0) return VFS_ERR_IO;
    memcpy(buf, bounce + start_off, len);
    return VFS_OK;
}

/* Read-modify-write counterpart of btrfs_read_phys() - every write in this
 * driver is nodesize-aligned-and-sized (tree blocks) or sectorsize-aligned
 * (data extents, via the allocator in btrfs_write.c), but this still
 * handles the unaligned-tail case defensively via the same bounce buffer
 * approach as the read path. */
int btrfs_write_phys(blkdev_t *dev, uint64_t phys_offset, const void *buf, uint32_t len)
{
    uint32_t ssz = dev->sector_size;
    uint64_t start_sector = phys_offset / ssz;
    uint32_t start_off = (uint32_t)(phys_offset % ssz);
    uint32_t total = start_off + len;
    uint32_t sector_count = (total + ssz - 1) / ssz;

    if (start_off == 0 && (len % ssz) == 0) {
        return blk_write(dev, (int64_t)start_sector, sector_count, buf) == 0 ? VFS_OK : VFS_ERR_IO;
    }

    uint8_t bounce[8192];
    if (sector_count * ssz > sizeof(bounce)) return VFS_ERR_IO;
    if (blk_read(dev, (int64_t)start_sector, sector_count, bounce) != 0) return VFS_ERR_IO;
    memcpy(bounce + start_off, buf, len);
    if (blk_write(dev, (int64_t)start_sector, sector_count, bounce) != 0) return VFS_ERR_IO;
    return VFS_OK;
}

int btrfs_logical_to_phys(btrfs_mount_t *mnt, uint64_t logical, uint64_t *out_phys)
{
    for (int i = 0; i < mnt->chunk_count; i++) {
        btrfs_chunk_map_t *c = &mnt->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length) {
            *out_phys = c->phys + (logical - c->logical);
            return VFS_OK;
        }
    }
    return VFS_ERR_IO;
}

int btrfs_read_logical(btrfs_mount_t *mnt, uint64_t logical, void *buf, uint32_t len)
{
    uint64_t phys;
    if (btrfs_logical_to_phys(mnt, logical, &phys) != VFS_OK) return VFS_ERR_IO;
    return btrfs_read_phys((blkdev_t *)mnt->dev, phys, buf, len);
}

int btrfs_write_logical(btrfs_mount_t *mnt, uint64_t logical, const void *buf, uint32_t len)
{
    uint64_t phys;
    if (btrfs_logical_to_phys(mnt, logical, &phys) != VFS_OK) return VFS_ERR_IO;
    return btrfs_write_phys((blkdev_t *)mnt->dev, phys, buf, len);
}

int btrfs_read_node(btrfs_mount_t *mnt, uint64_t logical, uint8_t *buf)
{
    return btrfs_read_logical(mnt, logical, buf, mnt->nodesize);
}

int btrfs_write_node(btrfs_mount_t *mnt, uint64_t logical, const uint8_t *buf)
{
    return btrfs_write_logical(mnt, logical, buf, mnt->nodesize);
}

/* ── Chunk map ───────────────────────────────────────────────────────────── */

void add_chunk_map(btrfs_mount_t *mnt, uint64_t logical, uint64_t length, uint64_t phys, uint64_t type)
{
    if (mnt->chunk_count >= BTRFS_MAX_CHUNKS) return;
    mnt->chunks[mnt->chunk_count].logical = logical;
    mnt->chunks[mnt->chunk_count].length = length;
    mnt->chunks[mnt->chunk_count].phys = phys;
    mnt->chunks[mnt->chunk_count].type = type;
    mnt->chunk_count++;
}

/* Parse the raw system chunk array embedded in the superblock: a packed
 * sequence of (btrfs_key_t, btrfs_chunk_t-with-N-stripes) pairs. */
static void parse_system_chunk_array(btrfs_mount_t *mnt, const uint8_t *arr, uint32_t size)
{
    uint32_t off = 0;
    while (off + sizeof(btrfs_key_t) <= size) {
        const btrfs_key_t *key = (const btrfs_key_t *)(arr + off);
        off += sizeof(btrfs_key_t);
        if (key->type != BTRFS_CHUNK_ITEM_KEY) break; /* malformed; stop */
        if (off + sizeof(btrfs_chunk_t) > size) break;

        const btrfs_chunk_t *chunk = (const btrfs_chunk_t *)(arr + off);
        uint16_t num_stripes = chunk->num_stripes ? chunk->num_stripes : 1;
        uint32_t chunk_bytes = (uint32_t)(sizeof(btrfs_chunk_t) - sizeof(btrfs_stripe_t))
                                + (uint32_t)num_stripes * sizeof(btrfs_stripe_t);
        if (off + chunk_bytes > size) break;

        add_chunk_map(mnt, key->offset, chunk->length, chunk->stripes[0].offset, chunk->type);
        off += chunk_bytes;
    }
}

/* Walk every leaf of a tree, invoking cb(mnt, leaf_buf, ctx) on each. */
typedef void (*leaf_cb_t)(btrfs_mount_t *mnt, const uint8_t *leaf, void *ctx);

static int walk_subtree(btrfs_mount_t *mnt, uint64_t logical, uint8_t level,
                         uint8_t *scratch, leaf_cb_t cb, void *ctx)
{
    if (btrfs_read_node(mnt, logical, scratch) != VFS_OK) return VFS_ERR_IO;
    const btrfs_header_t *hdr = (const btrfs_header_t *)scratch;

    if (level == 0) {
        cb(mnt, scratch, ctx);
        return VFS_OK;
    }

    /* Internal node: recurse into every child. Save the parent's key
     * pointers first, since the same scratch buffer gets reused. */
    uint32_t nritems = hdr->nritems;
    const uint8_t *ptr_base = scratch + BTRFS_HEADER_SIZE;
    for (uint32_t i = 0; i < nritems; i++) {
        btrfs_key_ptr_t kp;
        memcpy(&kp, ptr_base + (size_t)i * BTRFS_KEY_PTR_SIZE, sizeof(kp));
        uint8_t *child_scratch = (uint8_t *)mem_alloc(mnt->nodesize);
        if (!child_scratch) return VFS_ERR_NOSPACE;
        int r = walk_subtree(mnt, kp.blockptr, level - 1, child_scratch, cb, ctx);
        mem_free(child_scratch);
        if (r != VFS_OK) return r;
    }
    return VFS_OK;
}

static void chunk_tree_leaf_cb(btrfs_mount_t *mnt, const uint8_t *leaf, void *ctx)
{
    (void)ctx;
    const btrfs_header_t *hdr = (const btrfs_header_t *)leaf;
    const uint8_t *item_base = leaf + BTRFS_HEADER_SIZE;

    for (uint32_t i = 0; i < hdr->nritems; i++) {
        btrfs_item_t it;
        memcpy(&it, item_base + (size_t)i * BTRFS_ITEM_SIZE, sizeof(it));
        if (it.key.type != BTRFS_CHUNK_ITEM_KEY) continue;

        const btrfs_chunk_t *chunk = (const btrfs_chunk_t *)(leaf + it.offset);
        add_chunk_map(mnt, it.key.offset, chunk->length, chunk->stripes[0].offset, chunk->type);
    }
}

/* ── Generic B-tree search: descend to the leaf that would contain
 * target_key, returning that leaf's raw bytes and the lower-bound item
 * index within it (first item with key >= target_key, or nritems if
 * every item in this leaf sorts before target_key). ─────────────────── */

int btrfs_search(btrfs_mount_t *mnt, uint64_t root_logical, uint8_t root_level,
                         btrfs_key_t target, uint8_t *leaf_out, uint32_t *index_out)
{
    uint64_t logical = root_logical;
    uint8_t level = root_level;

    for (;;) {
        if (btrfs_read_node(mnt, logical, leaf_out) != VFS_OK) return VFS_ERR_IO;
        const btrfs_header_t *hdr = (const btrfs_header_t *)leaf_out;

        if (level == 0) {
            const uint8_t *item_base = leaf_out + BTRFS_HEADER_SIZE;
            uint32_t i;
            for (i = 0; i < hdr->nritems; i++) {
                btrfs_item_t it;
                memcpy(&it, item_base + (size_t)i * BTRFS_ITEM_SIZE, sizeof(it));
                if (key_cmp(&it.key, &target) >= 0) break;
            }
            *index_out = i;
            return VFS_OK;
        }

        const uint8_t *ptr_base = leaf_out + BTRFS_HEADER_SIZE;
        int chosen = -1;
        for (uint32_t i = 0; i < hdr->nritems; i++) {
            btrfs_key_ptr_t kp;
            memcpy(&kp, ptr_base + (size_t)i * BTRFS_KEY_PTR_SIZE, sizeof(kp));
            if (key_cmp(&kp.key, &target) <= 0) chosen = (int)i;
            else break;
        }
        if (chosen < 0) chosen = 0; /* target before first child; descend leftmost */

        btrfs_key_ptr_t kp;
        memcpy(&kp, ptr_base + (size_t)chosen * BTRFS_KEY_PTR_SIZE, sizeof(kp));
        logical = kp.blockptr;
        level--;
    }
}

int item_at(const uint8_t *leaf, uint32_t index, btrfs_item_t *out_item, const uint8_t **out_data)
{
    const btrfs_header_t *hdr = (const btrfs_header_t *)leaf;
    if (index >= hdr->nritems) return VFS_ERR_NOTFOUND;
    const uint8_t *item_base = leaf + BTRFS_HEADER_SIZE;
    memcpy(out_item, item_base + (size_t)index * BTRFS_ITEM_SIZE, sizeof(*out_item));
    *out_data = leaf + out_item->offset;
    return VFS_OK;
}

/* ── Inode / directory / file operations ────────────────────────────────── */

int btrfs_read_inode(btrfs_mount_t *mnt, uint64_t ino, btrfs_inode_item_t *out)
{
    btrfs_key_t target = {ino, BTRFS_INODE_ITEM_KEY, 0};
    uint32_t idx;
    if (btrfs_search(mnt, mnt->fs_tree_root, mnt->fs_tree_level, target, mnt->leaf_buffer, &idx) != VFS_OK)
        return VFS_ERR_IO;

    btrfs_item_t it;
    const uint8_t *data;
    if (item_at(mnt->leaf_buffer, idx, &it, &data) != VFS_OK) return VFS_ERR_NOTFOUND;
    if (it.key.objectid != ino || it.key.type != BTRFS_INODE_ITEM_KEY) return VFS_ERR_NOTFOUND;

    memset(out, 0, sizeof(*out));
    size_t copy_len = sizeof(*out) < it.size ? sizeof(*out) : it.size;
    memcpy(out, data, copy_len);
    return VFS_OK;
}

/* Look up a name within a directory, or (if name==NULL) advance *pos to
 * the next DIR_INDEX entry for readdir. Returns VFS_OK with *found set,
 * or VFS_ERR_NOTFOUND when the directory is exhausted. */
int btrfs_dir_step(btrfs_mount_t *mnt, uint64_t dir_ino, uint64_t *pos,
                           const char *want_name, int want_len,
                           uint64_t *out_child_ino, uint8_t *out_type,
                           char *out_name, int *out_name_len)
{
    for (;;) {
        btrfs_key_t target = {dir_ino, BTRFS_DIR_INDEX_KEY, *pos};
        uint32_t idx;
        if (btrfs_search(mnt, mnt->fs_tree_root, mnt->fs_tree_level, target, mnt->leaf_buffer, &idx) != VFS_OK)
            return VFS_ERR_IO;

        btrfs_item_t it;
        const uint8_t *data;
        if (item_at(mnt->leaf_buffer, idx, &it, &data) != VFS_OK) return VFS_ERR_NOTFOUND;
        if (it.key.objectid != dir_ino || it.key.type != BTRFS_DIR_INDEX_KEY) return VFS_ERR_NOTFOUND;

        btrfs_dir_item_t di;
        memcpy(&di, data, sizeof(di));
        const char *name = (const char *)(data + sizeof(di));

        *pos = it.key.offset; /* caller bumps via key_successor before next call */

        if (want_name) {
            if (di.name_len == want_len && memcmp(name, want_name, (size_t)want_len) == 0) {
                *out_child_ino = di.location.objectid;
                *out_type = di.type;
                return VFS_OK;
            }
            /* not a match; caller advances *pos and retries */
            return VFS_ERR_NOTDIR; /* sentinel: "keep scanning" */
        }

        *out_child_ino = di.location.objectid;
        *out_type = di.type;
        int n = di.name_len;
        if (n > *out_name_len) n = *out_name_len;
        memcpy(out_name, name, (size_t)n);
        *out_name_len = n;
        return VFS_OK;
    }
}

uint64_t btrfs_dir_lookup(btrfs_mount_t *mnt, uint64_t dir_ino, const char *name, int name_len, uint8_t *out_type)
{
    uint64_t pos = 0;
    for (;;) {
        uint64_t child;
        uint8_t type;
        int r = btrfs_dir_step(mnt, dir_ino, &pos, name, name_len, &child, &type, NULL, NULL);
        if (r == VFS_OK) {
            if (out_type) *out_type = type;
            return child;
        }
        if (r != VFS_ERR_NOTDIR) return 0; /* IO error or exhausted */
        btrfs_key_t bumped = key_successor((btrfs_key_t){dir_ino, BTRFS_DIR_INDEX_KEY, pos});
        pos = bumped.offset;
    }
}

int btrfs_resolve_path(btrfs_mount_t *mnt, const char *path, uint64_t *out_ino, btrfs_inode_item_t *out_inode)
{
    uint64_t cur_ino = BTRFS_FIRST_FREE_OBJECTID; /* 256: subvolume root dir */
    btrfs_inode_item_t cur_inode;
    if (btrfs_read_inode(mnt, cur_ino, &cur_inode) != VFS_OK) return VFS_ERR_IO;

    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        if (len > 0) {
            if ((cur_inode.mode & BTRFS_S_IFMT) != BTRFS_S_IFDIR) return VFS_ERR_NOTDIR;

            uint8_t type;
            uint64_t next_ino = btrfs_dir_lookup(mnt, cur_ino, start, len, &type);
            if (next_ino == 0) return VFS_ERR_NOTFOUND;
            if (btrfs_read_inode(mnt, next_ino, &cur_inode) != VFS_OK) return VFS_ERR_IO;
            cur_ino = next_ino;
        }
        while (*p == '/') p++;
    }

    *out_ino = cur_ino;
    *out_inode = cur_inode;
    return VFS_OK;
}

int btrfs_inode_pread(btrfs_mount_t *mnt, uint64_t ino, btrfs_inode_item_t *inode,
                              uint64_t offset, void *buf, uint32_t len, uint32_t *out_actual)
{
    uint64_t file_size = inode->size;
    if (offset >= file_size) { *out_actual = 0; return VFS_OK; }
    if (offset + len > file_size) len = (uint32_t)(file_size - offset);

    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (total < len) {
        uint64_t want_off = offset + total;
        btrfs_key_t target = {ino, BTRFS_EXTENT_DATA_KEY, want_off};
        uint32_t idx;
        if (btrfs_search(mnt, mnt->fs_tree_root, mnt->fs_tree_level, target, mnt->leaf_buffer, &idx) != VFS_OK)
            return VFS_ERR_IO;

        /* The extent covering want_off has key.offset <= want_off, i.e. the
         * item just before the lower-bound result (unless it's an exact
         * match, in which case idx itself is the one). */
        btrfs_item_t it;
        const uint8_t *data;
        if (item_at(mnt->leaf_buffer, idx, &it, &data) == VFS_OK &&
            it.key.objectid == ino && it.key.type == BTRFS_EXTENT_DATA_KEY && it.key.offset == want_off) {
            /* exact match, use idx directly */
        } else if (idx == 0) {
            return VFS_ERR_IO; /* no extent covers this offset */
        } else {
            if (item_at(mnt->leaf_buffer, idx - 1, &it, &data) != VFS_OK) return VFS_ERR_IO;
            if (it.key.objectid != ino || it.key.type != BTRFS_EXTENT_DATA_KEY) return VFS_ERR_IO;
        }

        btrfs_file_extent_item_t fe;
        memcpy(&fe, data, sizeof(fe));
        uint64_t extent_file_start = it.key.offset;

        if (fe.type == BTRFS_FILE_EXTENT_INLINE) {
            uint64_t inline_len = it.size - (sizeof(fe) - sizeof(uint64_t) * 4); /* header sans disk_* fields */
            const uint8_t *inline_data = data + (sizeof(fe) - sizeof(uint64_t) * 4);
            uint64_t rel = want_off - extent_file_start;
            if (rel >= inline_len) return VFS_ERR_IO;
            uint32_t chunk = (uint32_t)(inline_len - rel);
            if (chunk > len - total) chunk = len - total;
            memcpy(dst + total, inline_data + rel, chunk);
            total += chunk;
            continue;
        }

        if (fe.disk_bytenr == 0) {
            /* Hole: sparse region, reads as zero. */
            uint32_t chunk = (uint32_t)(fe.num_bytes - (want_off - extent_file_start));
            if (chunk > len - total) chunk = len - total;
            memset(dst + total, 0, chunk);
            total += chunk;
            continue;
        }

        uint64_t rel = want_off - extent_file_start;
        uint64_t disk_off = fe.disk_bytenr + fe.offset + rel;
        uint32_t chunk = (uint32_t)(fe.num_bytes - rel);
        if (chunk > len - total) chunk = len - total;
        if (chunk > mnt->nodesize) chunk = mnt->nodesize; /* one node-sized scratch read at a time */

        if (btrfs_read_logical(mnt, disk_off, mnt->io_buffer, chunk) != VFS_OK) return VFS_ERR_IO;
        memcpy(dst + total, mnt->io_buffer, chunk);
        total += chunk;
    }

    *out_actual = total;
    return VFS_OK;
}

/* ── Mount / unmount ─────────────────────────────────────────────────────── */

static int btrfs_mount_impl(vfs_mount_t *mnt)
{
    if (!mnt || !mnt->priv) return VFS_ERR_INVAL;
    blkdev_t *dev = (blkdev_t *)mnt->priv;

    terminal_writestring("[BTRFS] Mounting filesystem...\n");
    if (dev->sector_size == 0) return VFS_ERR_INVAL;

    uint8_t sb_raw[4096];
    if (btrfs_read_phys(dev, BTRFS_SUPER_OFFSET, sb_raw, sizeof(sb_raw)) != VFS_OK) {
        terminal_writestring("[BTRFS] ERROR: Failed to read superblock\n");
        return VFS_ERR_IO;
    }

    btrfs_super_t *sb = (btrfs_super_t *)sb_raw;
    if (memcmp(sb->magic, BTRFS_MAGIC_STR, 8) != 0) {
        terminal_writestring("[BTRFS] ERROR: Bad superblock magic (not btrfs)\n");
        return VFS_ERR_IO;
    }
    if (sb->sectorsize < 512 || sb->nodesize < sb->sectorsize || sb->nodesize > 65536) {
        terminal_writestring("[BTRFS] ERROR: Invalid sector/node size\n");
        return VFS_ERR_IO;
    }

    btrfs_mount_t *bmnt = (btrfs_mount_t *)mem_alloc(sizeof(btrfs_mount_t));
    if (!bmnt) return VFS_ERR_NOSPACE;
    memset(bmnt, 0, sizeof(*bmnt));

    bmnt->dev = dev;
    bmnt->sectorsize = sb->sectorsize;
    bmnt->nodesize = sb->nodesize;
    bmnt->chunk_root = sb->chunk_root;
    bmnt->chunk_root_level = sb->chunk_root_level;

    bmnt->node_buffer = (uint8_t *)mem_alloc(bmnt->nodesize);
    bmnt->leaf_buffer = (uint8_t *)mem_alloc(bmnt->nodesize);
    bmnt->io_buffer = (uint8_t *)mem_alloc(bmnt->nodesize);
    if (!bmnt->node_buffer || !bmnt->leaf_buffer || !bmnt->io_buffer) {
        if (bmnt->node_buffer) mem_free(bmnt->node_buffer);
        if (bmnt->leaf_buffer) mem_free(bmnt->leaf_buffer);
        if (bmnt->io_buffer) mem_free(bmnt->io_buffer);
        mem_free(bmnt);
        return VFS_ERR_NOSPACE;
    }

    /* Bootstrap: the embedded system chunk array maps enough of the
     * logical address space to physically locate the chunk tree root. */
    if (sb->sys_chunk_array_size > BTRFS_SYSTEM_CHUNK_ARRAY_SIZE) {
        terminal_writestring("[BTRFS] ERROR: Invalid system chunk array size\n");
        goto fail;
    }
    parse_system_chunk_array(bmnt, sb->sys_chunk_array, sb->sys_chunk_array_size);
    if (bmnt->chunk_count == 0) {
        terminal_writestring("[BTRFS] ERROR: No usable system chunks\n");
        goto fail;
    }

    /* Now walk the full chunk tree to pick up METADATA/DATA chunks too. */
    if (walk_subtree(bmnt, bmnt->chunk_root, bmnt->chunk_root_level, bmnt->node_buffer,
                      chunk_tree_leaf_cb, NULL) != VFS_OK) {
        terminal_writestring("[BTRFS] ERROR: Failed to read chunk tree\n");
        goto fail;
    }

    /* Root tree -> this subvolume's ROOT_ITEM -> fs tree root. */
    btrfs_key_t target = {BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0};
    uint32_t idx;
    if (btrfs_search(bmnt, sb->root, sb->root_level, target, bmnt->leaf_buffer, &idx) != VFS_OK) {
        terminal_writestring("[BTRFS] ERROR: Failed to read root tree\n");
        goto fail;
    }
    btrfs_item_t it;
    const uint8_t *data;
    if (item_at(bmnt->leaf_buffer, idx, &it, &data) != VFS_OK ||
        it.key.objectid != BTRFS_FS_TREE_OBJECTID || it.key.type != BTRFS_ROOT_ITEM_KEY) {
        terminal_writestring("[BTRFS] ERROR: Default subvolume not found\n");
        goto fail;
    }
    btrfs_root_item_t root_item;
    memset(&root_item, 0, sizeof(root_item));
    size_t copy_len = sizeof(root_item) < it.size ? sizeof(root_item) : it.size;
    memcpy(&root_item, data, copy_len);

    bmnt->root_tree_root = sb->root;
    bmnt->root_tree_level = sb->root_level;
    bmnt->fs_tree_root = root_item.bytenr;
    bmnt->fs_tree_level = root_item.level;
    bmnt->generation = sb->generation;

    /* Bootstrap the write-side bump allocator + inode counter by scanning
     * the trees just mounted (see btrfs_write.c) - sets bmnt->writable. */
    btrfs_write_init_allocators(bmnt);

    mnt->priv = bmnt;
    kprintf("[BTRFS] Mounted: %u byte nodes, %d chunks, %s\n", bmnt->nodesize, bmnt->chunk_count,
            bmnt->writable ? "read/write" : "read-only");
    return VFS_OK;

fail:
    mem_free(bmnt->node_buffer);
    mem_free(bmnt->leaf_buffer);
    mem_free(bmnt->io_buffer);
    mem_free(bmnt);
    return VFS_ERR_IO;
}

static int btrfs_unmount_impl(vfs_mount_t *mnt)
{
    if (!mnt || !mnt->priv) return VFS_ERR_INVAL;
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;

    mem_free(bmnt->node_buffer);
    mem_free(bmnt->leaf_buffer);
    mem_free(bmnt->io_buffer);
    mem_free(bmnt);

    mnt->priv = NULL;
    terminal_writestring("[BTRFS] Unmounted\n");
    return VFS_OK;
}

/* ── File operations ─────────────────────────────────────────────────────── */

static int btrfs_open(vfs_mount_t *mnt, const char *path, int flags, void **fpriv)
{
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;

    /* Write/create opens hand off to the CoW write engine (btrfs_write.c).
     * Checked before path resolution: a filesystem this driver decided
     * not to write to (bmnt->writable == 0 - see btrfs_write_init_allocators)
     * should report "read-only" rather than a misleading "not found" for
     * the common case of creating a brand new file. */
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) {
        if (!bmnt->writable) return VFS_ERR_ROFS;
        btrfs_file_t *f = NULL;
        int r = btrfs_write_open(bmnt, path, flags, &f);
        if (r != VFS_OK) return r;
        *fpriv = f;
        return VFS_OK;
    }

    uint64_t ino;
    btrfs_inode_item_t inode;
    int r = btrfs_resolve_path(bmnt, path, &ino, &inode);
    if (r != VFS_OK) return r;

    if ((inode.mode & BTRFS_S_IFMT) == BTRFS_S_IFDIR) return VFS_ERR_ISDIR;

    btrfs_file_t *f = (btrfs_file_t *)mem_alloc(sizeof(btrfs_file_t));
    if (!f) return VFS_ERR_NOSPACE;
    f->mnt = bmnt;
    f->ino = ino;
    f->inode = inode;
    f->pos = 0;
    f->flags = flags;

    *fpriv = f;
    return VFS_OK;
}

static int btrfs_close(vfs_mount_t *mnt, void *fpriv)
{
    (void)mnt;
    if (!fpriv) return VFS_ERR_BADFD;
    mem_free(fpriv);
    return VFS_OK;
}

/* Anywhere within the file (writes still only append: btrfs_write). */
static int btrfs_seek(vfs_mount_t *mnt, void *fpriv, int32_t offset, int whence, uint32_t *newpos)
{
    (void)mnt;
    btrfs_file_t *f = (btrfs_file_t *)fpriv;
    if (!f) return VFS_ERR_BADFD;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos
                 : whence == SEEK_END ? (int64_t)f->inode.size : -1;
    if (base < 0) return VFS_ERR_INVAL;
    int64_t p = base + offset;
    if (p < 0 || (uint64_t)p > f->inode.size || p > 0xFFFFFFFFLL) return VFS_ERR_INVAL;
    f->pos = (uint64_t)p;
    *newpos = (uint32_t)p;
    return VFS_OK;
}

static int btrfs_read(vfs_mount_t *mnt, void *fpriv, void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    btrfs_file_t *f = (btrfs_file_t *)fpriv;
    if (!f) { *actual = 0; return VFS_ERR_BADFD; }

    uint32_t got = 0;
    int r = btrfs_inode_pread(f->mnt, f->ino, &f->inode, f->pos, buf, len, &got);
    f->pos += got;
    *actual = got;
    return r;
}

static int btrfs_write(vfs_mount_t *mnt, void *fpriv, const void *buf, uint32_t len, uint32_t *actual)
{
    (void)mnt;
    btrfs_file_t *f = (btrfs_file_t *)fpriv;
    if (!f) { *actual = 0; return VFS_ERR_BADFD; }
    return btrfs_write_data(f, buf, len, actual);
}

/* ── Directory operations ────────────────────────────────────────────────── */

static int btrfs_opendir(vfs_mount_t *mnt, const char *path, void **dpriv)
{
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;

    uint64_t ino;
    btrfs_inode_item_t inode;
    int r = btrfs_resolve_path(bmnt, path, &ino, &inode);
    if (r != VFS_OK) return r;
    if ((inode.mode & BTRFS_S_IFMT) != BTRFS_S_IFDIR) return VFS_ERR_NOTDIR;

    btrfs_file_t *file = (btrfs_file_t *)mem_alloc(sizeof(btrfs_file_t));
    if (!file) return VFS_ERR_NOSPACE;
    file->mnt = bmnt;
    file->ino = ino;
    file->inode = inode;
    file->pos = 0;
    file->flags = O_RDONLY;

    btrfs_dir_t *d = (btrfs_dir_t *)mem_alloc(sizeof(btrfs_dir_t));
    if (!d) { mem_free(file); return VFS_ERR_NOSPACE; }
    d->mnt = bmnt;
    d->dir_file = file;
    d->next_offset = 0;

    *dpriv = d;
    return VFS_OK;
}

static int btrfs_readdir(vfs_mount_t *mnt, void *dpriv, vfs_dirent_t *ent)
{
    (void)mnt;
    btrfs_dir_t *d = (btrfs_dir_t *)dpriv;
    if (!d) return VFS_ERR_BADFD;

    uint64_t child;
    uint8_t type;
    char name[VFS_NAME_MAX + 1];
    int name_len = VFS_NAME_MAX;

    int r = btrfs_dir_step(d->mnt, d->dir_file->ino, &d->next_offset, NULL, 0,
                            &child, &type, name, &name_len);
    if (r != VFS_OK) return r;

    btrfs_key_t bumped = key_successor((btrfs_key_t){d->dir_file->ino, BTRFS_DIR_INDEX_KEY, d->next_offset});
    d->next_offset = bumped.offset;

    memcpy(ent->name, name, (size_t)name_len);
    ent->name[name_len] = '\0';
    return VFS_OK;
}

static int btrfs_closedir(vfs_mount_t *mnt, void *dpriv)
{
    (void)mnt;
    if (!dpriv) return VFS_ERR_BADFD;
    btrfs_dir_t *d = (btrfs_dir_t *)dpriv;
    if (d->dir_file) mem_free(d->dir_file);
    mem_free(d);
    return VFS_OK;
}

/* ── Metadata ─────────────────────────────────────────────────────────────── */

static int btrfs_stat(vfs_mount_t *mnt, const char *path, vfs_stat_t *st)
{
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;

    uint64_t ino;
    btrfs_inode_item_t inode;
    int r = btrfs_resolve_path(bmnt, path, &ino, &inode);
    if (r != VFS_OK) return r;

    memset(st, 0, sizeof(*st));

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    int n = 0;
    while (base[n] && n < VFS_NAME_MAX) { st->name[n] = base[n]; n++; }
    st->name[n] = '\0';

    st->size = (inode.size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)inode.size;
    st->attrs = ((inode.mode & BTRFS_S_IFMT) == BTRFS_S_IFDIR) ? VFS_ATTR_DIR : 0;
    return VFS_OK;
}

static int btrfs_unlink(vfs_mount_t *mnt, const char *path)
{
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;
    if (!bmnt->writable) return VFS_ERR_ROFS;
    return btrfs_do_unlink(bmnt, path);
}

static int btrfs_mkdir(vfs_mount_t *mnt, const char *path)
{
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;
    if (!bmnt->writable) return VFS_ERR_ROFS;
    return btrfs_do_mkdir(bmnt, path);
}

static int btrfs_rename(vfs_mount_t *mnt, const char *old, const char *nw)
{
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;
    if (!bmnt->writable) return VFS_ERR_ROFS;
    return btrfs_do_rename(bmnt, old, nw);
}

/* ── Driver init / VFS wiring ────────────────────────────────────────────── */

int btrfs_init(const char *params)
{
    (void)params;
    uart_puts("DEBUG: Entering btrfs_init\r\n");

    blkdev_t *dev = blkdev_get_by_name("disk0p2");
    if (!dev) dev = blkdev_get_by_name("C");
    if (!dev) {
        kprintf("[BTRFS] No block device found; not mounting\n");
        uart_puts("DEBUG: btrfs_init complete\r\n");
        return 0;
    }

    int r = vfs_mount("/", btrfs_get_fs(), dev);
    if (r != VFS_OK) {
        kprintf("[BTRFS] Mount failed: %d\n", r);
    }

    uart_puts("DEBUG: btrfs_init complete\r\n");
    return 0;
}

vfs_fs_t *btrfs_get_fs(void)
{
    return &btrfs_fs_ops;
}

/* Size and use: the superblock's total_bytes / bytes_used. */
static int btrfs_statfs(vfs_mount_t *mnt, vfs_statfs_t *st)
{
    btrfs_mount_t *bmnt = (btrfs_mount_t *)mnt->priv;
    static uint8_t sb_raw[4096];
    if (btrfs_read_phys(bmnt->dev, BTRFS_SUPER_OFFSET, sb_raw, sizeof(sb_raw)) != VFS_OK)
        return VFS_ERR_IO;
    const btrfs_super_t *sb = (const btrfs_super_t *)sb_raw;
    uint64_t bs = sb->sectorsize ? sb->sectorsize : 4096;
    st->bsize = bs;
    st->blocks = sb->total_bytes / bs;
    st->bfree = sb->total_bytes > sb->bytes_used ? (sb->total_bytes - sb->bytes_used) / bs : 0;
    return VFS_OK;
}
