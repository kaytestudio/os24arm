/**
 * @file linux.c
 * @brief Linux-compatible user mode, stage 1 (see userland/README.md)
 *
 * Runs one static AArch64 Linux ELF executable at a time at EL0, inside
 * the MMU's user window (mmu.h: 64 MB at 0x60000000, 4 KB pages, identity
 * mapped), and implements the Linux system calls such a program (musl)
 * needs:
 *
 *   user window layout
 *     0x60000000  program image (PT_LOAD segments, per-page permissions)
 *       ...       brk heap, growing up              (to 0x62000000)
 *     0x62000000  mmap area, growing up             (to 0x63800000)
 *     0x63800000  stack, 8 MB, growing down         (to 0x64000000)
 *
 *   syscalls: Linux AArch64 numbering, svc #0, number in x8, arguments in
 *   x0..x5, result (or -errno) in x0. Files go through the VFS (paths are
 *   VFS paths: /BOOT/..., /H/... for MOUNTed volumes); fds 0-2 are the
 *   console. Every user pointer is checked against the user page tables
 *   before the kernel touches it (-EFAULT otherwise).
 *
 * Not yet (later stages): more than one process, fork/exec/wait, signals,
 * preemption, poll/epoll, sockets, shared mappings, dynamic linking.
 */

#include "linux_abi.h"
#include "mmu.h"
#include "vfs.h"
#include "kio.h"
#include "uart.h"
#include "kprintf.h"

extern long el0_run(lx_u64 entry, lx_u64 sp, lx_u64 *kctx);
extern void el0_exit(lx_u64 *kctx, long status) __attribute__((noreturn));
extern uint32_t kernel_time_seconds(void) __attribute__((weak));
extern void usbd_daemon(void) __attribute__((weak));

#define PAGE            4096UL
#define USER_TOP        (MMU_USER_BASE + MMU_USER_SIZE)
#define BRK_LIMIT       0x62000000UL
#define MMAP_BASE       0x62000000UL
#define MMAP_LIMIT      0x63800000UL
#define STACK_BOTTOM    0x63800000UL
#define MAX_FDS         64
#define MAX_PHDRS       16
#define PAGES           (MMU_USER_SIZE / PAGE)

/* errno */
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39

/* Linux AArch64 open flags (asm-generic values, arm64 directory bits) */
#define L_O_ACCMODE   3
#define L_O_CREAT     0100
#define L_O_EXCL      0200
#define L_O_TRUNC     01000
#define L_O_APPEND    02000
#define L_O_DIRECTORY 040000
#define AT_FDCWD      (-100)
#define AT_REMOVEDIR  0x200

typedef __INT64_TYPE__ s64;

enum { FD_FREE, FD_CONSOLE, FD_FILE, FD_DIR };

typedef struct {
    int      kind;
    int      vfd;              /* VFS file or directory descriptor */
    int      flags;
    int      dir_pos;          /* synthetic "." / ".." emitted */
    int      have_pending;     /* a readdir result that didn't fit */
    vfs_dirent_t pending;
    char     path[VFS_PATH_MAX];
} lfd_t;

static struct {
    int      active;
    lx_u64   kctx[13];
    lx_u64   brk_start, brk_cur, mmap_next;
    char     cwd[VFS_PATH_MAX];
    lfd_t    fd[MAX_FDS];
    lx_u64   rng;
} P;

static uint8_t page_prot[PAGES];            /* loader: combined segment permissions */

/* ── User memory access ───────────────────────────────────────────────── */
static int uok(lx_u64 p, lx_u64 n, int w)
{
    return mmu_user_mapped(p, n, w ? MMU_PROT_WRITE : MMU_PROT_READ);
}

static int copy_in_str(char *dst, lx_u64 src, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++) {
        if ((i == 0 || ((src + i) & (PAGE - 1)) == 0) && !uok(src + i, 1, 0)) return -EFAULT;
        dst[i] = ((const char *)(uintptr_t)src)[i];
        if (!dst[i]) return 0;
    }
    return -ERANGE;
}

static int verr(int r)                      /* VFS error -> -errno */
{
    switch (r) {
    case VFS_ERR_NOTFOUND: case VFS_ERR_NOMOUNT: return -ENOENT;
    case VFS_ERR_NOTDIR:   return -ENOTDIR;
    case VFS_ERR_ISDIR:    return -EISDIR;
    case VFS_ERR_ROFS:     return -EROFS;
    case VFS_ERR_NOSPACE:  return -ENOSPC;
    case VFS_ERR_INVAL:    return -EINVAL;
    case VFS_ERR_BADFD:    return -EBADF;
    case VFS_ERR_BUSY:     return -EBUSY;
    default:               return -EIO;
    }
}

