/* userland/os2x86/ne16.inc - 16-bit OS/2 1.x programs (NE executables),
 * included by os2x86.c after the 16-bit console calls.
 *
 * Each segment gets its own 64 KB tile, so its selector is the tile's
 * (n*8+7) and 16:16 pointers work as on OS/2 2.x. Relocations are applied
 * (offsets, selectors, far pointers; chained or additive; internal by segment
 * or by entry ordinal for movable segments; imports by ordinal or name).
 * Imports from system DLLs become thunks, the same as a 32-bit program's
 * 16-bit calls. The program starts the way OS/2 starts it: CS:IP, SS:SP, DS
 * the automatic data segment, AX the environment selector, BX the command
 * line's offset in it, CX the automatic data size.
 *
 * Most 16-bit DOSCALLS (far Pascal: DosOpen, DosRead, DosWrite, DosFindFirst,
 * ...) convert their arguments and call the 32-bit versions on a stack of
 * their own, copying 32-bit results back into the program's 16-bit ones. */

static _Thread_local uint32_t ne_scr;         /* per-thread scratch for the 32-bit calls' arguments and results */
static uint32_t ne_env, ne_cmd;               /* environment segment (flat) and the command line's offset */
static uint32_t ne_ginfo, ne_linfo;           /* global and local information segments */
static uint16_t sel16(uint32_t flat) { return (uint16_t)(((flat >> 16) << 3) | 7); }
static uint32_t fp16(uint16_t sel, uint16_t off) { return ((uint32_t)(sel >> 3) << 16) + off; }

/* make a 32-bit call with its arguments on a separate stack. Calls nest (a
 * window procedure called back makes calls of its own), each level below the
 * last with its own room for results. The 16-bit caller's SP is kept for
 * callbacks, which run on the program's stack below it. */
static _Thread_local int c32_depth;
static _Thread_local uint32_t c32_sp, c32_prog_esp;
static uint32_t call32(api_fn fn, int n, const uint32_t *a)
{
    if (!ne_scr && !(ne_scr = mem_alloc(UNIT))) return 8;
    if (!c32_depth) c32_sp = ne_scr + 0xF000;
    uint32_t save = C.r[ESP], save_sp = c32_sp, save_prog = c32_prog_esp, sp = (c32_sp - 4 - 4 * (uint32_t)n - 0x40) & ~3u;
    c32_prog_esp = C.r[ESP];                                 /* 16-bit caller's SP, for callbacks */
    if (sp < ne_scr + 0x9000) return 8;                      /* nested too deep */
    p32(sp, 0);
    for (int i = 0; i < n; i++) p32(sp + 4 + 4 * (uint32_t)i, a[i]);
    c32_sp = sp;
    c32_depth++;
    C.r[ESP] = sp;
    uint32_t r = fn();
    C.r[ESP] = save;
    c32_depth--;
    c32_sp = save_sp;
    c32_prog_esp = save_prog;
    return r;
}
static uint32_t T(int i)                                     /* this level's room for results */
{
    if (!ne_scr) ne_scr = mem_alloc(UNIT);
    return ne_scr + 0x100 + 0x1000 * (uint32_t)(c32_depth & 7) + 0x200 * (uint32_t)i;
}
#define L(o) g32(a16 + (o))

