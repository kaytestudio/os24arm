/**
 * @file btrfs_write.c
 * @brief btrfs copy-on-write B-tree engine + write-side filesystem
 *        operations (create/write/truncate/unlink/mkdir/rename).
 *
 * IMPORTANT - real-world interop caveat: this driver's leaf layout is
 * already a simplified, project-local dialect (see fs/btrfs/btrfs.c's
 * header comment and scripts/mkbtrfs_image.py: items are packed forward,
 * contiguously, not backward from the end of the leaf the way upstream
 * btrfs does). On top of that, this write path deliberately does NOT
 * maintain the extent tree (space accounting / back-references) or the
 * checksum tree (per-block data CRC32C, consulted by a real Linux mount
 * on every read). Both are large, self-referential subsystems (allocating
 * a block for the extent tree is itself an event the extent tree needs to
 * record) that this project has no way to validate here anyway - this Mac
 * has no btrfs-progs/mkfs.btrfs, and mkbtrfs_image.py's own test fixtures
 * don't model those trees either.
 *
 * So: images this driver writes to are readable and writable by *this*
 * driver, durably (checksummed nodes, generation-bumped, superblock
 * commit as the atomic finish line), but should be treated as owned by
 * this OS - not blindly re-mounted by a real Linux btrfs or btrfs-progs
 * afterward.
 *
 * What IS maintained correctly: the chunk/root/fs trees, with a real
 * copy-on-write B-tree engine (path-tracking search, leaf/internal-node
 * splitting, tree-height growth, ancestor key-pointer fixup) and CRC32C
 * checksums on every node and the superblock. Space is handed out by a
 * simple per-mount bump allocator, recomputed at every mount by scanning
 * the existing trees (never persisted, never reclaims space freed by
 * deletes within a session) - the same "recompute, don't persist"
 * philosophy as kernel/src/mem.c's heap (whose mem_free() is a documented
 * no-op). That heap is where this file's own scratch buffers come from
 * too, which is worth knowing before running a long write-heavy session:
 * every mem_alloc() here is real, but mem_free() never reclaims, so
 * heavy write activity in a single boot burns heap - fine for testing
 * and moderate use, not for an unbounded workload.
 *
 * Writes are only supported sequentially/appending from the current end
 * of a file's data (fresh O_CREAT, O_TRUNC, append, or plain front-to-back
 * writes) - arbitrary random-access overwrite of already-written bytes is
 * out of scope (this driver doesn't even wire up .seek - see btrfs.c's
 * btrfs_fs_ops - and nothing in the DOS command set needs it).
 */

#include "btrfs_internal.h"
#include "string.h"

/* Defined at the bottom of this file; forward-declared here since
 * btrfs_do_fs_op() (defined well before it) needs it. */
static int btrfs_search_path(btrfs_mount_t *mnt, uint64_t root_logical, uint8_t root_level,
                              btrfs_key_t target, uint8_t *leaf_out, btrfs_path_t *path);

/* CRC32C */

static uint32_t crc32c_table[256];
static int crc32c_table_ready = 0;

static void crc32c_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        crc32c_table[i] = c;
    }
    crc32c_table_ready = 1;
}

