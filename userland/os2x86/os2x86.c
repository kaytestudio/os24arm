/* userland/os2x86/os2x86.c - runs 32-bit x86 OS/2 programs (LX .EXEs).
 *
 *   os2x86 PROGRAM.EXE [arguments]
 *
 * The program's objects go where they're linked, in a guest memory of
 * their own (cpu.h: an i386 in software). Its imports from DOSCALLS & co.
 * become addresses in a thunk area at the top of that memory: when EIP
 * gets there, the interpreter stops and the C function below does the
 * call with this system's own (POSIX) calls. DLLs it needs that are x86
 * LX files too load the same way, from the program's directory, the
 * current one or LIBPATH.
 *
 * Drives: C: is /, D: is /h (the stick) when it's there, or as
 * OS2X86_DRIVES says ("C=/;D=/h;E=/mnt"). Each OS/2 thread is a thread
 * here, with a CPU of its own; one lock (gil) lets one at a time run the
 * program or the calls, given up around those that wait and every so many
 * instructions. OS2X86_MEM: the guest's memory
 * in MB (512). OS2X86_TRACE=1: every system call on stderr.
 * The kernel runs x86 LX files through this (as it does #! scripts). */
#define _GNU_SOURCE
#include "cpu.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char **environ;

/* OS/2 error codes */
#define NO_ERROR                 0
#define ERROR_INVALID_FUNCTION   1
#define ERROR_FILE_NOT_FOUND     2
#define ERROR_PATH_NOT_FOUND     3
#define ERROR_TOO_MANY_OPEN_FILES 4
#define ERROR_ACCESS_DENIED      5
#define ERROR_INVALID_HANDLE     6
#define ERROR_NOT_ENOUGH_MEMORY  8
#define ERROR_INVALID_DRIVE      15
#define ERROR_NOT_SAME_DEVICE    17
#define ERROR_NO_MORE_FILES      18
#define ERROR_SHARING_VIOLATION  32
#define ERROR_FILE_EXISTS        80
#define ERROR_INVALID_PARAMETER  87
#define ERROR_BROKEN_PIPE        109
#define ERROR_OPEN_FAILED        110
#define ERROR_BUFFER_OVERFLOW    111
#define ERROR_DISK_FULL          112
#define ERROR_INVALID_NAME       123
#define ERROR_INVALID_LEVEL      124
#define ERROR_MOD_NOT_FOUND      126
#define ERROR_PROC_NOT_FOUND     127
#define ERROR_WAIT_NO_CHILDREN   128
#define ERROR_CHILD_NOT_COMPLETE 129
#define ERROR_NEGATIVE_SEEK      131
#define ERROR_BAD_EXE_FORMAT     193
#define ERROR_ENVVAR_NOT_FOUND   203
#define ERROR_FILENAME_EXCED_RANGE 206
#define ERROR_NOT_OWNER          288
#define ERROR_ALREADY_POSTED     299
#define ERROR_ALREADY_RESET      300
#define ERROR_INVALID_PROCID     303
#define ERROR_INVALID_ADDRESS    487
#define ERROR_TIMEOUT            640
#define ERROR_INVALID_HANDLE_SEM 6

#define UNIT    0x10000u                /* OS/2 allocates memory in 64 KB */
#define T_SLOT  16                      /* bytes per thunk */
#define SEL_CODE 0x5B
#define SEL_DATA 0x53

/* the threads: thread 1 is the program's first */
typedef struct thr {
    cpu_t cpu;
    uint32_t tid, stack, exitcode;
    int state;                          /* 1 running, 2 ended, 3 collected (DosWaitThread) */
    int suspended, kill;
    int api_noret, xcpt_depth;
} thr_t;
#define MAXTHR 128
static thr_t thr0 = { .tid = 1, .state = 1 };
static _Thread_local thr_t *me = &thr0;
#define C (me->cpu)
static thr_t *thrs[MAXTHR] = { &thr0 };
static int nlive = 1;
static uint32_t crit_tid, crit_n;       /* DosEnterCritSec: only this one runs */
static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcv = PTHREAD_COND_INITIALIZER;   /* a semaphore, a thread changed */
#define UNLOCKED(stmt) do { pthread_mutex_unlock(&gil); stmt; pthread_mutex_lock(&gil); } while (0)
static int trace;
static char self_path[PATH_MAX] = "/h/OS2/os2x86";
static const char *prog_name = "os2x86";

/* ── the guest's memory, from here (no longjmp: these just say no) ──── */
static int ok(uint32_t a, uint32_t n) { return mem_ok(&C, a, n); }
static uint32_t g32(uint32_t a) { uint32_t v = 0; if (ok(a, 4)) memcpy(&v, C.mem + a, 4); return v; }
static uint16_t g16(uint32_t a) { uint16_t v = 0; if (ok(a, 2)) memcpy(&v, C.mem + a, 2); return v; }
static int p32(uint32_t a, uint32_t v) { if (!ok(a, 4)) return 0; memcpy(C.mem + a, &v, 4); return 1; }
static int p16(uint32_t a, uint16_t v) { if (!ok(a, 2)) return 0; memcpy(C.mem + a, &v, 2); return 1; }
static int p8(uint32_t a, uint8_t v) { if (!ok(a, 1)) return 0; C.mem[a] = v; return 1; }
static void *gp(uint32_t a, uint32_t n) { return ok(a, n ? n : 1) ? C.mem + a : 0; }
static char *gs(uint32_t a)             /* a NUL-terminated string */
{
    if (!ok(a, 1)) return 0;
    return memchr(C.mem + a, 0, C.size - a) ? (char *)C.mem + a : 0;
}
static int gpush(uint32_t v) { if (!p32(C.r[ESP] - 4, v)) return 0; C.r[ESP] -= 4; return 1; }
/* the API's arguments: _System, on the stack after the return address */
static uint32_t A(int n) { return g32(C.r[ESP] + 4 + 4 * n); }

static void die(int code, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
#include <stdarg.h>
static void die(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    _exit(code);
}

/* 64 KB units: taken or not, and at an allocation's first one its length */
static uint32_t nunits;
static uint8_t *uused;
static uint32_t *ulen;

static int units_free(uint32_t u, uint32_t n)
{
    if (u + n > nunits) return 0;
    for (uint32_t i = 0; i < n; i++) if (uused[u + i]) return 0;
    return 1;
}
static uint32_t mem_alloc(uint32_t bytes)
{
    uint32_t n = (uint32_t)(((uint64_t)bytes + UNIT - 1) / UNIT);
    if (!n) n = 1;
    for (uint32_t u = 1; u + n <= nunits; u++) {
        if (!units_free(u, n)) continue;
        memset(uused + u, 1, n);
        ulen[u] = n;
        return u * UNIT;
    }
    return 0;
}
static uint32_t mem_free(uint32_t a)
{
    if (a % UNIT || a / UNIT >= nunits || !ulen[a / UNIT]) return ERROR_INVALID_ADDRESS;
    uint32_t u = a / UNIT, n = ulen[u];
    memset(uused + u, 0, n);
    ulen[u] = 0;
    /* fresh zero pages for the next one */
    if (mmap(C.mem + a, (size_t)n * UNIT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
        memset(C.mem + a, 0, (size_t)n * UNIT);
    return NO_ERROR;
}
static int mem_reserve(uint32_t a, uint32_t size)
{
    if (a < UNIT || !size) return 0;
    uint32_t u = a / UNIT, e = (uint32_t)(((uint64_t)a + size + UNIT - 1) / UNIT);
    if (e > nunits || !units_free(u, e - u)) return 0;
    memset(uused + u, 1, e - u);
    return 1;
}

/* ── drives and paths ────────────────────────────────────────────────── */
static char droot[26][PATH_MAX];        /* "" : no such drive */
static char dcwd[26][PATH_MAX];         /* each drive's current directory (host) */
static int curdrv = 2;

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void norm(char *p)               /* an absolute path: no //, ., .. */
{
    char out[PATH_MAX];
    size_t o = 1;
    const char *s = p;
    out[0] = '/';
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - s);
        if (n == 1 && s[0] == '.') { }
        else if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;
            if (o > 1) o--;
        } else if (o + n + 2 < sizeof out) {
            if (o > 1) out[o++] = '/';
            memcpy(out + o, s, n);
            o += n;
        }
        s = e;
    }
    out[o] = 0;
    strcpy(p, out);
}

/* OS/2 file names don't care about case; this file system does */
static void casefix(char *p)
{
    struct stat st;
    if (lstat(p, &st) == 0) return;
    char *s = p + 1;
    while (*s) {
        char *e = strchr(s, '/');
        if (e) *e = 0;
        if (lstat(p, &st) != 0) {
            char *slash = s - 1;
            *slash = 0;
            DIR *d = opendir(slash == p ? "/" : p);
            *slash = '/';
            int found = 0;
            if (d) {
                struct dirent *de;
                size_t l = strlen(s);
                while ((de = readdir(d)))
                    if (strlen(de->d_name) == l && strcasecmp(de->d_name, s) == 0) {
                        memcpy(s, de->d_name, l);
                        found = 1;
                        break;
                    }
                closedir(d);
            }
            if (!found) { if (e) *e = '/'; return; }
        }
        if (!e) break;
        *e = '/';
        s = e + 1;
    }
}

static int drive_of_host(const char *h, const char **rest)
{
    int best = -1;
    size_t bl = 0;
    for (int d = 0; d < 26; d++) {
        const char *r = droot[d];
        if (!r[0]) continue;
        size_t l = strlen(r);
        if (l == 1 && r[0] == '/') {
            if (best < 0) { best = d; bl = 1; }
            continue;
        }
        if (strncmp(h, r, l) == 0 && (h[l] == '/' || !h[l]) && l > bl) { best = d; bl = l; }
    }
    if (rest) {
        const char *x = h + (best >= 0 ? bl : 0);
        while (*x == '/') x++;
        *rest = x;
    }
    return best;
}

static void host_to_os2(const char *h, char *out, size_t n)
{
    const char *rest;
    int d = drive_of_host(h, &rest);
    snprintf(out, n, "%c:\\%s", d >= 0 ? 'A' + d : 'C', rest);
    for (char *q = out; *q; q++) if (*q == '/') *q = '\\';
}

static const char *host_cwd(int d, char *buf, size_t n)
{
    if (d == curdrv && getcwd(buf, n)) return buf;
    return dcwd[d][0] ? dcwd[d] : droot[d];
}

/* an OS/2 file name to this system's; *drv its drive */
static uint32_t os2_to_host(const char *in, char *out, size_t n, int *drv)
{
    const char *s = in;
    int d = curdrv;
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        d = toupper((unsigned char)s[0]) - 'A';
        s += 2;
        if (!droot[d][0]) return ERROR_INVALID_DRIVE;
    }
    if (drv) *drv = d;
    /* devices */
    const char *b = s;
    if (strncasecmp(b, "\\DEV\\", 5) == 0 || strncasecmp(b, "/dev/", 5) == 0) b += 5;
    char dev[16];
    size_t k = 0;
    while (b[k] && b[k] != '.' && k < sizeof dev - 1) { dev[k] = b[k]; k++; }
    dev[k] = 0;
    if (!strchr(b, '\\') && !strchr(b, '/') && (b[k] == 0 || b[k] == '.')) {
        if (!strcasecmp(dev, "NUL")) { snprintf(out, n, "/dev/null"); return 0; }
        if (!strcasecmp(dev, "CON") || !strcasecmp(dev, "KBD$") || !strcasecmp(dev, "SCREEN$")) { snprintf(out, n, "/dev/tty"); return 0; }
    }
    char buf[PATH_MAX * 2], cw[PATH_MAX];
    if (*s == '/' && strcmp(droot[d], "/") != 0) {
        /* "/tmp/x": this system's own path, when /tmp is there */
        char first[NAME_MAX + 2] = "/";
        size_t i = 1;
        while (s[i] && s[i] != '/' && i < sizeof first - 1) { first[i] = s[i]; i++; }
        first[i] = 0;
        if (i > 1 && is_dir(first)) { snprintf(buf, sizeof buf, "%s", s); goto have; }
    }
    if (*s == '\\' || *s == '/') snprintf(buf, sizeof buf, "%s/%s", droot[d], s);
    else snprintf(buf, sizeof buf, "%s/%s", host_cwd(d, cw, sizeof cw), s);
    for (char *q = buf; *q; q++) if (*q == '\\') *q = '/';
    norm(buf);
    {
        size_t rl = strlen(droot[d]);
        if (!(rl == 1 || (strncmp(buf, droot[d], rl) == 0 && (buf[rl] == '/' || !buf[rl]))))
            snprintf(buf, sizeof buf, "%s", droot[d]);   /* (no .. above the drive's root) */
    }
have:
    norm(buf);
    casefix(buf);
    if (strlen(buf) >= n) return ERROR_FILENAME_EXCED_RANGE;
    strcpy(out, buf);
    return 0;
}

static void drives_init(void)
{
    const char *e = getenv("OS2X86_DRIVES");
    if (e) {
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", e);
        for (char *t = strtok(tmp, ";"); t; t = strtok(0, ";"))
            if (isalpha((unsigned char)t[0]) && t[1] == '=') {
                int d = toupper((unsigned char)t[0]) - 'A';
                snprintf(droot[d], PATH_MAX, "%s", t + 2);
                if (droot[d][0] != '/') droot[d][0] = 0; else norm(droot[d]);
            }
    }
    if (!droot[2][0] && !e) strcpy(droot[2], "/");
    if (!e && is_dir("/h")) strcpy(droot[3], "/h");
    char cw[PATH_MAX];
    if (getcwd(cw, sizeof cw)) {
        int d = drive_of_host(cw, 0);
        if (d >= 0) { curdrv = d; strcpy(dcwd[d], cw); }
    }
    if (!droot[curdrv][0]) for (int d = 0; d < 26; d++) if (droot[d][0]) { curdrv = d; break; }
}

static uint32_t oserr(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case EACCES: case EPERM: case EROFS: case EISDIR: case ENOTEMPTY: case EEXIST: return ERROR_ACCESS_DENIED;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case ENOSPC: return ERROR_DISK_FULL;
    case EPIPE: return ERROR_BROKEN_PIPE;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EXDEV: return ERROR_NOT_SAME_DEVICE;
    case EBUSY: return ERROR_SHARING_VIOLATION;
    default: return ERROR_INVALID_PARAMETER;
    }
}

/* ── modules ─────────────────────────────────────────────────────────── */
#define MAXOBJ 64
typedef struct mod {
    char name[64];                      /* "HELLO", "FOO" */
    char path[PATH_MAX];                /* this system's */
    uint8_t *f;
    size_t fsz;
    uint32_t lx;                        /* the LX header's offset */
    int nobj;
    uint32_t obase[MAXOBJ], osize[MAXOBJ], oflags[MAXOBJ];
    uint32_t hmod;
    int dll;
} mod_t;
static mod_t *mods[64];
static int nmods;
static mod_t *exe;
static mod_t *pending_init[64];
static int npending;
static int process_ready;

static uint32_t fu8(mod_t *m, uint32_t o) { return o < m->fsz ? m->f[o] : 0; }
static uint32_t fu16(mod_t *m, uint32_t o) { return fu8(m, o) | fu8(m, o + 1) << 8; }
static uint32_t fu32(mod_t *m, uint32_t o) { return fu16(m, o) | fu16(m, o + 2) << 16; }
static uint32_t H(mod_t *m, uint32_t off) { return fu32(m, m->lx + off); }
enum {
    LX_CPU = 0x08, LX_MFLAGS = 0x10, LX_NPAGES = 0x14, LX_EIPOBJ = 0x18, LX_EIP = 0x1C, LX_ESPOBJ = 0x20,
    LX_ESP = 0x24, LX_PAGESIZE = 0x28, LX_PAGESHIFT = 0x2C, LX_OBJTAB = 0x40, LX_OBJCNT = 0x44,
    LX_OBJPAGE = 0x48, LX_RESNAME = 0x58, LX_ENTRY = 0x5C, LX_FIXPAGE = 0x68, LX_FIXREC = 0x6C,
    LX_IMPMOD = 0x70, LX_IMPMODCNT = 0x74, LX_IMPPROC = 0x78, LX_DATAPAGE = 0x80, LX_NONRES = 0x88,
    LX_NONRESLEN = 0x8C, LX_STACKSIZE = 0xAC,
};

static int lx_offset(const uint8_t *f, size_t n)
{
    uint32_t o = 0;
    if (n >= 0x40 && f[0] == 'M' && f[1] == 'Z') o = f[0x3C] | f[0x3D] << 8 | f[0x3E] << 16 | (uint32_t)f[0x3F] << 24;
    if (o + 0xC4 > n || f[o] != 'L' || f[o + 1] != 'X') return -1;
    return (int)o;
}
/* an x86 LX file? (2: a DLL) */
static int x86_lx(const char *path, uint32_t *mflags)
{
    uint8_t h[0x200];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, h, 0x40);
    int r = 0;
    if (n == 0x40) {
        uint32_t o = 0;
        if (h[0] == 'M' && h[1] == 'Z') o = h[0x3C] | h[0x3D] << 8 | h[0x3E] << 16 | (uint32_t)h[0x3F] << 24;
        if (pread(fd, h, 0x18, o) == 0x18 && h[0] == 'L' && h[1] == 'X') {
            uint16_t cpu = h[8] | h[9] << 8;
            uint32_t fl = h[0x10] | h[0x11] << 8 | h[0x12] << 16 | (uint32_t)h[0x13] << 24;
            if (cpu >= 1 && cpu <= 4) { r = (fl & 0x8000) ? 2 : 1; if (mflags) *mflags = fl; }
        }
    }
    close(fd);
    return r;
}

