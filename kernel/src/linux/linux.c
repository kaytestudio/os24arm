/**
 * @file linux.c
 * @brief Linux-compatible user mode (see userland/README.md), stages 2-3:
 *        processes, signals, event and IPC primitives
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
 * Scheduling: a process runs until it blocks (pipe, socket, poll, wait4,
 * console input, sleep), yields, or its time slice ends - the EL1
 * physical timer interrupts EL0 every SLICE_MS (the kernel itself always
 * runs with interrupts masked, so the tick only ever arrives from EL0) -
 * and then the next runnable one runs, round robin. Blocking waits can
 * have a deadline (timeouts, timers, sleeps); with nothing runnable the
 * shell's context runs the idle loop in linux_run() until a deadline
 * passes.
 *
 * Processes: clone (fork: the address space is copied, MAP_SHARED pages
 * are shared; CLONE_VM | CLONE_VFORK, as musl's posix_spawn uses it: the
 * child borrows the parent's address space, the parent waits until the
 * child execs or exits), execve, exit / exit_group (zombie until reaped,
 * SIGCHLD to the parent), wait4, getpid / getppid.
 *
 * Threads: a process is a group_t (address space, open files, signal
 * handlers, cwd) with one or more threads (proc_t: what's scheduled, with
 * its own kernel stack, TLS pointer, signal mask and pending set).
 * clone(CLONE_THREAD), futex, set_tid_address / CLONE_CHILD_CLEARTID
 * (pthread_join), exit (a thread) vs exit_group; a fatal signal or execve
 * takes the other threads with it.
 *
 * Signals: rt_sigaction, rt_sigprocmask, kill / tkill / tgkill, handlers
 * run on the user stack with a Linux-layout rt_sigframe (siginfo +
 * ucontext with the FP/SIMD record) and return through rt_sigreturn
 * (musl's SA_RESTORER trampoline); SA_RESTART, SA_NODEFER, SA_RESETHAND;
 * default actions (terminate, or ignore for SIGCHLD/SIGURG/SIGWINCH/
 * SIGCONT; stop signals are ignored). Blocked signals stay pending, which
 * is what signalfd reads. Interrupted waits return -EINTR, or restart.
 *
 * Files: VFS files and directories (paths are VFS paths: /BOOT/...,
 * /H/... for MOUNTed volumes, /tmp is a RAM file system), the console
 * (the first process's fds 0-2), /dev/null, /dev/zero, /dev/urandom,
 * pipes, eventfd, timerfd, signalfd, epoll, memfd and POSIX shared memory
 * (/dev/shm/<name>, shm_open; both shareable with mmap(MAP_SHARED)), AF_UNIX stream sockets (bind/listen/accept/connect,
 * socketpair, SCM_RIGHTS fd passing, SO_PEERCRED). Open files are shared
 * objects with reference counts (fork and dup share them).
 *
 * Every user pointer is checked against the current page tables before
 * the kernel touches it (-EFAULT otherwise); a fault in a program kills
 * it (SIGSEGV / SIGILL; handlers for those aren't run).
 *
 * Terminals: the console and pseudo-terminals (/dev/ptmx, /dev/pts/N) are
 * tty_t's with one line discipline (termios: canonical editing, raw mode,
 * echo, signals to the foreground process group), window sizes, process
 * groups, sessions and controlling terminals. /proc/self/exe and
 * /proc/self/fd/N exist as links (readlink, and exe as a path).
 *
 * Not yet: datagram sockets, futexes shared between processes, the rest
 * of /proc, dynamic linking.
 */

#include "linux_abi.h"
#include "mmu.h"
#include "vfs.h"
#include "ramfs.h"
#include "kio.h"
#include "uart.h"
#include "kprintf.h"
#include "gicv3.h"

extern void cpu_switch(lx_u64 *save, const lx_u64 *load);
extern void el0_start(void);
extern uint32_t kernel_time_seconds(void) __attribute__((weak));
extern void usbd_daemon(void) __attribute__((weak));
/* Display (drivers/video/virtiofb.c) and input (drivers/usb) for /dev/fb0
 * and /dev/input/event*; weak, so boards without them still link. */
extern int       fb_init_driver(void) __attribute__((weak));
extern uint32_t *fb_front(void) __attribute__((weak));
extern void      fb_flush(int x, int y, int w, int h) __attribute__((weak));
extern void      fb_get_info(int *w, int *h, int *bpp) __attribute__((weak));
extern int       usbkbd_poll_raw(int *usage, int *down) __attribute__((weak));
extern void      usbkbd_grab(int on) __attribute__((weak));
typedef struct { int absolute, x, y, dx, dy, wheel; unsigned buttons; } usbmouse_ev_t;
extern int       usbmouse_poll(usbmouse_ev_t *ev) __attribute__((weak));

#define PAGE            4096UL
#define USER_TOP        (MMU_USER_BASE + MMU_USER_SIZE)
#define BRK_LIMIT       0x62000000UL
#define MMAP_BASE       0x62000000UL
#define MMAP_LIMIT      0x63800000UL
#define STACK_BOTTOM    0x63800000UL
#define MAX_FDS         64
#define MAX_PHDRS       16
#define PAGES           (MMU_USER_SIZE / PAGE)
#define MAX_PROCS       48                     /* threads */
#define MAX_GROUPS      16                     /* processes */
#define KSTACK_PAGES    16                     /* 64 KB: btrfs writes need a lot */
#define KSTACK_CANARY   0x5354414B43414E59ULL  /* bottom of every kernel stack */
#define MAX_FILES       160
#define MAX_PIPES       32
#define PIPE_SIZE       4096
#define ARGBUF          32768
#define MAX_EPOLLS      16
#define EPOLL_ITEMS     64
#define MAX_MEMOBJS     64
#define MEMFD_MAX       (32UL << 20)
#define MAX_SOCKS       48
#define SOCK_BUF_PAGES  16                     /* 64 KB receive buffer */
#define SOCK_BUF        (SOCK_BUF_PAGES * PAGE)
#define SOCK_FDQ        16                     /* SCM_RIGHTS messages in flight */
#define SCM_MAX         28                     /* fds per message (Linux: 253) */
#define BACKLOG         8
#define NSIG            64

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
#define ENODEV 19
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EPIPE 32
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39
#define ENOTSOCK 88
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define ECONNRESET 104
#define EISCONN 106
#define ENOTCONN 107
#define ECONNREFUSED 111

/* Linux AArch64 open flags (asm-generic values, arm64 directory bits) */
#define L_O_ACCMODE   3
#define L_O_RDWR      2
#define L_O_CREAT     0100
#define L_O_EXCL      0200
#define L_O_TRUNC     01000
#define L_O_APPEND    02000
#define L_O_NONBLOCK  04000
#define L_O_DIRECTORY 040000
#define L_O_CLOEXEC   02000000
#define AT_FDCWD      (-100)
#define AT_REMOVEDIR  0x200
#define AT_EMPTY_PATH 0x1000

/* clone flags */
#define CLONE_VM      0x00000100
#define CLONE_VFORK   0x00004000
#define CLONE_THREAD  0x00010000
#define CLONE_SIGHAND 0x00000800
#define CLONE_SETTLS  0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define ETIMEDOUT 110

/* poll / epoll events */
#define PIN     0x001
#define PPRI    0x002
#define POUT    0x004
#define PERR    0x008
#define PHUP    0x010
#define PNVAL   0x020
#define PRDNORM 0x040
#define PWRNORM 0x100
#define PRDHUP  0x2000
#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)

/* signals */
#define SIGINT   2
#define SIGILL   4
#define SIGKILL  9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL
#define SA_RESTART   0x10000000UL
#define SA_RESTORER  0x04000000UL
#define SIG_DFL      0
#define SIG_IGN      1
#define SIGBIT(s)    (1ULL << ((s) - 1))
#define UNBLOCKABLE  (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

/* socket flags */
#define MSG_PEEK      0x2
#define MSG_CTRUNC    0x8
#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

typedef __INT64_TYPE__ s64;

/* ── Objects behind file descriptors ──────────────────────────────────── */
enum { F_FREE, F_CONSOLE /* a tty: the console or a pty's slave */, F_PTMX, F_FILE, F_DIR, F_PIPE_R, F_PIPE_W, F_NULL, F_ZERO, F_RANDOM,
       F_EVENTFD, F_TIMERFD, F_SIGNALFD, F_EPOLL, F_MEMFD, F_SOCK, F_FB, F_EVDEV };

typedef struct pipe {
    int      used;
    uint8_t  buf[PIPE_SIZE];
    uint32_t head, count;
    int      readers, writers;
} pipe_t;

typedef struct memobj {               /* memfd / shared anonymous memory */
    int      refs;
    lx_u64   size;
    uint32_t npages, cap;             /* pages used, page-pointer slots */
    void   **pages;                   /* frames (one reference each) */
    uint32_t table_frames;
} memobj_t;

struct file;
typedef struct {
    struct file *f;
    int      fd;
    uint32_t events;
    lx_u64   data;
    int      disarmed;                /* EPOLLONESHOT item reported; until EPOLL_CTL_MOD */
} epitem_t;

typedef struct {
    int      used;
    int      n;
    epitem_t it[EPOLL_ITEMS];
} epoll_t;

typedef struct {
    lx_u64   start, end;              /* stream positions the fds came with */
    int      n;
    struct file *f[SCM_MAX];
} fdmsg_t;

enum { S_NEW, S_BOUND, S_LISTEN, S_CONN, S_PENDING };

typedef struct sock {
    int      used;
    int      state;
    struct sock *peer;
    int      peer_gone;               /* peer closed or shut down writing: EOF after data */
    int      shut_wr;                 /* we shut down writing */
    uint8_t *buf;                     /* receive ring */
    lx_u64   rd, wr;                  /* stream positions */
    fdmsg_t  fdq[SOCK_FDQ];
    int      nfdq;
    char     name[108];               /* bound name (sun_path), abstract: name[0] = 0 */
    uint32_t namelen;
    struct sock *backlog[BACKLOG];
    int      nbacklog;
    int      cred_pid;                /* peer's pid (SO_PEERCRED) */
    int      own_pid;
} sock_t;

typedef struct file {
    int      refs;
    int      kind;
    int      vfd;                     /* VFS file or directory descriptor */
    int      flags;                   /* O_ACCMODE | O_APPEND | O_NONBLOCK, and 1 = writer */
    int      dir_pos;                 /* synthetic "." / ".." emitted */
    int      have_pending;            /* a readdir result that didn't fit */
    vfs_dirent_t pending;
    pipe_t  *pipe;
    lx_u64   pos;                     /* memfd position */
    lx_u64   count;                   /* eventfd counter; timerfd expirations */
    int      semaphore;               /* eventfd */
    int      clock;                   /* timerfd */
    lx_u64   next, interval;          /* timerfd, in counter ticks; next 0 = disarmed */
    lx_u64   sigmask;                 /* signalfd */
    epoll_t *ep;
    memobj_t *mem;
    sock_t  *sock;
    struct tty *tty;                  /* F_CONSOLE, F_PTMX */
    char     path[VFS_PATH_MAX];
} file_t;

static file_t   files[MAX_FILES];
static pipe_t   pipes[MAX_PIPES];
static epoll_t  epolls[MAX_EPOLLS];
static memobj_t memobjs[MAX_MEMOBJS];
static sock_t   socks[MAX_SOCKS];

/* ── Processes (thread groups) and threads ────────────────────────────── */
/* A process is a group_t: what its threads share. A thread is a proc_t:
 * what's scheduled. getpid() is the group's pid (its first thread's
 * tid), gettid() the thread's own. */
enum { P_FREE, P_RUN, P_SLEEP, P_DEAD };    /* P_DEAD: exited, kernel stack not freed yet */
enum { G_FREE, G_LIVE, G_ZOMBIE };

typedef struct {
    lx_u64 handler, flags, restorer, mask;
} sigact_t;

typedef struct group {
    int      state;
    int      pid, ppid;
    int      wstatus;          /* wait4 encoding, once a zombie */
    int      nthreads;
    int      exiting;          /* exit_group / fatal signal: every thread goes */
    int      exec_kill;        /* execve: the other threads go */
    int      exit_status;      /* wstatus, when the last thread is gone */
    mmu_as_t *as;
    int      as_borrowed;      /* vfork child using the parent's */
    file_t  *fd[MAX_FDS];
    __UINT64_TYPE__ cloexec;   /* bit per fd */
    lx_u64   brk_start, brk_cur, mmap_next;
    sigact_t sa[NSIG];
    char     cwd[VFS_PATH_MAX];
    char     exe[VFS_PATH_MAX];    /* /proc/self/exe */
    int      pgid, sid;            /* process group, session */
    struct tty *ctty;              /* controlling terminal */
} group_t;

typedef struct proc {
    int      state;
    int      pid;              /* thread id */
    group_t *g;
    void    *wchan;            /* what it sleeps on */
    lx_u64   deadline;         /* wake by then (counter ticks), 0 = none */
    struct proc *vfork_parent; /* waiting until we exec or exit */
    lx_u64   kctx[13];         /* kernel context (cpu_switch) */
    lx_u64   tpidr;            /* EL0 thread pointer (musl TLS) */
    uint8_t *kstack;
    linux_tf_t *tf;            /* trap frame of the current syscall */
    lx_u64   sig_pending, sig_blocked;
    lx_u64   saved_mask;       /* ppoll / pselect / epoll_pwait / sigsuspend: */
    int      restore_mask;     /* mask to restore on the way out */
    int      sig_from[NSIG];   /* sender pid, for siginfo */
    lx_u64   clear_tid;        /* CLONE_CHILD_CLEARTID / set_tid_address */
    mmu_as_t *futex_as;        /* futex wait: (address space, address), */
    lx_u64   futex_addr;       /* 0 once woken */
    uint32_t futex_bits;
} proc_t;

static proc_t  procs[MAX_PROCS];
static group_t groups[MAX_GROUPS];
static int     futexq;                 /* channel futex waiters sleep on */
static proc_t *cur;                    /* NULL = the shell's context */
static lx_u64  shell_ctx[13];
static int     next_pid = 1;
static lx_u64  rng;
static int     pollq;                  /* channel: "something may be ready" */
static uint8_t page_prot[PAGES];       /* loader: combined segment permissions */
static char    argbuf[ARGBUF];         /* execve: argv / envp copied in */

static lx_u64 ticks(void) { lx_u64 v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)); return v; }
static lx_u64 freq(void)  { lx_u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

/* A kernel stack overflow would silently corrupt the stack below it
 * (another thread's): stop loudly instead. */
static void check_kstack(proc_t *p)
{
    if (!p || !p->kstack) return;
    for (int k = 0; k < 8; k++)
        if (((lx_u64 *)p->kstack)[k] != KSTACK_CANARY) {
            kprintf("[LINUX]  FATAL: kernel stack overflow in thread %d\n", p->pid);
            for (;;) { }
        }
}

static void switch_to(proc_t *next)
{
    proc_t *prev = cur;
    if (prev == next) return;
    check_kstack(prev);
    if (prev) __asm__ volatile("mrs %0, tpidr_el0" : "=r"(prev->tpidr));
    cur = next;
    mmu_as_t *as = next ? next->g->as : 0;
    if (mmu_as_current() != as) mmu_as_activate(as);         /* threads share one */
    if (next) __asm__ volatile("msr tpidr_el0, %0" :: "r"(next->tpidr));
    cpu_switch(prev ? prev->kctx : shell_ctx, next ? next->kctx : shell_ctx);
}

/* Wake sleepers whose deadline has passed. */
static void check_deadlines(void)
{
    lx_u64 now = ticks();
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_SLEEP && procs[i].deadline && now >= procs[i].deadline) {
            procs[i].state = P_RUN;
            procs[i].wchan = 0;
        }
}

static proc_t *pick_runnable(proc_t *after)
{
    check_deadlines();
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

static void yield(void)
{
    if (usbd_daemon) usbd_daemon();         /* keep USB (keyboard, disks) alive */
    schedule();
}

/* Sleep until woken on ch, or until deadline (ticks; 0 = none). */
static void sleep_until(void *ch, lx_u64 deadline)
{
    cur->state = P_SLEEP;
    cur->wchan = ch;
    cur->deadline = deadline;
    schedule();
    cur->deadline = 0;
}

static void sleep_on(void *ch) { sleep_until(ch, 0); }

/* Wake everyone sleeping on ch, and every poll / epoll waiter. */
static void wakeup(void *ch)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_SLEEP && (procs[i].wchan == ch || procs[i].wchan == &pollq)) {
            procs[i].state = P_RUN;
            procs[i].wchan = 0;
        }
}

static proc_t *thread_by_tid(int tid)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if ((procs[i].state == P_RUN || procs[i].state == P_SLEEP) && procs[i].pid == tid)
            return &procs[i];
    return 0;
}

static group_t *group_by_pid(int pid)
{
    for (int i = 0; i < MAX_GROUPS; i++)
        if (groups[i].state != G_FREE && groups[i].pid == pid) return &groups[i];
    return 0;
}

static int alive(const proc_t *p) { return p->state == P_RUN || p->state == P_SLEEP; }

/* ── Signals ──────────────────────────────────────────────────────────── */
static int sig_default_ignored(int sig)
{
    return sig == SIGCHLD || sig == 23 || sig == 28 || sig == SIGCONT ||   /* URG, WINCH */
           sig == SIGSTOP || sig == 20 || sig == 21 || sig == 22;         /* stops: ignored */
}

/* A signal for one thread (tkill, tgkill, faults). */
static void send_sig(proc_t *p, int sig, int from)
{
    if (sig < 1 || sig > NSIG || !alive(p)) return;
    lx_u64 bit = SIGBIT(sig);
    if (sig != SIGKILL && sig != SIGSTOP) {
        if (p->g->sa[sig - 1].handler == SIG_IGN) return;
        if (!(p->sig_blocked & bit) && p->g->sa[sig - 1].handler == SIG_DFL &&
            sig_default_ignored(sig))
            return;
    }
    p->sig_pending |= bit;
    p->sig_from[sig - 1] = from;
    if (!(p->sig_blocked & bit) || (bit & UNBLOCKABLE)) {
        if (p->state == P_SLEEP) { p->state = P_RUN; p->wchan = 0; }
    } else {
        wakeup(&pollq);                     /* signalfd readers */
    }
}

/* A signal for a process: a thread that doesn't block it takes it (the
 * first one by preference); SIGKILL goes to all of them. */
static void send_sig_group(group_t *g, int sig, int from)
{
    if (!g || g->state != G_LIVE) return;
    proc_t *pick = 0, *any = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_t *t = &procs[i];
        if (!alive(t) || t->g != g) continue;
        if (sig == SIGKILL) { send_sig(t, sig, from); continue; }
        if (!any || t->pid == g->pid) any = t;
        if (!(t->sig_blocked & SIGBIT(sig)) && (!pick || t->pid == g->pid)) pick = t;
    }
    if (sig != SIGKILL && (pick || any)) send_sig(pick ? pick : any, sig, from);
}