static uint32_t d16_DosExit(void) { if (W(2) == EXIT_THREAD) thread_end(W(0)); do_exit(W(0)); }   /* (else EXIT_PROCESS) */
static uint32_t d16_DosGetInfoSeg(void)       /* (PSEL global, PSEL local) */
{
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t g = ne_ginfo;
    p32(g, (uint32_t)now); p32(g + 4, 0);
    C.mem[g + 8] = (uint8_t)tm.tm_hour; C.mem[g + 9] = (uint8_t)tm.tm_min; C.mem[g + 10] = (uint8_t)tm.tm_sec; C.mem[g + 11] = 0;
    p16(g + 14, 310);                                        /* cusecTimerInterval, in tenths of a ms */
    C.mem[g + 16] = (uint8_t)tm.tm_mday; C.mem[g + 17] = (uint8_t)(tm.tm_mon + 1); p16(g + 18, (uint16_t)(tm.tm_year + 1900));
    C.mem[g + 20] = (uint8_t)tm.tm_wday; C.mem[g + 21] = 20; C.mem[g + 22] = 40;   /* OS/2 20.40 */
    p16(ne_linfo, (uint16_t)getpid()); p16(ne_linfo + 2, (uint16_t)getppid()); p16(ne_linfo + 4, 0x200); p16(ne_linfo + 6, 1);
    p16(FP(4), sel16(g)); p16(FP(0), sel16(ne_linfo));
    return 0;
}
static uint32_t d16_DosSleep(void) { uint32_t a[] = { L(0) }; return call32(api_DosSleep, 1, a); }
static uint32_t d16_DosGetDateTime(void) { uint32_t a[] = { FP(0) }; return call32(api_DosGetDateTime, 1, a); }
static uint32_t d16_DosAllocSeg(void)         /* (USHORT size, PSEL psel, USHORT flags) */
{
    uint32_t m = mem_alloc(UNIT);
    if (!m) return 8;
    p16(FP(2), sel16(m));
    return 0;
}
static uint32_t d16_DosReallocSeg(void) { return 0; }          /* a tile is already 64 KB */
static uint32_t d16_DosFreeSeg(void) { return mem_free(fp16(W(0), 0)) ? 5 : 0; }
static uint32_t d16_DosGetHugeShift(void) { p16(FP(0), 3); return 0; }   /* next tile = selector + 8 */
static uint32_t d16_DosGetMachineMode(void) { C.mem[FP(0)] = 1; return 0; }   /* MODE_PROTECTED */
static uint32_t d16_DosBeep(void) { uint32_t a[] = { W(2), W(0) }; return call32(api_DosBeep, 2, a); }
static uint32_t d16_DosBufReset(void) { uint32_t a[] = { W(0) }; return call32(api_DosResetBuffer, 1, a); }
static uint32_t d16_DosChDir(void) { uint32_t a[] = { FP(4) }; return call32(api_DosSetCurrentDir, 1, a); }
static uint32_t d16_DosChgFilePtr(void)       /* (HFILE, LONG, USHORT method, PULONG pnew) */
{
    uint32_t a[] = { W(10), L(6), W(4), FP(0) };
    return call32(api_DosSetFilePtr, 4, a);
}
static uint32_t d16_DosClose(void) { uint32_t a[] = { W(0) }; return call32(api_DosClose, 1, a); }
static uint32_t d16_DosDelete(void) { uint32_t a[] = { FP(4), 0 }; return call32(api_DosDelete, 1, a); }
static uint32_t d16_DosDupHandle(void)        /* (HFILE old, PHFILE pnew) */
{
    uint32_t t = T(0), pn = FP(0);
    p32(t, g16(pn) == 0xFFFF ? 0xFFFFFFFF : g16(pn));
    uint32_t a[] = { W(4), t }, r = call32(api_DosDupHandle, 2, a);
    if (!r) p16(pn, (uint16_t)g32(t));
    return r;
}
/* fill a 16-bit FILEFINDBUF (dates/times, cbFile, cbFileAlloc, USHORT attrFile, cchName, achName[13]) from a FILEFINDBUF3 */
static void ne_ffb(uint32_t to, uint32_t from)
{
    memcpy(C.mem + to, C.mem + from + 4, 20);
    p16(to + 20, (uint16_t)g32(from + 24));
    uint8_t n = C.mem[from + 28];
    if (n > 12) n = 12;
    C.mem[to + 22] = n;
    memcpy(C.mem + to + 23, C.mem + from + 29, n);
    C.mem[to + 23 + n] = 0;
}
static uint32_t ne_find(uint32_t hdir, uint32_t buf, uint32_t cb, uint32_t pc, int first, uint32_t spec, uint32_t attr, uint32_t phdir)
{
    uint32_t want = g16(pc), got = 0, r = 0, t = T(0), hp = T(1), cp = T(2);
    if (!want) want = 1;
    while (got < want && (got + 1) * 36 <= cb) {
        p32(cp, 1);
        if (first && !got) {
            p32(hp, hdir == 0xFFFF ? 0xFFFFFFFF : hdir);
            uint32_t a[] = { spec, hp, attr, t, 300, cp, FIL_STANDARD };
            r = call32(api_DosFindFirst, 7, a);
            if (r) break;
            hdir = g32(hp);
            if (phdir) p16(phdir, (uint16_t)hdir);
        } else {
            uint32_t a[] = { hdir, t, 300, cp };
            r = call32(api_DosFindNext, 4, a);
            if (r) break;
        }
        ne_ffb(buf + got * 36, t);
        got++;
    }
    p16(pc, (uint16_t)got);
    return got ? 0 : r;
}
static uint32_t d16_DosFindFirst(void)        /* (spec, phdir, attr, buf, cb, pcSearch, reserved) */
{
    return ne_find(g16(FP(16)), FP(10), W(8), FP(4), 1, FP(20), W(14), FP(16));
}
static uint32_t d16_DosFindNext(void) { return ne_find(W(10), FP(6), W(4), FP(0), 0, 0, 0, 0); }
static uint32_t d16_DosFindClose(void) { uint32_t a[] = { W(0) }; return call32(api_DosFindClose, 1, a); }
static uint32_t d16_DosMkDir(void) { uint32_t a[] = { FP(4), 0 }; return call32(api_DosCreateDir, 2, a); }
static uint32_t d16_DosMove(void) { uint32_t a[] = { FP(8), FP(4) }; return call32(api_DosMove, 2, a); }
static uint32_t d16_DosNewSize(void) { uint32_t a[] = { W(4), L(0) }; return call32(api_DosSetFileSize, 2, a); }
static uint32_t d16_DosOpen(void)             /* (name, phf, pusAction, ULONG size, attr, flags, mode, ULONG reserved) */
{
    uint32_t th = T(0), ta = T(1);
    uint32_t a[] = { FP(22), th, ta, L(10), W(8), W(6), W(4), 0 }, r = call32(api_DosOpen, 8, a);
    if (!r) { p16(FP(18), (uint16_t)g32(th)); p16(FP(14), (uint16_t)g32(ta)); }
    return r;
}
static uint32_t d16_DosQCurDir(void)          /* (drive, buf, pcb) */
{
    uint32_t t = T(0);
    p32(t, g16(FP(0)));
    uint32_t a[] = { W(8), FP(4), t }, r = call32(api_DosQueryCurrentDir, 3, a);
    p16(FP(0), (uint16_t)g32(t));
    return r;
}
static uint32_t d16_DosQCurDisk(void)         /* (pdrive, pmap) */
{
    uint32_t t = T(0), a[] = { t, FP(0) }, r = call32(api_DosQueryCurrentDisk, 2, a);
    p16(FP(4), (uint16_t)g32(t));
    return r;
}
static uint32_t d16_DosQFHandState(void) { uint32_t t = T(0), a[] = { W(4), t }, r = call32(api_DosQueryFHState, 2, a); p16(FP(0), (uint16_t)g32(t)); return r; }
static uint32_t d16_DosSetFHandState(void) { uint32_t a[] = { W(2), W(0) }; return call32(api_DosSetFHState, 2, a); }
/* 16-bit FILESTATUS (dates/times, cbFile, cbFileAlloc, USHORT attrFile) from FILESTATUS3 */
static uint32_t d16_DosQFileInfo(void)        /* (hf, level, buf, cb) */
{
    uint32_t t = T(0), a[] = { W(8), 1, t, 24 }, r = call32(api_DosQueryFileInfo, 4, a);
    if (!r) { uint32_t b = FP(2); memcpy(C.mem + b, C.mem + t, 20); p16(b + 20, (uint16_t)g32(t + 20)); }
    return r;
}
static uint32_t d16_DosQFileMode(void)        /* (psz, pattr, reserved) */
{
    uint32_t t = T(0), a[] = { FP(8), 1, t, 24 }, r = call32(api_DosQueryPathInfo, 4, a);
    if (!r) p16(FP(4), (uint16_t)g32(t + 20));
    return r;
}
static uint32_t d16_DosSetFileMode(void) { return 0; }
static uint32_t d16_DosQFSInfo(void) { uint32_t a[] = { W(8), W(6), FP(2), W(0) }; return call32(api_DosQueryFSInfo, 4, a); }
static uint32_t d16_DosQHandType(void)        /* (hf, ptype, pattr) */
{
    uint32_t t = T(0), u = T(1), a[] = { W(8), t, u }, r = call32(api_DosQueryHType, 3, a);
    if (!r) { p16(FP(4), (uint16_t)g32(t)); p16(FP(0), (uint16_t)g32(u)); }
    return r;
}
static uint32_t d16_DosRmDir(void) { uint32_t a[] = { FP(4) }; return call32(api_DosDeleteDir, 1, a); }
static uint32_t d16_DosSelectDisk(void) { uint32_t a[] = { W(0) }; return call32(api_DosSetDefaultDisk, 1, a); }
static uint32_t d16_DosSetMaxFH(void) { return 0; }
static uint32_t d16_DosSetVec(void) { if (FP(0)) p32(FP(0), 0); return 0; }
static uint32_t d16_DosGetEnv(void) { p16(FP(4), sel16(ne_env)); p16(FP(0), (uint16_t)ne_cmd); return 0; }
static uint32_t d16_DosGetVersion(void) { p16(FP(0), 0x1428); return 0; }   /* 20.40 */
static uint32_t d16_DosGetPID(void) { uint32_t p = FP(0); p16(p, (uint16_t)getpid()); p16(p + 2, (uint16_t)me->tid); p16(p + 4, (uint16_t)getppid()); return 0; }
static uint32_t d16_DosError(void) { return 0; }
static uint32_t d16_DosGetCp(void)            /* (cb, arCP, pcCP) */
{
    if (W(8) >= 2) p16(FP(4), 850);
    p16(FP(0), W(8) >= 2 ? 2 : 0);
    return 0;
}
static uint32_t d16_DosRead(void)             /* (hf, buf, cb, pcb) */
{
    uint32_t t = T(0), a[] = { W(10), FP(6), W(4), t }, r = call32(api_DosRead, 4, a);
    p16(FP(0), (uint16_t)g32(t));
    return r;
}
static uint32_t d16_DosWrite(void)
{
    uint32_t t = T(0), a[] = { W(10), FP(6), W(4), t }, r = call32(api_DosWrite, 4, a);
    p16(FP(0), (uint16_t)g32(t));
    return r;
}
static uint32_t d16_DosSetSigHandler(void) { if (FP(8)) p32(FP(8), 0); if (FP(4)) p16(FP(4), 0); return 0; }
static uint32_t d16_DosHoldSignal(void) { return 0; }
static uint32_t d16_DosGetPrty(void) { p16(FP(2), 0x200); return 0; }
static uint32_t d16_DosSetPrty(void) { return 0; }
static uint32_t d16_DosDevConfig(void) { uint32_t a[] = { FP(4), W(2) }; return call32(api_DosDevConfig, 2, a); }
static uint32_t d16_DosScanEnv(void)          /* (name, PSZ far *ppValue) */
{
    char *n = gs(FP(4));
    if (!n) return 87;
    size_t l = strlen(n);
    for (uint32_t p = ne_env; C.mem[p]; p += (uint32_t)strlen((char *)C.mem + p) + 1)
        if (!strncasecmp((char *)C.mem + p, n, l) && C.mem[p + l] == '=') {
            p32(FP(0), (uint32_t)(p + l + 1 - ne_env) | (uint32_t)sel16(ne_env) << 16);
            return 0;
        }
    return ERROR_ENVVAR_NOT_FOUND;
}
/* semaphores: RAM ones (an HSEM is a far pointer to a doubleword in the program) and system ones (DosCreateSem,
 * DosOpenSem: handles kept here). Both hold the owner thread (high word) and a count (low word). A request waits
 * until it's clear or already ours; a clear releases one level and wakes waiters; a wait waits until it's clear. */
