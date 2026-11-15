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
APIRET DosCreateThread(ULONG *tid, void (*fn)(ULONG), ULONG param, ULONG flags, ULONG stack);
APIRET DosWaitThread(ULONG *tid, ULONG option);
APIRET DosCreateMutexSem(const char *name, ULONG *h, ULONG flags, ULONG state);
APIRET DosRequestMutexSem(ULONG h, ULONG timeout);
APIRET DosReleaseMutexSem(ULONG h);
APIRET DosCreateEventSem(const char *name, ULONG *h, ULONG flags, ULONG state);
APIRET DosPostEventSem(ULONG h);
APIRET DosWaitEventSem(ULONG h, ULONG timeout);
APIRET DosCreateMuxWaitSem(const char *name, ULONG *h, ULONG n, ULONG *recs, ULONG flags);
APIRET DosWaitMuxWaitSem(ULONG h, ULONG timeout, ULONG *user);
APIRET DosSleep(ULONG ms);
APIRET DosCreateQueue(ULONG *hq, ULONG prio, const char *name);
APIRET DosWriteQueue(ULONG hq, ULONG req, ULONG cb, void *buf, ULONG prio);
APIRET DosReadQueue(ULONG hq, ULONG *reqdata, ULONG *cb, void **buf, ULONG elem, ULONG nowait, unsigned char *prio, ULONG hev);
APIRET DosQueryQueue(ULONG hq, ULONG *count);
APIRET DosCloseQueue(ULONG hq);
APIRET DosStartSession(void *sd, ULONG *session, ULONG *pid);
APIRET DosQueryModuleName(ULONG hmod, ULONG cb, char *buf);
ULONG WinInitialize(ULONG flags);

static int fails;

/* a 16-bit call as OpenWatcom's runtime makes one (clib far16fun:
 * __Far16Func2): the arguments copied to the stack, a 16:16 stack (LSS
 * SP), a 16-bit code segment that loads DS, ES and makes the far call,
 * then back with a 16:32 jump and LSS ESP through the 16-bit SS.
 *   ULONG far16_call(ULONG target16, void *args, ULONG bytes)  -> DX:AX */
__asm__(
    "    .text\n"
    "flat_to_16:\n"                         /* ULONG flat_to_16(void *): DosFlatToSel (EAX in, EAX out) */
    "    movl 4(%esp), %eax\n"
    "    call DosFlatToSel\n"
    "    ret\n"
    "far16_call:\n"
    "    pushl %ebp\n pushl %edi\n pushl %ebx\n pushl %esi\n pushl %es\n pushl %ds\n"
    "    movl 28(%esp), %eax\n"
    "    movl 32(%esp), %esi\n"
    "    movl 36(%esp), %ecx\n"
    "    movl %esp, %ebp\n"
    "    pushl %ss\n pushl %ebp\n pushl %eax\n"
    "    addl $3, %ecx\n andb $-4, %cl\n"
    "    xorl %ebp, %ebp\n movw %cx, %bp\n"
    "    subl %ecx, %esp\n movl %esp, %edi\n shrl %ecx\n"
    "    rep movsw\n"
    "    movl %esp, %eax\n"
    "    call DosFlatToSel\n"
    "    pushl %eax\n"
    "    addw %ax, %bp\n"
    "    lssw (%esp), %sp\n"
    "    ljmpw *f16_ptr\n"
    "back32:\n"
    "    lssl 4(%ebp), %esp\n"
    "    roll $16, %eax\n movw %dx, %ax\n roll $16, %eax\n"
    "    popl %ds\n popl %es\n popl %esi\n popl %ebx\n popl %edi\n popl %ebp\n"
    "    ret\n"
    "    .code16\n"
    "f16_stub:\n"
    "    movw %ss, %cx\n movw %cx, %ds\n movw %cx, %es\n"
    "    lcallw *(%bp)\n"
    "    ljmpl $0x5b, $back32\n"
    "    .code32\n"
    "    .data\n"
    "f16_ptr: .short 0, 0\n"
    "    .text\n");
