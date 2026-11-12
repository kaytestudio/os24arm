/**
 * @file linux.c
 * @brief Linux-compatible user mode (see userland/README.md), stage 2:
 *        processes
 *
 * Static AArch64 Linux ELF executables run at EL0, each process in its own
 * address space (mmu.h: the 64 MB user window at 0x60000000 maps to frames
 * of its own) with its own kernel stack, and use Linux system calls
 * (svc #0, number in x8, arguments x0..x5, result or -errno in x0).
 *
 *   user window layout (per process)
 *     0x60000000  program image (PT_LOAD segments, per-page permissions)
 *       ...       brk heap, growing up              (to 0x62000000)
 *     0x62000000  mmap area, growing up             (to 0x63800000)
 *     0x63800000  stack, 8 MB, growing down         (to 0x64000000)
 *
 * Scheduling is cooperative for now: a process runs until it blocks (pipe,
 * wait4, console input, nanosleep) or yields; then the next runnable one
 * runs, round robin. With nothing runnable the shell's context runs the
 * loop in linux_run(). Preemption from the timer interrupt is a later step.
 *
 * Processes: clone (fork: the address space is copied; CLONE_VM |
 * CLONE_VFORK, as musl's posix_spawn uses it: the child borrows the
 * parent's address space, the parent waits until the child execs or
 * exits), execve, exit / exit_group (zombie until reaped), wait4, kill
 * (default action: terminate), getpid / getppid. Open files are shared
 * objects with reference counts (fork and dup share file positions);
 * pipes; close-on-exec. Each process's EL0 thread pointer (tpidr_el0,
 * musl's TLS / errno) is saved and restored on every switch.
 *
 * Files go through the VFS (paths are VFS paths: /BOOT/..., /H/... for
 * MOUNTed volumes); the first process's fds 0-2 are the console. Every
 * user pointer is checked against the current page tables before the
 * kernel touches it (-EFAULT otherwise); a fault in a program kills it.
 *
 * Not yet: threads (CLONE_THREAD), signal handlers, preemption, poll /
 * epoll, sockets, shared mappings, dynamic linking.
 */

#include "linux_abi.h"
#include "mmu.h"
#include "vfs.h"
#include "kio.h"
#include "uart.h"
#include "kprintf.h"

extern void cpu_switch(lx_u64 *save, const lx_u64 *load);
extern void el0_start(void);
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
#define MAX_PROCS       16
#define KSTACK_PAGES    4
#define MAX_FILES       128
#define MAX_PIPES       32
#define PIPE_SIZE       4096
#define ARGBUF          32768

/* errno */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
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
#define EPIPE 32
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39

/* Linux AArch64 open flags (asm-generic values, arm64 directory bits) */
#define L_O_ACCMODE   3
#define L_O_CREAT     0100
#define L_O_EXCL      0200
#define L_O_TRUNC     01000
#define L_O_APPEND    02000
#define L_O_NONBLOCK  04000
#define L_O_DIRECTORY 040000
#define L_O_CLOEXEC   02000000
#define AT_FDCWD      (-100)
#define AT_REMOVEDIR  0x200

/* clone flags */
#define CLONE_VM      0x00000100
#define CLONE_VFORK   0x00004000
#define CLONE_THREAD  0x00010000
#define CLONE_SETTLS  0x00080000

typedef __INT64_TYPE__ s64;

/* ── Open files and pipes ─────────────────────────────────────────────── */
enum { F_FREE, F_CONSOLE, F_FILE, F_DIR, F_PIPE_R, F_PIPE_W };

typedef struct pipe {
    int      used;
    uint8_t  buf[PIPE_SIZE];
    uint32_t head, count;
    int      readers, writers;
} pipe_t;

typedef struct file {
    int      refs;
    int      kind;
    int      vfd;              /* VFS file or directory descriptor */
    int      flags;
    int      dir_pos;          /* synthetic "." / ".." emitted */
    int      have_pending;     /* a readdir result that didn't fit */
    vfs_dirent_t pending;
    pipe_t  *pipe;
    char     path[VFS_PATH_MAX];
} file_t;

static file_t files[MAX_FILES];
static pipe_t pipes[MAX_PIPES];

/* ── Processes ────────────────────────────────────────────────────────── */
enum { P_FREE, P_RUN, P_SLEEP, P_ZOMBIE };

typedef struct proc {
    int      state;
    int      pid, ppid;
    int      wstatus;          /* wait4 encoding, once a zombie */
    int      killed;           /* pending terminating signal */
    void    *wchan;            /* what it sleeps on */
    mmu_as_t *as;
    int      as_borrowed;      /* vfork child using the parent's */
    struct proc *vfork_parent; /* waiting until we exec or exit */
    lx_u64   kctx[13];         /* kernel context (cpu_switch) */
    lx_u64   tpidr;            /* EL0 thread pointer (musl TLS) */
    uint8_t *kstack;
    linux_tf_t *tf;            /* trap frame of the current syscall */
    file_t  *fd[MAX_FDS];
    __UINT64_TYPE__ cloexec;   /* bit per fd */
    lx_u64   brk_start, brk_cur, mmap_next;
    char     cwd[VFS_PATH_MAX];
} proc_t;

static proc_t  procs[MAX_PROCS];
static proc_t *cur;                    /* NULL = the shell's context */
static lx_u64  shell_ctx[13];
static int     next_pid = 1;
static lx_u64  rng;
static uint8_t page_prot[PAGES];       /* loader: combined segment permissions */
static char    argbuf[ARGBUF];         /* execve: argv / envp copied in */

