/* userland/os2x86/ne16.inc - 16-bit OS/2 1.x programs (NE executables;
 * included by os2x86.c after the 16-bit console calls).
 *
 * Each segment goes into a 64 KB tile of its own, so its selector is
 * the tile's (n*8+7) and 16:16 pointers work as on OS/2 2.x. The
 * relocations (offsets, selectors, far pointers; chained or additive;
 * internal - by segment, or by entry ordinal for movable ones - or
 * imported by ordinal or name) are applied; imports from the system's
 * DLLs become thunks, as a 32-bit program's 16-bit calls are. It starts
 * as OS/2 starts it: CS:IP, SS:SP, DS the automatic data segment, AX
 * the environment's selector, BX the command line's offset in it, CX
 * the automatic data's size.
 *
 * The 16-bit DOSCALLS (Pascal, far: DosOpen, DosRead, DosWrite,
 * DosFindFirst ...) mostly turn their arguments into the 32-bit calls'
 * - put on a stack of their own, with 32-bit results copied back into
 * the program's 16-bit ones. Not yet: the program's own 16-bit DLLs,
 * DosExecPgm, threads, semaphores, Presentation Manager's 16-bit calls. */

static uint32_t ne_scr;                       /* (the 32-bit calls' arguments and results) */
static uint32_t ne_env, ne_cmd;               /* (the environment segment: flat; the command line's offset) */
static uint32_t ne_ginfo, ne_linfo;           /* (the information segments) */
static uint16_t sel16(uint32_t flat) { return (uint16_t)(((flat >> 16) << 3) | 7); }
static uint32_t fp16(uint16_t sel, uint16_t off) { return ((uint32_t)(sel >> 3) << 16) + off; }

/* a 32-bit call, its arguments on a stack of their own */
static uint32_t call32(api_fn fn, int n, const uint32_t *a)
{
    if (!ne_scr && !(ne_scr = mem_alloc(UNIT))) return 8;
    uint32_t save = C.r[ESP], sp = ne_scr + 0x8000;
    p32(sp, 0);
    for (int i = 0; i < n; i++) p32(sp + 4 + 4 * (uint32_t)i, a[i]);
    C.r[ESP] = sp;
    uint32_t r = fn();
    C.r[ESP] = save;
    return r;
}
static uint32_t T(int i) { if (!ne_scr) ne_scr = mem_alloc(UNIT); return ne_scr + 0x100 + 0x200 * (uint32_t)i; }   /* (results' room) */
#define L(o) g32(a16 + (o))