/* A signal that would interrupt a wait. */
static int sig_interrupt(void)
{
    return (cur->sig_pending & ~(cur->sig_blocked & ~UNBLOCKABLE)) != 0;
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

/* ── Time ─────────────────────────────────────────────────────────────── */
static lx_u64 t0;
static s64    rt0;

static void time_init(void)
{
    if (!t0) { t0 = ticks(); rt0 = kernel_time_seconds ? kernel_time_seconds() : 0; }
}

static int clock_realtime(lx_u64 clk) { return clk == 0 || clk == 5 || clk == 8; }

/* clock time -> counter ticks (for absolute timers); 0 if in the past */
static lx_u64 clock_to_ticks(lx_u64 clk, s64 sec, s64 nsec)
{
    lx_u64 f = freq();
    time_init();
    if (clock_realtime(clk)) {
        sec -= rt0;
        if (sec < 0) return 1;
        return t0 + (lx_u64)sec * f + (lx_u64)nsec * f / 1000000000ULL;
    }
    if (sec < 0) return 1;
    return (lx_u64)sec * f + (lx_u64)nsec * f / 1000000000ULL;
}

static lx_u64 dur_ticks(s64 sec, s64 nsec)
{
    lx_u64 f = freq();
    return (lx_u64)sec * f + (lx_u64)nsec * f / 1000000000ULL;
}

static void ticks_to_ts(lx_u64 t, s64 *ts)
{
    lx_u64 f = freq();
    ts[0] = (s64)(t / f);
    ts[1] = (s64)((t % f) * 1000000000ULL / f);
}

/* user struct timespec -> 0 / -EFAULT / -EINVAL */
static int get_ts(lx_u64 up, s64 *sec, s64 *nsec)
{
    if (!uok(up, 16, 0)) return -EFAULT;
    *sec = ((s64 *)(uintptr_t)up)[0];
    *nsec = ((s64 *)(uintptr_t)up)[1];
    if (*sec < 0 || *nsec < 0 || *nsec >= 1000000000) return -EINVAL;
    return 0;
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

static int streq(const char *a, const char *b) { return kstrcmp(a, b) == 0; }

static int user_path(int dirfd, lx_u64 up, char *out)
{
    char in[VFS_PATH_MAX];
    int r = copy_in_str(in, up, sizeof in);
    if (r) return r;
    const char *base = cur->g->cwd;
    if (in[0] != '/' && dirfd != AT_FDCWD) {
        if (dirfd < 0 || dirfd >= MAX_FDS || !cur->g->fd[dirfd] || cur->g->fd[dirfd]->kind != F_DIR)
            return -EBADF;
        base = cur->g->fd[dirfd]->path;
    }
    r = norm_path(base, in, out);
    if (!r && streq(out, "/proc/self/exe") && cur->g->exe[0])   /* the program itself */
        kstrncpy(out, cur->g->exe, VFS_PATH_MAX);
    return r;
}


/* /tmp/... -> the path inside the RAM file system, else NULL */
static const char *tmp_rel(const char *path)
{
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' &&
        (path[4] == '/' || !path[4]))
        return path[4] ? path + 4 : "/";
    return 0;
}

/* ── Console ──────────────────────────────────────────────────────────── */
/* ── Terminals: the console and pseudo-terminals ─────────────────────── */
/* A tty: termios state (Linux's kernel struct termios, 36 bytes), window
 * size, foreground process group and session, the input queue a reader
 * gets (complete lines in canonical mode, bytes in raw mode) and the line
 * being edited. Input arrives a character at a time (tty_input) from the
 * UART / USB keyboard (the console) or a pty master's writes; output (and
 * echo) goes to out(): the UART, or the master's read queue. */
#define T_ISIG   0x0001
#define T_ICANON 0x0002
#define T_ECHO   0x0008
#define T_ECHOE  0x0010
#define T_ECHOK  0x0020
#define T_ECHOCTL 0x0200
#define T_IEXTEN 0x8000
#define T_ICRNL  0x0100
#define T_INLCR  0x0040
#define T_IGNCR  0x0080
#define T_OPOST  0x0001
#define T_ONLCR  0x0004
enum { VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VTIME = 5, VMIN = 6,
       VSUSP = 10, VEOL = 11, VWERASE = 14, VEOL2 = 16 };
#define TTY_IN 2048
#define MAX_PTYS 8

typedef struct tty {
    int      used;
    struct {
        uint32_t iflag, oflag, cflag, lflag;
        uint8_t  line, cc[19];
    } __attribute__((packed)) tio;
    uint16_t rows, cols, xpix, ypix;
    char     in[TTY_IN];               /* readable input */
    uint32_t inh, inn;
    uint32_t lines;                    /* canonical: line ends (or EOFs) in in[] */
    char     edit[256];                /* canonical: the line being typed */
    uint32_t editn;
    int      pgrp, sid;
    void   (*out)(struct tty *t, const char *s, uint32_t n);
    void   (*pump)(struct tty *t);     /* pull input from the device (console) */
    /* pty */
    int      index, locked, masters, slaves, hung_up, slave_gone;
    char     obuf[8192];               /* slave output for the master to read */
    uint32_t oh, on;
} tty_t;

static tty_t console_tty, ptys[MAX_PTYS];

static void tty_defaults(tty_t *t)
{
    kmemset(&t->tio, 0, sizeof t->tio);
    t->tio.iflag = T_ICRNL | 0x2000 /* IXON */ | 0x4000 /* IUTF8 */;
    t->tio.oflag = T_OPOST | T_ONLCR;
    t->tio.cflag = 0x000F /* B38400 */ | 0x0030 /* CS8 */ | 0x0080 /* CREAD */;
    t->tio.lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | T_ECHOCTL | 0x0800 /* ECHOKE */ | T_IEXTEN;
    t->tio.cc[VINTR] = 3; t->tio.cc[VQUIT] = 28; t->tio.cc[VERASE] = 127; t->tio.cc[VKILL] = 21;
    t->tio.cc[VEOF] = 4; t->tio.cc[VMIN] = 1; t->tio.cc[VSUSP] = 26; t->tio.cc[VWERASE] = 23;
    t->rows = 25; t->cols = 80;
    t->inh = t->inn = t->lines = t->editn = 0;
}

/* Output with OPOST processing (\n -> \r\n under ONLCR). */
static void tty_output(tty_t *t, const char *s, lx_u64 n)
{
    if (!(t->tio.oflag & T_OPOST) || !(t->tio.oflag & T_ONLCR)) { t->out(t, s, (uint32_t)n); return; }
    lx_u64 i = 0;
    while (i < n) {
        lx_u64 j = i;
        while (j < n && s[j] != '\n') j++;
        if (j > i) t->out(t, s + i, (uint32_t)(j - i));
        if (j < n) { t->out(t, "\r\n", 2); j++; }
        i = j;
    }
}

static void tty_echo(tty_t *t, char c)
{
    if (!(t->tio.lflag & T_ECHO)) return;
    if ((uint8_t)c < 0x20 && c != '\n' && c != '\t' && (t->tio.lflag & T_ECHOCTL)) {
        char e[2] = { '^', (char)(c + 64) };
        tty_output(t, e, 2);
    } else {
        tty_output(t, &c, 1);
    }
}

static void sig_pgrp(int pgrp, int sig);

static void tty_push(tty_t *t, char c)
{
    if (t->inn < TTY_IN) { t->in[(t->inh + t->inn) % TTY_IN] = c; t->inn++; }
}

/* One character of input, through the line discipline. */
static void tty_input(tty_t *t, char c)
{
    uint32_t lf = t->tio.lflag, iff = t->tio.iflag;
    const uint8_t *cc = t->tio.cc;
    if (c == '\r') { if (iff & T_IGNCR) return; if (iff & T_ICRNL) c = '\n'; }
    else if (c == '\n' && (iff & T_INLCR)) c = '\r';
    if (lf & T_ISIG) {
        int sig = (uint8_t)c == cc[VINTR] ? SIGINT : (uint8_t)c == cc[VQUIT] ? 3 : 0;
        if (sig && c) {
            tty_echo(t, c);
            if (lf & T_ICANON) t->editn = 0;
            if (t->pgrp) sig_pgrp(t->pgrp, sig);
            wakeup(t);
            return;
        }
        if (c && (uint8_t)c == cc[VSUSP]) return;          /* no job stopping (yet) */
    }
    if (!(lf & T_ICANON)) {                                 /* raw */
        tty_push(t, c);
        tty_echo(t, c);
        wakeup(t);
        return;
    }
    if ((uint8_t)c == cc[VERASE] || c == '\b') {
        if (t->editn) {
            t->editn--;
            if ((lf & T_ECHO) && (lf & T_ECHOE)) tty_output(t, "\b \b", 3);
        }
        return;
    }
    if ((uint8_t)c == cc[VKILL] && c) {
        while (t->editn) { t->editn--; if ((lf & T_ECHO) && (lf & T_ECHOE)) tty_output(t, "\b \b", 3); }
        return;
    }
    if ((uint8_t)c == cc[VWERASE] && c && (lf & T_IEXTEN)) {
        while (t->editn && t->edit[t->editn - 1] == ' ') { t->editn--; tty_output(t, "\b \b", 3); }
        while (t->editn && t->edit[t->editn - 1] != ' ') { t->editn--; tty_output(t, "\b \b", 3); }
        return;
    }
    int eof = c && (uint8_t)c == cc[VEOF];
    if (c == '\n' || eof || (c && ((uint8_t)c == cc[VEOL] || (uint8_t)c == cc[VEOL2]))) {
        if (!eof) { t->edit[t->editn++] = c; tty_echo(t, c); }
        if (t->inn + t->editn + 1 > TTY_IN) { t->editn = 0; return; }   /* full: drop the line */
        for (uint32_t i = 0; i < t->editn; i++) tty_push(t, t->edit[i]);
        if (eof && !t->editn) tty_push(t, 0);                         /* EOF marker (a read of 0) */
        t->lines++;
        t->editn = 0;
        wakeup(t);
        return;
    }
    if (t->editn < sizeof t->edit - 1) {
        t->edit[t->editn++] = c;
        tty_echo(t, c);
    }
}

static int tty_readable(tty_t *t)
{
    if (t->pump) t->pump(t);
    if (t->hung_up) return 1;
    return (t->tio.lflag & T_ICANON) ? t->lines > 0 : t->inn > 0;
}

static s64 tty_read(tty_t *t, char *buf, lx_u64 n, int nonblock)
{
    for (;;) {
        if (tty_readable(t)) break;
        if (!(t->tio.lflag & T_ICANON) && !t->tio.cc[VMIN] && !t->tio.cc[VTIME]) return 0;  /* polling read */
        if (nonblock) return -EAGAIN;
        if (sig_interrupt()) return -EINTR;
        if (t->pump) yield();                          /* the console is polled */
        else sleep_on(t);
    }
    if (t->hung_up && !t->inn) return 0;
    lx_u64 k = 0;
    if (t->tio.lflag & T_ICANON) {                     /* one line at most */
        if (t->in[t->inh] == 0 && t->inn) {            /* EOF marker */
            t->inh = (t->inh + 1) % TTY_IN; t->inn--; t->lines--;
            return 0;
        }
        while (k < n && t->inn) {
            char c = t->in[t->inh];
            t->inh = (t->inh + 1) % TTY_IN; t->inn--;
            buf[k++] = c;
            if (c == '\n' || c == (char)t->tio.cc[VEOL] || c == (char)t->tio.cc[VEOL2]) { t->lines--; break; }
        }
    } else {
        while (k < n && t->inn) { buf[k++] = t->in[t->inh]; t->inh = (t->inh + 1) % TTY_IN; t->inn--; }
    }
    return (s64)k;
}

/* The console: UART output, input from the UART / USB keyboard. */
static void con_out(tty_t *t, const char *s, uint32_t n) { (void)t; for (uint32_t i = 0; i < n; i++) uart_putc(s[i]); }
static void con_pump(tty_t *t) { while (uart_ready()) tty_input(t, uart_getc()); }

/* A pty's slave output (and echo) is what its master reads. */
static void pty_out(tty_t *t, const char *s, uint32_t n)
{
    for (uint32_t i = 0; i < n && t->on < sizeof t->obuf; i++) {
        t->obuf[(t->oh + t->on) % sizeof t->obuf] = s[i];
        t->on++;
    }
    wakeup(t);
}

static tty_t *pty_alloc(void)
{
    for (int i = 0; i < MAX_PTYS; i++)
        if (!ptys[i].used) {
            tty_t *t = &ptys[i];
            kmemset(t, 0, sizeof *t);
            t->used = 1;
            t->index = i;
            t->locked = 1;
            t->out = pty_out;
            tty_defaults(t);
            return t;
        }
    return 0;
}

static void pty_release(tty_t *t)
{
    if (!t->masters && !t->slaves) t->used = 0;
}

static s64 ptm_read(file_t *f, char *buf, lx_u64 n)
{
    tty_t *t = f->tty;
    while (!t->on) {
        if (!t->slaves && t->slave_gone) return -EIO;  /* every slave closed */
        if (f->flags & L_O_NONBLOCK) return -EAGAIN;
        if (sig_interrupt()) return -EINTR;
        sleep_on(t);
    }
    lx_u64 k = 0;
    while (k < n && t->on) { buf[k++] = t->obuf[t->oh]; t->oh = (t->oh + 1) % sizeof t->obuf; t->on--; }
    wakeup(t);
    return (s64)k;
}

static s64 ptm_write(file_t *f, const char *buf, lx_u64 n)
{
    for (lx_u64 i = 0; i < n; i++) tty_input(f->tty, buf[i]);
    return (s64)n;
}

static void sig_pgrp(int pgrp, int sig)
{
    for (int i = 0; i < MAX_GROUPS; i++)
        if (groups[i].state == G_LIVE && groups[i].pgid == pgrp) send_sig_group(&groups[i], sig, 0);
}

/* termios / window / job-control ioctls, for the console and ptys (either side). */
static s64 tty_ioctl(file_t *f, tty_t *t, lx_u64 req, lx_u64 arg)
{
    switch (req) {
    case 0x5401:                                       /* TCGETS */
        if (!uok(arg, sizeof t->tio, 1)) return -EFAULT;
        kmemcpy((void *)(uintptr_t)arg, &t->tio, sizeof t->tio);
        return 0;
    case 0x5402: case 0x5403: case 0x5404: {          /* TCSETS, TCSETSW, TCSETSF */
        if (!uok(arg, sizeof t->tio, 0)) return -EFAULT;
        int was_canon = (t->tio.lflag & T_ICANON) != 0;
        kmemcpy(&t->tio, (void *)(uintptr_t)arg, sizeof t->tio);
        if (req == 0x5404) { t->inh = t->inn = t->lines = 0; t->editn = 0; }   /* flush input */
        if (was_canon && !(t->tio.lflag & T_ICANON) && t->editn) {          /* to raw: keep the typed part */
            for (uint32_t i = 0; i < t->editn; i++) tty_push(t, t->edit[i]);
            t->editn = 0;
        }
        if (!(t->tio.lflag & T_ICANON)) t->lines = 0;
        wakeup(t);
        return 0;
    }
    case 0x540B:                                       /* TCFLSH */
        if (arg == 0 || arg == 2) { t->inh = t->inn = t->lines = 0; t->editn = 0; }
        if (arg == 1 || arg == 2) t->oh = t->on = 0;
        return 0;
    case 0x5409: case 0x540A: return 0;                /* TCSBRK, TCXONC */
    case 0x5413:                                       /* TIOCGWINSZ */
        if (!uok(arg, 8, 1)) return -EFAULT;
        ((uint16_t *)(uintptr_t)arg)[0] = t->rows; ((uint16_t *)(uintptr_t)arg)[1] = t->cols;
        ((uint16_t *)(uintptr_t)arg)[2] = t->xpix; ((uint16_t *)(uintptr_t)arg)[3] = t->ypix;
        return 0;
    case 0x5414: {                                     /* TIOCSWINSZ */
        if (!uok(arg, 8, 0)) return -EFAULT;
        const uint16_t *w = (const uint16_t *)(uintptr_t)arg;
        int changed = w[0] != t->rows || w[1] != t->cols;
        t->rows = w[0]; t->cols = w[1]; t->xpix = w[2]; t->ypix = w[3];
        if (changed && t->pgrp) sig_pgrp(t->pgrp, 28);   /* SIGWINCH */
        return 0;
    }
    case 0x540E:                                       /* TIOCSCTTY */
        if (cur->g->sid != cur->g->pid && t->sid != cur->g->sid) return -EPERM;
        cur->g->ctty = t;
        t->sid = cur->g->sid;
        t->pgrp = cur->g->pgid;
        return 0;
    case 0x5422:                                       /* TIOCNOTTY */
        if (cur->g->ctty == t) cur->g->ctty = 0;
        return 0;
    case 0x540F:                                       /* TIOCGPGRP */
        if (!uok(arg, 4, 1)) return -EFAULT;
        *(int32_t *)(uintptr_t)arg = t->pgrp ? t->pgrp : cur->g->pgid;
        return 0;
    case 0x5410:                                       /* TIOCSPGRP */
        if (!uok(arg, 4, 0)) return -EFAULT;
        t->pgrp = *(int32_t *)(uintptr_t)arg;
        return 0;
    case 0x5429:                                       /* TIOCGSID */
        if (!uok(arg, 4, 1)) return -EFAULT;
        *(int32_t *)(uintptr_t)arg = t->sid ? t->sid : cur->g->sid;
        return 0;
    case 0x541B:                                       /* FIONREAD */
        if (!uok(arg, 4, 1)) return -EFAULT;
        *(int32_t *)(uintptr_t)arg = f->kind == F_PTMX ? (int32_t)t->on
                                     : (t->tio.lflag & T_ICANON) && !t->lines ? 0 : (int32_t)t->inn;
        return 0;
    case 0x80045430:                                   /* TIOCGPTN */
        if (f->kind != F_PTMX) return -ENOTTY;
        if (!uok(arg, 4, 1)) return -EFAULT;
        *(int32_t *)(uintptr_t)arg = t->index;
        return 0;
    case 0x40045431:                                   /* TIOCSPTLCK */
        if (f->kind != F_PTMX) return -ENOTTY;
        if (!uok(arg, 4, 0)) return -EFAULT;
        t->locked = *(int32_t *)(uintptr_t)arg != 0;
        return 0;
    case 0x80045439:                                   /* TIOCGPKT */
        if (!uok(arg, 4, 1)) return -EFAULT;
        *(int32_t *)(uintptr_t)arg = 0;
        return 0;
    default:
        return -ENOTTY;
    }
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

/* ── Memory objects (memfd, shared anonymous mappings) ────────────────── */
static memobj_t *mem_new(void)
{
    for (int i = 0; i < MAX_MEMOBJS; i++)
        if (!memobjs[i].refs) {
            kmemset(&memobjs[i], 0, sizeof memobjs[i]);
            memobjs[i].refs = 1;
            return &memobjs[i];
        }
    return 0;
}

static void mem_put(memobj_t *m)
{
    if (!m || --m->refs > 0) return;
    for (uint32_t i = 0; i < m->npages; i++) mmu_frame_put(m->pages[i]);
    if (m->pages) mmu_frames_free(m->pages, m->table_frames);
    m->refs = 0;
}

/* Set the size: pages are allocated (zeroed) or released as needed. */
static int mem_resize(memobj_t *m, lx_u64 size)
{
    if (size > MEMFD_MAX) return -EFBIG;
    uint32_t want = (uint32_t)((size + PAGE - 1) / PAGE);
    if (want > m->cap) {
        uint32_t tf = (uint32_t)((want * sizeof(void *) + PAGE - 1) / PAGE);
        void **t = mmu_frames_alloc(tf);
        if (!t) return -ENOMEM;
        for (uint32_t i = 0; i < m->npages; i++) t[i] = m->pages[i];
        if (m->pages) mmu_frames_free(m->pages, m->table_frames);
        m->pages = t;
        m->table_frames = tf;
        m->cap = tf * (uint32_t)(PAGE / sizeof(void *));
    }
    while (m->npages < want) {
        void *f = mmu_frames_alloc(1);
        if (!f) return -ENOMEM;
        m->pages[m->npages++] = f;
    }
    while (m->npages > want) mmu_frame_put(m->pages[--m->npages]);
    if (size % PAGE && want)                        /* bytes past the end read as zero */
        kmemset((uint8_t *)m->pages[want - 1] + size % PAGE, 0, PAGE - size % PAGE);
    m->size = size;
    return 0;
}

static s64 mem_rw(memobj_t *m, lx_u64 *pos, uint8_t *buf, lx_u64 n, int wr)
{
    if (wr && *pos + n > m->size) {
        int r = mem_resize(m, *pos + n);
        if (r) return r;
    }
    lx_u64 done = 0;
    while (done < n && *pos < m->size) {
        lx_u64 off = *pos % PAGE, k = PAGE - off;
        if (k > n - done) k = n - done;
        if (k > m->size - *pos) k = m->size - *pos;
        uint8_t *pg = (uint8_t *)m->pages[*pos / PAGE] + off;
        if (wr) kmemcpy(pg, buf + done, k); else kmemcpy(buf + done, pg, k);
        done += k;
        *pos += k;
    }
    return (s64)done;
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

static void file_free(file_t *f)                /* a file_new that never got an fd */
{
    f->refs = 0;
    f->kind = F_FREE;
}

static void sock_release(sock_t *s);

static void file_put(file_t *f)
{
    if (!f || --f->refs > 0) return;
    /* epoll interest lists forget a file when it's closed for good */
    for (int e = 0; e < MAX_EPOLLS; e++) {
        epoll_t *ep = &epolls[e];
        if (!ep->used) continue;
        for (int i = 0; i < ep->n; )
            if (ep->it[i].f == f) ep->it[i] = ep->it[--ep->n]; else i++;
    }
    switch (f->kind) {
    case F_FILE: vfs_close(f->vfd); break;
    case F_DIR:  vfs_closedir(f->vfd); break;
    case F_PIPE_R: case F_PIPE_W: {
        pipe_t *p = f->pipe;
        if (f->kind == F_PIPE_R) p->readers--; else p->writers--;
        wakeup(p);
        if (!p->readers && !p->writers) p->used = 0;
        break;
    }
    case F_EPOLL:  f->ep->used = 0; break;
    case F_MEMFD:  mem_put(f->mem); break;
    case F_SOCK:   sock_release(f->sock); break;
    case F_EVDEV:  if (f->vfd == 0 && f->count && usbkbd_grab) usbkbd_grab(0); break;
    case F_CONSOLE:
        if (f->tty != &console_tty && --f->tty->slaves == 0) {
            f->tty->slave_gone = 1;
            wakeup(f->tty);
            pty_release(f->tty);
        }
        break;
    case F_PTMX:
        if (--f->tty->masters == 0) {                  /* hang-up for the slave side */
            tty_t *t = f->tty;
            t->hung_up = 1;
            if (t->pgrp) sig_pgrp(t->pgrp, 1);         /* SIGHUP */
            for (int i = 0; i < MAX_GROUPS; i++)
                if (groups[i].state == G_LIVE && groups[i].ctty == t) groups[i].ctty = 0;
            wakeup(t);
            pty_release(t);
        }
        break;
    default: break;
    }
    f->kind = F_FREE;
}

static int fd_install(file_t *f, int cloexec, int min)
{
    for (int i = min; i < MAX_FDS; i++)
        if (!cur->g->fd[i]) {
            cur->g->fd[i] = f;
            if (cloexec) cur->g->cloexec |= 1ULL << i;
            else cur->g->cloexec &= ~(1ULL << i);
            return i;
        }
    return -EMFILE;
}

/* Install a new file, or free it if there's no descriptor for it. */
static int fd_new(file_t *f, int cloexec)
{
    int fd = fd_install(f, cloexec, 0);
    if (fd < 0) file_put(f);
    return fd;
}

static file_t *fd_get(lx_u64 fd) { return fd < MAX_FDS ? cur->g->fd[fd] : 0; }

static void fd_close(proc_t *p, int i)
{
    file_t *f = p->g->fd[i];
    p->g->fd[i] = 0;
    p->g->cloexec &= ~(1ULL << i);
    file_put(f);
}

/* struct stat, AArch64 Linux layout (asm-generic). */
enum { ST_VFS, ST_CHR, ST_FIFO, ST_SOCK, ST_ANON, ST_MEM };
static void fill_stat(uint8_t *st, const vfs_stat_t *vs, uint32_t kind, lx_u64 size)
{
    kmemset(st, 0, 128);
    uint32_t mode;
    switch (kind) {
    case ST_CHR:  mode = 0020000 | 0666; break;
    case ST_FIFO: mode = 0010000 | 0600; break;
    case ST_SOCK: mode = 0140000 | 0777; break;
    case ST_ANON: mode = 0600; break;
    case ST_MEM:  mode = 0100000 | 0600; break;
    default:
        if (vs->attrs & VFS_ATTR_DIR) mode = 0040000 | 0755;
        else mode = 0100000 | ((vs->attrs & VFS_ATTR_RDONLY) ? 0444 : 0644);
        size = vs->size;
    }
    *(uint32_t *)(st + 16) = mode;                 /* st_mode */
    *(uint32_t *)(st + 20) = 1;                    /* st_nlink */
    *(int32_t *)(st + 56) = 4096;                  /* st_blksize */
    *(s64 *)(st + 48) = (s64)size;                 /* st_size */
    *(s64 *)(st + 64) = ((s64)size + 511) / 512;
    if (kind == ST_VFS) {
        s64 t = dos_to_unix(vs->mdate, vs->mtime);
        *(s64 *)(st + 72) = t;                     /* atime */
        *(s64 *)(st + 88) = t;                     /* mtime */
        *(s64 *)(st + 104) = t;                    /* ctime */
    }
}

static lx_u64 page_up(lx_u64 a);

/* ── /dev/fb0: Linux fbdev on the kernel's framebuffer ────────────────── */
#define FB_DAMAGE_IOCTL 0x401046F0      /* _IOW('F', 0xF0, {u32 x, y, w, h}): present a rectangle */

static long frames_kept;                /* allocated for good meanwhile (leak check) */

static int fb_geom(int *w, int *h, lx_u64 *size)
{
    if (!fb_front || !fb_get_info) return -ENODEV;
    if (!fb_front()) {
        unsigned long before = mmu_frames_free_count();
        if (!fb_init_driver || !fb_init_driver() || !fb_front()) return -ENODEV;
        frames_kept += (long)(before - mmu_frames_free_count());   /* the framebuffer stays */
    }
    int bpp;
    fb_get_info(w, h, &bpp);
    *size = (lx_u64)*w * *h * 4;
    return 0;
}

static s64 fb_ioctl(lx_u64 req, lx_u64 arg)
{
    int w, h;
    lx_u64 size;
    if (fb_geom(&w, &h, &size)) return -ENODEV;
    switch (req) {
    case 0x4600: case 0x4601: {                        /* FBIOGET / FBIOPUT_VSCREENINFO */
        if (!uok(arg, 160, req == 0x4600)) return -EFAULT;
        uint32_t *v = (uint32_t *)(uintptr_t)arg;
        if (req == 0x4601)                             /* one mode: accept only it */
            return v[0] == (uint32_t)w && v[1] == (uint32_t)h && (v[6] == 32 || !v[6]) ? 0 : -EINVAL;
        kmemset(v, 0, 160);
        v[0] = v[2] = (uint32_t)w;                     /* xres, xres_virtual */
        v[1] = v[3] = (uint32_t)h;
        v[6] = 32;                                     /* bits_per_pixel: XRGB8888 */
        v[8] = 16; v[9] = 8;                           /* red: offset, length */
        v[11] = 8; v[12] = 8;                          /* green */
        v[14] = 0; v[15] = 8;                          /* blue */
        v[22] = (uint32_t)-1; v[23] = (uint32_t)-1;    /* height, width in mm: unknown */
        v[25] = 16667;                                 /* pixclock (ps), nominal 60 Hz */
        return 0;
    }
    case 0x4602: {                                     /* FBIOGET_FSCREENINFO */
        if (!uok(arg, 80, 1)) return -EFAULT;
        uint8_t *x = (uint8_t *)(uintptr_t)arg;
        kmemset(x, 0, 80);
        kstrncpy((char *)x, "os2warp-fb", 16);
        *(lx_u64 *)(x + 16) = (lx_u64)(uintptr_t)fb_front();   /* smem_start */
        *(uint32_t *)(x + 24) = (uint32_t)size;        /* smem_len */
        *(uint32_t *)(x + 36) = 2;                     /* visual: TRUECOLOR */
        *(uint32_t *)(x + 48) = (uint32_t)w * 4;       /* line_length */
        return 0;
    }
    case 0x4606:                                       /* FBIOPAN_DISPLAY: present all */
        if (fb_flush) fb_flush(0, 0, w, h);
        return 0;
    case 0x40044620: return 0;                         /* FBIO_WAITFORVSYNC: no vsync to wait for */
    case FB_DAMAGE_IOCTL: {
        if (!uok(arg, 16, 0)) return -EFAULT;
        const uint32_t *r = (const uint32_t *)(uintptr_t)arg;
        if (fb_flush) fb_flush((int)r[0], (int)r[1], (int)r[2], (int)r[3]);
        return 0;
    }
    default: return -ENOTTY;
    }
}

/* Map the framebuffer's pages: the program draws straight to the screen. */
static s64 fb_mmap(lx_u64 at, lx_u64 size, lx_u64 off, int prot)
{
    int w, h;
    lx_u64 fbsize;
    if (fb_geom(&w, &h, &fbsize)) return -ENODEV;
    if (off + size > page_up(fbsize)) return -EINVAL;
    uint8_t *base = (uint8_t *)fb_front() + off;
    for (lx_u64 k = 0; k < size; k += PAGE) {
        void *frame = base + k;
        if (mmu_as_map_shared(cur->g->as, at + k, &frame, 1, prot)) {
            mmu_as_unmap(cur->g->as, at, k);
            return -ENOMEM;
        }
    }
    return (s64)at;
}

/* ── /dev/input/event0 (keyboard), event1 (pointer): Linux evdev ──────── */
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define BTN_LEFT 0x110

/* HID keyboard usage (page 7) -> Linux key code (as drivers/hid/hid-input.c) */
static const uint8_t hid_to_key[256] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     29, 42, 56,125, 97, 54,100,126,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
};

typedef struct { uint16_t type, code; int32_t value; } evrec_t;
#define EVQ 128
static evrec_t  evq[2][EVQ];
static uint32_t evh[2], evt[2];
static unsigned mouse_buttons;

static void ev_push(int d, uint16_t type, uint16_t code, int32_t value)
{
    uint32_t next = (evt[d] + 1) % EVQ;
    if (next == evh[d]) return;                        /* full: drop (a SYN_DROPPED would be right) */
    evq[d][evt[d]] = (evrec_t){ type, code, value };
    evt[d] = next;
}

/* Pull whatever the drivers have into the device's queue. */
static void ev_fill(int d)
{
    if (d == 0 && usbkbd_poll_raw) {
        int usage, down;
        while ((evt[0] + 3) % EVQ != evh[0] && usbkbd_poll_raw(&usage, &down)) {
            if (!hid_to_key[usage]) continue;
            ev_push(0, EV_KEY, hid_to_key[usage], down);
            ev_push(0, EV_SYN, 0, 0);
        }
    }
    if (d == 1 && usbmouse_poll) {
        usbmouse_ev_t m;
        while ((evt[1] + 8) % EVQ != evh[1] && usbmouse_poll(&m)) {
            if (m.absolute) { ev_push(1, EV_ABS, 0, m.x); ev_push(1, EV_ABS, 1, m.y); }
            else {
                if (m.dx) ev_push(1, EV_REL, 0, m.dx);
                if (m.dy) ev_push(1, EV_REL, 1, m.dy);
            }
            if (m.wheel) ev_push(1, EV_REL, 8, m.wheel);   /* REL_WHEEL */
            for (int b = 0; b < 3; b++)
                if ((m.buttons ^ mouse_buttons) >> b & 1)
                    ev_push(1, EV_KEY, (uint16_t)(BTN_LEFT + (b == 0 ? 0 : b == 1 ? 1 : 2)),
                            (int32_t)(m.buttons >> b & 1));
            mouse_buttons = m.buttons;
            ev_push(1, EV_SYN, 0, 0);
        }
    } else if (d == 1 && usbd_daemon) {
        usbd_daemon();
    }
}

static int ev_ready(int d) { ev_fill(d); return evh[d] != evt[d]; }

static s64 ev_read(file_t *f, uint8_t *buf, lx_u64 n)
{
    int d = f->vfd;
    if (n < 24) return -EINVAL;
    while (!ev_ready(d)) {
        if (f->flags & L_O_NONBLOCK) return -EAGAIN;
        if (sig_interrupt()) return -EINTR;
        sleep_until(&pollq, ticks() + freq() / 200);   /* devices are polled: 5 ms */
    }
    s64 ts[2];
    ticks_to_ts(ticks(), ts);
    lx_u64 k = 0;
    while (k + 24 <= n && evh[d] != evt[d]) {
        evrec_t *e = &evq[d][evh[d]];
        uint8_t *o = buf + k;
        *(s64 *)o = ts[0];                             /* struct timeval */
        *(s64 *)(o + 8) = ts[1] / 1000;
        *(uint16_t *)(o + 16) = e->type;
        *(uint16_t *)(o + 18) = e->code;
        *(int32_t *)(o + 20) = e->value;
        evh[d] = (evh[d] + 1) % EVQ;
        k += 24;
    }
    return (s64)k;
}

static void set_bit(uint8_t *bits, uint32_t room, uint32_t bit)
{
    if (bit / 8 < room) bits[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static s64 ev_ioctl(file_t *f, lx_u64 req, lx_u64 arg)
{
    int d = f->vfd;
    uint32_t dir = (uint32_t)(req >> 30), size = (uint32_t)(req >> 16) & 0x3FFF;
    uint32_t type = (uint32_t)(req >> 8) & 0xFF, nr = (uint32_t)req & 0xFF;
    if (type != 'E') return -ENOTTY;
    if (nr == 0x90) {                                                 /* EVIOCGRAB (value) */
        if (d == 0 && usbkbd_grab) { usbkbd_grab(arg != 0); f->count = arg != 0; }
        return 0;
    }
    if (nr == 0x91) return 0;                                         /* EVIOCREVOKE (value) */
    if (size && !uok(arg, size, dir & 2)) return -EFAULT;
    uint8_t *out = (uint8_t *)(uintptr_t)arg;
    if (nr == 0x01) { *(int32_t *)out = 0x010001; return 0; }        /* EVIOCGVERSION */
    if (nr == 0x02) {                                                 /* EVIOCGID */
        uint16_t *id = (uint16_t *)out;
        id[0] = 3; id[1] = 0x0627; id[2] = d ? 1 : 2; id[3] = 1;      /* BUS_USB, QEMU-ish */
        return 0;
    }
    if (nr == 0x06 || nr == 0x07 || nr == 0x08) {                     /* EVIOCGNAME / PHYS / UNIQ */
        const char *s = nr == 0x06 ? (d ? "USB pointer" : "USB keyboard") : nr == 0x07 ? "usb-0" : "";
        uint32_t l = (uint32_t)kstrlen(s) + 1;
        if (l > size) l = size;
        kmemcpy(out, s, l);
        if (l) out[l - 1] = 0;
        return l;
    }
    if (nr >= 0x18 && nr <= 0x1B) { kmemset(out, 0, size); return size; }   /* GKEY/GLED/GSND/GSW state */
    if (nr == 0x03 || nr == 0x04 || nr == 0x81 || nr == 0xA0) return 0;  /* REP, KEYCODE, REVOKE, CLKID */
    if (nr >= 0x20 && nr < 0x40) {                                    /* EVIOCGBIT(ev, len) */
        kmemset(out, 0, size);
        uint32_t ev = nr - 0x20;
        if (ev == 0) {
            set_bit(out, size, EV_SYN); set_bit(out, size, EV_KEY);
            if (d) { set_bit(out, size, EV_REL); set_bit(out, size, EV_ABS); }
        } else if (ev == EV_KEY) {
            if (d) for (int b = 0; b < 3; b++) set_bit(out, size, BTN_LEFT + (uint32_t)b);
            else for (int i = 0; i < 256; i++) if (hid_to_key[i]) set_bit(out, size, hid_to_key[i]);
        } else if (ev == EV_REL && d) {
            set_bit(out, size, 0); set_bit(out, size, 1); set_bit(out, size, 8);
        } else if (ev == EV_ABS && d) {
            set_bit(out, size, 0); set_bit(out, size, 1);
        }
        return size;
    }
    if (nr >= 0x40 && nr < 0x80) {                                    /* EVIOCGABS(axis) */
        if (size < 24) return -EINVAL;
        int32_t *a = (int32_t *)out;
        kmemset(a, 0, 24);
        if (!d || nr - 0x40 > 1) return -EINVAL;
        a[2] = 32767;                                                 /* min 0, max 32767 */
        return 0;
    }
    return -ENOTTY;
}

/* ── Memory ───────────────────────────────────────────────────────────── */
static lx_u64 page_up(lx_u64 a) { return (a + PAGE - 1) & ~(PAGE - 1); }

static s64 sys_brk(lx_u64 want)
{
    if (want < cur->g->brk_start || want > BRK_LIMIT) return (s64)cur->g->brk_cur;
    lx_u64 old = page_up(cur->g->brk_cur), new = page_up(want);
    if (new > old) {
        if (mmu_as_map(cur->g->as, old, new - old, MMU_PROT_READ | MMU_PROT_WRITE))
            return (s64)cur->g->brk_cur;
    } else if (new < old) {
        mmu_as_unmap(cur->g->as, new, old - new);
    }
    cur->g->brk_cur = want;
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
    if (!len || (off & (PAGE - 1)) || len > MMU_USER_SIZE) return -EINVAL;
    int p = prot_bits(prot);
    if ((p & MMU_PROT_WRITE) && (p & MMU_PROT_EXEC)) return -EPERM;      /* W^X */
    file_t *f = 0;
    if (!(flags & MAP_ANON)) {
        f = fd_get((lx_u64)fd);
        if (!f) return -EBADF;
        if (f->kind != F_FILE && f->kind != F_MEMFD && f->kind != F_FB) return -ENODEV;
        if (f->kind == F_FB && !(flags & MAP_SHARED)) return -EINVAL;
    }
    lx_u64 size = page_up(len), at;
    if (flags & MAP_FIXED) {
        if ((addr & (PAGE - 1)) || addr < MMU_USER_BASE || addr + size > STACK_BOTTOM) return -EINVAL;
        mmu_as_unmap(cur->g->as, addr, size);           /* fresh pages */
        at = addr;
    } else {
        if (cur->g->mmap_next + size > MMAP_LIMIT || cur->g->mmap_next + size < cur->g->mmap_next)
            return -ENOMEM;
        at = cur->g->mmap_next;
        cur->g->mmap_next += size;
    }
    /* Shared memory: memfd, or shared anonymous memory (shared with
     * children across fork). The mapping holds its own frame references. */
    memobj_t *m = 0;
    int own = 0;
    if ((flags & MAP_SHARED) && (flags & MAP_ANON)) {
        if (!(m = mem_new())) return -ENOMEM;
        own = 1;
        if (mem_resize(m, size)) { mem_put(m); return -ENOMEM; }
        off = 0;
    } else if ((flags & MAP_SHARED) && f->kind == F_MEMFD) {
        m = f->mem;
    } else if (f && f->kind == F_FB) {
        s64 r = fb_mmap(at, size, off, p ? p : MMU_PROT_READ);
        if (r >= 0 && !p) mmu_as_unmap(cur->g->as, at, size);
        return r;
    }
    if (m) {
        if (off + size > page_up(m->size)) { if (own) mem_put(m); return -EINVAL; }
        int r = mmu_as_map_shared(cur->g->as, at, m->pages + off / PAGE, size / PAGE,
                                  p ? p : MMU_PROT_READ);
        if (own) mem_put(m);
        if (r) return -ENOMEM;
        if (!p) mmu_as_unmap(cur->g->as, at, size);
        return (s64)at;
    }
    /* Private: fresh pages, with a copy of the file's contents. (A
     * read-only MAP_SHARED file mapping is the same thing, as long as
     * nobody writes the file meanwhile.) */
    if ((flags & MAP_SHARED) && (p & MMU_PROT_WRITE)) return -ENOSYS;   /* VFS files */
    if (mmu_as_map(cur->g->as, at, size, MMU_PROT_READ | MMU_PROT_WRITE)) return -ENOMEM;
    if (f && f->kind == F_MEMFD) {
        lx_u64 pos = off;
        mem_rw(f->mem, &pos, (uint8_t *)(uintptr_t)at, len, 0);
    } else if (f) {
        if (vfs_seek(f->vfd, (int32_t)off, SEEK_SET) < 0) { mmu_as_unmap(cur->g->as, at, size); return -EINVAL; }
        int got = vfs_read(f->vfd, (void *)(uintptr_t)at, (uint32_t)len);
        if (got < 0) { mmu_as_unmap(cur->g->as, at, size); return verr(got); }
    }
    if (p != (MMU_PROT_READ | MMU_PROT_WRITE)) {
        if (p) mmu_as_protect(cur->g->as, at, size, p);
        else   mmu_as_unmap(cur->g->as, at, size);       /* PROT_NONE */
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
/* Free the kernel stacks of threads that have exited (not the running one:
 * it's still on its stack). */
static void reap_dead(void)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_DEAD && &procs[i] != cur) {
            if (procs[i].kstack) mmu_frames_free(procs[i].kstack, KSTACK_PAGES);
            procs[i].kstack = 0;
            procs[i].state = P_FREE;
        }
}

static proc_t *proc_alloc(void)
{
    reap_dead();
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_FREE && !procs[i].kstack) {
            proc_t *p = &procs[i];
            kmemset(p, 0, sizeof *p);
            if (!(p->kstack = mmu_frames_alloc(KSTACK_PAGES))) return 0;
            for (int k = 0; k < 8; k++) ((lx_u64 *)p->kstack)[k] = KSTACK_CANARY;
            p->pid = next_pid++;
            return p;
        }
    return 0;
}

static group_t *group_alloc(void)
{
    for (int i = 0; i < MAX_GROUPS; i++)
        if (groups[i].state == G_FREE) {
            kmemset(&groups[i], 0, sizeof groups[i]);
            groups[i].state = G_LIVE;
            return &groups[i];
        }
    return 0;
}

/* A thread that never ran (clone failed half way). */
static void proc_discard(proc_t *p)
{
    if (p->kstack) mmu_frames_free(p->kstack, KSTACK_PAGES);
    p->kstack = 0;
    p->state = P_FREE;
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

/* Wake up to n futex waiters on (as, addr) whose bitset matches. */
static int futex_wake(mmu_as_t *as, lx_u64 addr, int n, uint32_t bits)
{
    int woken = 0;
    for (int i = 0; i < MAX_PROCS && woken < n; i++) {
        proc_t *t = &procs[i];
        if (t->state == P_SLEEP && t->wchan == &futexq && t->futex_as == as &&
            t->futex_addr == addr && (t->futex_bits & bits)) {
            t->futex_addr = 0;
            t->state = P_RUN;
            t->wchan = 0;
            woken++;
        }
    }
    return woken;
}

/* The last thread of a process is gone: files, memory, zombie, SIGCHLD. */
static void group_finish(group_t *g)
{
    for (int i = 0; i < MAX_FDS; i++) if (g->fd[i]) fd_close(cur, i);
    for (int i = 0; i < MAX_GROUPS; i++)               /* orphans go to pid 1 */
        if (groups[i].state != G_FREE && groups[i].ppid == g->pid) groups[i].ppid = 1;
    mmu_as_activate(0);
    if (!g->as_borrowed) mmu_as_destroy(g->as);
    g->as = 0;
    g->wstatus = g->exit_status;
    g->state = G_ZOMBIE;
    group_t *parent = group_by_pid(g->ppid);
    if (parent) {
        send_sig_group(parent, SIGCHLD, g->pid);
        wakeup(parent);                                /* wait4 */
    }
}

/* The current thread ends; never returns. */
static void thread_exit(void)
{
    proc_t *p = cur;
    group_t *g = p->g;
    if (p->clear_tid && g->as && uok(p->clear_tid, 4, 1)) {   /* pthread_join's futex */
        *(int32_t *)(uintptr_t)p->clear_tid = 0;
        futex_wake(g->as, p->clear_tid, 1, ~0u);
    }
    if (p->vfork_parent) { p->vfork_parent = 0; wakeup(p); }
    p->state = P_DEAD;
    if (--g->nthreads == 0) group_finish(g);
    else wakeup(g);                                    /* execve waits for the others */
    schedule();                                        /* the dead don't run again */
    for (;;) { }
}

/* exit (one thread) with a status, which is the process's if it was the
 * last one. */
static void do_exit(int wstatus)
{
    if (!cur->g->exiting) cur->g->exit_status = wstatus;
    thread_exit();
}

/* exit_group, or a fatal signal: the whole process goes. */
static void do_exit_group(int wstatus)
{
    group_t *g = cur->g;
    if (!g->exiting) {
        g->exiting = 1;
        g->exit_status = wstatus;
        for (int i = 0; i < MAX_PROCS; i++)
            if (alive(&procs[i]) && procs[i].g == g && &procs[i] != cur)
                send_sig(&procs[i], SIGKILL, 0);
    }
    thread_exit();
}

static s64 sys_clone(lx_u64 flags, lx_u64 newsp, lx_u64 ptid, lx_u64 tls, lx_u64 ctid)
{
    if ((flags & CLONE_THREAD) && (!(flags & CLONE_VM) || !(flags & CLONE_SIGHAND))) return -EINVAL;
    if ((flags & CLONE_VM) && !(flags & (CLONE_VFORK | CLONE_THREAD))) return -ENOSYS;
    if ((flags & CLONE_PARENT_SETTID) && !uok(ptid, 4, 1)) return -EFAULT;
    proc_t *c = proc_alloc();
    if (!c) return -EAGAIN;
    group_t *g;
    if (flags & CLONE_THREAD) {                        /* a thread of this process */
        g = cur->g;
    } else {                                           /* a new process */
        if (!(g = group_alloc())) { proc_discard(c); return -EAGAIN; }
        if (flags & CLONE_VM) {
            g->as = cur->g->as;
            g->as_borrowed = 1;
        } else if (!(g->as = mmu_as_fork(cur->g->as))) {
            g->state = G_FREE;
            proc_discard(c);
            return -ENOMEM;
        }
        g->pid = c->pid;
        g->ppid = cur->g->pid;
        for (int i = 0; i < MAX_FDS; i++)
            if (cur->g->fd[i]) { g->fd[i] = cur->g->fd[i]; g->fd[i]->refs++; }
        g->cloexec = cur->g->cloexec;
        g->brk_start = cur->g->brk_start;
        g->brk_cur = cur->g->brk_cur;
        g->mmap_next = cur->g->mmap_next;
        kmemcpy(g->sa, cur->g->sa, sizeof g->sa);
        kstrncpy(g->cwd, cur->g->cwd, sizeof g->cwd);
        kstrncpy(g->exe, cur->g->exe, sizeof g->exe);
        g->pgid = cur->g->pgid;
        g->sid = cur->g->sid;
        g->ctty = cur->g->ctty;
    }
    c->g = g;
    g->nthreads++;
    linux_tf_t *tf = new_frame(c);
    kmemcpy(tf, cur->tf, sizeof *tf);
    tf->x[0] = 0;                                       /* child: clone returns 0 */
    if (newsp) tf->sp = newsp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(c->tpidr));
    if (flags & CLONE_SETTLS) c->tpidr = tls;
    c->sig_blocked = cur->sig_blocked;                 /* pending signals aren't inherited */
    if (flags & CLONE_CHILD_CLEARTID) c->clear_tid = ctid;
    int pid = c->pid;
    if (flags & CLONE_PARENT_SETTID) *(int32_t *)(uintptr_t)ptid = pid;
    if ((flags & CLONE_CHILD_SETTID) && uok(ctid, 4, 1) && (flags & CLONE_VM))
        *(int32_t *)(uintptr_t)ctid = pid;             /* (same memory: threads, vfork) */
    c->state = P_RUN;
    if (flags & CLONE_VFORK) {                          /* until it execs or exits */
        c->vfork_parent = cur;
        while (c->vfork_parent) sleep_on(c);
    }
    return pid;
}

/* futex(uaddr, op, val, timeout | val2, uaddr2, val3). Private and shared
 * futexes are both keyed by (address space, address): shared ones work
 * between the threads of a process, not across processes. */
static s64 sys_futex(lx_u64 uaddr, lx_u64 op, lx_u64 val, lx_u64 uts, lx_u64 uaddr2, lx_u64 val3)
{
    const int WAIT = 0, WAKE = 1, REQUEUE = 3, CMP_REQUEUE = 4, WAIT_BITSET = 9, WAKE_BITSET = 10;
    int cmd = (int)(op & 127), realtime = (op & 256) != 0;
    mmu_as_t *as = cur->g->as;
    if (uaddr & 3) return -EINVAL;
    if (!uok(uaddr, 4, 0)) return -EFAULT;
    if (cmd == WAIT || cmd == WAIT_BITSET) {
        uint32_t bits = cmd == WAIT_BITSET ? (uint32_t)val3 : ~0u;
        if (!bits) return -EINVAL;
        lx_u64 deadline = 0;
        if (uts) {
            s64 sec, ns;
            int r = get_ts(uts, &sec, &ns);
            if (r) return r;
            deadline = cmd == WAIT ? ticks() + dur_ticks(sec, ns)          /* relative */
                                   : clock_to_ticks(realtime ? 0 : 1, sec, ns);   /* absolute */
            if (!deadline) deadline = 1;
        }
        if (*(volatile uint32_t *)(uintptr_t)uaddr != (uint32_t)val) return -EAGAIN;
        cur->futex_as = as;
        cur->futex_addr = uaddr;
        cur->futex_bits = bits;
        while (cur->futex_addr) {
            if (sig_interrupt()) { cur->futex_addr = 0; return -EINTR; }
            if (deadline && ticks() >= deadline) { cur->futex_addr = 0; return -ETIMEDOUT; }
            sleep_until(&futexq, deadline);
        }
        return 0;
    }
    if (cmd == WAKE || cmd == WAKE_BITSET) {
        uint32_t bits = cmd == WAKE_BITSET ? (uint32_t)val3 : ~0u;
        if (!bits) return -EINVAL;
        return futex_wake(as, uaddr, (int)(val > 0x7FFFFFFF ? 0x7FFFFFFF : val), bits);
    }
    if (cmd == REQUEUE || cmd == CMP_REQUEUE) {
        if ((uaddr2 & 3) || !uok(uaddr2, 4, 0)) return -EFAULT;
        if (cmd == CMP_REQUEUE && *(volatile uint32_t *)(uintptr_t)uaddr != (uint32_t)val3)
            return -EAGAIN;
        int woken = futex_wake(as, uaddr, (int)(val > 0x7FFFFFFF ? 0x7FFFFFFF : val), ~0u);
        int moved = 0, max = (int)(uts > 0x7FFFFFFF ? 0x7FFFFFFF : uts);   /* val2 */
        for (int i = 0; i < MAX_PROCS && moved < max; i++) {
            proc_t *t = &procs[i];
            if (t->state == P_SLEEP && t->wchan == &futexq && t->futex_as == as &&
                t->futex_addr == uaddr) {
                t->futex_addr = uaddr2;
                moved++;
            }
        }
        return cmd == CMP_REQUEUE ? woken + moved : woken;
    }
    return -ENOSYS;
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
    mmu_as_t *as, *old = cur->g->as;
    lx_u64 entry, sp, top;
    if ((r = exec_image(path, argbuf, argc, envc, &as, &entry, &sp, &top))) return r;
    /* Point of no return: the new image replaces the old one, and this
     * thread is the process's only one. */
    group_t *g = cur->g;
    if (g->nthreads > 1) {
        mmu_as_activate(old);                           /* they still run in the old image */
        g->exec_kill = 1;
        for (int i = 0; i < MAX_PROCS; i++)
            if (alive(&procs[i]) && procs[i].g == g && &procs[i] != cur) send_sig(&procs[i], SIGKILL, 0);
        while (g->nthreads > 1) sleep_on(g);
        g->exec_kill = 0;
        cur->sig_pending &= ~SIGBIT(SIGKILL);
    }
    cur->clear_tid = 0;
    if (cur->g->as_borrowed) {
        cur->g->as_borrowed = 0;
        if (cur->vfork_parent) { cur->vfork_parent = 0; wakeup(cur); }
    } else {
        mmu_as_destroy(old);
    }
    mmu_as_activate(as);
    cur->g->as = as;
    cur->g->brk_start = cur->g->brk_cur = top;
    kstrncpy(cur->g->exe, path, sizeof cur->g->exe);
    cur->g->mmap_next = MMAP_BASE;
    for (int i = 0; i < MAX_FDS; i++)
        if ((cur->g->cloexec >> i) & 1) fd_close(cur, i);
    for (int i = 0; i < NSIG; i++)                      /* handlers don't survive exec */
        if (cur->g->sa[i].handler != SIG_IGN) kmemset(&cur->g->sa[i], 0, sizeof cur->g->sa[i]);
    linux_tf_t *tf = cur->tf;
    kmemset(tf, 0, sizeof *tf);
    tf->elr = entry;
    tf->sp = sp;
    tf->spsr = 0;                                       /* EL0t, interrupts on */
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
        for (int i = 0; i < MAX_GROUPS; i++) {
            group_t *c = &groups[i];
            if (c->state == G_FREE || c->ppid != cur->g->pid || c == cur->g) continue;
            if (pid > 0 && c->pid != pid) continue;
            any = 1;
            if (c->state == G_ZOMBIE) {
                int cp = c->pid;
                if (ustatus) *(int32_t *)(uintptr_t)ustatus = c->wstatus;
                c->state = G_FREE;
                reap_dead();
                return cp;
            }
        }
        if (!any) return -ECHILD;
        if (options & WNOHANG) return 0;
        if (sig_interrupt()) return -EINTR;
        sleep_on(cur->g);
    }
}

/* kill(pid > 0): that process; 0: the caller's process group; -1: every
 * process; < -1: process group -pid. */
static s64 sys_kill(s64 pid, lx_u64 sig)
{
    if (sig > NSIG) return -EINVAL;
    int hits = 0;
    for (int i = 0; i < MAX_GROUPS; i++) {
        group_t *g = &groups[i];
        if (g->state != G_LIVE) continue;
        if (pid > 0 && g->pid != pid) continue;
        if (pid == 0 && g->pgid != cur->g->pgid) continue;
        if (pid < -1 && g->pgid != -pid) continue;
        hits++;
        if (sig) send_sig_group(g, (int)sig, cur->g->pid);
    }
    return hits ? 0 : -ESRCH;
}

/* tkill / tgkill: one thread (tgid < 0: any process). */
static s64 sys_tgkill(s64 tgid, s64 tid, lx_u64 sig)
{
    if (sig > NSIG || tid <= 0) return -EINVAL;
    proc_t *t = thread_by_tid((int)tid);
    if (!t || (tgid > 0 && t->g->pid != tgid)) return -ESRCH;
    if (sig) send_sig(t, (int)sig, cur->g->pid);
    return 0;
}

static s64 sys_sigaction(lx_u64 sig, lx_u64 act, lx_u64 old, lx_u64 size)
{
    if (sig < 1 || sig > NSIG || size != 8) return -EINVAL;
    if ((act && !uok(act, 32, 0)) || (old && !uok(old, 32, 1))) return -EFAULT;
    sigact_t *sa = &cur->g->sa[sig - 1];
    if (old) kmemcpy((void *)(uintptr_t)old, sa, sizeof *sa);
    if (act) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        kmemcpy(sa, (void *)(uintptr_t)act, sizeof *sa);
        sa->mask &= ~UNBLOCKABLE;
        /* Setting SIG_IGN (or SIG_DFL for a default-ignored signal)
         * discards a pending one. */
        if (sa->handler == SIG_IGN || (sa->handler == SIG_DFL && sig_default_ignored((int)sig)))
            cur->sig_pending &= ~SIGBIT(sig);
    }
    return 0;
}

static s64 sys_sigprocmask(lx_u64 how, lx_u64 set, lx_u64 old, lx_u64 size)
{
    if (size != 8) return -EINVAL;
    if ((set && !uok(set, 8, 0)) || (old && !uok(old, 8, 1))) return -EFAULT;
    lx_u64 prev = cur->sig_blocked;
    if (set) {
        lx_u64 s = *(lx_u64 *)(uintptr_t)set;
        if (how == 0) cur->sig_blocked |= s;
        else if (how == 1) cur->sig_blocked &= ~s;
        else if (how == 2) cur->sig_blocked = s;
        else return -EINVAL;
        cur->sig_blocked &= ~UNBLOCKABLE;
    }
    if (old) *(lx_u64 *)(uintptr_t)old = prev;
    return 0;
}

/* Signal frame (Linux arm64 struct rt_sigframe): siginfo (128 bytes),
 * then ucontext: uc_flags, uc_link, uc_stack (24), uc_sigmask (8, padded
 * to 128), then mcontext at +176: fault_address, regs[31], sp, pc,
 * pstate, then __reserved (4096) holding an fpsimd_context record and a
 * terminating empty record. A frame record {fp, lr} follows. */
#define SF_SIGINFO   0
#define SF_UC        128
#define UC_SIGMASK   40
#define UC_MCONTEXT  176
#define MC_REGS      8
#define MC_SP        256
#define MC_PC        264
#define MC_PSTATE    272
#define MC_RESERVED  288
#define FPSIMD_MAGIC 0x46508001u
#define SF_SIZE      (SF_UC + UC_MCONTEXT + MC_RESERVED + 4096)
#define SF_TOTAL     (SF_SIZE + 16)

/* Run the handler for sig on return to EL0. 0, or -1 if the frame
 * didn't fit on the user stack. */
static int setup_frame(linux_tf_t *tf, int sig, const sigact_t *sa)
{
    lx_u64 sp = (tf->sp - SF_TOTAL) & ~15UL;
    if (!uok(sp, SF_TOTAL, 1)) return -1;
    uint8_t *f = (uint8_t *)(uintptr_t)sp;
    kmemset(f, 0, SF_TOTAL);
    int32_t *si = (int32_t *)(f + SF_SIGINFO);
    si[0] = sig;                                      /* si_signo */
    si[2] = 0;                                        /* si_code: SI_USER */
    si[4] = cur->sig_from[sig - 1];                   /* si_pid */
    uint8_t *uc = f + SF_UC, *mc = uc + UC_MCONTEXT;
    *(lx_u64 *)(uc + UC_SIGMASK) = cur->restore_mask ? cur->saved_mask : cur->sig_blocked;
    cur->restore_mask = 0;                            /* sigreturn restores it */
    for (int i = 0; i < 31; i++) ((lx_u64 *)(mc + MC_REGS))[i] = tf->x[i];
    *(lx_u64 *)(mc + MC_SP) = tf->sp;
    *(lx_u64 *)(mc + MC_PC) = tf->elr;
    *(lx_u64 *)(mc + MC_PSTATE) = tf->spsr;
    uint8_t *fp = mc + MC_RESERVED;
    *(uint32_t *)(fp + 0) = FPSIMD_MAGIC;
    *(uint32_t *)(fp + 4) = 528;
    *(uint32_t *)(fp + 8) = (uint32_t)tf->fpsr;
    *(uint32_t *)(fp + 12) = (uint32_t)tf->fpcr;
    kmemcpy(fp + 16, tf->q, sizeof tf->q);            /* then a zero record: end */
    lx_u64 *rec = (lx_u64 *)(f + SF_SIZE);
    rec[0] = tf->x[29];
    rec[1] = tf->elr;
    tf->x[0] = (lx_u64)sig;
    tf->x[1] = sp + SF_SIGINFO;
    tf->x[2] = sp + SF_UC;
    tf->x[29] = sp + SF_SIZE;
    tf->x[30] = (sa->flags & SA_RESTORER) ? sa->restorer : 0;
    tf->sp = sp;
    tf->elr = sa->handler;
    cur->sig_blocked |= sa->mask;
    if (!(sa->flags & SA_NODEFER)) cur->sig_blocked |= SIGBIT(sig);
    cur->sig_blocked &= ~UNBLOCKABLE;
    return 0;
}

static s64 sys_sigreturn(linux_tf_t *tf)
{
    lx_u64 sp = tf->sp;
    if (!uok(sp, SF_SIZE, 0)) { do_exit_group(SIGSEGV); return 0; }
    const uint8_t *uc = (const uint8_t *)(uintptr_t)(sp + SF_UC), *mc = uc + UC_MCONTEXT;
    for (int i = 0; i < 31; i++) tf->x[i] = ((const lx_u64 *)(mc + MC_REGS))[i];
    tf->sp = *(const lx_u64 *)(mc + MC_SP);
    tf->elr = *(const lx_u64 *)(mc + MC_PC);
    tf->spsr = *(const lx_u64 *)(mc + MC_PSTATE) & 0xF0000000UL;   /* NZCV only; EL0t */
    const uint8_t *fp = mc + MC_RESERVED;
    if (*(const uint32_t *)fp == FPSIMD_MAGIC) {
        tf->fpsr = *(const uint32_t *)(fp + 8);
        tf->fpcr = *(const uint32_t *)(fp + 12);
        kmemcpy(tf->q, fp + 16, sizeof tf->q);
    }
    cur->sig_blocked = *(const lx_u64 *)(uc + UC_SIGMASK) & ~UNBLOCKABLE;
    return (s64)tf->x[0];
}

/* On the way back to EL0: act on one deliverable signal. Returns 1 if a
 * handler will run. */
static int deliver_signal(linux_tf_t *tf)
{
    for (;;) {
        lx_u64 ready = cur->sig_pending & ~(cur->sig_blocked & ~UNBLOCKABLE);
        if (!ready) return 0;
        int sig = __builtin_ctzll(ready) + 1;
        cur->sig_pending &= ~SIGBIT(sig);
        sigact_t *sa = &cur->g->sa[sig - 1];
        if (sig == SIGKILL || sig == SIGSTOP || sa->handler == SIG_DFL) {
            if (sig != SIGKILL && sig_default_ignored(sig)) continue;
            if (cur->g->exiting || cur->g->exec_kill) thread_exit();   /* the process is going */
            do_exit_group(sig);
        }
        if (sa->handler == SIG_IGN) continue;
        sigact_t act = *sa;
        if (sa->flags & SA_RESETHAND) kmemset(sa, 0, sizeof *sa);
        if (setup_frame(tf, sig, &act)) {
            kprintf("[LINUX]  pid %d: no room on the stack for a signal frame - killed\n", cur->pid);
            do_exit_group(SIGSEGV);
        }
        return 1;
    }
}

/* ── Pipes ────────────────────────────────────────────────────────────── */
static s64 sys_pipe2(lx_u64 ufds, lx_u64 flags)
{
    if (!uok(ufds, 8, 1)) return -EFAULT;
    pipe_t *p = 0;
    for (int i = 0; i < MAX_PIPES; i++) if (!pipes[i].used) { p = &pipes[i]; break; }
    if (!p) return -ENFILE;
    file_t *r = file_new(F_PIPE_R);
    if (!r) return -ENFILE;
    file_t *w = file_new(F_PIPE_W);
    if (!w) { file_free(r); return -ENFILE; }
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

static s64 pipe_read(file_t *f, uint8_t *buf, lx_u64 n, int nonblock)
{
    pipe_t *p = f->pipe;
    while (!p->count) {
        if (!p->writers) return 0;                       /* end of file */
        if (nonblock) return -EAGAIN;
        if (sig_interrupt()) return -EINTR;
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

static s64 pipe_write(file_t *f, const uint8_t *buf, lx_u64 n, int nonblock)
{
    pipe_t *p = f->pipe;
    lx_u64 k = 0;
    while (k < n) {
        if (!p->readers) {
            if (k) return (s64)k;
            send_sig(cur, SIGPIPE, cur->g->pid);
            return -EPIPE;
        }
        if (p->count == PIPE_SIZE) {
            if (nonblock) return k ? (s64)k : -EAGAIN;
            if (sig_interrupt()) return k ? (s64)k : -EINTR;
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

/* ── eventfd, timerfd, signalfd ───────────────────────────────────────── */
static s64 new_simple_fd(int kind, lx_u64 flags)
{
    file_t *f = file_new(kind);
    if (!f) return -ENFILE;
    f->flags = (int)(flags & L_O_NONBLOCK) | L_O_RDWR;
    return fd_new(f, (flags & L_O_CLOEXEC) != 0);
}

/* Bring a timerfd's expiration count up to date. */
static void timer_update(file_t *f)
{
    if (!f->next) return;
    lx_u64 now = ticks();
    if (now < f->next) return;
    if (f->interval) {
        lx_u64 n = (now - f->next) / f->interval + 1;
        f->count += n;
        f->next += n * f->interval;
    } else {
        f->count++;
        f->next = 0;
    }
}

static s64 sys_timerfd_settime(lx_u64 fd, lx_u64 flags, lx_u64 unew, lx_u64 uold)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_TIMERFD) return -EINVAL;
    if (!uok(unew, 32, 0) || (uold && !uok(uold, 32, 1))) return -EFAULT;
    timer_update(f);
    if (uold) {
        s64 *o = (s64 *)(uintptr_t)uold;
        lx_u64 now = ticks();
        ticks_to_ts(f->interval, o);
        ticks_to_ts(f->next && f->next > now ? f->next - now : 0, o + 2);
    }
    s64 is, ins, vs, vns;
    int r;
    if ((r = get_ts(unew, &is, &ins)) || (r = get_ts(unew + 16, &vs, &vns))) return r;
    f->interval = dur_ticks(is, ins);
    f->count = 0;
    if (!vs && !vns) f->next = 0;                       /* disarm */
    else if (flags & 1) f->next = clock_to_ticks((lx_u64)f->clock, vs, vns);   /* ABSTIME */
    else f->next = ticks() + dur_ticks(vs, vns);
    wakeup(&pollq);
    return 0;
}

static s64 sys_timerfd_gettime(lx_u64 fd, lx_u64 uold)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_TIMERFD) return -EINVAL;
    if (!uok(uold, 32, 1)) return -EFAULT;
    timer_update(f);
    s64 *o = (s64 *)(uintptr_t)uold;
    lx_u64 now = ticks();
    ticks_to_ts(f->interval, o);
    ticks_to_ts(f->next && f->next > now ? f->next - now : 0, o + 2);
    return 0;
}

static s64 sys_signalfd4(s64 fd, lx_u64 umask, lx_u64 size, lx_u64 flags)
{
    if (size != 8) return -EINVAL;
    if (!uok(umask, 8, 0)) return -EFAULT;
    lx_u64 mask = *(lx_u64 *)(uintptr_t)umask & ~UNBLOCKABLE;
    if (fd != -1) {
        file_t *f = fd_get((lx_u64)fd);
        if (!f) return -EBADF;
        if (f->kind != F_SIGNALFD) return -EINVAL;
        f->sigmask = mask;
        return fd;
    }
    s64 r = new_simple_fd(F_SIGNALFD, flags);
    if (r >= 0) cur->g->fd[r]->sigmask = mask;
    return r;
}

/* ── AF_UNIX stream sockets ───────────────────────────────────────────── */
static sock_t *sock_new(void)
{
    for (int i = 0; i < MAX_SOCKS; i++)
        if (!socks[i].used) {
            kmemset(&socks[i], 0, sizeof socks[i]);
            if (!(socks[i].buf = mmu_frames_alloc(SOCK_BUF_PAGES))) return 0;
            socks[i].used = 1;
            socks[i].own_pid = cur ? cur->g->pid : 0;
            return &socks[i];
        }
    return 0;
}

static void sock_free(sock_t *s)
{
    for (int i = 0; i < s->nfdq; i++)                  /* fds nobody received */
        for (int k = 0; k < s->fdq[i].n; k++) file_put(s->fdq[i].f[k]);
    s->nfdq = 0;
    for (int i = 0; i < s->nbacklog; i++) {            /* connections never accepted */
        sock_t *c = s->backlog[i];
        if (c->peer) { c->peer->peer = 0; c->peer->peer_gone = 1; }
        sock_free(c);
    }
    s->nbacklog = 0;
    if (s->buf) mmu_frames_free(s->buf, SOCK_BUF_PAGES);
    s->buf = 0;
    s->used = 0;
}

static void sock_release(sock_t *s)
{
    if (s->peer) {
        s->peer->peer = 0;
        s->peer->peer_gone = 1;
        wakeup(s->peer);
    }
    wakeup(s);
    sock_free(s);
}

static file_t *sock_file(sock_t *s, lx_u64 flags)
{
    file_t *f = file_new(F_SOCK);
    if (!f) return 0;
    f->sock = s;
    f->flags = (int)(flags & L_O_NONBLOCK) | L_O_RDWR;
    return f;
}

static s64 sys_socket(lx_u64 domain, lx_u64 type, lx_u64 proto)
{
    if (domain != 1) return -EAFNOSUPPORT;              /* AF_UNIX only */
    if ((type & 0xF) != 1) return -EPROTOTYPE;          /* SOCK_STREAM only (for now) */
    if (proto) return -EPROTONOSUPPORT;
    sock_t *s = sock_new();
    if (!s) return -ENFILE;
    file_t *f = sock_file(s, type);
    if (!f) { sock_free(s); return -ENFILE; }
    return fd_new(f, (type & L_O_CLOEXEC) != 0);
}

static s64 sys_socketpair(lx_u64 domain, lx_u64 type, lx_u64 proto, lx_u64 usv)
{
    if (domain != 1) return -EAFNOSUPPORT;
    if ((type & 0xF) != 1) return -EPROTOTYPE;
    if (proto) return -EPROTONOSUPPORT;
    if (!uok(usv, 8, 1)) return -EFAULT;
    sock_t *a = sock_new(), *b = a ? sock_new() : 0;
    if (!b) { if (a) sock_free(a); return -ENFILE; }
    a->state = b->state = S_CONN;
    a->peer = b; b->peer = a;
    a->cred_pid = b->cred_pid = cur->g->pid;
    file_t *fa = sock_file(a, type), *fb = fa ? sock_file(b, type) : 0;
    if (!fb) {
        if (fa) file_free(fa);
        a->peer = b->peer = 0;
        sock_free(a); sock_free(b);
        return -ENFILE;
    }
    int cl = (type & L_O_CLOEXEC) != 0;
    int x = fd_install(fa, cl, 0);
    if (x < 0) { file_put(fa); file_put(fb); return -EMFILE; }
    int y = fd_install(fb, cl, 0);
    if (y < 0) { fd_close(cur, x); file_put(fb); return -EMFILE; }
    ((int32_t *)(uintptr_t)usv)[0] = x;
    ((int32_t *)(uintptr_t)usv)[1] = y;
    return 0;
}

static sock_t *fd_sock(lx_u64 fd, s64 *err)
{
    file_t *f = fd_get(fd);
    if (!f) { *err = -EBADF; return 0; }
    if (f->kind != F_SOCK) { *err = -ENOTSOCK; return 0; }
    return f->sock;
}

/* struct sockaddr_un -> name (absolute for paths), 0 or -errno */
static int sun_name(lx_u64 uaddr, lx_u64 len, char *name, uint32_t *nlen)
{
    if (len < 3 || len > 110) return -EINVAL;
    if (!uok(uaddr, len, 0)) return -EFAULT;
    const uint8_t *a = (const uint8_t *)(uintptr_t)uaddr;
    if (*(const uint16_t *)a != 1) return -EAFNOSUPPORT;
    const char *p = (const char *)a + 2;
    uint32_t pl = (uint32_t)len - 2;
    if (p[0] == 0) {                                    /* abstract namespace */
        kmemcpy(name, p, pl);
        *nlen = pl;
        return 0;
    }
    char tmp[110];
    uint32_t k = 0;
    while (k < pl && p[k]) { tmp[k] = p[k]; k++; }
    tmp[k] = 0;
    char abs[VFS_PATH_MAX];
    if (norm_path(cur->g->cwd, tmp, abs) || kstrlen(abs) >= 108) return -EINVAL;
    *nlen = (uint32_t)kstrlen(abs);
    kmemcpy(name, abs, *nlen);
    return 0;
}

static sock_t *sock_by_name(const char *name, uint32_t nlen)
{
    for (int i = 0; i < MAX_SOCKS; i++) {
        sock_t *s = &socks[i];
        if (!s->used || !s->namelen || s->namelen != nlen ||
            (s->state != S_BOUND && s->state != S_LISTEN))
            continue;
        uint32_t k = 0;
        while (k < nlen && s->name[k] == name[k]) k++;
        if (k == nlen) return s;
    }
    return 0;
}

static s64 sys_bind(lx_u64 fd, lx_u64 uaddr, lx_u64 len)
{
    s64 err;
    sock_t *s = fd_sock(fd, &err);
    if (!s) return err;
    if (s->state != S_NEW) return -EINVAL;
    char name[108];
    uint32_t nlen;
    int r = sun_name(uaddr, len, name, &nlen);
    if (r) return r;
    if (sock_by_name(name, nlen)) return -EADDRINUSE;
    kmemcpy(s->name, name, nlen);
    s->namelen = nlen;
    s->state = S_BOUND;
    return 0;
}

static s64 sys_listen(lx_u64 fd)
{
    s64 err;
    sock_t *s = fd_sock(fd, &err);
    if (!s) return err;
    if (s->state != S_BOUND && s->state != S_LISTEN) return -EINVAL;
    s->state = S_LISTEN;
    return 0;
}

static s64 sys_connect(lx_u64 fd, lx_u64 uaddr, lx_u64 len)
{
    s64 err;
    sock_t *s = fd_sock(fd, &err);
    if (!s) return err;
    if (s->state == S_CONN) return -EISCONN;
    if (s->state != S_NEW && s->state != S_BOUND) return -EINVAL;
    char name[108];
    uint32_t nlen;
    int r = sun_name(uaddr, len, name, &nlen);
    if (r) return r;
    sock_t *l = sock_by_name(name, nlen);
    if (!l) return -ENOENT;
    if (l->state != S_LISTEN) return -ECONNREFUSED;
    if (l->nbacklog >= BACKLOG) return -EAGAIN;
    sock_t *srv = sock_new();
    if (!srv) return -ENFILE;
    srv->state = S_PENDING;
    srv->own_pid = group_by_pid(l->own_pid) ? l->own_pid : 0;
    kmemcpy(srv->name, l->name, l->namelen);
    srv->namelen = l->namelen;
    srv->peer = s; s->peer = srv;
    srv->cred_pid = cur->g->pid;
    s->cred_pid = l->own_pid;
    s->state = S_CONN;
    l->backlog[l->nbacklog++] = srv;
    wakeup(l);
    return 0;
}

static s64 sys_accept4(lx_u64 fd, lx_u64 uaddr, lx_u64 ulen, lx_u64 flags)
{
    s64 err;
    file_t *lf = fd_get(fd);
    sock_t *l = fd_sock(fd, &err);
    if (!l) return err;
    if (l->state != S_LISTEN) return -EINVAL;
    if (uaddr && (!uok(ulen, 4, 1) || !uok(uaddr, *(uint32_t *)(uintptr_t)ulen, 1))) return -EFAULT;
    while (!l->nbacklog) {
        if (lf->flags & L_O_NONBLOCK) return -EAGAIN;
        if (sig_interrupt()) return -EINTR;
        sleep_on(l);
        if (!l->used || cur->g->fd[fd] != lf) return -EBADF;
    }
    sock_t *s = l->backlog[0];
    file_t *f = sock_file(s, flags);
    if (!f) return -ENFILE;
    int nfd = fd_install(f, (flags & L_O_CLOEXEC) != 0, 0);
    if (nfd < 0) { file_free(f); return -EMFILE; }
    for (int i = 1; i < l->nbacklog; i++) l->backlog[i - 1] = l->backlog[i];
    l->nbacklog--;
    s->state = S_CONN;
    s->own_pid = cur->g->pid;
    if (s->peer) s->peer->cred_pid = cur->g->pid;
    if (uaddr) {
        uint32_t room = *(uint32_t *)(uintptr_t)ulen;
        if (room >= 2) *(uint16_t *)(uintptr_t)uaddr = 1;
        *(uint32_t *)(uintptr_t)ulen = 2;               /* the client is unnamed */
    }
    return nfd;
}

static s64 sys_sockname(lx_u64 fd, lx_u64 uaddr, lx_u64 ulen, int peer)
{
    s64 err;
    sock_t *s = fd_sock(fd, &err);
    if (!s) return err;
    if (peer && !s->peer) return -ENOTCONN;
    if (!uok(ulen, 4, 1)) return -EFAULT;
    uint32_t room = *(uint32_t *)(uintptr_t)ulen;
    if (!uok(uaddr, room, 1)) return -EFAULT;
    sock_t *n = peer ? (s->peer->state == S_PENDING || s->peer->namelen ? s->peer : 0) : s;
    uint8_t a[110];
    kmemset(a, 0, sizeof a);
    *(uint16_t *)a = 1;
    uint32_t len = 2;
    if (n && n->namelen) {
        kmemcpy(a + 2, n->name, n->namelen);
        len = 2 + n->namelen + (n->name[0] ? 1 : 0);
    }
    kmemcpy((void *)(uintptr_t)uaddr, a, room < len ? room : len);
    *(uint32_t *)(uintptr_t)ulen = len;
    return 0;
}

static s64 sys_getsockopt(lx_u64 fd, lx_u64 level, lx_u64 opt, lx_u64 uval, lx_u64 ulen)
{
    s64 err;
    sock_t *s = fd_sock(fd, &err);
    if (!s) return err;
    if (level != 1) return -ENOPROTOOPT;                /* SOL_SOCKET */
    if (!uok(ulen, 4, 1)) return -EFAULT;
    uint32_t room = *(uint32_t *)(uintptr_t)ulen;
    int32_t v[3] = { 0, 0, 0 };
    uint32_t n = 4;
    switch (opt) {
    case 17:                                            /* SO_PEERCRED: pid, uid, gid */
        if (!s->peer && s->state != S_CONN) return -ENOTCONN;
        v[0] = s->cred_pid; n = 12; break;
    case 3: v[0] = 1; break;                            /* SO_TYPE: SOCK_STREAM */
    case 4: v[0] = 0; break;                            /* SO_ERROR */
    case 7: case 8: v[0] = SOCK_BUF; break;             /* SO_SNDBUF, SO_RCVBUF */
    case 39: v[0] = 1; break;                           /* SO_DOMAIN: AF_UNIX */
    case 30: v[0] = s->state == S_LISTEN; break;        /* SO_ACCEPTCONN */
    default: return -ENOPROTOOPT;
    }
    if (room < n) n = room;
    if (!uok(uval, n, 1)) return -EFAULT;
    kmemcpy((void *)(uintptr_t)uval, v, n);
    *(uint32_t *)(uintptr_t)ulen = n;
    return 0;
}

static s64 sys_shutdown(lx_u64 fd, lx_u64 how)
{
    s64 err;
    sock_t *s = fd_sock(fd, &err);
    if (!s) return err;
    if (how > 2) return -EINVAL;
    if (s->state != S_CONN) return -ENOTCONN;
    if (how != 0) {                                     /* SHUT_WR / SHUT_RDWR */
        s->shut_wr = 1;
        if (s->peer) { s->peer->peer_gone = 1; wakeup(s->peer); }
    }
    return 0;
}

/* Send n bytes (and nf files, attached to the first byte sent). */
static s64 sock_send(file_t *f, const uint8_t *buf, lx_u64 n, file_t **fds, int nf, lx_u64 flags)
{
    sock_t *s = f->sock;
    int nonblock = (f->flags & L_O_NONBLOCK) || (flags & MSG_DONTWAIT);
    if (s->state != S_CONN) return -ENOTCONN;
    lx_u64 k = 0;
    for (;;) {
        sock_t *p = s->peer;
        if (!p || s->shut_wr) {
            if (k) return (s64)k;
            if (!(flags & MSG_NOSIGNAL)) send_sig(cur, SIGPIPE, cur->g->pid);
            return -EPIPE;
        }
        lx_u64 room = SOCK_BUF - (p->wr - p->rd);
        int fdq_full = nf && k == 0 && p->nfdq >= SOCK_FDQ;
        if (k == n && (k || !nf)) break;
        if (!room || fdq_full) {
            if (k) break;
            if (nonblock) return -EAGAIN;
            if (sig_interrupt()) return -EINTR;
            sleep_on(s);
            continue;
        }
        lx_u64 c = n - k < room ? n - k : room;
        if (nf && k == 0) {
            fdmsg_t *m = &p->fdq[p->nfdq++];
            m->start = p->wr;
            m->end = p->wr + c;
            m->n = nf;
            for (int i = 0; i < nf; i++) { m->f[i] = fds[i]; fds[i]->refs++; }
        }
        for (lx_u64 i = 0; i < c; i++) p->buf[(p->wr + i) % SOCK_BUF] = buf[k + i];
        p->wr += c;
        k += c;
        wakeup(p);
        if (k == n) break;
    }
    return (s64)k;
}

/* Receive up to n bytes; files that came with them go to *fds (at most
 * maxf; the rest are closed and *trunc set). */
static s64 sock_recv(file_t *f, uint8_t *buf, lx_u64 n, file_t **fds, int maxf, int *nf,
                     int *trunc, lx_u64 flags)
{
    sock_t *s = f->sock;
    int nonblock = (f->flags & L_O_NONBLOCK) || (flags & MSG_DONTWAIT);
    *nf = 0;
    *trunc = 0;
    if (s->state != S_CONN) return s->state == S_LISTEN ? -EINVAL : -ENOTCONN;
    while (s->wr == s->rd) {
        if (s->peer_gone || !s->peer) return 0;          /* end of stream */
        if (nonblock) return -EAGAIN;
        if (sig_interrupt()) return -EINTR;
        sleep_on(s);
    }
    lx_u64 avail = s->wr - s->rd, c = n < avail ? n : avail;
    fdmsg_t *m = s->nfdq ? &s->fdq[0] : 0;
    int take = 0;
    if (m && m->start < s->rd + c) {
        if (m->start > s->rd) c = m->start - s->rd;     /* data before the fds first */
        else {
            take = 1;
            if (m->end - s->rd < c) c = m->end - s->rd; /* stop after the fds' message */
        }
    }
    for (lx_u64 i = 0; i < c; i++) buf[i] = s->buf[(s->rd + i) % SOCK_BUF];
    if (flags & MSG_PEEK) return (s64)c;
    s->rd += c;
    if (take) {
        for (int i = 0; i < m->n; i++) {
            if (*nf < maxf) fds[(*nf)++] = m->f[i];
            else { file_put(m->f[i]); *trunc = 1; }
        }
        for (int i = 1; i < s->nfdq; i++) s->fdq[i - 1] = s->fdq[i];
        s->nfdq--;
    }
    if (s->peer) wakeup(s->peer);
    return (s64)c;
}

/* sendmsg / recvmsg: struct msghdr { name, namelen, iov, iovlen, control,
 * controllen, flags } (56 bytes); cmsghdr { len, level, type } (16). */
static s64 sys_sendmsg(lx_u64 fd, lx_u64 umsg, lx_u64 flags)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_SOCK) return -ENOTSOCK;
    if (!uok(umsg, 56, 0)) return -EFAULT;
    const lx_u64 *mh = (const lx_u64 *)(uintptr_t)umsg;
    lx_u64 iov = mh[2], iovlen = mh[3], ctl = mh[4], ctllen = mh[5];
    if (iovlen > 1024 || !uok(iov, iovlen * 16, 0)) return -EFAULT;
    file_t *fds[SCM_MAX];
    int nf = 0;
    if (ctl && ctllen) {
        if (!uok(ctl, ctllen, 0)) return -EFAULT;
        for (lx_u64 off = 0; off + 16 <= ctllen; ) {
            const uint8_t *c = (const uint8_t *)(uintptr_t)(ctl + off);
            lx_u64 clen = *(const lx_u64 *)c;
            int level = *(const int32_t *)(c + 8), type = *(const int32_t *)(c + 12);
            if (clen < 16 || off + clen > ctllen) return -EINVAL;
            if (level == 1 && type == 1) {               /* SOL_SOCKET, SCM_RIGHTS */
                lx_u64 cnt = (clen - 16) / 4;
                for (lx_u64 i = 0; i < cnt; i++) {
                    int32_t x = ((const int32_t *)(c + 16))[i];
                    file_t *t = x >= 0 ? fd_get((lx_u64)x) : 0;
                    if (!t) return -EBADF;
                    if (nf >= SCM_MAX) return -EINVAL;
                    fds[nf++] = t;
                }
            } else if (level == 1 && type == 2) {        /* SCM_CREDENTIALS: ignored */
            } else {
                return -EINVAL;
            }
            off += (clen + 7) & ~7UL;
        }
    }
    s64 total = 0;
    for (lx_u64 i = 0; i < iovlen; i++) {
        lx_u64 base = ((const lx_u64 *)(uintptr_t)iov)[2 * i];
        lx_u64 len = ((const lx_u64 *)(uintptr_t)iov)[2 * i + 1];
        if (!len) continue;
        if (!uok(base, len, 0)) return total ? total : -EFAULT;
        s64 r = sock_send(f, (const uint8_t *)(uintptr_t)base, len, fds, nf, flags);
        if (r < 0) return total ? total : r;
        nf = 0;                                          /* sent with the first bytes */
        total += r;
        if ((lx_u64)r < len) break;
    }
    if (nf) {                                            /* no data at all: send the fds alone */
        s64 r = sock_send(f, (const uint8_t *)"", 0, fds, nf, flags);
        if (r < 0) return r;
    }
    return total;
}

static s64 sys_recvmsg(lx_u64 fd, lx_u64 umsg, lx_u64 flags)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->kind != F_SOCK) return -ENOTSOCK;
    if (!uok(umsg, 56, 1)) return -EFAULT;
    lx_u64 *mh = (lx_u64 *)(uintptr_t)umsg;
    lx_u64 iov = mh[2], iovlen = mh[3], ctl = mh[4], ctllen = mh[5];
    if (iovlen > 1024 || !uok(iov, iovlen * 16, 0)) return -EFAULT;
    if (ctl && ctllen && !uok(ctl, ctllen, 1)) return -EFAULT;
    int maxf = ctl && ctllen >= 16 ? (int)((ctllen - 16) / 4) : 0;
    if (maxf > SCM_MAX) maxf = SCM_MAX;
    /* One read into the first non-empty iovec (stream data; enough for
     * every caller that matters), then scatter. */
    static uint8_t kbuf[SOCK_BUF];
    lx_u64 want = 0;
    for (lx_u64 i = 0; i < iovlen; i++) {
        lx_u64 base = ((lx_u64 *)(uintptr_t)iov)[2 * i], len = ((lx_u64 *)(uintptr_t)iov)[2 * i + 1];
        if (len && !uok(base, len, 1)) return -EFAULT;
        want += len;
    }
    if (want > SOCK_BUF) want = SOCK_BUF;
    file_t *fds[SCM_MAX];
    int nf, trunc;
    s64 r = sock_recv(f, kbuf, want, fds, maxf, &nf, &trunc, flags);
    if (r < 0) return r;
    lx_u64 done = 0;
    for (lx_u64 i = 0; i < iovlen && done < (lx_u64)r; i++) {
        lx_u64 base = ((lx_u64 *)(uintptr_t)iov)[2 * i], len = ((lx_u64 *)(uintptr_t)iov)[2 * i + 1];
        lx_u64 c = (lx_u64)r - done < len ? (lx_u64)r - done : len;
        kmemcpy((void *)(uintptr_t)base, kbuf + done, c);
        done += c;
    }
    int got = 0;
    int32_t nums[SCM_MAX];
    for (int i = 0; i < nf; i++) {
        int x = fd_install(fds[i], (flags & MSG_CMSG_CLOEXEC) != 0, 0);
        if (x < 0) { file_put(fds[i]); trunc = 1; continue; }
        nums[got++] = x;
    }
    int mflags = trunc ? MSG_CTRUNC : 0;
    if (got) {
        uint8_t *c = (uint8_t *)(uintptr_t)ctl;
        *(lx_u64 *)c = 16 + 4 * (lx_u64)got;
        *(int32_t *)(c + 8) = 1;
        *(int32_t *)(c + 12) = 1;
        kmemcpy(c + 16, nums, 4 * (lx_u64)got);
        mh[5] = (16 + 4 * (lx_u64)got + 7) & ~7UL;
        if (mh[5] > ctllen) mh[5] = 16 + 4 * (lx_u64)got;
    } else {
        mh[5] = 0;
    }
    mh[1] = 0;                                           /* msg_namelen */
    ((int32_t *)(uintptr_t)umsg)[12] = mflags;           /* msg_flags (+48) */
    return r;
}