typedef struct { int used, refs; char name[128]; uint32_t v; } sysem_t;
static sysem_t sysems[64];
static uint32_t *sem_ptr(uint32_t h)
{
    if (h >> 16) { uint32_t s = fp16((uint16_t)(h >> 16), (uint16_t)h); return ok(s, 4) ? (uint32_t *)(C.mem + s) : 0; }
    h -= 1;
    return h < 64 && sysems[h].used ? &sysems[h].v : 0;
}
/* wait until free (or ours) for up to timeout ms (-1 forever, 0 not at all), then take it if asked */
static uint32_t sem_wait16(uint32_t h, int32_t timeout, int take)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (timeout > 0) { dl.tv_sec += timeout / 1000; dl.tv_nsec += (long)(timeout % 1000) * 1000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; } }
    uint32_t *v = sem_ptr(h);
    if (!v) return ERROR_SEM_NOT_FOUND;                      /* ERROR_INVALID_SEMAPHORE, really */
    for (;;) {
        v = sem_ptr(h);
        uint32_t cur = v ? *v : 0;
        if (!cur || (take && (cur >> 16) == me->tid)) break;
        if (!timeout) return ERROR_SEM_TIMEOUT;
        if (timeout < 0) pthread_cond_wait(&gcv, &gil);
        else if (pthread_cond_timedwait(&gcv, &gil, &dl) == ETIMEDOUT) return 121;
        if (me->kill) thread_end(0);
    }
    if (take && v) *v = (me->tid << 16) | ((*v & 0xFFFF) + 1);
    return 0;
}
static uint32_t d16_DosSemRequest(void) { return sem_wait16(L(4), (int32_t)L(0), 1); }   /* (HSEM, LONG timeout) */
static uint32_t d16_DosSemClear(void)                       /* (HSEM) */
{
    uint32_t *v = sem_ptr(L(0));
    if (!v) return 187;
    *v = (*v & 0xFFFF) > 1 ? *v - 1 : 0;
    pthread_cond_broadcast(&gcv);
    return 0;
}
static uint32_t d16_DosSemSet(void) { uint32_t *v = sem_ptr(L(0)); if (!v) return 187; if (!*v) *v = (me->tid << 16) | 1; return 0; }
static uint32_t d16_DosSemWait(void) { return sem_wait16(L(4), (int32_t)L(0), 0); }
static uint32_t d16_DosSemSetWait(void)                     /* (HSEM, LONG timeout): set it, then wait for a clear */
{
    uint32_t *v = sem_ptr(L(4));
    if (!v) return 187;
    *v = 0xFFFF0001u;                                        /* owner that isn't a thread: only a clear frees it */
    return sem_wait16(L(4), (int32_t)L(0), 0);
}
static uint32_t d16_DosCreateSem(void)                      /* (USHORT fExclusive, PHSYSSEM phsem, PSZ name) */
{
    char *n = gs(FP(0));
    if (!n) return 87;
    for (int i = 0; i < 64; i++) if (sysems[i].used && !strcasecmp(sysems[i].name, n)) return ERROR_ALREADY_EXISTS;
    for (int i = 0; i < 64; i++)
        if (!sysems[i].used) {
            memset(&sysems[i], 0, sizeof sysems[i]);
            sysems[i].used = 1; sysems[i].refs = 1;
            snprintf(sysems[i].name, sizeof sysems[i].name, "%s", n);
            p32(FP(4), (uint32_t)i + 1);
            return 0;
        }
    return ERROR_TOO_MANY_SEMAPHORES;
}
static uint32_t d16_DosOpenSem(void)                        /* (PHSEM phsem, PSZ name) */
{
    char *n = gs(FP(0));
    if (!n) return 87;
    for (int i = 0; i < 64; i++) if (sysems[i].used && !strcasecmp(sysems[i].name, n)) { sysems[i].refs++; p32(FP(4), (uint32_t)i + 1); return 0; }
    return ERROR_SEM_NOT_FOUND;
}
static uint32_t d16_DosCloseSem(void)                       /* (HSEM) */
{
    uint32_t h = L(0) - 1;
    if (L(0) >> 16 || h >= 64 || !sysems[h].used) return 187;
    if (--sysems[h].refs <= 0) sysems[h].used = 0;
    return 0;
}
/* DosMuxSemWait (PUSHORT pisemCleared, PVOID pmsxl: { USHORT count, { USHORT reserved, HSEM }[] }, LONG timeout): returns the first that's clear */
static uint32_t d16_DosMuxSemWait(void)
{
    uint32_t list = FP(4), n = g16(list);
    int32_t timeout = (int32_t)L(0);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (timeout > 0) { dl.tv_sec += timeout / 1000; dl.tv_nsec += (long)(timeout % 1000) * 1000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; } }
    for (;;) {
        for (uint32_t i = 0; i < n && i < 16; i++) {
            uint32_t *v = sem_ptr(g32(list + 2 + 6 * i + 2));
            if (v && !*v) { p16(FP(8), (uint16_t)i); return 0; }
        }
        if (!timeout) return 121;
        if (timeout < 0) pthread_cond_wait(&gcv, &gil);
        else if (pthread_cond_timedwait(&gcv, &gil, &dl) == ETIMEDOUT) return 121;
    }
}
/* threads: DosCreateThread (PFNTHREAD pfn (far), PTID ptid, PBYTE pstack (far top of stack)) uses the caller's DS;
 * a far return from the function ends the thread, like DosExit(EXIT_THREAD) */