static int unpack1(uint8_t *dst, uint32_t dsz, const uint8_t *s, uint32_t n)
{
    uint32_t o = 0;
    while (n >= 4) {
        uint32_t it = s[0] | s[1] << 8, len = s[2] | s[3] << 8;
        s += 4; n -= 4;
        if (!it && !len) break;
        if (len > n) return -1;
        for (uint32_t k = 0; k < it; k++) {
            if (o + len > dsz) return -1;
            memcpy(dst + o, s, len);
            o += len;
        }
        s += len; n -= len;
    }
    return 0;
}
static int unpack2(uint8_t *dst, uint32_t dsz, const uint8_t *s, uint32_t n)
{
    uint32_t o = 0, i = 0;
    while (i < n) {
        uint8_t b = s[i];
        uint32_t lit = 0, cb = 0, off = 0;
        switch (b & 3) {
        case 0:
            if (b) { lit = b >> 2; i++; }
            else {
                if (i + 1 >= n) return -1;
                cb = s[i + 1];
                if (!cb) return 0;
                if (i + 2 >= n || o + cb > dsz) return -1;
                memset(dst + o, s[i + 2], cb);
                o += cb; i += 3;
                continue;
            }
            if (i + lit > n || o + lit > dsz) return -1;
            memcpy(dst + o, s + i, lit);
            o += lit; i += lit;
            continue;
        case 1: {
            if (i + 2 > n) return -1;
            uint32_t w = s[i] | s[i + 1] << 8;
            lit = (w >> 2) & 3; cb = ((w >> 4) & 7) + 3; off = w >> 7;
            i += 2;
            break;
        }
        case 2: {
            if (i + 2 > n) return -1;
            uint32_t w = s[i] | s[i + 1] << 8;
            cb = ((w >> 2) & 3) + 3; off = w >> 4;
            i += 2;
            break;
        }
        case 3: {
            if (i + 3 > n) return -1;
            uint32_t w = s[i] | s[i + 1] << 8 | s[i + 2] << 16;
            lit = (w >> 2) & 15; cb = (w >> 6) & 63; off = w >> 12;
            i += 3;
            break;
        }
        }
        if (i + lit > n || o + lit > dsz) return -1;
        memcpy(dst + o, s + i, lit);
        o += lit; i += lit;
        if (off > o || o + cb > dsz) return -1;
        for (uint32_t k = 0; k < cb; k++, o++) dst[o] = dst[o - off];
    }
    return 0;
}

/* ── thunks: the imports from the system ────────────────────────────── */
static const char *const libs[] = { "DOSCALLS", "NLS", "MSG", "QUECALLS", "SESMGR", "KBDCALLS", "VIOCALLS", "MOUCALLS", 0 };
enum { L_DOS, L_NLS, L_MSG, L_QUE, L_SES, L_KBD, L_VIO, L_MOU, NLIBS };

typedef uint32_t (*api_fn)(void);
typedef struct { int lib; uint16_t ord; const char *name; api_fn fn; uint8_t a16; } api_t;   /* a16: a 16-bit call, its arguments' bytes */
static const api_t apis[];

enum { T_EXIT, T_CBRET, T_THREXIT, T_FIRST };
typedef struct { int lib; uint32_t ord; const api_t *api; int warned; char name[40]; } thunk_t;
#define NTHUNKS (UNIT / T_SLOT)
static thunk_t thunks[NTHUNKS];
static int nthunks = T_FIRST;
static uint32_t thunk_addr(int i) { return C.thunk_lo + (uint32_t)i * T_SLOT; }

static int builtin_lib(const char *name)
{
    char n[64];
    const char *b = name;
    for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    snprintf(n, sizeof n, "%s", b);
    char *dot = strrchr(n, '.');
    if (dot && !strcasecmp(dot, ".DLL")) *dot = 0;
    if (!strcasecmp(n, "DOSCALL1")) return L_DOS;
    for (int i = 0; libs[i]; i++) if (!strcasecmp(n, libs[i])) return i;
    return -1;
}

static const api_t *api_find(int lib, uint32_t ord, const char *name);
static uint32_t thunk_for(int lib, uint32_t ord, const char *name)
{
    const api_t *a = api_find(lib, ord, name);
    if (a) ord = a->ord;
    for (int i = T_FIRST; i < nthunks; i++)
        if (thunks[i].lib == lib && thunks[i].ord == ord && (ord || !strcmp(thunks[i].name, name ? name : "")))
            return thunk_addr(i);
    if (nthunks >= (int)NTHUNKS) die(8, "%s: too many imports", prog_name);
    thunk_t *t = &thunks[nthunks];
    t->lib = lib; t->ord = ord; t->api = a;
    snprintf(t->name, sizeof t->name, "%s", a ? a->name : name ? name : "");
    return thunk_addr(nthunks++);
}

/* ── loading ─────────────────────────────────────────────────────────── */
static mod_t *load_dll(const char *name, char *err, size_t errn);
static int entry_addr(mod_t *m, uint32_t ord, uint32_t *out, char *err, size_t errn);

static void imp_name(mod_t *m, uint32_t idx, char *out, size_t n)
{
    uint32_t p = m->lx + H(m, LX_IMPMOD);
    out[0] = 0;
    for (uint32_t i = 1; i <= H(m, LX_IMPMODCNT); i++) {
        uint32_t l = fu8(m, p);
        if (i == idx) {
            size_t k = l < n - 1 ? l : n - 1;
            if (p + 1 + k <= m->fsz) memcpy(out, m->f + p + 1, k);
            out[k] = 0;
            return;
        }
        p += 1 + l;
    }
}

static int export_ord(mod_t *m, const char *name)
{
    for (int pass = 0; pass < 4; pass++) {
        uint32_t p = pass & 1 ? H(m, LX_NONRES) : m->lx + H(m, LX_RESNAME);
        uint32_t end = pass & 1 ? p + H(m, LX_NONRESLEN) : (uint32_t)m->fsz;
        if (pass & 1 && !H(m, LX_NONRES)) continue;
        int first = 1;
        while (p < end && p < m->fsz) {
            uint32_t l = fu8(m, p);
            if (!l) break;
            if (!first && p + 1 + l + 2 <= m->fsz) {
                const char *s = (const char *)m->f + p + 1;
                if (strlen(name) == l && (pass < 2 ? !strncmp(s, name, l) : !strncasecmp(s, name, l)))
                    return (int)fu16(m, p + 1 + l);
            }
            if (!(pass & 1)) first = 0;
            p += 1 + l + 2;
        }
    }
    return -1;
}

static int resolve_import(mod_t *m, uint32_t modord, uint32_t ord, const char *pname, uint32_t *out, char *err, size_t errn)
{
    char mn[256];
    imp_name(m, modord, mn, sizeof mn);
    int lib = builtin_lib(mn);
    if (lib >= 0) { *out = thunk_for(lib, pname ? 0 : ord, pname); return 0; }
    mod_t *d = load_dll(mn, err, errn);
    if (!d) return -1;
    if (pname) {
        int o = export_ord(d, pname);
        if (o < 0) { snprintf(err, errn, "SYS2070: %s has no %s (that %s needs)", mn, pname, m->name); return -1; }
        ord = (uint32_t)o;
    }
    return entry_addr(d, ord, out, err, errn);
}

static int entry_addr(mod_t *m, uint32_t ord, uint32_t *out, char *err, size_t errn)
{
    uint32_t p = m->lx + H(m, LX_ENTRY), cur = 1;
    for (;;) {
        uint32_t cnt = fu8(m, p), type = fu8(m, p + 1);
        if (!cnt || p >= m->fsz) break;
        p += 2;
        if (type == 0) { cur += cnt; continue; }
        uint32_t obj = fu16(m, p), size = type == 1 ? 3 : type == 4 ? 7 : 5;
        p += 2;
        if (ord >= cur && ord < cur + cnt) {
            uint32_t e = p + (ord - cur) * size;
            if (type == 4) {
                uint32_t flags = fu8(m, e), mo = fu16(m, e + 1), v = fu32(m, e + 3);
                if (flags & 1) return resolve_import(m, mo, v, 0, out, err, errn);
                char nm[256];
                uint32_t q = m->lx + H(m, LX_IMPPROC) + v, l = fu8(m, q);
                if (l > 255 || q + 1 + l > m->fsz) break;
                memcpy(nm, m->f + q + 1, l); nm[l] = 0;
                return resolve_import(m, mo, 0, nm, out, err, errn);
            }
            if (!obj || (int)obj > m->nobj) break;
            uint32_t off = type == 3 ? fu32(m, e + 1) : fu16(m, e + 1);
            *out = m->obase[obj - 1] + off;
            return 0;
        }
        p += cnt * size;
        cur += cnt;
    }
    snprintf(err, errn, "SYS2070: %s has no entry point %u", m->name, ord);
    return -1;
}

static int fixup_write(uint32_t t, uint32_t a, uint32_t target, uint16_t sel)
{
    switch (t) {
    case 0: return p8(a, (uint8_t)target);
    case 2: return p16(a, sel);
    case 3: return p16(a, (uint16_t)target) && p16(a + 2, (uint16_t)(((target >> 16) << 3) | 7));
    case 5: return p16(a, (uint16_t)target);
    case 6: return p32(a, target) && p16(a + 4, sel);
    case 7: return p32(a, target);
    case 8: return p32(a, target - (a + 4));
    }
    return 0;
}

static int do_fixups(mod_t *m, uint32_t pg, uint32_t paddr, char *err, size_t errn)
{
    uint32_t fpt = m->lx + H(m, LX_FIXPAGE), frt = m->lx + H(m, LX_FIXREC);
    uint32_t p = fu32(m, fpt + 4 * (pg - 1)), e = fu32(m, fpt + 4 * pg);
    while (p < e) {
        uint32_t src = fu8(m, frt + p), fl = fu8(m, frt + p + 1), cnt = 0;
        int32_t soff = 0;
        p += 2;
        if (src & 0x20) cnt = fu8(m, frt + p++);
        else { soff = (int16_t)fu16(m, frt + p); p += 2; }
        uint32_t target = 0;
        uint16_t sel = SEL_CODE;
#define RD(big) ((big) ? (p += 4, fu32(m, frt + p - 4)) : (p += 2, fu16(m, frt + p - 2)))
#define RD8or16(big) ((big) ? (p += 2, fu16(m, frt + p - 2)) : (p += 1, fu8(m, frt + p - 1)))
        switch (fl & 3) {
        case 0: {
            uint32_t on = RD8or16(fl & 0x40);
            if (!on || (int)on > m->nobj) { snprintf(err, errn, "%s: a fixup to object %u", m->name, on); return -1; }
            sel = (m->oflags[on - 1] & 4) ? SEL_CODE : SEL_DATA;
            if ((src & 15) != 2) target = m->obase[on - 1] + RD(fl & 0x10);
            break;
        }
        case 1: {
            uint32_t mo = RD8or16(fl & 0x40), ord;
            if (fl & 0x80) ord = fu8(m, frt + p++);
            else ord = RD(fl & 0x10);
            if (resolve_import(m, mo, ord, 0, &target, err, errn)) return -1;
            break;
        }
        case 2: {
            uint32_t mo = RD8or16(fl & 0x40), no = RD(fl & 0x10);
            uint32_t q = m->lx + H(m, LX_IMPPROC) + no, l = fu8(m, q);
            char nm[256];
            if (l > 255 || q + 1 + l > m->fsz) { snprintf(err, errn, "%s: a bad import name", m->name); return -1; }
            memcpy(nm, m->f + q + 1, l); nm[l] = 0;
            if (resolve_import(m, mo, 0, nm, &target, err, errn)) return -1;
            break;
        }
        case 3: {
            uint32_t ord = RD8or16(fl & 0x40);
            if (entry_addr(m, ord, &target, err, errn)) return -1;
            break;
        }
        }
        if (fl & 4) target += RD(fl & 0x20);
#undef RD
#undef RD8or16
        if (src & 0x20) {
            for (uint32_t k = 0; k < cnt; k++, p += 2)
                fixup_write(src & 15, paddr + (uint32_t)(int32_t)(int16_t)fu16(m, frt + p), target, sel);
        } else {
            fixup_write(src & 15, paddr + (uint32_t)soff, target, sel);
        }
    }
    return 0;
}

static mod_t *load_module(const char *path, int dll, char *err, size_t errn)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, errn, "SYS0002: The system cannot find the file %s.", path); return 0; }
    struct stat st;
    fstat(fd, &st);
    mod_t *m = calloc(1, sizeof *m);
    m->fsz = (size_t)st.st_size;
    m->f = malloc(m->fsz ? m->fsz : 1);
    size_t got = 0;
    while (got < m->fsz) {
        ssize_t n = read(fd, m->f + got, m->fsz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    int lo = lx_offset(m->f, got);
    if (lo < 0) { snprintf(err, errn, "SYS0193: %s isn't an OS/2 program (LX).", path); return 0; }
    m->lx = (uint32_t)lo;
    uint32_t cpu = fu16(m, m->lx + LX_CPU);
    if (cpu < 1 || cpu > 4) { snprintf(err, errn, "SYS0193: %s isn't for an x86 CPU.", path); return 0; }
    snprintf(m->path, sizeof m->path, "%s", path);
    m->dll = dll;
    /* the name: the resident names' first */
    uint32_t rn = m->lx + H(m, LX_RESNAME), rl = fu8(m, rn);
    if (rl && rl < sizeof m->name && rn + 1 + rl <= m->fsz) { memcpy(m->name, m->f + rn + 1, rl); m->name[rl] = 0; }
    else {
        const char *b = strrchr(path, '/');
        snprintf(m->name, sizeof m->name, "%s", b ? b + 1 : path);
        char *dot = strrchr(m->name, '.');
        if (dot) *dot = 0;
    }
    for (char *q = m->name; *q; q++) *q = (char)toupper((unsigned char)*q);

    /* objects: all of them together, where linked or (if they can be
     * moved) wherever there's room */
    m->nobj = (int)H(m, LX_OBJCNT);
    if (m->nobj > MAXOBJ) { snprintf(err, errn, "%s: %d objects", path, m->nobj); return 0; }
    uint32_t ot = m->lx + H(m, LX_OBJTAB), lo_a = 0xFFFFFFFF, hi_a = 0;
    for (int i = 0; i < m->nobj; i++) {
        m->osize[i] = fu32(m, ot + 24 * i);
        m->obase[i] = fu32(m, ot + 24 * i + 4);
        m->oflags[i] = fu32(m, ot + 24 * i + 8);
        uint32_t sz = m->osize[i] ? m->osize[i] : 1;
        if (m->obase[i] < lo_a) lo_a = m->obase[i];
        if ((uint64_t)m->obase[i] + sz > hi_a) hi_a = m->obase[i] + sz;
    }
    if (!m->nobj) { snprintf(err, errn, "%s: no objects", path); return 0; }
    uint32_t span = hi_a - lo_a;
    if (!mem_reserve(lo_a, span)) {
        if (H(m, LX_MFLAGS) & 0x10) { snprintf(err, errn, "SYS0192: %s can't go where it's linked (0x%x).", path, lo_a); return 0; }
        uint32_t at = mem_alloc(span + (lo_a % UNIT));
        if (!at) { snprintf(err, errn, "SYS0008: no memory for %s", path); return 0; }
        ulen[at / UNIT] = 0;                           /* (not DosFreeMem's) */
        int32_t delta = (int32_t)(at + lo_a % UNIT - lo_a);
        for (int i = 0; i < m->nobj; i++) m->obase[i] += (uint32_t)delta;
    }
    /* pages */
    uint32_t psz = H(m, LX_PAGESIZE), shift = H(m, LX_PAGESHIFT), opt = m->lx + H(m, LX_OBJPAGE);
    uint32_t data = H(m, LX_DATAPAGE);
    if (!psz) psz = 4096;
    for (int i = 0; i < m->nobj; i++) {
        uint32_t first = fu32(m, ot + 24 * i + 12), n = fu32(m, ot + 24 * i + 16);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t pe = opt + (first + k - 1) * 8;
            uint32_t off = fu32(m, pe), size = fu16(m, pe + 4), fl = fu16(m, pe + 6);
            uint32_t va = m->obase[i] + k * psz;
            uint64_t fo = (uint64_t)data + ((uint64_t)off << shift);
            if (!ok(va, psz)) { snprintf(err, errn, "%s: a page outside memory", path); return 0; }
            if (fl == 3 || fl == 2 || !size) continue;
            if (fo + size > m->fsz) { snprintf(err, errn, "%s: cut short", path); return 0; }
            int r = 0;
            if (fl == 0) memcpy(C.mem + va, m->f + fo, size > psz ? psz : size);
            else if (fl == 1) r = unpack1(C.mem + va, psz, m->f + fo, size);
            else if (fl == 5) r = unpack2(C.mem + va, psz, m->f + fo, size);
            else r = -1;
            if (r) { snprintf(err, errn, "%s: page %u can't be unpacked (type %u)", path, first + k, fl); return 0; }
        }
    }
    mods[nmods++] = m;
    m->hmod = 0x100 + (uint32_t)nmods;
    /* then the fixups (after every page is in: they cross pages) */
    for (int i = 0; i < m->nobj; i++) {
        uint32_t first = fu32(m, ot + 24 * i + 12), n = fu32(m, ot + 24 * i + 16);
        for (uint32_t k = 0; k < n; k++)
            if (first + k <= H(m, LX_NPAGES) && do_fixups(m, first + k, m->obase[i] + k * psz, err, errn)) { nmods--; return 0; }
    }
    if (dll && H(m, LX_EIPOBJ)) pending_init[npending++] = m;
    return m;
}