/* ── Readiness (poll, epoll) ──────────────────────────────────────────── */
static uint32_t f_poll(file_t *f, int depth);

static uint32_t ep_ready(epoll_t *ep, int depth)
{
    uint32_t any = 0;
    for (int i = 0; i < ep->n; i++) {
        uint32_t want = ep->it[i].events | PERR | PHUP;
        if (!ep->it[i].disarmed && (f_poll(ep->it[i].f, depth + 1) & want)) { any = 1; break; }
    }
    return any ? PIN : 0;
}

static uint32_t f_poll(file_t *f, int depth)
{
    switch (f->kind) {
    case F_CONSOLE:
        return (f->tty->hung_up ? PHUP | PIN : 0) | POUT | (tty_readable(f->tty) ? PIN : 0);
    case F_PTMX: {
        tty_t *t = f->tty;
        return (t->on ? PIN : 0) | (t->slave_gone && !t->slaves ? PHUP : 0) | POUT;
    }
    case F_PIPE_R:
        return (f->pipe->count ? PIN : 0) | (!f->pipe->writers ? PHUP : 0);
    case F_PIPE_W:
        return (f->pipe->count < PIPE_SIZE ? POUT : 0) | (!f->pipe->readers ? PERR : 0);
    case F_EVENTFD:
        return (f->count ? PIN : 0) | (f->count < 0xFFFFFFFFFFFFFFFEULL ? POUT : 0);
    case F_TIMERFD:
        timer_update(f);
        return f->count ? PIN : 0;
    case F_SIGNALFD:
        return (cur->sig_pending & f->sigmask) ? PIN : 0;
    case F_EPOLL:
        return depth < 4 ? ep_ready(f->ep, depth) : 0;
    case F_EVDEV:
        return ev_ready(f->vfd) ? PIN : 0;
    case F_FB:
        return POUT;
    case F_SOCK: {
        sock_t *s = f->sock;
        if (s->state == S_LISTEN) return s->nbacklog ? PIN : 0;
        if (s->state != S_CONN) return POUT | PHUP;
        uint32_t m = 0;
        if (s->wr != s->rd) m |= PIN;
        if (s->peer_gone || !s->peer) m |= PIN | PRDHUP | (!s->peer ? PHUP : 0);
        if (s->peer && !s->shut_wr && s->peer->wr - s->peer->rd < SOCK_BUF) m |= POUT;
        return m;
    }
    default:
        return PIN | POUT;                              /* files, /dev: always ready */
    }
}