/* ── Paths ────────────────────────────────────────────────────────────── */
/* user path (relative to cwd or dirfd) -> normalised absolute VFS path */
static int user_path(int dirfd, lx_u64 up, char *out)
{
    char in[VFS_PATH_MAX], tmp[2 * VFS_PATH_MAX];
    int r = copy_in_str(in, up, sizeof in);
    if (r) return r;
    uint32_t n = 0;
    if (in[0] != '/') {
        const char *base = P.cwd;
        if (dirfd != AT_FDCWD) {
            if (dirfd < 0 || dirfd >= MAX_FDS || P.fd[dirfd].kind != FD_DIR) return -EBADF;
            base = P.fd[dirfd].path;
        }
        for (const char *c = base; *c && n < VFS_PATH_MAX; c++) tmp[n++] = *c;
        tmp[n++] = '/';
    }
    for (const char *c = in; *c && n < sizeof tmp - 1; c++) tmp[n++] = *c;
    tmp[n] = 0;
    /* split, drop "." and "", resolve ".." */
    uint32_t o = 0;
    out[o++] = '/';
    for (char *s = tmp; *s; ) {
        while (*s == '/') s++;
        char *e = s;
        while (*e && *e != '/') e++;
        uint32_t len = (uint32_t)(e - s);
        if (len == 0) break;
        if (len == 1 && s[0] == '.') {
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; }
        } else {
            if (o + len + 1 >= VFS_PATH_MAX) return -ERANGE;
            if (o > 1) out[o++] = '/';
            for (uint32_t i = 0; i < len; i++) out[o++] = s[i];
        }
        s = e;
    }
    if (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return 0;
}

/* ── Console ──────────────────────────────────────────────────────────── */
static void con_write(const char *s, lx_u64 n)
{
    for (lx_u64 i = 0; i < n; i++) {
        if (s[i] == '\n') uart_putc('\r');
        uart_putc(s[i]);
    }
}

/* Cooked line input: echo, backspace, Enter -> '\n', Ctrl-D at the start
 * of a line -> end of file. */
static s64 con_read(char *buf, lx_u64 n)
{
    static char line[256];
    static uint32_t have, pos;
    if (pos >= have) {
        have = pos = 0;
        for (;;) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') { con_write("\n", 1); line[have++] = '\n'; break; }
            if (c == 0x04 && !have) return 0;
            if ((c == 0x7F || c == '\b') && have) { have--; uart_puts("\b \b"); continue; }
            if ((uint8_t)c < 0x20 || have >= sizeof line - 1) continue;
            line[have++] = c;
            uart_putc(c);
        }
    }
    lx_u64 k = 0;
    while (k < n && pos < have) buf[k++] = line[pos++];
    return (s64)k;
}

/* ── Time and randomness ──────────────────────────────────────────────── */
static lx_u64 ticks(void) { lx_u64 v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)); return v; }
static lx_u64 freq(void)  { lx_u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

static lx_u64 rnd(void)
{
    if (!P.rng) P.rng = ticks() * 0x9E3779B97F4A7C15ULL | 1;
    P.rng ^= P.rng << 13; P.rng ^= P.rng >> 7; P.rng ^= P.rng << 17;
    return P.rng;
}

/* DOS date/time (VFS stat) -> seconds since 1970 */
static s64 dos_to_unix(uint16_t d, uint16_t t)
{
    if (!d) return 0;
    int y = 1980 + (d >> 9), mo = (d >> 5) & 15, da = d & 31;
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    s64 days = (s64)(y - 1970) * 365 + (y - 1969) / 4 + cum[(mo >= 1 && mo <= 12) ? mo - 1 : 0] + da - 1;
    if (mo > 2 && y % 4 == 0) days++;
    return days * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2;
}

/* ── File descriptors ─────────────────────────────────────────────────── */
static int fd_alloc(int kind)
{
    for (int i = 0; i < MAX_FDS; i++)
        if (P.fd[i].kind == FD_FREE) {
            kmemset(&P.fd[i], 0, sizeof P.fd[i]);
            P.fd[i].kind = kind;
            return i;
        }
    return -EMFILE;
}

static lfd_t *fd_get(lx_u64 fd)
{
    return (fd < MAX_FDS && P.fd[fd].kind != FD_FREE) ? &P.fd[fd] : 0;
}

static void fd_close(int i)
{
    lfd_t *f = &P.fd[i];
    if (f->kind == FD_FILE) vfs_close(f->vfd);
    if (f->kind == FD_DIR)  vfs_closedir(f->vfd);
    f->kind = FD_FREE;
}

/* struct stat, AArch64 Linux layout (asm-generic) */
static void fill_stat(uint8_t *st, const vfs_stat_t *vs, int is_console)
{
    kmemset(st, 0, 128);
    uint32_t mode;
    if (is_console)                       mode = 0020000 | 0620;   /* S_IFCHR */
    else if (vs->attrs & VFS_ATTR_DIR)    mode = 0040000 | 0755;
    else mode = 0100000 | ((vs->attrs & VFS_ATTR_RDONLY) ? 0444 : 0644);
    *(uint32_t *)(st + 16) = mode;                 /* st_mode */
    *(uint32_t *)(st + 20) = 1;                    /* st_nlink */
    if (!is_console) {
        *(s64 *)(st + 48) = vs->size;              /* st_size */
        *(int32_t *)(st + 56) = 4096;              /* st_blksize */
        *(s64 *)(st + 64) = ((s64)vs->size + 511) / 512;
        s64 t = dos_to_unix(vs->mdate, vs->mtime);
        *(s64 *)(st + 72) = t;                     /* atime */
        *(s64 *)(st + 88) = t;                     /* mtime */
        *(s64 *)(st + 104) = t;                    /* ctime */
    }
}

/* ── Memory ───────────────────────────────────────────────────────────── */
static lx_u64 page_up(lx_u64 a) { return (a + PAGE - 1) & ~(PAGE - 1); }

static s64 sys_brk(lx_u64 want)
{
    if (want < P.brk_start || want > BRK_LIMIT) return (s64)P.brk_cur;
    lx_u64 old = page_up(P.brk_cur), new = page_up(want);
    if (new > old) {
        if (mmu_user_map(old, new - old, MMU_PROT_READ | MMU_PROT_WRITE)) return (s64)P.brk_cur;
        kmemset((void *)(uintptr_t)old, 0, new - old);
    } else if (new < old) {
        mmu_user_unmap(new, old - new);
    }
    P.brk_cur = want;
    return (s64)want;
}

static int prot_bits(lx_u64 p)              /* PROT_READ 1, WRITE 2, EXEC 4 */
{
    return (int)((p & 1 ? MMU_PROT_READ : 0) | (p & 2 ? MMU_PROT_WRITE : 0) |
                 (p & 4 ? MMU_PROT_EXEC : 0));
}

static s64 sys_mmap(lx_u64 addr, lx_u64 len, lx_u64 prot, lx_u64 flags, s64 fd, lx_u64 off)
{
    const lx_u64 MAP_SHARED = 1, MAP_FIXED = 0x10, MAP_ANON = 0x20;
    if (!len || (off & (PAGE - 1))) return -EINVAL;
    if (flags & MAP_SHARED && !(flags & MAP_ANON)) return -ENOSYS;   /* stage 3 */
    lx_u64 size = page_up(len), at;
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE - 1)) || addr < MMU_USER_BASE || addr + size > STACK_BOTTOM) return -EINVAL;
        at = addr;
    } else {
        if (P.mmap_next + size > MMAP_LIMIT || P.mmap_next + size < P.mmap_next) return -ENOMEM;
        at = P.mmap_next;
        P.mmap_next += size;
    }
    if (mmu_user_map(at, size, MMU_PROT_READ | MMU_PROT_WRITE)) return -ENOMEM;
    kmemset((void *)(uintptr_t)at, 0, size);
    if (!(flags & MAP_ANON)) {                       /* private file mapping: a copy */
        lfd_t *f = fd_get((lx_u64)fd);
        if (!f || f->kind != FD_FILE) { mmu_user_unmap(at, size); return -EBADF; }
        if (vfs_seek(f->vfd, (int32_t)off, SEEK_SET) < 0) { mmu_user_unmap(at, size); return -EINVAL; }
        int got = vfs_read(f->vfd, (void *)(uintptr_t)at, (uint32_t)len);
        if (got < 0) { mmu_user_unmap(at, size); return verr(got); }
    }
    int p = prot_bits(prot);
    if (p != (MMU_PROT_READ | MMU_PROT_WRITE)) {
        if (p) mmu_user_map(at, size, p);
        else   mmu_user_unmap(at, size);                 /* PROT_NONE */
    }
    return (s64)at;
}