static lx_u64 ticks(void) { lx_u64 v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)); return v; }
static lx_u64 freq(void)  { lx_u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

static void switch_to(proc_t *next)
{
    proc_t *prev = cur;
    if (prev == next) return;
    if (prev) __asm__ volatile("mrs %0, tpidr_el0" : "=r"(prev->tpidr));
    cur = next;
    mmu_as_activate(next ? next->as : 0);
    if (next) __asm__ volatile("msr tpidr_el0, %0" :: "r"(next->tpidr));
    cpu_switch(prev ? prev->kctx : shell_ctx, next ? next->kctx : shell_ctx);
}

static proc_t *pick_runnable(proc_t *after)
{
    int start = after ? (int)(after - procs) + 1 : 0;
    for (int k = 0; k < MAX_PROCS; k++) {
        proc_t *p = &procs[(start + k) % MAX_PROCS];
        if (p->state == P_RUN) return p;
    }
    return 0;
}

/* The current process gives up the CPU (it may stay runnable: a yield). */
static void schedule(void)
{
    proc_t *next = pick_runnable(cur);
    if (next == cur) return;                /* nobody else: carry on */
    switch_to(next);                        /* NULL: back to the shell loop */
}

static void yield(void) { schedule(); }

static void sleep_on(void *ch)
{
    cur->state = P_SLEEP;
    cur->wchan = ch;
    schedule();
}

static void wakeup(void *ch)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_SLEEP && procs[i].wchan == ch) {
            procs[i].state = P_RUN;
            procs[i].wchan = 0;
        }
}

static proc_t *proc_by_pid(int pid)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != P_FREE && procs[i].pid == pid) return &procs[i];
    return 0;
}

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