/* How long a waiter may sleep for f: soonest timer, short slices for the
 * console (polled, no interrupt). 0 = no limit. */
static lx_u64 f_deadline(file_t *f, int depth)
{
    if (f->kind == F_TIMERFD) return f->next;
    if (f->kind == F_CONSOLE && f->tty->pump) return ticks() + freq() / 100;   /* polled console */
    if (f->kind == F_EVDEV) return ticks() + freq() / 200;           /* polled devices */
    if (f->kind == F_EPOLL && depth < 4) {
        lx_u64 d = 0;
        for (int i = 0; i < f->ep->n; i++) {
            lx_u64 x = f_deadline(f->ep->it[i].f, depth + 1);
            if (x && (!d || x < d)) d = x;
        }
        return d;
    }
    return 0;
}

static lx_u64 dmin(lx_u64 a, lx_u64 b) { return !a ? b : !b ? a : a < b ? a : b; }

/* Temporarily replace the signal mask for a wait (ppoll, pselect6,
 * epoll_pwait, rt_sigsuspend). The old one comes back on the way out to
 * EL0 - after a handler, through its frame (linux_el0_trap). */
static int swap_mask(lx_u64 umask, lx_u64 size)
{
    if (!umask) return 0;
    if (size != 8) return -EINVAL;
    if (!uok(umask, 8, 0)) return -EFAULT;
    if (!cur->restore_mask) { cur->saved_mask = cur->sig_blocked; cur->restore_mask = 1; }
    cur->sig_blocked = *(lx_u64 *)(uintptr_t)umask & ~UNBLOCKABLE;
    return 0;
}