static uint32_t d16_DosExit(void) { do_exit(W(0)); }
static uint32_t d16_DosGetInfoSeg(void)       /* (PSEL global, PSEL local) */
{
    time_t now = time(0);
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t g = ne_ginfo;
    p32(g, (uint32_t)now); p32(g + 4, 0);
    C.mem[g + 8] = (uint8_t)tm.tm_hour; C.mem[g + 9] = (uint8_t)tm.tm_min; C.mem[g + 10] = (uint8_t)tm.tm_sec; C.mem[g + 11] = 0;
    p16(g + 14, 310);                                        /* (cusecTimerInterval: tenths of ms) */
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
static uint32_t d16_DosReallocSeg(void) { return 0; }          /* (a tile is 64 KB already) */
static uint32_t d16_DosFreeSeg(void) { return mem_free(fp16(W(0), 0)) ? 5 : 0; }
static uint32_t d16_DosGetHugeShift(void) { p16(FP(0), 3); return 0; }   /* (the next tile: the selector + 8) */
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
/* FILEFINDBUF (16-bit: dates and times, cbFile, cbFileAlloc, USHORT attrFile, cchName, achName[13]) from FILEFINDBUF3's */
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
            uint32_t a[] = { spec, hp, attr, t, 300, cp, 1 };   /* FIL_STANDARD */
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
/* FILESTATUS (16-bit: dates and times, cbFile, cbFileAlloc, USHORT attrFile) from FILESTATUS3's */
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
    return 203;                                              /* ERROR_ENVVAR_NOT_FOUND */
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

/* ── loading ── */
static int ne_find_lib(const char *name)
{
    int l = builtin_lib(name);
    return l >= 0 ? l : stub_lib(name);
}
static void ne_main(const char *path, uint8_t *f, size_t n, uint32_t ne, int argc, char **argv) __attribute__((noreturn));
static void ne_main(const char *path, uint8_t *f, size_t n, uint32_t ne, int argc, char **argv)
{
    #define NE16(o) (uint32_t)(f[ne + (o)] | f[ne + (o) + 1] << 8)
    if (ne + 0x40 > n) die(193, "SYS0193: %s is damaged.", path);
    if (f[ne + 0x36] != 1 && f[ne + 0x36] != 0) die(193, "SYS0193: %s isn't an OS/2 program (NE, for another system).", path);
    if (NE16(0x0C) & 0x8000) die(193, "SYS0193: %s is a library (DLL), not a program.", path);
    uint32_t nseg = NE16(0x1C), st = ne + NE16(0x22), shift = NE16(0x32) ? NE16(0x32) : 9;
    uint32_t modref = ne + NE16(0x28), impn = ne + NE16(0x2A), nmod = NE16(0x1E), ent = ne + NE16(0x04);
    uint32_t base[256], len[256];
    if (nseg > 255 || st + 8 * nseg > n) die(193, "SYS0193: %s has too many segments.", path);
    /* the segments: a tile each */
    for (uint32_t i = 0; i < nseg; i++) {
        uint32_t e = st + 8 * i, sec = (uint32_t)(f[e] | f[e + 1] << 8), l = (uint32_t)(f[e + 2] | f[e + 3] << 8);
        if (!l && sec) l = 0x10000;
        if (!(base[i] = mem_alloc(UNIT))) die(8, "SYS0008: no memory for %s", path);
        len[i] = l;
        uint32_t off = sec << shift;
        if (sec && l && off + l <= n) memcpy(C.mem + base[i], f + off, l);
    }
    /* the entry table: a movable entry's segment and offset, by ordinal */
    #define ENTRY(ord, sp, op) do { *(sp) = 0; uint32_t p_ = ent, o_ = 1; \
        while (p_ < n && f[p_]) { uint32_t c_ = f[p_], t_ = f[p_ + 1]; p_ += 2; \
            if (!t_) { o_ += c_; continue; } \
            for (uint32_t k_ = 0; k_ < c_; k_++, o_++) { \
                if (o_ == (ord)) { if (t_ == 0xFF) { *(sp) = f[p_ + 3]; *(op) = (uint32_t)(f[p_ + 4] | f[p_ + 5] << 8); } \
                                   else { *(sp) = t_; *(op) = (uint32_t)(f[p_ + 1] | f[p_ + 2] << 8); } } \
                p_ += t_ == 0xFF ? 6 : 3; } } } while (0)
    /* the relocations */
    for (uint32_t i = 0; i < nseg; i++) {
        uint32_t e = st + 8 * i, sec = (uint32_t)(f[e] | f[e + 1] << 8), flags = (uint32_t)(f[e + 4] | f[e + 5] << 8);
        if (!(flags & 0x100) || !sec) continue;
        uint32_t r = (sec << shift) + len[i];
        if (r + 2 > n) continue;
        uint32_t cnt = (uint32_t)(f[r] | f[r + 1] << 8);
        r += 2;
        for (uint32_t k = 0; k < cnt && r + 8 <= n; k++, r += 8) {
            uint32_t src = f[r], fl = f[r + 1], at = (uint32_t)(f[r + 2] | f[r + 3] << 8);
            uint32_t t1 = (uint32_t)(f[r + 4] | f[r + 5] << 8), t2 = (uint32_t)(f[r + 6] | f[r + 7] << 8);
            uint32_t target = 0;
            switch (fl & 3) {
            case 0: {                                    /* internal: a segment and offset, or a movable entry */
                uint32_t s = t1 & 0xFF, o = t2;
                if (s == 0xFF) ENTRY(t2, &s, &o);
                if (!s || s > nseg) continue;
                target = base[s - 1] + o;
                break;
            }
            case 1: case 2: {                            /* imported: by ordinal or by name */
                char mod[64] = "", nm[64] = "";
                if (t1 >= 1 && t1 <= nmod) {
                    uint32_t mo = impn + (uint32_t)(f[modref + 2 * (t1 - 1)] | f[modref + 2 * (t1 - 1) + 1] << 8);
                    if (mo < n) snprintf(mod, sizeof mod, "%.*s", f[mo], (char *)f + mo + 1);
                }
                if ((fl & 3) == 2 && impn + t2 < n) snprintf(nm, sizeof nm, "%.*s", f[impn + t2], (char *)f + impn + t2 + 1);
                int lib = ne_find_lib(mod);
                if (lib < 0) die(193, "SYS0193: %s needs %s.DLL - a 16-bit DLL of its own, which can't be loaded yet.", path, mod);
                target = thunk_for(lib, (fl & 3) == 1 ? t2 : 0, (fl & 3) == 2 ? nm : 0);
                break;
            }
            default: continue;                           /* OS fixups (the floating point emulator's): not needed */
            }
            uint16_t tsel = sel16(target), toff = (uint16_t)(target & 0xFFFF);
            uint32_t sb = base[i];
            for (int guard = 0; guard < 0x10000; guard++) {   /* a chain (each place holds the next), or one (additive) */
                uint32_t a = sb + at;
                if (at >= 0x10000) break;
                uint32_t next = g16(a);
                switch (src) {
                case 0: C.mem[a] = (fl & 4) ? (uint8_t)(C.mem[a] + toff) : (uint8_t)toff; break;   /* LOBYTE */
                case 2: p16(a, tsel); break;                                                        /* SELECTOR */
                case 3: p16(a, (fl & 4) ? (uint16_t)(g16(a) + toff) : toff); p16(a + 2, tsel); break;   /* POINTER */
                case 5: p16(a, (fl & 4) ? (uint16_t)(g16(a) + toff) : toff); break;                 /* OFFSET */
                case 11: p32(a, (fl & 4) ? g32(a) + toff : toff); p16(a + 4, tsel); break;          /* POINTER48 */
                case 13: p32(a, (fl & 4) ? g32(a) + toff : toff); break;                            /* OFFSET32 */
                }
                if ((fl & 4) || next == 0xFFFF || src == 0 || src == 11 || src == 13) break;
                at = next;
            }
        }
    }
    /* the environment segment: the strings, an empty one, the program, the command line */
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
    /* a TIB for FS (the 32-bit calls that want one) */
    if (!(tib = mem_alloc(UNIT))) die(8, "SYS0008: no memory");
    p32(tib, 0xFFFFFFFF);
    C.tib = tib;
    process_ready = 1;
    /* the start: CS:IP, SS:SP (SP 0: the top of the automatic data), DS the automatic data */
    uint32_t ad = NE16(0x0E), ip = NE16(0x14), cs = NE16(0x16), sp = NE16(0x18), ss = NE16(0x1A);
    if (!cs || cs > nseg) die(193, "SYS0193: %s has no starting point.", path);
    if (!ss || ss > nseg) ss = ad;
    if (!sp) {
        uint32_t e = st + 8 * (ss - 1), min = (uint32_t)(f[e + 6] | f[e + 7] << 8);
        sp = (min ? min : 0x10000) + (ss == ad ? NE16(0x10) + NE16(0x12) : 0);
        if (sp >= 0x10000) sp = 0xFFFE;
    }
    cpu_load_seg(&C, 1, sel16(base[cs - 1]));
    cpu_load_seg(&C, 2, sel16(base[ss - 1]));
    cpu_load_seg(&C, 3, ad ? sel16(base[ad - 1]) : 0);
    cpu_load_seg(&C, 0, 0);
    C.eip = ip;
    C.r[ESP] = sp;
    C.r[EAX] = sel16(ne_env); C.r[EBX] = ne_cmd;
    uint32_t ade = ad ? st + 8 * (ad - 1) : 0, adsz = ad ? (uint32_t)(f[ade + 6] | f[ade + 7] << 8) : 0;
    C.r[ECX] = ad ? (adsz ? adsz : 0x10000) : 0;
    C.r[EDX] = C.r[ESI] = C.r[EDI] = C.r[EBP] = 0;
    run(0);
    do_exit(0);
    #undef NE16
    #undef ENTRY
}
/* is it one? (MZ, then NE at its offset): the NE header's place */
static uint32_t ne_header(const uint8_t *f, size_t n)
{
    if (n < 0x40 || f[0] != 'M' || f[1] != 'Z') return 0;
    uint32_t o = (uint32_t)(f[0x3C] | f[0x3D] << 8 | f[0x3E] << 16 | (uint32_t)f[0x3F] << 24);
    return o && o + 0x40 <= n && f[o] == 'N' && f[o + 1] == 'E' ? o : 0;
}
