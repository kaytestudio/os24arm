/* userland/os2x86/test/os2test.c - os2x86's test program: freestanding C
 * that only calls DOSCALLS, made an LX .EXE by elf2lx.py (run-tests.sh).
 * It prints "ok ..." / "FAIL ..." lines and exits with the failures. */
typedef unsigned long ULONG;
typedef ULONG APIRET;

APIRET DosWrite(ULONG h, const void *buf, ULONG cb, ULONG *actual);
APIRET DosRead(ULONG h, void *buf, ULONG cb, ULONG *actual);
APIRET DosExit(ULONG action, ULONG result);
APIRET DosOpen(const char *name, ULONG *h, ULONG *action, ULONG size, ULONG attr, ULONG flags, ULONG mode, void *eas);
APIRET DosClose(ULONG h);
APIRET DosAllocMem(void **p, ULONG cb, ULONG flags);
APIRET DosFreeMem(void *p);
APIRET DosGetInfoBlocks(void **tib, void **pib);
APIRET DosScanEnv(const char *name, const char **value);
APIRET DosQueryCurrentDir(ULONG disk, char *buf, ULONG *cb);
APIRET DosQueryCurrentDisk(ULONG *disk, ULONG *map);
APIRET DosFindFirst(const char *spec, ULONG *hdir, ULONG attr, void *buf, ULONG cb, ULONG *count, ULONG level);
APIRET DosFindNext(ULONG hdir, void *buf, ULONG cb, ULONG *count);
APIRET DosFindClose(ULONG hdir);
APIRET DosSetExceptionHandler(void *rec);
APIRET DosUnsetExceptionHandler(void *rec);
APIRET DosSetFilePtr(ULONG h, long off, ULONG how, ULONG *pos);
APIRET DosDelete(const char *name);
APIRET DosQueryPathInfo(const char *name, ULONG level, void *buf, ULONG cb);
APIRET DosQuerySysInfo(ULONG first, ULONG last, void *buf, ULONG cb);
APIRET DosCreateDir(const char *name, void *eas);
APIRET DosDeleteDir(const char *name);

static int fails;

