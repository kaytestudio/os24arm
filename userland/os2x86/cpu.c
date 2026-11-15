/* userland/os2x86/cpu.c - the i386 interpreter (see cpu.h).
 *
 * One instruction at a time: prefixes, the opcode, ModRM/SIB, then the
 * work, flags computed as it goes. A bad memory access longjmps out of
 * the instruction to cpu_run, which reports it (EIP back at the
 * instruction's start, as a trap would leave it). 32-bit addressing
 * and 16-bit (0x67, and 16-bit code: the thunks to the 16-bit calls). */
#include "cpu.h"
#include <math.h>
#include <setjmp.h>
#include <string.h>

static _Thread_local jmp_buf *trap_jb;    /* (each OS/2 thread: a host thread) */
static uint8_t parity[256];

static void trap(cpu_t *c, int why) { (void)c; longjmp(*trap_jb, why); }

int mem_ok(const cpu_t *c, uint32_t a, uint32_t n)
{
    return a >= c->lo && a < c->size && n <= c->size - a;
}

static void fault(cpu_t *c, uint32_t a) { c->fault_addr = a; trap(c, CPU_FAULT); }

uint8_t rd8(cpu_t *c, uint32_t a) { if (a >= c->size || a < c->lo) fault(c, a); return c->mem[a]; }
uint16_t rd16(cpu_t *c, uint32_t a) { if (!mem_ok(c, a, 2)) fault(c, a); uint16_t v; memcpy(&v, c->mem + a, 2); return v; }
uint32_t rd32(cpu_t *c, uint32_t a) { if (!mem_ok(c, a, 4)) fault(c, a); uint32_t v; memcpy(&v, c->mem + a, 4); return v; }
static uint64_t rd64(cpu_t *c, uint32_t a) { if (!mem_ok(c, a, 8)) fault(c, a); uint64_t v; memcpy(&v, c->mem + a, 8); return v; }
void wr8(cpu_t *c, uint32_t a, uint8_t v) { if (a >= c->size || a < c->lo) fault(c, a); c->mem[a] = v; }
void wr16(cpu_t *c, uint32_t a, uint16_t v) { if (!mem_ok(c, a, 2)) fault(c, a); memcpy(c->mem + a, &v, 2); }
void wr32(cpu_t *c, uint32_t a, uint32_t v) { if (!mem_ok(c, a, 4)) fault(c, a); memcpy(c->mem + a, &v, 4); }
static void wr64(cpu_t *c, uint32_t a, uint64_t v) { if (!mem_ok(c, a, 8)) fault(c, a); memcpy(c->mem + a, &v, 8); }

/* the stack: SS:ESP, or SS:SP in a 16-bit stack segment */
static uint32_t sp_get(cpu_t *c) { return c->stack16 ? c->r[ESP] & 0xFFFF : c->r[ESP]; }
static void sp_set(cpu_t *c, uint32_t v) { c->r[ESP] = c->stack16 ? (c->r[ESP] & 0xFFFF0000) | (v & 0xFFFF) : v; }
static uint32_t sp_at(cpu_t *c, uint32_t sp) { return c->ss_base + (c->stack16 ? sp & 0xFFFF : sp); }
void push32(cpu_t *c, uint32_t v) { uint32_t sp = sp_get(c) - 4; wr32(c, sp_at(c, sp), v); sp_set(c, sp); }
uint32_t pop32(cpu_t *c) { uint32_t sp = sp_get(c), v = rd32(c, sp_at(c, sp)); sp_set(c, sp + 4); return v; }
static void push16(cpu_t *c, uint16_t v) { uint32_t sp = sp_get(c) - 2; wr16(c, sp_at(c, sp), v); sp_set(c, sp); }
static uint16_t pop16(cpu_t *c) { uint32_t sp = sp_get(c); uint16_t v = rd16(c, sp_at(c, sp)); sp_set(c, sp + 2); return v; }

void cpu_load_seg(cpu_t *c, int seg, uint16_t sel)
{
    uint32_t base = (sel & 4) ? (uint32_t)(sel >> 3) << 16 : 0;
    c->sreg[seg] = sel;
    switch (seg) {
    case 0: c->es_base = base; break;
    case 1: c->cs_base = base; c->code16 = (sel & 4) != 0; break;
    case 2: c->ss_base = base; c->stack16 = (sel & 4) != 0; break;
    case 3: c->ds_base = base; break;
    case 4: c->fs_base = (sel & 4) ? base : c->tib; break;
    case 5: c->gs_base = base; break;
    }
}

void cpu_reset(cpu_t *c)
{
    static int parity_done;
    for (int i = 0; i < 256 && !parity_done; i++) {
        int b = 0;
        for (int k = 0; k < 8; k++) b ^= (i >> k) & 1;
        parity[i] = b ? 0 : F_PF;
    }
    parity_done = 1;
    memset(c->r, 0, sizeof c->r);
    c->es_base = c->cs_base = c->ss_base = c->ds_base = c->gs_base = 0;
    c->code16 = c->stack16 = 0;
    c->eflags = 0x202;
    c->fcw = 0x37F; c->fsw = 0; c->top = 0;
    memset(c->empty, 1, sizeof c->empty);
    c->sreg[0] = c->sreg[2] = c->sreg[3] = 0x53;   /* OS/2's flat data selector */
    c->sreg[1] = 0x5B;                            /* ... and code */
    c->sreg[4] = 0x150b; c->sreg[5] = 0;
}

/* ── the instruction being decoded ───────────────────────────────────── */
typedef struct {
    int os, as;             /* operand and address size: 16 or 32 */
    int ovr;                /* a segment override (0-5), or -1 */
    int rep;                /* 0, 0xF3 (REP/REPE), 0xF2 (REPNE) */
    uint8_t modrm;
    int mod, reg, rm;
    uint32_t off;           /* mod != 3: the offset, */
    uint32_t ea;            /* ... and the linear address */
} ins_t;

static uint32_t segbase(cpu_t *c, int s)
{
    switch (s) {
    case 0: return c->es_base;
    case 1: return c->cs_base;
    case 2: return c->ss_base;
    case 4: return c->fs_base;
    case 5: return c->gs_base;
    default: return c->ds_base;
    }
}
/* the selectors a program has: flat code and data, the TIB's, the tiles */
static int sel_valid(cpu_t *c, uint16_t sel)
{
    if (sel & 4) return ((uint32_t)(sel >> 3) << 16) < c->size;
    return (sel | 3) == 0x5B || (sel | 3) == 0x53 || (sel | 3) == 0x150B;
}
static uint32_t dseg(cpu_t *c, ins_t *in) { return segbase(c, in->ovr >= 0 ? in->ovr : 3); }

static uint8_t fetch8(cpu_t *c) { return rd8(c, c->cs_base + c->eip++); }
static uint16_t fetch16(cpu_t *c) { uint16_t v = rd16(c, c->cs_base + c->eip); c->eip += 2; return v; }
static uint32_t fetch32(cpu_t *c) { uint32_t v = rd32(c, c->cs_base + c->eip); c->eip += 4; return v; }
static uint32_t fetchv(cpu_t *c, int os) { return os == 16 ? fetch16(c) : fetch32(c); }

static void modrm(cpu_t *c, ins_t *in)
{
    uint8_t m = fetch8(c);
    in->modrm = m;
    in->mod = m >> 6; in->reg = (m >> 3) & 7; in->rm = m & 7;
    if (in->mod == 3) return;
    uint32_t a;
    int ss = 0;                             /* (E)BP or ESP based: SS's */
    if (in->as == 16) {
        static const int8_t b1[8] = { EBX, EBX, EBP, EBP, ESI, EDI, EBP, EBX }, b2[8] = { ESI, EDI, ESI, EDI, -1, -1, -1, -1 };
        if (in->mod == 0 && in->rm == 6) a = fetch16(c);
        else {
            a = c->r[b1[in->rm]] & 0xFFFF;
            if (b2[in->rm] >= 0) a += c->r[b2[in->rm]] & 0xFFFF;
            ss = b1[in->rm] == EBP;
        }
        if (in->mod == 1) a += (uint32_t)(int32_t)(int8_t)fetch8(c);
        else if (in->mod == 2) a += fetch16(c);
        a &= 0xFFFF;
    } else {
        if (in->rm == 4) {
            uint8_t sib = fetch8(c);
            int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
            a = idx == 4 ? 0 : c->r[idx] << sc;
            if (base == 5 && in->mod == 0) a += fetch32(c);
            else { a += c->r[base]; ss = base == ESP || base == EBP; }
        } else if (in->rm == 5 && in->mod == 0) {
            a = fetch32(c);
        } else {
            a = c->r[in->rm];
            ss = in->rm == EBP;
        }
        if (in->mod == 1) a += (uint32_t)(int32_t)(int8_t)fetch8(c);
        else if (in->mod == 2) a += fetch32(c);
    }
    in->off = a;
    in->ea = a + segbase(c, in->ovr >= 0 ? in->ovr : ss ? 2 : 3);
}