static s64 sys_ppoll(lx_u64 ufds, lx_u64 nfds, lx_u64 uts, lx_u64 umask, lx_u64 msize)
{
    if (nfds > 256) return -EINVAL;
    if (nfds && !uok(ufds, nfds * 8, 1)) return -EFAULT;
    lx_u64 deadline = 0;
    int timeout_zero = 0;
    if (uts) {
        s64 sec, ns;
        int r = get_ts(uts, &sec, &ns);
        if (r) return r;
        if (!sec && !ns) timeout_zero = 1;
        else deadline = ticks() + dur_ticks(sec, ns);
    }
    int r = swap_mask(umask, msize);
    if (r) return r;
    s64 ret;
    for (;;) {
        int ready = 0;
        lx_u64 wake = deadline;
        for (lx_u64 i = 0; i < nfds; i++) {
            uint8_t *e = (uint8_t *)(uintptr_t)(ufds + 8 * i);
            int32_t fd = *(int32_t *)e;
            uint16_t ev = *(uint16_t *)(e + 4), rev = 0;
            if (fd >= 0) {
                file_t *f = fd_get((lx_u64)fd);
                if (!f) rev = PNVAL;
                else {
                    rev = (uint16_t)(f_poll(f, 0) & (ev | PERR | PHUP | PNVAL));
                    wake = dmin(wake, f_deadline(f, 0));
                }
            }
            *(uint16_t *)(e + 6) = rev;
            if (rev) ready++;
        }
        if (ready || timeout_zero) { ret = ready; break; }
        if (deadline && ticks() >= deadline) { ret = 0; break; }
        if (sig_interrupt()) { ret = -EINTR; break; }
        sleep_until(&pollq, wake);
    }
    return ret;
}