/* ── System calls ─────────────────────────────────────────────────────── */
static s64 sys_openat(int dirfd, lx_u64 up, lx_u64 flags)
{
    char path[VFS_PATH_MAX];
    int r = user_path(dirfd, up, path);
    if (r) return r;
    vfs_stat_t st;
    int exists = vfs_stat(path, &st) == VFS_OK;
    if (exists && (flags & L_O_CREAT) && (flags & L_O_EXCL)) return -EEXIST;
    if ((flags & L_O_DIRECTORY) || (exists && (st.attrs & VFS_ATTR_DIR))) {
        if (!exists) return -ENOENT;
        if (!(st.attrs & VFS_ATTR_DIR)) return -ENOTDIR;
        if ((flags & L_O_ACCMODE) != 0) return -EISDIR;
        int i = fd_alloc(FD_DIR);
        if (i < 0) return i;
        int d = vfs_opendir(path);
        if (d < 0) { P.fd[i].kind = FD_FREE; return verr(d); }
        P.fd[i].vfd = d;
        kstrncpy(P.fd[i].path, path, sizeof P.fd[i].path);
        return i;
    }
    int i = fd_alloc(FD_FILE);
    if (i < 0) return i;
    int vf = vfs_open(path, (int)(flags & (L_O_ACCMODE | L_O_CREAT | L_O_TRUNC | L_O_APPEND)));
    if (vf < 0) { P.fd[i].kind = FD_FREE; return verr(vf); }
    P.fd[i].vfd = vf;
    P.fd[i].flags = (int)flags;
    kstrncpy(P.fd[i].path, path, sizeof P.fd[i].path);
    return i;
}

static s64 sys_read(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    lfd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!n) return 0;
    if (!uok(buf, n, 1)) return -EFAULT;
    if (f->kind == FD_CONSOLE) return con_read((char *)(uintptr_t)buf, n);
    if (f->kind == FD_DIR) return -EISDIR;
    int r = vfs_read(f->vfd, (void *)(uintptr_t)buf, (uint32_t)(n > 0x40000000 ? 0x40000000 : n));
    return r < 0 ? verr(r) : r;
}

static s64 sys_write(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    lfd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!n) return 0;
    if (!uok(buf, n, 0)) return -EFAULT;
    if (f->kind == FD_CONSOLE) { con_write((const char *)(uintptr_t)buf, n); return (s64)n; }
    if (f->kind == FD_DIR) return -EISDIR;
    int r = vfs_write(f->vfd, (const void *)(uintptr_t)buf, (uint32_t)(n > 0x40000000 ? 0x40000000 : n));
    return r < 0 ? verr(r) : r;
}