static lx_u64 rnd(void)
{
    if (!rng) rng = ticks() * 0x9E3779B97F4A7C15ULL | 1;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

/* ── Paths ────────────────────────────────────────────────────────────── */
/* path (kernel string) relative to base -> normalised absolute VFS path */
static int norm_path(const char *base, const char *in, char *out)
{
    char tmp[2 * VFS_PATH_MAX];
    uint32_t n = 0;
    if (in[0] != '/') {
        for (const char *c = base; *c && n < VFS_PATH_MAX; c++) tmp[n++] = *c;
        tmp[n++] = '/';
    }
    for (const char *c = in; *c && n < sizeof tmp - 1; c++) tmp[n++] = *c;
    tmp[n] = 0;
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

static int user_path(int dirfd, lx_u64 up, char *out)
{
    char in[VFS_PATH_MAX];
    int r = copy_in_str(in, up, sizeof in);
    if (r) return r;
    const char *base = cur->cwd;
    if (in[0] != '/' && dirfd != AT_FDCWD) {
        if (dirfd < 0 || dirfd >= MAX_FDS || !cur->fd[dirfd] || cur->fd[dirfd]->kind != F_DIR)
            return -EBADF;
        base = cur->fd[dirfd]->path;
    }
    return norm_path(base, in, out);
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
 * of a line -> end of file, Ctrl-C -> SIGINT. Other processes run while
 * it waits. */
static s64 con_read(char *buf, lx_u64 n)
{
    static char line[256];
    static uint32_t have, pos;
    if (pos >= have) {
        have = pos = 0;
        for (;;) {
            while (!uart_ready()) {
                if (cur->killed) return -EINTR;
                yield();
            }
            char c = uart_getc();
            if (c == '\r' || c == '\n') { con_write("\n", 1); line[have++] = '\n'; break; }
            if (c == 0x04 && !have) return 0;
            if (c == 0x03) { cur->killed = 2; return -EINTR; }
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

/* ── Files and descriptors ────────────────────────────────────────────── */
static file_t *file_new(int kind)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (!files[i].refs) {
            kmemset(&files[i], 0, sizeof files[i]);
            files[i].refs = 1;
            files[i].kind = kind;
            return &files[i];
        }
    return 0;
}

static void file_put(file_t *f)
{
    if (!f || --f->refs > 0) return;
    if (f->kind == F_FILE) vfs_close(f->vfd);
    if (f->kind == F_DIR)  vfs_closedir(f->vfd);
    if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) {
        pipe_t *p = f->pipe;
        if (f->kind == F_PIPE_R) p->readers--; else p->writers--;
        wakeup(p);
        if (!p->readers && !p->writers) p->used = 0;
    }
    f->kind = F_FREE;
}

static int fd_install(file_t *f, int cloexec, int min)
{
    for (int i = min; i < MAX_FDS; i++)
        if (!cur->fd[i]) {
            cur->fd[i] = f;
            if (cloexec) cur->cloexec |= 1ULL << i;
            else cur->cloexec &= ~(1ULL << i);
            return i;
        }
    return -EMFILE;
}

static file_t *fd_get(lx_u64 fd) { return fd < MAX_FDS ? cur->fd[fd] : 0; }

static void fd_close(proc_t *p, int i)
{
    file_put(p->fd[i]);
    p->fd[i] = 0;
    p->cloexec &= ~(1ULL << i);
}

/* struct stat, AArch64 Linux layout (asm-generic). kind: 0 VFS object,
 * 1 console, 2 pipe. */
static void fill_stat(uint8_t *st, const vfs_stat_t *vs, uint32_t kind)
{
    kmemset(st, 0, 128);
    uint32_t mode;
    if (kind == 1)                        mode = 0020000 | 0620;   /* S_IFCHR */
    else if (kind == 2)                   mode = 0010000 | 0600;   /* S_IFIFO */
    else if (vs->attrs & VFS_ATTR_DIR)    mode = 0040000 | 0755;
    else mode = 0100000 | ((vs->attrs & VFS_ATTR_RDONLY) ? 0444 : 0644);
    *(uint32_t *)(st + 16) = mode;                 /* st_mode */
    *(uint32_t *)(st + 20) = 1;                    /* st_nlink */
    *(int32_t *)(st + 56) = 4096;                  /* st_blksize */
    if (!kind) {
        *(s64 *)(st + 48) = vs->size;              /* st_size */
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
    if (want < cur->brk_start || want > BRK_LIMIT) return (s64)cur->brk_cur;
    lx_u64 old = page_up(cur->brk_cur), new = page_up(want);
    if (new > old) {
        if (mmu_as_map(cur->as, old, new - old, MMU_PROT_READ | MMU_PROT_WRITE))
            return (s64)cur->brk_cur;
    } else if (new < old) {
        mmu_as_unmap(cur->as, new, old - new);
    }
    cur->brk_cur = want;
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
        mmu_as_unmap(cur->as, addr, size);           /* fresh pages */
        at = addr;
    } else {
        if (cur->mmap_next + size > MMAP_LIMIT || cur->mmap_next + size < cur->mmap_next)
            return -ENOMEM;
        at = cur->mmap_next;
        cur->mmap_next += size;
    }
    if (mmu_as_map(cur->as, at, size, MMU_PROT_READ | MMU_PROT_WRITE)) return -ENOMEM;
    if (!(flags & MAP_ANON)) {                       /* private file mapping: a copy */
        file_t *f = fd_get((lx_u64)fd);
        if (!f || f->kind != F_FILE) { mmu_as_unmap(cur->as, at, size); return -EBADF; }
        if (vfs_seek(f->vfd, (int32_t)off, SEEK_SET) < 0) { mmu_as_unmap(cur->as, at, size); return -EINVAL; }
        int got = vfs_read(f->vfd, (void *)(uintptr_t)at, (uint32_t)len);
        if (got < 0) { mmu_as_unmap(cur->as, at, size); return verr(got); }
    }
    int p = prot_bits(prot);
    if (p != (MMU_PROT_READ | MMU_PROT_WRITE)) {
        if (p) mmu_as_protect(cur->as, at, size, p);
        else   mmu_as_unmap(cur->as, at, size);       /* PROT_NONE */
    }
    return (s64)at;
}

/* ── ELF loading (into the active address space) ──────────────────────── */
static int read_at(int vfd, lx_u64 off, void *buf, uint32_t n)
{
    if (vfs_seek(vfd, (int32_t)off, SEEK_SET) < 0) return -1;
    return vfs_read(vfd, buf, n) == (int)n ? 0 : -1;
}

/* 0, or -errno (the reason is printed). */
static int load_elf(mmu_as_t *as, const char *path, lx_u64 *entry, lx_u64 *phdr_va,
                    uint32_t *phnum, lx_u64 *top_out)
{
    uint8_t eh[64], ph[MAX_PHDRS * 56];
    int vfd = vfs_open(path, O_RDONLY);
    if (vfd < 0) return verr(vfd);
    int err = -ENOEXEC;
    if (read_at(vfd, 0, eh, 64) != 0 || eh[0] != 0x7F || eh[1] != 'E' || eh[2] != 'L' ||
        eh[3] != 'F') goto out;
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
    if (!top) goto out;

    /* Pass 2: map writable (fresh, zeroed frames), copy file contents. */
    err = -ENOMEM;
    for (lx_u64 a = MMU_USER_BASE; a < top; a += PAGE)
        if (page_prot[(a - MMU_USER_BASE) / PAGE] &&
            mmu_as_map(as, a, PAGE, MMU_PROT_READ | MMU_PROT_WRITE))
            goto out;
    err = -EIO;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = ph + 56 * i;
        if (*(uint32_t *)p != 1) continue;
        lx_u64 off = *(lx_u64 *)(p + 8), va = *(lx_u64 *)(p + 16), fsz = *(lx_u64 *)(p + 32);
        if (fsz && read_at(vfd, off, (void *)(uintptr_t)va, (uint32_t)fsz) != 0) goto out;
    }
    /* Pass 3: final permissions. */
    for (lx_u64 a = MMU_USER_BASE; a < top; a += PAGE) {
        uint8_t pp = page_prot[(a - MMU_USER_BASE) / PAGE];
        if (pp) mmu_as_protect(as, a, PAGE, pp | MMU_PROT_READ);
    }
    if (*entry < MMU_USER_BASE || *entry >= top) { err = -ENOEXEC; goto out; }
    *top_out = top;
    err = 0;
out:
    vfs_close(vfd);
    return err;
}

/* Initial stack (in the active address space): argc, argv[], NULL,
 * envp[], NULL, auxv pairs, AT_NULL, with strings and AT_RANDOM above.
 * args holds argc argument strings then envc environment strings, each
 * NUL-terminated. Returns the stack pointer, or 0 if it doesn't fit. */
static lx_u64 build_stack(const char *args, int argc, int envc, lx_u64 entry, lx_u64 phdr,
                          uint32_t phnum, const char *execfn)
{
    static lx_u64 sptr[512];
    if (argc + envc > 510) return 0;
    lx_u64 sp = USER_TOP, total = 0;
    const char *s = args;
    for (int i = 0; i < argc + envc; i++) { lx_u64 l = kstrlen(s) + 1; total += l; s += l; }
    lx_u64 el = kstrlen(execfn) + 1;
    if (total + el + 8192 > USER_TOP - STACK_BOTTOM) return 0;
    sp -= el;
    kmemcpy((void *)(uintptr_t)sp, execfn, el);
    lx_u64 sexec = sp;
    sp -= total;
    kmemcpy((void *)(uintptr_t)sp, args, total);
    lx_u64 at = sp;
    for (int i = 0; i < argc + envc; i++) {
        sptr[i] = at;
        at += kstrlen((const char *)(uintptr_t)at) + 1;
    }
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
    lx_u64 words = 1 + (lx_u64)argc + 1 + (lx_u64)envc + 1 + 2 * naux;
    sp -= words * 8;
    sp &= ~15UL;
    lx_u64 *w = (lx_u64 *)(uintptr_t)sp;
    *w++ = (lx_u64)argc;
    for (int i = 0; i < argc; i++) *w++ = sptr[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = sptr[argc + i];
    *w++ = 0;
    for (uint32_t i = 0; i < naux; i++) { *w++ = aux[i][0]; *w++ = aux[i][1]; }
    return sp;
}

/* A fresh address space with path loaded and its stack built, left
 * active. On failure the previous address space is active again. */
static int exec_image(const char *path, const char *args, int argc, int envc,
                      mmu_as_t **as_out, lx_u64 *entry, lx_u64 *sp, lx_u64 *top)
{
    mmu_as_t *old = mmu_as_current(), *as = mmu_as_create();
    if (!as) return -ENOMEM;
    mmu_as_activate(as);
    lx_u64 phdr = 0;
    uint32_t phnum = 0;
    int r = load_elf(as, path, entry, &phdr, &phnum, top);
    if (r == 0 && mmu_as_map(as, STACK_BOTTOM, USER_TOP - STACK_BOTTOM,
                             MMU_PROT_READ | MMU_PROT_WRITE))
        r = -ENOMEM;
    if (r == 0 && !(*sp = build_stack(args, argc, envc, *entry, phdr, phnum, path)))
        r = -E2BIG;
    if (r) {
        mmu_as_activate(old);
        mmu_as_destroy(as);
        return r;
    }
    *as_out = as;
    return 0;
}

/* ── Process lifecycle ────────────────────────────────────────────────── */
static proc_t *proc_alloc(void)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_FREE && !procs[i].kstack) {
            proc_t *p = &procs[i];
            kmemset(p, 0, sizeof *p);
            if (!(p->kstack = mmu_frames_alloc(KSTACK_PAGES))) return 0;
            p->pid = next_pid++;
            return p;
        }
    return 0;
}

/* A trap frame at the top of p's kernel stack, and a kernel context that
 * returns to EL0 through it. */
static linux_tf_t *new_frame(proc_t *p)
{
    linux_tf_t *tf = (linux_tf_t *)(p->kstack + KSTACK_PAGES * PAGE - sizeof(linux_tf_t));
    kmemset(p->kctx, 0, sizeof p->kctx);
    p->kctx[11] = (lx_u64)(uintptr_t)el0_start;      /* x30 */
    p->kctx[12] = (lx_u64)(uintptr_t)tf;             /* sp */
    p->tf = tf;
    return tf;
}

static void reap(proc_t *p)
{
    if (p->kstack) mmu_frames_free(p->kstack, KSTACK_PAGES);
    p->kstack = 0;
    p->state = P_FREE;
}

/* Terminate the current process; never returns. wstatus is wait4's
 * encoding: (code << 8), or the signal number. */
static void do_exit(int wstatus)
{
    proc_t *p = cur;
    for (int i = 0; i < MAX_FDS; i++) if (p->fd[i]) fd_close(p, i);
    if (p->vfork_parent) { p->vfork_parent = 0; wakeup(p); }
    for (int i = 0; i < MAX_PROCS; i++)                /* orphans go to pid 1 */
        if (procs[i].state != P_FREE && procs[i].ppid == p->pid) procs[i].ppid = 1;
    mmu_as_activate(0);
    if (!p->as_borrowed) mmu_as_destroy(p->as);
    p->as = 0;
    p->wstatus = wstatus;
    p->state = P_ZOMBIE;
    proc_t *parent = proc_by_pid(p->ppid);
    if (parent) wakeup(parent);
    schedule();                                        /* zombies don't run again */
    for (;;) { }
}

static s64 sys_clone(lx_u64 flags, lx_u64 newsp, lx_u64 tls)
{
    if (flags & CLONE_THREAD) return -ENOSYS;          /* threads: later */
    if ((flags & CLONE_VM) && !(flags & CLONE_VFORK)) return -ENOSYS;
    proc_t *c = proc_alloc();
    if (!c) return -EAGAIN;
    if (flags & CLONE_VM) {
        c->as = cur->as;
        c->as_borrowed = 1;
    } else if (!(c->as = mmu_as_fork(cur->as))) {
        reap(c);
        return -ENOMEM;
    }
    linux_tf_t *tf = new_frame(c);
    kmemcpy(tf, cur->tf, sizeof *tf);
    tf->x[0] = 0;                                       /* child: clone returns 0 */
    if (newsp) tf->sp = newsp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(c->tpidr));
    if (flags & CLONE_SETTLS) c->tpidr = tls;
    c->ppid = cur->pid;
    for (int i = 0; i < MAX_FDS; i++)
        if (cur->fd[i]) { c->fd[i] = cur->fd[i]; c->fd[i]->refs++; }
    c->cloexec = cur->cloexec;
    c->brk_start = cur->brk_start;
    c->brk_cur = cur->brk_cur;
    c->mmap_next = cur->mmap_next;
    kstrncpy(c->cwd, cur->cwd, sizeof c->cwd);
    c->state = P_RUN;
    int pid = c->pid;
    if (flags & CLONE_VFORK) {                          /* until it execs or exits */
        c->vfork_parent = cur;
        while (c->vfork_parent && c->state == P_RUN) sleep_on(c);
        while (c->vfork_parent && c->state == P_SLEEP) sleep_on(c);
    }
    return pid;
}

/* Copy a NULL-terminated user array of strings into argbuf at *used. */
static int copy_in_argv(lx_u64 uv, uint32_t *used, int *count)
{
    *count = 0;
    if (!uv) return 0;
    for (int i = 0; ; i++) {
        if (!uok(uv + 8 * (lx_u64)i, 8, 0)) return -EFAULT;
        lx_u64 s = ((lx_u64 *)(uintptr_t)uv)[i];
        if (!s) return 0;
        if (*count >= 500) return -E2BIG;
        int r = copy_in_str(argbuf + *used, s, ARGBUF - *used);
        if (r) return r == -ERANGE ? -E2BIG : r;
        *used += (uint32_t)kstrlen(argbuf + *used) + 1;
        (*count)++;
    }
}

static s64 sys_execve(lx_u64 upath, lx_u64 uargv, lx_u64 uenvp)
{
    char path[VFS_PATH_MAX];
    int r = user_path(AT_FDCWD, upath, path);
    if (r) return r;
    uint32_t used = 0;
    int argc, envc;
    if ((r = copy_in_argv(uargv, &used, &argc)) || (r = copy_in_argv(uenvp, &used, &envc)))
        return r;
    mmu_as_t *as, *old = cur->as;
    lx_u64 entry, sp, top;
    if ((r = exec_image(path, argbuf, argc, envc, &as, &entry, &sp, &top))) return r;
    /* Point of no return: the new image replaces the old one. */
    if (cur->as_borrowed) {
        cur->as_borrowed = 0;
        if (cur->vfork_parent) { cur->vfork_parent = 0; wakeup(cur); }
    } else {
        mmu_as_destroy(old);
    }
    mmu_as_activate(as);
    cur->as = as;
    cur->brk_start = cur->brk_cur = top;
    cur->mmap_next = MMAP_BASE;
    for (int i = 0; i < MAX_FDS; i++)
        if ((cur->cloexec >> i) & 1) fd_close(cur, i);
    linux_tf_t *tf = cur->tf;
    kmemset(tf, 0, sizeof *tf);
    tf->elr = entry;
    tf->sp = sp;
    tf->spsr = 0x3c0;                                   /* EL0t */
    cur->tpidr = 0;
    __asm__ volatile("msr tpidr_el0, xzr");
    return 0;                                           /* -> x0 of the new program */
}

static s64 sys_wait4(s64 pid, lx_u64 ustatus, lx_u64 options)
{
    const lx_u64 WNOHANG = 1;
    if (ustatus && !uok(ustatus, 4, 1)) return -EFAULT;
    for (;;) {
        int any = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            proc_t *c = &procs[i];
            if (c->state == P_FREE || c->ppid != cur->pid || c == cur) continue;
            if (pid > 0 && c->pid != pid) continue;
            any = 1;
            if (c->state == P_ZOMBIE) {
                int cp = c->pid;
                if (ustatus) *(int32_t *)(uintptr_t)ustatus = c->wstatus;
                reap(c);
                return cp;
            }
        }
        if (!any) return -ECHILD;
        if (options & WNOHANG) return 0;
        if (cur->killed) return -EINTR;
        sleep_on(cur);
    }
}