static s64 sys_pselect6(lx_u64 n, lx_u64 rd, lx_u64 wr, lx_u64 ex, lx_u64 uts, lx_u64 usig)
{
    if (n > MAX_FDS) n = MAX_FDS;
    lx_u64 bytes = (n + 7) / 8;
    lx_u64 *sets[3] = { (lx_u64 *)(uintptr_t)rd, (lx_u64 *)(uintptr_t)wr, (lx_u64 *)(uintptr_t)ex };
    for (int k = 0; k < 3; k++)
        if (sets[k] && !uok((lx_u64)(uintptr_t)sets[k], bytes, 1)) return -EFAULT;
    lx_u64 in[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) if (sets[k]) kmemcpy(&in[k], sets[k], bytes);
    lx_u64 deadline = 0;
    int zero = 0;
    if (uts) {
        s64 sec, ns;
        int r = get_ts(uts, &sec, &ns);
        if (r) return r;
        if (!sec && !ns) zero = 1; else deadline = ticks() + dur_ticks(sec, ns);
    }
    lx_u64 um = 0, us = 0;
    if (usig) {                                        /* { const sigset_t *ss; size_t len; } */
        if (!uok(usig, 16, 0)) return -EFAULT;
        um = ((lx_u64 *)(uintptr_t)usig)[0];
        us = ((lx_u64 *)(uintptr_t)usig)[1];
    }
    int r = swap_mask(um, us);
    if (r) return r;
    s64 ret;
    for (;;) {
        lx_u64 out[3] = { 0, 0, 0 };
        int ready = 0;
        lx_u64 wake = deadline;
        for (lx_u64 fd = 0; fd < n; fd++) {
            if (!((in[0] | in[1] | in[2]) >> fd & 1)) continue;
            file_t *f = fd_get(fd);
            if (!f) return -EBADF;
            uint32_t m = f_poll(f, 0);
            wake = dmin(wake, f_deadline(f, 0));
            if ((in[0] >> fd & 1) && (m & (PIN | PHUP | PERR))) { out[0] |= 1ULL << fd; ready++; }
            if ((in[1] >> fd & 1) && (m & (POUT | PERR))) { out[1] |= 1ULL << fd; ready++; }
            if ((in[2] >> fd & 1) && (m & PPRI)) { out[2] |= 1ULL << fd; ready++; }
        }
        if (ready || zero || (deadline && ticks() >= deadline)) {
            for (int k = 0; k < 3; k++) if (sets[k]) kmemcpy(sets[k], &out[k], bytes);
            ret = ready;
            break;
        }
        if (sig_interrupt()) { ret = -EINTR; break; }
        sleep_until(&pollq, wake);
    }
    return ret;
}

static s64 sys_epoll_create1(lx_u64 flags)
{
    epoll_t *ep = 0;
    for (int i = 0; i < MAX_EPOLLS; i++) if (!epolls[i].used) { ep = &epolls[i]; break; }
    if (!ep) return -ENFILE;
    file_t *f = file_new(F_EPOLL);
    if (!f) return -ENFILE;
    kmemset(ep, 0, sizeof *ep);
    ep->used = 1;
    f->ep = ep;
    f->flags = L_O_RDWR;
    return fd_new(f, (flags & L_O_CLOEXEC) != 0);
}

static s64 sys_epoll_ctl(lx_u64 epfd, lx_u64 op, lx_u64 fd, lx_u64 uev)
{
    file_t *e = fd_get(epfd), *f = fd_get(fd);
    if (!e || !f) return -EBADF;
    if (e->kind != F_EPOLL || e == f) return -EINVAL;
    if (f->kind == F_FILE || f->kind == F_DIR) return -EPERM;   /* as Linux: regular files */
    epoll_t *ep = e->ep;
    int at = -1;
    for (int i = 0; i < ep->n; i++) if (ep->it[i].f == f && ep->it[i].fd == (int)fd) at = i;
    uint32_t events = 0;
    lx_u64 data = 0;
    if (op != 2) {                                      /* not EPOLL_CTL_DEL */
        if (!uok(uev, 16, 0)) return -EFAULT;          /* { u32 events; u64 data } - not packed on arm64 */
        events = *(uint32_t *)(uintptr_t)uev;
        data = *(lx_u64 *)(uintptr_t)(uev + 8);
    }
    switch (op) {
    case 1:                                             /* ADD */
        if (at >= 0) return -EEXIST;
        if (ep->n >= EPOLL_ITEMS) return -ENOSPC;
        ep->it[ep->n++] = (epitem_t){ f, (int)fd, events, data, 0 };
        break;
    case 2:                                             /* DEL */
        if (at < 0) return -ENOENT;
        ep->it[at] = ep->it[--ep->n];
        break;
    case 3:                                             /* MOD */
        if (at < 0) return -ENOENT;
        ep->it[at].events = events;
        ep->it[at].data = data;
        ep->it[at].disarmed = 0;
        break;
    default: return -EINVAL;
    }
    wakeup(&pollq);
    return 0;
}

static s64 sys_epoll_pwait(lx_u64 epfd, lx_u64 uev, lx_u64 max, s64 timeout_ms, lx_u64 umask,
                           lx_u64 msize)
{
    file_t *e = fd_get(epfd);
    if (!e) return -EBADF;
    if (e->kind != F_EPOLL || (s64)max <= 0 || max > 1024) return -EINVAL;
    if (!uok(uev, max * 16, 1)) return -EFAULT;
    lx_u64 deadline = timeout_ms > 0 ? ticks() + (lx_u64)timeout_ms * freq() / 1000 : 0;
    int r = swap_mask(umask, msize);
    if (r) return r;
    s64 ret;
    for (;;) {
        epoll_t *ep = e->ep;
        int n = 0;
        lx_u64 wake = deadline;
        for (int i = 0; i < ep->n && (lx_u64)n < max; i++) {
            epitem_t *it = &ep->it[i];
            if (it->disarmed) continue;
            /* EPOLLERR / EPOLLHUP are reported even when not asked for */
            uint32_t m = f_poll(it->f, 1) & (it->events | PERR | PHUP);
            wake = dmin(wake, f_deadline(it->f, 1));
            if (!m) continue;
            uint8_t *o = (uint8_t *)(uintptr_t)(uev + 16 * (lx_u64)n);
            *(uint32_t *)o = m;
            *(lx_u64 *)(o + 8) = it->data;
            n++;
            if (it->events & EPOLLONESHOT) it->disarmed = 1;
        }
        if (n || timeout_ms == 0) { ret = n; break; }
        if (deadline && ticks() >= deadline) { ret = 0; break; }
        if (sig_interrupt()) { ret = -EINTR; break; }
        sleep_until(&pollq, wake);
        if (cur->g->fd[epfd] != e) { ret = -EBADF; break; }
    }
    return ret;
}

/* ── POSIX shared memory: /dev/shm/<name> (musl's shm_open) ──────────── */
#define MAX_SHM 32
static struct { char name[64]; memobj_t *m; } shm_names[MAX_SHM];

/* "/dev/shm/x" -> "x" (one component), else NULL */
static const char *shm_name(const char *path)
{
    const char *pre = "/dev/shm/";
    for (int i = 0; pre[i]; i++) if (path[i] != pre[i]) return 0;
    const char *n = path + 9;
    if (!*n || kstrlen(n) >= 64) return 0;
    for (const char *c = n; *c; c++) if (*c == '/') return 0;
    return n;
}

static int shm_find(const char *n)
{
    for (int i = 0; i < MAX_SHM; i++) if (shm_names[i].m && streq(shm_names[i].name, n)) return i;
    return -1;
}

static s64 shm_open_path(const char *n, lx_u64 flags)
{
    int i = shm_find(n);
    if (i >= 0 && (flags & L_O_CREAT) && (flags & L_O_EXCL)) return -EEXIST;
    if (i < 0) {
        if (!(flags & L_O_CREAT)) return -ENOENT;
        for (i = 0; i < MAX_SHM && shm_names[i].m; i++) { }
        if (i == MAX_SHM) return -ENFILE;
        if (!(shm_names[i].m = mem_new())) return -ENFILE;   /* the name's reference */
        kstrncpy(shm_names[i].name, n, sizeof shm_names[i].name);
    } else if (flags & L_O_TRUNC) {
        int r = mem_resize(shm_names[i].m, 0);
        if (r) return r;
    }
    file_t *f = file_new(F_MEMFD);
    if (!f) return -ENFILE;
    f->mem = shm_names[i].m;
    f->mem->refs++;
    f->flags = (int)(flags & (L_O_ACCMODE | L_O_NONBLOCK));
    kstrncpy(f->path, "/dev/shm/", sizeof f->path);
    kstrncpy(f->path + 9, n, sizeof f->path - 10);
    return fd_new(f, (flags & L_O_CLOEXEC) != 0);
}

static void shm_unlink_index(int i)
{
    memobj_t *m = shm_names[i].m;
    shm_names[i].m = 0;
    mem_put(m);
}

/* ── File system calls ────────────────────────────────────────────────── */
static s64 sys_openat(int dirfd, lx_u64 up, lx_u64 flags)
{
    char path[VFS_PATH_MAX];
    int r = user_path(dirfd, up, path);
    if (r) return r;
    int cl = (flags & L_O_CLOEXEC) != 0;
    const char *sn = shm_name(path);
    if (sn) return shm_open_path(sn, flags);
    /* /dev */
    static const struct { const char *name; int kind; } devs[] = {
        { "/dev/null", F_NULL }, { "/dev/zero", F_ZERO }, { "/dev/urandom", F_RANDOM },
        { "/dev/random", F_RANDOM }, { "/dev/tty", F_CONSOLE }, { "/dev/console", F_CONSOLE },
        { "/dev/ptmx", F_PTMX },
        { "/dev/fb0", F_FB }, { "/dev/input/event0", F_EVDEV }, { "/dev/input/event1", F_EVDEV },
    };
    for (unsigned i = 0; i < sizeof devs / sizeof devs[0]; i++)
        if (streq(path, devs[i].name)) {
            if (devs[i].kind == F_FB) { int w, h; lx_u64 sz; if (fb_geom(&w, &h, &sz)) return -ENODEV; }
            tty_t *t = 0;
            if (devs[i].kind == F_CONSOLE) {           /* /dev/tty: the controlling terminal */
                t = path[5] == 't' ? cur->g->ctty : &console_tty;
                if (!t) return -6;                     /* ENXIO */
            }
            if (devs[i].kind == F_PTMX && !(t = pty_alloc())) return -28;   /* ENOSPC */
            file_t *f = file_new(devs[i].kind);
            if (!f) { if (devs[i].kind == F_PTMX) pty_release(t); return -ENFILE; }
            f->tty = t;
            if (devs[i].kind == F_PTMX) t->masters++;
            if (t && devs[i].kind == F_CONSOLE && t != &console_tty) t->slaves++;
            f->flags = (int)(flags & (L_O_ACCMODE | L_O_NONBLOCK));
            if (devs[i].kind == F_EVDEV) {
                f->vfd = path[16] - '0';                /* event0 / event1 */
                evh[f->vfd] = evt[f->vfd];             /* start with nothing old */
            }
            kstrncpy(f->path, path, sizeof f->path);
            return fd_new(f, cl);
        }
    const char *pre = "/dev/pts/";
    int is_pts = 1;
    for (int i = 0; pre[i]; i++) if (path[i] != pre[i]) is_pts = 0;
    if (is_pts && path[9] >= '0' && path[9] <= '9' && !path[10]) {     /* a pty's slave */
        tty_t *t = &ptys[path[9] - '0'];
        if (path[9] - '0' >= MAX_PTYS || !t->used || t->hung_up) return -ENOENT;
        if (t->locked) return -5;                       /* EIO, as Linux */
        file_t *f = file_new(F_CONSOLE);
        if (!f) return -ENFILE;
        f->tty = t;
        f->flags = (int)(flags & (L_O_ACCMODE | L_O_NONBLOCK));
        kstrncpy(f->path, path, sizeof f->path);
        t->slaves++;
        t->slave_gone = 0;
        /* A session leader without a terminal gets this one (no O_NOCTTY). */
        if (!(flags & 0400) && !cur->g->ctty && cur->g->sid == cur->g->pid && !t->sid) {
            cur->g->ctty = t;
            t->sid = cur->g->sid;
            t->pgrp = cur->g->pgid;
        }
        return fd_new(f, cl);
    }
    if (streq(path, "/dev") || (path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
                                path[3] == 'v' && path[4] == '/'))
        return -ENOENT;

    vfs_stat_t st;
    kmemset(&st, 0, sizeof st);
    int is_root = path[0] == '/' && !path[1];
    int exists = is_root || vfs_stat(path, &st) == VFS_OK;
    if (is_root) st.attrs = VFS_ATTR_DIR;
    if (exists && (flags & L_O_CREAT) && (flags & L_O_EXCL)) return -EEXIST;
    if ((flags & L_O_DIRECTORY) || (exists && (st.attrs & VFS_ATTR_DIR))) {
        if (!exists) return -ENOENT;
        if (!(st.attrs & VFS_ATTR_DIR)) return -ENOTDIR;
        if ((flags & L_O_ACCMODE) != 0) return -EISDIR;
        file_t *f = file_new(F_DIR);
        if (!f) return -ENFILE;
        int d = vfs_opendir(path);
        if (d < 0) { file_free(f); return verr(d); }
        f->vfd = d;
        kstrncpy(f->path, path, sizeof f->path);
        return fd_new(f, cl);
    }
    file_t *f = file_new(F_FILE);
    if (!f) return -ENFILE;
    int vf = vfs_open(path, (int)(flags & (L_O_ACCMODE | L_O_CREAT | L_O_TRUNC | L_O_APPEND)));
    if (vf < 0) { file_free(f); return verr(vf); }
    f->vfd = vf;
    f->flags = (int)(flags & (L_O_ACCMODE | L_O_APPEND | L_O_NONBLOCK));
    kstrncpy(f->path, path, sizeof f->path);
    return fd_new(f, cl);
}

static s64 sys_memfd_create(lx_u64 uname, lx_u64 flags)
{
    char name[VFS_PATH_MAX - 8];
    int r = copy_in_str(name, uname, 250);
    if (r) return r == -ERANGE ? -EINVAL : r;
    if (flags & ~7UL) return -EINVAL;                   /* CLOEXEC 1, ALLOW_SEALING 2, HUGETLB 4 */
    memobj_t *m = mem_new();
    if (!m) return -ENFILE;
    file_t *f = file_new(F_MEMFD);
    if (!f) { mem_put(m); return -ENFILE; }
    f->mem = m;
    f->flags = L_O_RDWR;
    kstrncpy(f->path, "/memfd:", sizeof f->path);
    kstrncpy(f->path + 7, name, sizeof f->path - 8);
    return fd_new(f, (flags & 1) != 0);
}