static void *thr_main(void *arg);
static uint32_t d16_DosCreateThread(void)
{
    uint32_t pfn = L(8), stk = L(0);
    int i;
    for (i = 1; i < MAXTHR && thrs[i] && thrs[i]->state != 3; i++) { }
    if (i == MAXTHR) return ERROR_MAX_THRDS_REACHED;
    if (!(pfn >> 16) || !(stk >> 16)) return 87;
    thr_t *t = thrs[i] ? thrs[i] : calloc(1, sizeof *t);
    if (!t) return 8;
    memset(t, 0, sizeof *t);
    cpu_reset(&t->cpu);
    t->cpu.mem = C.mem; t->cpu.size = C.size; t->cpu.lo = C.lo;
    t->cpu.thunk_lo = C.thunk_lo; t->cpu.thunk_hi = C.thunk_hi;
    t->cpu.fs_base = t->cpu.tib = C.tib;
    t->tid = (uint32_t)i + 1;
    t->state = 1;
    cpu_load_seg(&t->cpu, 2, (uint16_t)(stk >> 16));
    cpu_load_seg(&t->cpu, 3, C.sreg[3]);
    cpu_load_seg(&t->cpu, 0, C.sreg[0]);
    uint32_t sp = (stk & 0xFFFF) - 4, back = thunk_addr(T_THREXIT);   /* far return to the thread's end */
    p16(t->cpu.ss_base + (sp & 0xFFFF), (uint16_t)(back & 0xFFFF)); p16(t->cpu.ss_base + ((sp + 2) & 0xFFFF), sel16(back));
    t->cpu.r[ESP] = sp & 0xFFFF;
    cpu_load_seg(&t->cpu, 1, (uint16_t)(pfn >> 16));
    t->cpu.eip = pfn & 0xFFFF;
    thrs[i] = t;
    nlive++;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t pt;
    int r = pthread_create(&pt, &at, thr_main, t);
    pthread_attr_destroy(&at);
    if (r) { nlive--; t->state = 3; return 8; }
    p16(FP(4), (uint16_t)t->tid);
    return 0;
}
static uint32_t d16_DosEnterCritSec(void) { return call32(api_DosEnterCritSec, 0, 0); }
static uint32_t d16_DosExitCritSec(void) { return call32(api_DosExitCritSec, 0, 0); }
static uint32_t d16_DosSuspendThread(void) { uint32_t a[] = { W(0) }; return call32(api_DosSuspendThread, 1, a); }
static uint32_t d16_DosResumeThread(void) { uint32_t a[] = { W(0) }; return call32(api_DosResumeThread, 1, a); }
static uint32_t d16_DosSizeSeg(void) { p32(FP(0), 0x10000); return 0; }                             /* (SEL, PULONG): always a tile */
static uint32_t d16_DosMemAvail(void) { p32(FP(0), 64u << 20); return 0; }
/* DosExecPgm (failname, cbFail, flags, args, env, RESULTCODES (two USHORTs), program) and DosCWait (scope, wait,
 * RESULTCODES, PPID, PID) map to the 32-bit calls; an x86 child runs through os2x86 again, 16- or 32-bit */