extern ULONG flat_to_16(const void *p);
extern ULONG far16_call(ULONG target, void *args, ULONG bytes);
extern unsigned short f16_ptr[2];
extern char f16_stub[], VIO16WRTTTY[], VIO16GETMODE[], VIO16GETCP[], KBD16PEEK[];

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

/* threads: a counter under a mutex, with a yield between its read and
 * its write (without the mutex, increments would get lost) */
static ULONG mtx, ev, ev2, woke;
static volatile ULONG counter;
static void adder(ULONG n)
{
    for (ULONG i = 0; i < n; i++) {
        DosRequestMutexSem(mtx, 0xFFFFFFFF);
        ULONG v = counter;
        if ((i & 15) == 0) DosSleep(0);
        counter = v + 1;
        DosReleaseMutexSem(mtx);
    }
}
static void waiter(ULONG x)
{
    if (DosWaitEventSem(ev, 0xFFFFFFFF) == 0) woke = x;
    DosPostEventSem(ev2);
}

ULONG start(void)
{
    out("Hello from an x86 OS/2 program!\n");

    void *tib, *pib;
    check(DosGetInfoBlocks(&tib, &pib) == 0, "DosGetInfoBlocks");
    const char *cmd = ((const char **)pib)[3];
    while (*cmd) cmd++;
    cmd++;
    out("arguments: \""); out(cmd); out("\"\n");
    if (seq(cmd, "child")) DosExit(1, 5);          /* (the DosStartSession test's child) */
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

    ULONG t1, t2, t3, tw;
    check(DosCreateMutexSem(0, &mtx, 0, 0) == 0, "DosCreateMutexSem");
    check(DosCreateThread(&t1, adder, 3000, 0, 65536) == 0 && DosCreateThread(&t2, adder, 3000, 0, 65536) == 0
          && DosCreateThread(&t3, adder, 3000, 2, 65536) == 0 && t1 != t2 && t1 > 1, "DosCreateThread (3)");
    ULONG w = t1, w2 = t2, w3 = t3;
    check(DosWaitThread(&w, 0) == 0 && DosWaitThread(&w2, 0) == 0 && DosWaitThread(&w3, 0) == 0, "DosWaitThread");
    out("  counter "); num(counter); out("\n");
    check(counter == 9000, "mutex: no increments lost");
    ULONG none = t1;
    check(DosWaitThread(&none, 1) == 309, "DosWaitThread: already collected");
    check(DosCreateEventSem(0, &ev, 0, 0) == 0 && DosCreateEventSem(0, &ev2, 0, 0) == 0, "DosCreateEventSem");
    check(DosWaitEventSem(ev, 30) == 640, "DosWaitEventSem: times out");
    check(DosCreateThread(&tw, waiter, 77, 0, 65536) == 0, "a waiting thread");
    DosSleep(50);
    check(woke == 0, "  still waiting");
    ULONG recs[2] = { ev2, 5 }, mux, user = 0;
    check(DosCreateMuxWaitSem(0, &mux, 1, recs, 2) == 0, "DosCreateMuxWaitSem");
    DosPostEventSem(ev);
    check(DosWaitMuxWaitSem(mux, 2000, &user) == 0 && user == 5, "DosWaitMuxWaitSem");
    ULONG any = 0;
    check(DosWaitThread(&any, 0) == 0 && any == tw && woke == 77, "event wakes the other thread");

    /* the 16-bit calls (Pascal: the last argument at the lowest address) */
    ULONG s16 = flat_to_16(f16_stub);
    f16_ptr[0] = (unsigned short)s16;
    f16_ptr[1] = (unsigned short)(s16 >> 16);
    check(((s16 >> 16) & 7) == 7, "DosFlatToSel: a tiled selector");
    static char msg[] = "  (16-bit VioWrtTTY)\r\n";
    ULONG pm = flat_to_16(msg);
    unsigned short a1[4] = { 0, (unsigned short)(sizeof msg - 1), (unsigned short)pm, (unsigned short)(pm >> 16) };
    check((far16_call(flat_to_16(VIO16WRTTTY), a1, 8) & 0xFFFF) == 0, "VioWrtTTY through a 16-bit thunk");
    static unsigned short vmi[7] = { 14 };
    ULONG pv = flat_to_16(vmi);
    unsigned short a2[3] = { 0, (unsigned short)pv, (unsigned short)(pv >> 16) };
    check((far16_call(flat_to_16(VIO16GETMODE), a2, 6) & 0xFFFF) == 0 && vmi[2] >= 20 && vmi[3] >= 5, "VioGetMode");
    out("  screen "); num(vmi[2]); out("x"); num(vmi[3]); out("\n");
    static unsigned short cp;
    ULONG pc = flat_to_16(&cp);
    unsigned short a3[4] = { 0, (unsigned short)pc, (unsigned short)(pc >> 16), 0 };
    check((far16_call(flat_to_16(VIO16GETCP), a3, 8) & 0xFFFF) == 0 && cp == 437, "VioGetCp");
    static unsigned char ki[10] = { 0xAA, 0xAA, 0xAA };
    ULONG pk = flat_to_16(ki);
    unsigned short a4[3] = { 0, (unsigned short)pk, (unsigned short)(pk >> 16) };
    check((far16_call(flat_to_16(KBD16PEEK), a4, 6) & 0xFFFF) == 0 && ki[2] == 0, "KbdPeek (no key)");
    unsigned short ds, ss;
    __asm__ volatile("movw %%ds, %0\n\tmovw %%ss, %1" : "=r"(ds), "=r"(ss));
    check(ds == 0x53 && ss == 0x53, "back in the flat segments");

    /* queues */
    ULONG hq, qn, req[2], qcb;
    void *qb;
    unsigned char qp;
    check(DosCreateQueue(&hq, 2, "\\QUEUES\\OS2TEST") == 0, "DosCreateQueue (priority)");
    DosWriteQueue(hq, 1, 10, (void *)0x111, 1);
    DosWriteQueue(hq, 2, 20, (void *)0x222, 9);
    DosWriteQueue(hq, 3, 30, (void *)0x333, 1);
    check(DosQueryQueue(hq, &qn) == 0 && qn == 3, "DosWriteQueue, DosQueryQueue");
    check(DosReadQueue(hq, req, &qcb, &qb, 0, 0, &qp, 0) == 0 && req[1] == 2 && qcb == 20 && qp == 9, "DosReadQueue: highest priority first");
    check(DosReadQueue(hq, req, &qcb, &qb, 0, 0, &qp, 0) == 0 && req[1] == 1, "  then in order");
    DosReadQueue(hq, req, &qcb, &qb, 0, 0, &qp, 0);
    check(DosReadQueue(hq, req, &qcb, &qb, 0, 1, &qp, 0) == 342, "DosReadQueue: empty");
    DosCloseQueue(hq);

    /* a session: this program again, its end through a termination queue */
    char me_name[260];
    check(DosQueryModuleName(((ULONG *)pib)[2], sizeof me_name, me_name) == 0, "DosQueryModuleName");
    ULONG tq, sess = 0, spid = 0;
    check(DosCreateQueue(&tq, 0, "\\QUEUES\\OS2TERM") == 0, "a termination queue");
    ULONG sd[15] = { 0 };
    ((unsigned short *)sd)[0] = 60;                 /* Length */
    ((unsigned short *)sd)[1] = 1;                  /* Related */
    sd[3] = (ULONG)me_name;                         /* PgmName */
    sd[4] = (ULONG)"child";                         /* PgmInputs */
    sd[5] = (ULONG)"\\QUEUES\\OS2TERM";             /* TermQ */
    APIRET sr = DosStartSession(sd, &sess, &spid);
    check(sr == 0 && sess && spid, "DosStartSession");
    unsigned short *res = 0;
    if (sr == 0) {
        check(DosReadQueue(tq, req, &qcb, (void **)&res, 0, 0, &qp, 0) == 0 && qcb == 4 && res[0] == sess && res[1] == 5,
              "  its end in the queue (result 5)");
        DosFreeMem(res);
    }

    /* Presentation Manager isn't here: the import loads, the call fails */
    check(WinInitialize(0) == 0, "WinInitialize (PMWIN, not here): 0");

    out(fails ? "some tests failed\n" : "all passed\n");
    DosExit(1, (ULONG)fails);
    return 99;
}