/* registers by size: 8-bit 0-3 AL CL DL BL, 4-7 AH CH DH BH */
static uint32_t getr(cpu_t *c, int r, int bits)
{
    if (bits == 32) return c->r[r];
    if (bits == 16) return c->r[r] & 0xFFFF;
    return r < 4 ? c->r[r] & 0xFF : (c->r[r - 4] >> 8) & 0xFF;
}
static void setr(cpu_t *c, int r, int bits, uint32_t v)
{
    if (bits == 32) c->r[r] = v;
    else if (bits == 16) c->r[r] = (c->r[r] & 0xFFFF0000) | (v & 0xFFFF);
    else if (r < 4) c->r[r] = (c->r[r] & 0xFFFFFF00) | (v & 0xFF);
    else c->r[r - 4] = (c->r[r - 4] & 0xFFFF00FF) | ((v & 0xFF) << 8);
}
static uint32_t rdm(cpu_t *c, uint32_t a, int bits) { return bits == 32 ? rd32(c, a) : bits == 16 ? rd16(c, a) : rd8(c, a); }
static void wrm(cpu_t *c, uint32_t a, int bits, uint32_t v) { if (bits == 32) wr32(c, a, v); else if (bits == 16) wr16(c, a, (uint16_t)v); else wr8(c, a, (uint8_t)v); }
static uint32_t getE(cpu_t *c, ins_t *in, int bits) { return in->mod == 3 ? getr(c, in->rm, bits) : rdm(c, in->ea, bits); }
static void setE(cpu_t *c, ins_t *in, int bits, uint32_t v) { if (in->mod == 3) setr(c, in->rm, bits, v); else wrm(c, in->ea, bits, v); }

static uint32_t mask_of(int bits) { return bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1; }
static uint32_t sign_of(int bits) { return 1u << (bits - 1); }
static uint32_t sext(uint32_t v, int bits) { return bits == 32 ? v : bits == 16 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v; }

#define FLAGS_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