static uint32_t d16_DosExecPgm(void)
{
    uint32_t t = T(0), a[] = { FP(20), W(18), W(16), FP(12), FP(8), t, FP(0) }, r = call32(api_DosExecPgm, 7, a);
    if (!r) { p16(FP(4), (uint16_t)g32(t)); p16(FP(4) + 2, (uint16_t)g32(t + 4)); }
    return r;
}
static uint32_t d16_DosCWait(void)
{
    uint32_t t = T(0), pp = T(1), a[] = { W(12), W(10), t, pp, W(0) }, r = call32(api_DosWaitChild, 5, a);
    if (!r) { p16(FP(6), (uint16_t)g32(t)); p16(FP(6) + 2, (uint16_t)g32(t + 4)); p16(FP(2), (uint16_t)g32(pp)); }
    return r;
}
static uint32_t n16_DosGetDBCSEv(void) { if (W(8) >= 2) p16(FP(0), 0); return 0; }
static uint32_t n16_DosCaseMap(void)          /* (USHORT cb, PCOUNTRYCODE, PCHAR) */
{
    uint32_t s = FP(0);
    for (uint32_t i = 0; i < W(8); i++) C.mem[s + i] = (uint8_t)toupper(C.mem[s + i]);
    return 0;
}
static uint32_t n16_DosGetCtryInfo(void)      /* (USHORT cb, PCOUNTRYCODE, PCOUNTRYINFO, PUSHORT pcbActual): 38 bytes */
{
    uint32_t t = T(0), a[] = { 44, FP(10), t, T(1) }, r = call32(api_DosQueryCtryInfo, 4, a);
    uint32_t n = W(14) < 38 ? W(14) : 38;
    if (!r) { memcpy(C.mem + FP(4), C.mem + t, n); p16(FP(0), (uint16_t)n); }
    return r;
}