static s64 sys_iov(lx_u64 fd, lx_u64 iov, lx_u64 cnt, int wr)
{
    if (cnt > 1024 || !uok(iov, cnt * 16, 0)) return -EFAULT;
    s64 total = 0;
    for (lx_u64 i = 0; i < cnt; i++) {
        lx_u64 base = ((lx_u64 *)(uintptr_t)iov)[2 * i], len = ((lx_u64 *)(uintptr_t)iov)[2 * i + 1];
        if (!len) continue;
        s64 r = wr ? sys_write(fd, base, len) : sys_read(fd, base, len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((lx_u64)r < len) break;
    }
    return total;
}

static s64 sys_lseek(lx_u64 fd, s64 off, lx_u64 whence)
{
    lfd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != FD_FILE) return -ESPIPE;
    if (whence > 2 || off > 0x7FFFFFFF || off < -0x7FFFFFFFLL) return -EINVAL;
    int r = vfs_seek(f->vfd, (int32_t)off, (int)whence);
    return r < 0 ? verr(r) : r;
}

static s64 sys_fstatat(int dirfd, lx_u64 up, lx_u64 stp, lx_u64 flags, int by_fd)
{
    (void)flags;
    if (!uok(stp, 128, 1)) return -EFAULT;
    vfs_stat_t vs;
    kmemset(&vs, 0, sizeof vs);
    if (by_fd) {
        lfd_t *f = fd_get((lx_u64)dirfd);
        if (!f) return -EBADF;
        if (f->kind == FD_CONSOLE) { fill_stat((uint8_t *)(uintptr_t)stp, &vs, 1); return 0; }
        int r = vfs_stat(f->path, &vs);
        if (r != VFS_OK) return verr(r);
    } else {
        char path[VFS_PATH_MAX];
        int r = user_path(dirfd, up, path);
        if (r) return r;
        if (!(path[0] == '/' && !path[1])) {
            if ((r = vfs_stat(path, &vs)) != VFS_OK) return verr(r);
        } else {
            vs.attrs = VFS_ATTR_DIR;
        }
    }
    fill_stat((uint8_t *)(uintptr_t)stp, &vs, 0);
    return 0;
}

static s64 sys_getdents64(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    lfd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != FD_DIR) return -ENOTDIR;
    if (!uok(buf, n, 1)) return -EFAULT;
    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    lx_u64 used = 0;
    for (;;) {
        vfs_dirent_t e;
        const char *name;
        int dtype = 0;
        if (f->dir_pos < 2) {
            name = f->dir_pos == 0 ? "." : "..";
            dtype = 4;
        } else if (f->have_pending) {
            e = f->pending;
            name = e.name;
        } else if (vfs_readdir(f->vfd, &e) == VFS_OK) {
            name = e.name;
        } else {
            break;
        }
        if (!dtype) {                                 /* DT_DIR 4 / DT_REG 8 */
            char p[VFS_PATH_MAX];
            uint32_t k = 0;
            for (const char *c = f->path; *c && k < sizeof p - 2; c++) p[k++] = *c;
            if (k > 1) p[k++] = '/';
            for (const char *c = name; *c && k < sizeof p - 1; c++) p[k++] = *c;
            p[k] = 0;
            vfs_stat_t vs;
            dtype = (vfs_stat(p, &vs) == VFS_OK && (vs.attrs & VFS_ATTR_DIR)) ? 4 : 8;
        }
        uint32_t nl = (uint32_t)kstrlen(name), rec = (19 + nl + 1 + 7) & ~7u;
        if (used + rec > n) {
            if (f->dir_pos >= 2 && !f->have_pending) { f->pending = e; f->have_pending = 1; }
            if (!used) return -EINVAL;
            break;
        }
        uint8_t *d = out + used;
        kmemset(d, 0, rec);
        *(lx_u64 *)d = 1 + used;                       /* d_ino (made up) */
        *(s64 *)(d + 8) = (s64)(used + rec);           /* d_off */
        *(uint16_t *)(d + 16) = (uint16_t)rec;
        d[18] = (uint8_t)dtype;
        kmemcpy(d + 19, name, nl);
        used += rec;
        if (f->dir_pos < 2) f->dir_pos++;
        else f->have_pending = 0;
    }
    return (s64)used;
}

static s64 sys_ioctl(lx_u64 fd, lx_u64 req, lx_u64 arg)
{
    lfd_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != FD_CONSOLE) return -ENOTTY;
    if (req == 0x5413) {                               /* TIOCGWINSZ */
        if (!uok(arg, 8, 1)) return -EFAULT;
        uint16_t *w = (uint16_t *)(uintptr_t)arg;
        w[0] = 25; w[1] = 80; w[2] = w[3] = 0;
        return 0;
    }
    if (req == 0x5401) {                               /* TCGETS: a plain tty */
        if (!uok(arg, 60, 1)) return -EFAULT;
        kmemset((void *)(uintptr_t)arg, 0, 60);
        return 0;
    }
    return -ENOTTY;
}