static mod_t *find_mod(const char *name)
{
    char n[64];
    const char *b = name;
    for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    snprintf(n, sizeof n, "%s", b);
    char *dot = strrchr(n, '.');
    if (dot && !strcasecmp(dot, ".DLL")) *dot = 0;
    for (int i = 0; i < nmods; i++) if (!strcasecmp(mods[i]->name, n)) return mods[i];
    return 0;
}

static uint32_t guest_call(uint32_t fn, int n, const uint32_t *args);
static void run_inits(void)
{
    while (npending) {
        mod_t *m = pending_init[0];
        memmove(pending_init, pending_init + 1, --npending * sizeof *pending_init);
        uint32_t o = H(m, LX_EIPOBJ);
        if (!o || (int)o > m->nobj) continue;
        uint32_t args[2] = { m->hmod, 0 };
        if (!guest_call(m->obase[o - 1] + H(m, LX_EIP), 2, args))
            die(255, "SYS0191: %s's initialisation failed.", m->name);
    }
}

static mod_t *load_dll(const char *name, char *err, size_t errn)
{
    mod_t *m = find_mod(name);
    if (m) return m;
    char file[NAME_MAX + 1], h[PATH_MAX];
    const char *b = name;
    for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    snprintf(file, sizeof file, "%s%s", b, strchr(b, '.') ? "" : ".DLL");
    int found = 0;
    if (b != name) found = !os2_to_host(name, h, sizeof h, 0) && access(h, R_OK) == 0;
    /* the program's directory, the current one, then LIBPATH */
    char dirs[8192] = "";
    if (exe) {
        snprintf(dirs, sizeof dirs, "%s", exe->path);
        char *sl = strrchr(dirs, '/');
        if (sl) *sl = 0;
    }
    size_t l = strlen(dirs);
    const char *lp[] = { getenv("BEGINLIBPATH"), getenv("LIBPATH"), getenv("OS2X86_LIBPATH"), getenv("ENDLIBPATH") };
    snprintf(dirs + l, sizeof dirs - l, ";.");
    for (int i = 0; i < 4; i++)
        if (lp[i]) { l = strlen(dirs); snprintf(dirs + l, sizeof dirs - l, ";%s", lp[i]); }
    for (char *t = strtok(dirs, ";"); t && !found; t = strtok(0, ";")) {
        char p[PATH_MAX * 2];
        if (t[0] == '/') snprintf(p, sizeof p, "%s/%s", t, file);
        else {
            char q[PATH_MAX];
            snprintf(q, sizeof q, "%s\\%s", t, file);
            if (os2_to_host(q, p, sizeof p, 0)) continue;
        }
        casefix(p);
        if (access(p, R_OK) == 0) { snprintf(h, sizeof h, "%s", p); found = 1; }
    }
    if (!found) {
        const char *why = !strncasecmp(b, "PM", 2) ? " (Presentation Manager programs can't run here yet)" : "";
        snprintf(err, errn, "SYS1804: The system cannot find the file %s.%s", b, why);
        return 0;
    }
    return load_module(h, 1, err, errn);
}

/* ── the process ─────────────────────────────────────────────────────── */
static uint32_t tib, pib, envp, cmdp, stack_lo, stack_top;

static void guest_strcpy(uint32_t *at, const char *s)
{
    size_t n = strlen(s) + 1;
    if (ok(*at, (uint32_t)n)) memcpy(C.mem + *at, s, n);
    *at += (uint32_t)n;
}

static void setup_process(int argc, char **argv)
{
    uint32_t so = H(exe, LX_ESPOBJ);
    if (so && (int)so <= exe->nobj && H(exe, LX_ESP)) {
        stack_lo = exe->obase[so - 1];
        stack_top = stack_lo + H(exe, LX_ESP);
    } else {
        uint32_t sz = H(exe, LX_STACKSIZE);
        if (sz < 256 * 1024) sz = 256 * 1024;
        if (!(stack_lo = mem_alloc(sz))) die(8, "SYS0008: no memory for the stack");
        stack_top = stack_lo + sz;
    }
    /* the environment, then the program's name; the command line */
    size_t need = 64 + PATH_MAX * 2;
    for (char **e = environ; *e; e++) need += strlen(*e) + 1;
    for (int i = 1; i < argc; i++) need += strlen(argv[i]) * 2 + 3;
    uint32_t blk = mem_alloc((uint32_t)need + 0x100);
    if (!blk) die(8, "SYS0008: no memory");
    tib = blk; pib = blk + 0x40;
    uint32_t tib2 = blk + 0x80, at = blk + 0x100;
    char os2path[PATH_MAX];
    host_to_os2(exe->path, os2path, sizeof os2path);
    envp = at;
    for (char **e = environ; *e; e++) guest_strcpy(&at, *e);
    if (at == envp) guest_strcpy(&at, "");
    guest_strcpy(&at, "");
    guest_strcpy(&at, os2path);
    cmdp = at;
    guest_strcpy(&at, os2path);
    char *args = malloc(need), *w = args;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int q = !*a || strpbrk(a, " \t") != 0;
        if (i > 2) *w++ = ' ';
        if (q) *w++ = '"';
        while (*a) { if (*a == '"') *w++ = '\\'; *w++ = *a++; }
        if (q) *w++ = '"';
    }
    *w = 0;
    guest_strcpy(&at, args);
    guest_strcpy(&at, "");
    free(args);
    /* the thread's and process's information blocks */
    p32(tib, 0xFFFFFFFF);                   /* exception handlers: none */
    p32(tib + 4, stack_lo);
    p32(tib + 8, stack_top);
    p32(tib + 12, tib2);
    p32(tib + 16, 20);
    p32(tib + 20, 1);
    p32(tib2, 1);                           /* thread 1 */
    p32(tib2 + 4, 0x200);
    p32(tib2 + 8, 20);
    p32(pib, (uint32_t)getpid());
    p32(pib + 4, (uint32_t)getppid());
    p32(pib + 8, exe->hmod);
    p32(pib + 12, cmdp);
    p32(pib + 16, envp);
    p32(pib + 20, 0);
    p32(pib + 24, 2);                       /* a VIO (windowable) program */
    C.fs_base = C.tib = tib;
    C.r[ESP] = stack_top;
    process_ready = 1;
}

/* ── running: interpreter, then system call, then interpreter ... ────── */
#define api_noret (me->api_noret)
static int running_exitlist;
static void exception(int why);
static void api_call(int idx);
static void api16(thunk_t *t);
static void thread_end(uint32_t code) __attribute__((noreturn));

/* another thread's turn; then as DosSuspendThread, DosEnterCritSec,
 * DosKillThread say */
static void checkpoint(void)
{
    UNLOCKED(sched_yield());
    while (me->suspended || (crit_tid && crit_tid != me->tid)) pthread_cond_wait(&gcv, &gil);
    if (me->kill) thread_end(0);
}

static void run(int nested)
{
    for (;;) {
        int r = cpu_run(&C, nlive > 1 ? 1L << 15 : 1L << 22);
        if (r == CPU_STEPS) { if (nlive > 1) checkpoint(); continue; }
        if (r == CPU_THUNK) {
            uint32_t o = C.cs_base + C.eip - C.thunk_lo;
            int idx = (int)(o / T_SLOT);
            if (o % T_SLOT || idx >= nthunks) { exception(CPU_FAULT); continue; }
            if (idx == T_CBRET) {
                if (nested) return;
                exception(CPU_FAULT);
                continue;
            }
            api_call(idx);
            continue;
        }
        exception(r);
    }
}

static uint32_t guest_call(uint32_t fn, int n, const uint32_t *args)
{
    uint32_t r[8], eip = C.eip, fl = C.eflags;
    memcpy(r, C.r, sizeof r);
    for (int i = n - 1; i >= 0; i--) gpush(args[i]);
    gpush(thunk_addr(T_CBRET));
    C.eip = fn;
    run(1);
    uint32_t ret = C.r[EAX];
    memcpy(C.r, r, sizeof r);
    C.eip = eip; C.eflags = fl;
    return ret;
}

/* exit list: DosExitList */
typedef struct { uint32_t fn; uint32_t order; } exitfn_t;
static exitfn_t exitlist[32];
static int nexit;

static void do_exit(uint32_t code) __attribute__((noreturn));
static void do_exit(uint32_t code)
{
    if (!running_exitlist && process_ready) {
        running_exitlist = 1;
        while (nexit) {
            int best = 0;
            for (int i = 1; i < nexit; i++) if (exitlist[i].order < exitlist[best].order) best = i;
            uint32_t fn = exitlist[best].fn, arg = 0;    /* TC_EXIT */
            exitlist[best] = exitlist[--nexit];
            guest_call(fn, 1, &arg);
        }
    }
    _exit((int)(code & 0xFF));
}

static void thread_end(uint32_t code)
{
    if (me->tid == 1) do_exit(code);        /* (thread 1's end is the process's) */
    me->state = 2;
    me->exitcode = code;
    nlive--;
    if (crit_tid == me->tid) { crit_tid = 0; crit_n = 0; }
    pthread_cond_broadcast(&gcv);
    pthread_mutex_unlock(&gil);
    pthread_exit(0);
}

/* a 16-bit call (far, Pascal): SS:SP the return address, then the
 * arguments, the last one first; it pops them; the result in AX */
static _Thread_local uint32_t a16;
static void api16(thunk_t *t)
{
    uint32_t sp = C.stack16 ? C.r[ESP] & 0xFFFF : C.r[ESP];
    uint32_t at = C.ss_base + sp, ip = g16(at), cs = g16(at + 2);
    if (!t->api || !t->api->a16)
        die(255, "%s: %s.%u%s%s (a 16-bit call) isn't there yet", prog_name, libs[t->lib], t->ord,
            t->name[0] ? " " : "", t->name);
    a16 = at + 4;
    uint32_t rv = t->api->fn();
    if (trace) fprintf(stderr, "[os2x86] %s.%s (16-bit) = %u\n", libs[t->lib], t->api->name, rv);
    C.r[EAX] = (C.r[EAX] & 0xFFFF0000) | (rv & 0xFFFF);
    sp += 4 + t->api->a16;
    C.r[ESP] = C.stack16 ? (C.r[ESP] & 0xFFFF0000) | (sp & 0xFFFF) : sp;
    cpu_load_seg(&C, 1, (uint16_t)cs);
    C.eip = ip;
}

static void api_call(int idx)
{
    thunk_t *t = &thunks[idx];
    if (idx == T_EXIT) do_exit(C.r[EAX]);
    if (idx == T_THREXIT) thread_end(C.r[EAX]);
    if (C.code16) { api16(t); return; }
    uint32_t ret = g32(C.r[ESP]), rv;
    uint32_t a0 = A(0), a1 = A(1), a2 = A(2), a3 = A(3);
    api_noret = 0;
    if (t->api) rv = t->api->fn();
    else {
        if (!t->warned) {
            fprintf(stderr, "%s: %s.%u%s%s isn't there yet (the program gets error 1)\n", prog_name,
                    libs[t->lib], t->ord, t->name[0] ? " " : "", t->name);
            t->warned = 1;
        }
        rv = ERROR_INVALID_FUNCTION;
    }
    if (trace)
        fprintf(stderr, "[os2x86] %s.%s(%x, %x, %x, %x) = %u\n", libs[t->lib],
                t->api ? t->api->name : "?", a0, a1, a2, a3, rv);
    if (!api_noret) {
        C.r[EAX] = rv;
        C.eip = ret;
        C.r[ESP] += 4;
    }
}

/* ── exceptions ──────────────────────────────────────────────────────── */
#define XCPT_ACCESS_VIOLATION       0xC0000005
#define XCPT_ILLEGAL_INSTRUCTION    0xC000001C
#define XCPT_INTEGER_DIVIDE_BY_ZERO 0xC000009B
#define XCPT_INTEGER_OVERFLOW       0xC000009C
#define XCPT_PRIVILEGED_INSTRUCTION 0xC000009D
#define XCPT_BREAKPOINT             0xC000009F
#define xcpt_depth (me->xcpt_depth)

static void where(uint32_t a, char *out, size_t n)
{
    for (int i = 0; i < nmods; i++)
        for (int k = 0; k < mods[i]->nobj; k++)
            if (a >= mods[i]->obase[k] && a - mods[i]->obase[k] < mods[i]->osize[k]) {
                snprintf(out, n, "%s %04d:%08x", mods[i]->name, k + 1, a - mods[i]->obase[k]);
                return;
            }
    snprintf(out, n, "%08x", a);
}

static void fatal_trap(uint32_t num, uint32_t p0, uint32_t p1, uint32_t addr) __attribute__((noreturn));
static void fatal_trap(uint32_t num, uint32_t p0, uint32_t p1, uint32_t addr)
{
    char w[96];
    where(addr, w, sizeof w);
    fprintf(stderr, "\nSYS3175: A program in this session generated an exception and cannot continue.\n\n");
    fprintf(stderr, "%s %08x  at %s\n", exe ? exe->name : "?", num, w);
    if (num == XCPT_ACCESS_VIOLATION) fprintf(stderr, "P1=%08x  P2=%08x\n", p0, p1);
    if (num == XCPT_ILLEGAL_INSTRUCTION) {
        fprintf(stderr, "instruction:");
        for (int i = 0; i < 8 && ok(addr + i, 1); i++) fprintf(stderr, " %02x", C.mem[addr + i]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "EAX=%08x  EBX=%08x  ECX=%08x  EDX=%08x\n", C.r[EAX], C.r[EBX], C.r[ECX], C.r[EDX]);
    fprintf(stderr, "ESI=%08x  EDI=%08x  EBP=%08x  ESP=%08x\n", C.r[ESI], C.r[EDI], C.r[EBP], C.r[ESP]);
    fprintf(stderr, "CS:EIP=%04x:%08x  EFLAGS=%08x\n", SEL_CODE, addr, C.eflags);
    _exit(255);
}

static void dispatch(uint32_t num, uint32_t np, uint32_t p0, uint32_t p1, uint32_t addr)
{
    if (!process_ready || xcpt_depth > 3) fatal_trap(num, p0, p1, addr);
    uint32_t ctx = ((C.r[ESP] - 512) & ~15u) - 176, rep = ctx - 48;
    if (!ok(rep, 48 + 176)) fatal_trap(num, p0, p1, addr);
    memset(C.mem + rep, 0, 48 + 176);
    p32(rep, num); p32(rep + 12, addr); p32(rep + 16, np); p32(rep + 20, p0); p32(rep + 24, p1);
    p32(ctx, 7);                            /* control, integer, segments */
    p32(ctx + 112, C.sreg[5]); p32(ctx + 116, C.sreg[4]); p32(ctx + 120, C.sreg[0]); p32(ctx + 124, C.sreg[3]);
    p32(ctx + 128, C.r[EDI]); p32(ctx + 132, C.r[ESI]); p32(ctx + 136, C.r[EAX]); p32(ctx + 140, C.r[EBX]);
    p32(ctx + 144, C.r[EDX]); p32(ctx + 148, C.r[ECX]); p32(ctx + 152, C.r[EBP]); p32(ctx + 156, C.eip);
    p32(ctx + 160, SEL_CODE); p32(ctx + 164, C.eflags); p32(ctx + 168, C.r[ESP]); p32(ctx + 172, SEL_DATA);
    xcpt_depth++;
    for (uint32_t rec = g32(C.fs_base); rec && rec != 0xFFFFFFFF && ok(rec, 8); rec = g32(rec)) {
        uint32_t args[4] = { rep, rec, ctx, 0 }, sp = C.r[ESP];
        C.r[ESP] = rep - 16;
        uint32_t r = guest_call(g32(rec + 4), 4, args);
        C.r[ESP] = sp;
        if (r == 0xFFFFFFFF) {              /* XCPT_CONTINUE_EXECUTION: as the context says */
            C.r[EDI] = g32(ctx + 128); C.r[ESI] = g32(ctx + 132); C.r[EAX] = g32(ctx + 136);
            C.r[EBX] = g32(ctx + 140); C.r[EDX] = g32(ctx + 144); C.r[ECX] = g32(ctx + 148);
            C.r[EBP] = g32(ctx + 152); C.eip = g32(ctx + 156); C.eflags = (g32(ctx + 164) & 0x00254FD5) | 0x202;
            C.r[ESP] = g32(ctx + 168);
            xcpt_depth--;
            return;
        }
    }
    fatal_trap(num, p0, p1, addr);
}

static void exception(int why)
{
    uint32_t num = XCPT_ACCESS_VIOLATION, np = 0, p0 = 0, p1 = 0, addr = C.op_eip;
    switch (why) {
    case CPU_FAULT: np = 2; p1 = C.fault_addr; if (C.eip != C.op_eip) addr = C.eip; break;
    case CPU_ILLEGAL: num = XCPT_ILLEGAL_INSTRUCTION; break;
    case CPU_DIVIDE: num = XCPT_INTEGER_DIVIDE_BY_ZERO; break;
    case CPU_INT:
        if (C.int_no == 3) num = XCPT_BREAKPOINT;
        else if (C.int_no == 4) num = XCPT_INTEGER_OVERFLOW;
        else if (C.int_no == -1) { num = XCPT_PRIVILEGED_INSTRUCTION; C.eip = C.op_eip; }
        else { np = 2; p1 = 0xFFFFFFFF; C.eip = C.op_eip; }
        break;
    }
    if (why == CPU_FAULT && C.fault_addr >= C.thunk_lo) { addr = C.eip; }
    dispatch(num, np, p0, p1, addr);
}

/* ── file information ────────────────────────────────────────────────── */
static void fdatetime(time_t t, uint16_t *d, uint16_t *tm)
{
    struct tm lt;
    localtime_r(&t, &lt);
    int y = lt.tm_year + 1900 - 1980;
    if (y < 0) y = 0;
    *d = (uint16_t)(y << 9 | (lt.tm_mon + 1) << 5 | lt.tm_mday);
    *tm = (uint16_t)(lt.tm_hour << 11 | lt.tm_min << 5 | lt.tm_sec / 2);
}
static uint32_t fattr(const struct stat *st, const char *name)
{
    uint32_t a = 0;
    if (S_ISDIR(st->st_mode)) a |= 0x10; else a |= 0x20;
    if (!(st->st_mode & S_IWUSR)) a |= 1;
    (void)name;
    return a;
}
static void put_dates(uint32_t b, const struct stat *st)
{
    uint16_t d, t;
    fdatetime(st->st_ctime, &d, &t); p16(b, d); p16(b + 2, t);
    fdatetime(st->st_atime, &d, &t); p16(b + 4, d); p16(b + 6, t);
    fdatetime(st->st_mtime, &d, &t); p16(b + 8, d); p16(b + 10, t);
}
static uint32_t put_status(const struct stat *st, uint32_t level, uint32_t b, uint32_t cb)
{
    uint32_t need = level == 1 ? 24 : level == 2 ? 28 : level == 11 ? 32 : level == 12 ? 36 : 0;
    if (!need) return ERROR_INVALID_LEVEL;
    if (cb < need || !ok(b, need)) return ERROR_BUFFER_OVERFLOW;
    memset(C.mem + b, 0, need);
    put_dates(b, st);
    uint64_t size = S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size, alloc = (size + 4095) & ~4095ull;
    if (level <= 2) {
        p32(b + 12, size > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)size);
        p32(b + 16, alloc > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)alloc);
        p32(b + 20, fattr(st, 0));
    } else {
        p32(b + 12, (uint32_t)size); p32(b + 16, (uint32_t)(size >> 32));
        p32(b + 20, (uint32_t)alloc); p32(b + 24, (uint32_t)(alloc >> 32));
        p32(b + 28, fattr(st, 0));
    }
    return 0;
}
static void set_times(const char *path, int fd, uint32_t b)
{
    uint16_t d = g16(b + 8), t = g16(b + 10);
    if (d) {
        struct tm lt = { 0 };
        lt.tm_year = (d >> 9) + 80; lt.tm_mon = ((d >> 5) & 15) - 1; lt.tm_mday = d & 31;
        lt.tm_hour = t >> 11; lt.tm_min = (t >> 5) & 63; lt.tm_sec = (t & 31) * 2; lt.tm_isdst = -1;
        struct timespec ts[2];
        ts[0].tv_sec = ts[1].tv_sec = mktime(&lt);
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
        if (fd >= 0) futimens(fd, ts); else utimensat(AT_FDCWD, path, ts, 0);
    }
}