static uint32_t btrfs_crc32c(const void *data, uint32_t len)
{
    if (!crc32c_table_ready) crc32c_init_table();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* Checksums bytes [32, size) of a node/superblock and stores the result
 * (little-endian, first 4 bytes of the 32-byte csum field, rest zeroed)
 * at the start of the buffer - the same convention for tree nodes and
 * the superblock alike. */
static void checksum_block(uint8_t *buf, uint32_t size)
{
    uint32_t crc = btrfs_crc32c(buf + 32, size - 32);
    memset(buf, 0, 32);
    memcpy(buf, &crc, 4);
}

/* Bump allocator */

static int btrfs_alloc_extent(btrfs_mount_t *mnt, uint64_t len, uint64_t *out_addr)
{
    uint64_t gran = mnt->sectorsize ? mnt->sectorsize : 512;
    uint64_t need = (len + gran - 1) & ~(gran - 1);
    if (need == 0) need = gran;

    if (mnt->next_alloc + need > mnt->alloc_chunk_end) {
        uint64_t best = 0; int found = 0; uint64_t best_end = 0;
        for (int i = 0; i < mnt->chunk_count; i++) {
            btrfs_chunk_map_t *c = &mnt->chunks[i];
            if (c->logical >= mnt->next_alloc && (!found || c->logical < best)) {
                best = c->logical; best_end = c->logical + c->length; found = 1;
            }
        }
        if (!found) return VFS_ERR_NOSPACE;
        mnt->next_alloc = best;
        mnt->alloc_chunk_end = best_end;
        if (mnt->next_alloc + need > mnt->alloc_chunk_end) return VFS_ERR_NOSPACE;
    }

    *out_addr = mnt->next_alloc;
    mnt->next_alloc += need;
    return VFS_OK;
}

/* Mount-time bootstrap: high-water mark + next inode number */

typedef struct {
    uint64_t max_end;
    uint64_t max_ino;
} btw_scan_t;

static void scan_leaf_extents(const uint8_t *leaf, btw_scan_t *acc)
{
    const btrfs_header_t *hdr = (const btrfs_header_t *)leaf;
    const uint8_t *item_base = leaf + BTRFS_HEADER_SIZE;
    for (uint32_t i = 0; i < hdr->nritems; i++) {
        btrfs_item_t it;
        memcpy(&it, item_base + (size_t)i * BTRFS_ITEM_SIZE, sizeof(it));

        if (it.key.type == BTRFS_INODE_ITEM_KEY && it.key.objectid > acc->max_ino)
            acc->max_ino = it.key.objectid;

        if (it.key.type == BTRFS_EXTENT_DATA_KEY && it.size >= sizeof(btrfs_file_extent_item_t)) {
            btrfs_file_extent_item_t fe;
            memcpy(&fe, leaf + it.offset, sizeof(fe));
            if (fe.type != BTRFS_FILE_EXTENT_INLINE && fe.disk_bytenr != 0) {
                uint64_t end = fe.disk_bytenr + fe.disk_num_bytes;
                if (end > acc->max_end) acc->max_end = end;
            }
        }
    }
}

static int scan_high_water(btrfs_mount_t *mnt, uint64_t logical, uint8_t level, btw_scan_t *acc)
{
    uint8_t *scratch = (uint8_t *)mem_alloc(mnt->nodesize);
    if (!scratch) return VFS_ERR_NOSPACE;
    if (btrfs_read_node(mnt, logical, scratch) != VFS_OK) { mem_free(scratch); return VFS_ERR_IO; }
    const btrfs_header_t *hdr = (const btrfs_header_t *)scratch;

    uint64_t end = logical + mnt->nodesize;
    if (end > acc->max_end) acc->max_end = end;

    if (level == 0) {
        scan_leaf_extents(scratch, acc);
        mem_free(scratch);
        return VFS_OK;
    }

    uint32_t nritems = hdr->nritems;
    const uint8_t *ptr_base = scratch + BTRFS_HEADER_SIZE;
    for (uint32_t i = 0; i < nritems; i++) {
        btrfs_key_ptr_t kp;
        memcpy(&kp, ptr_base + (size_t)i * BTRFS_KEY_PTR_SIZE, sizeof(kp));
        int r = scan_high_water(mnt, kp.blockptr, level - 1, acc);
        if (r != VFS_OK) { mem_free(scratch); return r; }
    }
    mem_free(scratch);
    return VFS_OK;
}

void btrfs_write_init_allocators(btrfs_mount_t *mnt)
{
    mnt->writable = 0;

    /* Bump allocator alignment math below assumes a power-of-two nodesize
     * (always true for real btrfs, but this is hand-authored test data). */
    if (mnt->nodesize == 0 || (mnt->nodesize & (mnt->nodesize - 1)) != 0) return;
    if (mnt->chunk_root_level + 1 > BTRFS_MAX_TREE_HEIGHT) return;
    if (mnt->root_tree_level + 1 > BTRFS_MAX_TREE_HEIGHT) return;
    if (mnt->fs_tree_level + 1 > BTRFS_MAX_TREE_HEIGHT) return;

    btw_scan_t acc = {0, 0};
    if (scan_high_water(mnt, mnt->chunk_root, mnt->chunk_root_level, &acc) != VFS_OK) return;
    if (scan_high_water(mnt, mnt->root_tree_root, mnt->root_tree_level, &acc) != VFS_OK) return;
    if (scan_high_water(mnt, mnt->fs_tree_root, mnt->fs_tree_level, &acc) != VFS_OK) return;

    /* Never allocate back over the superblock's own reserved region. */
    uint64_t sb_end = BTRFS_SUPER_OFFSET + 4096;
    if (sb_end > acc.max_end) acc.max_end = sb_end;

    uint64_t start = (acc.max_end + mnt->nodesize - 1) & ~((uint64_t)mnt->nodesize - 1);

    uint64_t chunk_end = 0; int found = 0;
    for (int i = 0; i < mnt->chunk_count; i++) {
        btrfs_chunk_map_t *c = &mnt->chunks[i];
        if (start >= c->logical && start < c->logical + c->length) {
            chunk_end = c->logical + c->length; found = 1;
            break;
        }
    }
    if (!found) {
        uint64_t best = 0; int have = 0;
        for (int i = 0; i < mnt->chunk_count; i++) {
            btrfs_chunk_map_t *c = &mnt->chunks[i];
            if (c->logical >= start && (!have || c->logical < best)) {
                best = c->logical; have = 1; chunk_end = c->logical + c->length;
            }
        }
        if (!have) return; /* no room anywhere; stays read-only */
        start = best;
    }

    mnt->next_alloc = start;
    mnt->alloc_chunk_end = chunk_end;
    mnt->next_ino = (acc.max_ino >= BTRFS_FIRST_FREE_OBJECTID) ? acc.max_ino + 1 : BTRFS_FIRST_FREE_OBJECTID + 1;
    mnt->writable = 1;
}

/* Superblock commit */

static int btrfs_write_superblock(btrfs_mount_t *mnt, uint64_t new_root, uint8_t new_root_level, uint64_t new_gen)
{
    uint8_t sb_raw[4096];
    if (btrfs_read_phys((blkdev_t *)mnt->dev, BTRFS_SUPER_OFFSET, sb_raw, sizeof(sb_raw)) != VFS_OK)
        return VFS_ERR_IO;

    btrfs_super_t *sb = (btrfs_super_t *)sb_raw;
    sb->generation = new_gen;
    sb->root = new_root;
    sb->root_level = new_root_level;

    checksum_block(sb_raw, sizeof(sb_raw));
    return btrfs_write_phys((blkdev_t *)mnt->dev, BTRFS_SUPER_OFFSET, sb_raw, sizeof(sb_raw));
}

/* Leaf item array (de)serialization */

#define BTW_MAX_ITEM_DATA 320  /* covers every item type this driver writes:
                                 * inode item (160), extent data (53), and
                                 * dir entries (30 + up to VFS_NAME_MAX=255) */

typedef struct {
    btrfs_key_t key;
    uint16_t    len;
    uint8_t     data[BTW_MAX_ITEM_DATA];
} btw_item_t;

typedef enum { BTW_INSERT, BTW_UPDATE, BTW_DELETE } btw_op_t;

static int leaf_decode(const uint8_t *leaf, btw_item_t *out, uint32_t max_out, uint32_t *out_n)
{
    const btrfs_header_t *hdr = (const btrfs_header_t *)leaf;
    if (hdr->nritems > max_out) return VFS_ERR_NOSPACE;
    const uint8_t *item_base = leaf + BTRFS_HEADER_SIZE;
    for (uint32_t i = 0; i < hdr->nritems; i++) {
        btrfs_item_t it;
        memcpy(&it, item_base + (size_t)i * BTRFS_ITEM_SIZE, sizeof(it));
        if (it.size > BTW_MAX_ITEM_DATA) return VFS_ERR_IO;
        out[i].key = it.key;
        out[i].len = (uint16_t)it.size;
        memcpy(out[i].data, leaf + it.offset, it.size);
    }
    *out_n = hdr->nritems;
    return VFS_OK;
}

static int leaf_apply(btw_item_t *items, uint32_t *n, uint32_t max_n,
                       btw_op_t op, btrfs_key_t key, const void *data, uint16_t len)
{
    uint32_t idx;
    for (idx = 0; idx < *n; idx++) {
        if (key_cmp(&items[idx].key, &key) >= 0) break;
    }
    int exact = (idx < *n && key_cmp(&items[idx].key, &key) == 0);

    if (op == BTW_INSERT) {
        if (exact) return VFS_ERR_INVAL;
        if (*n >= max_n || len > BTW_MAX_ITEM_DATA) return VFS_ERR_NOSPACE;
        memmove(&items[idx + 1], &items[idx], (size_t)(*n - idx) * sizeof(btw_item_t));
        items[idx].key = key;
        items[idx].len = len;
        if (len) memcpy(items[idx].data, data, len);
        (*n)++;
        return VFS_OK;
    }
    if (op == BTW_UPDATE) {
        if (!exact) return VFS_ERR_NOTFOUND;
        if (len > BTW_MAX_ITEM_DATA) return VFS_ERR_NOSPACE;
        items[idx].len = len;
        if (len) memcpy(items[idx].data, data, len);
        return VFS_OK;
    }
    /* BTW_DELETE */
    if (!exact) return VFS_ERR_NOTFOUND;
    memmove(&items[idx], &items[idx + 1], (size_t)(*n - idx - 1) * sizeof(btw_item_t));
    (*n)--;
    return VFS_OK;
}

static uint32_t serialize_leaf(uint8_t *out, uint32_t nodesize, uint64_t owner, uint64_t generation,
                                uint64_t bytenr, const btw_item_t *items, uint32_t n)
{
    uint32_t data_start = BTRFS_HEADER_SIZE + n * BTRFS_ITEM_SIZE;
    uint32_t total = data_start;
    for (uint32_t i = 0; i < n; i++) total += items[i].len;
    if (total > nodesize) return total;

    memset(out, 0, nodesize);
    btrfs_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.bytenr = bytenr;
    hdr.generation = generation;
    hdr.owner = owner;
    hdr.nritems = n;
    hdr.level = 0;
    memcpy(out, &hdr, sizeof(hdr));

    uint32_t item_off = BTRFS_HEADER_SIZE;
    uint32_t data_off = data_start;
    for (uint32_t i = 0; i < n; i++) {
        btrfs_item_t it;
        it.key = items[i].key;
        it.offset = data_off;
        it.size = items[i].len;
        memcpy(out + item_off, &it, sizeof(it));
        item_off += BTRFS_ITEM_SIZE;
        if (items[i].len) memcpy(out + data_off, items[i].data, items[i].len);
        data_off += items[i].len;
    }
    return total;
}

static uint32_t serialize_internal(uint8_t *out, uint32_t nodesize, uint64_t owner, uint64_t generation,
                                    uint64_t bytenr, uint8_t level, const btrfs_key_ptr_t *ptrs, uint32_t n)
{
    uint32_t total = BTRFS_HEADER_SIZE + n * BTRFS_KEY_PTR_SIZE;
    if (total > nodesize) return total;

    memset(out, 0, nodesize);
    btrfs_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.bytenr = bytenr;
    hdr.generation = generation;
    hdr.owner = owner;
    hdr.nritems = n;
    hdr.level = level;
    memcpy(out, &hdr, sizeof(hdr));
    memcpy(out + BTRFS_HEADER_SIZE, ptrs, (size_t)n * BTRFS_KEY_PTR_SIZE);
    return total;
}

/* Core CoW commit: apply one mutation to a tree, following `path`,
 * splitting nodes / growing tree height as needed. On success, *root_addr
 * and *root_level are updated to the tree's new root. */

typedef struct {
    uint64_t    addr;
    btrfs_key_t low_key;
} btw_child_t;

static int btrfs_commit_tree(btrfs_mount_t *mnt, btrfs_path_t *path,
                              uint64_t owner, uint64_t generation,
                              btrfs_key_t key, const void *data, uint16_t len, btw_op_t op,
                              uint64_t *root_addr, uint8_t *root_level)
{
    /* --- Level 0: the leaf --- */
    uint8_t *leafbuf = (uint8_t *)mem_alloc(mnt->nodesize);
    if (!leafbuf) return VFS_ERR_NOSPACE;
    if (btrfs_read_node(mnt, path->levels[0].logical, leafbuf) != VFS_OK) { mem_free(leafbuf); return VFS_ERR_IO; }

    uint32_t max_items = ((const btrfs_header_t *)leafbuf)->nritems + 1;
    btw_item_t *items = (btw_item_t *)mem_alloc(sizeof(btw_item_t) * max_items);
    if (!items) { mem_free(leafbuf); return VFS_ERR_NOSPACE; }
    uint32_t n;
    if (leaf_decode(leafbuf, items, max_items, &n) != VFS_OK) { mem_free(items); mem_free(leafbuf); return VFS_ERR_IO; }
    mem_free(leafbuf);

    int r = leaf_apply(items, &n, max_items, op, key, data, len);
    if (r != VFS_OK) { mem_free(items); return r; }

    btw_child_t left = {0}, right = {0};
    int split = 0;

    uint32_t total = BTRFS_HEADER_SIZE + n * BTRFS_ITEM_SIZE;
    for (uint32_t i = 0; i < n; i++) total += items[i].len;

    uint8_t *nodebuf = (uint8_t *)mem_alloc(mnt->nodesize);
    if (!nodebuf) { mem_free(items); return VFS_ERR_NOSPACE; }

    if (total <= mnt->nodesize) {
        uint64_t addr;
        if (btrfs_alloc_extent(mnt, mnt->nodesize, &addr) != VFS_OK) { mem_free(items); mem_free(nodebuf); return VFS_ERR_NOSPACE; }
        serialize_leaf(nodebuf, mnt->nodesize, owner, generation, addr, items, n);
        checksum_block(nodebuf, mnt->nodesize);
        if (btrfs_write_node(mnt, addr, nodebuf) != VFS_OK) { mem_free(items); mem_free(nodebuf); return VFS_ERR_IO; }
        left.addr = addr;
        left.low_key = (n > 0) ? items[0].key : key;
    } else {
        uint32_t half = total / 2, acc = 0, split_i = 0;
        for (uint32_t i = 0; i < n; i++) {
            acc += BTRFS_ITEM_SIZE + items[i].len;
            if (acc >= half) { split_i = i + 1; break; }
        }
        if (split_i == 0) split_i = 1;
        if (split_i >= n) split_i = n - 1;

        uint64_t addr_l, addr_r;
        if (btrfs_alloc_extent(mnt, mnt->nodesize, &addr_l) != VFS_OK) { mem_free(items); mem_free(nodebuf); return VFS_ERR_NOSPACE; }
        serialize_leaf(nodebuf, mnt->nodesize, owner, generation, addr_l, items, split_i);
        checksum_block(nodebuf, mnt->nodesize);
        if (btrfs_write_node(mnt, addr_l, nodebuf) != VFS_OK) { mem_free(items); mem_free(nodebuf); return VFS_ERR_IO; }
        left.addr = addr_l; left.low_key = items[0].key;

        if (btrfs_alloc_extent(mnt, mnt->nodesize, &addr_r) != VFS_OK) { mem_free(items); mem_free(nodebuf); return VFS_ERR_NOSPACE; }
        serialize_leaf(nodebuf, mnt->nodesize, owner, generation, addr_r, items + split_i, n - split_i);
        checksum_block(nodebuf, mnt->nodesize);
        if (btrfs_write_node(mnt, addr_r, nodebuf) != VFS_OK) { mem_free(items); mem_free(nodebuf); return VFS_ERR_IO; }
        right.addr = addr_r; right.low_key = items[split_i].key;
        split = 1;
    }
    mem_free(items);
    mem_free(nodebuf);

    /* --- Levels 1..height-1: internal nodes, fixing up parent pointers --- */
    for (uint8_t lvl = 1; lvl < path->height; lvl++) {
        uint8_t *pbuf = (uint8_t *)mem_alloc(mnt->nodesize);
        if (!pbuf) return VFS_ERR_NOSPACE;
        if (btrfs_read_node(mnt, path->levels[lvl].logical, pbuf) != VFS_OK) { mem_free(pbuf); return VFS_ERR_IO; }
        uint32_t old_pn = ((const btrfs_header_t *)pbuf)->nritems;

        uint32_t max_ptrs = old_pn + 1;
        btrfs_key_ptr_t *ptrs = (btrfs_key_ptr_t *)mem_alloc(sizeof(btrfs_key_ptr_t) * max_ptrs);
        if (!ptrs) { mem_free(pbuf); return VFS_ERR_NOSPACE; }
        const uint8_t *ptr_base = pbuf + BTRFS_HEADER_SIZE;
        for (uint32_t i = 0; i < old_pn; i++)
            memcpy(&ptrs[i], ptr_base + (size_t)i * BTRFS_KEY_PTR_SIZE, sizeof(btrfs_key_ptr_t));
        mem_free(pbuf);

        uint32_t idx = path->levels[lvl].index;
        ptrs[idx].blockptr = left.addr;
        ptrs[idx].key = left.low_key;
        ptrs[idx].generation = generation;
        uint32_t pn = old_pn;

        if (split) {
            btrfs_key_ptr_t rkp;
            rkp.key = right.low_key; rkp.blockptr = right.addr; rkp.generation = generation;
            memmove(&ptrs[idx + 2], &ptrs[idx + 1], (size_t)(pn - idx - 1) * sizeof(btrfs_key_ptr_t));
            ptrs[idx + 1] = rkp;
            pn++;
        }

        uint32_t ptotal = BTRFS_HEADER_SIZE + pn * BTRFS_KEY_PTR_SIZE;
        uint8_t *outbuf = (uint8_t *)mem_alloc(mnt->nodesize);
        if (!outbuf) { mem_free(ptrs); return VFS_ERR_NOSPACE; }

        if (ptotal <= mnt->nodesize) {
            uint64_t addr;
            if (btrfs_alloc_extent(mnt, mnt->nodesize, &addr) != VFS_OK) { mem_free(ptrs); mem_free(outbuf); return VFS_ERR_NOSPACE; }
            serialize_internal(outbuf, mnt->nodesize, owner, generation, addr, lvl, ptrs, pn);
            checksum_block(outbuf, mnt->nodesize);
            if (btrfs_write_node(mnt, addr, outbuf) != VFS_OK) { mem_free(ptrs); mem_free(outbuf); return VFS_ERR_IO; }
            left.addr = addr; left.low_key = ptrs[0].key;
            split = 0;
        } else {
            uint32_t half = pn / 2;
            if (half == 0) half = 1;
            if (half >= pn) half = pn - 1;

            uint64_t addr_l, addr_r;
            if (btrfs_alloc_extent(mnt, mnt->nodesize, &addr_l) != VFS_OK) { mem_free(ptrs); mem_free(outbuf); return VFS_ERR_NOSPACE; }
            serialize_internal(outbuf, mnt->nodesize, owner, generation, addr_l, lvl, ptrs, half);
            checksum_block(outbuf, mnt->nodesize);
            if (btrfs_write_node(mnt, addr_l, outbuf) != VFS_OK) { mem_free(ptrs); mem_free(outbuf); return VFS_ERR_IO; }
            btrfs_key_t new_left_low = ptrs[0].key;

            if (btrfs_alloc_extent(mnt, mnt->nodesize, &addr_r) != VFS_OK) { mem_free(ptrs); mem_free(outbuf); return VFS_ERR_NOSPACE; }
            serialize_internal(outbuf, mnt->nodesize, owner, generation, addr_r, lvl, ptrs + half, pn - half);
            checksum_block(outbuf, mnt->nodesize);
            if (btrfs_write_node(mnt, addr_r, outbuf) != VFS_OK) { mem_free(ptrs); mem_free(outbuf); return VFS_ERR_IO; }

            left.addr = addr_l; left.low_key = new_left_low;
            right.addr = addr_r; right.low_key = ptrs[half].key;
            split = 1;
        }
        mem_free(ptrs);
        mem_free(outbuf);
    }

    /* --- Root handling --- */
    if (split) {
        uint8_t new_level = (uint8_t)(*root_level + 1);
        if (new_level >= BTRFS_MAX_TREE_HEIGHT) return VFS_ERR_NOSPACE;

        btrfs_key_ptr_t ptrs[2];
        ptrs[0].key = left.low_key;  ptrs[0].blockptr = left.addr;  ptrs[0].generation = generation;
        ptrs[1].key = right.low_key; ptrs[1].blockptr = right.addr; ptrs[1].generation = generation;

        uint8_t *outbuf = (uint8_t *)mem_alloc(mnt->nodesize);
        if (!outbuf) return VFS_ERR_NOSPACE;
        uint64_t addr;
        if (btrfs_alloc_extent(mnt, mnt->nodesize, &addr) != VFS_OK) { mem_free(outbuf); return VFS_ERR_NOSPACE; }
        serialize_internal(outbuf, mnt->nodesize, owner, generation, addr, new_level, ptrs, 2);
        checksum_block(outbuf, mnt->nodesize);
        if (btrfs_write_node(mnt, addr, outbuf) != VFS_OK) { mem_free(outbuf); return VFS_ERR_IO; }
        mem_free(outbuf);

        *root_addr = addr;
        *root_level = new_level;
    } else {
        *root_addr = left.addr;
    }

    return VFS_OK;
}

/* Top-level fs-tree operation: fs tree commit -> this subvolume's
 * ROOT_ITEM in the root tree -> superblock. Each of the (up to) two
 * B-tree commits below is its own atomic on-disk step, and the
 * superblock write is the very last thing touched - a crash at any point
 * before it leaves the previous, still-fully-valid tree state as the one
 * a subsequent mount sees (more, smaller atomic checkpoints than real
 * btrfs's one-commit-per-transaction, at the cost of more superblock
 * rewrites; simpler, and still crash-safe by the same argument). */

static int btrfs_do_fs_op(btrfs_mount_t *mnt, btrfs_key_t key, const void *data, uint16_t len, btw_op_t op)
{
    uint64_t new_gen = mnt->generation + 1;

    uint8_t *leaf_scratch = (uint8_t *)mem_alloc(mnt->nodesize);
    if (!leaf_scratch) return VFS_ERR_NOSPACE;
    btrfs_path_t path;
    int r = btrfs_search_path(mnt, mnt->fs_tree_root, mnt->fs_tree_level, key, leaf_scratch, &path);
    mem_free(leaf_scratch);
    if (r != VFS_OK) return r;

    uint64_t new_fs_root = mnt->fs_tree_root;
    uint8_t new_fs_level = mnt->fs_tree_level;
    r = btrfs_commit_tree(mnt, &path, BTRFS_FS_TREE_OBJECTID, new_gen, key, data, len, op, &new_fs_root, &new_fs_level);
    if (r != VFS_OK) return r;

    /* Update this subvolume's ROOT_ITEM (fixed size - never splits). */
    btrfs_key_t rikey = {BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0};
    uint8_t *rleaf = (uint8_t *)mem_alloc(mnt->nodesize);
    if (!rleaf) return VFS_ERR_NOSPACE;
    btrfs_path_t rpath;
    r = btrfs_search_path(mnt, mnt->root_tree_root, mnt->root_tree_level, rikey, rleaf, &rpath);
    if (r != VFS_OK) { mem_free(rleaf); return r; }

    btrfs_item_t rit; const uint8_t *rdata;
    r = item_at(rleaf, rpath.levels[0].index, &rit, &rdata);
    if (r != VFS_OK || rit.key.objectid != BTRFS_FS_TREE_OBJECTID || rit.key.type != BTRFS_ROOT_ITEM_KEY) {
        mem_free(rleaf);
        return VFS_ERR_IO;
    }
    btrfs_root_item_t root_item;
    memset(&root_item, 0, sizeof(root_item));
    size_t copy_len = sizeof(root_item) < rit.size ? sizeof(root_item) : rit.size;
    memcpy(&root_item, rdata, copy_len);
    mem_free(rleaf);

    root_item.bytenr = new_fs_root;
    root_item.level = new_fs_level;
    root_item.generation = new_gen;

    uint64_t new_root_tree_root = mnt->root_tree_root;
    uint8_t new_root_tree_level = mnt->root_tree_level;
    r = btrfs_commit_tree(mnt, &rpath, BTRFS_ROOT_TREE_OBJECTID, new_gen, rikey,
                           &root_item, (uint16_t)copy_len, BTW_UPDATE, &new_root_tree_root, &new_root_tree_level);
    if (r != VFS_OK) return r;

    r = btrfs_write_superblock(mnt, new_root_tree_root, new_root_tree_level, new_gen);
    if (r != VFS_OK) return r;

    mnt->fs_tree_root = new_fs_root;
    mnt->fs_tree_level = new_fs_level;
    mnt->root_tree_root = new_root_tree_root;
    mnt->root_tree_level = new_root_tree_level;
    mnt->generation = new_gen;
    return VFS_OK;
}

/* Deletes every item {ino, type, *} - used to drop a file's data extents
 * on truncate/unlink. Re-searches from scratch after each delete since
 * physical/logical addresses of everything shift on every commit. */
static int btrfs_delete_all_of_type(btrfs_mount_t *mnt, uint64_t ino, uint8_t type)
{
    for (;;) {
        btrfs_key_t target = {ino, type, 0};
        uint8_t *leaf = (uint8_t *)mem_alloc(mnt->nodesize);
        if (!leaf) return VFS_ERR_NOSPACE;
        uint32_t idx;
        if (btrfs_search(mnt, mnt->fs_tree_root, mnt->fs_tree_level, target, leaf, &idx) != VFS_OK) {
            mem_free(leaf);
            return VFS_ERR_IO;
        }
        btrfs_item_t it; const uint8_t *data;
        int found = (item_at(leaf, idx, &it, &data) == VFS_OK && it.key.objectid == ino && it.key.type == type);
        mem_free(leaf);
        if (!found) return VFS_OK;

        int r = btrfs_do_fs_op(mnt, it.key, NULL, 0, BTW_DELETE);
        if (r != VFS_OK) return r;
    }
}

/* Directory helpers */

static int btrfs_dir_find_index(btrfs_mount_t *mnt, uint64_t dir_ino, const char *name, int name_len,
                                 uint64_t *out_offset, uint64_t *out_child_ino, uint8_t *out_type)
{
    uint64_t pos = 0;
    for (;;) {
        uint64_t child; uint8_t type;
        int r = btrfs_dir_step(mnt, dir_ino, &pos, name, name_len, &child, &type, NULL, NULL);
        if (r == VFS_OK) {
            *out_offset = pos;
            *out_child_ino = child;
            if (out_type) *out_type = type;
            return VFS_OK;
        }
        if (r != VFS_ERR_NOTDIR) return VFS_ERR_NOTFOUND;
        btrfs_key_t bumped = key_successor((btrfs_key_t){dir_ino, BTRFS_DIR_INDEX_KEY, pos});
        pos = bumped.offset;
    }
}

/* Directory indices are derived from the tree on demand rather than persisted. */
static uint64_t btrfs_dir_next_index(btrfs_mount_t *mnt, uint64_t dir_ino)
{
    uint64_t pos = 0, max_seen = 1;
    for (;;) {
        uint64_t child; uint8_t type;
        int r = btrfs_dir_step(mnt, dir_ino, &pos, NULL, 0, &child, &type, NULL, NULL);
        if (r != VFS_OK) break;
        if (pos > max_seen) max_seen = pos;
        btrfs_key_t bumped = key_successor((btrfs_key_t){dir_ino, BTRFS_DIR_INDEX_KEY, pos});
        pos = bumped.offset;
    }
    return max_seen + 1;
}

static void split_path(const char *path, char *parent_out, uint32_t parent_out_size,
                        const char **base_out, int *base_len_out)
{
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    *base_out = base;
    *base_len_out = (int)strlen(base);

    uint32_t plen = (uint32_t)(base - path);
    if (plen == 0) {
        parent_out[0] = '/'; parent_out[1] = '\0';
    } else if (plen < parent_out_size) {
        memcpy(parent_out, path, plen);
        parent_out[plen] = '\0';
    } else {
        parent_out[0] = '\0';
    }
}

/* Wall clock (kernel/src/main.c: the PL031 RTC); absent in LX builds. */
extern uint32_t kernel_time_seconds(void) __attribute__((weak));

/* Store seconds and zero nanoseconds; the kernel clock has only second precision. */
static void stamp_now(uint8_t ts[12])
{
    if (!kernel_time_seconds) return;
    uint64_t t = kernel_time_seconds();
    for (int i = 0; i < 8; i++) ts[i] = (uint8_t)(t >> (8 * i));
    for (int i = 8; i < 12; i++) ts[i] = 0;
}

static void build_inode_item(btrfs_inode_item_t *out, uint32_t mode, uint64_t size, uint32_t nlink, uint64_t generation)
{
    memset(out, 0, sizeof(*out));
    out->generation = generation;
    out->transid = generation;
    out->size = size;
    out->nbytes = size;
    out->nlink = nlink;
    out->mode = mode;
    stamp_now(out->atime);
    stamp_now(out->ctime);
    stamp_now(out->mtime);
    stamp_now(out->otime);
}

static int insert_dir_entry(btrfs_mount_t *mnt, uint64_t parent_ino, const char *name, int name_len,
                             btrfs_key_t target, uint8_t ftype)
{
    uint64_t idx = btrfs_dir_next_index(mnt, parent_ino);
    if ((uint32_t)name_len > BTW_MAX_ITEM_DATA - sizeof(btrfs_dir_item_t)) return VFS_ERR_INVAL;

    uint8_t dbuf[BTW_MAX_ITEM_DATA];
    btrfs_dir_item_t di;
    memset(&di, 0, sizeof(di));
    di.location = target;
    di.transid = mnt->generation;
    di.name_len = (uint16_t)name_len;
    di.type = ftype;
    memcpy(dbuf, &di, sizeof(di));
    memcpy(dbuf + sizeof(di), name, (size_t)name_len);
    uint16_t dlen = (uint16_t)(sizeof(di) + name_len);

    btrfs_key_t dir_key = {parent_ino, BTRFS_DIR_INDEX_KEY, idx};
    return btrfs_do_fs_op(mnt, dir_key, dbuf, dlen, BTW_INSERT);
}

/* File / directory write operations (called from btrfs.c) */

int btrfs_write_open(btrfs_mount_t *mnt, const char *path, int flags, btrfs_file_t **out_file)
{
    uint64_t ino;
    btrfs_inode_item_t inode;
    int r = btrfs_resolve_path(mnt, path, &ino, &inode);

    if (r == VFS_OK && (inode.mode & BTRFS_S_IFMT) == BTRFS_S_IFDIR) return VFS_ERR_ISDIR;

    if (r != VFS_OK) {
        if (!(flags & O_CREAT)) return r;

        char parent_path[VFS_PATH_MAX];
        const char *base; int base_len;
        split_path(path, parent_path, sizeof(parent_path), &base, &base_len);
        if (base_len == 0 || base_len > VFS_NAME_MAX || parent_path[0] == '\0') return VFS_ERR_INVAL;

        uint64_t parent_ino; btrfs_inode_item_t parent_inode;
        if (btrfs_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
        if ((parent_inode.mode & BTRFS_S_IFMT) != BTRFS_S_IFDIR) return VFS_ERR_NOTDIR;

        uint64_t new_ino = mnt->next_ino;
        btrfs_inode_item_t new_inode;
        build_inode_item(&new_inode, BTRFS_S_IFREG | 0644, 0, 1, mnt->generation + 1);

        btrfs_key_t inode_key = {new_ino, BTRFS_INODE_ITEM_KEY, 0};
        r = btrfs_do_fs_op(mnt, inode_key, &new_inode, sizeof(new_inode), BTW_INSERT);
        if (r != VFS_OK) return r;

        r = insert_dir_entry(mnt, parent_ino, base, base_len, inode_key, BTRFS_FT_REG_FILE);
        if (r != VFS_OK) return r;

        mnt->next_ino = new_ino + 1;
        ino = new_ino;
        inode = new_inode;
    } else if (flags & O_TRUNC) {
        r = btrfs_delete_all_of_type(mnt, ino, BTRFS_EXTENT_DATA_KEY);
        if (r != VFS_OK) return r;
        build_inode_item(&inode, inode.mode, 0, inode.nlink, mnt->generation + 1);
        btrfs_key_t ikey = {ino, BTRFS_INODE_ITEM_KEY, 0};
        r = btrfs_do_fs_op(mnt, ikey, &inode, sizeof(inode), BTW_UPDATE);
        if (r != VFS_OK) return r;
    }

    btrfs_file_t *f = (btrfs_file_t *)mem_alloc(sizeof(btrfs_file_t));
    if (!f) return VFS_ERR_NOSPACE;
    f->mnt = mnt;
    f->ino = ino;
    f->inode = inode;
    f->pos = (flags & O_APPEND) ? inode.size : 0;
    f->flags = flags;
    *out_file = f;
    return VFS_OK;
}

int btrfs_write_data(btrfs_file_t *f, const void *buf, uint32_t len, uint32_t *actual)
{
    *actual = 0;
    if (len == 0) return VFS_OK;
    /* Only sequential/append writes at the current end of the file's data
     * are supported - see this file's header comment. */
    if (f->pos != f->inode.size) return VFS_ERR_INVAL;

    btrfs_mount_t *mnt = f->mnt;
    uint64_t alloc_len = ((uint64_t)len + mnt->sectorsize - 1) & ~((uint64_t)mnt->sectorsize - 1);

    uint64_t disk_addr;
    int r = btrfs_alloc_extent(mnt, alloc_len, &disk_addr);
    if (r != VFS_OK) return r;
    if (btrfs_write_logical(mnt, disk_addr, buf, len) != VFS_OK) return VFS_ERR_IO;

    btrfs_file_extent_item_t fe;
    memset(&fe, 0, sizeof(fe));
    fe.generation = mnt->generation + 1;
    fe.ram_bytes = len;
    fe.compression = BTRFS_COMPRESS_NONE;
    fe.type = BTRFS_FILE_EXTENT_REG;
    fe.disk_bytenr = disk_addr;
    fe.disk_num_bytes = alloc_len;
    fe.offset = 0;
    fe.num_bytes = len;

    btrfs_key_t key = {f->ino, BTRFS_EXTENT_DATA_KEY, f->pos};
    r = btrfs_do_fs_op(mnt, key, &fe, sizeof(fe), BTW_INSERT);
    if (r != VFS_OK) return r;

    f->inode.size += len;
    f->inode.nbytes += len;
    f->inode.generation = mnt->generation;
    stamp_now(f->inode.mtime);
    stamp_now(f->inode.ctime);
    btrfs_key_t ikey = {f->ino, BTRFS_INODE_ITEM_KEY, 0};
    r = btrfs_do_fs_op(mnt, ikey, &f->inode, sizeof(f->inode), BTW_UPDATE);
    if (r != VFS_OK) return r;

    f->pos += len;
    *actual = len;
    return VFS_OK;
}

int btrfs_do_unlink(btrfs_mount_t *mnt, const char *path)
{
    uint64_t ino; btrfs_inode_item_t inode;
    int r = btrfs_resolve_path(mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;
    if ((inode.mode & BTRFS_S_IFMT) == BTRFS_S_IFDIR) {
        /* Directories go only when empty (RD); never the root. */
        uint64_t pos = 0, child; uint8_t type;
        if (ino == BTRFS_FIRST_FREE_OBJECTID || path[0] == '\0' || (path[0] == '/' && !path[1]))
            return VFS_ERR_BUSY;
        if (btrfs_dir_step(mnt, ino, &pos, NULL, 0, &child, &type, NULL, NULL) == VFS_OK)
            return VFS_ERR_BUSY;
    }

    char parent_path[VFS_PATH_MAX];
    const char *base; int base_len;
    split_path(path, parent_path, sizeof(parent_path), &base, &base_len);

    uint64_t parent_ino; btrfs_inode_item_t parent_inode;
    if (btrfs_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_IO;

    uint64_t dir_offset, child_ino; uint8_t type;
    if (btrfs_dir_find_index(mnt, parent_ino, base, base_len, &dir_offset, &child_ino, &type) != VFS_OK)
        return VFS_ERR_NOTFOUND;

    r = btrfs_delete_all_of_type(mnt, ino, BTRFS_EXTENT_DATA_KEY);
    if (r != VFS_OK) return r;

    r = btrfs_do_fs_op(mnt, (btrfs_key_t){ino, BTRFS_INODE_ITEM_KEY, 0}, NULL, 0, BTW_DELETE);
    if (r != VFS_OK) return r;

    return btrfs_do_fs_op(mnt, (btrfs_key_t){parent_ino, BTRFS_DIR_INDEX_KEY, dir_offset}, NULL, 0, BTW_DELETE);
}

int btrfs_do_mkdir(btrfs_mount_t *mnt, const char *path)
{
    uint64_t existing_ino; btrfs_inode_item_t existing;
    if (btrfs_resolve_path(mnt, path, &existing_ino, &existing) == VFS_OK) return VFS_ERR_INVAL;

    char parent_path[VFS_PATH_MAX];
    const char *base; int base_len;
    split_path(path, parent_path, sizeof(parent_path), &base, &base_len);
    if (base_len == 0 || base_len > VFS_NAME_MAX || parent_path[0] == '\0') return VFS_ERR_INVAL;

    uint64_t parent_ino; btrfs_inode_item_t parent_inode;
    if (btrfs_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
    if ((parent_inode.mode & BTRFS_S_IFMT) != BTRFS_S_IFDIR) return VFS_ERR_NOTDIR;

    uint64_t new_ino = mnt->next_ino;
    btrfs_inode_item_t new_inode;
    build_inode_item(&new_inode, BTRFS_S_IFDIR | 0755, 0, 1, mnt->generation + 1);

    btrfs_key_t inode_key = {new_ino, BTRFS_INODE_ITEM_KEY, 0};
    int r = btrfs_do_fs_op(mnt, inode_key, &new_inode, sizeof(new_inode), BTW_INSERT);
    if (r != VFS_OK) return r;

    r = insert_dir_entry(mnt, parent_ino, base, base_len, inode_key, BTRFS_FT_DIR);
    if (r != VFS_OK) return r;

    mnt->next_ino = new_ino + 1;
    return VFS_OK;
}

int btrfs_do_rename(btrfs_mount_t *mnt, const char *old_path, const char *new_path)
{
    uint64_t ino; btrfs_inode_item_t inode;
    if (btrfs_resolve_path(mnt, old_path, &ino, &inode) != VFS_OK) return VFS_ERR_NOTFOUND;

    uint64_t existing_ino; btrfs_inode_item_t existing_inode;
    if (btrfs_resolve_path(mnt, new_path, &existing_ino, &existing_inode) == VFS_OK) return VFS_ERR_INVAL;

    char oparent[VFS_PATH_MAX];
    const char *obase; int olen;
    split_path(old_path, oparent, sizeof(oparent), &obase, &olen);
    uint64_t oparent_ino; btrfs_inode_item_t oparent_inode;
    if (btrfs_resolve_path(mnt, oparent, &oparent_ino, &oparent_inode) != VFS_OK) return VFS_ERR_IO;

    char nparent[VFS_PATH_MAX];
    const char *nbase; int nlen;
    split_path(new_path, nparent, sizeof(nparent), &nbase, &nlen);
    if (nlen == 0 || nlen > VFS_NAME_MAX || nparent[0] == '\0') return VFS_ERR_INVAL;
    uint64_t nparent_ino; btrfs_inode_item_t nparent_inode;
    if (btrfs_resolve_path(mnt, nparent, &nparent_ino, &nparent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
    if ((nparent_inode.mode & BTRFS_S_IFMT) != BTRFS_S_IFDIR) return VFS_ERR_NOTDIR;

    uint64_t old_dir_offset, old_child_ino; uint8_t old_type;
    if (btrfs_dir_find_index(mnt, oparent_ino, obase, olen, &old_dir_offset, &old_child_ino, &old_type) != VFS_OK)
        return VFS_ERR_NOTFOUND;

    int r = btrfs_do_fs_op(mnt, (btrfs_key_t){oparent_ino, BTRFS_DIR_INDEX_KEY, old_dir_offset}, NULL, 0, BTW_DELETE);
    if (r != VFS_OK) return r;

    return insert_dir_entry(mnt, nparent_ino, nbase, nlen, (btrfs_key_t){ino, BTRFS_INODE_ITEM_KEY, 0}, old_type);
}

/* Path-tracking B-tree search (extends btrfs_search() with ancestry) */

static int btrfs_search_path(btrfs_mount_t *mnt, uint64_t root_logical, uint8_t root_level,
                              btrfs_key_t target, uint8_t *leaf_out, btrfs_path_t *path)
{
    if ((uint32_t)root_level + 1 > BTRFS_MAX_TREE_HEIGHT) return VFS_ERR_IO;
    path->height = (uint8_t)(root_level + 1);

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
            path->levels[0].logical = logical;
            path->levels[0].index = i;
            return VFS_OK;
        }

        path->levels[level].logical = logical;

        const uint8_t *ptr_base = leaf_out + BTRFS_HEADER_SIZE;
        int chosen = -1;
        for (uint32_t i = 0; i < hdr->nritems; i++) {
            btrfs_key_ptr_t kp;
            memcpy(&kp, ptr_base + (size_t)i * BTRFS_KEY_PTR_SIZE, sizeof(kp));
            if (key_cmp(&kp.key, &target) <= 0) chosen = (int)i;
            else break;
        }
        if (chosen < 0) chosen = 0;
        path->levels[level].index = (uint32_t)chosen;

        btrfs_key_ptr_t kp;
        memcpy(&kp, ptr_base + (size_t)chosen * BTRFS_KEY_PTR_SIZE, sizeof(kp));
        logical = kp.blockptr;
        level--;
    }
}
