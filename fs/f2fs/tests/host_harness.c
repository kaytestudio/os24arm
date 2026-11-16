/**
 * @file host_harness.c
 * @brief Host-native (macOS/Linux clang, NOT the ARM64 cross compiler)
 *        exerciser for the real fs/f2fs/f2fs.c + f2fs_write.c driver code.
 *
 * Unlike fs/ext4/tests/host_harness.c, there is no mkfs.f2fs available to
 * build a starting image (see fs/f2fs/f2fs_write.c's header comment: not
 * on Homebrew, no Linux VM in this sandbox), so this harness also
 * implements its OWN independent formatter (format_f2fs_image, below) -
 * writing raw superblock/checkpoint/SIT/NAT/root-inode bytes directly via
 * pwrite(), sharing only the on-disk struct definitions (f2fs_disk.h) and
 * f2fs_crc32() with the real driver, never its write-engine internals.
 * A fresh image the driver can then successfully MOUNT and BUILD ON is a
 * real (if self-referential) cross-check: two separately-written pieces
 * of code agreeing on the format catches bugs a single implementation
 * checking itself cannot.
 *
 * Not part of the kernel build (see scripts/test_f2fs_driver.sh, which
 * compiles this ad hoc). Calls the driver's vfs_fs_t ops directly
 * (mount/open/read/write/stat/opendir/readdir/mkdir/unlink/rename/close),
 * bypassing kernel/src/vfs.c's fd table.
 *
 * Usage: host_harness <image-path>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

/* Captured before f2fs_internal.h (transitively vfs.h) redefines O_RDWR/
 * O_CREAT to this project's own small bit values (e.g. O_CREAT=0x0040) -
 * using the raw macro tokens for the HOST open() call below, after that
 * include, would silently open() with the wrong flags. */
static const int HOST_O_RDWR_CREAT = O_RDWR | O_CREAT;

#include "f2fs_internal.h"
#include "blkdev.h"

/* ── mem_alloc/mem_free/kprintf/terminal_writestring - host stdio/malloc ─── */

void *mem_alloc(size_t size) { return malloc(size); }
void mem_free(void *ptr) { free(ptr); }

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
void terminal_writestring(const char *s) { fputs(s, stdout); }
void uart_puts(const char *s) { fputs(s, stdout); }

/* Pulled in by f2fs_init() for symbol resolution only - this harness
 * calls the vfs_fs_t ops directly and never calls f2fs_init() itself. */
blkdev_t *blkdev_get_by_name(const char *name) { (void)name; return NULL; }
int vfs_mount(const char *mountpoint, vfs_fs_t *fs, void *fs_priv)
{
    (void)mountpoint; (void)fs; (void)fs_priv;
    return VFS_ERR_INVAL;
}

/* ── blkdev_t backed by pread/pwrite on the raw image file ───────────────── */

