/* vmtest.c - demand paging and VM areas: big sparse mappings, a lazy stack,
 * guard pages, sparse memfds shared across fork with PUNCH_HOLE, mremap,
 * munmap in the middle of an area. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

static int deep(int n, volatile char *prev)
{
    volatile char buf[4096];
    buf[0] = (char)n; buf[4095] = prev ? prev[0] : 0;
    return n ? deep(n - 1, buf) + buf[0] - (char)n : 0;
}

static int child_status(void (*fn)(void *), void *arg)
{
    pid_t p = fork();
    if (p == 0) { fn(arg); _exit(0); }
    int st;
    waitpid(p, &st, 0);
    return st;
}
static void touch(void *a) { *(volatile char *)a = 1; }

int main(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    /* 1 GB anonymous, touched sparsely */
    size_t big = 1UL << 30;
    char *m = mmap(0, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED, "mmap 1 GB anonymous at %p", (void *)m);
    int ok = 1;
    if (m != MAP_FAILED) {
        for (size_t off = 0; off < big; off += 64UL << 20) m[off] = (char)(off >> 20);
        for (size_t off = 0; off < big; off += 64UL << 20) if (m[off] != (char)(off >> 20)) ok = 0;
        ok = ok && m[12345] == 0 && m[big - 1] == 0;       /* untouched pages read as zero */
    }
    CHECK(ok, "16 pages touched across 1 GB, the rest zero");
    CHECK(munmap(m, big) == 0, "munmap 1 GB");

    /* the stack grows on demand (8 MB of recursion > the 64 KB mapped at exec) */
    CHECK(deep(2000, 0) == 0, "deep recursion: ~8 MB of stack");

    /* a PROT_NONE guard page */
    char *g = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(g + 4096, 4096, PROT_NONE);
    g[0] = 1; g[8192] = 2;
    int st = child_status(touch, g + 4096);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "guard page: SIGSEGV (%d)", WIFSIGNALED(st) ? WTERMSIG(st) : -1);
    st = child_status(touch, (char *)0x1000);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "unmapped address: SIGSEGV");

    /* sparse memfd: 512 MB, shared across fork, punch a hole */
    int fd = memfd_create("pool", MFD_CLOEXEC);
    size_t pool = 512UL << 20;
    CHECK(ftruncate(fd, (off_t)pool) == 0, "memfd ftruncate 512 MB");
    char *s = mmap(0, pool, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(s != MAP_FAILED, "mmap the whole pool");
    s[0] = 'a';
    pid_t p = fork();
    if (p == 0) { s[pool / 2] = 'b'; s[pool - 1] = 'c'; s[4096 * 3] = 'd'; _exit(0); }
    waitpid(p, &st, 0);
    CHECK(s[0] == 'a' && s[pool / 2] == 'b' && s[pool - 1] == 'c', "child's writes in the pool visible");
    CHECK(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096 * 3, 4096) == 0 && s[4096 * 3] == 0,
          "PUNCH_HOLE: the page reads as zero again");
    char *s2 = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, pool / 2);
    CHECK(s2 != MAP_FAILED && s2[0] == 'b', "a second mapping at an offset sees the same page");
    munmap(s2, 4096);
    munmap(s, pool);
    close(fd);

    /* mremap: grow (in place or moved), contents kept */
    char *r = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    strcpy(r, "kept"); r[8191] = 'z';
    char *r2 = mremap(r, 8192, 64 << 20, MREMAP_MAYMOVE);
    CHECK(r2 != MAP_FAILED && !strcmp(r2, "kept") && r2[8191] == 'z' && r2[(64 << 20) - 1] == 0,
          "mremap 8 KB -> 64 MB keeps the contents");
    char *r3 = mremap(r2, 64 << 20, 4096, 0);
    CHECK(r3 == r2 && !strcmp(r3, "kept"), "mremap shrink in place");
    munmap(r3, 4096);

    /* munmap the middle of an area */
    char *a = mmap(0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    a[0] = 1; a[8192] = 3;
    munmap(a + 4096, 4096);
    st = child_status(touch, a + 4096);
    CHECK(a[0] == 1 && a[8192] == 3 && WIFSIGNALED(st), "munmap of the middle page splits the area");

    printf("vmtest: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