/* ── the calls: DOSCALLS ─────────────────────────────────────────────── */
static uint32_t do_open(uint32_t pname, uint32_t phf, uint32_t pact, uint64_t size, uint32_t attr,
                        uint32_t oflags, uint32_t omode)
{
    char *name = gs(pname), h[PATH_MAX];
    if (!name || !ok(phf, 4)) return ERROR_INVALID_PARAMETER;
    uint32_t r = os2_to_host(name, h, sizeof h, 0);
    if (r) return r;
    struct stat st;
    int exists = stat(h, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) return ERROR_ACCESS_DENIED;
    int acc = omode & 7, fl = acc == 0 ? O_RDONLY : acc == 1 ? O_WRONLY : O_RDWR, action;
    if (omode & 0x80) fl |= O_CLOEXEC;
    if (exists) {
        switch (oflags & 15) {
        case 0: return ERROR_OPEN_FAILED;
        case 1: action = 1; break;
        case 2: action = 3; if (acc != 0) fl |= O_TRUNC; break;
        default: return ERROR_INVALID_PARAMETER;
        }
    } else {
        if (!(oflags & 0x10)) return ERROR_OPEN_FAILED;
        fl |= O_CREAT;
        action = 2;
    }
    int fd = open(h, fl, (attr & 1) ? 0444 : 0666);
    if (fd < 0) return oserr(errno);
    if (action != 1 && size && S_ISREG(exists ? st.st_mode : S_IFREG)) { if (ftruncate(fd, (off_t)size)) { } }
    p32(phf, (uint32_t)fd);
    if (pact) p32(pact, (uint32_t)action);
    return 0;
}
static uint32_t api_DosOpen(void) { return do_open(A(0), A(1), A(2), A(3), A(4), A(5), A(6)); }
static uint32_t api_DosOpenL(void) { return do_open(A(0), A(1), A(2), A(3) | (uint64_t)A(4) << 32, A(5), A(6), A(7)); }

static uint32_t api_DosClose(void) { return close((int)A(0)) ? ERROR_INVALID_HANDLE : 0; }

static uint32_t api_DosRead(void)
{
    uint32_t cb = A(2);
    void *p = gp(A(1), cb);
    if (!p && cb) return ERROR_INVALID_PARAMETER;
    ssize_t n;
    int fd = (int)A(0);
    UNLOCKED(do n = read(fd, p, cb); while (n < 0 && errno == EINTR));
    if (n < 0) return errno == EBADF ? ERROR_INVALID_HANDLE : oserr(errno);
    p32(A(3), (uint32_t)n);
    return 0;
}