static int host_blk_read(blkdev_t *dev, int64_t sector, uint32_t count, void *buf)
{
    int fd = (int)(intptr_t)dev->driver_data;
    off_t off = (off_t)sector * dev->sector_size;
    size_t len = (size_t)count * dev->sector_size;
    ssize_t n = pread(fd, buf, len, off);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int host_blk_write(blkdev_t *dev, int64_t sector, uint32_t count, const void *buf)
{
    int fd = (int)(intptr_t)dev->driver_data;
    off_t off = (off_t)sector * dev->sector_size;
    size_t len = (size_t)count * dev->sector_size;
    ssize_t n = pwrite(fd, buf, len, off);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* ── Layout (see this file's header comment / fs/f2fs/f2fs_disk.h) ───────── */

#define FMT_SEG_CKPT   2
#define FMT_SEG_SIT    1
#define FMT_SEG_NAT    1
#define FMT_SEG_SSA    1
#define FMT_SEG_MAIN   16
#define FMT_BLOCKS_PER_SEG 512u
#define FMT_ROOT_INO   3u

static int pwrite_block(int fd, uint32_t blkaddr, const void *buf)
{
    off_t off = (off_t)blkaddr * F2FS_BLKSIZE;
    ssize_t n = pwrite(fd, buf, F2FS_BLKSIZE, off);
    return (n == (ssize_t)F2FS_BLKSIZE) ? 0 : -1;
}

/* Independent formatter - see this file's header comment. Builds a
 * minimal, cleanly-"unmounted" (CP_UMOUNT_FLAG) f2fs image: superblock
 * (both copies), one valid checkpoint pack with root's inode already
 * placed, a SIT with segment 0 marked (1 valid block), and a NAT entry
 * for the root inode. */
static int format_f2fs_image(int fd)
{
    uint32_t segment_count = FMT_SEG_CKPT + FMT_SEG_SIT + FMT_SEG_NAT + FMT_SEG_SSA + FMT_SEG_MAIN;
    uint32_t total_blocks = segment_count * FMT_BLOCKS_PER_SEG;

    if (ftruncate(fd, (off_t)total_blocks * F2FS_BLKSIZE) != 0) return -1;

    uint8_t zero[F2FS_BLKSIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t b = 0; b < total_blocks; b++)
        if (pwrite_block(fd, b, zero) != 0) return -1;

    uint32_t cp_blkaddr   = 2;                                   /* within segment 0, after the 2 SB blocks */
    uint32_t sit_blkaddr  = FMT_SEG_CKPT * FMT_BLOCKS_PER_SEG;
    uint32_t nat_blkaddr  = sit_blkaddr + FMT_SEG_SIT * FMT_BLOCKS_PER_SEG;
    uint32_t ssa_blkaddr  = nat_blkaddr + FMT_SEG_NAT * FMT_BLOCKS_PER_SEG;
    uint32_t main_blkaddr = ssa_blkaddr + FMT_SEG_SSA * FMT_BLOCKS_PER_SEG;

    /* ── Superblock (two identical copies, blocks 0 and 1) ──────────────── */
    uint8_t sb_raw[F2FS_BLKSIZE];
    memset(sb_raw, 0, sizeof(sb_raw));
    /* The struct itself starts at byte offset F2FS_SUPER_OFFSET (1024)
     * within the block, not at the block's own start - matches
     * f2fs_read_superblock()'s sector-based read (sector 2 = byte 1024),
     * same convention ext4 uses for its own superblock. Bytes [0,1024)
     * stay zeroed padding. */
    struct f2fs_super_block *sb = (struct f2fs_super_block *)(sb_raw + F2FS_SUPER_OFFSET);
    sb->magic = F2FS_SUPER_MAGIC;
    sb->major_ver = 1; sb->minor_ver = 15;
    sb->log_sectorsize = 9;
    sb->log_sectors_per_block = 3;
    sb->log_blocksize = F2FS_BLKSIZE_BITS;
    sb->log_blocks_per_seg = 9;
    sb->segs_per_sec = 1;
    sb->secs_per_zone = 1;
    sb->checksum_offset = (uint32_t)((uint8_t *)&sb->crc - (uint8_t *)sb);
    sb->block_count = FMT_SEG_MAIN * FMT_BLOCKS_PER_SEG;
    sb->section_count = segment_count;
    sb->segment_count = segment_count;
    sb->segment_count_ckpt = FMT_SEG_CKPT;
    sb->segment_count_sit = FMT_SEG_SIT;
    sb->segment_count_nat = FMT_SEG_NAT;
    sb->segment_count_ssa = FMT_SEG_SSA;
    sb->segment_count_main = FMT_SEG_MAIN;
    sb->segment0_blkaddr = 0;
    sb->cp_blkaddr = cp_blkaddr;
    sb->sit_blkaddr = sit_blkaddr;
    sb->nat_blkaddr = nat_blkaddr;
    sb->ssa_blkaddr = ssa_blkaddr;
    sb->main_blkaddr = main_blkaddr;
    sb->root_ino = FMT_ROOT_INO;
    sb->node_ino = 1;
    sb->meta_ino = 2;
    sb->cp_payload = 0;

    sb->crc = f2fs_crc32(F2FS_SUPER_MAGIC, sb, sb->checksum_offset);

    if (pwrite_block(fd, 0, sb_raw) != 0) return -1;
    if (pwrite_block(fd, 1, sb_raw) != 0) return -1;

    /* ── Root inode node block ───────────────────────────────────────────
     * Placed as the first block of main segment 0 (node log). */
    uint32_t root_blkaddr = main_blkaddr + 0;
    uint8_t root_raw[F2FS_BLKSIZE];
    memset(root_raw, 0, sizeof(root_raw));
    struct f2fs_node *root_node = (struct f2fs_node *)root_raw;
    root_node->i.i_mode = F2FS_DEF_DIR;
    root_node->i.i_links = 2;
    root_node->i.i_size = 0;
    root_node->i.i_blocks = 1;
    root_node->i.i_current_depth = 1;
    root_node->i.i_pino = FMT_ROOT_INO;
    root_node->footer.nid = FMT_ROOT_INO;
    root_node->footer.ino = FMT_ROOT_INO;
    root_node->footer.cp_ver = 1;
    if (pwrite_block(fd, root_blkaddr, root_raw) != 0) return -1;

    /* ── SIT: segment 0 has 1 valid block (root's inode) ─────────────────── */
    uint8_t sit_raw[F2FS_BLKSIZE];
    memset(sit_raw, 0, sizeof(sit_raw));
    struct f2fs_sit_block *sit_blk = (struct f2fs_sit_block *)sit_raw;
    sit_blk->entries[0].vblocks = 1;
    sit_blk->entries[0].valid_map[0] = 0x01;
    if (pwrite_block(fd, sit_blkaddr, sit_raw) != 0) return -1;

    /* ── SSA: segment 0's summary - the root inode (a node) in its first block ── */
    {
        uint8_t sum_raw[F2FS_BLKSIZE];
        memset(sum_raw, 0, sizeof(sum_raw));
        struct f2fs_summary_block *sumb = (struct f2fs_summary_block *)sum_raw;
        sumb->entries[0].nid = FMT_ROOT_INO;
        sumb->footer.entry_type = SUM_TYPE_NODE;
        if (pwrite_block(fd, ssa_blkaddr, sum_raw) != 0) return -1;
    }

    /* ── NAT: root_ino -> root_blkaddr ───────────────────────────────────── */
    uint8_t nat_raw[F2FS_BLKSIZE];
    memset(nat_raw, 0, sizeof(nat_raw));
    struct f2fs_nat_block *nat_blk = (struct f2fs_nat_block *)nat_raw;
    nat_blk->entries[FMT_ROOT_INO].ino = FMT_ROOT_INO;
    nat_blk->entries[FMT_ROOT_INO].block_addr = root_blkaddr;
    if (pwrite_block(fd, nat_blkaddr, nat_raw) != 0) return -1;

    /* ── Checkpoint pack 1 (header, spacer, duplicate tail) ──────────────── */
    uint8_t cp_raw[F2FS_BLKSIZE];
    memset(cp_raw, 0, sizeof(cp_raw));
    struct f2fs_checkpoint *cp = (struct f2fs_checkpoint *)cp_raw;
    cp->checkpoint_ver = 1;
    cp->user_block_count = 1;
    cp->valid_block_count = 1;
    cp->free_segment_count = FMT_SEG_MAIN - 1;
    cp->cur_node_segno[0] = 0;
    cp->cur_node_blkoff[0] = 1;
    cp->cur_data_segno[0] = 1;
    cp->cur_data_blkoff[0] = 0;
    cp->ckpt_flags = CP_UMOUNT_FLAG;
    cp->cp_pack_total_block_count = 3;
    cp->cp_pack_start_sum = 0;
    cp->valid_node_count = 1;
    cp->valid_inode_count = 1;
    cp->next_free_nid = FMT_ROOT_INO + 1;
    cp->sit_ver_bitmap_bytesize = 0;
    cp->nat_ver_bitmap_bytesize = 0;
    cp->checksum_offset = CP_CHKSUM_OFFSET;

    uint32_t crc = f2fs_crc32(F2FS_SUPER_MAGIC, cp_raw, CP_CHKSUM_OFFSET);
    memcpy(cp_raw + CP_CHKSUM_OFFSET, &crc, sizeof(crc));

    if (pwrite_block(fd, cp_blkaddr, cp_raw) != 0) return -1;
    if (pwrite_block(fd, cp_blkaddr + 1, zero) != 0) return -1;
    if (pwrite_block(fd, cp_blkaddr + 2, cp_raw) != 0) return -1;

    return 0;
}

/* ── Test scaffolding ─────────────────────────────────────────────────────── */

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL: "); printf(__VA_ARGS__); printf(" (%s:%d)\n", __FILE__, __LINE__); \
        g_failures++; \
    } else { \
        printf("  ok: "); printf(__VA_ARGS__); printf("\n"); \
    } \
} while (0)

#define MUST(cond, msg) do { \
    if (!(cond)) { \
        printf("FATAL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        return 1; \
    } \
    printf("  ok: %s\n", msg); \
} while (0)

static int do_mount(int fd, vfs_mount_t *mnt, blkdev_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    strcpy(dev->name, "hostimg");
    dev->sector_size = 512;
    dev->read = host_blk_read;
    dev->write = host_blk_write;
    dev->driver_data = (void *)(intptr_t)fd;

    memset(mnt, 0, sizeof(*mnt));
    strcpy(mnt->mountpoint, "/");
    mnt->fs = f2fs_get_fs();
    mnt->priv = dev;

    return mnt->fs->mount(mnt);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc == 3 && !strcmp(argv[1], "--format")) {    /* just a fresh, empty volume (42 MB) */
        int fd = open(argv[2], HOST_O_RDWR_CREAT, 0644);
        if (fd < 0) { perror("open image"); return 2; }
        int r = format_f2fs_image(fd);
        close(fd);
        return r ? 2 : 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-path> | --format <image-path>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], HOST_O_RDWR_CREAT, 0644);
    if (fd < 0) { perror("open image"); return 2; }

    printf("== format ==\n");
    if (format_f2fs_image(fd) != 0) { fprintf(stderr, "format failed\n"); return 2; }
    printf("  ok: image formatted\n");

    blkdev_t dev;
    vfs_mount_t mnt;

    printf("== mount ==\n");
    int r = do_mount(fd, &mnt, &dev);
    CHECK(r == VFS_OK, "mount succeeds");
    if (r != VFS_OK) { close(fd); return 1; }

    f2fs_mount_t *fmnt = (f2fs_mount_t *)mnt.priv;
    CHECK(fmnt->writable, "mount is writable (cleanly-formatted image accepted)");
    if (!fmnt->writable) { close(fd); return 1; }

    printf("== mkdir ==\n");
    r = mnt.fs->mkdir(&mnt, "/testdir");
    CHECK(r == VFS_OK, "mkdir /testdir");

    vfs_stat_t st;
    r = mnt.fs->stat(&mnt, "/testdir", &st);
    CHECK(r == VFS_OK, "stat /testdir succeeds");
    CHECK((st.attrs & VFS_ATTR_DIR) != 0, "stat /testdir reports a directory");

    printf("== create + write ==\n");
    void *fpriv = NULL;
    r = mnt.fs->open(&mnt, "/testdir/hello.txt", O_WRONLY | O_CREAT, &fpriv);
    MUST(r == VFS_OK, "create /testdir/hello.txt");

    const char *msg = "Hello from the os24arm f2fs write engine!\n";
    uint32_t actual = 0;
    r = mnt.fs->write(&mnt, fpriv, msg, (uint32_t)strlen(msg), &actual);
    CHECK(r == VFS_OK && actual == strlen(msg), "write full message");
    r = mnt.fs->close(&mnt, fpriv);
    CHECK(r == VFS_OK, "close after write");

    r = mnt.fs->open(&mnt, "/testdir/hello.txt", O_WRONLY | O_APPEND, &fpriv);
    MUST(r == VFS_OK, "reopen for append");
    char big[9000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)('A' + (i % 26));
    r = mnt.fs->write(&mnt, fpriv, big, sizeof(big), &actual);
    CHECK(r == VFS_OK && actual == sizeof(big), "append large multi-block write (crosses node tree levels)");
    r = mnt.fs->close(&mnt, fpriv);
    CHECK(r == VFS_OK, "close after append");

    printf("== read back ==\n");
    r = mnt.fs->open(&mnt, "/testdir/hello.txt", O_RDONLY, &fpriv);
    MUST(r == VFS_OK, "open for read");

    r = mnt.fs->stat(&mnt, "/testdir/hello.txt", &st);
    uint32_t expect_size = (uint32_t)(strlen(msg) + sizeof(big));
    CHECK(r == VFS_OK && st.size == expect_size, "stat reports correct combined size");

    char *readbuf = (char *)malloc(st.size + 1);
    uint32_t total = 0;
    for (;;) {
        uint32_t got = 0;
        r = mnt.fs->read(&mnt, fpriv, readbuf + total, st.size - total, &got);
        if (r != VFS_OK || got == 0) break;
        total += got;
    }
    CHECK(total == st.size, "read back full size");
    CHECK(memcmp(readbuf, msg, strlen(msg)) == 0, "leading bytes match first write");
    CHECK(memcmp(readbuf + strlen(msg), big, sizeof(big)) == 0, "trailing bytes match appended write");
    free(readbuf);
    mnt.fs->close(&mnt, fpriv);

    printf("== truncate ==\n");
    r = mnt.fs->open(&mnt, "/testdir/hello.txt", O_WRONLY | O_TRUNC, &fpriv);
    MUST(r == VFS_OK, "open with O_TRUNC");
    mnt.fs->close(&mnt, fpriv);
    r = mnt.fs->stat(&mnt, "/testdir/hello.txt", &st);
    CHECK(r == VFS_OK && st.size == 0, "size is 0 after truncate");

    printf("== directory listing ==\n");
    void *dpriv = NULL;
    r = mnt.fs->opendir(&mnt, "/testdir", &dpriv);
    CHECK(r == VFS_OK, "opendir /testdir");
    int saw_hello = 0, count = 0;
    vfs_dirent_t ent;
    while (mnt.fs->readdir(&mnt, dpriv, &ent) == VFS_OK) {
        count++;
        if (strcmp(ent.name, "hello.txt") == 0) saw_hello = 1;
    }
    CHECK(count == 1 && saw_hello, "listing shows exactly hello.txt");
    mnt.fs->closedir(&mnt, dpriv);

    printf("== unlink ==\n");
    r = mnt.fs->unlink(&mnt, "/testdir/hello.txt");
    CHECK(r == VFS_OK, "unlink hello.txt");
    r = mnt.fs->stat(&mnt, "/testdir/hello.txt", &st);
    CHECK(r != VFS_OK, "stat fails after unlink");

    printf("== rename (mkdir + move across directories) ==\n");
    r = mnt.fs->mkdir(&mnt, "/otherdir");
    CHECK(r == VFS_OK, "mkdir /otherdir");
    r = mnt.fs->rename(&mnt, "/testdir", "/otherdir/moved");
    CHECK(r == VFS_OK, "rename /testdir -> /otherdir/moved");
    r = mnt.fs->stat(&mnt, "/testdir", &st);
    CHECK(r != VFS_OK, "old path gone after rename");
    r = mnt.fs->stat(&mnt, "/otherdir/moved", &st);
    CHECK(r == VFS_OK && (st.attrs & VFS_ATTR_DIR), "new path resolves as a directory");

    printf("== many small directories (exercise dentry block growth + LFS segment rollover) ==\n");
    int mkdir_fail = 0;
    char path[64];
    for (int i = 0; i < 60; i++) {
        snprintf(path, sizeof(path), "/otherdir/moved/d%03d", i);
        if (mnt.fs->mkdir(&mnt, path) != VFS_OK) { mkdir_fail = i + 1; break; }
    }
    CHECK(mkdir_fail == 0, "created 60 subdirectories without failure");

    r = mnt.fs->opendir(&mnt, "/otherdir/moved", &dpriv);
    CHECK(r == VFS_OK, "opendir /otherdir/moved");
    count = 0;
    while (mnt.fs->readdir(&mnt, dpriv, &ent) == VFS_OK) count++;
    CHECK(count == 60, "listing shows all 60 subdirectories");
    mnt.fs->closedir(&mnt, dpriv);
    printf("== many small writes in one open (as stdio does) ==\n");
    {
        static uint8_t pat[110000], back[110000];
        for (uint32_t i = 0; i < sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + (i >> 9));
        void *wf = NULL;
        r = mnt.fs->open(&mnt, "/small.bin", O_WRONLY | O_CREAT, &wf);
        CHECK(r == VFS_OK, "create /small.bin");
        uint32_t done = 0, bad = 0;
        while (r == VFS_OK && done < sizeof pat) {
            uint32_t n = sizeof pat - done < 1024 ? (uint32_t)(sizeof pat - done) : 1024, got = 0;
            if (mnt.fs->write(&mnt, wf, pat + done, n, &got) != VFS_OK || got != n) { bad = 1; break; }
            done += n;
        }
        if (wf) mnt.fs->close(&mnt, wf);
        CHECK(!bad && done == sizeof pat, "110000 bytes written 1 KB at a time (%u)", done);
        r = mnt.fs->stat(&mnt, "/small.bin", &st);
        CHECK(r == VFS_OK && st.size == sizeof pat, "its size (%u)", (unsigned)st.size);
        void *rf = NULL;
        uint32_t tot = 0, got = 0;
        if (mnt.fs->open(&mnt, "/small.bin", O_RDONLY, &rf) == VFS_OK) {
            while (tot < sizeof back && mnt.fs->read(&mnt, rf, back + tot, sizeof back - tot, &got) == VFS_OK && got) tot += got;
            mnt.fs->close(&mnt, rf);
        }
        CHECK(tot == sizeof pat && memcmp(back, pat, sizeof pat) == 0, "read back the same (%u bytes)", tot);
    }

    printf("== space: 30 files written, rewritten ==\n");
    {
        static uint8_t blk[20000];
        memset(blk, 'q', sizeof blk);
        int ok = 1;
        for (int round = 0; round < 3 && ok; round++)
            for (int i = 0; i < 10 && ok; i++) {
                char nm[32]; snprintf(nm, sizeof nm, "/f%02d.dat", i);
                void *wf = NULL; uint32_t got = 0;
                if (mnt.fs->open(&mnt, nm, O_WRONLY | O_CREAT | O_TRUNC, &wf) != VFS_OK) { ok = 0; printf("  open %s failed (round %d)\n", nm, round); break; }
                if (mnt.fs->write(&mnt, wf, blk, sizeof blk, &got) != VFS_OK || got != sizeof blk) { ok = 0; printf("  write %s failed (round %d)\n", nm, round); }
                mnt.fs->close(&mnt, wf);
            }
        CHECK(ok, "rewrites don't run out of space");
    }

    printf("== space reclaim: line-at-a-time writes, rewrites past the volume's size ==\n");
    {
        void *wf = NULL; uint32_t got = 0, n = 0; int ok = 1;
        r = mnt.fs->open(&mnt, "/lines.txt", O_WRONLY | O_CREAT | O_TRUNC, &wf);
        for (int i = 1; r == VFS_OK && i <= 20000; i++) {    /* (a line a write: stdio, line-buffered) */
            char ln[16]; int l = snprintf(ln, sizeof ln, "%d\n", i);
            if (mnt.fs->write(&mnt, wf, ln, (uint32_t)l, &got) != VFS_OK || got != (uint32_t)l) { ok = 0; printf("  line %d failed\n", i); break; }
            n += (uint32_t)l;
        }
        if (wf) mnt.fs->close(&mnt, wf);
        r = mnt.fs->stat(&mnt, "/lines.txt", &st);
        CHECK(ok && r == VFS_OK && st.size == n && n == 108894, "20000 one-line writes (%u bytes, size %u) - node blocks reclaimed", n, (unsigned)st.size);
        static uint8_t mb[1 << 20];
        for (uint32_t i = 0; i < sizeof mb; i++) mb[i] = (uint8_t)(i * 13);
        ok = 1;
        for (int round = 0; round < 50 && ok; round++) {
            mb[0] = (uint8_t)round;
            wf = NULL;
            if (mnt.fs->open(&mnt, "/mb.bin", O_WRONLY | O_CREAT | O_TRUNC, &wf) != VFS_OK) { ok = 0; break; }
            if (mnt.fs->write(&mnt, wf, mb, sizeof mb, &got) != VFS_OK || got != sizeof mb) { ok = 0; printf("  round %d: wrote %u\n", round, got); }
            mnt.fs->close(&mnt, wf);
        }
        CHECK(ok, "a 1 MB file rewritten 50 times (50 MB through a 32 MB main area)");
        uint32_t sum = 0;
        for (uint32_t seg = 0; seg < fmnt->main_segments; seg++) {
            uint8_t blk[F2FS_BLKSIZE];
            uint32_t b = seg / SIT_ENTRY_PER_BLOCK;
            pread(fd, blk, sizeof blk, (off_t)(fmnt->sb.sit_blkaddr + b) * F2FS_BLKSIZE);
            sum += ((struct f2fs_sit_block *)blk)->entries[seg % SIT_ENTRY_PER_BLOCK].vblocks & 0x3FF;
        }
        CHECK(sum == fmnt->ckpt.valid_block_count, "SIT's live blocks (%u) are the checkpoint's count (%u)", sum, (unsigned)fmnt->ckpt.valid_block_count);
    }

    printf("== the cleaner: half-used segments made free again ==\n");
    {
        static uint8_t f128[128 * 1024], back[128 * 1024];
        int made = 0, ok = 1;
        for (int i = 0; i < 400; i++) {                       /* (until it's full) */
            char nm[32]; snprintf(nm, sizeof nm, "/g%03d.dat", i);
            for (uint32_t k = 0; k < sizeof f128; k++) f128[k] = (uint8_t)(i + k * 3);
            void *wf = NULL; uint32_t got = 0;
            if (mnt.fs->open(&mnt, nm, O_WRONLY | O_CREAT | O_TRUNC, &wf) != VFS_OK) break;
            int w = mnt.fs->write(&mnt, wf, f128, sizeof f128, &got);
            mnt.fs->close(&mnt, wf);
            if (w != VFS_OK || got != sizeof f128) { mnt.fs->unlink(&mnt, nm); break; }
            made++;
        }
        for (int i = 0; i < made; i += 2) { char nm[32]; snprintf(nm, sizeof nm, "/g%03d.dat", i); mnt.fs->unlink(&mnt, nm); }
        uint32_t big = 0;
        static uint8_t mb4[4 << 20];
        memset(mb4, 'B', sizeof mb4);
        for (int k = 0; k < 2; k++) {                          /* 8 MB more: only room if half-used segments are cleaned */
            char nm[32]; snprintf(nm, sizeof nm, "/big%d.dat", k);
            void *wf = NULL; uint32_t got = 0;
            if (mnt.fs->open(&mnt, nm, O_WRONLY | O_CREAT | O_TRUNC, &wf) != VFS_OK) break;
            for (uint32_t off = 0; off < sizeof mb4; off += 256 * 1024) {
                if (mnt.fs->write(&mnt, wf, mb4 + off, 256 * 1024, &got) != VFS_OK || got != 256 * 1024) break;
                big += got;
            }
            mnt.fs->close(&mnt, wf);
        }
        CHECK(made > 40 && big == 2u * sizeof mb4, "%d files of 128 KB (the volume full), every other one deleted, then 8 MB more (%u)", made, big);
        for (int i = 1; i < made && ok; i += 2) {             /* the ones left, intact after their blocks moved */
            char nm[32]; snprintf(nm, sizeof nm, "/g%03d.dat", i);
            for (uint32_t k = 0; k < sizeof f128; k++) f128[k] = (uint8_t)(i + k * 3);
            void *rf = NULL; uint32_t tot = 0, got = 0;
            if (mnt.fs->open(&mnt, nm, O_RDONLY, &rf) != VFS_OK) { ok = 0; printf("  %s gone\n", nm); break; }
            while (tot < sizeof back && mnt.fs->read(&mnt, rf, back + tot, sizeof back - tot, &got) == VFS_OK && got) tot += got;
            mnt.fs->close(&mnt, rf);
            if (tot != sizeof f128 || memcmp(back, f128, sizeof f128)) { ok = 0; printf("  %s differs (%u bytes)\n", nm, tot); }
        }
        CHECK(ok, "the files that stayed read back the same");
        uint32_t sum = 0;
        for (uint32_t seg = 0; seg < fmnt->main_segments; seg++) {
            uint8_t blk[F2FS_BLKSIZE];
            pread(fd, blk, sizeof blk, (off_t)(fmnt->sb.sit_blkaddr + seg / SIT_ENTRY_PER_BLOCK) * F2FS_BLKSIZE);
            sum += ((struct f2fs_sit_block *)blk)->entries[seg % SIT_ENTRY_PER_BLOCK].vblocks & 0x3FF;
        }
        CHECK(sum == fmnt->ckpt.valid_block_count, "SIT's live blocks (%u) are still the checkpoint's count (%u)", sum, (unsigned)fmnt->ckpt.valid_block_count);
    }

    printf("== rmdir ==\n");
    r = mnt.fs->mkdir(&mnt, "/rmd");
    CHECK(r == VFS_OK, "mkdir /rmd");
    r = mnt.fs->unlink(&mnt, "/rmd");
    CHECK(r == VFS_OK, "an empty directory goes (%d)", r);
    r = mnt.fs->stat(&mnt, "/rmd", &st);
    CHECK(r != VFS_OK, "stat fails after it");


    printf("== unmount ==\n");
    r = mnt.fs->unmount(&mnt);
    CHECK(r == VFS_OK, "unmount succeeds");

    /* ── Remount + re-verify: the strongest available substitute for an
     * independent fsck (see this file's header comment) - proves the
     * checkpoint commit protocol is genuinely durable, not just
     * self-consistent within one open session. ─────────────────────────── */
    printf("== remount ==\n");
    r = do_mount(fd, &mnt, &dev);
    MUST(r == VFS_OK, "remount succeeds");
    fmnt = (f2fs_mount_t *)mnt.priv;
    CHECK(fmnt->writable, "remounted image still reports writable");

    r = mnt.fs->opendir(&mnt, "/otherdir/moved", &dpriv);
    CHECK(r == VFS_OK, "opendir /otherdir/moved survives remount");
    count = 0;
    while (mnt.fs->readdir(&mnt, dpriv, &ent) == VFS_OK) count++;
    CHECK(count == 60, "all 60 subdirectories still listed after remount");
    mnt.fs->closedir(&mnt, dpriv);

    r = mnt.fs->stat(&mnt, "/otherdir/moved/d059", &st);
    CHECK(r == VFS_OK && (st.attrs & VFS_ATTR_DIR), "last-created subdirectory survives remount");
    {   /* blocks the cleaner moved: there after a remount too */
        static uint8_t want[128 * 1024], got_b[128 * 1024];
        for (uint32_t k = 0; k < sizeof want; k++) want[k] = (uint8_t)(1 + k * 3);
        void *rf = NULL; uint32_t tot = 0, got = 0;
        if (mnt.fs->open(&mnt, "/g001.dat", O_RDONLY, &rf) == VFS_OK) {
            while (tot < sizeof got_b && mnt.fs->read(&mnt, rf, got_b + tot, sizeof got_b - tot, &got) == VFS_OK && got) tot += got;
            mnt.fs->close(&mnt, rf);
        }
        CHECK(tot == sizeof want && !memcmp(got_b, want, sizeof want), "a file the cleaner moved reads back the same after remount");
        r = mnt.fs->stat(&mnt, "/big1.dat", &st);
        CHECK(r == VFS_OK && st.size == (4u << 20), "and the one written after cleaning (%u)", (unsigned)st.size);
    }

    mnt.fs->unmount(&mnt);
    close(fd);

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