static void szp(cpu_t *c, uint32_t r, int bits)
{
    r &= mask_of(bits);
    c->eflags &= ~(F_ZF | F_SF | F_PF);
    if (!r) c->eflags |= F_ZF;
    if (r & sign_of(bits)) c->eflags |= F_SF;
    c->eflags |= parity[r & 0xFF];
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static uint32_t alu(cpu_t *c, int op, uint32_t a, uint32_t b, int bits)
{
    uint32_t m = mask_of(bits), s = sign_of(bits), r = 0, cin;
    a &= m; b &= m;
    uint32_t f = c->eflags & ~FLAGS_ARITH;
    switch (op) {
    case 0: case 2:
        cin = op == 2 ? (c->eflags & F_CF) : 0;
        r = (a + b + cin) & m;
        if ((uint64_t)a + b + cin > m) f |= F_CF;
        if ((a ^ r) & (b ^ r) & s) f |= F_OF;
        if ((a ^ b ^ r) & 0x10) f |= F_AF;
        break;
    case 3: case 5: case 7:
        cin = op == 3 ? (c->eflags & F_CF) : 0;
        r = (a - b - cin) & m;
        if ((uint64_t)b + cin > a) f |= F_CF;
        if ((a ^ b) & (a ^ r) & s) f |= F_OF;
        if ((a ^ b ^ r) & 0x10) f |= F_AF;
        break;
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    case 6: r = a ^ b; break;
    }
    c->eflags = f;
    szp(c, r, bits);
    return r;
}

static uint32_t incdec(cpu_t *c, uint32_t a, int dec, int bits)
{
    uint32_t cf = c->eflags & F_CF;
    uint32_t r = alu(c, dec ? 5 : 0, a, 1, bits);
    c->eflags = (c->eflags & ~F_CF) | cf;
    return r;
}

/* grp2: 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 6 SAL 7 SAR */
static uint32_t shift(cpu_t *c, int op, uint32_t v, unsigned n, int bits)
{
    uint32_t m = mask_of(bits), s = sign_of(bits);
    n &= 31;
    if (!n) return v & m;
    v &= m;
    uint32_t r = v, cf = c->eflags & F_CF;
    switch (op) {
    case 0: {
        unsigned k = n % bits;
        r = k ? ((v << k) | (v >> (bits - k))) & m : v;
        cf = r & 1;
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | ((((r & s) != 0) ^ cf) ? F_OF : 0);
        return r;
    }
    case 1: {
        unsigned k = n % bits;
        r = k ? ((v >> k) | (v << (bits - k))) & m : v;
        cf = (r & s) != 0;
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | ((((r & s) != 0) ^ ((r & (s >> 1)) != 0)) ? F_OF : 0);
        return r;
    }
    case 2: {
        unsigned k = n % (bits + 1);
        for (unsigned i = 0; i < k; i++) {
            uint32_t out = (r & s) != 0;
            r = ((r << 1) | (cf ? 1 : 0)) & m;
            cf = out;
        }
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | ((((r & s) != 0) ^ (cf != 0)) ? F_OF : 0);
        return r;
    }
    case 3: {
        unsigned k = n % (bits + 1);
        uint32_t of = (((r & s) != 0) ^ (cf != 0));
        for (unsigned i = 0; i < k; i++) {
            uint32_t out = r & 1;
            r = (r >> 1) | (cf ? s : 0);
            cf = out;
        }
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    }
    case 4: case 6:
        r = n < 32 ? (v << n) & m : 0;
        cf = n <= (unsigned)bits ? (v >> (bits - n)) & 1 : 0;
        c->eflags = (c->eflags & ~(F_CF | F_OF | F_AF)) | (cf ? F_CF : 0) | ((((r & s) != 0) ^ (cf != 0)) ? F_OF : 0);
        szp(c, r, bits);
        return r;
    case 5:
        r = v >> n;
        cf = (v >> (n - 1)) & 1;
        c->eflags = (c->eflags & ~(F_CF | F_OF | F_AF)) | (cf ? F_CF : 0) | ((v & s) ? F_OF : 0);
        szp(c, r, bits);
        return r;
    case 7: {
        int32_t sv = (int32_t)sext(v, bits);
        unsigned k = n > 31 ? 31 : n;
        r = (uint32_t)(sv >> k) & m;
        cf = (uint32_t)(sv >> (k - 1)) & 1;
        c->eflags = (c->eflags & ~(F_CF | F_OF | F_AF)) | (cf ? F_CF : 0);
        szp(c, r, bits);
        return r;
    }
    }
    return r;
}

static int cond(cpu_t *c, int cc)
{
    uint32_t f = c->eflags;
    int r;
    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}

/* ── x87 ─────────────────────────────────────────────────────────────── */
#define ST(i) c->st[(c->top + (i)) & 7]
#define C0 0x0100
#define C1 0x0200
#define C2 0x0400
#define C3 0x4000

static void fpush(cpu_t *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; c->empty[c->top] = 0; }
static void fpop(cpu_t *c) { c->empty[c->top] = 1; c->top = (c->top + 1) & 7; }
static uint16_t fsw(cpu_t *c) { return (uint16_t)((c->fsw & ~0x3800) | (c->top << 11)); }

static double from80(cpu_t *c, uint32_t a)
{
    uint64_t mant = rd64(c, a);
    uint16_t se = rd16(c, a + 8);
    int e = se & 0x7FFF;
    double v;
    if (e == 0 && mant == 0) v = 0;
    else if (e == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, e - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static void to80(cpu_t *c, uint32_t a, double v)
{
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double f = frexp(fabs(v), &e);            /* f in [0.5, 1) */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    wr64(c, a, mant);
    wr16(c, a + 8, se);
}

static double fround(cpu_t *c, double v)
{
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static void fcompare(cpu_t *c, double a, double b)
{
    c->fsw &= ~(C0 | C2 | C3);
    if (isnan(a) || isnan(b)) c->fsw |= C0 | C2 | C3;
    else if (a < b) c->fsw |= C0;
    else if (a == b) c->fsw |= C3;
}
static void fcomi(cpu_t *c, double a, double b)
{
    c->eflags &= ~(F_ZF | F_PF | F_CF | F_OF | F_SF | F_AF);
    if (isnan(a) || isnan(b)) c->eflags |= F_ZF | F_PF | F_CF;
    else if (a < b) c->eflags |= F_CF;
    else if (a == b) c->eflags |= F_ZF;
}

/* the 8 arithmetic ops on st0 and v: FADD FMUL FCOM FCOMP FSUB FSUBR FDIV FDIVR */
static void farith(cpu_t *c, int op, double v)
{
    double *d = &ST(0);
    switch (op) {
    case 0: *d += v; break;
    case 1: *d *= v; break;
    case 2: fcompare(c, *d, v); break;
    case 3: fcompare(c, *d, v); fpop(c); break;
    case 4: *d -= v; break;
    case 5: *d = v - *d; break;
    case 6: *d /= v; break;
    case 7: *d = v / *d; break;
    }
}

static int64_t fint(cpu_t *c, double v, int bits, int *bad)
{
    double r = fround(c, v);
    double lim = ldexp(1.0, bits - 1);
    if (isnan(r) || r >= lim || r < -lim) { *bad = 1; return 0; }      /* (the callers store the "integer indefinite") */
    *bad = 0;
    return (int64_t)r;
}

static void fpu(cpu_t *c, ins_t *in, uint8_t op)
{
    int o = op & 7;
    if (in->mod != 3) {
        uint32_t a = in->ea;
        int r = in->reg, bad;
        int64_t iv;
        switch (o) {
        case 0: { float f; uint32_t u = rd32(c, a); memcpy(&f, &u, 4); farith(c, r, f); return; }
        case 4: { double d; uint64_t u = rd64(c, a); memcpy(&d, &u, 8); farith(c, r, d); return; }
        case 2: farith(c, r, (double)(int32_t)rd32(c, a)); return;
        case 6: farith(c, r, (double)(int16_t)rd16(c, a)); return;
        case 1:
            switch (r) {
            case 0: { float f; uint32_t u = rd32(c, a); memcpy(&f, &u, 4); fpush(c, f); return; }
            case 2: case 3: { float f = (float)ST(0); uint32_t u; memcpy(&u, &f, 4); wr32(c, a, u); if (r == 3) fpop(c); return; }
            case 4: c->fcw = rd16(c, a); c->fsw = rd16(c, a + 4); return;          /* FLDENV (the start of it) */
            case 5: c->fcw = rd16(c, a); return;
            case 6: wr32(c, a, c->fcw); wr32(c, a + 4, fsw(c)); wr32(c, a + 8, 0xFFFF);
                    for (int k = 12; k < 28; k += 4) wr32(c, a + k, 0); return;     /* FNSTENV */
            case 7: wr16(c, a, c->fcw); return;
            }
            break;
        case 3:
            switch (r) {
            case 0: fpush(c, (double)(int32_t)rd32(c, a)); return;
            case 1: { double t = trunc(ST(0)); wr32(c, a, (t >= 2147483648.0 || t < -2147483648.0 || isnan(t)) ? 0x80000000u : (uint32_t)(int32_t)t); fpop(c); return; }
            case 2: case 3: iv = fint(c, ST(0), 32, &bad); wr32(c, a, bad ? 0x80000000u : (uint32_t)iv); if (r == 3) fpop(c); return;
            case 5: fpush(c, from80(c, a)); return;
            case 7: to80(c, a, ST(0)); fpop(c); return;
            }
            break;
        case 5:
            switch (r) {
            case 0: { double d; uint64_t u = rd64(c, a); memcpy(&d, &u, 8); fpush(c, d); return; }
            case 1: { double t = trunc(ST(0)); wr64(c, a, (t >= 9.2233720368547758e18 || t < -9.2233720368547758e18 || isnan(t)) ? 0x8000000000000000ull : (uint64_t)(int64_t)t); fpop(c); return; }
            case 2: case 3: { double d = ST(0); uint64_t u; memcpy(&u, &d, 8); wr64(c, a, u); if (r == 3) fpop(c); return; }
            case 4: {                                       /* FRSTOR: environment, then 8 x 80-bit */
                c->fcw = rd16(c, a); c->fsw = rd16(c, a + 4); c->top = (c->fsw >> 11) & 7;
                for (int k = 0; k < 8; k++) { c->st[(c->top + k) & 7] = from80(c, a + 28 + 10 * k); c->empty[(c->top + k) & 7] = 0; }
                return;
            }
            case 6: {                                       /* FNSAVE, then FNINIT */
                wr32(c, a, c->fcw); wr32(c, a + 4, fsw(c)); wr32(c, a + 8, 0);
                for (int k = 12; k < 28; k += 4) wr32(c, a + k, 0);
                for (int k = 0; k < 8; k++) to80(c, a + 28 + 10 * k, ST(k));
                c->fcw = 0x37F; c->fsw = 0; c->top = 0; memset(c->empty, 1, 8);
                return;
            }
            case 7: wr16(c, a, fsw(c)); return;
            }
            break;
        case 7:
            switch (r) {
            case 0: fpush(c, (double)(int16_t)rd16(c, a)); return;
            case 1: { double t = trunc(ST(0)); wr16(c, a, (t >= 32768.0 || t < -32768.0 || isnan(t)) ? 0x8000 : (uint16_t)(int16_t)t); fpop(c); return; }
            case 2: case 3: iv = fint(c, ST(0), 16, &bad); wr16(c, a, bad ? 0x8000 : (uint16_t)iv); if (r == 3) fpop(c); return;
            case 5: fpush(c, (double)(int64_t)rd64(c, a)); return;
            case 7: iv = fint(c, ST(0), 64, &bad); wr64(c, a, bad ? 0x8000000000000000ull : (uint64_t)iv); fpop(c); return;
            case 4: {                                       /* FBLD: packed BCD */
                double v = 0;
                for (int k = 8; k >= 0; k--) { uint8_t b = rd8(c, a + k); v = v * 100 + (b >> 4) * 10 + (b & 15); }
                fpush(c, (rd8(c, a + 9) & 0x80) ? -v : v);
                return;
            }
            case 6: {                                       /* FBSTP */
                double v = fround(c, ST(0));
                int neg = v < 0;
                uint64_t u = (uint64_t)fabs(v);
                for (int k = 0; k < 9; k++) { uint8_t lo = u % 10; u /= 10; uint8_t hi = u % 10; u /= 10; wr8(c, a + k, (uint8_t)(hi << 4 | lo)); }
                wr8(c, a + 9, neg ? 0x80 : 0);
                fpop(c);
                return;
            }
            }
            break;
        }
        trap(c, CPU_ILLEGAL);
    }
    int i = in->rm, r = in->reg;
    double t;
    switch (o) {
    case 0: farith(c, r, ST(i)); return;
    case 1:
        switch (r) {
        case 0: t = ST(i); fpush(c, t); return;
        case 1: t = ST(0); ST(0) = ST(i); ST(i) = t; return;
        case 2: return;                                    /* FNOP */
        case 4:
            switch (i) {
            case 0: ST(0) = -ST(0); return;
            case 1: ST(0) = fabs(ST(0)); return;
            case 4: fcompare(c, ST(0), 0.0); return;
            case 5: {                                      /* FXAM */
                double v = ST(0);
                c->fsw &= ~(C0 | C1 | C2 | C3);
                if (signbit(v)) c->fsw |= C1;
                if (c->empty[c->top]) c->fsw |= C3 | C0;
                else if (isnan(v)) c->fsw |= C0;
                else if (isinf(v)) c->fsw |= C2 | C0;
                else if (v == 0) c->fsw |= C3;
                else c->fsw |= C2;
                return;
            }
            }
            break;
        case 5: {
            static const double k[] = { 1.0, 3.321928094887362347870, 1.442695040888963407360,
                                        3.141592653589793238463, 0.301029995663981195214, 0.693147180559945309417, 0.0 };
            if (i < 7) { fpush(c, k[i]); return; }
            break;
        }
        case 6:
            switch (i) {
            case 0: ST(0) = exp2(ST(0)) - 1; return;
            case 1: ST(1) = ST(1) * log2(ST(0)); fpop(c); return;
            case 2: ST(0) = tan(ST(0)); fpush(c, 1.0); c->fsw &= ~C2; return;
            case 3: ST(1) = atan2(ST(1), ST(0)); fpop(c); return;
            case 4: { int e; double m = frexp(ST(0), &e); ST(0) = (double)(e - 1); fpush(c, m * 2); return; }
            case 5: ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~(C0 | C1 | C2 | C3); return;
            case 6: c->top = (c->top - 1) & 7; return;
            case 7: c->top = (c->top + 1) & 7; return;
            }
            break;
        case 7:
            switch (i) {
            case 0: {                                      /* FPREM: quotient bits in C0 C3 C1 */
                double q = trunc(ST(0) / ST(1));
                ST(0) = fmod(ST(0), ST(1));
                long long qi = (long long)fabs(q);
                c->fsw &= ~(C0 | C1 | C2 | C3);
                if (qi & 1) c->fsw |= C1;
                if (qi & 2) c->fsw |= C3;
                if (qi & 4) c->fsw |= C0;
                return;
            }
            case 1: ST(1) = ST(1) * log2(ST(0) + 1); fpop(c); return;
            case 2: ST(0) = sqrt(ST(0)); return;
            case 3: t = ST(0); ST(0) = sin(t); fpush(c, cos(t)); c->fsw &= ~C2; return;
            case 4: ST(0) = fround(c, ST(0)); return;
            case 5: ST(0) = ldexp(ST(0), (int)trunc(ST(1))); return;
            case 6: ST(0) = sin(ST(0)); c->fsw &= ~C2; return;
            case 7: ST(0) = cos(ST(0)); c->fsw &= ~C2; return;
            }
            break;
        }
        break;
    case 2:
        if (r == 5 && i == 1) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); return; }   /* FUCOMPP */
        if (r < 4) {                                         /* FCMOVB/E/BE/U */
            static const int cc[] = { 2, 4, 6, 10 };
            if (cond(c, cc[r])) ST(0) = ST(i);
            return;
        }
        break;
    case 3:
        if (r == 4 && i == 2) { c->fsw &= 0x7F00; return; }                         /* FNCLEX */
        if (r == 4 && i == 3) { c->fcw = 0x37F; c->fsw = 0; c->top = 0; memset(c->empty, 1, 8); return; }   /* FNINIT */
        if (r == 4 && (i == 0 || i == 1 || i == 4)) return;                         /* FENI FDISI FSETPM */
        if (r == 5 || r == 6) { fcomi(c, ST(0), ST(i)); return; }                   /* FUCOMI FCOMI */
        if (r < 4) {                                                                /* FCMOVNB/NE/NBE/NU */
            static const int cc[] = { 3, 5, 7, 11 };
            if (cond(c, cc[r])) ST(0) = ST(i);
            return;
        }
        break;
    case 4: {
        double *d = &ST(i);
        switch (r) {
        case 0: *d += ST(0); return;
        case 1: *d *= ST(0); return;
        case 2: fcompare(c, ST(0), *d); return;
        case 3: fcompare(c, ST(0), *d); fpop(c); return;
        case 4: *d = ST(0) - *d; return;
        case 5: *d = *d - ST(0); return;
        case 6: *d = ST(0) / *d; return;
        case 7: *d = *d / ST(0); return;
        }
        break;
    }
    case 5:
        switch (r) {
        case 0: c->empty[(c->top + i) & 7] = 1; return;                             /* FFREE */
        case 2: ST(i) = ST(0); return;
        case 3: ST(i) = ST(0); fpop(c); return;
        case 4: fcompare(c, ST(0), ST(i)); return;
        case 5: fcompare(c, ST(0), ST(i)); fpop(c); return;
        }
        break;
    case 6: {
        if (r == 3 && i == 1) { fcompare(c, ST(0), ST(1)); fpop(c); fpop(c); return; }   /* FCOMPP */
        double *d = &ST(i);
        switch (r) {
        case 0: *d += ST(0); break;
        case 1: *d *= ST(0); break;
        case 4: *d = ST(0) - *d; break;
        case 5: *d = *d - ST(0); break;
        case 6: *d = ST(0) / *d; break;
        case 7: *d = *d / ST(0); break;
        default: trap(c, CPU_ILLEGAL);
        }
        fpop(c);
        return;
    }
    case 7:
        if (r == 4 && i == 0) { setr(c, EAX, 16, fsw(c)); return; }                 /* FNSTSW AX */
        if (r == 5 || r == 6) { fcomi(c, ST(0), ST(i)); fpop(c); return; }         /* FUCOMIP FCOMIP */
        if (r == 0) { c->empty[(c->top + i) & 7] = 1; fpop(c); return; }           /* FFREEP */
        break;
    }
    trap(c, CPU_ILLEGAL);
}

/* ── string instructions ─────────────────────────────────────────────── */
static void string_op(cpu_t *c, ins_t *in, uint8_t op)
{
    int bits = (op & 1) ? in->os : 8, n = bits / 8;
    int32_t d = (c->eflags & F_DF) ? -n : n;
    uint32_t sb = dseg(c, in), db = c->es_base;   /* (DS:ESI may be overridden; ES:EDI not) */
    uint32_t am = in->as == 16 ? 0xFFFF : 0xFFFFFFFF;
#define SI (c->r[ESI] & am)
#define DI (c->r[EDI] & am)
#define STEP(x) (c->r[x] = (c->r[x] & ~am) | ((c->r[x] + (uint32_t)d) & am))
    for (;;) {
        if (in->rep && !(c->r[ECX] & am)) return;
        switch (op) {
        case 0xA4: case 0xA5: wrm(c, db + DI, bits, rdm(c, sb + SI, bits)); STEP(ESI); STEP(EDI); break;
        case 0xA6: case 0xA7: alu(c, 7, rdm(c, sb + SI, bits), rdm(c, db + DI, bits), bits); STEP(ESI); STEP(EDI); break;
        case 0xAA: case 0xAB: wrm(c, db + DI, bits, getr(c, EAX, bits)); STEP(EDI); break;
        case 0xAC: case 0xAD: setr(c, EAX, bits, rdm(c, sb + SI, bits)); STEP(ESI); break;
        case 0xAE: case 0xAF: alu(c, 7, getr(c, EAX, bits), rdm(c, db + DI, bits), bits); STEP(EDI); break;
        }
#undef SI
#undef DI
#undef STEP
        if (!in->rep) return;
        c->r[ECX] = (c->r[ECX] & ~am) | ((c->r[ECX] - 1) & am);
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            int z = (c->eflags & F_ZF) != 0;
            if (in->rep == 0xF3 && !z) return;
            if (in->rep == 0xF2 && z) return;
        }
    }
}

/* ── MUL / DIV ───────────────────────────────────────────────────────── */
static void muldiv(cpu_t *c, int op, uint32_t v, int bits)
{
    uint32_t m = mask_of(bits);
    switch (op) {
    case 4: case 5: {                              /* MUL, IMUL (one operand) */
        int hi_set;
        if (bits == 8) {
            uint16_t r = op == 4 ? (uint16_t)((c->r[EAX] & 0xFF) * v) : (uint16_t)((int8_t)c->r[EAX] * (int8_t)v);
            setr(c, EAX, 16, r);
            hi_set = op == 4 ? (r >> 8) != 0 : (int16_t)r != (int8_t)r;
        } else if (bits == 16) {
            uint32_t r = op == 4 ? (c->r[EAX] & 0xFFFF) * v : (uint32_t)((int16_t)c->r[EAX] * (int16_t)v);
            setr(c, EAX, 16, r); setr(c, EDX, 16, r >> 16);
            hi_set = op == 4 ? (r >> 16) != 0 : (int32_t)r != (int16_t)r;
        } else {
            uint64_t r = op == 4 ? (uint64_t)c->r[EAX] * v : (uint64_t)((int64_t)(int32_t)c->r[EAX] * (int32_t)v);
            c->r[EAX] = (uint32_t)r; c->r[EDX] = (uint32_t)(r >> 32);
            hi_set = op == 4 ? (r >> 32) != 0 : (int64_t)r != (int32_t)r;
        }
        c->eflags = (c->eflags & ~(F_CF | F_OF)) | (hi_set ? F_CF | F_OF : 0);
        return;
    }
    case 6: {                                      /* DIV */
        if (!(v & m)) trap(c, CPU_DIVIDE);
        if (bits == 8) {
            uint16_t a = c->r[EAX] & 0xFFFF;
            uint16_t q = a / v;
            if (q > 0xFF) trap(c, CPU_DIVIDE);
            setr(c, EAX, 8, q); setr(c, 4, 8, a % v);      /* AL, AH */
        } else if (bits == 16) {
            uint32_t a = (c->r[EDX] & 0xFFFF) << 16 | (c->r[EAX] & 0xFFFF);
            uint32_t q = a / v;
            if (q > 0xFFFF) trap(c, CPU_DIVIDE);
            setr(c, EAX, 16, q); setr(c, EDX, 16, a % v);
        } else {
            uint64_t a = (uint64_t)c->r[EDX] << 32 | c->r[EAX];
            uint64_t q = a / v;
            if (q > 0xFFFFFFFFull) trap(c, CPU_DIVIDE);
            c->r[EAX] = (uint32_t)q; c->r[EDX] = (uint32_t)(a % v);
        }
        return;
    }
    case 7: {                                      /* IDIV */
        if (!(v & m)) trap(c, CPU_DIVIDE);
        if (bits == 8) {
            int16_t a = (int16_t)(c->r[EAX] & 0xFFFF);
            int32_t q = a / (int8_t)v;
            if (q > 127 || q < -128) trap(c, CPU_DIVIDE);
            setr(c, EAX, 8, (uint32_t)q); setr(c, 4, 8, (uint32_t)(a % (int8_t)v));
        } else if (bits == 16) {
            int32_t a = (int32_t)((c->r[EDX] & 0xFFFF) << 16 | (c->r[EAX] & 0xFFFF));
            int32_t q = a / (int16_t)v;
            if (q > 32767 || q < -32768) trap(c, CPU_DIVIDE);
            setr(c, EAX, 16, (uint32_t)q); setr(c, EDX, 16, (uint32_t)(a % (int16_t)v));
        } else {
            int64_t a = (int64_t)((uint64_t)c->r[EDX] << 32 | c->r[EAX]);
            if ((int32_t)v == -1 && a == INT64_MIN) trap(c, CPU_DIVIDE);
            int64_t q = a / (int32_t)v;
            if (q > INT32_MAX || q < INT32_MIN) trap(c, CPU_DIVIDE);
            c->r[EAX] = (uint32_t)q; c->r[EDX] = (uint32_t)(a % (int32_t)v);
        }
        return;
    }
    }
}

/* IMUL r, r/m(, imm): the low half, CF=OF if it didn't fit */
static uint32_t imul2(cpu_t *c, uint32_t a, uint32_t b, int bits)
{
    int64_t r = (int64_t)(int32_t)sext(a, bits) * (int32_t)sext(b, bits);
    uint32_t lo = (uint32_t)r & mask_of(bits);
    int over = (int64_t)(int32_t)sext(lo, bits) != r;
    c->eflags = (c->eflags & ~(F_CF | F_OF)) | (over ? F_CF | F_OF : 0);
    return lo;
}

/* SHLD / SHRD */
static uint32_t shxd(cpu_t *c, int left, uint32_t dst, uint32_t src, unsigned n, int bits)
{
    n &= 31;
    if (!n) return dst;
    uint32_t m = mask_of(bits), r, cf;
    if (bits == 32) {
        if (left) { r = (dst << n) | (src >> (32 - n)); cf = (dst >> (32 - n)) & 1; }
        else { r = (dst >> n) | (src << (32 - n)); cf = (dst >> (n - 1)) & 1; }
    } else {
        uint32_t both = left ? (dst << 16 | (src & 0xFFFF)) : ((src & 0xFFFF) << 16 | (dst & 0xFFFF));
        if (left) { uint32_t t = both << n; r = t >> 16; cf = (both >> (32 - n)) & 1; }
        else { r = (both >> n) & 0xFFFF; cf = (both >> (n - 1)) & 1; }
    }
    r &= m;
    c->eflags = (c->eflags & ~(F_CF | F_OF)) | (cf ? F_CF : 0) | (((r ^ dst) & sign_of(bits)) ? F_OF : 0);
    szp(c, r, bits);
    return r;
}

/* BT BTS BTR BTC: op 4 5 6 7 (as grp8); the bit's address from a register
 * index may reach past the operand (memory forms) */
static void bitop(cpu_t *c, ins_t *in, int op, uint32_t idx, int from_reg)
{
    int bits = in->os;
    uint32_t v, a = 0;
    if (in->mod == 3) { idx &= bits - 1; v = getr(c, in->rm, bits); }
    else {
        a = in->ea;
        if (from_reg) { int32_t off = (int32_t)sext(idx, bits); a += (uint32_t)((off >> (bits == 32 ? 5 : 4)) * (bits / 8)); }
        idx &= bits - 1;
        v = rdm(c, a, bits);
    }
    uint32_t b = 1u << idx;
    c->eflags = (c->eflags & ~F_CF) | ((v & b) ? F_CF : 0);
    if (op == 4) return;
    v = op == 5 ? v | b : op == 6 ? v & ~b : v ^ b;
    if (in->mod == 3) setr(c, in->rm, bits, v); else wrm(c, a, bits, v);
}

/* ── one instruction ─────────────────────────────────────────────────── */
static void step(cpu_t *c)
{
    int dsz = c->code16 ? 16 : 32;
    ins_t in = { dsz, dsz, -1, 0, 0, 0, 0, 0, 0, 0 };
    if (c->code16) c->eip &= 0xFFFF;
    c->op_eip = c->eip;
    uint8_t op;
    for (;;) {                                   /* prefixes */
        op = fetch8(c);
        if (op == 0x66) in.os = 48 - dsz;
        else if (op == 0x67) in.as = 48 - dsz;
        else if (op == 0x26) in.ovr = 0;
        else if (op == 0x2E) in.ovr = 1;
        else if (op == 0x36) in.ovr = 2;
        else if (op == 0x3E) in.ovr = 3;
        else if (op == 0x64) in.ovr = 4;
        else if (op == 0x65) in.ovr = 5;
        else if (op == 0xF0) { }
        else if (op == 0xF2 || op == 0xF3) in.rep = op;
        else break;
    }
    int os = in.os;
    uint32_t v, w;

    if (op < 0x40 && (op & 7) < 6) {            /* ADD OR ADC SBB AND SUB XOR CMP */
        int aop = op >> 3, bits = (op & 1) ? os : 8;
        switch (op & 7) {
        case 0: case 1:
            modrm(c, &in);
            v = alu(c, aop, getE(c, &in, bits), getr(c, in.reg, bits), bits);
            if (aop != 7) setE(c, &in, bits, v);
            return;
        case 2: case 3:
            modrm(c, &in);
            v = alu(c, aop, getr(c, in.reg, bits), getE(c, &in, bits), bits);
            if (aop != 7) setr(c, in.reg, bits, v);
            return;
        case 4: v = alu(c, aop, getr(c, EAX, 8), fetch8(c), 8); if (aop != 7) setr(c, EAX, 8, v); return;
        case 5: v = alu(c, aop, getr(c, EAX, os), fetchv(c, os), os); if (aop != 7) setr(c, EAX, os, v); return;
        }
    }
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E:  /* PUSH ES/CS/SS/DS */
        if (os == 16) push16(c, c->sreg[op >> 3]); else push32(c, c->sreg[op >> 3]);
        return;
    case 0x07: case 0x17: case 0x1F:             /* POP ES/SS/DS */
        cpu_load_seg(c, op >> 3, os == 16 ? pop16(c) : (uint16_t)pop32(c));
        return;
    case 0x0F: break;                            /* (below) */
    case 0x27: case 0x2F: {                      /* DAA DAS */
        uint8_t al = c->r[EAX] & 0xFF, oal = al;
        int cf = (c->eflags & F_CF) != 0, ncf = 0;
        if ((al & 15) > 9 || (c->eflags & F_AF)) {
            al = op == 0x27 ? al + 6 : al - 6;
            ncf = cf || (op == 0x27 ? oal > 0xF9 : oal < 6);
            c->eflags |= F_AF;
        } else c->eflags &= ~F_AF;
        if (oal > 0x99 || cf) { al = op == 0x27 ? al + 0x60 : al - 0x60; ncf = 1; }
        setr(c, EAX, 8, al);
        szp(c, al, 8);
        c->eflags = (c->eflags & ~F_CF) | (ncf ? F_CF : 0);
        return;
    }
    case 0x37: case 0x3F: {                      /* AAA AAS */
        if ((c->r[EAX] & 15) > 9 || (c->eflags & F_AF)) {
            uint16_t ax = c->r[EAX] & 0xFFFF;
            ax = op == 0x37 ? (uint16_t)(ax + 0x106) : (uint16_t)(ax - 6 - 0x100);
            setr(c, EAX, 16, ax);
            c->eflags |= F_AF | F_CF;
        } else c->eflags &= ~(F_AF | F_CF);
        setr(c, EAX, 8, c->r[EAX] & 15);
        return;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        setr(c, op & 7, os, incdec(c, getr(c, op & 7, os), 0, os)); return;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        setr(c, op & 7, os, incdec(c, getr(c, op & 7, os), 1, os)); return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (os == 16) push16(c, (uint16_t)c->r[op & 7]); else push32(c, c->r[op & 7]);
        return;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        if (os == 16) setr(c, op & 7, 16, pop16(c)); else { v = pop32(c); c->r[op & 7] = v; }
        return;
    case 0x60: {                                 /* PUSHA */
        uint32_t sp = c->r[ESP];
        for (int i = 0; i < 8; i++) { uint32_t x = i == ESP ? sp : c->r[i]; if (os == 16) push16(c, (uint16_t)x); else push32(c, x); }
        return;
    }
    case 0x61:                                   /* POPA */
        for (int i = 7; i >= 0; i--) { uint32_t x = os == 16 ? pop16(c) : pop32(c); if (i != ESP) setr(c, i, os, x); }
        return;
    case 0x68: v = fetchv(c, os); if (os == 16) push16(c, (uint16_t)v); else push32(c, v); return;
    case 0x6A: v = (uint32_t)(int32_t)(int8_t)fetch8(c); if (os == 16) push16(c, (uint16_t)v); else push32(c, v); return;
    case 0x69: modrm(c, &in); v = getE(c, &in, os); w = fetchv(c, os); setr(c, in.reg, os, imul2(c, v, w, os)); return;
    case 0x6B: modrm(c, &in); v = getE(c, &in, os); w = (uint32_t)(int32_t)(int8_t)fetch8(c); setr(c, in.reg, os, imul2(c, v, w, os)); return;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8(c);
        if (cond(c, op & 15)) c->eip += (uint32_t)(int32_t)d;
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int bits = (op & 1) ? os : 8;
        modrm(c, &in);
        uint32_t a = getE(c, &in, bits);
        uint32_t b = op == 0x81 ? fetchv(c, os) : op == 0x83 ? (uint32_t)(int32_t)(int8_t)fetch8(c) : fetch8(c);
        v = alu(c, in.reg, a, b, bits);
        if (in.reg != 7) setE(c, &in, bits, v);
        return;
    }
    case 0x84: case 0x85: { int bits = (op & 1) ? os : 8; modrm(c, &in); alu(c, 4, getE(c, &in, bits), getr(c, in.reg, bits), bits); return; }
    case 0x86: case 0x87: {
        int bits = (op & 1) ? os : 8;
        modrm(c, &in);
        v = getE(c, &in, bits); w = getr(c, in.reg, bits);
        setE(c, &in, bits, w); setr(c, in.reg, bits, v);
        return;
    }
    case 0x88: modrm(c, &in); setE(c, &in, 8, getr(c, in.reg, 8)); return;
    case 0x89: modrm(c, &in); setE(c, &in, os, getr(c, in.reg, os)); return;
    case 0x8A: modrm(c, &in); setr(c, in.reg, 8, getE(c, &in, 8)); return;
    case 0x8B: modrm(c, &in); setr(c, in.reg, os, getE(c, &in, os)); return;
    case 0x8C: modrm(c, &in); setE(c, &in, in.mod == 3 ? os : 16, in.reg < 6 ? c->sreg[in.reg] : 0); return;
    case 0x8D: modrm(c, &in); if (in.mod == 3) trap(c, CPU_ILLEGAL); setr(c, in.reg, os, in.off); return;
    case 0x8E: modrm(c, &in); if (in.reg > 5 || in.reg == 1) trap(c, CPU_ILLEGAL); cpu_load_seg(c, in.reg, (uint16_t)getE(c, &in, 16)); return;
    case 0x8F: {
        uint32_t x = os == 16 ? pop16(c) : pop32(c);
        modrm(c, &in);                              /* (ESP already up, as the CPU does) */
        setE(c, &in, os, x);
        return;
    }
    case 0x90: if (in.rep == 0xF3) return; return;  /* NOP, PAUSE */
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        v = getr(c, op & 7, os); setr(c, op & 7, os, getr(c, EAX, os)); setr(c, EAX, os, v); return;
    case 0x98: if (os == 16) setr(c, EAX, 16, (uint32_t)(int32_t)(int8_t)c->r[EAX]); else c->r[EAX] = (uint32_t)(int32_t)(int16_t)c->r[EAX]; return;
    case 0x99: if (os == 16) setr(c, EDX, 16, (c->r[EAX] & 0x8000) ? 0xFFFF : 0); else c->r[EDX] = (c->r[EAX] & 0x80000000) ? 0xFFFFFFFF : 0; return;
    case 0x9B: return;                           /* FWAIT */
    case 0x9C: v = (c->eflags & 0x00FCFFFF) | 2; if (os == 16) push16(c, (uint16_t)v); else push32(c, v); return;
    case 0x9D: {
        uint32_t keep = F_IF | 0x3000 | 0x20000;     /* (IF, IOPL, VM: not the program's) */
        v = os == 16 ? (c->eflags & 0xFFFF0000) | pop16(c) : pop32(c);
        c->eflags = (c->eflags & keep) | (v & ~keep & 0x00254FD5) | 2;
        return;
    }
    case 0x9E: c->eflags = (c->eflags & ~0xD5u) | ((c->r[EAX] >> 8) & 0xD5); return;
    case 0x9F: setr(c, 4, 8, (c->eflags & 0xD5) | 2); return;
    case 0xA0: v = fetchv(c, in.as); setr(c, EAX, 8, rd8(c, v + dseg(c, &in))); return;
    case 0xA1: v = fetchv(c, in.as); setr(c, EAX, os, rdm(c, v + dseg(c, &in), os)); return;
    case 0xA2: v = fetchv(c, in.as); wr8(c, v + dseg(c, &in), (uint8_t)c->r[EAX]); return;
    case 0xA3: v = fetchv(c, in.as); wrm(c, v + dseg(c, &in), os, c->r[EAX]); return;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, &in, op); return;
    case 0xA8: alu(c, 4, c->r[EAX], fetch8(c), 8); return;
    case 0xA9: alu(c, 4, c->r[EAX], fetchv(c, os), os); return;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setr(c, op & 7, 8, fetch8(c)); return;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        setr(c, op & 7, os, fetchv(c, os)); return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int bits = (op & 1) ? os : 8;
        modrm(c, &in);
        unsigned n = op <= 0xC1 ? fetch8(c) : op <= 0xD1 ? 1 : (c->r[ECX] & 0xFF);
        setE(c, &in, bits, shift(c, in.reg, getE(c, &in, bits), n, bits));
        return;
    }
    case 0xC2: { uint16_t n = fetch16(c); c->eip = os == 16 ? pop16(c) : pop32(c); sp_set(c, sp_get(c) + n); return; }
    case 0xC3: c->eip = os == 16 ? pop16(c) : pop32(c); return;
    case 0xC4: case 0xC5: {                      /* LES LDS */
        modrm(c, &in);
        if (in.mod == 3) trap(c, CPU_ILLEGAL);
        v = rdm(c, in.ea, os);
        uint16_t sel = rd16(c, in.ea + os / 8);
        cpu_load_seg(c, op == 0xC4 ? 0 : 3, sel);
        setr(c, in.reg, os, v);
        return;
    }
    case 0xCA: case 0xCB: {                      /* RETF (imm16) */
        uint16_t n = op == 0xCA ? fetch16(c) : 0;
        uint32_t ip = os == 16 ? pop16(c) : pop32(c);
        uint16_t sel = (uint16_t)(os == 16 ? pop16(c) : pop32(c));
        sp_set(c, sp_get(c) + n);
        cpu_load_seg(c, 1, sel);
        c->eip = ip;
        return;
    }
    case 0x9A: {                                 /* CALL ptr16:16/32 */
        uint32_t ip = fetchv(c, os);
        uint16_t sel = fetch16(c);
        if (os == 16) { push16(c, c->sreg[1]); push16(c, (uint16_t)c->eip); }
        else { push32(c, c->sreg[1]); push32(c, c->eip); }
        cpu_load_seg(c, 1, sel);
        c->eip = ip;
        return;
    }
    case 0xEA: {                                 /* JMP ptr16:16/32 */
        uint32_t ip = fetchv(c, os);
        uint16_t sel = fetch16(c);
        cpu_load_seg(c, 1, sel);
        c->eip = ip;
        return;
    }
    case 0xC6: modrm(c, &in); setE(c, &in, 8, fetch8(c)); return;
    case 0xC7: modrm(c, &in); setE(c, &in, os, fetchv(c, os)); return;
    case 0xC8: {                                 /* ENTER */
        uint16_t size = fetch16(c);
        uint8_t level = fetch8(c) & 31;
        push32(c, c->r[EBP]);
        uint32_t frame = c->r[ESP];
        for (int i = 1; i < level; i++) { c->r[EBP] -= 4; push32(c, rd32(c, c->r[EBP])); }
        if (level) push32(c, frame);
        c->r[EBP] = frame;
        c->r[ESP] -= size;
        return;
    }
    case 0xC9: c->r[ESP] = c->r[EBP]; c->r[EBP] = pop32(c); return;
    case 0xCC: c->int_no = 3; trap(c, CPU_INT); return;
    case 0xCD: c->int_no = fetch8(c); trap(c, CPU_INT); return;
    case 0xCE: if (c->eflags & F_OF) { c->int_no = 4; trap(c, CPU_INT); } return;
    case 0xD4: {                                 /* AAM */
        uint8_t base = fetch8(c), al = c->r[EAX] & 0xFF;
        if (!base) trap(c, CPU_DIVIDE);
        setr(c, 4, 8, al / base); setr(c, EAX, 8, al % base);
        szp(c, al % base, 8);
        return;
    }
    case 0xD5: {                                 /* AAD */
        uint8_t base = fetch8(c);
        uint8_t al = (uint8_t)((c->r[EAX] & 0xFF) + ((c->r[EAX] >> 8) & 0xFF) * base);
        setr(c, EAX, 16, al);
        szp(c, al, 8);
        return;
    }
    case 0xD6: setr(c, EAX, 8, (c->eflags & F_CF) ? 0xFF : 0); return;   /* SALC */
    case 0xD7: setr(c, EAX, 8, rd8(c, ((c->r[EBX] + (c->r[EAX] & 0xFF)) & (in.as == 16 ? 0xFFFF : 0xFFFFFFFF)) + dseg(c, &in))); return;   /* XLAT */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm(c, &in); fpu(c, &in, op); return;
    case 0xE0: case 0xE1: case 0xE2: {           /* LOOPNE LOOPE LOOP */
        int8_t d = (int8_t)fetch8(c);
        uint32_t am = in.as == 16 ? 0xFFFF : 0xFFFFFFFF;
        c->r[ECX] = (c->r[ECX] & ~am) | ((c->r[ECX] - 1) & am);
        int z = (c->eflags & F_ZF) != 0;
        if ((c->r[ECX] & am) && (op == 0xE2 || (op == 0xE1 && z) || (op == 0xE0 && !z))) c->eip += (uint32_t)(int32_t)d;
        return;
    }
    case 0xE3: { int8_t d = (int8_t)fetch8(c); if (!(c->r[ECX] & (in.as == 16 ? 0xFFFF : 0xFFFFFFFF))) c->eip += (uint32_t)(int32_t)d; return; }
    case 0xE8: {
        int32_t d = os == 16 ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
        if (os == 16) { push16(c, (uint16_t)c->eip); c->eip = (c->eip + (uint32_t)d) & 0xFFFF; }
        else { push32(c, c->eip); c->eip += (uint32_t)d; }
        return;
    }
    case 0xE9: {
        int32_t d = os == 16 ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
        c->eip += (uint32_t)d;
        if (os == 16) c->eip &= 0xFFFF;
        return;
    }
    case 0xEB: { int8_t d = (int8_t)fetch8(c); c->eip += (uint32_t)(int32_t)d; return; }
    case 0xF4: c->int_no = -1; trap(c, CPU_INT); return;
    case 0xF5: c->eflags ^= F_CF; return;
    case 0xF6: case 0xF7: {
        int bits = (op & 1) ? os : 8;
        modrm(c, &in);
        v = getE(c, &in, bits);
        switch (in.reg) {
        case 0: case 1: alu(c, 4, v, (op & 1) ? fetchv(c, os) : fetch8(c), bits); return;
        case 2: setE(c, &in, bits, ~v); return;
        case 3: setE(c, &in, bits, alu(c, 5, 0, v, bits)); return;
        default: muldiv(c, in.reg, v & mask_of(bits), bits); return;
        }
    }
    case 0xF8: c->eflags &= ~F_CF; return;
    case 0xF9: c->eflags |= F_CF; return;
    case 0xFA: case 0xFB: return;                /* CLI STI: not the program's to do (ignored) */
    case 0xFC: c->eflags &= ~F_DF; return;
    case 0xFD: c->eflags |= F_DF; return;
    case 0xFE:
        modrm(c, &in);
        if (in.reg > 1) trap(c, CPU_ILLEGAL);
        setE(c, &in, 8, incdec(c, getE(c, &in, 8), in.reg, 8));
        return;
    case 0xFF:
        modrm(c, &in);
        switch (in.reg) {
        case 0: case 1: setE(c, &in, os, incdec(c, getE(c, &in, os), in.reg, os)); return;
        case 2: v = getE(c, &in, os); if (os == 16) push16(c, (uint16_t)c->eip); else push32(c, c->eip); c->eip = v; return;
        case 4: c->eip = getE(c, &in, os); return;
        case 3: case 5: {                        /* CALL, JMP m16:16/32 */
            if (in.mod == 3) trap(c, CPU_ILLEGAL);
            uint32_t ip = rdm(c, in.ea, os);
            uint16_t sel = rd16(c, in.ea + os / 8);
            if (in.reg == 3) {
                if (os == 16) { push16(c, c->sreg[1]); push16(c, (uint16_t)c->eip); }
                else { push32(c, c->sreg[1]); push32(c, c->eip); }
            }
            cpu_load_seg(c, 1, sel);
            c->eip = ip;
            return;
        }
        case 6: v = getE(c, &in, os); if (os == 16) push16(c, (uint16_t)v); else push32(c, v); return;
        default: trap(c, CPU_ILLEGAL);
        }
        return;
    default: trap(c, CPU_ILLEGAL);
    }

    /* 0F xx */
    op = fetch8(c);
    switch (op) {
    case 0x05: case 0x0B: trap(c, CPU_ILLEGAL); return;
    case 0x00: {                                 /* SLDT STR VERR VERW (OpenWatcom's longjmp checks FS so) */
        modrm(c, &in);
        if (in.reg < 2) { setE(c, &in, in.mod == 3 ? os : 16, in.reg == 0 ? 0x28 : 0x30); return; }
        if (in.reg != 4 && in.reg != 5) trap(c, CPU_ILLEGAL);
        uint16_t sel = (uint16_t)getE(c, &in, 16);
        int okk = sel_valid(c, sel) && !(in.reg == 5 && sel == 0x5B);
        c->eflags = okk ? c->eflags | F_ZF : c->eflags & ~F_ZF;
        return;
    }
    case 0x02: case 0x03: {                      /* LAR LSL */
        modrm(c, &in);
        uint16_t sel = (uint16_t)getE(c, &in, 16);
        if (!sel_valid(c, sel)) { c->eflags &= ~F_ZF; return; }
        c->eflags |= F_ZF;
        if (op == 0x02) setr(c, in.reg, os, (sel & 4) ? 0x0000F300 : sel == 0x5B ? 0x00CFFB00 : 0x00CFF300);
        else setr(c, in.reg, os, (sel & 4) ? 0xFFFF : sel == 0x150B ? 0xFFF : 0xFFFFFFFF);
        return;
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        modrm(c, &in); return;                   /* NOP Ev, PREFETCH */
    case 0x31: c->tsc += 1000; c->r[EAX] = (uint32_t)c->tsc; c->r[EDX] = (uint32_t)(c->tsc >> 32); return;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        modrm(c, &in); v = getE(c, &in, os); if (cond(c, op & 15)) setr(c, in.reg, os, v); return;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        int32_t d = os == 16 ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
        if (cond(c, op & 15)) c->eip += (uint32_t)d;
        return;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        modrm(c, &in); setE(c, &in, 8, cond(c, op & 15)); return;
    case 0xA0: case 0xA8: if (os == 16) push16(c, c->sreg[op == 0xA0 ? 4 : 5]); else push32(c, c->sreg[op == 0xA0 ? 4 : 5]); return;
    case 0xA1: case 0xA9: v = os == 16 ? pop16(c) : pop32(c); cpu_load_seg(c, op == 0xA1 ? 4 : 5, (uint16_t)v); return;
    case 0xB2: case 0xB4: case 0xB5: {           /* LSS LFS LGS */
        modrm(c, &in);
        if (in.mod == 3) trap(c, CPU_ILLEGAL);
        v = rdm(c, in.ea, os);
        uint16_t sel = rd16(c, in.ea + os / 8);
        cpu_load_seg(c, op == 0xB2 ? 2 : op == 0xB4 ? 4 : 5, sel);
        setr(c, in.reg, os, v);
        return;
    }
    case 0xA2: {                                 /* CPUID: a Pentium with an FPU, TSC, CX8, CMOV */
        uint32_t leaf = c->r[EAX];
        if (leaf == 0) { c->r[EAX] = 1; c->r[EBX] = 0x756E6547; c->r[EDX] = 0x49656E69; c->r[ECX] = 0x6C65746E; }
        else if (leaf == 1) { c->r[EAX] = 0x543; c->r[EBX] = 0; c->r[ECX] = 0; c->r[EDX] = 0x00008111; }
        else c->r[EAX] = c->r[EBX] = c->r[ECX] = c->r[EDX] = 0;
        return;
    }
    case 0xA3: modrm(c, &in); bitop(c, &in, 4, getr(c, in.reg, os), 1); return;
    case 0xAB: modrm(c, &in); bitop(c, &in, 5, getr(c, in.reg, os), 1); return;
    case 0xB3: modrm(c, &in); bitop(c, &in, 6, getr(c, in.reg, os), 1); return;
    case 0xBB: modrm(c, &in); bitop(c, &in, 7, getr(c, in.reg, os), 1); return;
    case 0xBA: modrm(c, &in); if (in.reg < 4) trap(c, CPU_ILLEGAL); bitop(c, &in, in.reg, fetch8(c), 0); return;
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
        modrm(c, &in);
        unsigned n = (op & 1) ? (c->r[ECX] & 0xFF) : fetch8(c);
        setE(c, &in, os, shxd(c, op < 0xA8, getE(c, &in, os), getr(c, in.reg, os), n, os));
        return;
    }
    case 0xAF: modrm(c, &in); setr(c, in.reg, os, imul2(c, getr(c, in.reg, os), getE(c, &in, os), os)); return;
    case 0xB0: case 0xB1: {                      /* CMPXCHG */
        int bits = (op & 1) ? os : 8;
        modrm(c, &in);
        v = getE(c, &in, bits);
        alu(c, 7, getr(c, EAX, bits), v, bits);
        if (c->eflags & F_ZF) setE(c, &in, bits, getr(c, in.reg, bits));
        else setr(c, EAX, bits, v);
        return;
    }
    case 0xC7: {                                 /* CMPXCHG8B */
        modrm(c, &in);
        if (in.reg != 1 || in.mod == 3) trap(c, CPU_ILLEGAL);
        uint64_t m = rd64(c, in.ea);
        if (m == ((uint64_t)c->r[EDX] << 32 | c->r[EAX])) { wr64(c, in.ea, (uint64_t)c->r[ECX] << 32 | c->r[EBX]); c->eflags |= F_ZF; }
        else { c->r[EAX] = (uint32_t)m; c->r[EDX] = (uint32_t)(m >> 32); c->eflags &= ~F_ZF; }
        return;
    }
    case 0xB6: modrm(c, &in); setr(c, in.reg, os, getE(c, &in, 8)); return;
    case 0xB7: modrm(c, &in); setr(c, in.reg, os, getE(c, &in, 16)); return;
    case 0xBE: modrm(c, &in); setr(c, in.reg, os, sext(getE(c, &in, 8), 8)); return;
    case 0xBF: modrm(c, &in); setr(c, in.reg, os, sext(getE(c, &in, 16), 16)); return;
    case 0xBC: case 0xBD: {                      /* BSF BSR */
        modrm(c, &in);
        v = getE(c, &in, os);
        if (!v) { c->eflags |= F_ZF; return; }
        c->eflags &= ~F_ZF;
        setr(c, in.reg, os, op == 0xBC ? (uint32_t)__builtin_ctz(v) : (uint32_t)(31 - __builtin_clz(v)));
        return;
    }
    case 0xC0: case 0xC1: {                      /* XADD */
        int bits = (op & 1) ? os : 8;
        modrm(c, &in);
        v = getE(c, &in, bits); w = getr(c, in.reg, bits);
        uint32_t s = alu(c, 0, v, w, bits);
        setr(c, in.reg, bits, v);
        setE(c, &in, bits, s);
        return;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        c->r[op & 7] = __builtin_bswap32(c->r[op & 7]); return;
    }
    trap(c, CPU_ILLEGAL);
}

int cpu_run(cpu_t *c, long steps)
{
    jmp_buf jb, *outer = trap_jb;
    trap_jb = &jb;
    int why = setjmp(jb);
    if (why) {
        trap_jb = outer;
        if (why == CPU_FAULT || why == CPU_ILLEGAL || why == CPU_DIVIDE) c->eip = c->op_eip;   /* (at the instruction) */
        return why;
    }
    for (long i = 0; i < steps; i++) {
        uint32_t lin = c->cs_base + c->eip;
        if (lin >= c->thunk_lo && lin < c->thunk_hi) { trap_jb = outer; return CPU_THUNK; }
        step(c);
    }
    trap_jb = outer;
    return CPU_STEPS;
}