static s64 sys_clock_gettime(lx_u64 clk, lx_u64 tsp)
{
    if (!uok(tsp, 16, 1)) return -EFAULT;
    static lx_u64 t0;
    static s64 rt0;
    if (!t0) { t0 = ticks(); rt0 = kernel_time_seconds ? kernel_time_seconds() : 0; }
    lx_u64 t = ticks(), f = freq();
    s64 sec, nsec;
    if (clk == 0 || clk == 5 || clk == 8) {          /* REALTIME (+ coarse, alarm) */
        lx_u64 d = t - t0;
        sec = rt0 + (s64)(d / f);
        nsec = (s64)((d % f) * 1000000000ULL / f);
    } else {
        sec = (s64)(t / f);
        nsec = (s64)((t % f) * 1000000000ULL / f);
    }
    ((s64 *)(uintptr_t)tsp)[0] = sec;
    ((s64 *)(uintptr_t)tsp)[1] = nsec;
    return 0;
}

static s64 sys_nanosleep(lx_u64 req)
{
    if (!uok(req, 16, 0)) return -EFAULT;
    s64 sec = ((s64 *)(uintptr_t)req)[0], ns = ((s64 *)(uintptr_t)req)[1];
    if (sec < 0 || ns < 0 || ns >= 1000000000) return -EINVAL;
    lx_u64 f = freq(), end = ticks() + (lx_u64)sec * f + (lx_u64)ns * f / 1000000000ULL;
    while (ticks() < end)
        if (usbd_daemon) usbd_daemon();               /* keep USB alive */
    return 0;
}

static s64 sys_uname(lx_u64 up)
{
    if (!uok(up, 6 * 65, 1)) return -EFAULT;
    static const char *const f[6] = { "Linux", "os2warp", "6.1.0-os2warp-linux-abi",
                                      "#1 stage1", "aarch64", "(none)" };
    char *u = (char *)(uintptr_t)up;
    kmemset(u, 0, 6 * 65);
    for (int i = 0; i < 6; i++) kstrncpy(u + 65 * i, f[i], 64);
    return 0;
}

static void exit_process(long status)
{
    for (int i = 3; i < MAX_FDS; i++)
        if (P.fd[i].kind != FD_FREE) fd_close(i);
    mmu_user_clear();
    P.active = 0;
    el0_exit(P.kctx, status);
}