static s64 sys_kill(s64 pid, lx_u64 sig)
{
    if (sig > 64) return -EINVAL;
    int hits = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *p = &procs[i];
        if (p->state == P_FREE || p->state == P_ZOMBIE) continue;
        if (pid > 0 && p->pid != pid) continue;     /* pid 0 / -1: everyone (one group) */
        if (pid < -1 && p->pid != -pid) continue;
        hits++;
        if (!sig) continue;
        /* No handlers yet: a signal's default action applies - terminate,
         * except for the ones whose default is to be ignored. */
        if (sig == 17 || sig == 23 || sig == 28) continue;   /* SIGCHLD, SIGURG, SIGWINCH */
        p->killed = (int)sig;
        if (p->state == P_SLEEP) { p->state = P_RUN; p->wchan = 0; }
    }
    return hits ? 0 : -ESRCH;
}

/* ── Pipes ────────────────────────────────────────────────────────────── */
static s64 sys_pipe2(lx_u64 ufds, lx_u64 flags)
{
    if (!uok(ufds, 8, 1)) return -EFAULT;
    pipe_t *p = 0;
    for (int i = 0; i < MAX_PIPES; i++) if (!pipes[i].used) { p = &pipes[i]; break; }
    if (!p) return -ENOMEM;
    file_t *r = file_new(F_PIPE_R);
    if (!r) return -ENOMEM;
    file_t *w = file_new(F_PIPE_W);
    if (!w) { r->refs = 0; r->kind = F_FREE; return -ENOMEM; }
    kmemset(p, 0, sizeof *p);
    p->used = 1;
    p->readers = p->writers = 1;
    r->pipe = w->pipe = p;
    r->flags = (int)(flags & L_O_NONBLOCK);
    w->flags = (int)(flags & L_O_NONBLOCK) | 1;
    int cl = (flags & L_O_CLOEXEC) != 0;
    int fr = fd_install(r, cl, 0);
    if (fr < 0) { file_put(r); file_put(w); return -EMFILE; }
    int fw = fd_install(w, cl, 0);
    if (fw < 0) { fd_close(cur, fr); file_put(w); return -EMFILE; }
    ((int32_t *)(uintptr_t)ufds)[0] = fr;
    ((int32_t *)(uintptr_t)ufds)[1] = fw;
    return 0;
}