/* ── loading: the program and its 16-bit DLLs ── */
typedef struct {
    char name[16], path[PATH_MAX];
    uint8_t *f; size_t n; uint32_t ne, nseg;
    uint32_t base[256], len[256];
    int dll, inited;
} nemod_t;
static nemod_t *nemods[32];
static int nnemods;
#define MNE16(m, o) (uint32_t)((m)->f[(m)->ne + (o)] | (m)->f[(m)->ne + (o) + 1] << 8)
static uint16_t ne_hmod(nemod_t *m) { for (int i = 0; i < nnemods; i++) if (nemods[i] == m) return (uint16_t)(0x100 + i); return 0; }
static nemod_t *ne_mod_of(uint32_t h) { h -= 0x100; return h < (uint32_t)nnemods ? nemods[h] : 0; }
static int ne_find_lib(const char *name)
{
    int l = builtin_lib(name);
    return l >= 0 ? l : stub_lib(name);
}
/* flat address of an exported entry, by ordinal (entry table) */
static int ne_entry(nemod_t *m, uint32_t ord, uint32_t *flat)
{
    const uint8_t *f = m->f;
    uint32_t p = m->ne + MNE16(m, 0x04), o = 1;
    while (p + 1 < m->n && f[p]) {
        uint32_t c = f[p], t = f[p + 1];
        p += 2;
        if (!t) { o += c; continue; }
        for (uint32_t k = 0; k < c; k++, o++) {
            if (p + (t == 0xFF ? 6 : 3) > m->n) return 0;
            if (o == ord) {
                uint32_t sg = t == 0xFF ? f[p + 3] : t, off = t == 0xFF ? (uint32_t)(f[p + 4] | f[p + 5] << 8) : (uint32_t)(f[p + 1] | f[p + 2] << 8);
                if (!sg || sg > m->nseg) return 0;
                *flat = m->base[sg - 1] + off;
                return 1;
            }
            p += t == 0xFF ? 6 : 3;
        }
    }
    return 0;
}
/* ... by name: resident names, then non-resident ones (Pascal strings, each followed by its ordinal) */
static int ne_entry_name(nemod_t *m, const char *name, uint32_t *flat)
{
    uint32_t tabs[2] = { m->ne + MNE16(m, 0x26), (uint32_t)(m->f[m->ne + 0x2C] | m->f[m->ne + 0x2D] << 8 | m->f[m->ne + 0x2E] << 16 | (uint32_t)m->f[m->ne + 0x2F] << 24) };
    size_t l = strlen(name);
    for (int t = 0; t < 2; t++)
        for (uint32_t p = tabs[t], first = 1; p && p < m->n && m->f[p]; first = 0) {
            uint32_t n = m->f[p];
            if (p + 1 + n + 2 > m->n) break;
            uint32_t ord = (uint32_t)(m->f[p + 1 + n] | m->f[p + 2 + n] << 8);
            if (!first && n == l && !strncasecmp((char *)m->f + p + 1, name, l)) return ne_entry(m, ord, flat);
            p += 1 + n + 2;
        }
    return 0;
}
/* far call into 16-bit code (a DLL's init); returns AX */
static uint32_t ne_call16(uint32_t target, uint32_t ax, uint32_t cx, uint32_t di, uint16_t ds)
{
    uint32_t r[8], eip = C.eip, fl = C.eflags;
    uint16_t sr[4] = { C.sreg[0], C.sreg[1], C.sreg[2], C.sreg[3] };
    memcpy(r, C.r, sizeof r);
    uint32_t back = thunk_addr(T_CBRET);
    uint32_t sp = (C.r[ESP] - 4) & 0xFFFF;
    p16(C.ss_base + sp, (uint16_t)(back & 0xFFFF)); p16(C.ss_base + sp + 2, sel16(back));
    C.r[ESP] = (C.r[ESP] & 0xFFFF0000) | sp;
    C.r[EAX] = ax; C.r[ECX] = cx; C.r[EDI] = di; C.r[ESI] = 0;
    cpu_load_seg(&C, 3, ds);
    cpu_load_seg(&C, 0, 0);
    cpu_load_seg(&C, 1, sel16(target));
    C.eip = target & 0xFFFF;
    run(1);
    uint32_t ret = C.r[EAX] & 0xFFFF;
    memcpy(C.r, r, sizeof r);
    for (int i = 0; i < 4; i++) cpu_load_seg(&C, i, sr[i]);
    C.eip = eip; C.eflags = fl;
    return ret;
}
static uint32_t ne_header(const uint8_t *f, size_t n);
static nemod_t *ne_load(const char *path, int dll, char *err, size_t errn);
static nemod_t *ne_load_dll(const char *name, char *err, size_t errn)
{
    for (int i = 0; i < nnemods; i++) if (nemods[i]->dll && !strcasecmp(nemods[i]->name, name)) return nemods[i];
    char h[PATH_MAX];
    if (!dll_path(name, h, sizeof h)) { snprintf(err, errn, "SYS1804: The system cannot find the file %s.DLL.", name); return 0; }
    return ne_load(h, 1, err, errn);
}
static nemod_t *ne_load(const char *path, int dll, char *err, size_t errn)
{
    FILE *pf = fopen(path, "rb");
    if (!pf) { snprintf(err, errn, "SYS0002: The system cannot find the file %s.", path); return 0; }
    fseek(pf, 0, SEEK_END);
    long fl = ftell(pf);
    fseek(pf, 0, SEEK_SET);
    uint8_t *f = fl > 0 && fl < (32 << 20) ? malloc((size_t)fl) : 0;
    size_t n = f ? fread(f, 1, (size_t)fl, pf) : 0;
    fclose(pf);
    uint32_t ne = f ? ne_header(f, n) : 0;
    if (!ne) { free(f); snprintf(err, errn, "SYS0193: %s isn't a 16-bit OS/2 module (NE).", path); return 0; }
    if (f[ne + 0x36] != 1 && f[ne + 0x36] != 0) { free(f); snprintf(err, errn, "SYS0193: %s is for another system.", path); return 0; }
    if (nnemods >= 32) { free(f); snprintf(err, errn, "SYS0008: too many modules"); return 0; }
    nemod_t *m = calloc(1, sizeof *m);
    m->f = f; m->n = n; m->ne = ne; m->dll = dll;
    snprintf(m->path, sizeof m->path, "%s", path);
    uint32_t rn = ne + MNE16(m, 0x26);                       /* module name: the first resident name */
    if (rn < n) snprintf(m->name, sizeof m->name, "%.*s", f[rn] < 15 ? f[rn] : 15, (char *)f + rn + 1);
    nemods[nnemods++] = m;
    uint32_t nseg = MNE16(m, 0x1C), st = ne + MNE16(m, 0x22), shift = MNE16(m, 0x32) ? MNE16(m, 0x32) : 9;
    uint32_t modref = ne + MNE16(m, 0x28), impn = ne + MNE16(m, 0x2A), nmod = MNE16(m, 0x1E);
    if (nseg > 255 || st + 8 * nseg > n) { snprintf(err, errn, "SYS0193: %s is damaged.", path); return 0; }
    m->nseg = nseg;
    for (uint32_t i = 0; i < nseg; i++) {                    /* segments: one tile each */
        uint32_t e = st + 8 * i, sec = (uint32_t)(f[e] | f[e + 1] << 8), l = (uint32_t)(f[e + 2] | f[e + 3] << 8);
        if (!l && sec) l = 0x10000;
        if (!(m->base[i] = mem_alloc(UNIT))) { snprintf(err, errn, "SYS0008: no memory for %s", path); return 0; }
        m->len[i] = l;
        uint32_t off = sec << shift;
        if (sec && l && off + l <= n) memcpy(C.mem + m->base[i], f + off, l);
    }
    for (uint32_t i = 0; i < nseg; i++) {                    /* relocations */
        uint32_t e = st + 8 * i, sec = (uint32_t)(f[e] | f[e + 1] << 8), flags = (uint32_t)(f[e + 4] | f[e + 5] << 8);
        if (!(flags & 0x100) || !sec) continue;
        uint32_t r = (sec << shift) + m->len[i];
        if (r + 2 > n) continue;
        uint32_t cnt = (uint32_t)(f[r] | f[r + 1] << 8);
        r += 2;
        for (uint32_t k = 0; k < cnt && r + 8 <= n; k++, r += 8) {
            uint32_t src = f[r], fl8 = f[r + 1], at = (uint32_t)(f[r + 2] | f[r + 3] << 8);
            uint32_t t1 = (uint32_t)(f[r + 4] | f[r + 5] << 8), t2 = (uint32_t)(f[r + 6] | f[r + 7] << 8);
            uint32_t target = 0;
            switch (fl8 & 3) {
            case 0: {                                        /* internal: a segment and offset, or a movable entry */
                uint32_t sg = t1 & 0xFF;
                if (sg == 0xFF) { if (!ne_entry(m, t2, &target)) continue; }
                else { if (!sg || sg > nseg) continue; target = m->base[sg - 1] + t2; }
                break;
            }
            case 1: case 2: {                                /* imported by ordinal or name: from a system DLL or one of its own */
                char mod[64] = "", nm[64] = "";
                if (t1 >= 1 && t1 <= nmod) {
                    uint32_t mo = impn + (uint32_t)(f[modref + 2 * (t1 - 1)] | f[modref + 2 * (t1 - 1) + 1] << 8);
                    if (mo < n) snprintf(mod, sizeof mod, "%.*s", f[mo], (char *)f + mo + 1);
                }
                if ((fl8 & 3) == 2 && impn + t2 < n) snprintf(nm, sizeof nm, "%.*s", f[impn + t2], (char *)f + impn + t2 + 1);
                int lib = ne_find_lib(mod);
                if (lib >= 0) target = thunk_for(lib, (fl8 & 3) == 1 ? t2 : 0, (fl8 & 3) == 2 ? nm : 0);
                else {
                    nemod_t *d = ne_load_dll(mod, err, errn);
                    if (!d) return 0;
                    if (!((fl8 & 3) == 1 ? ne_entry(d, t2, &target) : ne_entry_name(d, nm, &target))) {
                        if ((fl8 & 3) == 1) snprintf(err, errn, "SYS0182: %s has no entry %u (%s needs it).", mod, t2, m->name);
                        else snprintf(err, errn, "SYS0182: %s has no entry %s (%s needs it).", mod, nm, m->name);
                        return 0;
                    }
                }
                break;
            }
            default: continue;                               /* OS fixups (floating point emulator): not needed */
            }
            uint16_t tsel = sel16(target), toff = (uint16_t)(target & 0xFFFF);
            uint32_t sb = m->base[i];
            for (int guard = 0; guard < 0x10000 && at < 0x10000; guard++) {   /* a chain (each place holds the next) or a single additive one */
                uint32_t a = sb + at, next = g16(a);
                switch (src) {
                case 0: C.mem[a] = (fl8 & 4) ? (uint8_t)(C.mem[a] + toff) : (uint8_t)toff; break;   /* LOBYTE */
                case 2: p16(a, tsel); break;                                                        /* SELECTOR */
                case 3: p16(a, (fl8 & 4) ? (uint16_t)(g16(a) + toff) : toff); p16(a + 2, tsel); break;   /* POINTER */
                case 5: p16(a, (fl8 & 4) ? (uint16_t)(g16(a) + toff) : toff); break;                 /* OFFSET */
                case 11: p32(a, (fl8 & 4) ? g32(a) + toff : toff); p16(a + 4, tsel); break;          /* POINTER48 */
                case 13: p32(a, (fl8 & 4) ? g32(a) + toff : toff); break;                            /* OFFSET32 */
                }
                if ((fl8 & 4) || next == 0xFFFF || src == 0 || src == 11 || src == 13) break;
                at = next;
            }
        }
    }
    return m;
}
/* DLL initialisation at its CS:IP: AX and DI get the module handle, DS its automatic data, CX the heap; returns 0 on failure */
static int ne_init(nemod_t *m, char *err, size_t errn)
{
    if (m->inited || !m->dll) return 1;
    m->inited = 1;
    uint32_t cs = MNE16(m, 0x16), ip = MNE16(m, 0x14), ad = MNE16(m, 0x0E);
    if (!cs || cs > m->nseg) return 1;
    uint32_t r = ne_call16(m->base[cs - 1] + ip, ne_hmod(m), MNE16(m, 0x10), ne_hmod(m), ad && ad <= m->nseg ? sel16(m->base[ad - 1]) : 0);
    if (!r) { snprintf(err, errn, "SYS1804: %s's initialisation failed.", m->name); return 0; }
    return 1;
}

