/**
 * @file f2fs_write.c
 * @brief f2fs LFS write-side filesystem operations (mkdir/create/write/
 *        truncate/unlink/rename) - the segment/nid allocator and
 *        checkpoint-commit engine that sits alongside f2fs.c's read-only
 *        mount/read/lookup code.
 *
 * IMPORTANT - what this does and does not maintain, same honesty
 * convention as fs/ext4/ext4_write.c and fs/btrfs/btrfs_write.c's own
 * header comments, and see fs/f2fs/f2fs_disk.h's header comment for the
 * on-disk format subset this whole driver is restricted to:
 *
 *   - **One active log per type, not six**: real f2fs keeps separate hot/
 *     warm/cold logs for node and data blocks as a flash placement
 *     heuristic, not a correctness requirement. This driver always
 *     allocates from log slot 0 (cur_node_segno[0]/cur_data_segno[0]).
 *   - **LFS-only, no GC/SSR, no space reclaim ever**: allocation always
 *     appends into the current open segment and rolls to a fresh
 *     all-free segment (found by scanning the SIT) when it fills. Delete
 *     paths (unlink/truncate/rename-over) never free blocks, nodes, or
 *     nids - the SIT/NAT never shrink. Same "never reclaims mid-session"
 *     philosophy as fs/btrfs/btrfs_write.c's bump allocator.
 *   - **Sequential nid allocation only**: new nids come from the
 *     checkpoint's own next_free_nid counter, bumped on every allocation,
 *     never reused.
 *   - **No SIT/NAT journal**: every SIT/NAT change is written straight to
 *     its dedicated block area immediately, never deferred through the
 *     checkpoint's journal cache. Simpler, and means the checkpoint's
 *     sit_nat_version_bitmap is meaningless here - written zeroed, never
 *     read back.
 *   - **One full checkpoint commit per top-level operation** (mkdir/
 *     create+write/truncate/unlink/rename): alternates CP pack 1/2, bumps
 *     checkpoint_ver, writes a 3-block pack (header, zeroed spacer,
 *     duplicate tail - the minimum satisfying cp_pack_total_block_count >
 *     F2FS_CP_PACKS), CRCs both header and tail. The atomic "finish line"
 *     for that operation - same design as btrfs_write.c's per-op
 *     superblock commit.
 *   - **Flat, dotless directories**: no "." or ".." dentries are ever
 *     written (matches real f2fs - see f2fs.c's header comment); no
 *     directory link-count bookkeeping is needed as a result. Lookup/
 *     insert/remove operate on a single hash-bucket level (bucket 0), so
 *     capacity is however many f2fs_dentry_blocks fit in the node levels
 *     below allow (~2959 blocks).
 *   - **Node tree depth 0-1 only** (inline i_addr[] plus the two direct
 *     node blocks NODE_DIR1/DIR2 - see f2fs_disk.h) - VFS_ERR_NOSPACE
 *     past ~11.5MB/file, no single/double-indirect node blocks.
 *   - **No** inline data/dentry, xattr, ACL, compression, encryption,
 *     verity, quota, atomic/volatile writes, or roll-forward recovery.
 *     f2fs_write_init_allocators() below refuses to write to an image
 *     whose last checkpoint wasn't cleanly unmounted (CP_UMOUNT_FLAG) or
 *     has CP_ORPHAN_PRESENT_FLAG set, precisely because there is no
 *     recovery path here to make such an image safe to build on.
 *
 * UNVERIFIED against real f2fs-tools or a real Linux f2fs mount: no
 * mkfs.f2fs/fsck.f2fs is available in this project's development
 * environment (unlike ext4/btrfs, which were cross-checked against real
 * e2fsprogs/mkbtrfs_image.py). Confidence here rests on: on-disk structs
 * transcribed directly from the real Linux kernel header rather than from
 * memory, the checkpoint-pack-selection and dentry-hash algorithms read
 * from the real driver's own source before it was removed from this
 * project, and this project's own host-native round-trip test
 * (fs/f2fs/tests/host_harness.c: format, mount, write, unmount, remount,
 * re-verify) - not an independent format checker.
 */