static uint32_t api_DosWrite(void)
{
    uint32_t cb = A(2), done = 0;
    char *p = gp(A(1), cb);
    if (!p && cb) return ERROR_INVALID_PARAMETER;
    int fd = (int)A(0);
    while (done < cb) {
        ssize_t n;
        UNLOCKED(n = write(fd, p + done, cb - done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { p32(A(3), done); return errno == EBADF ? ERROR_INVALID_HANDLE : oserr(errno); }
        done += (uint32_t)n;
    }
    p32(A(3), done);
    return 0;
}

static uint32_t seek(int fd, int64_t off, uint32_t how, int64_t *out)
{
    if (how > 2) return ERROR_INVALID_PARAMETER;
    off_t r = lseek(fd, (off_t)off, how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) return errno == EINVAL ? ERROR_NEGATIVE_SEEK : oserr(errno);
    *out = r;
    return 0;
}
static uint32_t api_DosSetFilePtr(void)
{
    int64_t pos;
    uint32_t r = seek((int)A(0), (int32_t)A(1), A(2), &pos);
    if (!r && A(3)) p32(A(3), (uint32_t)pos);
    return r;
}
static uint32_t api_DosSetFilePtrL(void)
{
    int64_t pos;
    uint32_t r = seek((int)A(0), (int64_t)(A(1) | (uint64_t)A(2) << 32), A(3), &pos);
    if (!r && A(4)) { p32(A(4), (uint32_t)pos); p32(A(4) + 4, (uint32_t)((uint64_t)pos >> 32)); }
    return r;
}
static uint32_t api_DosSetFileSize(void) { return ftruncate((int)A(0), (off_t)A(1)) ? oserr(errno) : 0; }
static uint32_t api_DosSetFileSizeL(void) { return ftruncate((int)A(0), (off_t)(A(1) | (uint64_t)A(2) << 32)) ? oserr(errno) : 0; }

static uint32_t api_DosQueryHType(void)
{
    struct stat st;
    if (fstat((int)A(0), &st)) return ERROR_INVALID_HANDLE;
    uint32_t type = S_ISCHR(st.st_mode) ? 1 : (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) ? 2 : 0, attr = 0;
    if (type == 1) attr = isatty((int)A(0)) ? 3 : 4;
    p32(A(1), type);
    p32(A(2), attr);
    return 0;
}
static uint32_t api_DosQueryFHState(void)
{
    int fl = fcntl((int)A(0), F_GETFL), fd = fcntl((int)A(0), F_GETFD);
    if (fl < 0) return ERROR_INVALID_HANDLE;
    uint32_t m = (fl & O_ACCMODE) == O_WRONLY ? 1 : (fl & O_ACCMODE) == O_RDWR ? 2 : 0;
    if (fd & FD_CLOEXEC) m |= 0x80;
    m |= 0x40;                               /* OPEN_SHARE_DENYNONE */
    return p32(A(1), m) ? 0 : ERROR_INVALID_PARAMETER;
}
static uint32_t api_DosSetFHState(void)
{
    int fd = fcntl((int)A(0), F_GETFD);
    if (fd < 0) return ERROR_INVALID_HANDLE;
    fcntl((int)A(0), F_SETFD, (A(1) & 0x80) ? fd | FD_CLOEXEC : fd & ~FD_CLOEXEC);
    return 0;
}
static uint32_t api_DosDupHandle(void)
{
    uint32_t nh = g32(A(1));
    int r = nh == 0xFFFFFFFF ? dup((int)A(0)) : dup2((int)A(0), (int)nh);
    if (r < 0) return errno == EBADF ? ERROR_INVALID_HANDLE : ERROR_TOO_MANY_OPEN_FILES;
    p32(A(1), (uint32_t)r);
    return 0;
}
static uint32_t api_DosCreatePipe(void)
{
    int fds[2];
    if (pipe(fds)) return oserr(errno);
    p32(A(0), (uint32_t)fds[0]);
    p32(A(1), (uint32_t)fds[1]);
    return 0;
}
static uint32_t api_DosResetBuffer(void)
{
    if (A(0) == 0xFFFFFFFF) { sync(); return 0; }
    if (fsync((int)A(0)) && errno == EBADF) return ERROR_INVALID_HANDLE;
    return 0;
}
static uint32_t path_arg(uint32_t a, char *h, size_t n)
{
    char *s = gs(a);
    if (!s) return ERROR_INVALID_PARAMETER;
    return os2_to_host(s, h, n, 0);
}
static uint32_t api_DosDelete(void)
{
    char h[PATH_MAX];
    uint32_t r = path_arg(A(0), h, sizeof h);
    if (r) return r;
    struct stat st;
    if (stat(h, &st) == 0 && S_ISDIR(st.st_mode)) return ERROR_ACCESS_DENIED;
    return unlink(h) ? oserr(errno) : 0;
}
static uint32_t api_DosMove(void)
{
    char a[PATH_MAX], b[PATH_MAX];
    uint32_t r = path_arg(A(0), a, sizeof a);
    if (!r) r = path_arg(A(1), b, sizeof b);
    if (r) return r;
    struct stat sa, sb;
    if (stat(a, &sa)) return oserr(errno);
    if (stat(b, &sb) == 0 && (sa.st_ino != sb.st_ino || sa.st_dev != sb.st_dev)) return ERROR_ACCESS_DENIED;
    if (sa.st_ino == sb.st_ino && strcmp(a, b) == 0) {
        /* only the case differs: the name as given */
        char *s = gs(A(1)), *base = strrchr(b, '/');
        const char *nb = s;
        for (const char *q = s; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') nb = q + 1;
        if (base) snprintf(base + 1, sizeof b - (size_t)(base + 1 - b), "%s", nb);
    }
    return rename(a, b) ? oserr(errno) : 0;
}
static uint32_t api_DosCopy(void)
{
    char a[PATH_MAX], b[PATH_MAX];
    uint32_t r = path_arg(A(0), a, sizeof a);
    if (!r) r = path_arg(A(1), b, sizeof b);
    if (r) return r;
    if (!(A(2) & 1) && access(b, F_OK) == 0) return ERROR_ACCESS_DENIED;
    int in = open(a, O_RDONLY), out;
    if (in < 0) return oserr(errno);
    if ((out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) { close(in); return oserr(errno); }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) { n = -1; break; }
    close(in);
    close(out);
    return n < 0 ? ERROR_DISK_FULL : 0;
}
static uint32_t api_DosCreateDir(void)
{
    char h[PATH_MAX];
    uint32_t r = path_arg(A(0), h, sizeof h);
    if (r) return r;
    return mkdir(h, 0777) ? oserr(errno) : 0;
}
static uint32_t api_DosDeleteDir(void)
{
    char h[PATH_MAX];
    uint32_t r = path_arg(A(0), h, sizeof h);
    if (r) return r;
    return rmdir(h) ? (errno == ENOENT ? ERROR_PATH_NOT_FOUND : oserr(errno)) : 0;
}

static uint32_t api_DosQueryCurrentDisk(void)
{
    uint32_t map = 0;
    for (int d = 0; d < 26; d++) if (droot[d][0]) map |= 1u << d;
    p32(A(0), (uint32_t)curdrv + 1);
    if (A(1)) p32(A(1), map);
    return 0;
}
static uint32_t api_DosSetDefaultDisk(void)
{
    uint32_t n = A(0);
    if (n < 1 || n > 26 || !droot[n - 1][0]) return ERROR_INVALID_DRIVE;
    char cw[PATH_MAX];
    if (getcwd(cw, sizeof cw)) strcpy(dcwd[curdrv], cw);
    const char *to = dcwd[n - 1][0] ? dcwd[n - 1] : droot[n - 1];
    if (chdir(to) && chdir(droot[n - 1])) return ERROR_INVALID_DRIVE;
    curdrv = (int)n - 1;
    return 0;
}
static uint32_t api_DosQueryCurrentDir(void)
{
    uint32_t disk = A(0);
    int d = disk ? (int)disk - 1 : curdrv;
    if (d < 0 || d > 25 || !droot[d][0]) return ERROR_INVALID_DRIVE;
    char cw[PATH_MAX], out[PATH_MAX];
    const char *h = host_cwd(d, cw, sizeof cw), *rest = "";
    size_t rl = strlen(droot[d]);
    if (rl == 1) rest = h + 1;
    else if (strncmp(h, droot[d], rl) == 0) rest = h + rl + (h[rl] == '/');
    snprintf(out, sizeof out, "%s", rest);
    for (char *q = out; *q; q++) if (*q == '/') *q = '\\';
    uint32_t need = (uint32_t)strlen(out) + 1, cb = g32(A(2));
    if (cb < need) { p32(A(2), need); return ERROR_BUFFER_OVERFLOW; }
    if (!ok(A(1), need)) return ERROR_INVALID_PARAMETER;
    memcpy(C.mem + A(1), out, need);
    return 0;
}
static uint32_t api_DosSetCurrentDir(void)
{
    char *s = gs(A(0)), h[PATH_MAX];
    int d;
    if (!s) return ERROR_INVALID_PARAMETER;
    uint32_t r = os2_to_host(s, h, sizeof h, &d);
    if (r) return r;
    if (!is_dir(h)) return ERROR_PATH_NOT_FOUND;
    if (d == curdrv) return chdir(h) ? oserr(errno) : 0;
    strcpy(dcwd[d], h);
    return 0;
}

static uint32_t api_DosQueryPathInfo(void)
{
    char *s = gs(A(0)), h[PATH_MAX];
    uint32_t level = A(1), b = A(2), cb = A(3);
    if (!s) return ERROR_INVALID_PARAMETER;
    uint32_t r = os2_to_host(s, h, sizeof h, 0);
    if (r) return r;
    if (level == 5) {                        /* FIL_QUERYFULLNAME */
        char o[PATH_MAX];
        host_to_os2(h, o, sizeof o);
        uint32_t n = (uint32_t)strlen(o) + 1;
        if (n > cb || !ok(b, n)) return ERROR_BUFFER_OVERFLOW;
        memcpy(C.mem + b, o, n);
        return 0;
    }
    struct stat st;
    if (stat(h, &st)) return errno == ENOENT ? ERROR_FILE_NOT_FOUND : oserr(errno);
    return put_status(&st, level, b, cb);
}
static uint32_t api_DosQueryFileInfo(void)
{
    struct stat st;
    if (fstat((int)A(0), &st)) return ERROR_INVALID_HANDLE;
    return put_status(&st, A(1), A(2), A(3));
}
static uint32_t api_DosSetPathInfo(void)
{
    char h[PATH_MAX];
    uint32_t r = path_arg(A(0), h, sizeof h);
    if (r) return r;
    if (A(1) != 1 && A(1) != 2) return A(1) == 11 ? 0 : ERROR_INVALID_LEVEL;
    if (access(h, F_OK)) return ERROR_FILE_NOT_FOUND;
    set_times(h, -1, A(2));
    struct stat st;
    if (stat(h, &st) == 0 && !S_ISDIR(st.st_mode))
        chmod(h, (g32(A(2) + 20) & 1) ? st.st_mode & ~0222u : st.st_mode | S_IWUSR);
    return 0;
}
static uint32_t api_DosSetFileInfo(void)
{
    if (A(1) != 1 && A(1) != 2) return A(1) == 11 ? 0 : ERROR_INVALID_LEVEL;
    set_times(0, (int)A(0), A(2));
    return 0;
}

/* DosFindFirst / Next / Close */
typedef struct { int used; DIR *d; char dir[PATH_MAX]; char pat[NAME_MAX + 1]; uint32_t attr, level; int root;
                 char held[NAME_MAX + 1]; } find_t;
static find_t finds[64];

static int wild(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') {
        for (;;) {
            if (wild(p + 1, s)) return 1;
            if (!*s) return 0;
            s++;
        }
    }
    if (!*s) return 0;
    if (*p != '?' && toupper((unsigned char)*p) != toupper((unsigned char)*s)) return 0;
    return wild(p + 1, s + 1);
}
static int match(const char *pat, const char *name)
{
    if (!strcmp(pat, "*.*") || !strcmp(pat, "*")) return 1;
    if (wild(pat, name)) return 1;
    size_t l = strlen(pat);
    if (!strchr(name, '.')) {               /* "X*.*" and "*." find names without a dot */
        char p2[NAME_MAX + 1];
        snprintf(p2, sizeof p2, "%s", pat);
        if (l >= 2 && !strcmp(p2 + l - 2, ".*")) { p2[l - 2] = 0; return wild(p2, name); }
        if (l >= 1 && p2[l - 1] == '.') { p2[l - 1] = 0; return wild(p2, name); }
    }
    return 0;
}
static uint32_t find_fill(find_t *f, uint32_t b, uint32_t cb, uint32_t pcount)
{
    uint32_t want = g32(pcount), got = 0, pos = 0, prev = 0xFFFFFFFF;
    if (!want) want = 1;
    while (got < want) {
        char name[NAME_MAX + 1];
        if (f->held[0]) { strcpy(name, f->held); f->held[0] = 0; }
        else {
            struct dirent *de = readdir(f->d);
            if (!de) break;
            snprintf(name, sizeof name, "%s", de->d_name);
        }
        if (!match(f->pat, name)) continue;
        int dots = !strcmp(name, ".") || !strcmp(name, "..");
        if (dots && f->root) continue;
        char full[PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", f->dir, name);
        struct stat st;
        if (stat(full, &st)) continue;
        uint32_t at = fattr(&st, name);
        if ((at & 0x10) && !(f->attr & 0x10)) continue;
        if (((f->attr >> 8) & at) != ((f->attr >> 8) & 0x37)) continue;   /* the MUST_HAVE_ bits */
        uint32_t nl = (uint32_t)strlen(name);
        if (nl > 255) continue;
        uint32_t lv = f->level, fixed = lv == 1 ? 29 : lv == 2 ? 33 : lv == 11 ? 37 : 41;
        uint32_t size = (fixed + nl + 1 + 3) & ~3u;
        if (pos + size > cb || !ok(b + pos, size)) {
            strcpy(f->held, name);
            if (!got) return ERROR_BUFFER_OVERFLOW;
            break;
        }
        uint32_t e = b + pos;
        memset(C.mem + e, 0, size);
        put_dates(e + 4, &st);
        uint64_t sz = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size, al = (sz + 4095) & ~4095ull;
        if (lv <= 2) {
            p32(e + 16, (uint32_t)(sz > 0xFFFFFFFF ? 0xFFFFFFFF : sz));
            p32(e + 20, (uint32_t)(al > 0xFFFFFFFF ? 0xFFFFFFFF : al));
            p32(e + 24, at);
            if (lv == 2) p32(e + 28, 4);
        } else {
            p32(e + 16, (uint32_t)sz); p32(e + 20, (uint32_t)(sz >> 32));
            p32(e + 24, (uint32_t)al); p32(e + 28, (uint32_t)(al >> 32));
            p32(e + 32, at);
            if (lv == 12) p32(e + 36, 4);
        }
        p8(e + fixed - 1, (uint8_t)nl);
        memcpy(C.mem + e + fixed, name, nl + 1);
        if (prev != 0xFFFFFFFF) p32(b + prev, pos - prev);
        prev = pos;
        pos += size;
        got++;
    }
    p32(pcount, got);
    return got ? 0 : ERROR_NO_MORE_FILES;
}
static void find_close(find_t *f) { if (f->used && f->d) closedir(f->d); memset(f, 0, sizeof *f); }
static uint32_t api_DosFindFirst(void)
{
    char *spec = gs(A(0));
    uint32_t phdir = A(1), attr = A(2), b = A(3), cb = A(4), pcount = A(5), level = A(6);
    if (!spec || !ok(phdir, 4) || !ok(pcount, 4)) return ERROR_INVALID_PARAMETER;
    if (level != 1 && level != 2 && level != 11 && level != 12) return ERROR_INVALID_LEVEL;
    uint32_t hd = g32(phdir);
    int slot;
    if (hd == 0xFFFFFFFF) {
        for (slot = 1; slot < 64 && finds[slot].used; slot++) { }
        if (slot == 64) return 113;          /* ERROR_NO_MORE_SEARCH_HANDLES */
    } else if (hd >= 1 && hd <= 64) slot = (int)hd - 1;
    else return ERROR_INVALID_HANDLE;
    find_close(&finds[slot]);
    find_t *f = &finds[slot];
    char dpart[PATH_MAX];
    const char *base = spec;
    for (const char *q = spec; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') base = q + 1;
    size_t dl = (size_t)(base - spec);
    snprintf(dpart, sizeof dpart, "%.*s", (int)dl, spec);
    if (dl > 1 && (dpart[dl - 1] == '\\' || dpart[dl - 1] == '/') && dpart[dl - 2] != ':') dpart[dl - 1] = 0;
    if (!dpart[0]) strcpy(dpart, ".");
    else if (dpart[dl - 1] == ':') strcat(dpart, ".");
    int d;
    uint32_t r = os2_to_host(dpart, f->dir, sizeof f->dir, &d);
    if (r) return r;
    snprintf(f->pat, sizeof f->pat, "%s", *base ? base : "*");
    if (!(f->d = opendir(f->dir))) return ERROR_PATH_NOT_FOUND;
    f->used = 1; f->attr = attr; f->level = level;
    f->root = !strcmp(f->dir, droot[d]);
    r = find_fill(f, b, cb, pcount);
    if (r) { find_close(f); return r; }
    p32(phdir, (uint32_t)slot + 1);
    return 0;
}
static uint32_t api_DosFindNext(void)
{
    uint32_t hd = A(0);
    if (hd < 1 || hd > 64 || !finds[hd - 1].used) return ERROR_INVALID_HANDLE;
    return find_fill(&finds[hd - 1], A(1), A(2), A(3));
}
static uint32_t api_DosFindClose(void)
{
    uint32_t hd = A(0);
    if (hd < 1 || hd > 64 || !finds[hd - 1].used) return ERROR_INVALID_HANDLE;
    find_close(&finds[hd - 1]);
    return 0;
}

static uint32_t api_DosQueryFSInfo(void)
{
    uint32_t disk = A(0), level = A(1), b = A(2), cb = A(3);
    int d = disk ? (int)disk - 1 : curdrv;
    if (d < 0 || d > 25 || !droot[d][0]) return ERROR_INVALID_DRIVE;
    if (level == 1) {
        struct statvfs sv;
        if (cb < 18 || !ok(b, 18)) return ERROR_BUFFER_OVERFLOW;
        if (statvfs(droot[d], &sv)) memset(&sv, 0, sizeof sv);
        uint64_t unit = sv.f_frsize ? sv.f_frsize : 4096, blocks = sv.f_blocks, avail = sv.f_bavail;
        while (blocks > 0xFFFFFFFF) { blocks >>= 1; avail >>= 1; unit <<= 1; }
        p32(b, 0); p32(b + 4, (uint32_t)(unit / 512)); p32(b + 8, (uint32_t)blocks);
        p32(b + 12, (uint32_t)avail); p16(b + 16, 512);
        return 0;
    }
    if (level == 2) {
        if (cb < 17 || !ok(b, 17)) return ERROR_BUFFER_OVERFLOW;
        memset(C.mem + b, 0, 17);
        const char *lbl = d == 2 ? "OS2" : d == 3 ? "STICK" : "";
        p8(b + 4, (uint8_t)strlen(lbl));
        memcpy(C.mem + b + 5, lbl, strlen(lbl));
        return 0;
    }
    return ERROR_INVALID_LEVEL;
}

/* memory */
static uint32_t api_DosAllocMem(void)
{
    if (!A(1)) return ERROR_INVALID_PARAMETER;
    uint32_t a = mem_alloc(A(1));
    if (!a) return ERROR_NOT_ENOUGH_MEMORY;
    return p32(A(0), a) ? 0 : (mem_free(a), ERROR_INVALID_PARAMETER);
}
static uint32_t api_DosAllocSharedMem(void)
{
    uint32_t a = mem_alloc(A(2));
    if (!a) return ERROR_NOT_ENOUGH_MEMORY;
    p32(A(0), a);
    return 0;
}
static uint32_t api_DosFreeMem(void) { return mem_free(A(0)); }
static uint32_t api_ok(void) { return 0; }
static uint32_t api_DosQueryMem(void)
{
    uint32_t a = A(0), want = g32(A(1)), u = a / UNIT, size, fl;
    if (u >= nunits) return ERROR_INVALID_ADDRESS;
    if (!uused[u]) {
        uint32_t e = u;
        while (e < nunits && !uused[e]) e++;
        size = e * UNIT - a; fl = 0x4000;    /* PAG_FREE */
    } else {
        uint32_t s = u;
        while (s > 0 && !ulen[s] && uused[s - 1]) s--;
        uint32_t end = ulen[s] && s + ulen[s] > u ? s + ulen[s] : u + 1;
        if (!ulen[s]) { end = u; while (end < nunits && uused[end]) end++; }
        size = end * UNIT - a;
        fl = 0x13 | (ulen[s] ? 0 : 4) | (a == s * UNIT && ulen[s] ? 0x10000 : 0);
    }
    if (want && size > want) size = want;
    p32(A(1), size);
    p32(A(2), fl);
    return 0;
}

static uint32_t api_DosGetInfoBlocks(void)
{
    if (A(0)) p32(A(0), C.fs_base);
    if (A(1)) p32(A(1), pib);
    return 0;
}

static uint32_t api_DosExit(void) { if (A(0) == 0) thread_end(A(1)); do_exit(A(1)); }
static uint32_t api_DosExitList(void)
{
    uint32_t code = A(0), fn = A(1);
    switch (code & 0xFF) {
    case 1:
        if (nexit < 32) { exitlist[nexit].fn = fn; exitlist[nexit].order = (code >> 8) & 0xFF; nexit++; }
        return 0;
    case 2:
        for (int i = 0; i < nexit; i++) if (exitlist[i].fn == fn) { exitlist[i] = exitlist[--nexit]; break; }
        return 0;
    case 3:
        if (!running_exitlist) return ERROR_INVALID_FUNCTION;
        C.eip = thunk_addr(T_CBRET);         /* back to do_exit, for the next */
        api_noret = 1;
        return 0;
    }
    return ERROR_INVALID_PARAMETER;
}

static uint32_t api_DosGetDateTime(void)
{
    uint32_t b = A(0);
    if (!ok(b, 11)) return ERROR_INVALID_PARAMETER;
    struct timeval tv;
    gettimeofday(&tv, 0);
    struct tm lt;
    time_t t = tv.tv_sec;
    localtime_r(&t, &lt);
    p8(b, (uint8_t)lt.tm_hour); p8(b + 1, (uint8_t)lt.tm_min); p8(b + 2, (uint8_t)lt.tm_sec);
    p8(b + 3, (uint8_t)(tv.tv_usec / 10000)); p8(b + 4, (uint8_t)lt.tm_mday); p8(b + 5, (uint8_t)(lt.tm_mon + 1));
    p16(b + 6, (uint16_t)(lt.tm_year + 1900));
    p16(b + 8, (uint16_t)(int16_t)(-lt.tm_gmtoff / 60));
    p8(b + 10, (uint8_t)lt.tm_wday);
    return 0;
}
static uint32_t api_DosSleep(void)
{
    uint32_t ms = A(0);
    if (!ms) { UNLOCKED(sched_yield()); return 0; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    UNLOCKED(while (nanosleep(&ts, &ts) && errno == EINTR) { });
    return 0;
}
static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static uint32_t api_DosQuerySysInfo(void)
{
    uint32_t s = A(0), e = A(1), b = A(2), cb = A(3);
    if (s < 1 || e < s || e > 30) return ERROR_INVALID_PARAMETER;
    if ((e - s + 1) * 4 > cb || !ok(b, (e - s + 1) * 4)) return ERROR_BUFFER_OVERFLOW;
    for (uint32_t i = s; i <= e; i++) {
        uint32_t v = 0;
        switch (i) {
        case 1: v = 260; break;              /* QSV_MAX_PATH_LENGTH */
        case 2: v = 16; break;
        case 3: v = 16; break;
        case 4: v = 0; break;
        case 5: v = 3; break;                /* boot drive C: */
        case 6: v = 1; break;
        case 7: v = 3; break;
        case 8: v = 32; break;
        case 9: v = 248; break;
        case 10: v = 4096; break;            /* QSV_PAGE_SIZE */
        case 11: v = 20; break;              /* version 20.45: Warp 4.52 */
        case 12: v = 45; break;
        case 13: v = 0; break;
        case 14: v = (uint32_t)ms_now(); break;
        case 15: v = (uint32_t)time(0); break;
        case 16: v = 0; break;
        case 17: case 18: case 19: case 20: case 21: v = C.size; break;
        case 22: v = 32; break;
        case 23: v = 255; break;             /* QSV_MAX_COMP_LENGTH */
        case 24: v = 1; break;
        case 25: v = (uint32_t)getpid(); break;
        case 26: v = 1; break;               /* QSV_NUMPROCESSORS */
        }
        p32(b + (i - s) * 4, v);
    }
    return 0;
}
static uint32_t api_DosQueryCp(void)
{
    uint32_t cb = A(0), b = A(1);
    uint32_t cps[2] = { 437, 850 }, n = 0;
    while (n < 2 && (n + 1) * 4 <= cb && p32(b + n * 4, cps[n])) n++;
    p32(A(2), n * 4);
    return n ? 0 : ERROR_BUFFER_OVERFLOW;
}
static uint32_t api_DosDevConfig(void)
{
    static const uint8_t v[] = { 0, 0, 0, 1, 0, 0xFC, 1 };
    uint32_t item = A(1);
    if (item >= sizeof v) return ERROR_INVALID_PARAMETER;
    return p8(A(0), v[item]) ? 0 : ERROR_INVALID_PARAMETER;
}
static uint32_t api_DosSetRelMaxFH(void)
{
    if (A(1)) p32(A(1), 256);
    return 0;
}
static uint32_t api_DosScanEnv(void)
{
    char *name = gs(A(0));
    if (!name) return ERROR_INVALID_PARAMETER;
    size_t l = strlen(name);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t p = envp; ok(p, 1) && C.mem[p]; ) {
            char *s = (char *)C.mem + p;
            if ((pass ? !strncasecmp(s, name, l) : !strncmp(s, name, l)) && s[l] == '=') {
                p32(A(1), p + (uint32_t)l + 1);
                return 0;
            }
            p += (uint32_t)strlen(s) + 1;
        }
    return ERROR_ENVVAR_NOT_FOUND;
}
static uint32_t api_DosBeep(void) { return 0; }
static uint32_t api_DosQueryVerify(void) { p32(A(0), 0); return 0; }
static uint32_t api_DosQueryExtLIBPATH(void) { p8(A(0), 0); return 0; }
static uint32_t api_DosFlatToSel(void) { uint32_t a = C.r[EAX]; return ((a >> 16) << 19 | 7 << 16) | (a & 0xFFFF); }
static uint32_t api_DosSelToFlat(void) { uint32_t v = C.r[EAX]; return ((v >> 19) << 16) | (v & 0xFFFF); }
static uint32_t api_DosEnterMustComplete(void) { if (A(0)) p32(A(0), 1); return 0; }
static uint32_t api_DosSetSignalExceptionFocus(void) { if (A(1)) p32(A(1), 1); return 0; }

static uint32_t api_DosSetExceptionHandler(void)
{
    uint32_t rec = A(0);
    if (!ok(rec, 8)) return ERROR_INVALID_PARAMETER;
    p32(rec, g32(C.fs_base));
    p32(C.fs_base, rec);
    return 0;
}
static uint32_t api_DosUnsetExceptionHandler(void)
{
    uint32_t rec = A(0), at = C.fs_base;
    for (int i = 0; i < 1000; i++) {
        uint32_t cur = g32(at);
        if (!cur || cur == 0xFFFFFFFF) return 0x6A3;    /* ERROR_INVALID_EXCEPTION_HANDLER... */
        if (cur == rec) { p32(at, g32(rec)); return 0; }
        at = cur;
    }
    return ERROR_INVALID_PARAMETER;
}
static uint32_t api_DosUnwindException(void)
{
    uint32_t target = A(1);
    p32(C.fs_base, A(0));
    if (target) {
        C.r[ESP] += 4;
        C.eip = target;
        api_noret = 1;
    }
    return 0;
}
static uint32_t api_DosRaiseException(void)
{
    uint32_t r = A(0);
    if (!ok(r, 24)) return ERROR_INVALID_PARAMETER;
    uint32_t sp = C.r[ESP], ret = g32(sp);
    C.eip = ret; C.r[ESP] = sp + 4;          /* (continuing goes back to the caller) */
    C.op_eip = ret;
    api_noret = 1;
    dispatch(g32(r), g32(r + 16), g32(r + 20), g32(r + 24), g32(r + 12) ? g32(r + 12) : ret);
    C.r[EAX] = 0;
    return 0;
}

/* modules */
static mod_t *mod_by_handle(uint32_t h) { for (int i = 0; i < nmods; i++) if (mods[i]->hmod == h) return mods[i]; return 0; }
static uint32_t api_DosQueryModuleName(void)
{
    uint32_t h = A(0), cb = A(1), b = A(2);
    char o[PATH_MAX];
    mod_t *m = mod_by_handle(h);
    if (m) host_to_os2(m->path, o, sizeof o);
    else if (h >= 0x1000 && h < 0x1000 + NLIBS) snprintf(o, sizeof o, "C:\\OS2\\DLL\\%s.DLL", libs[h - 0x1000]);
    else return ERROR_INVALID_HANDLE;
    uint32_t n = (uint32_t)strlen(o) + 1;
    if (n > cb || !ok(b, n)) return 24;      /* ERROR_BAD_LENGTH */
    memcpy(C.mem + b, o, n);
    return 0;
}
static uint32_t api_DosQueryModuleHandle(void)
{
    char *n = gs(A(0));
    if (!n) return ERROR_INVALID_PARAMETER;
    int lib = builtin_lib(n);
    mod_t *m;
    if (lib >= 0) return p32(A(1), 0x1000 + (uint32_t)lib) ? 0 : ERROR_INVALID_PARAMETER;
    if (!(m = find_mod(n))) return ERROR_MOD_NOT_FOUND;
    return p32(A(1), m->hmod) ? 0 : ERROR_INVALID_PARAMETER;
}
static uint32_t api_DosLoadModule(void)
{
    char *n = gs(A(2)), err[512];
    if (!n) return ERROR_INVALID_PARAMETER;
    int lib = builtin_lib(n);
    if (lib >= 0) return p32(A(3), 0x1000 + (uint32_t)lib) ? 0 : ERROR_INVALID_PARAMETER;
    mod_t *m = load_dll(n, err, sizeof err);
    if (!m) {
        if (trace) fprintf(stderr, "[os2x86] DosLoadModule %s: %s\n", n, err);
        if (A(0) && A(1)) {
            size_t k = strlen(n) < A(1) - 1 ? strlen(n) : A(1) - 1;
            if (ok(A(0), (uint32_t)k + 1)) { memcpy(C.mem + A(0), n, k); C.mem[A(0) + k] = 0; }
        }
        return ERROR_MOD_NOT_FOUND;
    }
    run_inits();
    return p32(A(3), m->hmod) ? 0 : ERROR_INVALID_PARAMETER;
}
static uint32_t api_DosQueryProcAddr(void)
{
    uint32_t h = A(0), ord = A(1), pn = A(2), pp = A(3);
    char *name = pn ? gs(pn) : 0, err[256];
    if (name && name[0] == '#') { ord = (uint32_t)atoi(name + 1); name = 0; }
    if (h >= 0x1000 && h < 0x1000 + NLIBS) {
        int lib = (int)(h - 0x1000);
        if (name && !api_find(lib, 0, name)) return ERROR_PROC_NOT_FOUND;
        return p32(pp, thunk_for(lib, name ? 0 : ord, name)) ? 0 : ERROR_INVALID_PARAMETER;
    }
    mod_t *m = mod_by_handle(h);
    if (!m) return ERROR_INVALID_HANDLE;
    if (name) { int o = export_ord(m, name); if (o < 0) return ERROR_PROC_NOT_FOUND; ord = (uint32_t)o; }
    uint32_t a;
    if (entry_addr(m, ord, &a, err, sizeof err)) return 182;   /* ERROR_INVALID_ORDINAL */
    return p32(pp, a) ? 0 : ERROR_INVALID_PARAMETER;
}
static uint32_t api_DosQueryProcType(void) { return p32(A(3), 1) ? 0 : ERROR_INVALID_PARAMETER; }

/* programs */
static int find_program(const char *name, char *out, size_t n)
{
    char h[PATH_MAX], t[PATH_MAX + 8];
    const char *b = name;
    for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    int ext = strchr(b, '.') != 0;
    for (int e = 0; e < 2; e++) {
        if (e && ext) break;
        snprintf(t, sizeof t, "%s%s", name, e ? ".EXE" : "");
        if (!os2_to_host(t, h, sizeof h, 0) && access(h, X_OK) == 0 && !is_dir(h)) { snprintf(out, n, "%s", h); return 1; }
    }
    if (b != name) return 0;
    const char *path = getenv("PATH");
    if (!path) return 0;
    char pl[8192];
    snprintf(pl, sizeof pl, "%s", path);
    int unixy = !strchr(pl, ';') && !(isalpha((unsigned char)pl[0]) && pl[1] == ':');
    for (char *dir = strtok(pl, unixy ? ":" : ";"); dir; dir = strtok(0, unixy ? ":" : ";"))
        for (int e = 0; e < 2; e++) {
            if (e && ext) break;
            char p[PATH_MAX * 2];
            if (unixy || dir[0] == '/') { snprintf(p, sizeof p, "%s/%s%s", dir, name, e ? ".EXE" : ""); norm(p); casefix(p); }
            else {
                snprintf(t, sizeof t, "%s\\%s%s", dir, name, e ? ".EXE" : "");
                if (os2_to_host(t, p, sizeof p, 0)) continue;
            }
            if (access(p, X_OK) == 0 && !is_dir(p)) { snprintf(out, n, "%s", p); return 1; }
        }
    return 0;
}

static int split_args(const char *s, char **argv, int max, char *buf)
{
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        argv[n++] = buf;
        int q = 0;
        while (*s && (q || (*s != ' ' && *s != '\t'))) {
            if (*s == '\\' && s[1] == '"') { *buf++ = '"'; s += 2; continue; }
            if (*s == '"') { q = !q; s++; continue; }
            *buf++ = *s++;
        }
        *buf++ = 0;
    }
    argv[n] = 0;
    return n;
}

static void put_result(uint32_t pres, int status)
{
    if (WIFEXITED(status)) { p32(pres, 0); p32(pres + 4, (uint32_t)WEXITSTATUS(status)); }
    else { p32(pres, WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGILL ? 2 : 3); p32(pres + 4, (uint32_t)WTERMSIG(status)); }
}

static uint32_t api_DosExecPgm(void)
{
    uint32_t flags = A(2), pargs = A(3), penv = A(4), pres = A(5);
    char *name = gs(A(6)), h[PATH_MAX];
    if (!name) return ERROR_INVALID_PARAMETER;
    if (!find_program(name, h, sizeof h)) {
        if (A(0) && A(1) && ok(A(0), A(1))) snprintf((char *)C.mem + A(0), A(1), "%s", name);
        return ERROR_FILE_NOT_FOUND;
    }
    /* arguments: "program\0arguments\0" */
    const char *astr = "";
    if (pargs && gs(pargs)) {
        char *a0 = gs(pargs);
        char *a1 = gs(pargs + (uint32_t)strlen(a0) + 1);
        if (a1) astr = a1;
    }
    size_t al = strlen(astr);
    char *abuf = malloc(al + 2), *av[256 + 3];
    char **argv = av + 2;
    int ac = split_args(astr, argv + 1, 256, abuf);
    (void)ac;
    char **envv = environ, *ev[1024];
    if (penv && gs(penv)) {
        int k = 0;
        for (uint32_t p = penv; k < 1023 && ok(p, 1) && C.mem[p]; p += (uint32_t)strlen((char *)C.mem + p) + 1)
            ev[k++] = (char *)C.mem + p;
        ev[k] = 0;
        envv = ev;
    }
    fflush(0);
    pid_t pid = fork();
    if (pid < 0) { free(abuf); return ERROR_NOT_ENOUGH_MEMORY; }
    if (!pid) {
        if (x86_lx(h, 0) == 1) {
            argv[-1] = self_path; argv[0] = h;
            execve(self_path, argv - 1, envv);
        } else {
            argv[0] = h;
            execve(h, argv, envv);
        }
        _exit(127);
    }
    free(abuf);
    if ((flags & 7) == 0) {                  /* EXEC_SYNC */
        int st;
        UNLOCKED(while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { });
        if (pres) put_result(pres, st);
    } else if (pres) {
        p32(pres, (uint32_t)pid);
        p32(pres + 4, 0);
    }
    return 0;
}
static uint32_t api_DosWaitChild(void)
{
    uint32_t option = A(1), pres = A(2), ppid = A(3), pid = A(4);
    int st;
    pid_t r;
    UNLOCKED(do r = waitpid(pid ? (pid_t)pid : -1, &st, option == 1 ? WNOHANG : 0); while (r < 0 && errno == EINTR));
    if (r == 0) return ERROR_CHILD_NOT_COMPLETE;
    if (r < 0) return ERROR_WAIT_NO_CHILDREN;
    if (pres) put_result(pres, st);
    if (ppid) p32(ppid, (uint32_t)r);
    return 0;
}
static uint32_t api_DosKillProcess(void) { return kill((pid_t)A(1), SIGTERM) ? ERROR_INVALID_PROCID : 0; }
static uint32_t api_DosQueryAppType(void)
{
    char *n = gs(A(0)), h[PATH_MAX];
    if (!n) return ERROR_INVALID_PARAMETER;
    if (!find_program(n, h, sizeof h)) return ERROR_FILE_NOT_FOUND;
    uint32_t fl = 0, t = 2;
    int k = x86_lx(h, &fl);
    if (k == 2) t = 0x10;
    else if (k == 1) t = (fl >> 8) & 7;
    return p32(A(1), t) ? 0 : ERROR_INVALID_PARAMETER;
}
/* threads */
static thr_t *thr_get(uint32_t tid)
{
    return tid >= 1 && tid <= MAXTHR && thrs[tid - 1] && thrs[tid - 1]->state == 1 ? thrs[tid - 1] : 0;
}
static void *thr_main(void *arg)
{
    me = arg;
    pthread_mutex_lock(&gil);
    while (me->suspended) pthread_cond_wait(&gcv, &gil);
    run(0);
    return 0;
}
static uint32_t api_DosCreateThread(void)
{
    uint32_t ptid = A(0), fn = A(1), param = A(2), flags = A(3), cb = A(4);
    int i;
    for (i = 1; i < MAXTHR && thrs[i] && thrs[i]->state != 3; i++) { }
    if (i == MAXTHR) return 164;             /* ERROR_MAX_THRDS_REACHED */
    if (!ok(ptid, 4)) return ERROR_INVALID_PARAMETER;
    if (cb < 0x10000) cb = 0x10000;
    if (cb > 0x4000000) cb = 0x4000000;
    uint32_t stack = mem_alloc(cb + 0x100);
    if (!stack) return ERROR_NOT_ENOUGH_MEMORY;
    thr_t *t = thrs[i] ? thrs[i] : calloc(1, sizeof *t);
    if (!t) { mem_free(stack); return ERROR_NOT_ENOUGH_MEMORY; }
    memset(t, 0, sizeof *t);
    cpu_reset(&t->cpu);
    t->cpu.mem = C.mem; t->cpu.size = C.size; t->cpu.lo = C.lo;
    t->cpu.thunk_lo = C.thunk_lo; t->cpu.thunk_hi = C.thunk_hi;
    t->tid = (uint32_t)i + 1;
    t->stack = stack;
    t->state = 1;
    t->suspended = flags & 1;                /* CREATE_SUSPENDED */
    /* its information blocks just past its stack */
    uint32_t top = stack + cb, ttib = top, ttib2 = top + 0x40;
    p32(ttib, 0xFFFFFFFF); p32(ttib + 4, stack); p32(ttib + 8, top); p32(ttib + 12, ttib2);
    p32(ttib + 16, 20); p32(ttib + 20, t->tid);
    p32(ttib2, t->tid); p32(ttib2 + 4, 0x200); p32(ttib2 + 8, 20);
    t->cpu.fs_base = t->cpu.tib = ttib;
    /* fn(param), returning to the end of the thread */
    p32(top - 4, param);
    p32(top - 8, thunk_addr(T_THREXIT));
    t->cpu.r[ESP] = top - 8;
    t->cpu.eip = fn;
    thrs[i] = t;
    nlive++;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    int r = pthread_create(&pt, &at, thr_main, t);
    pthread_attr_destroy(&at);
    if (r) { nlive--; t->state = 3; mem_free(stack); return ERROR_NOT_ENOUGH_MEMORY; }
    p32(ptid, t->tid);
    return 0;
}
static uint32_t api_DosWaitThread(void)
{
    uint32_t ptid = A(0), option = A(1);
    if (!ok(ptid, 4)) return ERROR_INVALID_PARAMETER;
    uint32_t want = g32(ptid);
    for (;;) {
        int others = want == 1 && me->tid != 1;      /* (thread 1: till the end) */
        for (int i = 1; i < MAXTHR; i++) {
            thr_t *t = thrs[i];
            if (!t || t->state == 3 || t == me || (want && t->tid != want)) continue;
            if (t->state == 2) {
                t->state = 3;
                mem_free(t->stack);
                p32(ptid, t->tid);
                return 0;
            }
            others++;
        }
        if (!others) return 309;                 /* ERROR_INVALID_THREADID */
        if (option == 1) return 294;             /* ERROR_THREAD_NOT_TERMINATED */
        pthread_cond_wait(&gcv, &gil);
    }
}
static uint32_t api_DosKillThread(void)
{
    thr_t *t = thr_get(A(0));
    if (!t || t == me) return 309;
    t->kill = 1;
    t->suspended = 0;
    pthread_cond_broadcast(&gcv);
    return 0;
}
static uint32_t api_DosSuspendThread(void)
{
    thr_t *t = thr_get(A(0));
    if (!t) return 309;
    t->suspended = 1;
    if (t == me) checkpoint();
    return 0;
}
static uint32_t api_DosResumeThread(void)
{
    thr_t *t = thr_get(A(0));
    if (!t) return 309;
    t->suspended = 0;
    pthread_cond_broadcast(&gcv);
    return 0;
}
static uint32_t api_DosEnterCritSec(void) { crit_tid = me->tid; crit_n++; return 0; }
static uint32_t api_DosExitCritSec(void)
{
    if (crit_tid == me->tid && crit_n && !--crit_n) { crit_tid = 0; pthread_cond_broadcast(&gcv); }
    return 0;
}

/* semaphores: event, mutex, muxwait; waiting gives up the lock (gcv) */
enum { SEM_EVENT = 1, SEM_MUTEX, SEM_MUX };
typedef struct { int used, kind; char name[64]; uint32_t count, owner, nrec, any; uint32_t rec[64][2]; } sem_t2;
static sem_t2 sems[256];
static sem_t2 *sem_new(uint32_t pname, uint32_t ph, int kind, uint32_t *err)
{
    char *n = pname ? gs(pname) : 0;
    if (n && *n)
        for (int i = 0; i < 256; i++)
            if (sems[i].used && !strcasecmp(sems[i].name, n)) { *err = 285; return 0; }   /* ERROR_DUPLICATE_NAME */
    for (int i = 0; i < 256; i++)
        if (!sems[i].used) {
            memset(&sems[i], 0, sizeof sems[i]);
            sems[i].used = 1;
            sems[i].kind = kind;
            snprintf(sems[i].name, sizeof sems[i].name, "%s", n ? n : "");
            if (!p32(ph, 0x10000 + (uint32_t)i)) { sems[i].used = 0; *err = ERROR_INVALID_PARAMETER; return 0; }
            return &sems[i];
        }
    *err = 290;                                  /* ERROR_TOO_MANY_HANDLES */
    return 0;
}
static sem_t2 *sem_get(uint32_t h, int kind)
{
    h -= 0x10000;
    return h < 256 && sems[h].used && (!kind || sems[h].kind == kind) ? &sems[h] : 0;
}
static uint32_t api_DosOpenSem(void)
{
    uint32_t pname = A(0), ph = A(1);
    char *n = pname ? gs(pname) : 0;
    if (!n || !*n) return sem_get(g32(ph), 0) ? 0 : ERROR_INVALID_HANDLE;
    for (int i = 0; i < 256; i++)
        if (sems[i].used && !strcasecmp(sems[i].name, n)) return p32(ph, 0x10000 + (uint32_t)i) ? 0 : ERROR_INVALID_PARAMETER;
    return 187;                                  /* ERROR_SEM_NOT_FOUND */
}
static uint32_t api_DosCloseSem(void)
{
    sem_t2 *s = sem_get(A(0), 0);
    if (!s) return ERROR_INVALID_HANDLE;
    if (s->kind == SEM_MUTEX && s->owner && s->owner != me->tid && thr_get(s->owner)) return 301;   /* ERROR_SEM_BUSY */
    s->used = 0;
    pthread_cond_broadcast(&gcv);
    return 0;
}
static struct timespec *deadline(uint32_t ms, struct timespec *ts)
{
    if (ms == 0xFFFFFFFF) return 0;              /* SEM_INDEFINITE_WAIT */
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
    return ts;
}
static int wait_until(struct timespec *dl) { return dl ? pthread_cond_timedwait(&gcv, &gil, dl) : pthread_cond_wait(&gcv, &gil); }

static uint32_t api_DosCreateEventSem(void)
{
    uint32_t err;
    sem_t2 *s = sem_new(A(0), A(1), SEM_EVENT, &err);
    if (!s) return err;
    s->count = A(3) ? 1 : 0;
    return 0;
}
static uint32_t api_DosPostEventSem(void)
{
    sem_t2 *s = sem_get(A(0), SEM_EVENT);
    if (!s) return ERROR_INVALID_HANDLE;
    pthread_cond_broadcast(&gcv);
    return s->count++ ? ERROR_ALREADY_POSTED : 0;
}
static uint32_t api_DosResetEventSem(void)
{
    sem_t2 *s = sem_get(A(0), SEM_EVENT);
    if (!s) return ERROR_INVALID_HANDLE;
    if (A(1)) p32(A(1), s->count);
    if (!s->count) return ERROR_ALREADY_RESET;
    s->count = 0;
    return 0;
}
static uint32_t api_DosWaitEventSem(void)
{
    uint32_t h = A(0), t = A(1);
    struct timespec ts, *dl = deadline(t, &ts);
    for (;;) {
        sem_t2 *s = sem_get(h, SEM_EVENT);
        if (!s) return ERROR_INVALID_HANDLE;
        if (s->count) return 0;
        if (!t || wait_until(dl) == ETIMEDOUT) {
            s = sem_get(h, SEM_EVENT);
            return s && s->count ? 0 : ERROR_TIMEOUT;
        }
    }
}
static uint32_t api_DosQueryEventSem(void)
{
    sem_t2 *s = sem_get(A(0), SEM_EVENT);
    if (!s) return ERROR_INVALID_HANDLE;
    return p32(A(1), s->count) ? 0 : ERROR_INVALID_PARAMETER;
}

static uint32_t api_DosCreateMutexSem(void)
{
    uint32_t err;
    sem_t2 *s = sem_new(A(0), A(1), SEM_MUTEX, &err);
    if (!s) return err;
    if (A(3)) { s->owner = me->tid; s->count = 1; }
    return 0;
}
static uint32_t api_DosRequestMutexSem(void)
{
    uint32_t h = A(0), t = A(1);
    struct timespec ts, *dl = deadline(t, &ts);
    for (;;) {
        sem_t2 *s = sem_get(h, SEM_MUTEX);
        if (!s) return ERROR_INVALID_HANDLE;
        if (!s->owner || s->owner == me->tid) { s->owner = me->tid; s->count++; return 0; }
        if (!thr_get(s->owner)) { s->owner = me->tid; s->count = 1; return 105; }   /* ERROR_SEM_OWNER_DIED */
        if (!t || wait_until(dl) == ETIMEDOUT) return ERROR_TIMEOUT;
    }
}
static uint32_t api_DosReleaseMutexSem(void)
{
    sem_t2 *s = sem_get(A(0), SEM_MUTEX);
    if (!s) return ERROR_INVALID_HANDLE;
    if (s->owner != me->tid || !s->count) return ERROR_NOT_OWNER;
    if (!--s->count) { s->owner = 0; pthread_cond_broadcast(&gcv); }
    return 0;
}
static uint32_t api_DosQueryMutexSem(void)
{
    sem_t2 *s = sem_get(A(0), SEM_MUTEX);
    if (!s) return ERROR_INVALID_HANDLE;
    if (A(1)) p32(A(1), s->owner ? (uint32_t)getpid() : 0);
    if (A(2)) p32(A(2), s->owner);
    if (A(3)) p32(A(3), s->count);
    return 0;
}

static uint32_t api_DosCreateMuxWaitSem(void)
{
    uint32_t pname = A(0), ph = A(1), n = A(2), recs = A(3), fl = A(4), err;
    if (n > 64 || (n && !ok(recs, n * 8))) return ERROR_INVALID_PARAMETER;
    for (uint32_t k = 0; k < n; k++) if (!sem_get(g32(recs + 8 * k), 0)) return ERROR_INVALID_HANDLE;
    sem_t2 *s = sem_new(pname, ph, SEM_MUX, &err);
    if (!s) return err;
    s->nrec = n;
    s->any = !(fl & 4);                          /* DCMW_WAIT_ALL: else any */
    for (uint32_t k = 0; k < n; k++) { s->rec[k][0] = g32(recs + 8 * k); s->rec[k][1] = g32(recs + 8 * k + 4); }
    return 0;
}
static int sem_ready(sem_t2 *s)
{
    return s->kind == SEM_EVENT ? s->count != 0 : (!s->owner || s->owner == me->tid || !thr_get(s->owner));
}
static void sem_take(sem_t2 *s)
{
    if (s->kind != SEM_MUTEX) return;
    if (s->owner != me->tid) { s->owner = me->tid; s->count = 0; }
    s->count++;
}
static uint32_t api_DosWaitMuxWaitSem(void)
{
    uint32_t h = A(0), t = A(1), pu = A(2);
    struct timespec ts, *dl = deadline(t, &ts);
    for (;;) {
        sem_t2 *m = sem_get(h, SEM_MUX);
        if (!m) return ERROR_INVALID_HANDLE;
        uint32_t ready = 0;
        int first = -1;
        for (uint32_t k = 0; k < m->nrec; k++) {
            sem_t2 *s = sem_get(m->rec[k][0], 0);
            if (!s) return ERROR_INVALID_HANDLE;
            if (sem_ready(s)) { ready++; if (first < 0) first = (int)k; }
        }
        if (m->any ? ready > 0 : ready == m->nrec) {
            if (m->any) sem_take(sem_get(m->rec[first][0], 0));
            else for (uint32_t k = 0; k < m->nrec; k++) sem_take(sem_get(m->rec[k][0], 0));
            if (pu) p32(pu, m->any ? m->rec[first][1] : 0);
            return 0;
        }
        if (!t || wait_until(dl) == ETIMEDOUT) return ERROR_TIMEOUT;
    }
}

/* NLS, MSG */
static uint32_t api_DosQueryCtryInfo(void)
{
    uint32_t cb = A(0), b = A(2);
    uint8_t ci[44] = { 0 };
    uint32_t v[3] = { 1, 437, 0 };
    memcpy(ci, v, 12);
    memcpy(ci + 12, "$", 2); memcpy(ci + 17, ",", 2); memcpy(ci + 19, ".", 2);
    memcpy(ci + 21, "-", 2); memcpy(ci + 23, ":", 2);
    ci[25] = 0; ci[26] = 2; ci[27] = 0;
    memcpy(ci + 32, ",", 2);
    uint32_t n = cb < 44 ? cb : 44;
    if (!ok(b, n)) return ERROR_INVALID_PARAMETER;
    memcpy(C.mem + b, ci, n);
    if (A(3)) p32(A(3), n);
    return 0;
}
static uint32_t api_DosQueryDBCSEnv(void)
{
    if (A(0) >= 2) { p8(A(2), 0); p8(A(2) + 1, 0); }
    return 0;
}
static uint32_t api_DosMapCase(void)
{
    uint32_t cb = A(0);
    char *p = gp(A(2), cb);
    if (!p) return ERROR_INVALID_PARAMETER;
    for (uint32_t i = 0; i < cb; i++) p[i] = (char)toupper((unsigned char)p[i]);
    return 0;
}
static uint32_t api_DosPutMessage(void)
{
    uint32_t cb = A(1);
    char *p = gp(A(2), cb);
    if (!p && cb) return ERROR_INVALID_PARAMETER;
    if (cb && write((int)A(0), p, cb) < 0) return ERROR_INVALID_HANDLE;
    return 0;
}

/* ── KBDCALLS, VIOCALLS: the 16-bit console calls, on this terminal ─── */
static uint16_t W(uint32_t o) { return g16(a16 + o); }
static uint32_t FP(uint32_t o) { uint32_t v = g32(a16 + o); return ((v >> 19) << 16) + (v & 0xFFFF); }   /* 16:16: flat */

static int kbd_pend_ch = -1, kbd_pend_scan;    /* a key KbdPeek saw */
static int rd1(int ms)                          /* a byte from stdin, waiting ms (-1: as long as it takes) */
{
    struct pollfd p = { 0, POLLIN, 0 };
    int r;
    UNLOCKED(r = poll(&p, 1, ms));
    if (r <= 0) return -1;
    unsigned char ch;
    ssize_t n;
    UNLOCKED(n = read(0, &ch, 1));
    return n == 1 ? ch : -2;
}
/* a key: its character (0xE0 for the extended ones) and scan code */
static int kbd_key(int wait, int *scan)
{
    if (kbd_pend_ch >= 0) { int ch = kbd_pend_ch; *scan = kbd_pend_scan; kbd_pend_ch = -1; return ch; }
    struct termios old, raw;
    int tty = tcgetattr(0, &old) == 0;
    if (tty) { raw = old; raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO); raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; tcsetattr(0, TCSANOW, &raw); }
    int ch = rd1(wait ? -1 : 0);
    *scan = 0;
    if (ch == 27) {                             /* ESC [ A ...: the cursor keys */
        int c2 = rd1(40);
        if (c2 == '[' || c2 == 'O') {
            int c3 = rd1(40), n = 0;
            while (c3 >= '0' && c3 <= '9') { n = n * 10 + c3 - '0'; c3 = rd1(40); }
            static const char k[] = "ABCDHF";
            static const int sc[] = { 0x48, 0x50, 0x4D, 0x4B, 0x47, 0x4F };
            const char *f = c3 > 0 ? strchr(k, c3) : 0;
            if (f && c3) { ch = 0xE0; *scan = sc[f - k]; }
            else if (c3 == '~') {
                static const int tl[] = { 0, 0x47, 0x52, 0x53, 0x4F, 0x49, 0x51 };
                if (n >= 1 && n <= 6) { ch = 0xE0; *scan = tl[n]; }
                else if (n >= 11 && n <= 15) { ch = 0; *scan = 0x3B + n - 11; }        /* F1-F5 */
                else if (n >= 17 && n <= 21) { ch = 0; *scan = 0x40 + n - 17; }       /* F6-F10 */
            }
        } else if (c2 >= 0) { kbd_pend_ch = c2; kbd_pend_scan = 0; }
        if (ch == 27) *scan = 1;
    } else if (ch == '\n') { ch = '\r'; *scan = 0x1C; }
    else if (ch == 127) { ch = 8; *scan = 0x0E; }
    if (tty) tcsetattr(0, TCSANOW, &old);
    return ch < 0 ? -1 : ch;
}
static void put_keyinfo(uint32_t p, int ch, int scan)
{
    if (!ok(p, 10)) return;
    memset(C.mem + p, 0, 10);
    if (ch < 0) return;
    p8(p, (uint8_t)ch); p8(p + 1, (uint8_t)scan);
    p8(p + 2, (uint8_t)(0x40 | (ch == 0 || ch == 0xE0 ? 2 : 0)));      /* final character; extended */
    p32(p + 6, (uint32_t)ms_now());
}
static uint32_t api_KbdCharIn(void)           /* (pkbci, fWait, hkbd) */
{
    int scan, ch = kbd_key(W(2) == 0, &scan);
    put_keyinfo(FP(4), ch, scan);
    return 0;
}
static uint32_t api_KbdPeek(void)             /* (pkbci, hkbd) */
{
    if (kbd_pend_ch < 0) {
        int scan, ch = kbd_key(0, &scan);
        if (ch >= 0) { kbd_pend_ch = ch; kbd_pend_scan = scan; }
    }
    put_keyinfo(FP(2), kbd_pend_ch, kbd_pend_scan);
    return 0;
}
static uint32_t api_KbdStringIn(void)         /* (pch, pchIn {cb, cchIn}, fsWait, hkbd) */
{
    uint32_t buf = FP(8), in = FP(4), cb = g16(in), n = 0;
    if (kbd_pend_ch > 0 && cb) { p8(buf, (uint8_t)kbd_pend_ch); n = 1; }
    kbd_pend_ch = -1;
    while (n < cb) {
        int ch = rd1(-1);
        if (ch < 0 || ch == '\n') break;
        if (ch == '\r') continue;
        p8(buf + n++, (uint8_t)ch);
    }
    if (n < cb) p8(buf + n, '\r');
    p16(in + 2, (uint16_t)n);
    return 0;
}
static uint32_t api_KbdFlushBuffer(void) { tcflush(0, TCIFLUSH); kbd_pend_ch = -1; return 0; }
static uint32_t api_KbdGetStatus(void)        /* (pkbdinfo, hkbd) */
{
    uint32_t p = FP(2);
    if (!ok(p, 10)) return ERROR_INVALID_PARAMETER;
    memset(C.mem + p, 0, 10);
    p16(p, 10);
    p16(p + 2, 0x0009);                          /* echo on, cooked */
    p16(p + 4, '\r');
    return 0;
}

static void tty_out(const char *s, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w;
        UNLOCKED(w = write(1, s + done, n - done));
        if (w <= 0) break;
        done += (size_t)w;
    }
}
static void tty_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tty_printf(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) tty_out(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
static void screen_size(int *rows, int *cols)
{
    struct winsize ws;
    *rows = 25; *cols = 80;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) { *rows = ws.ws_row; *cols = ws.ws_col; }
}
static void attr_out(uint8_t a)                  /* a text attribute: CGA colours as ANSI ones */
{
    static const int map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    tty_printf("\033[0;%d;%dm", (a & 8 ? 90 : 30) + map[a & 7], 40 + map[(a >> 4) & 7]);
}
static void at(uint32_t row, uint32_t col) { tty_printf("\033[%u;%uH", row + 1, col + 1); }

static uint32_t api_VioWrtTTY(void)           /* (pch, cb, hvio) */
{
    uint32_t n = W(2);
    char *p = gp(FP(4), n);
    if (!p && n) return ERROR_INVALID_PARAMETER;
    tty_out(p, n);
    return 0;
}
static uint32_t api_VioGetMode(void)          /* (pvmi, hvio) */
{
    uint32_t p = FP(2), cb = g16(p);
    if (!ok(p, 2) || cb < 2) return ERROR_INVALID_PARAMETER;
    int rows, cols;
    screen_size(&rows, &cols);
    uint8_t m[34] = { 0 };
    m[2] = 1;                                     /* VGMT_OTHER: colour text */
    m[3] = 4;                                     /* 16 colours */
    m[4] = (uint8_t)cols; m[5] = (uint8_t)(cols >> 8);
    m[6] = (uint8_t)rows; m[7] = (uint8_t)(rows >> 8);
    m[8] = (uint8_t)(cols * 9); m[9] = (uint8_t)((cols * 9) >> 8);
    m[10] = (uint8_t)(rows * 16); m[11] = (uint8_t)((rows * 16) >> 8);
    if (cb > sizeof m) cb = sizeof m;
    if (!ok(p, cb)) return ERROR_INVALID_PARAMETER;
    memcpy(C.mem + p + 2, m + 2, cb - 2);
    return 0;
}
static uint32_t cur_row, cur_col;
static uint32_t api_VioSetCurPos(void)        /* (row, col, hvio) */
{
    cur_row = W(4); cur_col = W(2);
    at(cur_row, cur_col);
    return 0;
}
static uint32_t api_VioGetCurPos(void)        /* (prow, pcol, hvio) */
{
    struct termios old, raw;
    if (isatty(0) && isatty(1) && tcgetattr(0, &old) == 0) {
        raw = old;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        tcsetattr(0, TCSANOW, &raw);
        tty_out("\033[6n", 4);                    /* the terminal says: ESC [ row ; col R */
        int r = 0, cl = 0, ch, *v = &r;
        if (rd1(200) == 27 && rd1(200) == '[') {
            while ((ch = rd1(200)) >= 0 && ch != 'R') {
                if (ch == ';') v = &cl;
                else if (ch >= '0' && ch <= '9') *v = *v * 10 + ch - '0';
            }
            if (r && cl) { cur_row = (uint32_t)r - 1; cur_col = (uint32_t)cl - 1; }
        }
        tcsetattr(0, TCSANOW, &old);
    }
    p16(FP(6), (uint16_t)cur_row);
    p16(FP(2), (uint16_t)cur_col);
    return 0;
}
static void scroll(int up)                     /* (top, left, bot, right, lines, pcell, hvio) */
{
    int rows, cols;
    screen_size(&rows, &cols);
    uint32_t top = W(14), left = W(12), bot = W(10), right = W(8), lines = W(6), cell = FP(2);
    if (bot >= (uint32_t)rows) bot = (uint32_t)rows - 1;
    if (right >= (uint32_t)cols) right = (uint32_t)cols - 1;
    if (top > bot || left > right) return;
    uint8_t ch = ok(cell, 2) ? C.mem[cell] : ' ', a = ok(cell, 2) ? C.mem[cell + 1] : 7;
    attr_out(a);
    tty_out("\0337", 2);
    if (lines >= bot - top + 1 || left > 0 || right + 1 < (uint32_t)cols) {
        /* the whole area (or not the full width: cleared) */
        if (lines) for (uint32_t r = top; r <= bot; r++) {
            at(r, left);
            for (uint32_t k = left; k <= right; k++) tty_out((char *)&ch, 1);
        }
    } else if (lines) {
        tty_printf("\033[%u;%ur\033[%u%c\033[r", top + 1, bot + 1, lines, up ? 'S' : 'T');
    }
    tty_out("\0338\033[0m", 6);
}
static uint32_t api_VioScrollUp(void) { scroll(1); return 0; }
static uint32_t api_VioScrollDn(void) { scroll(0); return 0; }
static uint32_t api_VioWrtCharStr(void)       /* (pch, cb, row, col, hvio) */
{
    uint32_t n = W(6);
    char *p = gp(FP(8), n);
    if (!p && n) return ERROR_INVALID_PARAMETER;
    tty_out("\0337", 2); at(W(4), W(2)); tty_out(p, n); tty_out("\0338", 2);
    return 0;
}
static uint32_t api_VioWrtCharStrAtt(void)    /* (pch, cb, row, col, pattr, hvio) */
{
    uint32_t n = W(10), pa = FP(2);
    char *p = gp(FP(12), n);
    if (!p && n) return ERROR_INVALID_PARAMETER;
    tty_out("\0337", 2); at(W(8), W(6)); attr_out(ok(pa, 1) ? C.mem[pa] : 7);
    tty_out(p, n); tty_out("\033[0m\0338", 6);
    return 0;
}
static uint32_t api_VioWrtNChar(void)         /* (pch, times, row, col, hvio) */
{
    uint32_t n = W(6), pc = FP(8);
    char ch = ok(pc, 1) ? (char)C.mem[pc] : ' ';
    tty_out("\0337", 2); at(W(4), W(2));
    for (uint32_t i = 0; i < n; i++) tty_out(&ch, 1);
    tty_out("\0338", 2);
    return 0;
}
static uint32_t api_VioWrtNCell(void)         /* (pcell, times, row, col, hvio) */
{
    uint32_t n = W(6), pc = FP(8);
    char ch = ok(pc, 2) ? (char)C.mem[pc] : ' ';
    tty_out("\0337", 2); at(W(4), W(2)); attr_out(ok(pc, 2) ? C.mem[pc + 1] : 7);
    for (uint32_t i = 0; i < n; i++) tty_out(&ch, 1);
    tty_out("\033[0m\0338", 6);
    return 0;
}
static uint32_t api_VioWrtCellStr(void)       /* (pcells, cb, row, col, hvio) */
{
    uint32_t n = W(6) & ~1u, pc = FP(8);
    if (!ok(pc, n ? n : 1)) return ERROR_INVALID_PARAMETER;
    tty_out("\0337", 2); at(W(4), W(2));
    for (uint32_t i = 0; i < n; i += 2) { if (!i || C.mem[pc + i + 1] != C.mem[pc + i - 1]) attr_out(C.mem[pc + i + 1]); tty_out((char *)C.mem + pc + i, 1); }
    tty_out("\033[0m\0338", 6);
    return 0;
}
static uint32_t api_VioReadCellStr(void)      /* (pcells, pcb, row, col, hvio): not readable here: blanks */
{
    uint32_t pcb = FP(6), b = FP(10), n = g16(pcb) & ~1u;
    if (!ok(b, n ? n : 1)) return ERROR_INVALID_PARAMETER;
    for (uint32_t i = 0; i < n; i += 2) { C.mem[b + i] = ' '; C.mem[b + i + 1] = 7; }
    return 0;
}
static uint32_t api_VioReadCharStr(void)      /* (pch, pcb, row, col, hvio) */
{
    uint32_t pcb = FP(6), b = FP(10), n = g16(pcb);
    if (!ok(b, n ? n : 1)) return ERROR_INVALID_PARAMETER;
    memset(C.mem + b, ' ', n);
    return 0;
}
static uint16_t cur_type[4] = { 14, 15, 1, 0 };
static uint32_t api_VioGetCurType(void)       /* (pci, hvio) */
{
    uint32_t p = FP(2);
    for (int i = 0; i < 4; i++) p16(p + 2 * (uint32_t)i, cur_type[i]);
    return 0;
}
static uint32_t api_VioSetCurType(void)       /* (pci, hvio) */
{
    uint32_t p = FP(2);
    for (int i = 0; i < 4; i++) cur_type[i] = g16(p + 2 * (uint32_t)i);
    tty_out(cur_type[3] == 0xFFFF ? "\033[?25l" : "\033[?25h", 6);
    return 0;
}
static uint32_t api_VioGetCp(void) { p16(FP(2), 437); return 0; }   /* (reserved, pcp, hvio) */
static uint32_t api_ok16(void) { return 0; }

static const api_t apis[] = {
    { L_DOS, 110, "DosForceDelete", api_DosDelete, 0 },
    { L_DOS, 111, "DosKillThread", api_DosKillThread, 0 },
    { L_DOS, 209, "DosSetMaxFH", api_ok, 0 },
    { L_DOS, 210, "DosSetVerify", api_ok, 0 },
    { L_DOS, 212, "DosError", api_ok, 0 },
    { L_DOS, 218, "DosSetFileInfo", api_DosSetFileInfo, 0 },
    { L_DOS, 219, "DosSetPathInfo", api_DosSetPathInfo, 0 },
    { L_DOS, 220, "DosSetDefaultDisk", api_DosSetDefaultDisk, 0 },
    { L_DOS, 221, "DosSetFHState", api_DosSetFHState, 0 },
    { L_DOS, 223, "DosQueryPathInfo", api_DosQueryPathInfo, 0 },
    { L_DOS, 224, "DosQueryHType", api_DosQueryHType, 0 },
    { L_DOS, 225, "DosQueryVerify", api_DosQueryVerify, 0 },
    { L_DOS, 226, "DosDeleteDir", api_DosDeleteDir, 0 },
    { L_DOS, 227, "DosScanEnv", api_DosScanEnv, 0 },
    { L_DOS, 229, "DosSleep", api_DosSleep, 0 },
    { L_DOS, 230, "DosGetDateTime", api_DosGetDateTime, 0 },
    { L_DOS, 231, "DosDevConfig", api_DosDevConfig, 0 },
    { L_DOS, 232, "DosEnterCritSec", api_DosEnterCritSec, 0 },
    { L_DOS, 233, "DosExitCritSec", api_DosExitCritSec, 0 },
    { L_DOS, 234, "DosExit", api_DosExit, 0 },
    { L_DOS, 235, "DosKillProcess", api_DosKillProcess, 0 },
    { L_DOS, 236, "DosSetPriority", api_ok, 0 },
    { L_DOS, 237, "DosResumeThread", api_DosResumeThread, 0 },
    { L_DOS, 238, "DosSuspendThread", api_DosSuspendThread, 0 },
    { L_DOS, 239, "DosCreatePipe", api_DosCreatePipe, 0 },
    { L_DOS, 254, "DosResetBuffer", api_DosResetBuffer, 0 },
    { L_DOS, 255, "DosSetCurrentDir", api_DosSetCurrentDir, 0 },
    { L_DOS, 256, "DosSetFilePtr", api_DosSetFilePtr, 0 },
    { L_DOS, 257, "DosClose", api_DosClose, 0 },
    { L_DOS, 258, "DosCopy", api_DosCopy, 0 },
    { L_DOS, 259, "DosDelete", api_DosDelete, 0 },
    { L_DOS, 260, "DosDupHandle", api_DosDupHandle, 0 },
    { L_DOS, 263, "DosFindClose", api_DosFindClose, 0 },
    { L_DOS, 264, "DosFindFirst", api_DosFindFirst, 0 },
    { L_DOS, 265, "DosFindNext", api_DosFindNext, 0 },
    { L_DOS, 270, "DosCreateDir", api_DosCreateDir, 0 },
    { L_DOS, 271, "DosMove", api_DosMove, 0 },
    { L_DOS, 272, "DosSetFileSize", api_DosSetFileSize, 0 },
    { L_DOS, 273, "DosOpen", api_DosOpen, 0 },
    { L_DOS, 274, "DosQueryCurrentDir", api_DosQueryCurrentDir, 0 },
    { L_DOS, 275, "DosQueryCurrentDisk", api_DosQueryCurrentDisk, 0 },
    { L_DOS, 276, "DosQueryFHState", api_DosQueryFHState, 0 },
    { L_DOS, 278, "DosQueryFSInfo", api_DosQueryFSInfo, 0 },
    { L_DOS, 279, "DosQueryFileInfo", api_DosQueryFileInfo, 0 },
    { L_DOS, 280, "DosWaitChild", api_DosWaitChild, 0 },
    { L_DOS, 281, "DosRead", api_DosRead, 0 },
    { L_DOS, 282, "DosWrite", api_DosWrite, 0 },
    { L_DOS, 283, "DosExecPgm", api_DosExecPgm, 0 },
    { L_DOS, 286, "DosBeep", api_DosBeep, 0 },
    { L_DOS, 291, "DosQueryCp", api_DosQueryCp, 0 },
    { L_DOS, 292, "DosSetDateTime", api_ok, 0 },
    { L_DOS, 296, "DosExitList", api_DosExitList, 0 },
    { L_DOS, 299, "DosAllocMem", api_DosAllocMem, 0 },
    { L_DOS, 300, "DosAllocSharedMem", api_DosAllocSharedMem, 0 },
    { L_DOS, 302, "DosGetSharedMem", api_ok, 0 },
    { L_DOS, 304, "DosFreeMem", api_DosFreeMem, 0 },
    { L_DOS, 305, "DosSetMem", api_ok, 0 },
    { L_DOS, 306, "DosQueryMem", api_DosQueryMem, 0 },
    { L_DOS, 311, "DosCreateThread", api_DosCreateThread, 0 },
    { L_DOS, 312, "DosGetInfoBlocks", api_DosGetInfoBlocks, 0 },
    { L_DOS, 318, "DosLoadModule", api_DosLoadModule, 0 },
    { L_DOS, 319, "DosQueryModuleHandle", api_DosQueryModuleHandle, 0 },
    { L_DOS, 320, "DosQueryModuleName", api_DosQueryModuleName, 0 },
    { L_DOS, 321, "DosQueryProcAddr", api_DosQueryProcAddr, 0 },
    { L_DOS, 322, "DosFreeModule", api_ok, 0 },
    { L_DOS, 323, "DosQueryAppType", api_DosQueryAppType, 0 },
    { L_DOS, 324, "DosCreateEventSem", api_DosCreateEventSem, 0 },
    { L_DOS, 325, "DosOpenEventSem", api_DosOpenSem, 0 },
    { L_DOS, 326, "DosCloseEventSem", api_DosCloseSem, 0 },
    { L_DOS, 327, "DosResetEventSem", api_DosResetEventSem, 0 },
    { L_DOS, 328, "DosPostEventSem", api_DosPostEventSem, 0 },
    { L_DOS, 329, "DosWaitEventSem", api_DosWaitEventSem, 0 },
    { L_DOS, 330, "DosQueryEventSem", api_DosQueryEventSem, 0 },
    { L_DOS, 331, "DosCreateMutexSem", api_DosCreateMutexSem, 0 },
    { L_DOS, 332, "DosOpenMutexSem", api_DosOpenSem, 0 },
    { L_DOS, 333, "DosCloseMutexSem", api_DosCloseSem, 0 },
    { L_DOS, 334, "DosRequestMutexSem", api_DosRequestMutexSem, 0 },
    { L_DOS, 335, "DosReleaseMutexSem", api_DosReleaseMutexSem, 0 },
    { L_DOS, 336, "DosQueryMutexSem", api_DosQueryMutexSem, 0 },
    { L_DOS, 337, "DosCreateMuxWaitSem", api_DosCreateMuxWaitSem, 0 },
    { L_DOS, 338, "DosOpenMuxWaitSem", api_DosOpenSem, 0 },
    { L_DOS, 339, "DosCloseMuxWaitSem", api_DosCloseSem, 0 },
    { L_DOS, 340, "DosWaitMuxWaitSem", api_DosWaitMuxWaitSem, 0 },
    { L_DOS, 348, "DosQuerySysInfo", api_DosQuerySysInfo, 0 },
    { L_DOS, 349, "DosWaitThread", api_DosWaitThread, 0 },
    { L_DOS, 354, "DosSetExceptionHandler", api_DosSetExceptionHandler, 0 },
    { L_DOS, 355, "DosUnsetExceptionHandler", api_DosUnsetExceptionHandler, 0 },
    { L_DOS, 356, "DosRaiseException", api_DosRaiseException, 0 },
    { L_DOS, 357, "DosUnwindException", api_DosUnwindException, 0 },
    { L_DOS, 378, "DosSetSignalExceptionFocus", api_DosSetSignalExceptionFocus, 0 },
    { L_DOS, 379, "DosSendSignalException", api_ok, 0 },
    { L_DOS, 380, "DosEnterMustComplete", api_DosEnterMustComplete, 0 },
    { L_DOS, 381, "DosExitMustComplete", api_DosEnterMustComplete, 0 },
    { L_DOS, 382, "DosSetRelMaxFH", api_DosSetRelMaxFH, 0 },
    { L_DOS, 418, "DosAcknowledgeSignalException", api_ok, 0 },
    { L_DOS, 425, "DosFlatToSel", api_DosFlatToSel, 0 },
    { L_DOS, 426, "DosSelToFlat", api_DosSelToFlat, 0 },
    { L_DOS, 428, "DosSetFileLocks", api_ok, 0 },
    { L_DOS, 586, "DosQueryProcType", api_DosQueryProcType, 0 },
    { L_DOS, 873, "DosSetExtLIBPATH", api_ok, 0 },
    { L_DOS, 874, "DosQueryExtLIBPATH", api_DosQueryExtLIBPATH, 0 },
    { L_DOS, 981, "DosOpenL", api_DosOpenL, 0 },
    { L_DOS, 986, "DosSetFileLocksL", api_ok, 0 },
    { L_DOS, 988, "DosSetFilePtrL", api_DosSetFilePtrL, 0 },
    { L_DOS, 989, "DosSetFileSizeL", api_DosSetFileSizeL, 0 },
    { L_NLS, 5, "DosQueryCtryInfo", api_DosQueryCtryInfo, 0 },
    { L_NLS, 6, "DosQueryDBCSEnv", api_DosQueryDBCSEnv, 0 },
    { L_NLS, 7, "DosMapCase", api_DosMapCase, 0 },
    { L_MSG, 5, "DosPutMessage", api_DosPutMessage, 0 },
    { L_KBD, 4, "KbdCharIn", api_KbdCharIn, 8 },
    { L_KBD, 9, "KbdStringIn", api_KbdStringIn, 12 },
    { L_KBD, 10, "KbdGetStatus", api_KbdGetStatus, 6 },
    { L_KBD, 11, "KbdSetStatus", api_ok16, 6 },
    { L_KBD, 13, "KbdFlushBuffer", api_KbdFlushBuffer, 2 },
    { L_KBD, 22, "KbdPeek", api_KbdPeek, 6 },
    { L_VIO, 7, "VioScrollUp", api_VioScrollUp, 16 },
    { L_VIO, 9, "VioGetCurPos", api_VioGetCurPos, 10 },
    { L_VIO, 10, "VioWrtCellStr", api_VioWrtCellStr, 12 },
    { L_VIO, 13, "VioWrtCharStr", api_VioWrtCharStr, 12 },
    { L_VIO, 15, "VioSetCurPos", api_VioSetCurPos, 6 },
    { L_VIO, 19, "VioWrtTTY", api_VioWrtTTY, 8 },
    { L_VIO, 21, "VioGetMode", api_VioGetMode, 6 },
    { L_VIO, 22, "VioSetMode", api_ok16, 6 },
    { L_VIO, 24, "VioReadCellStr", api_VioReadCellStr, 14 },
    { L_VIO, 26, "VioWrtNAttr", api_ok16, 12 },
    { L_VIO, 27, "VioGetCurType", api_VioGetCurType, 6 },
    { L_VIO, 30, "VioReadCharStr", api_VioReadCharStr, 14 },
    { L_VIO, 32, "VioSetCurType", api_VioSetCurType, 6 },
    { L_VIO, 40, "VioGetCp", api_VioGetCp, 8 },
    { L_VIO, 42, "VioSetCp", api_ok16, 6 },
    { L_VIO, 47, "VioScrollDn", api_VioScrollDn, 16 },
    { L_VIO, 48, "VioWrtCharStrAtt", api_VioWrtCharStrAtt, 16 },
    { L_VIO, 52, "VioWrtNCell", api_VioWrtNCell, 12 },
    { L_VIO, 53, "VioWrtNChar", api_VioWrtNChar, 12 },
    { 0, 0, 0, 0, 0 },
};

static const api_t *api_find(int lib, uint32_t ord, const char *name)
{
    for (const api_t *a = apis; a->name; a++)
        if (a->lib == lib && (name ? !strcmp(a->name, name) : a->ord == ord)) return a;
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fprintf(stderr, "usage: os2x86 PROGRAM.EXE [arguments]\n"
                        "  runs a 32-bit x86 OS/2 program (OS2X86_TRACE=1: its system calls on stderr)\n");
        return 2;
    }
    if (strchr(argv[0], '/') && realpath(argv[0], self_path)) { }
    else if (access(self_path, X_OK)) {
        const char *p = getenv("PATH");
        char pl[8192];
        snprintf(pl, sizeof pl, "%s", p ? p : "");
        for (char *d = strtok(pl, ":"); d; d = strtok(0, ":")) {
            char t[PATH_MAX * 2];
            snprintf(t, sizeof t, "%s/%s", d, argv[0]);
            if (access(t, X_OK) == 0 && realpath(t, self_path)) break;
        }
    }
    trace = getenv("OS2X86_TRACE") && *getenv("OS2X86_TRACE") && strcmp(getenv("OS2X86_TRACE"), "0");
    const char *mb = getenv("OS2X86_MEM");
    uint64_t size = (uint64_t)(mb ? strtoul(mb, 0, 10) : 512) << 20;
    if (size < (16u << 20)) size = 16u << 20;
    if (size > 0xF0000000ull) size = 0xF0000000ull;
    C.mem = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (C.mem == MAP_FAILED) die(8, "os2x86: no memory for the program (%u MB)", (unsigned)(size >> 20));
    pthread_mutex_lock(&gil);
    cpu_reset(&C);
    C.size = (uint32_t)size;
    C.lo = UNIT;
    /* the thunks: below 512 MB, where 16:16 pointers (tiles) reach */
    C.thunk_lo = (C.size < 0x20000000u ? C.size : 0x20000000u) - UNIT;
    C.thunk_hi = C.thunk_lo + UNIT;
    nunits = C.size / UNIT;
    uused = calloc(nunits, 1);
    ulen = calloc(nunits, sizeof *ulen);
    uused[0] = uused[nunits - 1] = uused[C.thunk_lo / UNIT] = 1;
    memset(C.mem + C.thunk_lo, 0xCC, UNIT);
    drives_init();

    /* the program: as this system names it, else as OS/2 does */
    char h[PATH_MAX], err[1024];
    if (access(argv[1], R_OK) == 0 && realpath(argv[1], h)) { }
    else if (!find_program(argv[1], h, sizeof h)) die(2, "SYS1041: The name %s is not recognized as a program.", argv[1]);
    const char *b = strrchr(h, '/');
    prog_name = b ? b + 1 : h;
    exe = load_module(h, 0, err, sizeof err);
    if (!exe) die(err[0] == 'S' && !strncmp(err, "SYS0002", 7) ? 2 : 193, "%s", err);
    uint32_t eo = H(exe, LX_EIPOBJ);
    if (!eo || (int)eo > exe->nobj) die(193, "SYS0193: %s has no starting point.", h);
    setup_process(argc, argv);
    run_inits();
    /* main: [esp] back to DosExit, the module, 0, environment, command line */
    C.r[ESP] = stack_top;
    gpush(cmdp); gpush(envp); gpush(0); gpush(exe->hmod); gpush(thunk_addr(T_EXIT));
    C.r[EAX] = C.r[EBX] = C.r[ECX] = C.r[EDX] = C.r[ESI] = C.r[EDI] = C.r[EBP] = 0;
    C.eip = exe->obase[eo - 1] + H(exe, LX_EIP);
    run(0);
    return 0;
}
