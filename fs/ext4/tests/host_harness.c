/**
 * @file host_harness.c
 * @brief Host-native (macOS/Linux clang, NOT the ARM64 cross compiler)
 *        exerciser for the real fs/ext4/ext4.c + ext4_write.c driver code
 *        against a real mke2fs-built image.
 *
 * Not part of the kernel build (see scripts/test_ext4_driver.sh, which
 * compiles this ad hoc). Calls the driver's vfs_fs_t ops directly
 * (mount/open/read/write/stat/opendir/readdir/mkdir/unlink/rename/close),
 * bypassing kernel/src/vfs.c's fd table - this is a unit test of the ext4
 * driver itself, not the full VFS layer.
 *
 * Usage: host_harness <image-path>
 * Exits 0 and prints "ALL PASSED" on success, nonzero with a message
 * naming the failed step otherwise. scripts/test_ext4_driver.sh runs
 * `e2fsck -f` on the image between phases as an independent correctness
 * oracle - this program only checks its own view (mount/read/stat/listing)
 * is self-consistent with what it wrote.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

#include "ext4_internal.h"
#include "blkdev.h"

/* ── mem_alloc/mem_free (ext4_internal.h contract) - host malloc/free ────── */

void *mem_alloc(size_t size) { return malloc(size); }
void mem_free(void *ptr) { free(ptr); }

/* ── kprintf/terminal_writestring/uart_puts - forward to real stdio ──────── */

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
void terminal_writestring(const char *s) { fputs(s, stdout); }
void uart_puts(const char *s) { fputs(s, stdout); }

/* Pulled in by ext4_init() (fs/ext4/ext4.c) for symbol resolution only -
 * this harness calls the vfs_fs_t ops (mount/open/.../close) directly and
 * never calls ext4_init() itself, so these are never actually invoked. */
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

/* ── Test scaffolding ─────────────────────────────────────────────────────── */

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        g_failures++; \
    } else { \
        printf("  ok: %s\n", msg); \
    } \
} while (0)

/* For open() calls specifically: bails out immediately on failure instead
 * of letting a later write/read/close use a NULL or (after a preceding
 * close()) dangling fpriv - the harness would otherwise mask the real
 * failure behind an unrelated crash. */
#define MUST(cond, msg) do { \
    if (!(cond)) { \
        printf("FATAL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        return 1; \
    } \
    printf("  ok: %s\n", msg); \
} while (0)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 2) {
        fprintf(stderr, "usage: %s <image-path>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open image"); return 2; }

    blkdev_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.name, "hostimg");
    dev.sector_size = 512;
    dev.read = host_blk_read;
    dev.write = host_blk_write;
    dev.driver_data = (void *)(intptr_t)fd;

    vfs_mount_t mnt;
    memset(&mnt, 0, sizeof(mnt));
    strcpy(mnt.mountpoint, "/");
    mnt.fs = ext4_get_fs();
    mnt.priv = &dev;

    printf("== mount ==\n");
    int r = mnt.fs->mount(&mnt);
    CHECK(r == VFS_OK, "mount succeeds");
    if (r != VFS_OK) { close(fd); return 1; }

    ext4_mount_t *emnt = (ext4_mount_t *)mnt.priv;
    CHECK(emnt->writable, "mount is writable (image feature flags accepted)");
    if (!emnt->writable) { close(fd); return 1; }

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

    const char *msg = "Hello from the os24arm ext4 write engine!\n";
    uint32_t actual = 0;
    r = mnt.fs->write(&mnt, fpriv, msg, (uint32_t)strlen(msg), &actual);
    CHECK(r == VFS_OK && actual == strlen(msg), "write full message");
    r = mnt.fs->close(&mnt, fpriv);
    CHECK(r == VFS_OK, "close after write");

    /* A second, larger write exercising multi-block append + a
     * non-block-aligned tail fill on a fresh open in append-at-end mode. */
    r = mnt.fs->open(&mnt, "/testdir/hello.txt", O_WRONLY | O_APPEND, &fpriv);
    MUST(r == VFS_OK, "reopen for append");
    char big[5000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)('A' + (i % 26));
    r = mnt.fs->write(&mnt, fpriv, big, sizeof(big), &actual);
    CHECK(r == VFS_OK && actual == sizeof(big), "append large multi-block write");
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

    printf("== many small directories (exercise block-growth + bitmap wraparound) ==\n");
    int mkdir_fail = 0;
    char path[64];
    for (int i = 0; i < 80; i++) {
        snprintf(path, sizeof(path), "/otherdir/moved/d%03d", i);
        if (mnt.fs->mkdir(&mnt, path) != VFS_OK) { mkdir_fail = i; break; }
    }
    CHECK(mkdir_fail == 0, "created 80 subdirectories without failure");

    r = mnt.fs->opendir(&mnt, "/otherdir/moved", &dpriv);
    CHECK(r == VFS_OK, "opendir /otherdir/moved");
    count = 0;
    while (mnt.fs->readdir(&mnt, dpriv, &ent) == VFS_OK) count++;
    CHECK(count == 80, "listing shows all 80 subdirectories");
    mnt.fs->closedir(&mnt, dpriv);

    printf("== unmount ==\n");
    r = mnt.fs->unmount(&mnt);
    CHECK(r == VFS_OK, "unmount succeeds");

    close(fd);

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "SOME FAILED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