static s64 do_truncate(file_t *f, const char *path, s64 len)
{
    if (len < 0) return -EINVAL;
    if (f && f->kind == F_MEMFD) return mem_resize(f->mem, (lx_u64)len);
    if (f && f->kind != F_FILE) return -EINVAL;
    const char *rel = tmp_rel(f ? f->path : path);
    if (rel) {
        if (len > 0x7FFFFFFF) return -EFBIG;
        int r = ramfs_truncate(rel, (uint32_t)len);
        return r == VFS_OK ? 0 : verr(r);
    }
    /* Other file systems: only truncating to the current size, or to 0
     * through a reopen with O_TRUNC. */
    vfs_stat_t vs;
    int r = vfs_stat(f ? f->path : path, &vs);
    if (r != VFS_OK) return verr(r);
    if ((lx_u64)len == vs.size) return 0;
    if (len == 0 && !f) {
        int vf = vfs_open(path, O_WRONLY | O_TRUNC);
        if (vf < 0) return verr(vf);
        vfs_close(vf);
        return 0;
    }
    return -EINVAL;
}

static s64 dev_read(file_t *f, uint8_t *b, lx_u64 n)
{
    if (f->kind == F_NULL) return 0;
    for (lx_u64 i = 0; i < n; i++) b[i] = f->kind == F_ZERO ? 0 : (uint8_t)(rnd() >> 24);
    return (s64)n;
}

static s64 sys_read(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!uok(buf, n, 1)) return -EFAULT;
    uint8_t *b = (uint8_t *)(uintptr_t)buf;
    int nb = (f->flags & L_O_NONBLOCK) != 0;
    switch (f->kind) {
    case F_CONSOLE: return n ? tty_read(f->tty, (char *)b, n, nb) : 0;
    case F_PTMX:    return n ? ptm_read(f, (char *)b, n) : 0;
    case F_PIPE_R:  return n ? pipe_read(f, b, n, nb) : 0;
    case F_PIPE_W:  return -EBADF;
    case F_DIR:     return -EISDIR;
    case F_NULL: case F_ZERO: case F_RANDOM: return dev_read(f, b, n);
    case F_EVDEV:   return ev_read(f, b, n);
    case F_FB:      return -EINVAL;
    case F_MEMFD:   return mem_rw(f->mem, &f->pos, b, n, 0);
    case F_EPOLL:   return -EINVAL;
    case F_SOCK: {
        int nf, tr;
        return n ? sock_recv(f, b, n, 0, 0, &nf, &tr, 0) : 0;
    }
    case F_EVENTFD:
        if (n < 8) return -EINVAL;
        while (!f->count) {
            if (nb) return -EAGAIN;
            if (sig_interrupt()) return -EINTR;
            sleep_on(f);
        }
        *(lx_u64 *)b = f->semaphore ? 1 : f->count;
        f->count = f->semaphore ? f->count - 1 : 0;
        wakeup(f);
        return 8;
    case F_TIMERFD:
        if (n < 8) return -EINVAL;
        for (;;) {
            timer_update(f);
            if (f->count) break;
            if (nb) return -EAGAIN;
            if (sig_interrupt()) return -EINTR;
            sleep_until(f, f->next ? f->next : 0);
        }
        *(lx_u64 *)b = f->count;
        f->count = 0;
        return 8;
    case F_SIGNALFD: {
        if (n < 128) return -EINVAL;
        while (!(cur->sig_pending & f->sigmask)) {
            if (nb) return -EAGAIN;
            if (sig_interrupt()) return -EINTR;
            sleep_on(&pollq);
        }
        lx_u64 k = 0;
        while (k + 128 <= n && (cur->sig_pending & f->sigmask)) {
            int sig = __builtin_ctzll(cur->sig_pending & f->sigmask) + 1;
            cur->sig_pending &= ~SIGBIT(sig);
            uint8_t *si = b + k;
            kmemset(si, 0, 128);
            *(uint32_t *)si = (uint32_t)sig;             /* ssi_signo */
            *(uint32_t *)(si + 12) = (uint32_t)cur->sig_from[sig - 1];   /* ssi_pid */
            k += 128;
        }
        return (s64)k;
    }
    default: {
        if (!n) return 0;
        int r = vfs_read(f->vfd, b, (uint32_t)(n > 0x40000000 ? 0x40000000 : n));
        return r < 0 ? verr(r) : r;
    }
    }
}

static s64 sys_write(lx_u64 fd, lx_u64 buf, lx_u64 n)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (!uok(buf, n, 0)) return -EFAULT;
    const uint8_t *b = (const uint8_t *)(uintptr_t)buf;
    int nb = (f->flags & L_O_NONBLOCK) != 0;
    switch (f->kind) {
    case F_CONSOLE:
        if (f->tty->hung_up) return -EIO;
        tty_output(f->tty, (const char *)b, n);
        return (s64)n;
    case F_PTMX:    return ptm_write(f, (const char *)b, n);
    case F_PIPE_W:  return n ? pipe_write(f, b, n, nb) : 0;
    case F_PIPE_R:  return -EBADF;
    case F_DIR:     return -EISDIR;
    case F_NULL: case F_ZERO: case F_RANDOM: return (s64)n;
    case F_MEMFD:   return mem_rw(f->mem, &f->pos, (uint8_t *)b, n, 1);
    case F_SOCK:    return n ? sock_send(f, b, n, 0, 0, 0) : 0;
    case F_EVENTFD: {
        if (n < 8) return -EINVAL;
        lx_u64 v = *(const lx_u64 *)b;
        if (v == 0xFFFFFFFFFFFFFFFFULL) return -EINVAL;
        while (f->count + v > 0xFFFFFFFFFFFFFFFEULL || f->count + v < f->count) {
            if (nb) return -EAGAIN;
            if (sig_interrupt()) return -EINTR;
            sleep_on(f);
        }
        f->count += v;
        wakeup(f);
        return 8;
    }
    case F_TIMERFD: case F_SIGNALFD: case F_EPOLL: return -EINVAL;
    default: {
        if (!n) return 0;
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
    if (whence > 2) return -EINVAL;
    if (f->kind == F_MEMFD) {
        s64 base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : (s64)f->mem->size;
        if (base + off < 0) return -EINVAL;
        f->pos = (lx_u64)(base + off);
        return (s64)f->pos;
    }
    if (f->kind == F_NULL || f->kind == F_ZERO || f->kind == F_RANDOM) return 0;
    if (f->kind != F_FILE) return -ESPIPE;
    if (off > 0x7FFFFFFF || off < -0x7FFFFFFFLL) return -EINVAL;
    int r = vfs_seek(f->vfd, (int32_t)off, (int)whence);
    return r < 0 ? verr(r) : r;
}

static s64 sys_pread(lx_u64 fd, lx_u64 buf, lx_u64 n, s64 off, int wr)
{
    file_t *f = fd_get(fd);
    if (!f) return -EBADF;
    if (off < 0) return -EINVAL;
    if (f->kind == F_MEMFD) {
        if (!uok(buf, n, !wr)) return -EFAULT;
        lx_u64 pos = (lx_u64)off;
        return mem_rw(f->mem, &pos, (uint8_t *)(uintptr_t)buf, n, wr);
    }
    if (f->kind != F_FILE) return -ESPIPE;
    int here = vfs_seek(f->vfd, 0, SEEK_CUR);
    if (here < 0) return verr(here);
    s64 r = sys_lseek(fd, off, 0);
    if (r >= 0) r = wr ? sys_write(fd, buf, n) : sys_read(fd, buf, n);
    vfs_seek(f->vfd, here, SEEK_SET);
    return r;
}

static s64 sys_fstatat(int dirfd, lx_u64 up, lx_u64 stp, lx_u64 flags, int by_fd)
{
    if (!uok(stp, 128, 1)) return -EFAULT;
    vfs_stat_t vs;
    kmemset(&vs, 0, sizeof vs);
    uint32_t kind = ST_VFS;
    lx_u64 size = 0;
    char path[VFS_PATH_MAX];
    if (!by_fd && (flags & AT_EMPTY_PATH) && uok(up, 1, 0) && !*(const char *)(uintptr_t)up)
        by_fd = 1;
    if (by_fd) {
        file_t *f = fd_get((lx_u64)dirfd);
        if (!f) return -EBADF;
        switch (f->kind) {
        case F_CONSOLE: case F_PTMX: case F_NULL: case F_ZERO: case F_RANDOM: case F_FB: case F_EVDEV:
            kind = ST_CHR; break;
        case F_PIPE_R: case F_PIPE_W: kind = ST_FIFO; break;
        case F_SOCK: kind = ST_SOCK; break;
        case F_MEMFD: kind = ST_MEM; size = f->mem->size; break;
        case F_EVENTFD: case F_TIMERFD: case F_SIGNALFD: case F_EPOLL: kind = ST_ANON; break;
        default:
            if (f->path[0] == '/' && !f->path[1]) vs.attrs = VFS_ATTR_DIR;
            else {
                int r = vfs_stat(f->path, &vs);
                if (r != VFS_OK) return verr(r);
            }
        }
    } else {
        int r = user_path(dirfd, up, path);
        if (r) return r;
        if (path[0] == '/' && !path[1]) vs.attrs = VFS_ATTR_DIR;
        else if (streq(path, "/dev") || streq(path, "/dev/shm") || streq(path, "/dev/input"))
            vs.attrs = VFS_ATTR_DIR;
        else if (streq(path, "/dev/fb0") || streq(path, "/dev/input/event0") ||
                 streq(path, "/dev/input/event1") || streq(path, "/dev/ptmx")) kind = ST_CHR;
        else if (streq(path, "/dev/pts")) vs.attrs = VFS_ATTR_DIR;
        else if (path[0] == '/' && path[1] == 'd' && path[5] == 'p' && path[6] == 't' &&
                 path[7] == 's' && path[8] == '/' && path[9] >= '0' && path[9] < '0' + MAX_PTYS &&
                 !path[10]) {
            if (!ptys[path[9] - '0'].used) return -ENOENT;
            kind = ST_CHR;
        }
        else if (shm_name(path)) {
            int i = shm_find(shm_name(path));
            if (i < 0) return -ENOENT;
            kind = ST_MEM;
            size = shm_names[i].m->size;
        }
        else if (streq(path, "/dev/null") || streq(path, "/dev/zero") ||
                 streq(path, "/dev/urandom") || streq(path, "/dev/random") ||
                 streq(path, "/dev/tty") || streq(path, "/dev/console")) kind = ST_CHR;
        else if ((r = vfs_stat(path, &vs)) != VFS_OK) return verr(r);
    }
    fill_stat((uint8_t *)(uintptr_t)stp, &vs, kind, size);
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
    if (req == 0x5421) {                               /* FIONBIO */
        if (!uok(arg, 4, 0)) return -EFAULT;
        if (*(int32_t *)(uintptr_t)arg) f->flags |= L_O_NONBLOCK; else f->flags &= ~L_O_NONBLOCK;
        return 0;
    }
    if (req == 0x5451) { cur->g->cloexec |= 1ULL << fd; return 0; }    /* FIOCLEX */
    if (req == 0x5450) { cur->g->cloexec &= ~(1ULL << fd); return 0; } /* FIONCLEX */
    if (req == 0x541B) {                               /* FIONREAD */
        if (!uok(arg, 4, 1)) return -EFAULT;
        int32_t v = 0;
        if (f->kind == F_PIPE_R) v = (int32_t)f->pipe->count;
        else if (f->kind == F_SOCK) v = (int32_t)(f->sock->wr - f->sock->rd);
        else if (f->kind == F_CONSOLE || f->kind == F_PTMX) return tty_ioctl(f, f->tty, req, arg);
        else return -ENOTTY;
        *(int32_t *)(uintptr_t)arg = v;
        return 0;
    }
    if (f->kind == F_FB) return fb_ioctl(req, arg);
    if (f->kind == F_EVDEV) return ev_ioctl(f, req, arg);
    if (f->kind != F_CONSOLE && f->kind != F_PTMX) return -ENOTTY;
    return tty_ioctl(f, f->tty, req, arg);
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
    if (flags & ~L_O_CLOEXEC) return -EINVAL;
    f->refs++;                                         /* before closing new: old may == f */
    if (cur->g->fd[new]) fd_close(cur, (int)new);
    cur->g->fd[new] = f;
    if (flags & L_O_CLOEXEC) cur->g->cloexec |= 1ULL << new;
    else cur->g->cloexec &= ~(1ULL << new);
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
    case 1: return (cur->g->cloexec >> fd) & 1;           /* F_GETFD */
    case 2:                                            /* F_SETFD */
        if (arg & 1) cur->g->cloexec |= 1ULL << fd; else cur->g->cloexec &= ~(1ULL << fd);
        return 0;
    case 3: return f->flags & (L_O_ACCMODE | L_O_APPEND | L_O_NONBLOCK);   /* F_GETFL */
    case 4: f->flags = (int)((f->flags & ~L_O_NONBLOCK) | (arg & L_O_NONBLOCK)); return 0;
    case 1033: return f->kind == F_MEMFD ? 0 : -EINVAL;   /* F_ADD_SEALS: accepted */
    case 1034: return f->kind == F_MEMFD ? 0 : -EINVAL;   /* F_GET_SEALS: none */
    case 5: case 6: case 7: return 0;                  /* record locks: single user */
    default: return -EINVAL;
    }
}