static void ne_main(const char *path, uint8_t *f0, size_t n0, uint32_t ne0, int argc, char **argv) __attribute__((noreturn));
static void ne_main(const char *path, uint8_t *f0, size_t n0, uint32_t ne0, int argc, char **argv)
{
    (void)f0; (void)n0; (void)ne0;
    char err[512];
    nemod_t *m = ne_load(path, 0, err, sizeof err);
    if (!m) die(err[0] == 'S' && !strncmp(err, "SYS0008", 7) ? 8 : 193, "%s", err);
    if (MNE16(m, 0x0C) & 0x8000) die(193, "SYS0193: %s is a library (DLL), not a program.", path);
    /* environment segment: the strings, an empty one, the program name, the command line */
    if (!(ne_env = mem_alloc(UNIT)) || !(ne_ginfo = mem_alloc(UNIT)) || !(ne_linfo = mem_alloc(UNIT))) die(8, "SYS0008: no memory");
    uint32_t at = ne_env, end = ne_env + 0xF000;
    for (char **e = environ; *e && at + strlen(*e) + 1 < end; e++) guest_strcpy(&at, *e);
    if (at == ne_env) guest_strcpy(&at, "");
    guest_strcpy(&at, "");
    char os2path[PATH_MAX];
    host_to_os2(path, os2path, sizeof os2path);
    ne_cmd = at - ne_env;
    guest_strcpy(&at, os2path);
    char args[0x800] = "";
    size_t k = 0;
    for (int i = 2; i < argc && k < sizeof args - 4; i++) {
        const char *a = argv[i];
        int q = !*a || strpbrk(a, " \t") != 0;
        if (i > 2) args[k++] = ' ';
        if (q) args[k++] = '"';
        while (*a && k < sizeof args - 3) args[k++] = *a++;
        if (q) args[k++] = '"';
        args[k] = 0;
    }
    guest_strcpy(&at, args);
    guest_strcpy(&at, "");
    if (!(tib = mem_alloc(UNIT))) die(8, "SYS0008: no memory");   /* a TIB for FS, for 32-bit calls that want one */
    p32(tib, 0xFFFFFFFF);
    C.tib = tib;
    process_ready = 1;
    /* stack: SS:SP (SP 0 means the top of the automatic data); DS is the automatic data */
    uint32_t nseg = m->nseg, st = m->ne + MNE16(m, 0x22);
    uint32_t ad = MNE16(m, 0x0E), ip = MNE16(m, 0x14), cs = MNE16(m, 0x16), sp = MNE16(m, 0x18), ss = MNE16(m, 0x1A);
    if (!cs || cs > nseg) die(193, "SYS0193: %s has no starting point.", path);
    if (!ss || ss > nseg) ss = ad;
    if (!ss) die(193, "SYS0193: %s has no stack.", path);
    if (!sp) {
        uint32_t e = st + 8 * (ss - 1), min = (uint32_t)(m->f[e + 6] | m->f[e + 7] << 8);
        sp = (min ? min : 0x10000) + (ss == ad ? MNE16(m, 0x10) + MNE16(m, 0x12) : 0);
        if (sp >= 0x10000) sp = 0xFFFE;
    }
    cpu_load_seg(&C, 2, sel16(m->base[ss - 1]));
    C.r[ESP] = sp;
    /* run its DLLs' initialisation, first loaded first, on its stack */
    for (int i = 0; i < nnemods; i++) if (!ne_init(nemods[i], err, sizeof err)) die(193, "%s", err);
    /* start: CS:IP, DS automatic data, AX environment, BX command line, CX automatic data size */
    cpu_load_seg(&C, 1, sel16(m->base[cs - 1]));
    cpu_load_seg(&C, 3, ad ? sel16(m->base[ad - 1]) : 0);
    cpu_load_seg(&C, 0, 0);
    C.eip = ip;
    C.r[EAX] = sel16(ne_env); C.r[EBX] = ne_cmd;
    uint32_t ade = ad ? st + 8 * (ad - 1) : 0, adsz = ad ? (uint32_t)(m->f[ade + 6] | m->f[ade + 7] << 8) : 0;
    C.r[ECX] = ad ? (adsz ? adsz : 0x10000) : 0;
    C.r[EDX] = C.r[ESI] = C.r[EDI] = C.r[EBP] = 0;
    run(0);
    do_exit(0);
}
/* resources: the last segments (NE offset 0x34 says how many), with the resource table (0x24) giving type and id */
static int ne_find_resource(uint32_t hmod, uint32_t type, uint32_t id, uint32_t *addr, uint32_t *size)
{
    nemod_t *m = hmod ? ne_mod_of(hmod) : (nnemods && !nemods[0]->dll ? nemods[0] : 0);
    if (!m) return -1;
    uint32_t cres = MNE16(m, 0x34), rt = m->ne + MNE16(m, 0x24);
    if (cres > m->nseg) return 0;
    for (uint32_t k = 0; k < cres && rt + 4 * k + 4 <= m->n; k++) {
        uint32_t t = (uint32_t)(m->f[rt + 4 * k] | m->f[rt + 4 * k + 1] << 8), i = (uint32_t)(m->f[rt + 4 * k + 2] | m->f[rt + 4 * k + 3] << 8);
        if (t != type || i != id) continue;
        uint32_t sg = m->nseg - cres + k;
        *addr = m->base[sg]; *size = m->len[sg];
        return 1;
    }
    return 0;
}
static uint32_t d16_DosGetResource(void)                   /* (HMODULE, USHORT type, USHORT id, PSEL): the resource's segment */
{
    uint32_t a, n;
    if (find_resource(W(8), W(6), W(4), &a, &n) != 1) return 87;
    p16(FP(0), sel16(a));
    return 0;
}