static ULONG slen(const char *s) { ULONG n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { ULONG a; DosWrite(1, s, slen(s), &a); }
static void num(ULONG v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(b + i);
}
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void check(int c, const char *what) { out(c ? "ok " : "FAIL "); out(what); out("\n"); if (!c) fails++; }

/* the exception handler: past the faulting "mov eax, [0]" (5 bytes), EAX = 42 */
typedef struct { void *prev; ULONG (*handler)(ULONG *, void *, ULONG *, void *); } xreg_t;
static ULONG handled;
static ULONG handler(ULONG *rep, void *reg, ULONG *ctx, void *dc)
{
    (void)reg; (void)dc;
    if (rep[0] != 0xC0000005) return 0;
    handled = rep[6];                       /* the address it tried */
    ctx[156 / 4] += 5;
    ctx[136 / 4] = 42;
    return 0xFFFFFFFF;                      /* XCPT_CONTINUE_EXECUTION */
}

static volatile double dv = 3.14159;

ULONG start(void)
{
    out("Hello from an x86 OS/2 program!\n");

    void *tib, *pib;
    check(DosGetInfoBlocks(&tib, &pib) == 0, "DosGetInfoBlocks");
    const char *cmd = ((const char **)pib)[3];
    while (*cmd) cmd++;
    cmd++;
    out("arguments: \""); out(cmd); out("\"\n");
    check(seq(cmd, "one \"two three\""), "command line");

    const char *v;
    check(DosScanEnv("OS2TEST", &v) == 0 && seq(v, "yes"), "DosScanEnv");
    check(DosScanEnv("NO_SUCH_VARIABLE_X", &v) == 203, "DosScanEnv, missing");

    char *m;
    check(DosAllocMem((void **)&m, 300000, 0x13) == 0 && m && m[299999] == 0, "DosAllocMem");
    for (int i = 0; i < 300000; i++) m[i] = (char)i;
    check(m[123456] == (char)123456, "memory");
    check(DosFreeMem(m) == 0, "DosFreeMem");

    ULONG disk, map, cb = 260;
    char dir[260];
    check(DosQueryCurrentDisk(&disk, &map) == 0 && disk >= 3, "DosQueryCurrentDisk");
    check(DosQueryCurrentDir(0, dir, &cb) == 0, "DosQueryCurrentDir");
    out("current: "); char d[4] = { (char)('A' + disk - 1), ':', '\\', 0 }; out(d); out(dir); out("\n");

    ULONG h, act, n;
    check(DosOpen("OS2TEST.TMP", &h, &act, 0, 0, 0x12, 0x42, 0) == 0 && act == 2, "DosOpen (create)");
    check(DosWrite(h, "written by OS/2\r\n", 17, &n) == 0 && n == 17, "DosWrite to a file");
    ULONG pos;
    check(DosSetFilePtr(h, 0, 0, &pos) == 0 && pos == 0, "DosSetFilePtr");
    char rb[32];
    check(DosRead(h, rb, sizeof rb, &n) == 0 && n == 17 && rb[11] == 'O', "DosRead");
    DosClose(h);
    ULONG fs3[6];
    check(DosQueryPathInfo("os2test.tmp", 1, fs3, sizeof fs3) == 0 && fs3[3] == 17, "DosQueryPathInfo (any case)");
    check(DosOpen("OS2TEST.TMP", &h, &act, 0, 0, 0x10, 0x42, 0) == 110, "DosOpen: exists, fail");
    check(DosOpen("NOSUCH.TMP", &h, &act, 0, 0, 0x01, 0x40, 0) == 110, "DosOpen: missing, fail");

    check(DosCreateDir("OS2TDIR", 0) == 0, "DosCreateDir");
    ULONG hdir = 0xFFFFFFFF, count = 10, found = 0, sawtmp = 0, sawdir = 0;
    char fb[2048];
    APIRET r = DosFindFirst("*.*", &hdir, 0x10, fb, sizeof fb, &count, 1);
    while (r == 0) {
        char *e = fb;
        for (ULONG k = 0; k < count; k++) {
            found++;
            if (seq(e + 29, "OS2TEST.TMP")) sawtmp = 1;
            if (seq(e + 29, "OS2TDIR") && (*(ULONG *)(e + 24) & 0x10)) sawdir = 1;
            e += *(ULONG *)e;
        }
        count = 10;
        r = DosFindNext(hdir, fb, sizeof fb, &count);
    }
    DosFindClose(hdir);
    check(r == 18 && sawtmp && sawdir, "DosFindFirst/Next");
    out("  ("); num(found); out(" entries)\n");
    check(DosDeleteDir("os2tdir") == 0, "DosDeleteDir");
    check(DosDelete("OS2TEST.TMP") == 0, "DosDelete");
    check(DosQueryPathInfo("OS2TEST.TMP", 1, fs3, sizeof fs3) == 2, "gone");

    ULONG ver[2];
    check(DosQuerySysInfo(11, 12, ver, sizeof ver) == 0 && ver[0] == 20, "DosQuerySysInfo");

    xreg_t reg = { 0, handler };
    check(DosSetExceptionHandler(&reg) == 0, "DosSetExceptionHandler");
    ULONG got;
    __asm__ volatile("movl $0, %%eax\n\t.byte 0xa1, 0x10, 0, 0, 0" : "=a"(got) : : "memory");   /* mov eax, [0x10] */
    check(got == 42 && handled == 0x10, "access violation, handled");
    DosUnsetExceptionHandler(&reg);

    double x = dv * 2;
    check((ULONG)(x * 1000) == 6283, "x87 arithmetic");
    long long big = 1234567890123LL;
    check((ULONG)(big >> 20) == 1177375, "64-bit shift");

    out(fails ? "some tests failed\n" : "all passed\n");
    DosExit(1, (ULONG)fails);
    return 99;
}
