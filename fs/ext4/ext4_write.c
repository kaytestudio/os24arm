/**
 * @file ext4_write.c
 * @brief ext4 write-side filesystem operations (mkdir/create/write/
 *        truncate/unlink/rename) - the bitmap-allocator engine that sits
 *        alongside ext4.c's read-only mount/read/lookup code.
 *
 * IMPORTANT - what this does and doesn't maintain, same spirit as
 * fs/btrfs/btrfs_write.c's own header comment:
 *
 * Write support is refused (the mount falls back to read-only, exactly
 * like any other "this layout isn't supported for writing" case) whenever
 * the on-disk feature flags include EXT4_FEATURE_RO_COMPAT_GDT_CSUM
 * ("uninit_bg"), EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, or
 * EXT4_FEATURE_INCOMPAT_META_BG - see ext4_write_init_allocators() below.
 * The first two would require maintaining CRC16/CRC32C checksums over
 * every group descriptor, block/inode bitmap, and (metadata_csum only)
 * every inode on every mutation; META_BG changes where the group-
 * descriptor table itself lives. All three are large, easy-to-get-subtly-
 * wrong subsystems in their own right and are simply out of scope here -
 * this driver only ever writes to images without them
 * (e.g. `mkfs.ext4 -O ^64bit,^metadata_csum,^uninit_bg`).
 *
 * Within that gate, what IS maintained correctly: real per-group block/
 * inode bitmap allocation (first-fit, group-local, with parent-group
 * locality), the primary superblock and group descriptors' free-space/
 * used-directory counts, proper extent-tree data-block mapping for every
 * inode this driver creates, and standard "."/".." directory bootstrapping
 * with correct link-count bookkeeping.
 *
 * Explicit, documented cuts:
 *   - Writes are sequential/append-only (mirrors btrfs_write.c exactly -
 *     `if (f->pos != inode_size) return VFS_ERR_INVAL` - the VFS layer
 *     never wires up .seek for a writable fd in this project either).
 *   - No hardlinks: every regular file this driver creates has
 *     links_count == 1, so unlink always frees its inode immediately.
 *   - Every inode this driver creates uses an inline (depth-0, in the
 *     60-byte i_block root) extent tree, capped at 4 extents - no index-
 *     block/tree-depth growth. A pre-existing classic-mapped inode can
 *     only be appended into its remaining direct block pointers (up to
 *     12) - no indirect-block growth either. Both return VFS_ERR_NOSPACE
 *     past their cap: small directories/files only, which is exactly what
 *     this project's boot-time UNIXROOT tree (kernel/src/unixroot.c) and
 *     typical config/data files need.
 *   - EXT4_INLINE_DATA_FL is never checked (pre-existing read-path gap,
 *     not introduced here) - files/dirs using inline data aren't read or
 *     written specially.
 *   - Only the primary superblock/group descriptors are kept in sync;
 *     sparse_super backup copies go stale, same as after any real
 *     filesystem resize until the next resize2fs/fsck pass.
 *   - Not crash-safe: a mutation can touch several blocks (bitmap, group
 *     descriptor, superblock, inode, directory data) across several
 *     separate writes with no journal or atomic commit step. Fine for
 *     this project's normal operation; a power loss mid-write can leave
 *     the image needing `e2fsck -f` before its next real-Linux mount.
 */

#include "ext4_internal.h"
#include "string.h"
#include "blkdev.h"

/* The 60-byte i_block area holds a 12-byte extent header plus up to 4
 * 12-byte extent entries - see the header comment above. */
#define EXT4_ROOT_EXTENT_MAX 4

/* ── Bit helpers ─────────────────────────────────────────────────────────── */