static s64 linux_syscall(linux_tf_t *tf)
{
    lx_u64 a0 = tf->x[0], a1 = tf->x[1], a2 = tf->x[2], a3 = tf->x[3], a4 = tf->x[4], a5 = tf->x[5];
    switch (tf->x[8]) {
    case 17: {                                          /* getcwd */
        lx_u64 n = kstrlen(P.cwd) + 1;
        if (a1 < n) return -ERANGE;
        if (!uok(a0, n, 1)) return -EFAULT;
        kmemcpy((void *)(uintptr_t)a0, P.cwd, n);
        return (s64)n;
    }
    case 23: case 24: {                                 /* dup, dup3 (consoles only) */
        lfd_t *f = fd_get(a0);
        if (!f) return -EBADF;
        if (f->kind != FD_CONSOLE) return -EINVAL;
        if (tf->x[8] == 24) {
            if (a1 >= MAX_FDS || a1 == a0) return -EINVAL;
            if (P.fd[a1].kind != FD_FREE) fd_close((int)a1);
            P.fd[a1].kind = FD_CONSOLE;
            return (s64)a1;
        }
        return fd_alloc(FD_CONSOLE);
    }
    case 25: {                                          /* fcntl */
        lfd_t *f = fd_get(a0);
        if (!f) return -EBADF;
        if (a1 == 3) return f->flags & (L_O_ACCMODE | L_O_APPEND);   /* F_GETFL */
        return 0;                                       /* F_GETFD/SETFD/SETFL */
    }
    case 29: return sys_ioctl(a0, a1, a2);
    case 34: {                                          /* mkdirat */
        char p[VFS_PATH_MAX];
        int r = user_path((int)a0, a1, p);
        if (r) return r;
        r = vfs_mkdir(p);
        return r == VFS_OK ? 0 : verr(r);
    }
    case 35: {                                          /* unlinkat */
        char p[VFS_PATH_MAX];
        int r = user_path((int)a0, a1, p);
        if (r) return r;
        vfs_stat_t vs;
        if ((r = vfs_stat(p, &vs)) != VFS_OK) return verr(r);
        if ((vs.attrs & VFS_ATTR_DIR) && !(a2 & AT_REMOVEDIR)) return -EISDIR;
        if (!(vs.attrs & VFS_ATTR_DIR) && (a2 & AT_REMOVEDIR)) return -ENOTDIR;
        r = vfs_unlink(p);
        if (r == VFS_ERR_ISDIR && (vs.attrs & VFS_ATTR_DIR)) return -ENOTEMPTY;
        return r == VFS_OK ? 0 : verr(r);
    }
    case 38: case 276: {                                /* renameat, renameat2 */
        char o[VFS_PATH_MAX], n[VFS_PATH_MAX];
        int r = user_path((int)a0, a1, o);
        if (!r) r = user_path((int)a2, a3, n);
        if (r) return r;
        r = vfs_rename(o, n);
        return r == VFS_OK ? 0 : verr(r);
    }
    case 48: {                                          /* faccessat */
        char p[VFS_PATH_MAX];
        vfs_stat_t vs;
        int r = user_path((int)a0, a1, p);
        if (r) return r;
        if (p[0] == '/' && !p[1]) return 0;
        return vfs_stat(p, &vs) == VFS_OK ? 0 : -ENOENT;
    }
    case 49: {                                          /* chdir */
        char p[VFS_PATH_MAX];
        vfs_stat_t vs;
        int r = user_path(AT_FDCWD, a0, p);
        if (r) return r;
        if (!(p[0] == '/' && !p[1])) {
            if (vfs_stat(p, &vs) != VFS_OK) return -ENOENT;
            if (!(vs.attrs & VFS_ATTR_DIR)) return -ENOTDIR;
        }
        kstrncpy(P.cwd, p, sizeof P.cwd);
        return 0;
    }
    case 56: return sys_openat((int)a0, a1, a2);
    case 57:                                            /* close */
        if (!fd_get(a0)) return -EBADF;
        fd_close((int)a0);
        return 0;
    case 61: return sys_getdents64(a0, a1, a2);
    case 62: return sys_lseek(a0, (s64)a1, a2);
    case 63: return sys_read(a0, a1, a2);
    case 64: return sys_write(a0, a1, a2);
    case 65: return sys_iov(a0, a1, a2, 0);
    case 66: return sys_iov(a0, a1, a2, 1);
    case 79: return sys_fstatat((int)a0, a1, a2, a3, 0);
    case 80: return sys_fstatat((int)a0, 0, a1, 0, 1);
    case 93: case 94: exit_process((long)(a0 & 0xFF)); return 0;   /* exit, exit_group */
    case 96: return 1;                                  /* set_tid_address -> tid */
    case 98: return -ENOSYS;                            /* futex: single thread */
    case 99: return 0;                                  /* set_robust_list */
    case 101: return sys_nanosleep(a0);
    case 113: return sys_clock_gettime(a0, a1);
    case 114: {                                         /* clock_getres */
        if (a1) {
            if (!uok(a1, 16, 1)) return -EFAULT;
            ((s64 *)(uintptr_t)a1)[0] = 0;
            ((s64 *)(uintptr_t)a1)[1] = 1;
        }
        return 0;
    }
    case 124: return 0;                                 /* sched_yield */
    case 129: case 130: case 131: return 0;            /* kill/tkill/tgkill: nothing to signal */
    case 134: case 135: return 0;                      /* rt_sigaction, rt_sigprocmask */
    case 160: return sys_uname(a0);
    case 172: case 173: case 178: return 1;            /* getpid, getppid, gettid */
    case 174: case 175: case 176: case 177: return 0;  /* getuid, geteuid, getgid, getegid */
    case 214: return sys_brk(a0);
    case 215:                                           /* munmap */
        if ((a0 & (PAGE - 1)) || a0 < MMU_USER_BASE || a0 + a1 > STACK_BOTTOM) return -EINVAL;
        mmu_user_unmap(a0, a1);
        return 0;
    case 216: {                                         /* mremap */
        lx_u64 old = page_up(a1), new = page_up(a2);
        if ((a0 & (PAGE - 1)) || !new || a0 < MMAP_BASE || a0 + old > MMAP_LIMIT ||
            !mmu_user_mapped(a0, old, MMU_PROT_READ | MMU_PROT_WRITE))
            return -EINVAL;
        if (new <= old) { mmu_user_unmap(a0 + new, old - new); return (s64)a0; }
        if (a0 + old == P.mmap_next && a0 + new <= MMAP_LIMIT) {     /* grow in place */
            if (mmu_user_map(a0 + old, new - old, MMU_PROT_READ | MMU_PROT_WRITE)) return -ENOMEM;
            kmemset((void *)(uintptr_t)(a0 + old), 0, new - old);
            P.mmap_next = a0 + new;
            return (s64)a0;
        }
        if (!(a3 & 1)) return -ENOMEM;                  /* MREMAP_MAYMOVE */
        s64 at = sys_mmap(0, new, 3, 0x22, -1, 0);      /* RW, private anonymous */
        if (at < 0) return at;
        kmemcpy((void *)(uintptr_t)at, (void *)(uintptr_t)a0, old);
        mmu_user_unmap(a0, old);
        return at;
    }
    case 222: return sys_mmap(a0, a1, a2, a3, (s64)a4, a5);
    case 226: {                                         /* mprotect */
        if ((a0 & (PAGE - 1)) || a0 < MMU_USER_BASE || a0 + a1 > USER_TOP) return -EINVAL;
        if (!mmu_user_mapped(a0, a1, MMU_PROT_READ)) return -ENOMEM;
        int p = prot_bits(a2);
        if ((p & MMU_PROT_WRITE) && (p & MMU_PROT_EXEC)) return -EPERM;   /* W^X */
        if (p) mmu_user_map(a0, a1, p);
        return 0;
    }
    case 233: return 0;                                 /* madvise */
    case 261: {                                         /* prlimit64: report "unlimited" */
        if (a3) {
            if (!uok(a3, 16, 1)) return -EFAULT;
            ((s64 *)(uintptr_t)a3)[0] = ((s64 *)(uintptr_t)a3)[1] = -1;
        }
        return 0;
    }
    case 278: {                                         /* getrandom (not cryptographic) */
        if (!uok(a0, a1, 1)) return -EFAULT;
        for (lx_u64 i = 0; i < a1; i++) ((uint8_t *)(uintptr_t)a0)[i] = (uint8_t)(rnd() >> 24);
        return (s64)a1;
    }
    default:
        kprintf("[LINUX]  unimplemented syscall %u (pc 0x%x)\n",
                (unsigned)tf->x[8], (unsigned)(tf->elr & 0xFFFFFFFF));
        return -ENOSYS;
    }
}