#include "f2fs_internal.h"
#include "string.h"
#include "blkdev.h"

/* ── Block write ─────────────────────────────────────────────────────────── */

static int f2fs_write_block(f2fs_mount_t *mnt, uint32_t blkaddr, const void *buf)
{
    blkdev_t *dev = (blkdev_t *)mnt->dev;
    uint32_t sectors_per_block = F2FS_BLKSIZE / dev->sector_size;
    if (sectors_per_block == 0) sectors_per_block = 1;
    int64_t sector = (int64_t)((uint64_t)blkaddr * sectors_per_block);
    return (blk_write(dev, sector, sectors_per_block, buf) == 0) ? VFS_OK : VFS_ERR_IO;
}

/* ── SIT (segment validity) ──────────────────────────────────────────────── */

static inline int bit_test(const uint8_t *bitmap, uint32_t bit) { return (bitmap[bit / 8] >> (bit % 8)) & 1; }
static inline void bit_set(uint8_t *bitmap, uint32_t bit) { bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8)); }

#define SIT_VBLOCKS_MASK 0x3FFu

static int read_sit_entry(f2fs_mount_t *mnt, uint32_t segno, struct f2fs_sit_entry *out)
{
    uint32_t block = segno / SIT_ENTRY_PER_BLOCK;
    uint32_t idx = segno % SIT_ENTRY_PER_BLOCK;
    if (f2fs_read_block(mnt, mnt->sb.sit_blkaddr + block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;
    struct f2fs_sit_block *sb = (struct f2fs_sit_block *)mnt->bitmap_buffer;
    memcpy(out, &sb->entries[idx], sizeof(*out));
    return VFS_OK;
}

static int write_sit_entry(f2fs_mount_t *mnt, uint32_t segno, const struct f2fs_sit_entry *in)
{
    uint32_t block = segno / SIT_ENTRY_PER_BLOCK;
    uint32_t idx = segno % SIT_ENTRY_PER_BLOCK;
    if (f2fs_read_block(mnt, mnt->sb.sit_blkaddr + block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;
    struct f2fs_sit_block *sb = (struct f2fs_sit_block *)mnt->bitmap_buffer;
    memcpy(&sb->entries[idx], in, sizeof(*in));
    return f2fs_write_block(mnt, mnt->sb.sit_blkaddr + block, mnt->bitmap_buffer);
}

static int alloc_free_segment(f2fs_mount_t *mnt, uint32_t *out_segno)
{
    for (uint32_t segno = 0; segno < mnt->main_segments; segno++) {
        struct f2fs_sit_entry e;
        if (read_sit_entry(mnt, segno, &e) != VFS_OK) return VFS_ERR_IO;
        if ((e.vblocks & SIT_VBLOCKS_MASK) == 0) {
            *out_segno = segno;
            return VFS_OK;
        }
    }
    return VFS_ERR_NOSPACE;
}

/* ── LFS block allocation (node or data, single active log per type) ─────── */

static int alloc_block(f2fs_mount_t *mnt, int is_node, uint32_t *out_blkaddr)
{
    uint32_t *segno_slot = is_node ? &mnt->ckpt.cur_node_segno[0] : &mnt->ckpt.cur_data_segno[0];
    uint16_t *blkoff_slot = is_node ? &mnt->ckpt.cur_node_blkoff[0] : &mnt->ckpt.cur_data_blkoff[0];

    if (*blkoff_slot >= mnt->blocks_per_seg) {
        uint32_t new_segno;
        if (alloc_free_segment(mnt, &new_segno) != VFS_OK) return VFS_ERR_NOSPACE;
        *segno_slot = new_segno;
        *blkoff_slot = 0;
        if (mnt->ckpt.free_segment_count > 0) mnt->ckpt.free_segment_count--;
    }

    uint32_t segno = *segno_slot;
    uint32_t off = *blkoff_slot;
    uint32_t blkaddr = mnt->sb.main_blkaddr + segno * mnt->blocks_per_seg + off;

    struct f2fs_sit_entry e;
    if (read_sit_entry(mnt, segno, &e) != VFS_OK) return VFS_ERR_IO;
    bit_set(e.valid_map, off);
    uint32_t vblocks = (e.vblocks & SIT_VBLOCKS_MASK) + 1;
    e.vblocks = (uint16_t)vblocks;
    if (write_sit_entry(mnt, segno, &e) != VFS_OK) return VFS_ERR_IO;

    *blkoff_slot = (uint16_t)(off + 1);
    mnt->ckpt.valid_block_count++;
    mnt->ckpt.user_block_count++;

    *out_blkaddr = blkaddr;
    return VFS_OK;
}

/* ── NAT (nid allocation + entry write) ──────────────────────────────────── */

static int alloc_nid(f2fs_mount_t *mnt, uint32_t *out_nid)
{
    *out_nid = mnt->ckpt.next_free_nid;
    mnt->ckpt.next_free_nid++;
    return VFS_OK;
}

static int write_nat_entry(f2fs_mount_t *mnt, uint32_t nid, uint32_t ino, uint32_t block_addr)
{
    uint32_t block = nid / NAT_ENTRY_PER_BLOCK;
    uint32_t idx = nid % NAT_ENTRY_PER_BLOCK;
    if (f2fs_read_block(mnt, mnt->sb.nat_blkaddr + block, mnt->bitmap_buffer) != VFS_OK) return VFS_ERR_IO;
    struct f2fs_nat_block *nb = (struct f2fs_nat_block *)mnt->bitmap_buffer;
    nb->entries[idx].version = 0;
    nb->entries[idx].ino = ino;
    nb->entries[idx].block_addr = block_addr;
    return f2fs_write_block(mnt, mnt->sb.nat_blkaddr + block, mnt->bitmap_buffer);
}

/* Writes node (a full block: node->i / .dn / .in already filled in by the
 * caller, node->footer.nid/.ino already set) to a freshly LFS-allocated
 * block and updates its NAT entry to point there. */
static int f2fs_write_node(f2fs_mount_t *mnt, struct f2fs_node *node)
{
    uint32_t blkaddr;
    if (alloc_block(mnt, /*is_node=*/1, &blkaddr) != VFS_OK) return VFS_ERR_NOSPACE;

    node->footer.cp_ver = mnt->ckpt.checkpoint_ver + 1;
    node->footer.next_blkaddr = 0;

    if (f2fs_write_block(mnt, blkaddr, node) != VFS_OK) return VFS_ERR_IO;
    if (write_nat_entry(mnt, node->footer.nid, node->footer.ino, blkaddr) != VFS_OK) return VFS_ERR_IO;

    mnt->ckpt.valid_node_count++;
    return VFS_OK;
}

static int f2fs_write_inode(f2fs_mount_t *mnt, uint32_t ino, struct f2fs_inode *inode)
{
    struct f2fs_node node;
    memset(&node, 0, sizeof(node));
    memcpy(&node.i, inode, sizeof(*inode));
    node.footer.nid = ino;
    node.footer.ino = ino;
    return f2fs_write_node(mnt, &node);
}

/* Allocates one data block and links it into inode's node tree at logical
 * position next_lb (levels 0-1 only - see this file's header comment).
 * Persists any newly-created direct-node block immediately (its own
 * f2fs_write_node call); the inode itself is NOT persisted here - callers
 * finish updating i_size/i_blocks and call f2fs_write_inode() once. */
static int f2fs_grow_inode(f2fs_mount_t *mnt, uint32_t ino, struct f2fs_inode *inode,
                            uint32_t next_lb, uint32_t *out_phys)
{
    uint32_t phys;
    if (alloc_block(mnt, /*is_node=*/0, &phys) != VFS_OK) return VFS_ERR_NOSPACE;

    if (next_lb < DEF_ADDRS_PER_INODE) {
        inode->i_addr[next_lb] = phys;
    } else {
        uint32_t rel = next_lb - DEF_ADDRS_PER_INODE;
        int slot;
        uint32_t off;
        if (rel < DEF_ADDRS_PER_BLOCK) { slot = 0; off = rel; }
        else if (rel - DEF_ADDRS_PER_BLOCK < DEF_ADDRS_PER_BLOCK) { slot = 1; off = rel - DEF_ADDRS_PER_BLOCK; }
        else return VFS_ERR_NOSPACE;

        struct f2fs_node dnode;
        uint32_t nid = inode->i_nid[slot];
        if (nid == 0) {
            if (alloc_nid(mnt, &nid) != VFS_OK) return VFS_ERR_NOSPACE;
            memset(&dnode, 0, sizeof(dnode));
            inode->i_nid[slot] = nid;
            inode->i_blocks++;
        } else {
            if (f2fs_read_node(mnt, nid, &dnode) != VFS_OK) return VFS_ERR_IO;
        }
        dnode.footer.nid = nid;
        dnode.footer.ino = ino;
        dnode.dn.addr[off] = phys;
        if (f2fs_write_node(mnt, &dnode) != VFS_OK) return VFS_ERR_IO;
    }

    inode->i_blocks++;
    *out_phys = phys;
    return VFS_OK;
}

/* ── Dentry hash (TEA, verbatim algorithm from the real driver's
 * fs/f2fs/hash.c, itself copied from ext3) - stored for format fidelity;
 * this driver's own lookup/insert never consults it (linear name-compare
 * scan, see f2fs_dir_lookup() in f2fs.c). ─────────────────────────────── */

#define TEA_DELTA 0x9E3779B9u

static void tea_transform(uint32_t buf[4], const uint32_t in[4])
{
    uint32_t sum = 0;
    uint32_t b0 = buf[0], b1 = buf[1];
    uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
    int n = 16;
    do {
        sum += TEA_DELTA;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    } while (--n);
    buf[0] += b0;
    buf[1] += b1;
}

static void str2hashbuf(const uint8_t *msg, int len, uint32_t *buf, int num)
{
    uint32_t pad = (uint32_t)len | ((uint32_t)len << 8);
    pad |= pad << 16;
    uint32_t val = pad;

    if (len > num * 4) len = num * 4;
    int i;
    for (i = 0; i < len; i++) {
        if ((i % 4) == 0) val = pad;
        val = msg[i] + (val << 8);
        if ((i % 4) == 3) { *buf++ = val; val = pad; num--; }
    }
    if (--num >= 0) *buf++ = val;
    while (--num >= 0) *buf++ = pad;
}

static f2fs_hash_t f2fs_dentry_hash(const char *name, int len)
{
    uint32_t in[4], buf[4];
    buf[0] = 0x67452301u; buf[1] = 0xefcdab89u; buf[2] = 0x98badcfeu; buf[3] = 0x10325476u;

    const uint8_t *p = (const uint8_t *)name;
    int l = len;
    for (;;) {
        str2hashbuf(p, l, in, 4);
        tea_transform(buf, in);
        p += 16;
        if (l <= 16) break;
        l -= 16;
    }
    return buf[0];
}

/* ── Directory entries (flat, single hash-bucket level) ──────────────────── */

#define GET_DENTRY_SLOTS(name_len) (((name_len) + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN)

static void put_dentry(struct f2fs_dentry_block *db, uint32_t start, uint32_t slots,
                        uint32_t ino, const char *name, int name_len, uint8_t file_type, f2fs_hash_t hash)
{
    for (uint32_t i = 0; i < slots; i++) bit_set(db->dentry_bitmap, start + i);

    db->dentry[start].hash_code = hash;
    db->dentry[start].ino = ino;
    db->dentry[start].name_len = (uint16_t)name_len;
    db->dentry[start].file_type = file_type;

    for (uint32_t i = 0; i < slots; i++) {
        uint32_t off = i * F2FS_SLOT_LEN;
        int n = name_len - (int)off;
        if (n > F2FS_SLOT_LEN) n = F2FS_SLOT_LEN;
        if (n > 0) memcpy(db->filename[start + i], name + off, (size_t)n);
        if (n < F2FS_SLOT_LEN) memset(db->filename[start + i] + (n > 0 ? n : 0), 0, (size_t)(F2FS_SLOT_LEN - (n > 0 ? n : 0)));
    }
}

static int f2fs_dir_insert(f2fs_mount_t *mnt, uint32_t dir_ino, struct f2fs_inode *dir_inode,
                            const char *name, int name_len, uint32_t child_ino, uint8_t file_type)
{
    f2fs_hash_t hash = f2fs_dentry_hash(name, name_len);
    uint32_t slots_needed = GET_DENTRY_SLOTS(name_len);
    uint64_t file_size = f2fs_inode_size(dir_inode);
    uint32_t num_blocks = (uint32_t)((file_size + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE);

    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        uint32_t phys = 0;
        if (f2fs_map_block(mnt, dir_inode, lb, &phys) != VFS_OK) return VFS_ERR_IO;
        if (phys == 0) continue;
        if (f2fs_read_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;
        struct f2fs_dentry_block *db = (struct f2fs_dentry_block *)mnt->block_buffer;

        uint32_t run = 0, start = 0;
        for (uint32_t bit = 0; bit < NR_DENTRY_IN_BLOCK; bit++) {
            if (!bit_test(db->dentry_bitmap, bit)) {
                if (run == 0) start = bit;
                run++;
                if (run == slots_needed) {
                    put_dentry(db, start, slots_needed, child_ino, name, name_len, file_type, hash);
                    return f2fs_write_block(mnt, phys, mnt->block_buffer);
                }
            } else {
                run = 0;
            }
        }
    }

    /* No room anywhere - append a fresh, all-free dentry block. name_len
     * <= F2FS_NAME_LEN(255) => slots_needed <= 32, always <= NR_DENTRY_IN_BLOCK(214)...
     * actually 255/8 rounds to 32 slots, well within 214, so this always fits. */
    uint32_t phys;
    if (f2fs_grow_inode(mnt, dir_ino, dir_inode, num_blocks, &phys) != VFS_OK) return VFS_ERR_NOSPACE;

    memset(mnt->block_buffer, 0, F2FS_BLKSIZE);
    struct f2fs_dentry_block *db = (struct f2fs_dentry_block *)mnt->block_buffer;
    put_dentry(db, 0, slots_needed, child_ino, name, name_len, file_type, hash);
    if (f2fs_write_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;

    dir_inode->i_size = file_size + F2FS_BLKSIZE;
    return f2fs_write_inode(mnt, dir_ino, dir_inode);
}

static int f2fs_dir_remove(f2fs_mount_t *mnt, struct f2fs_inode *dir_inode, const char *name, int name_len)
{
    uint64_t file_size = f2fs_inode_size(dir_inode);
    uint32_t num_blocks = (uint32_t)((file_size + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE);

    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        uint32_t phys = 0;
        if (f2fs_map_block(mnt, dir_inode, lb, &phys) != VFS_OK) return VFS_ERR_IO;
        if (phys == 0) continue;
        if (f2fs_read_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;
        struct f2fs_dentry_block *db = (struct f2fs_dentry_block *)mnt->block_buffer;

        uint32_t bit = 0;
        while (bit < NR_DENTRY_IN_BLOCK) {
            if (!bit_test(db->dentry_bitmap, bit)) { bit++; continue; }
            struct f2fs_dir_entry *de = &db->dentry[bit];
            uint32_t slots = GET_DENTRY_SLOTS(de->name_len);
            if (de->name_len == (uint16_t)name_len &&
                memcmp(db->filename[bit], name, (size_t)name_len) == 0) {
                for (uint32_t i = 0; i < slots; i++)
                    db->dentry_bitmap[(bit + i) / 8] &= (uint8_t)~(1u << ((bit + i) % 8));
                return f2fs_write_block(mnt, phys, mnt->block_buffer);
            }
            bit += (slots > 0) ? slots : 1;
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

/* ── Checkpoint commit ────────────────────────────────────────────────────── */

static int f2fs_commit_checkpoint(f2fs_mount_t *mnt)
{
    uint8_t buf[F2FS_BLKSIZE];
    memset(buf, 0, sizeof(buf));
    struct f2fs_checkpoint *cp = (struct f2fs_checkpoint *)buf;
    memcpy(cp, &mnt->ckpt, sizeof(mnt->ckpt));

    cp->checkpoint_ver = mnt->ckpt.checkpoint_ver + 1;
    cp->ckpt_flags = CP_UMOUNT_FLAG;
    cp->cp_pack_total_block_count = 3;   /* header, spacer, duplicate tail - see header comment */
    cp->cp_pack_start_sum = 0;           /* SSA unused - see header comment */
    cp->checksum_offset = CP_CHKSUM_OFFSET;

    uint32_t crc = f2fs_crc32(F2FS_SUPER_MAGIC, buf, CP_CHKSUM_OFFSET);
    memcpy(buf + CP_CHKSUM_OFFSET, &crc, sizeof(crc));

    int new_pack = (mnt->cur_cp_pack == 1) ? 2 : 1;
    uint32_t pack_start = mnt->sb.cp_blkaddr + ((new_pack == 2) ? mnt->blocks_per_seg : 0);

    if (f2fs_write_block(mnt, pack_start, buf) != VFS_OK) return VFS_ERR_IO;

    uint8_t spacer[F2FS_BLKSIZE];
    memset(spacer, 0, sizeof(spacer));
    if (f2fs_write_block(mnt, pack_start + 1, spacer) != VFS_OK) return VFS_ERR_IO;

    if (f2fs_write_block(mnt, pack_start + 2, buf) != VFS_OK) return VFS_ERR_IO;

    mnt->ckpt.checkpoint_ver = cp->checkpoint_ver;
    mnt->ckpt.ckpt_flags = cp->ckpt_flags;
    mnt->cur_cp_pack = new_pack;
    return VFS_OK;
}

/* ── Public entry points (f2fs_internal.h) ───────────────────────────────── */

void f2fs_write_init_allocators(f2fs_mount_t *mnt)
{
    mnt->writable = 0;
    if (!(mnt->ckpt.ckpt_flags & CP_UMOUNT_FLAG)) return;
    if (mnt->ckpt.ckpt_flags & CP_ORPHAN_PRESENT_FLAG) return;
    mnt->writable = 1;
}

int f2fs_write_open(f2fs_mount_t *mnt, const char *path, int flags, f2fs_file_t **out_file)
{
    uint32_t ino;
    struct f2fs_inode inode;
    int r = f2fs_resolve_path(mnt, path, &ino, &inode);

    if (r == VFS_OK && (inode.i_mode & F2FS_S_IFMT) == F2FS_S_IFDIR) return VFS_ERR_ISDIR;

    if (r != VFS_OK) {
        if (!(flags & O_CREAT)) return r;

        char parent_path[VFS_PATH_MAX];
        const char *base; int base_len;
        split_path(path, parent_path, sizeof(parent_path), &base, &base_len);
        if (base_len == 0 || base_len > F2FS_NAME_LEN || parent_path[0] == '\0') return VFS_ERR_INVAL;

        uint32_t parent_ino;
        struct f2fs_inode parent_inode;
        if (f2fs_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
        if ((parent_inode.i_mode & F2FS_S_IFMT) != F2FS_S_IFDIR) return VFS_ERR_NOTDIR;

        uint32_t new_ino;
        if (alloc_nid(mnt, &new_ino) != VFS_OK) return VFS_ERR_NOSPACE;

        struct f2fs_inode new_inode;
        memset(&new_inode, 0, sizeof(new_inode));
        new_inode.i_mode = F2FS_DEF_REG;
        new_inode.i_links = 1;
        new_inode.i_size = 0;
        new_inode.i_blocks = 1;
        new_inode.i_pino = parent_ino;
        new_inode.i_namelen = (uint32_t)base_len;
        memcpy(new_inode.i_name, base, (size_t)base_len);

        r = f2fs_write_inode(mnt, new_ino, &new_inode);
        if (r != VFS_OK) return r;

        r = f2fs_dir_insert(mnt, parent_ino, &parent_inode, base, base_len, new_ino, F2FS_FT_REG_FILE);
        if (r != VFS_OK) return r;

        r = f2fs_commit_checkpoint(mnt);
        if (r != VFS_OK) return r;

        ino = new_ino;
        inode = new_inode;
    } else if (flags & O_TRUNC) {
        memset(inode.i_addr, 0, sizeof(inode.i_addr));
        memset(inode.i_nid, 0, sizeof(inode.i_nid));
        inode.i_size = 0;
        inode.i_blocks = 1;
        r = f2fs_write_inode(mnt, ino, &inode);
        if (r != VFS_OK) return r;
        r = f2fs_commit_checkpoint(mnt);
        if (r != VFS_OK) return r;
    }

    f2fs_file_t *f = (f2fs_file_t *)mem_alloc(sizeof(f2fs_file_t));
    if (!f) return VFS_ERR_NOSPACE;
    f->mnt = mnt;
    f->ino = ino;
    f->inode = inode;
    f->pos = (flags & O_APPEND) ? (uint32_t)f2fs_inode_size(&inode) : 0;
    f->flags = flags;
    *out_file = f;
    return VFS_OK;
}

int f2fs_write_data(f2fs_file_t *f, const void *buf, uint32_t len, uint32_t *actual)
{
    *actual = 0;
    if (len == 0) return VFS_OK;

    f2fs_mount_t *mnt = f->mnt;
    uint64_t cur_size = f2fs_inode_size(&f->inode);
    /* Sequential/append-only - see this file's header comment. */
    if (f->pos != cur_size) return VFS_ERR_INVAL;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t remaining = len;

    uint32_t tail_off = (uint32_t)(cur_size % F2FS_BLKSIZE);
    if (tail_off != 0) {
        uint32_t last_lb = (uint32_t)(cur_size / F2FS_BLKSIZE);
        uint32_t phys = 0;
        if (f2fs_map_block(mnt, &f->inode, last_lb, &phys) == VFS_OK && phys != 0) {
            if (f2fs_read_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;
            uint32_t space = F2FS_BLKSIZE - tail_off;
            uint32_t chunk = (space < remaining) ? space : remaining;
            memcpy(mnt->block_buffer + tail_off, src, chunk);
            if (f2fs_write_block(mnt, phys, mnt->block_buffer) != VFS_OK) return VFS_ERR_IO;
            src += chunk; remaining -= chunk; cur_size += chunk;
        }
    }

    while (remaining > 0) {
        uint32_t next_lb = (uint32_t)(cur_size / F2FS_BLKSIZE);
        uint32_t phys;
        if (f2fs_grow_inode(mnt, f->ino, &f->inode, next_lb, &phys) != VFS_OK) break;

        uint32_t chunk = (remaining < F2FS_BLKSIZE) ? remaining : F2FS_BLKSIZE;
        memcpy(mnt->block_buffer, src, chunk);
        if (chunk < F2FS_BLKSIZE) memset(mnt->block_buffer + chunk, 0, F2FS_BLKSIZE - chunk);
        if (f2fs_write_block(mnt, phys, mnt->block_buffer) != VFS_OK) break;

        src += chunk; remaining -= chunk; cur_size += chunk;
    }

    uint32_t written = len - remaining;
    if (written > 0) {
        f->inode.i_size = cur_size;
        f2fs_write_inode(mnt, f->ino, &f->inode);
        f2fs_commit_checkpoint(mnt);
        f->pos += written;
    }
    *actual = written;
    return (written == len) ? VFS_OK : VFS_ERR_IO;
}

int f2fs_do_mkdir(f2fs_mount_t *mnt, const char *path)
{
    uint32_t existing_ino;
    struct f2fs_inode existing;
    if (f2fs_resolve_path(mnt, path, &existing_ino, &existing) == VFS_OK) return VFS_ERR_INVAL;

    char parent_path[VFS_PATH_MAX];
    const char *base; int base_len;
    split_path(path, parent_path, sizeof(parent_path), &base, &base_len);
    if (base_len == 0 || base_len > F2FS_NAME_LEN || parent_path[0] == '\0') return VFS_ERR_INVAL;

    uint32_t parent_ino;
    struct f2fs_inode parent_inode;
    if (f2fs_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
    if ((parent_inode.i_mode & F2FS_S_IFMT) != F2FS_S_IFDIR) return VFS_ERR_NOTDIR;

    uint32_t new_ino;
    if (alloc_nid(mnt, &new_ino) != VFS_OK) return VFS_ERR_NOSPACE;

    struct f2fs_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.i_mode = F2FS_DEF_DIR;
    new_inode.i_links = 2;
    new_inode.i_size = 0;
    new_inode.i_blocks = 1;
    new_inode.i_current_depth = 1;
    new_inode.i_pino = parent_ino;
    new_inode.i_namelen = (uint32_t)base_len;
    memcpy(new_inode.i_name, base, (size_t)base_len);

    int r = f2fs_write_inode(mnt, new_ino, &new_inode);
    if (r != VFS_OK) return r;

    r = f2fs_dir_insert(mnt, parent_ino, &parent_inode, base, base_len, new_ino, F2FS_FT_DIR);
    if (r != VFS_OK) return r;

    return f2fs_commit_checkpoint(mnt);
}

int f2fs_do_unlink(f2fs_mount_t *mnt, const char *path)
{
    uint32_t ino;
    struct f2fs_inode inode;
    int r = f2fs_resolve_path(mnt, path, &ino, &inode);
    if (r != VFS_OK) return r;
    if ((inode.i_mode & F2FS_S_IFMT) == F2FS_S_IFDIR) return VFS_ERR_ISDIR;

    char parent_path[VFS_PATH_MAX];
    const char *base; int base_len;
    split_path(path, parent_path, sizeof(parent_path), &base, &base_len);

    uint32_t parent_ino;
    struct f2fs_inode parent_inode;
    if (f2fs_resolve_path(mnt, parent_path, &parent_ino, &parent_inode) != VFS_OK) return VFS_ERR_IO;

    r = f2fs_dir_remove(mnt, &parent_inode, base, base_len);
    if (r != VFS_OK) return r;

    /* Blocks/nid are not freed - see this file's header comment. */
    return f2fs_commit_checkpoint(mnt);
}

int f2fs_do_rename(f2fs_mount_t *mnt, const char *old_path, const char *new_path)
{
    uint32_t ino;
    struct f2fs_inode inode;
    if (f2fs_resolve_path(mnt, old_path, &ino, &inode) != VFS_OK) return VFS_ERR_NOTFOUND;

    uint32_t existing_ino;
    struct f2fs_inode existing_inode;
    if (f2fs_resolve_path(mnt, new_path, &existing_ino, &existing_inode) == VFS_OK) return VFS_ERR_INVAL;

    char oparent[VFS_PATH_MAX];
    const char *obase; int olen;
    split_path(old_path, oparent, sizeof(oparent), &obase, &olen);
    uint32_t oparent_ino;
    struct f2fs_inode oparent_inode;
    if (f2fs_resolve_path(mnt, oparent, &oparent_ino, &oparent_inode) != VFS_OK) return VFS_ERR_IO;

    char nparent[VFS_PATH_MAX];
    const char *nbase; int nlen;
    split_path(new_path, nparent, sizeof(nparent), &nbase, &nlen);
    if (nlen == 0 || nlen > F2FS_NAME_LEN || nparent[0] == '\0') return VFS_ERR_INVAL;
    uint32_t nparent_ino;
    struct f2fs_inode nparent_inode;
    if (f2fs_resolve_path(mnt, nparent, &nparent_ino, &nparent_inode) != VFS_OK) return VFS_ERR_NOTFOUND;
    if ((nparent_inode.i_mode & F2FS_S_IFMT) != F2FS_S_IFDIR) return VFS_ERR_NOTDIR;

    uint8_t file_type = ((inode.i_mode & F2FS_S_IFMT) == F2FS_S_IFDIR) ? F2FS_FT_DIR : F2FS_FT_REG_FILE;

    int r = f2fs_dir_remove(mnt, &oparent_inode, obase, olen);
    if (r != VFS_OK) return r;

    r = f2fs_dir_insert(mnt, nparent_ino, &nparent_inode, nbase, nlen, ino, file_type);
    if (r != VFS_OK) return r;

    /* No ".." to rewrite (f2fs directories don't store one - see f2fs.c's
     * header comment); keep i_pino consistent for whatever does read it. */
    if (inode.i_pino != nparent_ino) {
        inode.i_pino = nparent_ino;
        r = f2fs_write_inode(mnt, ino, &inode);
        if (r != VFS_OK) return r;
    }

    return f2fs_commit_checkpoint(mnt);
}