static s64 sys_clock_gettime(lx_u64 clk, lx_u64 tsp)
{
    if (!uok(tsp, 16, 1)) return -EFAULT;
    time_init();
    lx_u64 t = ticks(), f = freq();
    s64 sec, nsec;
    if (clock_realtime(clk)) {                         /* REALTIME (+ coarse, alarm) */
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

static s64 do_sleep(lx_u64 end, lx_u64 urem)
{
    while (ticks() < end) {
        if (sig_interrupt()) {
            if (urem && uok(urem, 16, 1)) ticks_to_ts(end - ticks(), (s64 *)(uintptr_t)urem);
            return -EINTR;
        }
        sleep_until(cur, end);
    }
    return 0;
}

static s64 sys_nanosleep(lx_u64 req, lx_u64 rem)
{
    s64 sec, ns;
    int r = get_ts(req, &sec, &ns);
    if (r) return r;
    return do_sleep(ticks() + dur_ticks(sec, ns), rem);
}

static s64 sys_clock_nanosleep(lx_u64 clk, lx_u64 flags, lx_u64 req, lx_u64 rem)
{
    s64 sec, ns;
    int r = get_ts(req, &sec, &ns);
    if (r) return r;
    if (flags & 1) return do_sleep(clock_to_ticks(clk, sec, ns), 0);   /* TIMER_ABSTIME */
    return do_sleep(ticks() + dur_ticks(sec, ns), rem);
}

static s64 sys_uname(lx_u64 up)
{
    if (!uok(up, 6 * 65, 1)) return -EFAULT;
    static const char *const f[6] = { "Linux", "os2warp", "6.1.0-os2warp-linux-abi",
                                      "#3 stage3", "aarch64", "(none)" };
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
        lx_u64 n = kstrlen(cur->g->cwd) + 1;
        if (a1 < n) return -ERANGE;
        if (!uok(a0, n, 1)) return -EFAULT;
        kmemcpy((void *)(uintptr_t)a0, cur->g->cwd, n);
        return (s64)n;
    }
    case 19: {                                          /* eventfd2 */
        s64 r = new_simple_fd(F_EVENTFD, a1);
        if (r >= 0) { cur->g->fd[r]->count = (uint32_t)a0; cur->g->fd[r]->semaphore = (a1 & 1) != 0; }
        return r;
    }
    case 20: return sys_epoll_create1(a0);
    case 21: return sys_epoll_ctl(a0, a1, a2, a3);
    case 22: return sys_epoll_pwait(a0, a1, a2, (s64)(int32_t)a3, a4, a5);
    case 23: return sys_dup3(a0, 0, 0, 0);
    case 24: return sys_dup3(a0, a1, a2, 1);
    case 25: return sys_fcntl(a0, a1, a2);
    case 29: return sys_ioctl(a0, a1, a2);
    case 32: return fd_get(a0) ? 0 : -EBADF;            /* flock: one user, always granted */
    case 34: {                                          /* mkdirat */
        char p[VFS_PATH_MAX];
        int r = user_path((int)a0, a1, p);
        if (r) return r;
        vfs_stat_t vs;
        if (vfs_stat(p, &vs) == VFS_OK) return -EEXIST;
        r = vfs_mkdir(p);
        return r == VFS_OK ? 0 : verr(r);
    }
    case 35: {                                          /* unlinkat */
        char p[VFS_PATH_MAX];
        int r = user_path((int)a0, a1, p);
        if (r) return r;
        const char *sn = shm_name(p);
        if (sn) {                                       /* shm_unlink */
            int i = shm_find(sn);
            if (i < 0) return -ENOENT;
            shm_unlink_index(i);
            return 0;
        }
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
    case 45: {                                          /* truncate */
        char p[VFS_PATH_MAX];
        int r = user_path(AT_FDCWD, a0, p);
        if (r) return r;
        return do_truncate(0, p, (s64)a1);
    }
    case 46: {                                          /* ftruncate */
        file_t *f = fd_get(a0);
        if (!f) return -EBADF;
        return do_truncate(f, 0, (s64)a1);
    }
    case 47: {                                          /* fallocate (mode 0): grow only */
        file_t *f = fd_get(a0);
        if (!f) return -EBADF;
        if (a1 || f->kind != F_MEMFD) return f->kind == F_MEMFD ? -EOPNOTSUPP : 0;
        if ((s64)a2 < 0 || (s64)a3 <= 0) return -EINVAL;
        if (a2 + a3 > f->mem->size) return mem_resize(f->mem, a2 + a3);
        return 0;
    }
    case 48: case 439: {                                /* faccessat, faccessat2 */
        char p[VFS_PATH_MAX];
        vfs_stat_t vs;
        int r = user_path((int)a0, a1, p);
        if (r) return r;
        if ((p[0] == '/' && !p[1]) || streq(p, "/dev") || streq(p, "/dev/null") ||
            streq(p, "/dev/fb0") || streq(p, "/dev/input/event0") || streq(p, "/dev/input/event1"))
            return 0;
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
        kstrncpy(cur->g->cwd, p, sizeof cur->g->cwd);
        return 0;
    }
    case 50: {                                          /* fchdir */
        file_t *f = fd_get(a0);
        if (!f) return -EBADF;
        if (f->kind != F_DIR) return -ENOTDIR;
        kstrncpy(cur->g->cwd, f->path, sizeof cur->g->cwd);
        return 0;
    }
    case 52: case 53: case 54: case 55: return 0;       /* fchmod(at), fchownat, fchown */
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
    case 67: return sys_pread(a0, a1, a2, (s64)a3, 0);
    case 68: return sys_pread(a0, a1, a2, (s64)a3, 1);
    case 71: {                                          /* sendfile(out, in, offset *, count) */
        static uint8_t kb[4096];
        file_t *in = fd_get(a1);
        if (!in || !fd_get(a0)) return -EBADF;
        s64 saved = -1;
        if (a2) {                                       /* from *offset, leaving the position alone */
            if (!uok(a2, 8, 1)) return -EFAULT;
            if (in->kind != F_FILE) return -ESPIPE;
            saved = vfs_seek(in->vfd, 0, SEEK_CUR);
            if (vfs_seek(in->vfd, (int32_t)*(s64 *)(uintptr_t)a2, SEEK_SET) < 0) return -EINVAL;
        }
        s64 total = 0;
        while ((lx_u64)total < a3) {
            lx_u64 want = a3 - (lx_u64)total < sizeof kb ? a3 - (lx_u64)total : sizeof kb;
            s64 r;
            if (in->kind == F_FILE) { int g = vfs_read(in->vfd, kb, (uint32_t)want); r = g < 0 ? verr(g) : g; }
            else if (in->kind == F_MEMFD) r = mem_rw(in->mem, &in->pos, kb, want, 0);
            else { r = -EINVAL; }
            if (r <= 0) { if (!total) total = r; break; }
            /* the write side goes through the normal paths (user memory not needed) */
            file_t *out = fd_get(a0);
            s64 w;
            if (out->kind == F_FILE) { int g = vfs_write(out->vfd, kb, (uint32_t)r); w = g < 0 ? verr(g) : g; }
            else if (out->kind == F_PIPE_W) w = pipe_write(out, kb, (lx_u64)r, (out->flags & L_O_NONBLOCK) != 0);
            else if (out->kind == F_CONSOLE) { tty_output(out->tty, (const char *)kb, (lx_u64)r); w = r; }
            else if (out->kind == F_SOCK) w = sock_send(out, kb, (lx_u64)r, 0, 0, 0);
            else if (out->kind == F_MEMFD) w = mem_rw(out->mem, &out->pos, kb, (lx_u64)r, 1);
            else w = -EINVAL;
            if (w < 0) { if (!total) total = w; break; }
            total += w;
            if (w < r) break;
        }
        if (a2) {
            if (total > 0) *(s64 *)(uintptr_t)a2 += total;
            vfs_seek(in->vfd, (int32_t)saved, SEEK_SET);
        }
        return total;
    }
    case 72: return sys_pselect6(a0, a1, a2, a3, a4, a5);
    case 73: return sys_ppoll(a0, a1, a2, a3, a4);
    case 74: return sys_signalfd4((s64)(int32_t)a0, a1, a2, a3);
    case 78: {                                          /* readlinkat: /proc/self/exe only */
        char in[VFS_PATH_MAX], p2[VFS_PATH_MAX];
        int r = copy_in_str(in, a1, sizeof in);
        if (r) return r;
        if (norm_path(cur->g->cwd, in, p2)) return -EINVAL;
        const char *target = 0;
        if (streq(p2, "/proc/self/exe")) target = cur->g->exe;
        const char *fdp = "/proc/self/fd/";
        int isfd = 1;
        for (int i = 0; fdp[i]; i++) if (p2[i] != fdp[i]) isfd = 0;
        if (isfd && p2[14] >= '0' && p2[14] <= '9') {  /* /proc/self/fd/N: what it is */
            int n = 0;
            for (const char *c = p2 + 14; *c; c++) {
                if (*c < '0' || *c > '9') return -ENOENT;
                n = n * 10 + (*c - '0');
            }
            file_t *f = fd_get((lx_u64)n);
            if (!f) return -ENOENT;
            target = f->path[0] ? f->path : f->kind == F_PIPE_R || f->kind == F_PIPE_W ? "pipe:[0]"
                   : f->kind == F_SOCK ? "socket:[0]" : "anon_inode:[unknown]";
        }
        if (target) {
            lx_u64 n = kstrlen(target);
            if (n > a3) n = a3;
            if (!uok(a2, n, 1)) return -EFAULT;
            kmemcpy((void *)(uintptr_t)a2, target, n);
            return (s64)n;
        }
        vfs_stat_t vs;
        return vfs_stat(p2, &vs) == VFS_OK || (p2[0] == '/' && !p2[1]) ? -EINVAL : -ENOENT;
    }
    case 79: return sys_fstatat((int)a0, a1, a2, a3, 0);
    case 80: return sys_fstatat((int)a0, 0, a1, 0, 1);
    case 81: return 0;                                  /* sync */
    case 82: case 83: return fd_get(a0) ? 0 : -EBADF;   /* fsync, fdatasync */
    case 85: {                                          /* timerfd_create */
        if (a0 != 0 && a0 != 1 && a0 != 7 && a0 != 8 && a0 != 9) return -EINVAL;
        s64 r = new_simple_fd(F_TIMERFD, a1);
        if (r >= 0) cur->g->fd[r]->clock = (int)a0;
        return r;
    }
    case 86: return sys_timerfd_settime(a0, a1, a2, a3);
    case 87: return sys_timerfd_gettime(a0, a1);
    case 88: return 0;                                  /* utimensat */
    case 93: do_exit((int)((a0 & 0xFF) << 8)); return 0;          /* exit: this thread */
    case 94: do_exit_group((int)((a0 & 0xFF) << 8)); return 0;    /* exit_group */
    case 96: cur->clear_tid = a0; return cur->pid;      /* set_tid_address -> tid */
    case 98: return sys_futex(a0, a1, a2, a3, a4, a5);
    case 123: {                                         /* sched_getaffinity: one CPU */
        if (a1 < 8) return -EINVAL;
        if (!uok(a2, 8, 1)) return -EFAULT;
        *(lx_u64 *)(uintptr_t)a2 = 1;
        return 8;
    }
    case 99: return 0;                                  /* set_robust_list */
    case 101: return sys_nanosleep(a0, a1);
    case 113: return sys_clock_gettime(a0, a1);
    case 114: {                                         /* clock_getres */
        if (a1) {
            if (!uok(a1, 16, 1)) return -EFAULT;
            ((s64 *)(uintptr_t)a1)[0] = 0;
            ((s64 *)(uintptr_t)a1)[1] = 1;
        }
        return 0;
    }
    case 115: return sys_clock_nanosleep(a0, a1, a2, a3);
    case 124: yield(); return 0;                        /* sched_yield */
    case 129: return sys_kill((s64)a0, a1);             /* kill */
    case 130: return sys_tgkill(-1, (s64)a0, a1);       /* tkill */
    case 131: return sys_tgkill((s64)a0, (s64)a1, a2);  /* tgkill */
    case 132: return 0;                                 /* sigaltstack: handlers use the normal stack */
    case 133: {                                         /* rt_sigsuspend */
        if (!a0) return -EFAULT;
        int r = swap_mask(a0, a1);
        if (r) return r;
        while (!sig_interrupt()) sleep_on(&pollq);
        return -EINTR;
    }
    case 134: return sys_sigaction(a0, a1, a2, a3);
    case 135: return sys_sigprocmask(a0, a1, a2, a3);
    case 136: {                                         /* rt_sigpending */
        if (!uok(a0, 8, 1)) return -EFAULT;
        *(lx_u64 *)(uintptr_t)a0 = cur->sig_pending & cur->sig_blocked;
        return 0;
    }
    case 139: return sys_sigreturn(tf);
    case 140: case 141: return 0;                       /* setpriority, getpriority */
    case 153: return 0;                                 /* times */
    case 154: {                                         /* setpgid(pid, pgid) */
        group_t *g = a0 ? group_by_pid((int)a0) : cur->g;
        if (!g || (g != cur->g && g->ppid != cur->g->pid)) return -ESRCH;
        if ((s64)a1 < 0) return -EINVAL;
        if (g->sid == g->pid && g->pgid == g->pid && a1 != (lx_u64)g->pid) return -EPERM;  /* session leader */
        g->pgid = a1 ? (int)a1 : g->pid;
        return 0;
    }
    case 155: case 156: {                               /* getpgid, getsid */
        group_t *g = a0 ? group_by_pid((int)a0) : cur->g;
        if (!g) return -ESRCH;
        return tf->x[8] == 155 ? g->pgid : g->sid;
    }
    case 157:                                           /* setsid */
        if (cur->g->pgid == cur->g->pid) return -EPERM;
        for (int i = 0; i < MAX_GROUPS; i++)            /* (nobody else may use that group id) */
            if (groups[i].state == G_LIVE && &groups[i] != cur->g && groups[i].pgid == cur->g->pid)
                return -EPERM;
        cur->g->sid = cur->g->pgid = cur->g->pid;
        cur->g->ctty = 0;
        return cur->g->sid;
    case 160: return sys_uname(a0);
    case 165: {                                         /* getrusage: zeros */
        if (!uok(a1, 144, 1)) return -EFAULT;
        kmemset((void *)(uintptr_t)a1, 0, 144);
        return 0;
    }
    case 166: return 022;                               /* umask */
    case 167: return 0;                                 /* prctl */
    case 169: {                                         /* gettimeofday */
        if (a0) {
            s64 ts[2];
            if (!uok(a0, 16, 1)) return -EFAULT;
            sys_clock_gettime(0, (lx_u64)(uintptr_t)ts);
            ((s64 *)(uintptr_t)a0)[0] = ts[0];
            ((s64 *)(uintptr_t)a0)[1] = ts[1] / 1000;
        }
        return 0;
    }
    case 172: return cur->g->pid;                       /* getpid */
    case 173: return cur->g->ppid;                         /* getppid */
    case 178: return cur->pid;                          /* gettid */
    case 174: case 175: case 176: case 177: return 0;  /* getuid, geteuid, getgid, getegid */
    case 198: return sys_socket(a0, a1, a2);
    case 199: return sys_socketpair(a0, a1, a2, a3);
    case 200: return sys_bind(a0, a1, a2);
    case 201: return sys_listen(a0);
    case 202: return sys_accept4(a0, a1, a2, 0);
    case 242: return sys_accept4(a0, a1, a2, a3);
    case 203: return sys_connect(a0, a1, a2);
    case 204: return sys_sockname(a0, a1, a2, 0);
    case 205: return sys_sockname(a0, a1, a2, 1);
    case 206: {                                         /* sendto (address ignored: connected) */
        file_t *f = fd_get(a0);
        if (!f) return -EBADF;
        if (f->kind != F_SOCK) return -ENOTSOCK;
        if (!uok(a1, a2, 0)) return -EFAULT;
        return sock_send(f, (const uint8_t *)(uintptr_t)a1, a2, 0, 0, a3);
    }
    case 207: {                                         /* recvfrom */
        file_t *f = fd_get(a0);
        if (!f) return -EBADF;
        if (f->kind != F_SOCK) return -ENOTSOCK;
        if (!uok(a1, a2, 1)) return -EFAULT;
        if (a4 && a5 && uok(a5, 4, 1)) *(uint32_t *)(uintptr_t)a5 = 0;
        int nf, tr;
        return sock_recv(f, (uint8_t *)(uintptr_t)a1, a2, 0, 0, &nf, &tr, a3);
    }
    case 208: {                                         /* setsockopt: accepted */
        s64 err;
        return fd_sock(a0, &err) ? 0 : err;
    }
    case 209: return sys_getsockopt(a0, a1, a2, a3, a4);
    case 210: return sys_shutdown(a0, a1);
    case 211: return sys_sendmsg(a0, a1, a2);
    case 212: return sys_recvmsg(a0, a1, a2);
    case 214: return sys_brk(a0);
    case 215:                                           /* munmap */
        if ((a0 & (PAGE - 1)) || a0 < MMU_USER_BASE || a0 + a1 > STACK_BOTTOM) return -EINVAL;
        mmu_as_unmap(cur->g->as, a0, a1);
        return 0;
    case 216: {                                         /* mremap */
        lx_u64 old = page_up(a1), new = page_up(a2);
        if ((a0 & (PAGE - 1)) || !new || a0 < MMAP_BASE || a0 + old > MMAP_LIMIT ||
            !mmu_user_mapped(a0, old, MMU_PROT_READ | MMU_PROT_WRITE))
            return -EINVAL;
        if (new <= old) { mmu_as_unmap(cur->g->as, a0 + new, old - new); return (s64)a0; }
        if (a0 + old == cur->g->mmap_next && a0 + new <= MMAP_LIMIT) {     /* grow in place */
            if (mmu_as_map(cur->g->as, a0 + old, new - old, MMU_PROT_READ | MMU_PROT_WRITE))
                return -ENOMEM;
            cur->g->mmap_next = a0 + new;
            return (s64)a0;
        }
        if (!(a3 & 1)) return -ENOMEM;                  /* MREMAP_MAYMOVE */
        s64 at = sys_mmap(0, new, 3, 0x22, -1, 0);      /* RW, private anonymous */
        if (at < 0) return at;
        kmemcpy((void *)(uintptr_t)at, (void *)(uintptr_t)a0, old);
        mmu_as_unmap(cur->g->as, a0, old);
        return at;
    }
    case 220: return sys_clone(a0, a1, a2, a3, a4);     /* clone(flags, sp, ptid, tls, ctid) */
    case 221: return sys_execve(a0, a1, a2);
    case 222: return sys_mmap(a0, a1, a2, a3, (s64)a4, a5);
    case 226: {                                         /* mprotect */
        if ((a0 & (PAGE - 1)) || a0 < MMU_USER_BASE || a0 + a1 > USER_TOP || a0 + a1 < a0)
            return -EINVAL;
        int p = prot_bits(a2);
        if ((p & MMU_PROT_WRITE) && (p & MMU_PROT_EXEC)) return -EPERM;   /* W^X */
        if (!p) return 0;                                /* PROT_NONE: left as it is */
        /* PROT_NONE anonymous memory isn't mapped here (mmap leaves it out):
         * making it accessible brings in zeroed pages - what it held. musl
         * reserves thread stacks that way, then mprotects them. */
        for (lx_u64 a = a0; a < a0 + page_up(a1); a += PAGE)
            if (!mmu_user_mapped(a, 1, MMU_PROT_READ)) {
                if (a < MMAP_BASE || a >= MMAP_LIMIT) return -ENOMEM;
                if (mmu_as_map(cur->g->as, a, PAGE, MMU_PROT_READ | MMU_PROT_WRITE)) return -ENOMEM;
            }
        mmu_as_protect(cur->g->as, a0, a1, p);
        return 0;
    }
    case 227: case 228: case 229: case 233: return 0;  /* msync, mlock, munlock, madvise */
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
    case 279: return sys_memfd_create(a0, a1);
    case 291: return -ENOSYS;                           /* statx: musl falls back to fstatat */
    case 441: return sys_epoll_pwait(a0, a1, a2,        /* epoll_pwait2: timespec timeout */
                                     !a3 ? -1 : uok(a3, 16, 0) ?
                                     ((s64 *)(uintptr_t)a3)[0] * 1000 +
                                     ((s64 *)(uintptr_t)a3)[1] / 1000000 : 0, a4, a5);
    default:
        kprintf("[LINUX]  pid %d: unimplemented syscall %u (pc 0x%x)\n", cur->pid,
                (unsigned)tf->x[8], (unsigned)(tf->elr & 0xFFFFFFFF));
        return -ENOSYS;
    }
}

/* ── Scheduler tick ───────────────────────────────────────────────────── */
#define SLICE_MS 10

static void tick_arm(void)
{
    lx_u64 t = freq() * SLICE_MS / 1000;
    __asm__ volatile("msr cntp_tval_el0, %0\n\tmsr cntp_ctl_el0, %1\n\tisb" :: "r"(t), "r"(1UL));
}

static void tick_start(void)
{
    gicv3_ppi_setup(GICV3_PPI_PHYS_TIMER, 1);
    tick_arm();
}

static void tick_stop(void)
{
    __asm__ volatile("msr cntp_ctl_el0, %0\n\tisb" :: "r"(0UL));
    gicv3_ppi_setup(GICV3_PPI_PHYS_TIMER, 0);
}

/* IRQ taken from EL0 (el0_irq_entry). */
void linux_el0_irq(linux_tf_t *tf)
{
    cur->tf = tf;
    lx_u64 intid;
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(intid));      /* ICC_IAR1_EL1 */
    intid &= 0xFFFFFF;
    if (intid >= 1020) return;                                     /* spurious */
    if (intid == GICV3_PPI_PHYS_TIMER) tick_arm();                 /* (nothing else is routed) */
    if (console_tty.used) con_pump(&console_tty);                  /* typed keys, Ctrl-C */
    __asm__ volatile("msr S3_0_C12_C12_1, %0\n\tisb" :: "r"(intid)); /* ICC_EOIR1_EL1 */
    if (usbd_daemon) usbd_daemon();
    schedule();                                                    /* time's up: next one */
    deliver_signal(cur->tf);
}

void linux_el0_trap(linux_tf_t *tf)
{
    check_kstack(cur);
    lx_u64 esr, far;
    __asm__ volatile("mrs %0, esr_el1\n\tmrs %1, far_el1" : "=r"(esr), "=r"(far));
    cur->tf = tf;
    uint32_t ec = (uint32_t)(esr >> 26);
    if (ec == 0x15) {                                   /* SVC from AArch64 */
        lx_u64 nr = tf->x[8], orig_x0 = tf->x[0];
        s64 r = linux_syscall(tf);
        tf = cur->tf;
        tf->x[0] = (lx_u64)r;
        if (r == -EINTR && nr != 139) {
            /* Restart the call after a handler with SA_RESTART, or when no
             * handler runs at all (the signal was ignored after all). */
            lx_u64 ready = cur->sig_pending & ~(cur->sig_blocked & ~UNBLOCKABLE);
            int sig = ready ? __builtin_ctzll(ready) + 1 : 0;
            int restart = !sig || (cur->g->sa[sig - 1].handler > SIG_IGN &&
                                   (cur->g->sa[sig - 1].flags & SA_RESTART));
            if (sig && (cur->g->sa[sig - 1].handler == SIG_IGN ||
                        (cur->g->sa[sig - 1].handler == SIG_DFL && sig_default_ignored(sig))))
                restart = 1;
            if (restart && nr != 101 && nr != 115 && nr != 133 && nr != 73 && nr != 22 &&
                nr != 72 && nr != 441) {               /* sleeps and waits report EINTR */
                tf->x[0] = orig_x0;
                tf->elr -= 4;
            }
        }
        deliver_signal(tf);
        if (cur->restore_mask) { cur->sig_blocked = cur->saved_mask; cur->restore_mask = 0; }
        return;
    }
    const char *what = ec == 0x24 ? "data abort" : ec == 0x20 ? "instruction abort" :
                       ec == 0x22 ? "PC alignment fault" : ec == 0x26 ? "SP alignment fault" :
                       ec == 0x00 ? "undefined instruction" : "exception";
    kprintf("[LINUX]  pid %d: %s at pc 0x%x, address 0x%x (ESR 0x%x, lr 0x%x, sp 0x%x) - killed (%s)\n",
            cur->pid, what, (unsigned)(tf->elr & 0xFFFFFFFF), (unsigned)(far & 0xFFFFFFFF),
            (unsigned)esr, (unsigned)(tf->x[30] & 0xFFFFFFFF), (unsigned)(tf->sp & 0xFFFFFFFF),
            ec == 0x00 ? "SIGILL" : "SIGSEGV");
    do_exit_group(ec == 0x00 ? SIGILL : SIGSEGV);
}

/* ── Entry from the shell ─────────────────────────────────────────────── */
int linux_run(const char *path, int argc, char **argv)
{
    if (cur) { kprintf("[LINUX]  already running\n"); return -1; }
    static int tmp_mounted;
    if (!tmp_mounted && vfs_mount("/tmp", &vfs_ramfs, 0) == VFS_OK)   /* over any /tmp below */
        tmp_mounted = 1;
    static const char *const envp[] = { "PATH=/", "HOME=/", "TERM=vt100", "LANG=C",
                                        "XDG_RUNTIME_DIR=/tmp", 0 };
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
    unsigned long ram0 = ramfs_frames();
    frames_kept = 0;
    proc_t *init = proc_alloc();
    group_t *ig = init ? group_alloc() : 0;
    if (!ig) {
        if (init) proc_discard(init);
        kprintf("[LINUX]  out of memory\n");
        return -1;
    }
    init->g = ig;
    ig->pid = init->pid;
    ig->nthreads = 1;
    mmu_as_t *as;
    lx_u64 entry, sp, top;
    int r = exec_image(path, argbuf, argc, envc, &as, &entry, &sp, &top);
    mmu_as_activate(0);
    if (r) {
        kprintf("[LINUX]  %s: can't run (errno %d)\n", path, -r);
        proc_discard(init);
        ig->state = G_FREE;
        next_pid = 1;
        return -1;
    }
    init->g->as = as;
    init->g->ppid = 0;
    init->g->brk_start = init->g->brk_cur = top;
    init->g->mmap_next = MMAP_BASE;
    kstrncpy(init->g->cwd, "/", sizeof init->g->cwd);
    kstrncpy(init->g->exe, path, sizeof init->g->exe);
    tty_defaults(&console_tty);
    console_tty.used = 1;
    console_tty.out = con_out;
    console_tty.pump = con_pump;
    init->g->pgid = init->g->sid = init->g->pid;      /* a session, the console its terminal */
    init->g->ctty = &console_tty;
    console_tty.sid = console_tty.pgrp = init->g->pid;
    for (int i = 0; i < 3; i++) {
        file_t *f = file_new(F_CONSOLE);
        if (!f) break;
        f->flags = i ? 1 : 2;
        f->tty = &console_tty;
        kstrncpy(f->path, "/dev/console", sizeof f->path);
        init->g->fd[i] = f;
    }
    linux_tf_t *tf = new_frame(init);
    kmemset(tf, 0, sizeof *tf);
    tf->elr = entry;
    tf->sp = sp;
    tf->spsr = 0;                                       /* EL0t, interrupts on */
    init->state = P_RUN;
    tick_start();

    /* The shell's context is the idle loop: run whatever is runnable
     * until the first process has exited. */
    for (;;) {
        if (ig->state == G_ZOMBIE) break;
        proc_t *p = pick_runnable(0);
        if (p) { switch_to(p); continue; }
        /* Nothing runnable: wait for a deadline, a key (Ctrl-C reaches
         * the foreground process group), or USB. */
        if (console_tty.used) con_pump(&console_tty);
        if (usbd_daemon) usbd_daemon();
        __asm__ volatile("yield");
    }
    tick_stop();
    int status = ig->wstatus;
    /* Whatever is left: kill it, let it exit, reap everything. */
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == P_RUN || procs[i].state == P_SLEEP) {
            send_sig(&procs[i], SIGKILL, 0);
            procs[i].state = P_RUN;
        }
    for (proc_t *p; (p = pick_runnable(0)) != 0; ) switch_to(p);
    for (int i = 0; i < MAX_GROUPS; i++) groups[i].state = G_FREE;   /* zombies nobody reaped */
    cur = 0;
    reap_dead();
    mmu_as_activate(0);
    next_pid = 1;
    for (int i = 0; i < MAX_FILES; i++)
        if (files[i].refs) { kprintf("[LINUX]  leaked open file %d\n", i); files[i].refs = 1; file_put(&files[i]); }
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i].used) { kprintf("[LINUX]  leaked socket %d\n", i); sock_free(&socks[i]); }
    for (int i = 0; i < MAX_SHM; i++)                   /* shared memory nobody unlinked */
        if (shm_names[i].m) shm_unlink_index(i);
    frames_kept += (long)ramfs_frames() - (long)ram0;   /* files in /tmp stay */
    if ((long)mmu_frames_free_count() + frames_kept != (long)free0)
        kprintf("[LINUX]  warning: %d page frames not returned\n",
                (int)((long)free0 - frames_kept - (long)mmu_frames_free_count()));
    return (status & 0x7F) ? 128 + (status & 0x7F) : (status >> 8) & 0xFF;
}