static s64 pipe_read(file_t *f, uint8_t *buf, lx_u64 n)
{
    pipe_t *p = f->pipe;
    while (!p->count) {
        if (!p->writers) return 0;                       /* end of file */
        if (f->flags & L_O_NONBLOCK) return -EAGAIN;
        if (cur->killed) return -EINTR;
        sleep_on(p);
    }
    lx_u64 k = 0;
    while (k < n && p->count) {
        buf[k++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_SIZE;
        p->count--;
    }
    wakeup(p);
    return (s64)k;
}

static s64 pipe_write(file_t *f, const uint8_t *buf, lx_u64 n)
{
    pipe_t *p = f->pipe;
    lx_u64 k = 0;
    while (k < n) {
        if (!p->readers) return k ? (s64)k : -EPIPE;     /* no SIGPIPE yet */
        if (p->count == PIPE_SIZE) {
            if (f->flags & L_O_NONBLOCK) return k ? (s64)k : -EAGAIN;
            if (cur->killed) return k ? (s64)k : -EINTR;
            wakeup(p);
            sleep_on(p);
            continue;
        }
        p->buf[(p->head + p->count) % PIPE_SIZE] = buf[k++];
        p->count++;
    }
    wakeup(p);
    return (s64)k;
}

/* ── File system calls ────────────────────────────────────────────────── */
static s64 sys_openat(int dirfd, lx_u64 up, lx_u64 flags)
{
    char path[VFS_PATH_MAX];
    int r = user_path(dirfd, up, path);
    if (r) return r;
    vfs_stat_t st;
    kmemset(&st, 0, sizeof st);
    int is_root = path[0] == '/' && !path[1];
    int exists = is_root || vfs_stat(path, &st) == VFS_OK;
    if (is_root) st.attrs = VFS_ATTR_DIR;
    if (exists && (flags & L_O_CREAT) && (flags & L_O_EXCL)) return -EEXIST;
    int cl = (flags & L_O_CLOEXEC) != 0;
    if ((flags & L_O_DIRECTORY) || (exists && (st.attrs & VFS_ATTR_DIR))) {
        if (!exists) return -ENOENT;
        if (!(st.attrs & VFS_ATTR_DIR)) return -ENOTDIR;
        if ((flags & L_O_ACCMODE) != 0) return -EISDIR;
        file_t *f = file_new(F_DIR);
        if (!f) return -EMFILE;
        int d = vfs_opendir(path);
        if (d < 0) { f->refs = 0; f->kind = F_FREE; return verr(d); }
        f->vfd = d;
        kstrncpy(f->path, path, sizeof f->path);
        int fd = fd_install(f, cl, 0);
        if (fd < 0) file_put(f);
        return fd;
    }
    file_t *f = file_new(F_FILE);
    if (!f) return -EMFILE;
    int vf = vfs_open(path, (int)(flags & (L_O_ACCMODE | L_O_CREAT | L_O_TRUNC | L_O_APPEND)));
    if (vf < 0) { f->refs = 0; f->kind = F_FREE; return verr(vf); }
    f->vfd = vf;
    f->flags = (int)flags;
    kstrncpy(f->path, path, sizeof f->path);
    int fd = fd_install(f, cl, 0);
    if (fd < 0) file_put(f);
    return fd;
}

static s64 sys_read(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!n) return 0;
    if (!uok(buf, n, 1)) return -EFAULT;
    uint8_t *b = (uint8_t *)(uintptr_t)buf;
    switch (f->kind) {
    case F_CONSOLE: return con_read((char *)b, n);
    case F_PIPE_R:  return pipe_read(f, b, n);
    case F_PIPE_W:  return -EBADF;
    case F_DIR:     return -EISDIR;
    default: {
        int r = vfs_read(f->vfd, b, (uint32_t)(n > 0x40000000 ? 0x40000000 : n));
        return r < 0 ? verr(r) : r;
    }
    }
}