/* DosLoadModule (failname, cb, modname, phmod), DosGetProcAddr (hmod, procname or 0:ordinal, ppfn), DosFreeModule,
 * DosGetModHandle (modname, phmod), DosGetModName (hmod, cb, buf): for 16-bit DLLs, and system ones via their thunks */
static uint32_t d16_DosLoadModule(void)
{
    char *nm = gs(FP(4)), err[512];
    if (!nm) return 87;
    char b[64];
    const char *base = nm;
    for (const char *q = nm; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') base = q + 1;
    snprintf(b, sizeof b, "%s", base);
    char *dot = strrchr(b, '.');
    if (dot && !strcasecmp(dot, ".DLL")) *dot = 0;
    int lib = ne_find_lib(b);
    if (lib >= 0) { p16(FP(0), (uint16_t)(0x8000 + lib)); return 0; }   /* system DLL: its imports are ours */
    nemod_t *d = ne_load_dll(nm, err, sizeof err);
    if (d && !ne_init(d, err, sizeof err)) d = 0;
    if (!d) {
        if (W(8) && FP(10)) snprintf((char *)C.mem + FP(10), W(8), "%s", b);
        return ERROR_FILE_NOT_FOUND;
    }
    p16(FP(0), ne_hmod(d));
    return 0;
}
static uint32_t d16_DosGetProcAddr(void)
{
    uint32_t h = W(8), pn = g32(a16 + 4), target = 0;
    int byord = (pn >> 16) == 0;                             /* 0:ordinal */
    char *nm = byord ? 0 : gs(FP(4));
    if (h >= 0x8000) {
        if (byord) target = thunk_for((int)(h - 0x8000), pn & 0xFFFF, 0);
        else if (nm) target = thunk_for((int)(h - 0x8000), 0, nm);
    } else {
        nemod_t *m = ne_mod_of(h);
        if (!m || !(byord ? ne_entry(m, pn & 0xFFFF, &target) : nm && ne_entry_name(m, nm, &target))) return ERROR_INVALID_ORDINAL;
    }
    if (!target) return 182;
    p32(FP(0), (uint32_t)(target & 0xFFFF) | (uint32_t)sel16(target) << 16);
    return 0;
}
static uint32_t d16_DosFreeModule(void) { return 0; }      /* modules stay loaded */
static uint32_t d16_DosGetModHandle(void)
{
    char *nm = gs(FP(4));
    if (!nm) return 87;
    for (int i = 0; i < nnemods; i++) if (!strcasecmp(nemods[i]->name, nm)) { p16(FP(0), ne_hmod(nemods[i])); return 0; }
    int lib = ne_find_lib(nm);
    if (lib >= 0) { p16(FP(0), (uint16_t)(0x8000 + lib)); return 0; }
    return ERROR_INVALID_NAME;
}
static uint32_t d16_DosGetModName(void)                    /* (hmod, cb, buf): its file, by its OS/2 name */
{
    nemod_t *m = ne_mod_of(W(6));
    if (!m) return ERROR_INVALID_HANDLE;
    char o[PATH_MAX];
    host_to_os2(m->path, o, sizeof o);
    if ((uint32_t)strlen(o) + 1 > W(4)) return ERROR_BAD_LENGTH;
    snprintf((char *)C.mem + FP(0), W(4), "%s", o);
    return 0;
}

/* is it an NE (MZ, then NE at its offset)? returns the NE header's offset */
static uint32_t ne_header(const uint8_t *f, size_t n)
{
    if (n < 0x40 || f[0] != 'M' || f[1] != 'Z') return 0;
    uint32_t o = (uint32_t)(f[0x3C] | f[0x3D] << 8 | f[0x3E] << 16 | (uint32_t)f[0x3F] << 24);
    return o && o + 0x40 <= n && f[o] == 'N' && f[o + 1] == 'E' ? o : 0;
}