static inline int bit_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static inline void bit_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static inline void bit_clear(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* ── lo/hi field accessors ───────────────────────────────────────────────── */

static uint32_t gd_free_blocks(const ext4_group_desc_t *gd)
{
    return gd->bg_free_blocks_count_lo | ((uint32_t)gd->bg_free_blocks_count_hi << 16);
}
static void gd_set_free_blocks(ext4_group_desc_t *gd, uint32_t v)
{
    gd->bg_free_blocks_count_lo = (uint16_t)(v & 0xFFFFu);
    gd->bg_free_blocks_count_hi = (uint16_t)(v >> 16);
}
static uint32_t gd_free_inodes(const ext4_group_desc_t *gd)
{
    return gd->bg_free_inodes_count_lo | ((uint32_t)gd->bg_free_inodes_count_hi << 16);
}
static void gd_set_free_inodes(ext4_group_desc_t *gd, uint32_t v)
{
    gd->bg_free_inodes_count_lo = (uint16_t)(v & 0xFFFFu);
    gd->bg_free_inodes_count_hi = (uint16_t)(v >> 16);
}
static uint32_t gd_used_dirs(const ext4_group_desc_t *gd)
{
    return gd->bg_used_dirs_count_lo | ((uint32_t)gd->bg_used_dirs_count_hi << 16);
}
static void gd_set_used_dirs(ext4_group_desc_t *gd, uint32_t v)
{
    gd->bg_used_dirs_count_lo = (uint16_t)(v & 0xFFFFu);
    gd->bg_used_dirs_count_hi = (uint16_t)(v >> 16);
}
static uint64_t sb_free_blocks(const ext4_superblock_t *sb)
{
    return sb->s_free_blocks_count_lo | ((uint64_t)sb->s_free_blocks_count_hi << 32);
}
static void sb_set_free_blocks(ext4_superblock_t *sb, uint64_t v)
{
    sb->s_free_blocks_count_lo = (uint32_t)(v & 0xFFFFFFFFu);
    sb->s_free_blocks_count_hi = (uint32_t)(v >> 32);
}

/* Blocks/inodes actually backed by group g's bitmap - the last group can be
 * short of a full blocks_per_group/inodes_per_group. */
static uint32_t group_block_count(ext4_mount_t *mnt, uint32_t g)
{
    uint64_t total = mnt->sb.s_blocks_count_lo | ((uint64_t)mnt->sb.s_blocks_count_hi << 32);
    uint64_t first = (uint64_t)mnt->sb.s_first_data_block + (uint64_t)g * mnt->blocks_per_group;
    if (first >= total) return 0;
    uint64_t remain = total - first;
    return (uint32_t)((remain < mnt->blocks_per_group) ? remain : mnt->blocks_per_group);
}
static uint32_t group_inode_count(ext4_mount_t *mnt, uint32_t g)
{
    uint32_t first = g * mnt->inodes_per_group;
    if (first >= mnt->sb.s_inodes_count) return 0;
    uint32_t remain = mnt->sb.s_inodes_count - first;
    return (remain < mnt->inodes_per_group) ? remain : mnt->inodes_per_group;
}

/* ── Raw block / superblock / group-descriptor write-back ───────────────── */

static int ext4_write_block(ext4_mount_t *mnt, uint64_t block_num, const void *buf)
{
    blkdev_t *dev = (blkdev_t *)mnt->dev;
    uint32_t sectors_per_block = mnt->block_size / dev->sector_size;
    if (sectors_per_block == 0) sectors_per_block = 1;
    int64_t sector = (int64_t)(block_num * sectors_per_block);
    return (blk_write(dev, sector, sectors_per_block, buf) == 0) ? VFS_OK : VFS_ERR_IO;
}

/* The superblock lives at a fixed byte offset (1024) regardless of fs
 * block size, so - like ext4_mount_impl's original read of it - this goes
 * straight through sector I/O rather than ext4_write_block(). */
static int ext4_write_superblock(ext4_mount_t *mnt)
{
    blkdev_t *dev = (blkdev_t *)mnt->dev;
    if (dev->sector_size == 0) return VFS_ERR_INVAL;
    int64_t sb_sector = EXT4_SUPER_OFFSET / dev->sector_size;
    uint32_t sb_sectors = 1024 / dev->sector_size;
    if (sb_sectors < 1) sb_sectors = 1;

    uint8_t sb_raw[1024];
    if (blk_read(dev, sb_sector, sb_sectors, sb_raw) != 0) return VFS_ERR_IO;
    size_t n = sizeof(mnt->sb);
    if (n > sizeof(sb_raw)) n = sizeof(sb_raw);
    memcpy(sb_raw, &mnt->sb, n);
    return (blk_write(dev, sb_sector, sb_sectors, sb_raw) == 0) ? VFS_OK : VFS_ERR_IO;
}

/* Mirrors ext4_mount_impl's read-side byte-offset math exactly, so mount
 * and this stay in lock-step. Uses mnt->bitmap_buffer as scratch - callers
 * must be done with any bitmap they read into it first (see
 * ext4_alloc_block/ext4_alloc_inode: bitmap is written back before this
 * is called). */
static int ext4_write_group_desc(ext4_mount_t *mnt, uint32_t group)
{
    uint64_t gdt_start_block = (uint64_t)mnt->sb.s_first_data_block + 1;
    uint64_t byte_off = gdt_start_block * mnt->block_size + (uint64_t)group * mnt->desc_size;
    uint64_t blk = byte_off / mnt->block_size;
    uint32_t off_in_blk = (uint32_t)(byte_off % mnt->block_size);

    if (ext4_read_block(mnt, blk, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;
    size_t copy_len = mnt->desc_size < sizeof(ext4_group_desc_t) ? mnt->desc_size : sizeof(ext4_group_desc_t);
    memcpy(mnt->bitmap_buffer + off_in_blk, &mnt->group_desc[group], copy_len);
    return ext4_write_block(mnt, blk, mnt->bitmap_buffer);
}

/* ── Bitmap allocators ───────────────────────────────────────────────────── */

static int ext4_alloc_block(ext4_mount_t *mnt, uint32_t pref_group, uint64_t *out_block)
{
    for (uint32_t attempt = 0; attempt < mnt->group_count; attempt++) {
        uint32_t g = (pref_group + attempt) % mnt->group_count;
        if (gd_free_blocks(&mnt->group_desc[g]) == 0) continue;

        uint64_t bitmap_block = mnt->group_desc[g].bg_block_bitmap_lo |
                                 ((uint64_t)mnt->group_desc[g].bg_block_bitmap_hi << 32);
        if (ext4_read_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;

        uint32_t nblocks = group_block_count(mnt, g);
        uint32_t bit = 0;
        while (bit < nblocks && bit_test(mnt->bitmap_buffer, bit)) bit++;
        if (bit >= nblocks) continue;

        bit_set(mnt->bitmap_buffer, bit);
        if (ext4_write_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;

        uint64_t block_num = (uint64_t)mnt->sb.s_first_data_block + (uint64_t)g * mnt->blocks_per_group + bit;

        /* Zero the block's content before handing it out (uses
         * extent_buffer - safe, nothing else needs it mid-allocation). */
        memset(mnt->extent_buffer, 0, mnt->block_size);
        if (ext4_write_block(mnt, block_num, mnt->extent_buffer) != VFS_OK) return VFS_ERR_IO;

        gd_set_free_blocks(&mnt->group_desc[g], gd_free_blocks(&mnt->group_desc[g]) - 1);
        if (ext4_write_group_desc(mnt, g) != VFS_OK) return VFS_ERR_IO;

        sb_set_free_blocks(&mnt->sb, sb_free_blocks(&mnt->sb) - 1);
        if (ext4_write_superblock(mnt) != VFS_OK) return VFS_ERR_IO;

        *out_block = block_num;
        return VFS_OK;
    }
    return VFS_ERR_NOSPACE;
}

static int ext4_alloc_inode(ext4_mount_t *mnt, uint32_t pref_group, int is_dir, uint32_t *out_ino)
{
    for (uint32_t attempt = 0; attempt < mnt->group_count; attempt++) {
        uint32_t g = (pref_group + attempt) % mnt->group_count;
        if (gd_free_inodes(&mnt->group_desc[g]) == 0) continue;

        uint64_t bitmap_block = mnt->group_desc[g].bg_inode_bitmap_lo |
                                 ((uint64_t)mnt->group_desc[g].bg_inode_bitmap_hi << 32);
        if (ext4_read_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;

        uint32_t ninodes = group_inode_count(mnt, g);
        uint32_t bit = 0;
        while (bit < ninodes && bit_test(mnt->bitmap_buffer, bit)) bit++;
        if (bit >= ninodes) continue;

        bit_set(mnt->bitmap_buffer, bit);
        if (ext4_write_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;

        uint32_t ino = g * mnt->inodes_per_group + bit + 1;

        /* Zero the new inode's on-disk slot (extent_buffer - safe here,
         * same reasoning as ext4_alloc_block above). */
        uint64_t inode_table_block = mnt->group_desc[g].bg_inode_table_lo |
                                      ((uint64_t)mnt->group_desc[g].bg_inode_table_hi << 32);
        uint64_t byte_off = inode_table_block * mnt->block_size + (uint64_t)bit * mnt->inode_size;
        uint64_t blk = byte_off / mnt->block_size;
        uint32_t off_in_blk = (uint32_t)(byte_off % mnt->block_size);
        if (ext4_read_block(mnt, blk, mnt->extent_buffer) != VFS_OK) return VFS_ERR_IO;
        memset(mnt->extent_buffer + off_in_blk, 0, mnt->inode_size);
        if (ext4_write_block(mnt, blk, mnt->extent_buffer) != VFS_OK) return VFS_ERR_IO;

        gd_set_free_inodes(&mnt->group_desc[g], gd_free_inodes(&mnt->group_desc[g]) - 1);
        if (is_dir) gd_set_used_dirs(&mnt->group_desc[g], gd_used_dirs(&mnt->group_desc[g]) + 1);
        if (ext4_write_group_desc(mnt, g) != VFS_OK) return VFS_ERR_IO;

        mnt->sb.s_free_inodes_count -= 1;
        if (ext4_write_superblock(mnt) != VFS_OK) return VFS_ERR_IO;

        *out_ino = ino;
        return VFS_OK;
    }
    return VFS_ERR_NOSPACE;
}

static int ext4_free_block(ext4_mount_t *mnt, uint64_t block_num)
{
    uint64_t rel = block_num - mnt->sb.s_first_data_block;
    uint32_t g = (uint32_t)(rel / mnt->blocks_per_group);
    uint32_t bit = (uint32_t)(rel % mnt->blocks_per_group);
    if (g >= mnt->group_count) return VFS_ERR_INVAL;

    uint64_t bitmap_block = mnt->group_desc[g].bg_block_bitmap_lo |
                             ((uint64_t)mnt->group_desc[g].bg_block_bitmap_hi << 32);
    if (ext4_read_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;
    bit_clear(mnt->bitmap_buffer, bit);
    if (ext4_write_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;

    gd_set_free_blocks(&mnt->group_desc[g], gd_free_blocks(&mnt->group_desc[g]) + 1);
    if (ext4_write_group_desc(mnt, g) != VFS_OK) return VFS_ERR_IO;

    sb_set_free_blocks(&mnt->sb, sb_free_blocks(&mnt->sb) + 1);
    return ext4_write_superblock(mnt);
}

static int ext4_free_inode(ext4_mount_t *mnt, uint32_t ino, int was_dir)
{
    uint32_t g = (ino - 1) / mnt->inodes_per_group;
    uint32_t bit = (ino - 1) % mnt->inodes_per_group;
    if (g >= mnt->group_count) return VFS_ERR_INVAL;

    uint64_t bitmap_block = mnt->group_desc[g].bg_inode_bitmap_lo |
                             ((uint64_t)mnt->group_desc[g].bg_inode_bitmap_hi << 32);
    if (ext4_read_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;
    bit_clear(mnt->bitmap_buffer, bit);
    if (ext4_write_block(mnt, bitmap_block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;

    gd_set_free_inodes(&mnt->group_desc[g], gd_free_inodes(&mnt->group_desc[g]) + 1);
    if (was_dir) gd_set_used_dirs(&mnt->group_desc[g], gd_used_dirs(&mnt->group_desc[g]) - 1);
    if (ext4_write_group_desc(mnt, g) != VFS_OK) return VFS_ERR_IO;

    mnt->sb.s_free_inodes_count += 1;
    return ext4_write_superblock(mnt);
}

/* ── Extent tree (inline root only - see header comment) ─────────────────── */

static void extent_root_init(ext4_inode_t *inode)
{
    ext4_extent_header_t h;
    h.eh_magic = EXT4_EXTENT_MAGIC;
    h.eh_entries = 0;
    h.eh_max = EXT4_ROOT_EXTENT_MAX;
    h.eh_depth = 0;
    h.eh_generation = 0;
    memset(inode->i_block, 0, sizeof(inode->i_block));
    memcpy(inode->i_block, &h, sizeof(h));
    inode->i_flags |= EXT4_EXTENTS_FL;
}

/* Appends one physical block as the next logical block of inode's data
 * (logical_block is always the current block count - this driver only
 * ever grows a file/directory at its end). Extends the last extent when
 * phys_block is contiguous with it, else adds a new entry, capped at the
 * inline root's 4-entry capacity. A pre-existing classic (non-extent)
 * inode instead fills its next free direct pointer (up to 12, no
 * indirect-block growth). */
static int ext4_inode_append_block(ext4_inode_t *inode, uint32_t logical_block, uint64_t phys_block)
{
    if (!(inode->i_flags & EXT4_EXTENTS_FL)) {
        if (logical_block >= EXT4_NDIR_BLOCKS) return VFS_ERR_NOSPACE;
        inode->i_block[logical_block] = (uint32_t)phys_block;
        return VFS_OK;
    }

    ext4_extent_header_t *h = (ext4_extent_header_t *)inode->i_block;
    ext4_extent_t *ext = (ext4_extent_t *)((uint8_t *)inode->i_block + sizeof(*h));

    if (h->eh_entries > 0) {
        ext4_extent_t *last = &ext[h->eh_entries - 1];
        if (last->ee_len < 32768) {
            uint64_t last_phys = ((uint64_t)last->ee_start_hi << 32) | last->ee_start_lo;
            if (last->ee_block + (uint32_t)last->ee_len == logical_block &&
                last_phys + last->ee_len == phys_block) {
                last->ee_len++;
                return VFS_OK;
            }
        }
    }

    if (h->eh_entries >= h->eh_max) return VFS_ERR_NOSPACE;

    ext4_extent_t *slot = &ext[h->eh_entries];
    slot->ee_block = logical_block;
    slot->ee_len = 1;
    slot->ee_start_hi = (uint16_t)(phys_block >> 32);
    slot->ee_start_lo = (uint32_t)(phys_block & 0xFFFFFFFFu);
    h->eh_entries++;
    return VFS_OK;
}

/* ── Inode create / write-back ───────────────────────────────────────────── */

static void build_new_inode(ext4_inode_t *out, uint32_t mode, uint16_t links, uint64_t size)
{
    memset(out, 0, sizeof(*out));
    out->i_mode = (uint16_t)mode;
    out->i_links_count = links;
    out->i_size_lo = (uint32_t)(size & 0xFFFFFFFFu);
    out->i_size_high = (uint32_t)(size >> 32);
    extent_root_init(out);
}

/* Mirrors ext4_read_inode's byte-offset math exactly. */
static int ext4_write_inode(ext4_mount_t *mnt, uint32_t ino, const ext4_inode_t *inode)
{
    uint32_t group = (ino - 1) / mnt->inodes_per_group;
    uint32_t index = (ino - 1) % mnt->inodes_per_group;
    if (group >= mnt->group_count) return VFS_ERR_INVAL;

    uint64_t inode_table_block = mnt->group_desc[group].bg_inode_table_lo |
                                  ((uint64_t)mnt->group_desc[group].bg_inode_table_hi << 32);
    uint64_t byte_off = inode_table_block * mnt->block_size + (uint64_t)index * mnt->inode_size;
    uint64_t block_num = byte_off / mnt->block_size;
    uint32_t block_off = (uint32_t)(byte_off % mnt->block_size);

    if (ext4_read_block(mnt, block_num, mnt->extent_buffer) != VFS_OK) return VFS_ERR_IO;
    size_t copy_len = sizeof(*inode);
    if (copy_len > mnt->inode_size) copy_len = mnt->inode_size;
    memcpy(mnt->extent_buffer + block_off, inode, copy_len);
    return ext4_write_block(mnt, block_num, mnt->extent_buffer);
}

/* Allocates one new block and appends it (extent tree / direct pointers +
 * i_blocks_lo) at inode's next_lb logical position - purely in-memory on
 * inode past the allocation itself; callers finish updating i_size and
 * persist with one final ext4_write_inode() once they know how many bytes
 * of the new block they actually used.
 *
 * next_lb is an explicit caller-supplied parameter rather than derived
 * from ext4_inode_size(inode) here: a caller appending several blocks in
 * one operation (ext4_write_data's multi-block loop) only updates i_size
 * once, at the very end, so deriving next_lb from the inode's i_size on
 * every call would keep recomputing the SAME (stale) logical block and
 * silently create overlapping/duplicate extent entries. */
static int ext4_grow_inode(ext4_mount_t *mnt, ext4_inode_t *inode, uint32_t next_lb,
                            uint32_t pref_group, uint64_t *out_phys)
{
    uint64_t phys;
    int r = ext4_alloc_block(mnt, pref_group, &phys);
    if (r != VFS_OK) return r;

    r = ext4_inode_append_block(inode, next_lb, phys);
    if (r != VFS_OK) { ext4_free_block(mnt, phys); return r; }

    inode->i_blocks_lo += mnt->block_size / 512;
    *out_phys = phys;
    return VFS_OK;
}

/* ── Directory entries ───────────────────────────────────────────────────── */

static void put_dirent(uint8_t *block, uint32_t offset, uint32_t ino, uint16_t rec_len,
                        const char *name, int name_len, uint8_t file_type)
{
    ext4_dirent_t *de = (ext4_dirent_t *)(block + offset);
    de->inode = ino;
    de->rec_len = rec_len;
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy(de->name, name, (size_t)name_len);
}

static void init_dir_block(uint8_t *block, uint32_t block_size, uint32_t self_ino, uint32_t parent_ino)
{
    memset(block, 0, block_size);
    uint16_t dot_len = (uint16_t)EXT4_DIR_REC_LEN(1);
    put_dirent(block, 0, self_ino, dot_len, ".", 1, EXT4_FT_DIR);
    put_dirent(block, dot_len, parent_ino, (uint16_t)(block_size - dot_len), "..", 2, EXT4_FT_DIR);
}

static void init_free_dir_block(uint8_t *block, uint32_t block_size)
{
    memset(block, 0, block_size);
    ext4_dirent_t *de = (ext4_dirent_t *)block;
    de->inode = 0;
    de->rec_len = (uint16_t)block_size;
}

/* First-fit insert into dir_ino's directory data - see this file's header
 * comment for the slack/split strategy. Grows the directory by one block
 * (via ext4_grow_inode, which also persists dir_inode through
 * ext4_write_inode when it does) if no existing block has room. */
static int ext4_dir_insert(ext4_mount_t *mnt, uint32_t dir_ino, ext4_inode_t *dir_inode,
                            const char *name, int name_len, uint32_t child_ino, uint8_t file_type)
{
    uint32_t needed = EXT4_DIR_REC_LEN(name_len);
    uint64_t file_size = ext4_inode_size(dir_inode);
    uint32_t num_blocks = (uint32_t)((file_size + mnt->block_size - 1) / mnt->block_size);

    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        uint64_t phys = 0;
        if (ext4_map_block(mnt, dir_inode, lb, &phys) != VFS_OK) return VFS_ERR_IO;
        if (phys == 0) continue;
        if (ext4_read_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;

        uint32_t off = 0;
        while (off + 8 <= mnt->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(mnt->block_buffer + off);
            if (de->rec_len < 8) break; /* corrupt entry; give up on this block */

            uint32_t slack;
            if (de->inode == 0) {
                slack = de->rec_len;
            } else {
                uint32_t real = EXT4_DIR_REC_LEN(de->name_len);
                slack = (de->rec_len > real) ? (de->rec_len - real) : 0;
            }

            if (slack >= needed) {
                if (de->inode == 0) {
                    put_dirent(mnt->block_buffer, off, child_ino, de->rec_len, name, name_len, file_type);
                } else {
                    uint32_t real = EXT4_DIR_REC_LEN(de->name_len);
                    uint16_t old_rec_len = de->rec_len;
                    de->rec_len = (uint16_t)real;
                    put_dirent(mnt->block_buffer, off + real, child_ino,
                               (uint16_t)(old_rec_len - real), name, name_len, file_type);
                }
                return ext4_write_block(mnt, phys, mnt->block_buffer);
            }
            off += de->rec_len;
        }
    }

    /* No room anywhere - append a fresh, all-free block. name_len <= 255
     * (VFS_NAME_MAX) guarantees needed <= EXT4_DIR_REC_LEN(255) = 264,
     * always <= any valid ext4 block size (>= 1024), so this always fits. */
    uint32_t pref_group = (dir_ino - 1) / mnt->inodes_per_group;
    uint64_t phys;
    int r = ext4_grow_inode(mnt, dir_inode, num_blocks, pref_group, &phys);
    if (r != VFS_OK) return r;

    init_free_dir_block(mnt->block_buffer, mnt->block_size);
    put_dirent(mnt->block_buffer, 0, child_ino, (uint16_t)mnt->block_size, name, name_len, file_type);
    r = ext4_write_block(mnt, phys, mnt->block_buffer);
    if (r != VFS_OK) return r;

    uint64_t new_size = file_size + mnt->block_size;
    dir_inode->i_size_lo = (uint32_t)(new_size & 0xFFFFFFFFu);
    dir_inode->i_size_high = (uint32_t)(new_size >> 32);
    return ext4_write_inode(mnt, dir_ino, dir_inode);
}

/* Zeroes the entry's inode field in place - a spec-legal "hole" that
 * ext4_dir_lookup/ext4_readdir (ext4.c) already skip, and that
 * ext4_dir_insert above already knows how to reuse. */
static int ext4_dir_remove(ext4_mount_t *mnt, ext4_inode_t *dir_inode, const char *name, int name_len)
{
    uint64_t file_size = ext4_inode_size(dir_inode);
    uint32_t num_blocks = (uint32_t)((file_size + mnt->block_size - 1) / mnt->block_size);

    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        uint64_t phys = 0;
        if (ext4_map_block(mnt, dir_inode, lb, &phys) != VFS_OK) return VFS_ERR_IO;
        if (phys == 0) continue;
        if (ext4_read_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;

        uint32_t off = 0;
        while (off + 8 <= mnt->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(mnt->block_buffer + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, (size_t)name_len) == 0) {
                de->inode = 0;
                return ext4_write_block(mnt, phys, mnt->block_buffer);
            }
            off += de->rec_len;
        }
    }
    return VFS_ERR_NOTFOUND;
}

/* ── Path splitting ──────────────────────────────────────────────────────── */

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

/* ── Freeing an inode's data blocks (unlink/truncate) ────────────────────── */

static int ext4_free_inode_blocks(ext4_mount_t *mnt, ext4_inode_t *inode)
{
    if (inode->i_flags & EXT4_EXTENTS_FL) {
        ext4_extent_header_t *h = (ext4_extent_header_t *)inode->i_block;
        if (h->eh_magic != EXT4_EXTENT_MAGIC) return VFS_OK;
        ext4_extent_t *ext = (ext4_extent_t *)((uint8_t *)inode->i_block + sizeof(*h));
        for (uint16_t i = 0; i < h->eh_entries; i++) {
            uint16_t len = ext[i].ee_len;
            if (len > 32768) len -= 32768;
            uint64_t phys = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
            for (uint16_t j = 0; j < len; j++) {
                int r = ext4_free_block(mnt, phys + j);
                if (r != VFS_OK) return r;
            }
        }
    } else {
        for (int i = 0; i < EXT4_NDIR_BLOCKS; i++) {
            if (inode->i_block[i] != 0) {
                int r = ext4_free_block(mnt, inode->i_block[i]);
                if (r != VFS_OK) return r;
                inode->i_block[i] = 0;
            }
        }
        /* Indirect/double/triple-indirect blocks aren't freed here - such
         * inodes were never created by this driver (see header comment). */
    }
    return VFS_OK;
}

/* ── Public entry points (ext4_internal.h) ───────────────────────────────── */

void ext4_write_init_allocators(ext4_mount_t *mnt)
{
    mnt->writable = 0;

    if (mnt->sb.s_feature_ro_compat &
        (EXT4_FEATURE_RO_COMPAT_GDT_CSUM | EXT4_FEATURE_RO_COMPAT_METADATA_CSUM))
        return;
    if (mnt->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG)
        return;

    mnt->writable = 1;
}

int ext4_write_open(ext4_mount_t *mnt, const char *path, int flags, ext4_file_t **out_file)
{
    uint32_t ino;
    ext4_inode_t inode;
    int r = ext4_resolve_path(mnt, path, &ino, &inode);

    if (r == VFS_OK && (inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;

    if (r != VFS_OK) {
        if (!(flags & O_CREAT)) return r;

        char parent_path[VFS_PATH_MAX];
        const char *base; int base_len;
        split_path(path, parent_path, sizeof(parent_path), &base, &base_len);
        if (base_len == 0 || base_len > EXT4_NAME_LEN || parent_path[0] == '\0') return VFS_ERR_INVAL;

        uint32_t parent_ino;
        ext4_inode_t parent_inode;
        if (ext4_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
        if ((parent_inode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

        uint32_t pref_group = (parent_ino - 1) / mnt->inodes_per_group;
        uint32_t new_ino;
        r = ext4_alloc_inode(mnt, pref_group, 0, &new_ino);
        if (r != VFS_OK) return r;

        ext4_inode_t new_inode;
        build_new_inode(&new_inode, EXT4_S_IFREG | EXT4_DEFM_REG, 1, 0);
        r = ext4_write_inode(mnt, new_ino, &new_inode);
        if (r != VFS_OK) return r;

        r = ext4_dir_insert(mnt, parent_ino, &parent_inode, base, base_len, new_ino, EXT4_FT_REG_FILE);
        if (r != VFS_OK) return r;

        ino = new_ino;
        inode = new_inode;
    } else if (flags & O_TRUNC) {
        r = ext4_free_inode_blocks(mnt, &inode);
        if (r != VFS_OK) return r;
        extent_root_init(&inode);
        inode.i_size_lo = 0;
        inode.i_size_high = 0;
        inode.i_blocks_lo = 0;
        r = ext4_write_inode(mnt, ino, &inode);
        if (r != VFS_OK) return r;
    }

    ext4_file_t *f = (ext4_file_t *)mem_alloc(sizeof(ext4_file_t));
    if (!f) return VFS_ERR_NOSPACE;
    f->mnt = mnt;
    f->inode_num = ino;
    f->inode = inode;
    f->pos = (flags & O_APPEND) ? (uint32_t)ext4_inode_size(&inode) : 0;
    f->flags = flags;
    *out_file = f;
    return VFS_OK;
}

int ext4_write_data(ext4_file_t *f, const void *buf, uint32_t len, uint32_t *actual)
{
    *actual = 0;
    if (len == 0) return VFS_OK;

    ext4_mount_t *mnt = f->mnt;
    uint64_t cur_size = ext4_inode_size(&f->inode);
    /* Sequential/append-only - see this file's header comment. */
    if (f->pos != cur_size) return VFS_ERR_INVAL;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = len;
    uint32_t pref_group = (f->inode_num - 1) / mnt->inodes_per_group;

    /* Fill any trailing space left in the current last block by an
     * earlier write() call before allocating anything new. */
    uint32_t tail_off = (uint32_t)(cur_size % mnt->block_size);
    if (tail_off != 0) {
        uint32_t last_lb = (uint32_t)(cur_size / mnt->block_size);
        uint64_t phys = 0;
        if (ext4_map_block(mnt, &f->inode, last_lb, &phys) == VFS_OK && phys != 0) {
            if (ext4_read_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;
            uint32_t space = mnt->block_size - tail_off;
            uint32_t chunk = (space < remaining) ? space : remaining;
            memcpy(mnt->block_buffer + tail_off, src, chunk);
            if (ext4_write_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;
            src += chunk; remaining -= chunk; cur_size += chunk;
        }
    }

    while (remaining > 0) {
        uint32_t next_lb = (uint32_t)(cur_size / mnt->block_size);
        uint64_t phys;
        if (ext4_grow_inode(mnt, &f->inode, next_lb, pref_group, &phys) != VFS_OK) break;

        uint32_t chunk = (remaining < mnt->block_size) ? remaining : mnt->block_size;
        memcpy(mnt->block_buffer, src, chunk);
        if (chunk < mnt->block_size) memset(mnt->block_buffer + chunk, 0, mnt->block_size - chunk);
        if (ext4_write_block(mnt, phys, mnt->block_buffer) != VFS_OK) break;

        src += chunk; remaining -= chunk; cur_size += chunk;
    }

    uint32_t written = len - remaining;
    if (written > 0) {
        f->inode.i_size_lo = (uint32_t)(cur_size & 0xFFFFFFFFu);
        f->inode.i_size_high = (uint32_t)(cur_size >> 32);
        ext4_write_inode(mnt, f->inode_num, &f->inode);
        f->pos += written;
    }
    *actual = written;
    return (written == len) ? VFS_OK : VFS_ERR_IO;
}

int ext4_do_mkdir(ext4_mount_t *mnt, const char *path)
{
    uint32_t existing_ino;
    ext4_inode_t existing;
    if (ext4_resolve_path(mnt, path, &existing_ino, &existing) == VFS_OK) return VFS_ERR_INVAL;

    char parent_path[VFS_PATH_MAX];
    const char *base; int base_len;
    split_path(path, parent_path, sizeof(parent_path), &base, &base_len);
    if (base_len == 0 || base_len > EXT4_NAME_LEN || parent_path[0] == '\0') return VFS_ERR_INVAL;

    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    if (ext4_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
    if ((parent_inode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    uint32_t inode_pref_group = (parent_ino - 1) / mnt->inodes_per_group;
    uint32_t new_ino;
    int r = ext4_alloc_inode(mnt, inode_pref_group, 1, &new_ino);
    if (r != VFS_OK) return r;

    ext4_inode_t new_inode;
    /* Built with size 0, not the final block_size: ext4_grow_inode below
     * derives the next logical block to append from the inode's CURRENT
     * size, so pre-setting it to the final size would make it append at
     * logical block 1 instead of 0, leaving block 0 unmapped. Size is set
     * to its real value (mnt->block_size) once the block is actually
     * allocated. */
    build_new_inode(&new_inode, EXT4_S_IFDIR | EXT4_DEFM_DIR, 2, 0);

    uint32_t data_pref_group = (new_ino - 1) / mnt->inodes_per_group;
    uint64_t phys;
    r = ext4_grow_inode(mnt, &new_inode, 0, data_pref_group, &phys);
    if (r != VFS_OK) { ext4_free_inode(mnt, new_ino, 1); return r; }

    new_inode.i_size_lo = mnt->block_size;
    new_inode.i_size_high = 0;

    init_dir_block(mnt->block_buffer, mnt->block_size, new_ino, parent_ino);
    r = ext4_write_block(mnt, phys, mnt->block_buffer);
    if (r != VFS_OK) return r;

    r = ext4_write_inode(mnt, new_ino, &new_inode);
    if (r != VFS_OK) return r;

    r = ext4_dir_insert(mnt, parent_ino, &parent_inode, base, base_len, new_ino, EXT4_FT_DIR);
    if (r != VFS_OK) return r;

    parent_inode.i_links_count++;
    return ext4_write_inode(mnt, parent_ino, &parent_inode);
}

int ext4_do_unlink(ext4_mount_t *mnt, const char *path)
{
    uint32_t ino;
    ext4_inode_t inode;
    int r = ext4_resolve_path(mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;
    if ((inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return VFS_ERR_ISDIR;

    char parent_path[VFS_PATH_MAX];
    const char *base; int base_len;
    split_path(path, parent_path, sizeof(parent_path), &base, &base_len);

    uint32_t parent_ino;
    ext4_inode_t parent_inode;
    if (ext4_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_IO;

    r = ext4_dir_remove(mnt, &parent_inode, base, base_len);
    if (r != VFS_OK) return r;

    /* Every file this driver creates has links_count == 1 (no hardlink
     * support anywhere in this project) - always free immediately. */
    r = ext4_free_inode_blocks(mnt, &inode);
    if (r != VFS_OK) return r;

    return ext4_free_inode(mnt, ino, 0);
}

int ext4_do_rename(ext4_mount_t *mnt, const char *old_path, const char *new_path)
{
    uint32_t ino;
    ext4_inode_t inode;
    if (ext4_resolve_path(mnt, old_path, &ino, &inode) != VFS_OK) return VFS_ERR_NOTFOUND;

    uint32_t existing_ino;
    ext4_inode_t existing_inode;
    if (ext4_resolve_path(mnt, new_path, &existing_ino, &existing_inode) == VFS_OK) return VFS_ERR_INVAL;

    char oparent[VFS_PATH_MAX];
    const char *obase; int olen;
    split_path(old_path, oparent, sizeof(oparent), &obase, &olen);
    uint32_t oparent_ino;
    ext4_inode_t oparent_inode;
    if (ext4_resolve_path(mnt, oparent, &oparent_ino, &oparent_inode) != VFS_OK) return VFS_ERR_IO;

    char nparent[VFS_PATH_MAX];
    const char *nbase; int nlen;
    split_path(new_path, nparent, sizeof(nparent), &nbase, &nlen);
    if (nlen == 0 || nlen > EXT4_NAME_LEN || nparent[0] == '\0') return VFS_ERR_INVAL;
    uint32_t nparent_ino;
    ext4_inode_t nparent_inode;
    if (ext4_resolve_path(mnt, nparent, &nparent_ino, &nparent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
    if ((nparent_inode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return VFS_ERR_NOTDIR;

    int is_dir = (inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
    uint8_t file_type = is_dir ? EXT4_FT_DIR : EXT4_FT_REG_FILE;

    int r = ext4_dir_remove(mnt, &oparent_inode, obase, olen);
    if (r != VFS_OK) return r;

    r = ext4_dir_insert(mnt, nparent_ino, &nparent_inode, nbase, nlen, ino, file_type);
    if (r != VFS_OK) return r;

    if (is_dir && oparent_ino != nparent_ino) {
        /* Rewrite the moved directory's own ".." entry (always the second
         * entry, right after "."), then fix up both parents' link counts -
         * the ".." that used to point at oparent no longer does, and
         * nparent gains one. */
        uint64_t phys = 0;
        if (ext4_map_block(mnt, &inode, 0, &phys) != VFS_OK || phys == 0) return VFS_ERR_IO;
        if (ext4_read_block(mnt, phys, mnt->extent_buffer) != VFS_OK) return VFS_ERR_IO;

        uint32_t off = EXT4_DIR_REC_LEN(1);
        ext4_dirent_t *dotdot = (ext4_dirent_t *)(mnt->extent_buffer + off);
        if (dotdot->name_len != 2 || dotdot->name[0] != '.' || dotdot->name[1] != '.')
            return VFS_ERR_IO;
        dotdot->inode = nparent_ino;
        if (ext4_write_block(mnt, phys, mnt->extent_buffer) != VFS_OK) return VFS_ERR_IO;

        oparent_inode.i_links_count--;
        if (ext4_write_inode(mnt, oparent_ino, &oparent_inode) != VFS_OK) return VFS_ERR_IO;
        nparent_inode.i_links_count++;
        return ext4_write_inode(mnt, nparent_ino, &nparent_inode);
    }

    return VFS_OK;
}