static s64 sys_write(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!n) return 0;
    if (!uok(buf, n, 0)) return -EFAULT;
    const uint8_t *b = (const uint8_t *)(uintptr_t)buf;
    switch (f->kind) {
    case F_CONSOLE: con_write((const char *)b, n); return (s64)n;
    case F_PIPE_W:  return pipe_write(f, b, n);
    case F_PIPE_R:  return -EBADF;
    case F_DIR:     return -EISDIR;
    default: {
        int r = vfs_write(f->vfd, b, (uint32_t)(n > 0x40000000 ? 0x40000000 : n));
        return r < 0 ? verr(r) : r;
    }
    }
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
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_FILE) return -ESPIPE;
    if (whence > 2 || off > 0x7FFFFFFF || off < -0x7FFFFFFFLL) return -EINVAL;
    int r = vfs_seek(f->vfd, (int32_t)off, (int)whence);
    return r < 0 ? verr(r) : r;
}

static s64 sys_fstatat(int dirfd, lx_u64 up, lx_u64 stp, int by_fd)
{
    if (!uok(stp, 128, 1)) return -EFAULT;
    vfs_stat_t vs;
    kmemset(&vs, 0, sizeof vs);
    uint32_t kind = 0;
    if (by_fd) {
        file_t *f = fd_get((lx_u64)dirfd);
        if (!f) return -EBADF;
        if (f->kind == F_CONSOLE) kind = 1;
        else if (f->kind == F_PIPE_R || f->kind == F_PIPE_W) kind = 2;
        else if (f->path[0] == '/' && !f->path[1]) vs.attrs = VFS_ATTR_DIR;
        else {
            int r = vfs_stat(f->path, &vs);
            if (r != VFS_OK) return verr(r);
        }
    } else {
        char path[VFS_PATH_MAX];
        int r = user_path(dirfd, up, path);
        if (r) return r;
        if (path[0] == '/' && !path[1]) vs.attrs = VFS_ATTR_DIR;
        else if ((r = vfs_stat(path, &vs)) != VFS_OK) return verr(r);
    }
    fill_stat((uint8_t *)(uintptr_t)stp, &vs, kind);
    return 0;
}

static s64 sys_getdents64(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_DIR) return -ENOTDIR;
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
            vfs_stat_t vs;
            dtype = (norm_path(f->path, name, p) == 0 && vfs_stat(p, &vs) == VFS_OK &&
                     (vs.attrs & VFS_ATTR_DIR)) ? 4 : 8;
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
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_CONSOLE) return -ENOTTY;
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

static s64 sys_dup3(lx_u64 old, lx_u64 new, lx_u64 flags, int is_dup3)
{
    file_t *f = fd_get(old);
    if (!f) return -EBADF;
    if (!is_dup3) {                                    /* dup */
        int fd = fd_install(f, 0, 0);
        if (fd >= 0) f->refs++;
        return fd;
    }
    if (new >= MAX_FDS || new == old) return -EINVAL;
    f->refs++;                                         /* before closing new: old may == f */
    if (cur->fd[new]) fd_close(cur, (int)new);
    cur->fd[new] = f;
    if (flags & L_O_CLOEXEC) cur->cloexec |= 1ULL << new;
    return (s64)new;
}