void linux_el0_trap(linux_tf_t *tf)
{
    lx_u64 esr, far;
    __asm__ volatile("mrs %0, esr_el1\n\tmrs %1, far_el1" : "=r"(esr), "=r"(far));
    uint32_t ec = (uint32_t)(esr >> 26);
    if (ec == 0x15) {                                   /* SVC from AArch64 */
        tf->x[0] = (lx_u64)linux_syscall(tf);
        return;
    }
    const char *what = ec == 0x24 ? "data abort" : ec == 0x20 ? "instruction abort" :
                       ec == 0x22 ? "PC alignment fault" : ec == 0x26 ? "SP alignment fault" :
                       ec == 0x00 ? "undefined instruction" : "exception";
    kprintf("[LINUX]  %s at pc 0x%x, address 0x%x (ESR 0x%x) - killed (SIGSEGV)\n",
            what, (unsigned)(tf->elr & 0xFFFFFFFF), (unsigned)(far & 0xFFFFFFFF), (unsigned)esr);
    exit_process(128 + (ec == 0x00 ? 4 : 11));          /* SIGILL / SIGSEGV */
}

/* ── ELF loader ───────────────────────────────────────────────────────── */
static int read_at(int vfd, lx_u64 off, void *buf, uint32_t n)
{
    if (vfs_seek(vfd, (int32_t)off, SEEK_SET) < 0) return -1;
    return vfs_read(vfd, buf, n) == (int)n ? 0 : -1;
}

static int load_elf(const char *path, lx_u64 *entry, lx_u64 *phdr_va, uint32_t *phnum)
{
    uint8_t eh[64], ph[MAX_PHDRS * 56];
    int vfd = vfs_open(path, O_RDONLY);
    if (vfd < 0) { kprintf("[LINUX]  %s: can't open (%d)\n", path, vfd); return -1; }
    int bad = -1;
    if (read_at(vfd, 0, eh, 64) != 0 || eh[0] != 0x7F || eh[1] != 'E' || eh[2] != 'L' ||
        eh[3] != 'F') { kprintf("[LINUX]  %s: not an ELF file\n", path); goto out; }
    if (eh[4] != 2 || eh[5] != 1 || *(uint16_t *)(eh + 18) != 183) {
        kprintf("[LINUX]  %s: not a 64-bit little-endian AArch64 ELF\n", path); goto out;
    }
    if (*(uint16_t *)(eh + 16) != 2) {
        kprintf("[LINUX]  %s: not a static executable (ET_EXEC); dynamic linking comes later\n",
                path);
        goto out;
    }
    *entry = *(lx_u64 *)(eh + 24);
    lx_u64 phoff = *(lx_u64 *)(eh + 32);
    uint16_t phent = *(uint16_t *)(eh + 54), n = *(uint16_t *)(eh + 56);
    if (phent != 56 || n == 0 || n > MAX_PHDRS || read_at(vfd, phoff, ph, n * 56u) != 0) {
        kprintf("[LINUX]  %s: bad program headers\n", path); goto out;
    }
    *phnum = n;
    *phdr_va = 0;

    /* Pass 1: check the segments and combine permissions per page. */
    kmemset(page_prot, 0, sizeof page_prot);
    lx_u64 top = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = ph + 56 * i;
        uint32_t type = *(uint32_t *)p, fl = *(uint32_t *)(p + 4);
        lx_u64 off = *(lx_u64 *)(p + 8), va = *(lx_u64 *)(p + 16);
        lx_u64 fsz = *(lx_u64 *)(p + 32), msz = *(lx_u64 *)(p + 40);
        if (type == 6) *phdr_va = va;                    /* PT_PHDR */
        if (type != 1) continue;                         /* PT_LOAD */
        if (va < MMU_USER_BASE || va + msz > BRK_LIMIT || va + msz < va || fsz > msz) {
            kprintf("[LINUX]  %s: segment 0x%x+0x%x outside 0x%x..0x%x - link it with "
                    "--image-base=0x%x (userland/build-tests.sh)\n", path, (unsigned)va,
                    (unsigned)msz, (unsigned)MMU_USER_BASE, (unsigned)BRK_LIMIT,
                    (unsigned)MMU_USER_BASE);
            goto out;
        }
        if (off == 0 && !*phdr_va) *phdr_va = va + phoff;
        int prot = (fl & 4 ? MMU_PROT_READ : 0) | (fl & 2 ? MMU_PROT_WRITE : 0) |
                   (fl & 1 ? MMU_PROT_EXEC : 0);
        for (lx_u64 a = va & ~(PAGE - 1); a < va + msz; a += PAGE) {
            uint8_t *pp = &page_prot[(a - MMU_USER_BASE) / PAGE];
            *pp |= (uint8_t)prot;
            if ((*pp & MMU_PROT_WRITE) && (*pp & MMU_PROT_EXEC)) {
                kprintf("[LINUX]  %s: a page would be writable and executable - refusing\n", path);
                goto out;
            }
        }
        if (page_up(va + msz) > top) top = page_up(va + msz);
    }
    if (!top) { kprintf("[LINUX]  %s: no loadable segments\n", path); goto out; }

    /* Pass 2: map writable, zero, copy file contents. */
    for (lx_u64 a = MMU_USER_BASE; a < top; a += PAGE)
        if (page_prot[(a - MMU_USER_BASE) / PAGE]) {
            mmu_user_map(a, PAGE, MMU_PROT_READ | MMU_PROT_WRITE);
            kmemset((void *)(uintptr_t)a, 0, PAGE);
        }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = ph + 56 * i;
        if (*(uint32_t *)p != 1) continue;
        lx_u64 off = *(lx_u64 *)(p + 8), va = *(lx_u64 *)(p + 16), fsz = *(lx_u64 *)(p + 32);
        if (fsz && read_at(vfd, off, (void *)(uintptr_t)va, (uint32_t)fsz) != 0) {
            kprintf("[LINUX]  %s: short read\n", path); goto out;
        }
    }
    /* Pass 3: final permissions. */
    for (lx_u64 a = MMU_USER_BASE; a < top; a += PAGE) {
        uint8_t pp = page_prot[(a - MMU_USER_BASE) / PAGE];
        if (pp) mmu_user_map(a, PAGE, pp | MMU_PROT_READ);
    }
    P.brk_start = P.brk_cur = top;
    bad = 0;