static s64 sys_fcntl(lx_u64 fd, lx_u64 cmd, lx_u64 arg)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case 0: case 1030: {                               /* F_DUPFD, F_DUPFD_CLOEXEC */
        if (arg >= MAX_FDS) return -EINVAL;
        int n = fd_install(f, cmd == 1030, (int)arg);
        if (n >= 0) f->refs++;
        return n;
    }
    case 1: return (cur->cloexec >> fd) & 1;           /* F_GETFD */
    case 2:                                            /* F_SETFD */
        if (arg & 1) cur->cloexec |= 1ULL << fd; else cur->cloexec &= ~(1ULL << fd);
        return 0;
    case 3: return f->flags & (L_O_ACCMODE | L_O_APPEND | L_O_NONBLOCK);   /* F_GETFL */
    case 4: f->flags = (int)((f->flags & ~L_O_NONBLOCK) | (arg & L_O_NONBLOCK)); return 0;
    default: return 0;
    }
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
    while (ticks() < end) {
        if (cur->killed) return -EINTR;
        if (usbd_daemon) usbd_daemon();               /* keep USB alive */
        yield();
    }
    return 0;
}

static s64 sys_uname(lx_u64 up)
{
    if (!uok(up, 6 * 65, 1)) return -EFAULT;
    static const char *const f[6] = { "Linux", "os2warp", "6.1.0-os2warp-linux-abi",
                                      "#2 stage2", "aarch64", "(none)" };
    char *u = (char *)(uintptr_t)up;
    kmemset(u, 0, 6 * 65);
    for (int i = 0; i < 6; i++) kstrncpy(u + 65 * i, f[i], 64);
    return 0;
}

static s64 linux_syscall(linux_tf_t *tf)
{
    lx_u64 a0 = tf->x[0], a1 = tf->x[1], a2 = tf->x[2], a3 = tf->x[3], a4 = tf->x[4], a5 = tf->x[5];
    switch (tf->x[8]) {
    case 17: {                                          /* getcwd */
        lx_u64 n = kstrlen(cur->cwd) + 1;
        if (a1 < n) return -ERANGE;
        if (!uok(a0, n, 1)) return -EFAULT;
        kmemcpy((void *)(uintptr_t)a0, cur->cwd, n);
        return (s64)n;
    }
    case 23: return sys_dup3(a0, 0, 0, 0);
    case 24: return sys_dup3(a0, a1, a2, 1);
    case 25: return sys_fcntl(a0, a1, a2);
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
        kstrncpy(cur->cwd, p, sizeof cur->cwd);
        return 0;
    }
    case 56: return sys_openat((int)a0, a1, a2);
    case 57:                                            /* close */
        if (!fd_get(a0)) return -EBADF;
        fd_close(cur, (int)a0);
        return 0;
    case 59: return sys_pipe2(a0, a1);
    case 61: return sys_getdents64(a0, a1, a2);
    case 62: return sys_lseek(a0, (s64)a1, a2);
    case 63: return sys_read(a0, a1, a2);
    case 64: return sys_write(a0, a1, a2);
    case 65: return sys_iov(a0, a1, a2, 0);
    case 66: return sys_iov(a0, a1, a2, 1);
    case 79: return sys_fstatat((int)a0, a1, a2, 0);
    case 80: return sys_fstatat((int)a0, 0, a1, 1);
    case 93: case 94: do_exit((int)((a0 & 0xFF) << 8)); return 0;   /* exit, exit_group */
    case 96: return cur->pid;                           /* set_tid_address -> tid */
    case 98: return -ENOSYS;                            /* futex: no threads yet */
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
    case 124: yield(); return 0;                        /* sched_yield */
    case 129: return sys_kill((s64)a0, a1);             /* kill */
    case 130: return sys_kill((s64)a0, a1);             /* tkill (tid == pid) */
    case 131: return sys_kill((s64)a1, a2);             /* tgkill */
    case 134: case 135: return 0;                      /* rt_sigaction, rt_sigprocmask */
    case 160: return sys_uname(a0);
    case 172: return cur->pid;                          /* getpid */
    case 173: return cur->ppid;                         /* getppid */
    case 178: return cur->pid;                          /* gettid */
    case 174: case 175: case 176: case 177: return 0;  /* getuid, geteuid, getgid, getegid */
    case 214: return sys_brk(a0);
    case 215:                                           /* munmap */
        if ((a0 & (PAGE - 1)) || a0 < MMU_USER_BASE || a0 + a1 > STACK_BOTTOM) return -EINVAL;
        mmu_as_unmap(cur->as, a0, a1);
        return 0;
    case 216: {                                         /* mremap */
        lx_u64 old = page_up(a1), new = page_up(a2);
        if ((a0 & (PAGE - 1)) || !new || a0 < MMAP_BASE || a0 + old > MMAP_LIMIT ||
            !mmu_user_mapped(a0, old, MMU_PROT_READ | MMU_PROT_WRITE))
            return -EINVAL;
        if (new <= old) { mmu_as_unmap(cur->as, a0 + new, old - new); return (s64)a0; }
        if (a0 + old == cur->mmap_next && a0 + new <= MMAP_LIMIT) {     /* grow in place */
            if (mmu_as_map(cur->as, a0 + old, new - old, MMU_PROT_READ | MMU_PROT_WRITE))
                return -ENOMEM;
            cur->mmap_next = a0 + new;
            return (s64)a0;
        }
        if (!(a3 & 1)) return -ENOMEM;                  /* MREMAP_MAYMOVE */
        s64 at = sys_mmap(0, new, 3, 0x22, -1, 0);      /* RW, private anonymous */
        if (at < 0) return at;
        kmemcpy((void *)(uintptr_t)at, (void *)(uintptr_t)a0, old);
        mmu_as_unmap(cur->as, a0, old);
        return at;
    }
    case 220: return sys_clone(a0, a1, a3);             /* clone(flags, sp, ptid, tls, ctid) */
    case 221: return sys_execve(a0, a1, a2);
    case 222: return sys_mmap(a0, a1, a2, a3, (s64)a4, a5);
    case 226: {                                         /* mprotect */
        if ((a0 & (PAGE - 1)) || a0 < MMU_USER_BASE || a0 + a1 > USER_TOP) return -EINVAL;
        if (!mmu_user_mapped(a0, a1, MMU_PROT_READ)) return -ENOMEM;
        int p = prot_bits(a2);
        if ((p & MMU_PROT_WRITE) && (p & MMU_PROT_EXEC)) return -EPERM;   /* W^X */
        if (p) mmu_as_protect(cur->as, a0, a1, p);
        return 0;
    }
    case 233: return 0;                                 /* madvise */
    case 260: return sys_wait4((s64)a0, a1, a2);
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
        kprintf("[LINUX]  pid %d: unimplemented syscall %u (pc 0x%x)\n", cur->pid,
                (unsigned)tf->x[8], (unsigned)(tf->elr & 0xFFFFFFFF));
        return -ENOSYS;
    }
}

void linux_el0_trap(linux_tf_t *tf)
{
    lx_u64 esr, far;
    __asm__ volatile("mrs %0, esr_el1\n\tmrs %1, far_el1" : "=r"(esr), "=r"(far));
    cur->tf = tf;
    uint32_t ec = (uint32_t)(esr >> 26);
    if (ec == 0x15) {                                   /* SVC from AArch64 */
        s64 r = linux_syscall(tf);
        cur->tf->x[0] = (lx_u64)r;
        if (cur->killed) do_exit(cur->killed);
        return;
    }
    const char *what = ec == 0x24 ? "data abort" : ec == 0x20 ? "instruction abort" :
                       ec == 0x22 ? "PC alignment fault" : ec == 0x26 ? "SP alignment fault" :
                       ec == 0x00 ? "undefined instruction" : "exception";
    kprintf("[LINUX]  pid %d: %s at pc 0x%x, address 0x%x (ESR 0x%x) - killed (SIGSEGV)\n",
            cur->pid, what, (unsigned)(tf->elr & 0xFFFFFFFF), (unsigned)(far & 0xFFFFFFFF),
            (unsigned)esr);
    do_exit(ec == 0x00 ? 4 : 11);                       /* SIGILL / SIGSEGV */
}

/* ── Entry from the shell ─────────────────────────────────────────────── */
int linux_run(const char *path, int argc, char **argv)
{
    if (cur) { kprintf("[LINUX]  already running\n"); return -1; }
    static const char *const envp[] = { "PATH=/", "HOME=/", "TERM=vt100", "LANG=C", 0 };
    uint32_t used = 0;
    int envc = 0;
    if (argc > 64) argc = 64;
    for (int i = 0; i < argc; i++) {
        uint32_t l = (uint32_t)kstrlen(argv[i]) + 1;
        if (used + l > ARGBUF - 256) return -1;
        kmemcpy(argbuf + used, argv[i], l);
        used += l;
    }
    for (; envp[envc]; envc++) {
        uint32_t l = (uint32_t)kstrlen(envp[envc]) + 1;
        kmemcpy(argbuf + used, envp[envc], l);
        used += l;
    }
    uint32_t free0 = mmu_frames_free_count();
    proc_t *init = proc_alloc();
    if (!init) { kprintf("[LINUX]  out of memory\n"); return -1; }
    mmu_as_t *as;
    lx_u64 entry, sp, top;
    int r = exec_image(path, argbuf, argc, envc, &as, &entry, &sp, &top);
    mmu_as_activate(0);
    if (r) {
        kprintf("[LINUX]  %s: can't run (errno %d)\n", path, -r);
        reap(init);
        next_pid = 1;
        return -1;
    }
    init->as = as;
    init->ppid = 0;
    init->brk_start = init->brk_cur = top;
    init->mmap_next = MMAP_BASE;
    kstrncpy(init->cwd, "/", sizeof init->cwd);
    for (int i = 0; i < 3; i++) {
        file_t *f = file_new(F_CONSOLE);
        if (!f) break;
        f->flags = i ? 1 : 0;
        init->fd[i] = f;
    }
    linux_tf_t *tf = new_frame(init);
    kmemset(tf, 0, sizeof *tf);
    tf->elr = entry;
    tf->sp = sp;
    tf->spsr = 0x3c0;                                   /* EL0t */
    init->state = P_RUN;

    /* The shell's context is the idle loop: run whatever is runnable
     * until the first process has exited. */
    for (;;) {
        if (init->state == P_ZOMBIE) break;
        proc_t *p = pick_runnable(0);
        if (p) { switch_to(p); continue; }
        kprintf("[LINUX]  every process is blocked - killing them\n");
        for (int i = 0; i < MAX_PROCS; i++)
            if (procs[i].state == P_SLEEP) { procs[i].killed = 9; procs[i].state = P_RUN; }
    }
    int status = init->wstatus;
    /* Whatever is left: kill it, let it exit, reap everything. */
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_RUN || procs[i].state == P_SLEEP) {
            procs[i].killed = 9;
            procs[i].state = P_RUN;
        }
    for (proc_t *p; (p = pick_runnable(0)) != 0; ) switch_to(p);
    for (int i = 0; i < MAX_PROCS; i++) if (procs[i].state == P_ZOMBIE) reap(&procs[i]);
    mmu_as_activate(0);
    next_pid = 1;
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].refs) { kprintf("[LINUX]  leaked open file %d\n", i); files[i].refs = 1; file_put(&files[i]); }
    if (mmu_frames_free_count() != free0)
        kprintf("[LINUX]  warning: %d page frames not returned\n",
                (int)free0 - (int)mmu_frames_free_count());
    return (status & 0x7F) ? 128 + (status & 0x7F) : (status >> 8) & 0xFF;
}