out:
    vfs_close(vfd);
    return bad;
}

/* Initial stack: argc, argv[], NULL, envp[], NULL, auxv pairs, AT_NULL,
 * with the strings and AT_RANDOM bytes above. */
static lx_u64 build_stack(int argc, char **argv, lx_u64 entry, lx_u64 phdr, uint32_t phnum,
                          const char *execfn)
{
    static const char *const envp[] = { "PATH=/", "HOME=/", "TERM=vt100", "LANG=C", 0 };
    lx_u64 sp = USER_TOP;
    lx_u64 sargv[32], senvp[8], sexec;
    int nenv = 0;
    if (argc > 32) argc = 32;
#define PUSH_STR(s, dst) do { lx_u64 l_ = kstrlen(s) + 1; sp -= l_; \
        kmemcpy((void *)(uintptr_t)sp, (s), l_); (dst) = sp; } while (0)
    PUSH_STR(execfn, sexec);
    for (int i = argc - 1; i >= 0; i--) PUSH_STR(argv[i], sargv[i]);
    for (; envp[nenv]; nenv++) { }
    for (int i = nenv - 1; i >= 0; i--) PUSH_STR(envp[i], senvp[i]);
    sp -= 16;
    lx_u64 rnd_at = sp;
    for (int i = 0; i < 16; i++) ((uint8_t *)(uintptr_t)sp)[i] = (uint8_t)(rnd() >> 32);
    sp &= ~15UL;
    lx_u64 aux[][2] = {
        { 3, phdr }, { 4, 56 }, { 5, phnum }, { 6, PAGE }, { 9, entry },
        { 11, 0 }, { 12, 0 }, { 13, 0 }, { 14, 0 }, { 16, 0 }, { 17, 100 },
        { 23, 0 }, { 25, rnd_at }, { 31, sexec }, { 0, 0 },
    };
    uint32_t naux = sizeof aux / sizeof aux[0];
    lx_u64 words = 1 + (lx_u64)argc + 1 + (lx_u64)nenv + 1 + 2 * naux;
    sp -= words * 8;
    sp &= ~15UL;
    lx_u64 *w = (lx_u64 *)(uintptr_t)sp;
    *w++ = (lx_u64)argc;
    for (int i = 0; i < argc; i++) *w++ = sargv[i];
    *w++ = 0;
    for (int i = 0; i < nenv; i++) *w++ = senvp[i];
    *w++ = 0;
    for (uint32_t i = 0; i < naux; i++) { *w++ = aux[i][0]; *w++ = aux[i][1]; }
#undef PUSH_STR
    return sp;
}

int linux_run(const char *path, int argc, char **argv)
{
    if (P.active) { kprintf("[LINUX]  a program is already running\n"); return -1; }
    kmemset(&P, 0, sizeof P);
    mmu_user_clear();
    lx_u64 entry, phdr;
    uint32_t phnum;
    if (load_elf(path, &entry, &phdr, &phnum) != 0) { mmu_user_clear(); return -1; }
    if (entry < MMU_USER_BASE || entry >= P.brk_start ||
        !mmu_user_mapped(entry, 4, MMU_PROT_READ)) {
        kprintf("[LINUX]  %s: entry point 0x%x isn't in the image\n", path, (unsigned)entry);
        mmu_user_clear();
        return -1;
    }
    P.mmap_next = MMAP_BASE;
    kstrncpy(P.cwd, "/", sizeof P.cwd);
    for (int i = 0; i < 3; i++) P.fd[i].kind = FD_CONSOLE;
    P.fd[0].flags = 0;
    P.fd[1].flags = P.fd[2].flags = 1;
    mmu_user_map(STACK_BOTTOM, USER_TOP - STACK_BOTTOM, MMU_PROT_READ | MMU_PROT_WRITE);
    kmemset((void *)(uintptr_t)STACK_BOTTOM, 0, USER_TOP - STACK_BOTTOM);  /* nothing left over */
    lx_u64 sp = build_stack(argc, argv, entry, phdr, phnum, path);
    P.active = 1;
    return (int)el0_run(entry, sp, P.kctx);
}
