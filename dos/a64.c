/* dos/a64.c - AArch64 (A64) disassembler and assembler (see a64.h) for DEBUG's
 * U and A commands. Freestanding (only <stdint.h>): the same file builds into
 * the kernel and, for the LLVM comparison test, on the build machine
 * (dos/tests/test_a64.sh).
 *
 * Disassembles: branches (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR/BLR/RET/ERET),
 * exceptions (SVC/HVC/SMC/BRK/HLT), system (hints, barriers, MRS/MSR with names
 * from an llvm-mc generated table, PSTATE fields, SYS aliases for DC/IC/TLBI),
 * ADR/ADRP, add/sub (immediate, shifted and extended register, with carry),
 * logical (immediate and shifted register), move wide, bitfield and extract,
 * conditional compare and select, data processing (1, 2, 3 sources),
 * loads/stores (unsigned offset, unscaled, pre/post index, register offset,
 * literal, pairs, exclusive, acquire/release, FP/SIMD registers too), LSE
 * atomics, pointer authentication, and FP/Advanced SIMD (a64_simd.inc).
 * Spelling follows LLVM's alias rules (mov, cmp, cmn, tst, neg, mvn,
 * lsl/lsr/asr, ubfx, sxtw, cset, cinc, mul, ...). SVE, SME and ARMv8.9's
 * 128-bit system instructions come out as ".inst 0x...".
 *
 * The assembler encodes integer instructions and scalar FP directly. Other
 * FP/SIMD forms are found by search: try a group's varying bits with the
 * registers filled in until the disassembly matches the line (the
 * disassembler is exact, so that's the encoding). */
#include "a64.h"

#include "a64_sysregs.inc"

/* ── text ── */
typedef struct { char *p; unsigned n, cap; } buf_t;
static void put(buf_t *b, const char *s) { while (*s) { if (b->n + 1 < b->cap) b->p[b->n++] = *s; s++; } b->p[b->n < b->cap ? b->n : b->cap - 1] = 0; }
static void putc1(buf_t *b, char c) { char s[2] = { c, 0 }; put(b, s); }
static void puthex(buf_t *b, a64_u64 v)
{
    char t[20]; int i = 0;
    do { t[i++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    put(b, "0x");
    while (i) putc1(b, t[--i]);
}
static void putdec(buf_t *b, a64_u64 v)
{
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) putc1(b, t[--i]);
}
static void imm_hex(buf_t *b, a64_s64 v) { put(b, "#"); if (v < 0) { put(b, "-"); puthex(b, (a64_u64)-v); } else puthex(b, (a64_u64)v); }
static void imm_uhex(buf_t *b, a64_u64 v) { put(b, "#"); puthex(b, v); }
static void imm_dec(buf_t *b, a64_s64 v) { put(b, "#"); if (v < 0) { put(b, "-"); putdec(b, (a64_u64)-v); } else putdec(b, (a64_u64)v); }

static a64_u32 bits(a64_u32 x, int hi, int lo) { return (x >> lo) & ((1u << (hi - lo + 1)) - 1); }
static a64_s64 sext(a64_u64 v, int n) { a64_u64 m = 1ull << (n - 1); v &= (m << 1) - 1; return (a64_s64)((v ^ m) - m); }

/* a general register: 64-bit x / 32-bit w; 31 is the zero register, or the stack pointer where sp says so */
static void reg(buf_t *b, int r, int sf, int sp)
{
    if (r == 31) { put(b, sp ? (sf ? "sp" : "wsp") : (sf ? "xzr" : "wzr")); return; }
    putc1(b, sf ? 'x' : 'w');
    putdec(b, (a64_u64)r);
}
static void sep(buf_t *b) { put(b, ", "); }
static void mn(buf_t *b, const char *m) { put(b, m); putc1(b, '\t'); }

static const char *const conds[16] = { "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv" };
static const char *const shifts[4] = { "lsl", "lsr", "asr", "ror" };
static const char *const extends[8] = { "uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx" };

/* logical immediate (N:immr:imms) -> the value (0: not encodable) */
static a64_u64 ror_w(a64_u64 v, int r, int w) { a64_u64 m = w == 64 ? ~0ull : (1ull << w) - 1; v &= m; return r ? ((v >> r) | (v << (w - r))) & m : v; }
static int decode_bitmask(int n, int immr, int imms, int sf, a64_u64 *out)
{
    int len = -1;
    int v = (n << 6) | (~imms & 0x3f);
    for (int i = 6; i >= 0; i--) if (v & (1 << i)) { len = i; break; }
    if (len < 1) return 0;
    int esize = 1 << len, levels = esize - 1;
    int s = imms & levels, r = immr & levels;
    if (s == levels) return 0;
    if (!sf && n) return 0;
    a64_u64 welem = (1ull << (s + 1)) - 1;
    a64_u64 e = ror_w(welem, r, esize);
    a64_u64 res = 0;
    for (int i = 0; i < (sf ? 64 : 32); i += esize) res |= e << i;
    *out = sf ? res : res & 0xffffffffull;
    return 1;
}
/* MOVZ/MOVN can build it (LLVM then prefers mov via those over the bitmask form) */
static int move_wide_ok(a64_u64 v, int sf)
{
    int w = sf ? 64 : 32;
    a64_u64 m = sf ? ~0ull : 0xffffffffull;
    for (int k = 0; k < 2; k++) {
        a64_u64 x = (k ? ~v : v) & m;
        for (int h = 0; h < w; h += 16) if ((x & ~(0xffffull << h) & m) == 0) return 1;
    }
    return 0;
}

static void target(buf_t *b, a64_u64 a) { puthex(b, a); }

static const char *sysreg_name(a64_u32 key)
{
    for (unsigned i = 0; i < sizeof a64_sysregs / sizeof a64_sysregs[0]; i++) if (a64_sysregs[i].key == key) return a64_sysregs[i].name;
    return 0;
}
static void sysreg(buf_t *b, a64_u32 insn)
{
    a64_u32 key = bits(insn, 19, 5);
    const char *n = bits(insn, 20, 20) ? sysreg_name(key) : 0;    /* the table covers op0 2 and 3 */
    if (n) { put(b, n); return; }
    put(b, "S"); putdec(b, bits(insn, 20, 19)); put(b, "_"); putdec(b, bits(insn, 18, 16));
    put(b, "_C"); putdec(b, bits(insn, 15, 12)); put(b, "_C"); putdec(b, bits(insn, 11, 8)); put(b, "_"); putdec(b, bits(insn, 7, 5));
}

/* ── data processing, immediate ── */
static int dp_imm(a64_u32 i, a64_u64 pc, buf_t *b)
{
    int sf = bits(i, 31, 31), rd = bits(i, 4, 0), rn = bits(i, 9, 5);
    switch (bits(i, 25, 23)) {
    case 0: case 1: {                                            /* ADR, ADRP */
        a64_s64 imm = sext((bits(i, 23, 5) << 2) | bits(i, 30, 29), 21);
        int page = bits(i, 31, 31);
        mn(b, page ? "adrp" : "adr"); reg(b, rd, 1, 0); sep(b);
        target(b, page ? (pc & ~0xfffull) + (a64_u64)(imm << 12) : pc + (a64_u64)imm);
        return 1;
    }
    case 2: {                                                    /* add/sub immediate */
        int op = bits(i, 30, 30), s = bits(i, 29, 29), sh = bits(i, 22, 22);
        a64_u32 imm = bits(i, 21, 10);
        if (bits(i, 23, 23)) return 0;
        if (s && rd == 31) { mn(b, op ? "cmp" : "cmn"); reg(b, rn, sf, 1); }
        else if (!op && !s && !sh && !imm && (rd == 31 || rn == 31)) { mn(b, "mov"); reg(b, rd, sf, 1); sep(b); reg(b, rn, sf, 1); return 1; }
        else { mn(b, op ? (s ? "subs" : "sub") : (s ? "adds" : "add")); reg(b, rd, sf, !s); sep(b); reg(b, rn, sf, 1); }
        sep(b); imm_hex(b, imm);
        if (sh) put(b, ", lsl #12");
        return 1;
    }
    case 4: {                                                    /* logical immediate */
        int opc = bits(i, 30, 29);
        a64_u64 v;
        if (!decode_bitmask(bits(i, 22, 22), bits(i, 21, 16), bits(i, 15, 10), sf, &v)) return 0;
        if (opc == 1 && rn == 31 && !move_wide_ok(v, sf)) {        /* mov: show the value signed */
            mn(b, "mov"); reg(b, rd, sf, 1); sep(b);
            imm_hex(b, sf ? (a64_s64)v : (a64_s64)(int)(a64_u32)v); return 1;
        }
        if (opc == 3 && rd == 31) { mn(b, "tst"); reg(b, rn, sf, 0); }
        else {
            static const char *const m[4] = { "and", "orr", "eor", "ands" };
            mn(b, m[opc]); reg(b, rd, sf, opc != 3); sep(b); reg(b, rn, sf, 0);
        }
        sep(b); imm_uhex(b, v);
        return 1;
    }
    case 5: {                                                    /* move wide */
        int opc = bits(i, 30, 29), hw = bits(i, 22, 21);
        a64_u64 imm = bits(i, 20, 5);
        if (opc == 1 || (!sf && hw > 1)) return 0;
        if (opc == 3) { mn(b, "movk"); reg(b, rd, sf, 0); sep(b); imm_hex(b, (a64_s64)imm); if (hw) { put(b, ", lsl #"); putdec(b, (a64_u64)hw * 16); } return 1; }
        int alias = !(imm == 0 && hw != 0);
        if (opc == 0 && !sf && imm == 0xffff) alias = 0;          /* movn w, #0xffff isn't shown as mov */
        if (alias) {
            a64_u64 v = imm << (hw * 16);
            if (opc == 0) v = ~v;
            if (!sf) v &= 0xffffffffull;
            mn(b, "mov"); reg(b, rd, sf, 0); sep(b);
            imm_hex(b, sf ? (a64_s64)v : (a64_s64)(int)(a64_u32)v);
            return 1;
        }
        mn(b, opc == 0 ? "movn" : "movz"); reg(b, rd, sf, 0); sep(b); imm_hex(b, (a64_s64)imm);
        if (hw) { put(b, ", lsl #"); putdec(b, (a64_u64)hw * 16); }
        return 1;
    }
    case 6: {                                                    /* bitfield */
        int opc = bits(i, 30, 29), n = bits(i, 22, 22), immr = bits(i, 21, 16), imms = bits(i, 15, 10);
        int size = sf ? 64 : 32;
        if (opc == 3 || n != sf) return 0;
        if (opc == 0) {                                          /* SBFM */
            if (imms == size - 1) { mn(b, "asr"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, immr); return 1; }
            if (imms < immr) { mn(b, "sbfiz"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, size - immr); sep(b); imm_dec(b, imms + 1); return 1; }
            if (immr == 0 && (imms == 7 || imms == 15 || (sf && imms == 31))) {
                mn(b, imms == 7 ? "sxtb" : imms == 15 ? "sxth" : "sxtw"); reg(b, rd, sf, 0); sep(b); reg(b, rn, 0, 0); return 1;
            }
            mn(b, "sbfx"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, immr); sep(b); imm_dec(b, imms - immr + 1); return 1;
        }
        if (opc == 2) {                                          /* UBFM */
            if (imms != size - 1 && imms + 1 == immr) { mn(b, "lsl"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, size - 1 - imms); return 1; }
            if (imms == size - 1) { mn(b, "lsr"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, immr); return 1; }
            if (imms < immr) { mn(b, "ubfiz"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, size - immr); sep(b); imm_dec(b, imms + 1); return 1; }
            if (!sf && immr == 0 && (imms == 7 || imms == 15)) { mn(b, imms == 7 ? "uxtb" : "uxth"); reg(b, rd, 0, 0); sep(b); reg(b, rn, 0, 0); return 1; }
            mn(b, "ubfx"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, immr); sep(b); imm_dec(b, imms - immr + 1); return 1;
        }
        if (imms < immr) {                                       /* BFM */
            if (rn == 31) { mn(b, "bfc"); reg(b, rd, sf, 0); }
            else { mn(b, "bfi"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); }
            sep(b); imm_dec(b, size - immr); sep(b); imm_dec(b, imms + 1); return 1;
        }
        mn(b, "bfxil"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); imm_dec(b, immr); sep(b); imm_dec(b, imms - immr + 1); return 1;
    }
    case 7: {                                                    /* extract */
        int rm = bits(i, 20, 16), imms = bits(i, 15, 10);
        if (bits(i, 30, 29) || (int)bits(i, 22, 22) != sf || bits(i, 21, 21) || (!sf && imms > 31)) return 0;
        if (rn == rm) { mn(b, "ror"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); }
        else { mn(b, "extr"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0); }
        sep(b); imm_hex(b, imms);
        return 1;
    }
    }
    return 0;
}

/* ── branches, exceptions, system ── */
static const struct { a64_u32 sys; const char *name; int has_rt; } sysops[] = {   /* SYS op1:CRn:CRm:op2 (bits 18:5) */
    { (0 << 11) | (7 << 7) | (5 << 3) | 0, "ic\tiallu", 0 },  { (0 << 11) | (7 << 7) | (1 << 3) | 0, "ic\tialluis", 0 },
    { (3 << 11) | (7 << 7) | (5 << 3) | 1, "ic\tivau", 1 },
    { (3 << 11) | (7 << 7) | (4 << 3) | 1, "dc\tzva", 1 },    { (0 << 11) | (7 << 7) | (6 << 3) | 1, "dc\tivac", 1 },
    { (0 << 11) | (7 << 7) | (6 << 3) | 2, "dc\tisw", 1 },    { (3 << 11) | (7 << 7) | (10 << 3) | 1, "dc\tcvac", 1 },
    { (0 << 11) | (7 << 7) | (10 << 3) | 2, "dc\tcsw", 1 },   { (3 << 11) | (7 << 7) | (11 << 3) | 1, "dc\tcvau", 1 },
    { (3 << 11) | (7 << 7) | (14 << 3) | 1, "dc\tcivac", 1 }, { (0 << 11) | (7 << 7) | (14 << 3) | 2, "dc\tcisw", 1 },
    { (0 << 11) | (8 << 7) | (7 << 3) | 0, "tlbi\tvmalle1", 0 },   { (0 << 11) | (8 << 7) | (3 << 3) | 0, "tlbi\tvmalle1is", 0 },
    { (0 << 11) | (8 << 7) | (7 << 3) | 1, "tlbi\tvae1", 1 },      { (0 << 11) | (8 << 7) | (3 << 3) | 1, "tlbi\tvae1is", 1 },
    { (0 << 11) | (8 << 7) | (7 << 3) | 2, "tlbi\taside1", 1 },    { (0 << 11) | (8 << 7) | (3 << 3) | 2, "tlbi\taside1is", 1 },
    { (0 << 11) | (8 << 7) | (7 << 3) | 3, "tlbi\tvaae1", 1 },     { (0 << 11) | (8 << 7) | (3 << 3) | 3, "tlbi\tvaae1is", 1 },
    { (4 << 11) | (8 << 7) | (7 << 3) | 0, "tlbi\talle2", 0 },     { (4 << 11) | (8 << 7) | (3 << 3) | 0, "tlbi\talle2is", 0 },
    { (4 << 11) | (8 << 7) | (7 << 3) | 4, "tlbi\talle1", 0 },     { (4 << 11) | (8 << 7) | (3 << 3) | 4, "tlbi\talle1is", 0 },
    { (4 << 11) | (8 << 7) | (7 << 3) | 6, "tlbi\tvmalls12e1", 0 },{ (4 << 11) | (8 << 7) | (3 << 3) | 6, "tlbi\tvmalls12e1is", 0 },
    { (0 << 11) | (7 << 7) | (8 << 3) | 0, "at\ts1e1r", 1 },       { (0 << 11) | (7 << 7) | (8 << 3) | 1, "at\ts1e1w", 1 },
};
static const char *const barrier_opts[16] = { "#0x0", "oshld", "oshst", "osh", "#0x4", "nshld", "nshst", "nsh",
                                              "#0x8", "ishld", "ishst", "ish", "#0xc", "ld", "st", "sy" };
static int branch_sys(a64_u32 i, a64_u64 pc, buf_t *b)
{
    a64_u32 top6 = bits(i, 31, 26);
    if (top6 == 0x05 || top6 == 0x25) { mn(b, top6 == 0x25 ? "bl" : "b"); target(b, pc + (a64_u64)(sext(bits(i, 25, 0), 26) << 2)); return 1; }
    if (bits(i, 31, 24) == 0x54 && !bits(i, 4, 4)) {
        put(b, "b."); put(b, conds[bits(i, 3, 0)]); putc1(b, '\t');
        target(b, pc + (a64_u64)(sext(bits(i, 23, 5), 19) << 2)); return 1;
    }
    if (bits(i, 30, 25) == 0x1a) {                               /* CBZ/CBNZ */
        mn(b, bits(i, 24, 24) ? "cbnz" : "cbz"); reg(b, bits(i, 4, 0), bits(i, 31, 31), 0); sep(b);
        target(b, pc + (a64_u64)(sext(bits(i, 23, 5), 19) << 2)); return 1;
    }
    if (bits(i, 30, 25) == 0x1b) {                               /* TBZ/TBNZ */
        int bit = (bits(i, 31, 31) << 5) | bits(i, 23, 19);
        mn(b, bits(i, 24, 24) ? "tbnz" : "tbz"); reg(b, bits(i, 4, 0), bit >= 32, 0); sep(b); imm_hex(b, bit); sep(b);
        target(b, pc + (a64_u64)(sext(bits(i, 18, 5), 14) << 2)); return 1;
    }
    if (bits(i, 31, 24) == 0xd4) {                               /* exceptions */
        int opc = bits(i, 23, 21), ll = bits(i, 1, 0);
        a64_u32 imm = bits(i, 20, 5);
        const char *m = 0;
        if (bits(i, 4, 2)) return 0;
        if (opc == 0) m = ll == 1 ? "svc" : ll == 2 ? "hvc" : ll == 3 ? "smc" : 0;
        else if (opc == 1 && !ll) m = "brk";
        else if (opc == 2 && !ll) m = "hlt";
        else if (opc == 5) m = ll == 1 ? "dcps1" : ll == 2 ? "dcps2" : ll == 3 ? "dcps3" : 0;
        if (!m) return 0;
        if (opc == 5 && !imm) { put(b, m); return 1; }
        mn(b, m);
        if (opc == 0 && !imm) put(b, "#0"); else imm_hex(b, imm);   /* svc/hvc/smc #0, like LLVM */
        return 1;
    }
    if (bits(i, 31, 22) == 0x354) {                              /* system */
        int l = bits(i, 21, 21), op0 = bits(i, 20, 19), op1 = bits(i, 18, 16), crn = bits(i, 15, 12), crm = bits(i, 11, 8), op2 = bits(i, 7, 5), rt = bits(i, 4, 0);
        if (!l && op0 == 0 && crn == 2 && rt == 31) {             /* hints */
            static const char *const h[8] = { "nop", "yield", "wfe", "wfi", "sev", "sevl", 0, 0 };
            int imm = (crm << 3) | op2;
            if (imm < 6) { put(b, h[imm]); return 1; }
            if (imm == 7) { put(b, "xpaclri"); return 1; }
            {   /* pointer authentication */
                static const struct { int imm; const char *n; } pac[] = {
                    { 8, "pacia1716" }, { 10, "pacib1716" }, { 12, "autia1716" }, { 14, "autib1716" },
                    { 0x18, "paciaz" }, { 0x19, "paciasp" }, { 0x1a, "pacibz" }, { 0x1b, "pacibsp" },
                    { 0x1c, "autiaz" }, { 0x1d, "autiasp" }, { 0x1e, "autibz" }, { 0x1f, "autibsp" } };
                for (unsigned k = 0; k < sizeof pac / sizeof pac[0]; k++) if (pac[k].imm == imm) { put(b, pac[k].n); return 1; }
            }
            if (imm == 0x10) { put(b, "esb"); return 1; }
            if (imm == 0x11) { put(b, "psb\tcsync"); return 1; }
            if (imm == 0x14) { put(b, "csdb"); return 1; }
            if (imm == 0x20 || imm == 0x22 || imm == 0x24 || imm == 0x26) {
                static const char *const bti[4] = { "bti", "bti\tc", "bti\tj", "bti\tjc" };
                put(b, bti[(imm - 0x20) / 2]); return 1;
            }
            mn(b, "hint"); imm_hex(b, imm); return 1;
        }
        if (!l && op0 == 0 && crn == 3 && rt == 31) {             /* barriers */
            if (op2 == 2) { if (crm == 15) put(b, "clrex"); else { mn(b, "clrex"); imm_hex(b, crm); } return 1; }
            if (op2 == 4) { if (crm == 0) { put(b, "ssbb"); return 1; } if (crm == 4) { put(b, "pssbb"); return 1; } mn(b, "dsb"); put(b, barrier_opts[crm]); return 1; }
            if (op2 == 5) { mn(b, "dmb"); put(b, barrier_opts[crm]); return 1; }
            if (op2 == 6) { if (crm == 15) put(b, "isb"); else { mn(b, "isb"); imm_hex(b, crm); } return 1; }
            if (op2 == 7) { put(b, "sb"); return 1; }
            return 0;
        }
        if (!l && op0 == 0 && crn == 4 && rt == 31) {             /* MSR (immediate): PSTATE fields */
            const char *f = 0;
            if (op1 == 3 && op2 == 6) f = "DAIFSet";
            else if (op1 == 3 && op2 == 7) f = "DAIFClr";
            else if (op1 == 0 && op2 == 5) f = "SPSel";
            else if (op1 == 0 && op2 == 4) f = "PAN";
            else if (op1 == 0 && op2 == 3) f = "UAO";
            if (!f) return 0;
            mn(b, "msr"); put(b, f); sep(b); imm_hex(b, crm); return 1;
        }
        if (op0 == 1) {                                          /* SYS / SYSL: DC, IC, TLBI, AT */
            a64_u32 key = (a64_u32)((op1 << 11) | (crn << 7) | (crm << 3) | op2);
            if (!l)
                for (unsigned k = 0; k < sizeof sysops / sizeof sysops[0]; k++)
                    if (sysops[k].sys == key && (sysops[k].has_rt || rt == 31)) {
                        put(b, sysops[k].name); if (sysops[k].has_rt) { sep(b); reg(b, rt, 1, 0); } return 1;
                    }
            mn(b, l ? "sysl" : "sys");
            if (l) { reg(b, rt, 1, 0); sep(b); }
            imm_hex(b, op1); put(b, ", c"); putdec(b, (a64_u64)crn); put(b, ", c"); putdec(b, (a64_u64)crm); sep(b); imm_hex(b, op2);
            if (!l && rt != 31) { sep(b); reg(b, rt, 1, 0); }
            return 1;
        }
        if (op0 >= 2 || op0 == 0) {                              /* MRS / MSR (register; op0 0 uses the generic names, like LLVM) */
            if (l) { mn(b, "mrs"); reg(b, rt, 1, 0); sep(b); sysreg(b, i); }
            else { mn(b, "msr"); sysreg(b, i); sep(b); reg(b, rt, 1, 0); }
            return 1;
        }
        return 0;
    }
    if (bits(i, 31, 25) == 0x6b) {                               /* branch to register */
        int opc = bits(i, 24, 21), op2 = bits(i, 20, 16), op3 = bits(i, 15, 10), rn = bits(i, 9, 5), op4 = bits(i, 4, 0);
        if (op2 == 31 && (op3 == 2 || op3 == 3)) {               /* pointer authentication: braa(z)/blraa(z)/retaa, the b keys */
            const char *k = op3 == 3 ? "b" : "a";
            char m[16]; buf_t mb = { m, 0, sizeof m };
            if (opc == 2 && rn == 31 && op4 == 31) { put(b, op3 == 3 ? "retab" : "retaa"); return 1; }
            if (opc == 4 && rn == 31 && op4 == 31) { put(b, op3 == 3 ? "eretab" : "eretaa"); return 1; }
            if ((opc == 0 || opc == 1) && op4 == 31) { put(&mb, opc ? "blra" : "bra"); put(&mb, k); put(&mb, "z"); mn(b, m); reg(b, rn, 1, 0); return 1; }
            if (opc == 8 || opc == 9) { put(&mb, opc == 9 ? "blra" : "bra"); put(&mb, k); mn(b, m); reg(b, rn, 1, 0); sep(b); reg(b, op4, 1, 1); return 1; }
            return 0;
        }
        if (op2 != 31 || op3 || op4) return 0;
        if (opc == 0) { mn(b, "br"); reg(b, rn, 1, 0); return 1; }
        if (opc == 1) { mn(b, "blr"); reg(b, rn, 1, 0); return 1; }
        if (opc == 2) { if (rn == 30) put(b, "ret"); else { mn(b, "ret"); reg(b, rn, 1, 0); } return 1; }
        if (opc == 4 && rn == 31) { put(b, "eret"); return 1; }
        if (opc == 5 && rn == 31) { put(b, "drps"); return 1; }
        return 0;
    }
    return 0;
}

/* ── loads and stores ── */
static int simd_ldst(a64_u32 i, buf_t *b);
static void vreg(buf_t *b, int r, int scale)                    /* an FP/SIMD register: b h s d q */
{
    putc1(b, "bhsdq"[scale]); putdec(b, (a64_u64)r);
}
static void mem_imm(buf_t *b, int rn, a64_s64 off, int mode)    /* mode 0 offset, 1 pre-index, 2 post-index */
{
    put(b, "["); reg(b, rn, 1, 1);
    if (mode == 2) { put(b, "], "); imm_hex(b, off); return; }
    if (off || mode == 1) { sep(b); imm_hex(b, off); }
    put(b, "]");
    if (mode == 1) put(b, "!");
}
static void prfop(buf_t *b, int t)                              /* pld/pli/pst, l1 l2 l3 slc, keep/strm; 24 is ir */
{
    static const char *const ty[3] = { "pld", "pli", "pst" }, *const lv[4] = { "l1", "l2", "l3", "slc" };
    if (t == 24) { put(b, "ir"); return; }
    if (t >= 24) { imm_hex(b, t); return; }
    put(b, ty[t >> 3]); put(b, lv[(t >> 1) & 3]); put(b, t & 1 ? "strm" : "keep");
}

/* LSE atomics, LDAPR, LDRAA/LDRAB: the register-offset slot's other forms */
static int ldst_atomic(a64_u32 i, buf_t *b)
{
    int rt = bits(i, 4, 0), rn = bits(i, 9, 5), size = bits(i, 31, 30);
    if (bits(i, 11, 10) & 1) {                                   /* LDRAA / LDRAB (pointer authentication) */
        if (size != 3) return 0;
        a64_s64 off = sext((bits(i, 22, 22) << 9) | bits(i, 20, 12), 10) << 3;
        mn(b, bits(i, 23, 23) ? "ldrab" : "ldraa"); reg(b, rt, 1, 0); sep(b);
        mem_imm(b, rn, off, bits(i, 11, 11) ? 1 : 0);
        return 1;
    }
    if (bits(i, 11, 10) == 0) {                                  /* LSE atomics, LDAPR */
        int a = bits(i, 23, 23), r = bits(i, 22, 22), rs = bits(i, 20, 16), o3 = bits(i, 15, 15), aop = bits(i, 14, 12);
        static const char *const sfx2[4] = { "b", "h", "", "" };
        int sf = size == 3;
        char m[16]; buf_t mb = { m, 0, sizeof m };
        if (o3 && aop == 4) {                                    /* LDAPR */
            if (!a || r) return 0;
            (void)rs;
            put(&mb, "ldapr"); put(&mb, sfx2[size]);
            mn(b, m); reg(b, rt, sf, 0); put(b, ", ["); reg(b, rn, 1, 1); put(b, "]"); return 1;
        }
        if (o3 && aop) return 0;
        static const char *const ops[8] = { "add", "clr", "eor", "set", "smax", "smin", "umax", "umin" };
        int st = !o3 && rt == 31 && !a;                          /* ST<op>: the result isn't wanted */
        put(&mb, o3 ? "swp" : st ? "st" : "ld"); if (!o3) put(&mb, ops[aop]);
        if (a) put(&mb, "a"); if (r) put(&mb, "l"); put(&mb, sfx2[size]);
        mn(b, m); reg(b, rs, sf, 0); sep(b);
        if (!st) { reg(b, rt, sf, 0); sep(b); }
        put(b, "["); reg(b, rn, 1, 1); put(b, "]");
        return 1;
    }
    return 0;
}

static int ldst(a64_u32 i, a64_u64 pc, buf_t *b)
{
    int rt = bits(i, 4, 0), rn = bits(i, 9, 5), v = bits(i, 26, 26);
    if (v && bits(i, 29, 27) == 1) return simd_ldst(i, b);           /* structures: a64_simd.inc */
    /* load literal */
    if (bits(i, 29, 27) == 3 && bits(i, 25, 24) == 0) {
        int opc = bits(i, 31, 30);
        a64_u64 a = pc + (a64_u64)(sext(bits(i, 23, 5), 19) << 2);
        if (v) { if (opc == 3) return 0; mn(b, "ldr"); vreg(b, rt, opc + 2); }
        else if (opc == 3) { mn(b, "prfm"); prfop(b, rt); }
        else { mn(b, opc == 2 ? "ldrsw" : "ldr"); reg(b, rt, opc != 0, 0); }
        sep(b); target(b, a);
        return 1;
    }
    /* exclusive, acquire/release */
    if (bits(i, 29, 24) == 0x08 && !v) {
        int size = bits(i, 31, 30), o2 = bits(i, 23, 23), l = bits(i, 22, 22), o1 = bits(i, 21, 21), o0 = bits(i, 15, 15);
        int rs = bits(i, 20, 16), rt2 = bits(i, 14, 10);
        int sf = size == 3;
        static const char *const sfx[4] = { "b", "h", "", "" };
        char m[16]; buf_t mb = { m, 0, sizeof m };
        (void)rt2;
        if (o2 && !o1) {                                         /* load-acquire / store-release (LORegion: o0 0) */
            put(&mb, l ? (o0 ? "ldar" : "ldlar") : (o0 ? "stlr" : "stllr")); put(&mb, sfx[size]);
            mn(b, m); reg(b, rt, sf, 0); put(b, ", ["); reg(b, rn, 1, 1); put(b, "]"); return 1;
        }
        if (o1 && rt2 == 31 && (o2 || size < 2)) {               /* CAS (o2), CASP (size 0/1, o2 0) */
            int pair = !o2;
            put(&mb, pair ? "casp" : "cas"); if (l) put(&mb, "a"); if (o0) put(&mb, "l"); if (!pair) put(&mb, sfx[size]);
            int psf = pair ? (size & 1) : sf;
            mn(b, m); reg(b, rs, psf, 0); sep(b);
            if (pair) { reg(b, rs + 1, psf, 0); sep(b); }
            reg(b, rt, psf, 0); sep(b);
            if (pair) { reg(b, rt + 1, psf, 0); sep(b); }
            put(b, "["); reg(b, rn, 1, 1); put(b, "]");
            return pair ? !(rs & 1) && !(rt & 1) : 1;
        }
        if (o2) return 0;
        if (o1 && size < 2) return 0;
        put(&mb, l ? (o0 ? "ldax" : "ldx") : (o0 ? "stlx" : "stx")); put(&mb, o1 ? "p" : "r"); put(&mb, sfx[size]);
        mn(b, m);
        if (!l) { reg(b, rs, 0, 0); sep(b); }
        reg(b, rt, sf, 0);
        if (o1) { sep(b); reg(b, bits(i, 14, 10), sf, 0); }
        put(b, ", ["); reg(b, rn, 1, 1); put(b, "]");
        return 1;
    }
    /* LDAPUR / STLUR (RCpc, unscaled) */
    if (bits(i, 29, 24) == 0x19 && !bits(i, 21, 21) && !bits(i, 11, 10)) {
        int size = bits(i, 31, 30), opc = bits(i, 23, 22);
        static const char *const nm[4][4] = { { "stlurb", "ldapurb", "ldapursb", "ldapursb" }, { "stlurh", "ldapurh", "ldapursh", "ldapursh" },
                                               { "stlur", "ldapur", "ldapursw", 0 }, { "stlur", "ldapur", 0, 0 } };
        if (!nm[size][opc]) return 0;
        int sf = size == 3 || opc == 2;
        mn(b, nm[size][opc]); reg(b, rt, sf, 0); sep(b);
        mem_imm(b, rn, sext(bits(i, 20, 12), 9), 0);
        return 1;
    }
    /* pairs */
    if (bits(i, 29, 27) == 5) {
        int opc = bits(i, 31, 30), mode = bits(i, 24, 23), l = bits(i, 22, 22), rt2 = bits(i, 14, 10);
        int scale, sf = 0;
        const char *m;
        if (v) { if (opc == 3) return 0; scale = opc + 2; m = mode == 0 ? (l ? "ldnp" : "stnp") : (l ? "ldp" : "stp"); }
        else if (opc == 0) { scale = 2; m = mode == 0 ? (l ? "ldnp" : "stnp") : (l ? "ldp" : "stp"); }
        else if (opc == 2) { scale = 3; sf = 1; m = mode == 0 ? (l ? "ldnp" : "stnp") : (l ? "ldp" : "stp"); }
        else if (opc == 1 && l && mode != 0) { scale = 2; sf = 1; m = "ldpsw"; }
        else return 0;
        a64_s64 off = sext(bits(i, 21, 15), 7) << scale;
        mn(b, m);
        if (v) { vreg(b, rt, scale); sep(b); vreg(b, rt2, scale); } else { reg(b, rt, sf, 0); sep(b); reg(b, rt2, sf, 0); }
        sep(b);
        mem_imm(b, rn, off, mode == 1 ? 2 : mode == 3 ? 1 : 0);
        return 1;
    }
    /* single registers */
    if (bits(i, 29, 27) != 7) return 0;
    int size = bits(i, 31, 30), opc = bits(i, 23, 22);
    if (bits(i, 25, 24) == 0 && bits(i, 21, 21) && bits(i, 11, 10) != 2 && !v) return ldst_atomic(i, b);
    int scale, sf = 0, kind;                                     /* kind: 0 store, 1 load, 2 prfm */
    const char *base;
    if (v) {
        scale = (opc & 2) ? 4 : size;
        if ((opc & 2) && size) return 0;
        kind = opc & 1;
        base = kind ? "ldr" : "str";
    } else {
        static const char *const names[4][4] = {
            { "strb", "ldrb", "ldrsb", "ldrsb" }, { "strh", "ldrh", "ldrsh", "ldrsh" },
            { "str", "ldr", "ldrsw", 0 }, { "str", "ldr", "prfm", 0 } };
        base = names[size][opc];
        if (!base) return 0;
        scale = size;
        kind = size == 3 && opc == 2 ? 2 : opc != 0;
        sf = size == 3 ? 1 : (opc == 2);                         /* ldrsb/ldrsh: x for opc 2, w for 3; ldrsw: x */
        if (size == 2 && opc == 2) sf = 1;
    }
    #define RT() do { if (kind == 2) prfop(b, rt); else if (v) vreg(b, rt, scale); else reg(b, rt, sf, 0); } while (0)
    if (bits(i, 25, 24) == 1) {                                  /* unsigned offset */
        mn(b, base); RT(); sep(b);
        mem_imm(b, rn, (a64_s64)bits(i, 21, 10) << scale, 0);
        return 1;
    }
    if (bits(i, 25, 24) != 0) return 0;
    if (!bits(i, 21, 21)) {
        a64_s64 imm9 = sext(bits(i, 20, 12), 9);
        int k = bits(i, 11, 10);
        if (k == 0 || k == 2) {                                  /* unscaled, unprivileged */
            char m[16]; buf_t mb = { m, 0, sizeof m };
            const char *bn = base;
            if (kind == 2) bn = k == 0 ? "prfum" : 0;
            if (!bn) return 0;
            if (kind == 2) put(&mb, bn);
            else { put(&mb, bn[0] == 'l' ? "ld" : "st"); put(&mb, k == 0 ? "ur" : "tr"); put(&mb, bn + 3); }
            if (k == 2 && v) return 0;
            mn(b, m); RT(); sep(b);
            mem_imm(b, rn, imm9, 0);
            return 1;
        }
        if (kind == 2) return 0;
        mn(b, base); RT(); sep(b);
        mem_imm(b, rn, imm9, k == 1 ? 2 : 1);
        return 1;
    }
    if (bits(i, 11, 10) != 2) return 0;
    int rm = bits(i, 20, 16), option = bits(i, 15, 13), s = bits(i, 12, 12);
    if (!(option & 2)) return 0;
    mn(b, base); RT(); put(b, ", ["); reg(b, rn, 1, 1); sep(b);
    reg(b, rm, option & 1, 0);
    if (option == 3) { if (s) { put(b, ", lsl #"); putdec(b, (a64_u64)scale); } }
    else { sep(b); put(b, extends[option]); if (s) { put(b, " #"); putdec(b, (a64_u64)scale); } }
    put(b, "]");
    return 1;
    #undef RT
}

/* ── data processing, register ── */
static void shift_suffix(buf_t *b, int sh, int amt) { if (amt || sh) { sep(b); put(b, shifts[sh]); put(b, " #"); putdec(b, (a64_u64)amt); } }
static int dp_reg(a64_u32 i, buf_t *b)
{
    int sf = bits(i, 31, 31), rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    if (bits(i, 28, 24) == 0x0a) {                               /* logical, shifted register */
        int opc = bits(i, 30, 29), sh = bits(i, 23, 22), n = bits(i, 21, 21), amt = bits(i, 15, 10);
        if (!sf && amt > 31) return 0;
        static const char *const m[8] = { "and", "bic", "orr", "orn", "eor", "eon", "ands", "bics" };
        if (opc == 1 && rn == 31 && !n && !sh && !amt) { mn(b, "mov"); reg(b, rd, sf, 0); sep(b); reg(b, rm, sf, 0); return 1; }
        if (opc == 1 && rn == 31 && n) { mn(b, "mvn"); reg(b, rd, sf, 0); sep(b); reg(b, rm, sf, 0); shift_suffix(b, sh, amt); return 1; }
        if (opc == 3 && !n && rd == 31) { mn(b, "tst"); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0); shift_suffix(b, sh, amt); return 1; }
        mn(b, m[opc * 2 + n]); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0); shift_suffix(b, sh, amt);
        return 1;
    }
    if (bits(i, 28, 24) == 0x0b && !bits(i, 21, 21)) {           /* add/sub, shifted register */
        int op = bits(i, 30, 30), s = bits(i, 29, 29), sh = bits(i, 23, 22), amt = bits(i, 15, 10);
        if (sh == 3 || (!sf && amt > 31)) return 0;
        if (s && rd == 31) { mn(b, op ? "cmp" : "cmn"); reg(b, rn, sf, 0); }
        else if (op && rn == 31) { mn(b, s ? "negs" : "neg"); reg(b, rd, sf, 0); }
        else { mn(b, op ? (s ? "subs" : "sub") : (s ? "adds" : "add")); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); }
        sep(b); reg(b, rm, sf, 0); shift_suffix(b, sh, amt);
        return 1;
    }
    if (bits(i, 28, 24) == 0x0b && bits(i, 21, 21)) {            /* add/sub, extended register */
        int op = bits(i, 30, 30), s = bits(i, 29, 29), option = bits(i, 15, 13), imm3 = bits(i, 12, 10);
        if (bits(i, 23, 22) || imm3 > 4) return 0;
        int rd_sp = !s;
        if (s && rd == 31) { mn(b, op ? "cmp" : "cmn"); reg(b, rn, sf, 1); }
        else { mn(b, op ? (s ? "subs" : "sub") : (s ? "adds" : "add")); reg(b, rd, sf, rd_sp); sep(b); reg(b, rn, sf, 1); }
        sep(b);
        reg(b, rm, sf && (option & 3) == 3, 0);
        int lsl = (rd == 31 && rd_sp) || rn == 31;
        if (lsl && option == (sf ? 3 : 2)) { if (imm3) { put(b, ", lsl #"); putdec(b, (a64_u64)imm3); } }
        else { sep(b); put(b, extends[option]); if (imm3) { put(b, " #"); putdec(b, (a64_u64)imm3); } }
        return 1;
    }
    if (bits(i, 28, 21) == 0xd0) {                               /* with carry */
        int op = bits(i, 30, 30), s = bits(i, 29, 29);
        if (bits(i, 15, 10)) return 0;
        if (op && rn == 31) { mn(b, s ? "ngcs" : "ngc"); reg(b, rd, sf, 0); sep(b); reg(b, rm, sf, 0); return 1; }
        mn(b, op ? (s ? "sbcs" : "sbc") : (s ? "adcs" : "adc")); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0);
        return 1;
    }
    if (bits(i, 28, 21) == 0xd2) {                               /* conditional compare */
        int op = bits(i, 30, 30), s = bits(i, 29, 29), cond = bits(i, 15, 12), imm = bits(i, 11, 11);
        if (!s || bits(i, 10, 10) || bits(i, 4, 4)) return 0;
        mn(b, op ? "ccmp" : "ccmn"); reg(b, rn, sf, 0); sep(b);
        if (imm) imm_hex(b, rm); else reg(b, rm, sf, 0);
        sep(b); imm_hex(b, bits(i, 3, 0)); sep(b); put(b, conds[cond]);
        return 1;
    }
    if (bits(i, 28, 21) == 0xd4) {                               /* conditional select */
        int op = bits(i, 30, 30), op2 = bits(i, 11, 10), cond = bits(i, 15, 12);
        if (bits(i, 29, 29) || op2 > 1) return 0;
        int inv = cond ^ 1, real = cond < 14;
        if (op2 == 1 && rn == rm && real) {
            if (rn == 31) { mn(b, op ? "csetm" : "cset"); reg(b, rd, sf, 0); }
            else { mn(b, op ? "cneg" : "cinc"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); }
            sep(b); put(b, conds[inv]); return 1;
        }
        if (op && !op2 && rn == rm && real) {
            if (rn == 31) { mn(b, "csetm"); reg(b, rd, sf, 0); }
            else { mn(b, "cinv"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); }
            sep(b); put(b, conds[inv]); return 1;
        }
        static const char *const m[4] = { "csel", "csinc", "csinv", "csneg" };
        mn(b, m[op * 2 + op2]); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0); sep(b); put(b, conds[cond]);
        return 1;
    }
    if (bits(i, 28, 21) == 0xd6) {                               /* 1 and 2 sources */
        int opcode = bits(i, 15, 10);
        if (bits(i, 30, 30)) {
            if (rm || bits(i, 29, 29)) return 0;
            const char *m = 0;
            if (opcode == 0) m = "rbit"; else if (opcode == 1) m = "rev16";
            else if (opcode == 2) m = sf ? "rev32" : "rev"; else if (opcode == 3 && sf) m = "rev";
            else if (opcode == 4) m = "clz"; else if (opcode == 5) m = "cls";
            if (!m) return 0;
            mn(b, m); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); return 1;
        }
        const char *m = 0;
        if ((opcode & 0x38) == 0x10) {                           /* CRC32{C}{B,H,W,X} */
            int sz = opcode & 3, c = (opcode >> 2) & 1;
            static const char *const cn[2][4] = { { "crc32b", "crc32h", "crc32w", "crc32x" }, { "crc32cb", "crc32ch", "crc32cw", "crc32cx" } };
            if ((sz == 3) != sf || bits(i, 29, 29)) return 0;
            mn(b, cn[c][sz]); reg(b, rd, 0, 0); sep(b); reg(b, rn, 0, 0); sep(b); reg(b, rm, sz == 3, 0);
            return 1;
        }
        if (opcode == 2) m = "udiv"; else if (opcode == 3) m = "sdiv";
        else if (opcode >= 8 && opcode <= 11) m = shifts[opcode - 8];
        if (!m || bits(i, 29, 29)) return 0;
        mn(b, m); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0);
        return 1;
    }
    if (bits(i, 28, 24) == 0x1b) {                               /* 3 sources */
        int op31 = bits(i, 23, 21), o0 = bits(i, 15, 15), ra = bits(i, 14, 10);
        if (bits(i, 30, 29)) return 0;
        if (op31 == 0) {
            if (ra == 31) { mn(b, o0 ? "mneg" : "mul"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0); return 1; }
            mn(b, o0 ? "msub" : "madd"); reg(b, rd, sf, 0); sep(b); reg(b, rn, sf, 0); sep(b); reg(b, rm, sf, 0); sep(b); reg(b, ra, sf, 0);
            return 1;
        }
        if (!sf) return 0;
        if (op31 == 2 || op31 == 6) { if (o0) return 0; mn(b, op31 == 2 ? "smulh" : "umulh"); reg(b, rd, 1, 0); sep(b); reg(b, rn, 1, 0); sep(b); reg(b, rm, 1, 0); return 1; }
        if (op31 != 1 && op31 != 5) return 0;
        int u = op31 == 5;
        if (ra == 31) {
            mn(b, u ? (o0 ? "umnegl" : "umull") : (o0 ? "smnegl" : "smull"));
            reg(b, rd, 1, 0); sep(b); reg(b, rn, 0, 0); sep(b); reg(b, rm, 0, 0); return 1;
        }
        mn(b, u ? (o0 ? "umsubl" : "umaddl") : (o0 ? "smsubl" : "smaddl"));
        reg(b, rd, 1, 0); sep(b); reg(b, rn, 0, 0); sep(b); reg(b, rm, 0, 0); sep(b); reg(b, ra, 1, 0);
        return 1;
    }
    return 0;
}

#include "a64_simd.inc"

int a64_disasm(a64_u32 insn, a64_u64 pc, char *out, unsigned size)
{
    buf_t b = { out, 0, size };
    out[0] = 0;
    a64_u32 op0 = bits(insn, 28, 25);
    int ok = 0;
    if (bits(insn, 31, 16) == 0) { put(&b, "udf\t"); imm_uhex(&b, bits(insn, 15, 0)); return 1; }
    if ((op0 & 0xe) == 0x8) ok = dp_imm(insn, pc, &b);
    else if ((op0 & 0xe) == 0xa) ok = branch_sys(insn, pc, &b);
    else if ((op0 & 0x5) == 0x4) ok = ldst(insn, pc, &b);
    else if ((op0 & 0x7) == 0x5) ok = dp_reg(insn, &b);
    else if ((op0 & 0x7) == 0x7) ok = simd_dp(insn, pc, &b);
    if (ok) return 1;
    b.n = 0; out[0] = 0;
    put(&b, ".inst\t"); puthex(&b, insn);
    return 0;
}

/* ══ The assembler: one line in the syntax above ═════════════════════════ */
static int strcmp_a(const char *a, const char *b);
static unsigned strlen_a(const char *s);
typedef struct {
    int kind;                       /* O_REG, O_VREG, O_IMM, O_MEM, O_SHIFT, O_EXT, O_COND, O_NAME */
    int r, sf, sp, zr;              /* register: number, 64-bit, it was sp/wsp, it was xzr/wzr */
    int vscale;                     /* FP/SIMD register: 0 b, 1 h, 2 s, 3 d, 4 q */
    a64_s64 imm;
    int sh, amt, has_amt;           /* shift/extend: which, amount */
    int base, mode;                 /* memory: base register; mode 0 offset, 1 pre, 2 post, 3 register */
    a64_s64 off;
    int idx, idx_sf, ext, ext_amt, ext_has;   /* register offset */
    int arr, cnt, eidx;             /* vector: arrangement (size*2+q); list: how many; element/list: index (-1 none) */
    char name[32];
} opnd_t;
enum { O_REG = 1, O_VREG, O_IMM, O_MEM, O_SHIFT, O_EXT, O_COND, O_NAME, O_VEC, O_VELEM, O_VLIST, O_FIMM };

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int streq_ci(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return !*a && !*b; }
static int is_sp(char c) { return c == ' ' || c == '\t'; }
static void skipsp(const char **p) { while (is_sp(**p)) (*p)++; }

/* #n is decimal (0x for hex); a number without # is an address, hex the DEBUG way (0x optional) */
static int parse_num(const char **p, a64_s64 *v)
{
    const char *s = *p; int neg = 0, hash = *s == '#';
    if (*s == '#') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    a64_u64 x = 0; int any = 0;
    if (s[0] == '0' && lower(s[1]) == 'x') {
        s += 2;
        for (;; s++) {
            int c = lower(*s), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) break;
            x = x * 16 + (a64_u64)d; any = 1;
        }
    } else if (!hash) {
        for (;; s++) {
            int c = lower(*s), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) break;
            x = x * 16 + (a64_u64)d; any = 1;
        }
    } else for (; *s >= '0' && *s <= '9'; s++) { x = x * 10 + (a64_u64)(*s - '0'); any = 1; }
    if (!any) return 0;
    *v = neg ? -(a64_s64)x : (a64_s64)x;
    *p = s;
    return 1;
}
/* x0..x30 w0..w30 sp wsp xzr wzr (lr, fp too); b/h/s/d/q FP/SIMD */
static int parse_reg(const char *s, opnd_t *o)
{
    char t[8]; int n = 0;
    while (s[n] && n < 7) { t[n] = (char)lower(s[n]); n++; }
    t[n] = 0;
    if (s[n]) return 0;
    o->kind = O_REG; o->sp = o->zr = 0;
    if (!strcmp_a(t, "sp")) { o->r = 31; o->sf = 1; o->sp = 1; return 1; }
    if (!strcmp_a(t, "wsp")) { o->r = 31; o->sf = 0; o->sp = 1; return 1; }
    if (!strcmp_a(t, "xzr")) { o->r = 31; o->sf = 1; o->zr = 1; return 1; }
    if (!strcmp_a(t, "wzr")) { o->r = 31; o->sf = 0; o->zr = 1; return 1; }
    if (!strcmp_a(t, "lr")) { o->r = 30; o->sf = 1; return 1; }
    if (!strcmp_a(t, "fp")) { o->r = 29; o->sf = 1; return 1; }
    const char *k = "xwbhsdq";
    int c = -1;
    for (int j = 0; k[j]; j++) if (t[0] == k[j]) c = j;
    if (c < 0 || !t[1]) return 0;
    int v = 0;
    for (int j = 1; t[j]; j++) { if (t[j] < '0' || t[j] > '9') return 0; v = v * 10 + t[j] - '0'; }
    if (v > (c < 2 ? 30 : 31)) return 0;
    o->r = v;
    if (c < 2) { o->sf = c == 0; return 1; }
    o->kind = O_VREG; o->vscale = c - 2;
    return 1;
}
static int strcmp_a(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }

static int find_name(const char *const *tab, int n, const char *s) { for (int i = 0; i < n; i++) if (tab[i] && streq_ci(tab[i], s)) return i; return -1; }

/* one operand (s: trimmed text) */
static int parse_operand(const char *s, opnd_t *o, char *err, unsigned es);
static void seterr(char *err, unsigned es, const char *m) { unsigned i = 0; for (; m[i] && i + 1 < es; i++) err[i] = m[i]; if (es) err[i] = 0; }

static int parse_shift_ext(const char *s, opnd_t *o)
{
    char w[8]; int n = 0;
    while (s[n] && !is_sp(s[n]) && s[n] != '#' && n < 7) { w[n] = (char)lower(s[n]); n++; }
    w[n] = 0;
    int sh = find_name(shifts, 4, w), ex = find_name(extends, 8, w);
    if (streq_ci(w, "msl")) sh = 4;                          /* msl: movi's shifting forms */
    if (sh < 0 && ex < 0) return 0;
    const char *p = s + n;
    skipsp(&p);
    a64_s64 v = 0; int has = 0;
    if (*p) { if (!parse_num(&p, &v)) return 0; has = 1; skipsp(&p); if (*p) return 0; }
    if (sh >= 0) { if (!has) return 0; o->kind = O_SHIFT; o->sh = sh; o->amt = (int)v; o->has_amt = 1; return 1; }
    o->kind = O_EXT; o->sh = ex; o->amt = (int)v; o->has_amt = has;
    return 1;
}
/* v<n>.<arrangement> or v<n>.<b|h|s|d>[<index>] (after: what follows) */
static int parse_vec(const char *s, opnd_t *o, const char **after)
{
    if (lower(s[0]) != 'v') return 0;
    int r = 0, d = 0; const char *p = s + 1;
    while (*p >= '0' && *p <= '9') { r = r * 10 + (*p - '0'); p++; d++; }
    if (!d || r > 31 || *p != '.') return 0;
    p++;
    o->r = r;
    char t[4]; int n = 0;
    while (*p && *p != '[' && *p != ',' && *p != ' ' && *p != '}' && n < 3) t[n++] = (char)lower(*p++);
    t[n] = 0;
    if (*p == '[') {                                             /* an element */
        int es = t[0] == 'b' ? 0 : t[0] == 'h' ? 1 : t[0] == 's' ? 2 : t[0] == 'd' ? 3 : -1;
        if (es < 0 || t[1]) return 0;
        p++;
        int ix = 0, dd = 0;
        while (*p >= '0' && *p <= '9') { ix = ix * 10 + (*p - '0'); p++; dd++; }
        if (!dd || *p != ']') return 0;
        p++;
        o->kind = O_VELEM; o->vscale = es; o->eidx = ix;
        *after = p; return 1;
    }
    static const char *const an[8] = { "8b", "16b", "4h", "8h", "2s", "4s", "1d", "2d" };
    int a = -1;
    for (int k = 0; k < 8; k++) { const char *x = an[k], *y = t; while (*x && *x == *y) { x++; y++; } if (!*x && !*y) a = k; }
    if (t[0] == '1' && t[1] == 'q' && !t[2]) a = 8;              /* pmull's 128-bit result */
    if (a < 0) {                                                 /* in a single-structure list: v0.s with no lane count */
        int es = !t[1] ? (t[0] == 'b' ? 0 : t[0] == 'h' ? 1 : t[0] == 's' ? 2 : t[0] == 'd' ? 3 : -1) : -1;
        if (es < 0) return 0;
        o->kind = O_VELEM; o->vscale = es; o->eidx = -1;
        *after = p; return 1;
    }
    o->kind = O_VEC; o->arr = a;
    *after = p; return 1;
}
/* "#1.25", "#-0.328125" as value * 10^8 (exact for every FP8 immediate) */
static int parse_fimm(const char *s, a64_s64 *v)
{
    if (*s != '#') return 0;
    s++;
    int neg = 0; if (*s == '-') { neg = 1; s++; }
    a64_s64 ip = 0, fp = 0; int nd = 0, dig = 0;
    while (*s >= '0' && *s <= '9') { ip = ip * 10 + (*s - '0'); s++; dig = 1; }
    if (*s != '.') return 0;
    s++;
    while (*s >= '0' && *s <= '9') { if (nd < 8) { fp = fp * 10 + (*s - '0'); nd++; } s++; dig = 1; }
    while (*s == ' ') s++;
    if (*s || !dig) return 0;
    while (nd < 8) { fp *= 10; nd++; }
    *v = (ip * 100000000 + fp) * (neg ? -1 : 1);
    return 1;
}
static int parse_operand(const char *s, opnd_t *o, char *err, unsigned es)
{
    for (unsigned k = 0; k < sizeof *o; k++) ((char *)o)[k] = 0;
    o->eidx = -1;
    if (*s == '{') {                                             /* a register list */
        const char *p = s + 1;
        opnd_t first;
        int n = 0;
        for (;;) {
            while (*p == ' ') p++;
            opnd_t v; const char *a;
            for (unsigned k = 0; k < sizeof v; k++) ((char *)&v)[k] = 0;
            if (!parse_vec(p, &v, &a)) { seterr(err, es, "a register list: { v0.16b, v1.16b }"); return 0; }
            if (!n) first = v;
            else if (v.r != ((first.r + n) & 31) || v.kind != first.kind || v.arr != first.arr || v.vscale != first.vscale) { seterr(err, es, "a list's registers: one after another, alike"); return 0; }
            n++;
            p = a;
            while (*p == ' ') p++;
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            seterr(err, es, "a register list: { v0.16b, v1.16b }"); return 0;
        }
        if (n > 4) { seterr(err, es, "at most four registers in a list"); return 0; }
        o->kind = O_VLIST; o->r = first.r; o->cnt = n; o->arr = first.kind == O_VEC ? first.arr : -1; o->vscale = first.vscale;
        while (*p == ' ') p++;
        if (*p == '[') {
            p++; int ix = 0, d = 0;
            while (*p >= '0' && *p <= '9') { ix = ix * 10 + (*p - '0'); p++; d++; }
            if (!d || *p != ']') { seterr(err, es, "a bad [index]"); return 0; }
            p++; o->eidx = ix;
        }
        while (*p == ' ') p++;
        if (*p) { seterr(err, es, "something after the list"); return 0; }
        return 1;
    }
    {
        const char *a;
        if (lower(s[0]) == 'v' && parse_vec(s, o, &a)) {
            if (*a) { seterr(err, es, "something after the register"); return 0; }
            return 1;
        }
    }
    if (s[0] == '#') { a64_s64 fv; if (parse_fimm(s, &fv)) { o->kind = O_FIMM; o->imm = fv; return 1; } }
    if (*s == '[') {                                             /* memory */
        o->kind = O_MEM;
        char in[64]; int n = 0;
        const char *p = s + 1;
        while (*p && *p != ']' && n < 63) in[n++] = *p++;
        in[n] = 0;
        if (*p != ']') { seterr(err, es, "a [ without its ]"); return 0; }
        p++;
        /* parts of the inside, by commas */
        char *part[4]; int np = 0; char *q = in;
        part[np++] = q;
        for (; *q; q++) if (*q == ',' && np < 4) { *q = 0; part[np++] = q + 1; }
        for (int j = 0; j < np; j++) { while (is_sp(*part[j])) part[j]++; char *e = part[j] + strlen_a(part[j]); while (e > part[j] && is_sp(e[-1])) *--e = 0; }
        opnd_t b;
        if (!parse_reg(part[0], &b) || b.kind != O_REG || !b.sf || b.zr) { seterr(err, es, "a base register (x0-x30, sp)"); return 0; }
        o->base = b.r;
        if (np >= 2) {
            const char *v = part[1];
            if (*v == '#' || *v == '-' || (*v >= '0' && *v <= '9')) {
                if (!parse_num(&v, &o->off) || *v) { seterr(err, es, "a bad offset"); return 0; }
                if (np > 2) { seterr(err, es, "too much inside the []"); return 0; }
            } else {
                opnd_t ix;
                if (!parse_reg(part[1], &ix) || ix.kind != O_REG || ix.sp) { seterr(err, es, "an index register"); return 0; }
                o->mode = 3; o->idx = ix.r; o->idx_sf = ix.sf; o->ext = ix.sf ? 3 : -1; o->ext_has = 0;
                if (np == 3) {
                    opnd_t se;
                    if (!parse_shift_ext(part[2], &se)) { seterr(err, es, "lsl #n, or an extend (uxtw, sxtw, sxtx)"); return 0; }
                    if (se.kind == O_SHIFT) { if (se.sh != 0) { seterr(err, es, "only lsl, for an index"); return 0; } o->ext = 3; }
                    else o->ext = se.sh;
                    o->ext_amt = se.amt; o->ext_has = se.kind == O_SHIFT || se.has_amt;
                }
                if (o->ext < 0) { seterr(err, es, "a w index needs uxtw or sxtw"); return 0; }
            }
        }
        skipsp(&p);
        if (*p == '!') { if (o->mode == 3) { seterr(err, es, "no writeback with an index register"); return 0; } o->mode = 1; p++; }
        skipsp(&p);
        if (*p) { seterr(err, es, "something after the ]"); return 0; }
        return 1;
    }
    if (*s == '#' || *s == '-' || (*s >= '0' && *s <= '9')) {
        const char *p = s;
        if (!parse_num(&p, &o->imm) || *p) { seterr(err, es, "a bad number"); return 0; }
        o->kind = O_IMM;
        return 1;
    }
    if (parse_reg(s, o)) return 1;
    if (parse_shift_ext(s, o)) return 1;
    int c = find_name(conds, 16, s);
    if (c >= 0) { o->kind = O_COND; o->imm = c; return 1; }
    unsigned n = 0;
    while (s[n] && n < sizeof o->name - 1) { o->name[n] = s[n]; n++; }
    o->name[n] = 0;
    o->kind = O_NAME;
    return 1;
}
static unsigned strlen_a(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

/* logical immediate -> N:immr:imms (-1: not encodable) */
static int encode_bitmask(a64_u64 v, int sf)
{
    if (!sf) { v &= 0xffffffffull; v |= v << 32; }
    if (v == 0 || v == ~0ull) return -1;
    int size = 64;
    while (size > 2) {
        int h = size / 2;
        a64_u64 m = (1ull << h) - 1;
        if ((v & m) != ((v >> h) & m)) break;
        size = h;
    }
    a64_u64 mask = size == 64 ? ~0ull : (1ull << size) - 1;
    a64_u64 e = v & mask;
    /* e: a rotated run of ones */
    int ones = 0; for (int k = 0; k < size; k++) if ((e >> k) & 1) ones++;
    for (int r = 0; r < size; r++) {
        a64_u64 run = ones == 64 ? ~0ull : (1ull << ones) - 1;
        if (ror_w(run, r, size) == e) {
            int n = size == 64, imms = ((~(size - 1) << 1) & 0x3f) | (ones - 1);
            if (size == 64) imms = ones - 1;
            return (n << 12) | (r << 6) | (imms & 0x3f);
        }
    }
    return -1;
}

static a64_u32 R(int r) { return (a64_u32)r & 31; }

/* the prefetch operation by name */
static int prf_by_name(const char *s)
{
    for (int t = 0; t < 24; t++) { char nm[16]; buf_t b = { nm, 0, sizeof nm }; prfop(&b, t); if (streq_ci(nm, s)) return t; }
    if (streq_ci(s, "ir")) return 24;
    return -1;
}

static int sysreg_by_name(const char *s, a64_u32 *key)
{
    for (unsigned i = 0; i < sizeof a64_sysregs / sizeof a64_sysregs[0]; i++) if (streq_ci(a64_sysregs[i].name, s)) { *key = 0x8000u | a64_sysregs[i].key; return 1; }
    /* S<op0>_<op1>_C<n>_C<m>_<op2> */
    const char *p = s;
    if (lower(*p) != 's') return 0;
    p++;
    a64_s64 f[5]; int k = 0;
    while (k < 5) {
        if (k == 2 || k == 3) { if (lower(*p) != 'c') return 0; p++; }
        if (!parse_num(&p, &f[k])) return 0;
        k++;
        if (k < 5) { if (*p != '_') return 0; p++; }
    }
    if (*p || f[0] > 3 || f[1] > 7 || f[2] > 15 || f[3] > 15 || f[4] > 7) return 0;
    *key = (a64_u32)((f[0] << 14) | (f[1] << 11) | (f[2] << 7) | (f[3] << 3) | f[4]);
    return 1;
}

/* the instruction body: mnemonic m, operands o[n] */
static int assemble(const char *m, opnd_t *o, int n, a64_u64 pc, a64_u32 *out, char *err, unsigned es)
{
    #define E(msg) do { seterr(err, es, msg); return 0; } while (0)
    #define IS(x) streq_ci(m, x)
    #define REGN(k) (n > (k) && o[k].kind == O_REG)
    int sf = REGN(0) ? o[0].sf : 1;
    /* ── no operands ── */
    static const struct { const char *m; a64_u32 w; } plain[] = {
        { "nop", 0xd503201f }, { "yield", 0xd503203f }, { "wfe", 0xd503205f }, { "wfi", 0xd503207f }, { "sev", 0xd503209f },
        { "sevl", 0xd50320bf }, { "eret", 0xd69f03e0 }, { "drps", 0xd6bf03e0 }, { "clrex", 0xd503305f }, { "isb", 0xd5033fdf },
        { "ret", 0xd65f03c0 }, { "esb", 0xd503221f }, { "csdb", 0xd503229f }, { "xpaclri", 0xd50320ff },
        { "paciasp", 0xd503233f }, { "autiasp", 0xd50323bf }, { "pacibsp", 0xd503237f }, { "autibsp", 0xd50323ff },
        { "retaa", 0xd65f0bff }, { "retab", 0xd65f0fff }, { "ssbb", 0xd503309f }, { "pssbb", 0xd503349f }, { "sb", 0xd50330ff } };
    if (n == 0) {
        for (unsigned k = 0; k < sizeof plain / sizeof plain[0]; k++) if (IS(plain[k].m)) { *out = plain[k].w; return 1; }
        E("needs operands");
    }
    /* ── branches ── */
    if ((IS("b") || IS("bl")) && n == 1 && o[0].kind == O_IMM) {
        a64_s64 d = o[0].imm - (a64_s64)pc;
        if ((d & 3) || d < -(1ll << 27) || d >= (1ll << 27)) E("the target is out of reach (+-128 MB, 4-aligned)");
        *out = (IS("bl") ? 0x94000000u : 0x14000000u) | ((a64_u32)(d >> 2) & 0x3ffffff); return 1;
    }
    if ((m[0] == 'b' || m[0] == 'B') && m[1] == '.' && n == 1 && o[0].kind == O_IMM) {
        int c = find_name(conds, 16, m + 2);
        a64_s64 d = o[0].imm - (a64_s64)pc;
        if (c < 0) E("a condition: eq ne hs lo mi pl vs vc hi ls ge lt gt le al");
        if ((d & 3) || d < -(1ll << 20) || d >= (1ll << 20)) E("the target is out of reach (+-1 MB)");
        *out = 0x54000000u | (((a64_u32)(d >> 2) & 0x7ffff) << 5) | (a64_u32)c; return 1;
    }
    if ((IS("cbz") || IS("cbnz")) && n == 2 && REGN(0) && o[1].kind == O_IMM) {
        a64_s64 d = o[1].imm - (a64_s64)pc;
        if ((d & 3) || d < -(1ll << 20) || d >= (1ll << 20)) E("the target is out of reach (+-1 MB)");
        *out = (sf ? 0xb4000000u : 0x34000000u) | (IS("cbnz") ? 1u << 24 : 0) | (((a64_u32)(d >> 2) & 0x7ffff) << 5) | R(o[0].r); return 1;
    }
    if ((IS("tbz") || IS("tbnz")) && n == 3 && REGN(0) && o[1].kind == O_IMM && o[2].kind == O_IMM) {
        a64_s64 bit = o[1].imm, d = o[2].imm - (a64_s64)pc;
        if (bit < 0 || bit > (sf ? 63 : 31)) E("a bit number 0-63 (0-31 for a w register)");
        if ((d & 3) || d < -(1ll << 15) || d >= (1ll << 15)) E("the target is out of reach (+-32 KB)");
        *out = 0x36000000u | (IS("tbnz") ? 1u << 24 : 0) | ((a64_u32)(bit >> 5) << 31) | ((a64_u32)(bit & 31) << 19) | (((a64_u32)(d >> 2) & 0x3fff) << 5) | R(o[0].r); return 1;
    }
    if ((IS("br") || IS("blr") || IS("ret")) && n == 1 && REGN(0) && o[0].sf) {
        a64_u32 base = IS("br") ? 0xd61f0000u : IS("blr") ? 0xd63f0000u : 0xd65f0000u;
        *out = base | (R(o[0].r) << 5); return 1;
    }
    /* ── exceptions, system ── */
    if ((IS("svc") || IS("hvc") || IS("smc") || IS("brk") || IS("hlt") || IS("udf")) && n == 1 && o[0].kind == O_IMM) {
        if (o[0].imm < 0 || o[0].imm > 0xffff) E("a 16-bit number");
        a64_u32 v = (a64_u32)o[0].imm;
        if (IS("udf")) { *out = v; return 1; }
        a64_u32 base = IS("svc") ? 0xd4000001u : IS("hvc") ? 0xd4000002u : IS("smc") ? 0xd4000003u : IS("brk") ? 0xd4200000u : 0xd4400000u;
        *out = base | (v << 5); return 1;
    }
    if ((IS("dcps1") || IS("dcps2") || IS("dcps3")) && n <= 1) {
        a64_s64 v = n ? o[0].imm : 0;
        if ((n && o[0].kind != O_IMM) || v < 0 || v > 0xffff) E("a 16-bit number");
        *out = 0xd4a00000u | ((a64_u32)v << 5) | (a64_u32)(m[4] - '0'); return 1;
    }
    {   /* crc32{c}{b,h,w,x} wd, wn, wm (x: xm) */
        static const char *const cr[] = { "crc32b", "crc32h", "crc32w", "crc32x", "crc32cb", "crc32ch", "crc32cw", "crc32cx" };
        int k = find_name(cr, 8, m);
        if (k >= 0 && n == 3 && REGN(0) && REGN(1) && REGN(2)) {
            int sz = k & 3;
            *out = ((a64_u32)(sz == 3) << 31) | 0x1ac04000u | (R(o[2].r) << 16) | ((a64_u32)(k >> 2) << 12) | ((a64_u32)sz << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
    }
    {   /* braa(z)/brab(z)/blraa(z)/blrab(z) */
        static const char *const pb[] = { "braaz", "brabz", "blraaz", "blrabz", "braa", "brab", "blraa", "blrab" };
        int k = find_name(pb, 8, m);
        if (k >= 0 && REGN(0) && n == (k < 4 ? 1 : 2) && (k < 4 || REGN(1))) {
            a64_u32 opc = k < 4 ? (a64_u32)(k >> 1) : (a64_u32)(8 + ((k - 4) >> 1));
            *out = 0xd61f0800u | (opc << 21) | ((a64_u32)(k & 1) << 10) | (R(o[0].r) << 5) | (k < 4 ? 31u : R(o[1].r)); return 1;
        }
    }
    if (IS("hint") && n == 1 && o[0].kind == O_IMM && o[0].imm >= 0 && o[0].imm < 128) { *out = 0xd503201fu | ((a64_u32)o[0].imm << 5); return 1; }
    if ((IS("dsb") || IS("dmb") || IS("isb")) && n == 1) {
        int opt = -1;
        if (o[0].kind == O_NAME) opt = find_name(barrier_opts, 16, o[0].name);
        else if (o[0].kind == O_IMM && o[0].imm >= 0 && o[0].imm < 16) opt = (int)o[0].imm;
        if (opt < 0) E("an option: sy ish ishst ishld nsh osh ld st, or #0-15");
        *out = (IS("dsb") ? 0xd503309fu : IS("dmb") ? 0xd50330bfu : 0xd50330dfu) | ((a64_u32)opt << 8); return 1;
    }
    if (IS("mrs") && n == 2 && REGN(0) && o[1].kind == O_NAME) {
        a64_u32 key;
        if (!sysreg_by_name(o[1].name, &key)) E("a system register (by name, or S<op0>_<op1>_C<n>_C<m>_<op2>)");
        *out = 0xd5200000u | (1u << 21) | (key << 5) | R(o[0].r); return 1;
    }
    if (IS("msr") && n == 2 && o[0].kind == O_NAME && o[1].kind == O_IMM) {          /* PSTATE fields */
        static const struct { const char *n; int op1, op2; } ps[] = { { "DAIFSet", 3, 6 }, { "DAIFClr", 3, 7 }, { "SPSel", 0, 5 }, { "PAN", 0, 4 }, { "UAO", 0, 3 } };
        for (unsigned k = 0; k < 5; k++)
            if (streq_ci(o[0].name, ps[k].n)) {
                if (o[1].imm < 0 || o[1].imm > 15) E("#0-15");
                *out = 0xd500401fu | ((a64_u32)ps[k].op1 << 16) | ((a64_u32)o[1].imm << 8) | ((a64_u32)ps[k].op2 << 5); return 1;
            }
    }
    if (IS("msr") && n == 2 && o[0].kind == O_NAME && REGN(1)) {
        a64_u32 key;
        if (!sysreg_by_name(o[0].name, &key)) E("a system register (by name, or S<op0>_<op1>_C<n>_C<m>_<op2>)");
        *out = 0xd5000000u | (key << 5) | R(o[1].r); return 1;
    }
    if ((IS("dc") || IS("ic") || IS("tlbi") || IS("at")) && n >= 1 && o[0].kind == O_NAME) {
        for (unsigned k = 0; k < sizeof sysops / sizeof sysops[0]; k++) {
            const char *full = sysops[k].name; unsigned ml = strlen_a(m);
            int same = 1;
            for (unsigned c = 0; c < ml; c++) if (lower(full[c]) != lower(m[c])) same = 0;
            if (!same || full[ml] != '\t' || !streq_ci(full + ml + 1, o[0].name)) continue;
            if (sysops[k].has_rt != (n == 2)) E(sysops[k].has_rt ? "needs a register" : "takes no register");
            if (n == 2 && !(REGN(1) && o[1].sf)) E("an x register");
            *out = 0xd5080000u | (sysops[k].sys << 5) | (n == 2 ? R(o[1].r) : 31); return 1;
        }
        E("an operation this assembler doesn't know");
    }
    /* ── PC-relative ── */
    if ((IS("adr") || IS("adrp")) && n == 2 && REGN(0) && o[0].sf && o[1].kind == O_IMM) {
        a64_s64 d = IS("adrp") ? (a64_s64)((a64_u64)o[1].imm & ~0xfffull) - (a64_s64)(pc & ~0xfffull) : o[1].imm - (a64_s64)pc;
        if (IS("adrp")) { if (d & 0xfff) E("not a page"); d >>= 12; }
        if (d < -(1ll << 20) || d >= (1ll << 20)) E("out of reach");
        *out = (IS("adrp") ? 0x90000000u : 0x10000000u) | (((a64_u32)d & 3) << 29) | ((((a64_u32)d >> 2) & 0x7ffff) << 5) | R(o[0].r); return 1;
    }
    /* ── move wide, mov ── */
    if ((IS("movz") || IS("movn") || IS("movk")) && (n == 2 || n == 3) && REGN(0) && o[1].kind == O_IMM) {
        int hw = 0;
        if (n == 3) { if (o[2].kind != O_SHIFT || o[2].sh != 0 || (o[2].amt & 15) || o[2].amt > (sf ? 48 : 16)) E("lsl #0/16/32/48"); hw = o[2].amt / 16; }
        if (o[1].imm < 0 || o[1].imm > 0xffff) E("a 16-bit number");
        a64_u32 opc = IS("movn") ? 0 : IS("movz") ? 2 : 3;
        *out = ((a64_u32)sf << 31) | (opc << 29) | 0x12800000u | ((a64_u32)hw << 21) | ((a64_u32)o[1].imm << 5) | R(o[0].r); return 1;
    }
    if (IS("mov") && n == 2 && REGN(0) && o[1].kind == O_IMM) {
        a64_u64 v = (a64_u64)o[1].imm, mask = sf ? ~0ull : 0xffffffffull;
        if (!sf && (o[1].imm > 0xffffffffll || o[1].imm < -0x80000000ll)) E("doesn't fit in 32 bits");
        v &= mask;
        for (int h = 0; h < (sf ? 4 : 2); h++) if ((v & ~(0xffffull << (h * 16)) & mask) == 0 && !(v == 0 && h)) {    /* MOVZ */
            *out = ((a64_u32)sf << 31) | 0x52800000u | ((a64_u32)h << 21) | ((a64_u32)((v >> (h * 16)) & 0xffff) << 5) | R(o[0].r); return 1;
        }
        for (int h = 0; h < (sf ? 4 : 2); h++) { a64_u64 x = ~v & mask; if ((x & ~(0xffffull << (h * 16)) & mask) == 0 && !(x == 0 && h)) {   /* MOVN */
            if (!sf && ((x >> (h * 16)) & 0xffff) == 0xffff) continue;
            *out = ((a64_u32)sf << 31) | 0x12800000u | ((a64_u32)h << 21) | ((a64_u32)((x >> (h * 16)) & 0xffff) << 5) | R(o[0].r); return 1;
        } }
        int bm = encode_bitmask(v, sf);                          /* ORR (bitmask immediate) */
        if (bm < 0) E("not a value one instruction can make (movz/movn/orr): build it with mov + movk");
        *out = ((a64_u32)sf << 31) | 0x320003e0u | ((a64_u32)bm << 10) | R(o[0].r); return 1;
    }
    if (IS("mov") && n == 2 && REGN(0) && REGN(1)) {
        if (o[0].sf != o[1].sf) E("both x, or both w");
        if (o[0].sp || o[1].sp) { *out = ((a64_u32)sf << 31) | 0x11000000u | (R(o[1].r) << 5) | R(o[0].r); return 1; }   /* ADD #0 */
        *out = ((a64_u32)sf << 31) | 0x2a0003e0u | (R(o[1].r) << 16) | R(o[0].r); return 1;                          /* ORR */
    }
    /* ── add/sub/cmp/cmn/neg ── */
    {
        int op = -1, s = 0, cmp = 0, neg = 0;
        if (IS("add")) op = 0; else if (IS("adds")) { op = 0; s = 1; } else if (IS("sub")) op = 1; else if (IS("subs")) { op = 1; s = 1; }
        else if (IS("cmp")) { op = 1; s = 1; cmp = 1; } else if (IS("cmn")) { op = 0; s = 1; cmp = 1; }
        else if (IS("neg")) { op = 1; neg = 1; } else if (IS("negs")) { op = 1; s = 1; neg = 1; }
        if (op >= 0) {
            opnd_t full[4];                                      /* as rd, rn, operand[, shift] */
            int k = 0;
            if (cmp) { full[k].kind = O_REG; full[k].r = 31; full[k].sf = o[0].sf; full[k].sp = 0; full[k].zr = 1; k++; }
            for (int j = 0; j < n && k < 4; j++) {
                full[k++] = o[j];
                if (neg && j == 0) { full[k].kind = O_REG; full[k].r = 31; full[k].sf = o[0].sf; full[k].sp = 0; full[k].zr = 1; k++; }
            }
            if (k < 3 || full[0].kind != O_REG || full[1].kind != O_REG) E("rd, rn, then a register or #imm");
            sf = full[0].sf;
            if (full[1].sf != sf) E("mixed x and w registers");
            if (full[2].kind == O_IMM) {                         /* immediate */
                a64_s64 v = full[2].imm; int sh = 0;
                if (k == 4) { if (full[3].kind != O_SHIFT || full[3].sh || (full[3].amt != 0 && full[3].amt != 12)) E("lsl #0 or #12"); sh = full[3].amt == 12; }
                if (v < 0 || v > 0xfff) { if (!sh && v >= 0 && !(v & 0xfff) && v <= 0xfff000) { v >>= 12; sh = 1; } else E("a 12-bit immediate (optionally lsl #12)"); }
                if (full[1].zr || (!s && full[0].zr)) E("sp here, not the zero register");
                *out = ((a64_u32)sf << 31) | ((a64_u32)op << 30) | ((a64_u32)s << 29) | 0x11000000u | ((a64_u32)sh << 22) | ((a64_u32)v << 10) | (R(full[1].r) << 5) | R(full[0].r);
                return 1;
            }
            if (full[2].kind != O_REG) E("a register or #imm");
            int ext = k == 4 && full[3].kind == O_EXT;
            int with_sp = (!s && full[0].sp) || full[1].sp;
            if (ext || with_sp || (full[2].sf != sf)) {          /* extended register */
                int option = ext ? full[3].sh : (sf ? 3 : 2), amt = ext ? full[3].amt : 0;
                if (k == 4 && full[3].kind == O_SHIFT) { if (full[3].sh) E("lsl only, with sp"); amt = full[3].amt; }
                if (amt > 4) E("an extend's shift is 0-4");
                if (neg) E("neg takes no extend");
                *out = ((a64_u32)sf << 31) | ((a64_u32)op << 30) | ((a64_u32)s << 29) | 0x0b200000u | (R(full[2].r) << 16) | ((a64_u32)option << 13) | ((a64_u32)amt << 10) | (R(full[1].r) << 5) | R(full[0].r);
                return 1;
            }
            int sh = 0, amt = 0;
            if (k == 4) { if (full[3].kind != O_SHIFT || full[3].sh == 3) E("lsl, lsr or asr"); sh = full[3].sh; amt = full[3].amt; }
            if (amt < 0 || amt > (sf ? 63 : 31)) E("a shift 0-63 (0-31)");
            *out = ((a64_u32)sf << 31) | ((a64_u32)op << 30) | ((a64_u32)s << 29) | 0x0b000000u | ((a64_u32)sh << 22) | (R(full[2].r) << 16) | ((a64_u32)amt << 10) | (R(full[1].r) << 5) | R(full[0].r);
            return 1;
        }
    }
    /* ── logical: and orr eor ands bic orn eon bics tst mvn ── */
    {
        static const char *const lg[8] = { "and", "bic", "orr", "orn", "eor", "eon", "ands", "bics" };
        int li = find_name(lg, 8, m), tst = IS("tst"), mvn = IS("mvn");
        if (li >= 0 || tst || mvn) {
            opnd_t full[4]; int k = 0;
            if (tst) { li = 6; full[k].kind = O_REG; full[k].r = 31; full[k].sf = o[0].sf; full[k].sp = 0; full[k].zr = 1; k++; }
            for (int j = 0; j < n && k < 4; j++) {
                full[k++] = o[j];
                if (mvn && j == 0) { li = 3; full[k].kind = O_REG; full[k].r = 31; full[k].sf = o[0].sf; full[k].sp = 0; full[k].zr = 1; k++; }
            }
            if (k < 3 || full[0].kind != O_REG || full[1].kind != O_REG) E("rd, rn, then a register or #imm");
            sf = full[0].sf;
            if (full[2].kind == O_IMM) {
                if (li & 1) E("no immediate form (bic/orn/eon/bics)");
                int bm = encode_bitmask((a64_u64)full[2].imm, sf);
                if (bm < 0 || (!sf && (full[2].imm > 0xffffffffll || full[2].imm < -0x80000000ll))) E("not a logical immediate (a repeated, rotated run of ones)");
                int opc = li / 2;
                *out = ((a64_u32)sf << 31) | ((a64_u32)opc << 29) | 0x12000000u | ((a64_u32)bm << 10) | (R(full[1].r) << 5) | R(full[0].r);
                return 1;
            }
            if (full[2].kind != O_REG || full[2].sf != sf || full[1].sf != sf) E("registers of one size");
            int sh = 0, amt = 0;
            if (k == 4) { if (full[3].kind != O_SHIFT) E("a shift"); sh = full[3].sh; amt = full[3].amt; }
            if (amt < 0 || amt > (sf ? 63 : 31)) E("a shift 0-63 (0-31)");
            *out = ((a64_u32)sf << 31) | ((a64_u32)(li / 2) << 29) | 0x0a000000u | ((a64_u32)sh << 22) | ((a64_u32)(li & 1) << 21) | (R(full[2].r) << 16) | ((a64_u32)amt << 10) | (R(full[1].r) << 5) | R(full[0].r);
            return 1;
        }
    }
    /* ── shifts: lsl lsr asr ror (immediate: bitfield / extr; register: the v forms) ── */
    if ((IS("lsl") || IS("lsr") || IS("asr") || IS("ror")) && n == 3 && REGN(0) && REGN(1)) {
        int size = sf ? 64 : 32;
        if (o[2].kind == O_REG) {
            int k = IS("lsl") ? 8 : IS("lsr") ? 9 : IS("asr") ? 10 : 11;
            *out = ((a64_u32)sf << 31) | 0x1ac00000u | (R(o[2].r) << 16) | ((a64_u32)k << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        if (o[2].kind != O_IMM || o[2].imm < 0 || o[2].imm >= size) E("a shift 0-63 (0-31)");
        int a = (int)o[2].imm, nb = sf;
        if (IS("ror")) { *out = ((a64_u32)sf << 31) | 0x13800000u | ((a64_u32)nb << 22) | (R(o[1].r) << 16) | ((a64_u32)a << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1; }
        int immr, imms, opc = IS("asr") ? 0 : 2;
        if (IS("lsl")) { immr = (size - a) % size; imms = size - 1 - a; } else { immr = a; imms = size - 1; }
        *out = ((a64_u32)sf << 31) | ((a64_u32)opc << 29) | 0x13000000u | ((a64_u32)nb << 22) | ((a64_u32)immr << 16) | ((a64_u32)imms << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
    }
    /* ── bitfield ── */
    {
        static const char *const bf[] = { "ubfx", "sbfx", "ubfiz", "sbfiz", "bfi", "bfxil" };
        int k = find_name(bf, 6, m);
        if (k >= 0 && n == 4 && REGN(0) && REGN(1) && o[2].kind == O_IMM && o[3].kind == O_IMM) {
            int size = sf ? 64 : 32, lsb = (int)o[2].imm, w = (int)o[3].imm;
            if (lsb < 0 || w < 1 || lsb + w > size) E("lsb and width inside the register");
            int opc = (k == 0 || k == 2) ? 2 : (k == 1 || k == 3) ? 0 : 1, immr, imms;
            if (k == 0 || k == 1 || k == 5) { immr = lsb; imms = lsb + w - 1; } else { immr = (size - lsb) % size; imms = w - 1; }
            *out = ((a64_u32)sf << 31) | ((a64_u32)opc << 29) | 0x13000000u | ((a64_u32)sf << 22) | ((a64_u32)immr << 16) | ((a64_u32)imms << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        static const char *const xt[] = { "uxtb", "uxth", "sxtb", "sxth", "sxtw" };
        k = find_name(xt, 5, m);
        if (k >= 0 && n == 2 && REGN(0) && REGN(1)) {
            int imms = k == 0 || k == 2 ? 7 : k == 1 || k == 3 ? 15 : 31, opc = k < 2 ? 2 : 0;
            if (k < 2 && sf) E("uxtb/uxth: w registers");
            if (k == 4 && !sf) E("sxtw: an x destination");
            *out = ((a64_u32)sf << 31) | ((a64_u32)opc << 29) | 0x13000000u | ((a64_u32)sf << 22) | ((a64_u32)imms << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
    }
    /* ── multiply, divide, conditional select ── */
    {
        static const char *const m3[] = { "madd", "msub", "mul", "mneg", "udiv", "sdiv", "umulh", "smulh", "umull", "smull", "umaddl", "smaddl" };
        int k = find_name(m3, 12, m);
        if (k >= 0 && n >= 3 && REGN(0) && REGN(1) && REGN(2)) {
            int ra = n == 4 && REGN(3) ? o[3].r : 31;
            if (k <= 3) { if ((k < 2) != (n == 4)) E("madd/msub: four registers; mul/mneg: three");
                *out = ((a64_u32)sf << 31) | 0x1b000000u | (R(o[2].r) << 16) | ((a64_u32)(k & 1) << 15) | (R(ra) << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1; }
            if (k <= 5) { *out = ((a64_u32)sf << 31) | 0x1ac00000u | (R(o[2].r) << 16) | ((a64_u32)(k == 4 ? 2 : 3) << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1; }
            if (k <= 7) { *out = 0x9b407c00u | (k == 6 ? 1u << 23 : 0) | (R(o[2].r) << 16) | (R(o[1].r) << 5) | R(o[0].r); return 1; }
            *out = 0x9b200000u | ((k == 8 || k == 10) ? 1u << 23 : 0) | (R(o[2].r) << 16) | (R(ra) << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        static const char *const cs[] = { "csel", "csinc", "csinv", "csneg" };
        k = find_name(cs, 4, m);
        if (k >= 0 && n == 4 && REGN(0) && REGN(1) && REGN(2) && o[3].kind == O_COND) {
            *out = ((a64_u32)sf << 31) | ((a64_u32)(k >> 1) << 30) | 0x1a800000u | (R(o[2].r) << 16) | ((a64_u32)o[3].imm << 12) | ((a64_u32)(k & 1) << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        if ((IS("cset") || IS("csetm")) && n == 2 && REGN(0) && o[1].kind == O_COND) {
            *out = ((a64_u32)sf << 31) | (IS("csetm") ? 0x5a800000u : 0x1a800400u) | (31u << 16) | ((a64_u32)(o[1].imm ^ 1) << 12) | (31u << 5) | R(o[0].r);
            if (IS("csetm")) *out |= 0; return 1;
        }
        if ((IS("cinc") || IS("cinv") || IS("cneg")) && n == 3 && REGN(0) && REGN(1) && o[2].kind == O_COND) {
            a64_u32 base = IS("cinc") ? 0x1a800400u : IS("cinv") ? 0x5a800000u : 0x5a800400u;
            *out = ((a64_u32)sf << 31) | base | (R(o[1].r) << 16) | ((a64_u32)(o[2].imm ^ 1) << 12) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
    }
    /* ── conditional compare, 1-source, with carry, extr, bfc ── */
    if ((IS("ccmp") || IS("ccmn")) && n == 4 && REGN(0) && o[2].kind == O_IMM && o[3].kind == O_COND) {
        if (o[2].imm < 0 || o[2].imm > 15) E("nzcv: #0-15");
        a64_u32 base = ((a64_u32)sf << 31) | (IS("ccmp") ? 1u << 30 : 0) | 0x3a400000u | ((a64_u32)o[3].imm << 12) | (R(o[0].r) << 5) | (a64_u32)o[2].imm;
        if (o[1].kind == O_IMM) { if (o[1].imm < 0 || o[1].imm > 31) E("#0-31"); *out = base | (1u << 11) | ((a64_u32)o[1].imm << 16); return 1; }
        if (REGN(1)) { *out = base | (R(o[1].r) << 16); return 1; }
    }
    {
        static const char *const one[] = { "rbit", "rev16", "rev32", "rev", "clz", "cls" };
        int k = find_name(one, 6, m);
        if (k >= 0 && n == 2 && REGN(0) && REGN(1)) {
            int opcode = k == 0 ? 0 : k == 1 ? 1 : k == 2 ? 2 : k == 3 ? (sf ? 3 : 2) : k == 4 ? 4 : 5;
            if (k == 2 && !sf) E("rev32: x registers");
            *out = ((a64_u32)sf << 31) | 0x5ac00000u | ((a64_u32)opcode << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        static const char *const carry[] = { "adc", "adcs", "sbc", "sbcs" };
        k = find_name(carry, 4, m);
        if (k >= 0 && n == 3 && REGN(0) && REGN(1) && REGN(2)) {
            *out = ((a64_u32)sf << 31) | ((a64_u32)(k >> 1) << 30) | ((a64_u32)(k & 1) << 29) | 0x1a000000u | (R(o[2].r) << 16) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        if ((IS("ngc") || IS("ngcs")) && n == 2 && REGN(0) && REGN(1)) {
            *out = ((a64_u32)sf << 31) | (1u << 30) | (IS("ngcs") ? 1u << 29 : 0) | 0x1a000000u | (R(o[1].r) << 16) | (31u << 5) | R(o[0].r); return 1;
        }
        if (IS("extr") && n == 4 && REGN(0) && REGN(1) && REGN(2) && o[3].kind == O_IMM) {
            if (o[3].imm < 0 || o[3].imm >= (sf ? 64 : 32)) E("#0-63 (0-31)");
            *out = ((a64_u32)sf << 31) | 0x13800000u | ((a64_u32)sf << 22) | (R(o[2].r) << 16) | ((a64_u32)o[3].imm << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        if (IS("bfc") && n == 3 && REGN(0) && o[1].kind == O_IMM && o[2].kind == O_IMM) {
            int size = sf ? 64 : 32, lsb = (int)o[1].imm, w = (int)o[2].imm;
            if (lsb < 0 || w < 1 || lsb + w > size) E("lsb and width inside the register");
            *out = ((a64_u32)sf << 31) | (1u << 29) | 0x13000000u | ((a64_u32)sf << 22) | ((a64_u32)((size - lsb) % size) << 16) | ((a64_u32)(w - 1) << 10) | (31u << 5) | R(o[0].r); return 1;
        }
        static const char *const lm[] = { "umsubl", "smsubl", "umnegl", "smnegl" };
        k = find_name(lm, 4, m);
        if (k >= 0 && n >= 3 && REGN(0) && REGN(1) && REGN(2)) {
            int ra = k < 2 ? (n == 4 && REGN(3) ? o[3].r : -1) : 31;
            if (ra < 0) E("four registers");
            *out = 0x9b208000u | ((k == 0 || k == 2) ? 1u << 23 : 0) | (R(o[2].r) << 16) | (R(ra) << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
    }
    /* ── acquire/release, exclusive ── */
    {
        static const char *const ar[] = { "ldar", "ldarb", "ldarh", "stlr", "stlrb", "stlrh", "ldlar", "ldlarb", "ldlarh", "stllr", "stllrb", "stllrh" };
        int k = find_name(ar, 12, m);
        if (k >= 0 && n == 2 && REGN(0) && o[1].kind == O_MEM && o[1].mode == 0 && !o[1].off) {
            int sz = k % 3 == 1 ? 0 : k % 3 == 2 ? 1 : (o[0].sf ? 3 : 2), l = (k / 3) % 2 == 0, o0 = k < 6;
            *out = ((a64_u32)sz << 30) | 0x08800000u | ((a64_u32)l << 22) | (31u << 16) | ((a64_u32)o0 << 15) | (31u << 10) | (R(o[1].base) << 5) | R(o[0].r); return 1;
        }
        static const char *const ex[] = { "ldxr", "ldxrb", "ldxrh", "ldaxr", "ldaxrb", "ldaxrh", "stxr", "stxrb", "stxrh", "stlxr", "stlxrb", "stlxrh" };
        k = find_name(ex, 12, m);
        if (k >= 0) {
            int store = k >= 6, o0 = (k / 3) % 2;
            int ti = store ? 1 : 0;
            if (n != (store ? 3 : 2) || !REGN(ti) || o[ti + 1].kind != O_MEM || o[ti + 1].mode || o[ti + 1].off || (store && !REGN(0))) E(store ? "ws, rt, [xn]" : "rt, [xn]");
            int sz = k % 3 == 1 ? 0 : k % 3 == 2 ? 1 : (o[ti].sf ? 3 : 2);
            *out = ((a64_u32)sz << 30) | 0x08000000u | ((a64_u32)!store << 22) | ((store ? R(o[0].r) : 31u) << 16) | ((a64_u32)o0 << 15) | (31u << 10) | (R(o[ti + 1].base) << 5) | R(o[ti].r); return 1;
        }
    }
    /* ── exclusive pairs, unprivileged, RCpc, LSE atomics, CAS, LDRAA, SYS ── */
    {
        static const char *const xp[] = { "ldxp", "ldaxp", "stxp", "stlxp" };
        int k = find_name(xp, 4, m);
        if (k >= 0) {
            int store = k >= 2, ti = store;
            if (n != 3 + store || !REGN(ti) || !REGN(ti + 1) || o[ti + 2].kind != O_MEM || o[ti + 2].mode || o[ti + 2].off) E(store ? "ws, rt, rt2, [xn]" : "rt, rt2, [xn]");
            *out = ((a64_u32)(o[ti].sf ? 3 : 2) << 30) | 0x08200000u | ((a64_u32)!store << 22) | ((store ? R(o[0].r) : 31u) << 16) | ((a64_u32)(k & 1) << 15) | (R(o[ti + 1].r) << 10) | (R(o[ti + 2].base) << 5) | R(o[ti].r); return 1;
        }
        static const char *const tr[] = { "sttrb", "ldtrb", "ldtrsb", "sttrh", "ldtrh", "ldtrsh", "sttr", "ldtr", "ldtrsw",
                                          "stlurb", "ldapurb", "ldapursb", "stlurh", "ldapurh", "ldapursh", "stlur", "ldapur", "ldapursw" };
        k = find_name(tr, 18, m);
        if (k >= 0 && n == 2 && REGN(0) && o[1].kind == O_MEM && o[1].mode == 0) {
            int rc = k >= 9, j = k % 9, sfr = o[0].sf, size, opc;
            if (j <= 2) { size = 0; opc = j == 0 ? 0 : j == 1 ? 1 : (sfr ? 2 : 3); }
            else if (j <= 5) { size = 1; opc = j == 3 ? 0 : j == 4 ? 1 : (sfr ? 2 : 3); }
            else if (j == 8) { size = 2; opc = 2; }
            else { size = sfr ? 3 : 2; opc = j == 7; }
            if (o[1].off < -256 || o[1].off > 255) E("an offset -256..255");
            a64_u32 w = ((a64_u32)size << 30) | ((a64_u32)opc << 22) | (((a64_u32)o[1].off & 0x1ff) << 12) | (R(o[1].base) << 5) | R(o[0].r);
            *out = rc ? w | 0x19000000u : w | 0x38000800u; return 1;
        }
        if ((IS("ldraa") || IS("ldrab")) && n == 2 && REGN(0) && o[1].kind == O_MEM && o[1].mode != 2 && o[1].mode != 3) {
            a64_s64 off = o[1].off;
            if ((off & 7) || off < -4096 || off > 4088) E("a multiple of 8, -4096..4088");
            a64_u32 v10 = (a64_u32)(off >> 3) & 0x3ff;
            *out = 0xf8200400u | (IS("ldrab") ? 1u << 23 : 0) | ((v10 >> 9) << 22) | ((v10 & 0x1ff) << 12) | ((a64_u32)(o[1].mode == 1) << 11) | (R(o[1].base) << 5) | R(o[0].r); return 1;
        }
        /* LSE: ld<op>{a}{l}{b,h} rs, rt, [xn]; st<op>{l}{b,h} rs, [xn]; swp{a}{l}{b,h}; cas{a}{l}{b,h}; casp{a}{l} */
        static const char *const aops[8] = { "add", "clr", "eor", "set", "smax", "smin", "umax", "umin" };
        const char *q = m; int kind = -1, aop = 0;
        if (lower(q[0]) == 'c' && lower(q[1]) == 'a' && lower(q[2]) == 's') { kind = lower(q[3]) == 'p' ? 4 : 3; q += kind == 4 ? 4 : 3; }
        else if (lower(q[0]) == 's' && lower(q[1]) == 'w' && lower(q[2]) == 'p') { kind = 2; q += 3; }
        else if ((lower(q[0]) == 'l' && lower(q[1]) == 'd') || (lower(q[0]) == 's' && lower(q[1]) == 't')) {
            for (int a = 0; a < 8; a++) {
                const char *t = aops[a]; unsigned tl = strlen_a(t), c;
                for (c = 0; c < tl && lower(q[2 + c]) == t[c]; c++) { }
                if (c == tl) { kind = lower(q[0]) == 'l' ? 0 : 1; aop = a; q += 2 + tl; break; }
            }
        }
        if (kind >= 0) {
            int acq = 0, rel = 0, sz = -1;
            if (lower(*q) == 'a') { acq = 1; q++; }
            if (lower(*q) == 'l') { rel = 1; q++; }
            if (lower(*q) == 'b') { sz = 0; q++; } else if (lower(*q) == 'h') { sz = 1; q++; }
            if (*q || (kind == 1 && acq) || (kind == 4 && sz >= 0)) kind = -1;
            else {
                int nreg = kind == 1 ? 1 : kind == 4 ? 4 : 2;
                if (n != nreg + 1 || o[nreg].kind != O_MEM || o[nreg].mode || o[nreg].off) E("registers, then [xn]");
                for (int a = 0; a < nreg; a++) if (!REGN(a)) E("registers, then [xn]");
                int rsz = sz >= 0 ? sz : (o[0].sf ? 3 : 2);
                a64_u32 rn = R(o[nreg].base) << 5;
                if (kind == 3) { *out = ((a64_u32)rsz << 30) | 0x08a07c00u | ((a64_u32)acq << 22) | (R(o[0].r) << 16) | ((a64_u32)rel << 15) | rn | R(o[1].r); return 1; }
                if (kind == 4) {
                    if ((o[0].r & 1) || (o[2].r & 1) || o[1].r != o[0].r + 1 || o[3].r != o[2].r + 1) E("casp: even register pairs");
                    *out = ((a64_u32)o[0].sf << 30) | 0x08207c00u | ((a64_u32)acq << 22) | (R(o[0].r) << 16) | ((a64_u32)rel << 15) | rn | R(o[2].r); return 1;
                }
                int rt = kind == 1 ? 31 : o[1].r;
                *out = ((a64_u32)rsz << 30) | 0x38200000u | ((a64_u32)acq << 23) | ((a64_u32)rel << 22) | (R(o[0].r) << 16) | ((a64_u32)(kind == 2) << 15) | ((a64_u32)(kind == 2 ? 0 : aop) << 12) | rn | R(rt); return 1;
            }
        }
        static const char *const lp2[] = { "ldapr", "ldaprb", "ldaprh" };
        k = find_name(lp2, 3, m);
        if (k >= 0 && n == 2 && REGN(0) && o[1].kind == O_MEM && !o[1].mode && !o[1].off) {
            int sz = k == 1 ? 0 : k == 2 ? 1 : (o[0].sf ? 3 : 2);
            *out = ((a64_u32)sz << 30) | 0x38bfc000u | (R(o[1].base) << 5) | R(o[0].r); return 1;
        }
        if ((IS("sys") && n >= 4) || (IS("sysl") && n == 5)) {
            int l = IS("sysl"), b0 = l;
            opnd_t *f = o + b0;
            if (f[0].kind != O_IMM || f[1].kind != O_NAME || f[2].kind != O_NAME || f[3].kind != O_IMM) E("#op1, cn, cm, #op2");
            a64_s64 cn, cm; const char *pc1 = f[1].name + 1, *pc2 = f[2].name + 1;
            if (lower(f[1].name[0]) != 'c' || lower(f[2].name[0]) != 'c' || !parse_num(&pc1, &cn) || !parse_num(&pc2, &cm) || *pc1 || *pc2 || cn > 15 || cm > 15 || f[0].imm > 7 || f[3].imm > 7) E("#op1, cn, cm, #op2");
            int rt = l ? (REGN(0) ? o[0].r : -1) : (n == 5 && REGN(4) ? o[4].r : 31);
            if (rt < 0) E("sysl: a register first");
            *out = 0xd5080000u | ((a64_u32)l << 21) | ((a64_u32)f[0].imm << 16) | ((a64_u32)cn << 12) | ((a64_u32)cm << 8) | ((a64_u32)f[3].imm << 5) | R(rt); return 1;
        }
    }
    /* ── scalar floating point ── */
    #define VR(k) (n > (k) && o[k].kind == O_VREG && o[k].vscale >= 1 && o[k].vscale <= 3)
    #define FTYPE(k) (o[k].vscale == 2 ? 0u : o[k].vscale == 3 ? 1u : 3u)
    {
        static const char *const f2[] = { "fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul" };
        int k = find_name(f2, 9, m);
        if (k >= 0 && n == 3 && VR(0) && VR(1) && VR(2)) {
            if (o[1].vscale != o[0].vscale || o[2].vscale != o[0].vscale) E("registers of one precision (h, s or d)");
            *out = 0x1e200800u | (FTYPE(0) << 22) | (R(o[2].r) << 16) | ((a64_u32)k << 12) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        static const char *const f3[] = { "fmadd", "fmsub", "fnmadd", "fnmsub" };
        k = find_name(f3, 4, m);
        if (k >= 0 && n == 4 && VR(0) && VR(1) && VR(2) && VR(3)) {
            if (o[1].vscale != o[0].vscale || o[2].vscale != o[0].vscale || o[3].vscale != o[0].vscale) E("registers of one precision");
            *out = 0x1f000000u | (FTYPE(0) << 22) | ((a64_u32)(k >> 1) << 21) | (R(o[2].r) << 16) | ((a64_u32)(k & 1) << 15) | (R(o[3].r) << 10) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        static const char *const f1[] = { "fmov", "fabs", "fneg", "fsqrt", 0, 0, 0, 0, "frintn", "frintp", "frintm", "frintz", "frinta", 0, "frintx", "frinti" };
        k = find_name(f1, 16, m);
        if (k >= 0 && n == 2 && VR(0) && VR(1)) {
            if (o[1].vscale != o[0].vscale) E("registers of one precision (fcvt changes it)");
            *out = 0x1e204000u | (FTYPE(0) << 22) | ((a64_u32)k << 15) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        if (IS("fcvt") && n == 2 && VR(0) && VR(1) && o[0].vscale != o[1].vscale) {
            *out = 0x1e204000u | (FTYPE(1) << 22) | ((4u | FTYPE(0)) << 15) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        if (IS("fmov") && n == 2 && ((VR(0) && REGN(1)) || (REGN(0) && VR(1)))) {     /* to/from a general register */
            int tofp = VR(0), g = tofp ? 1 : 0, f = tofp ? 0 : 1;
            if (o[f].vscale == 1 ? 0 : (o[f].vscale == 3) != o[g].sf) E("fmov: w with s (or h), x with d (or h)");
            *out = ((a64_u32)o[g].sf << 31) | 0x1e260000u | (FTYPE(f) << 22) | ((a64_u32)tofp << 16) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        static const char *const cv[] = { "scvtf", "ucvtf", "fcvtzs", "fcvtzu", "fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms", "fcvtmu", "fcvtas", "fcvtau" };
        k = find_name(cv, 12, m);
        if (k >= 0 && n == 2) {
            if (k < 2) {
                if (!(VR(0) && REGN(1))) E("scvtf/ucvtf: an FP register, then a general one");
                *out = ((a64_u32)o[1].sf << 31) | 0x1e220000u | (FTYPE(0) << 22) | ((a64_u32)k << 16) | (R(o[1].r) << 5) | R(o[0].r); return 1;
            }
            if (!(REGN(0) && VR(1))) E("a general register, then an FP one");
            static const unsigned rm_op[12][2] = { { 0, 0 }, { 0, 0 }, { 3, 0 }, { 3, 1 }, { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 2, 0 }, { 2, 1 }, { 0, 4 }, { 0, 5 } };
            *out = ((a64_u32)o[0].sf << 31) | 0x1e200000u | (FTYPE(1) << 22) | (rm_op[k][0] << 19) | (rm_op[k][1] << 16) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
        if ((IS("fcmp") || IS("fcmpe")) && n == 2 && VR(0)) {
            a64_u32 e = IS("fcmpe") ? 0x10 : 0;
            if ((o[1].kind == O_IMM || o[1].kind == O_FIMM) && o[1].imm == 0) { *out = 0x1e202008u | (FTYPE(0) << 22) | (R(o[0].r) << 5) | e; return 1; }
            if (VR(1)) { *out = 0x1e202000u | (FTYPE(0) << 22) | (R(o[1].r) << 16) | (R(o[0].r) << 5) | e; return 1; }
            E("fcmp: two FP registers, or one and #0.0");
        }
        if ((IS("fccmp") || IS("fccmpe")) && n == 4 && VR(0) && VR(1) && o[2].kind == O_IMM && o[3].kind == O_COND) {
            if (o[2].imm < 0 || o[2].imm > 15) E("nzcv: #0-15");
            *out = 0x1e200400u | (FTYPE(0) << 22) | (R(o[1].r) << 16) | ((a64_u32)o[3].imm << 12) | (R(o[0].r) << 5) | (IS("fccmpe") ? 0x10u : 0) | (a64_u32)o[2].imm; return 1;
        }
        if (IS("fcsel") && n == 4 && VR(0) && VR(1) && VR(2) && o[3].kind == O_COND) {
            *out = 0x1e200c00u | (FTYPE(0) << 22) | (R(o[2].r) << 16) | ((a64_u32)o[3].imm << 12) | (R(o[1].r) << 5) | R(o[0].r); return 1;
        }
    }
    #undef VR
    #undef FTYPE
    /* ── loads and stores ── */
    {
        static const char *const ls[] = { "strb", "ldrb", "ldrsb", "strh", "ldrh", "ldrsh", "str", "ldr", "ldrsw", "prfm" };
        int k = find_name(ls, 10, m);
        if (k >= 0 && n == 2) {
            int size, opc, v = 0, scale;
            if (o[0].kind == O_VREG) {
                if (k != 6 && k != 7) E("str/ldr for an FP/SIMD register");
                v = 1; scale = o[0].vscale; size = scale == 4 ? 0 : scale; opc = (k == 7) | (scale == 4 ? 2 : 0);
            } else if (k == 9) {
                int t = o[0].kind == O_NAME ? prf_by_name(o[0].name) : o[0].kind == O_IMM ? (int)o[0].imm : -1;
                if (t < 0 || t > 31) E("a prefetch operation (pldl1keep, ...)");
                o[0].kind = O_REG; o[0].r = t; size = 3; opc = 2; scale = 3;
            } else {
                if (o[0].kind != O_REG || o[0].sp) E("a register to load or store");
                int sfr = o[0].sf;
                if (k <= 2) { size = 0; opc = k == 0 ? 0 : k == 1 ? 1 : (sfr ? 2 : 3); if (k < 2 && sfr) E("w registers for byte loads/stores"); }
                else if (k <= 5) { size = 1; opc = k == 3 ? 0 : k == 4 ? 1 : (sfr ? 2 : 3); if (k < 5 && sfr) E("w registers for halfword loads/stores"); }
                else if (k == 8) { size = 2; opc = 2; if (!sfr) E("ldrsw: an x register"); }
                else { size = sfr ? 3 : 2; opc = k == 7; }
                scale = size;
            }
            a64_u32 base = ((a64_u32)size << 30) | 0x38000000u | ((a64_u32)v << 26) | ((a64_u32)opc << 22) | R(o[0].r);
            if (o[1].kind == O_IMM) {                            /* literal */
                a64_s64 d = o[1].imm - (a64_s64)pc;
                if (k == 0 || k == 3 || k == 6 || (k != 7 && k != 8 && k != 9)) E("only ldr, ldrsw, prfm take a label");
                if ((d & 3) || d < -(1ll << 20) || d >= (1ll << 20)) E("out of reach (+-1 MB)");
                a64_u32 lopc = v ? (a64_u32)(scale - 2) : k == 8 ? 2 : k == 9 ? 3 : (a64_u32)(size == 3);
                if (v && scale < 2) E("ldr (literal): s, d or q");
                *out = (lopc << 30) | 0x18000000u | ((a64_u32)v << 26) | (((a64_u32)(d >> 2) & 0x7ffff) << 5) | R(o[0].r); return 1;
            }
            if (o[1].kind != O_MEM) E("an address: [xn{, #imm}], [xn, #imm]!, [xn], #imm or [xn, xm{, lsl #n}]");
            a64_u32 rn = R(o[1].base) << 5;
            if (o[1].mode == 3) {                                /* register offset */
                int s = 0;                                       /* amount given, or the access size: S (also "#0" for bytes) */
                if (o[1].ext_has) { if (o[1].ext_amt != scale && o[1].ext_amt != 0) E("the shift: 0 or the access size"); s = o[1].ext_amt == scale; }
                int option = o[1].ext;
                if (option != 2 && option != 3 && option != 6 && option != 7) E("uxtw, sxtw, sxtx or lsl");
                if (((option & 1) != 0) != o[1].idx_sf) E("an x index for lsl/sxtx, a w one for uxtw/sxtw");
                *out = base | (1u << 21) | (R(o[1].idx) << 16) | ((a64_u32)option << 13) | ((a64_u32)s << 12) | (2u << 10) | rn; return 1;
            }
            a64_s64 off = o[1].off;
            if (o[1].mode == 0 && off >= 0 && !(off & ((1 << scale) - 1)) && (off >> scale) < 4096) {   /* unsigned offset */
                *out = base | (1u << 24) | ((a64_u32)(off >> scale) << 10) | rn; return 1;
            }
            if (off < -256 || off > 255) E("an offset out of range");
            if (o[1].mode == 0 && k == 9) E("prfm: use prfum for this offset");
            *out = base | (((a64_u32)off & 0x1ff) << 12) | ((a64_u32)(o[1].mode == 0 ? 0 : o[1].mode == 1 ? 3 : 1) << 10) | rn;
            if (o[1].mode == 0) E("an offset needing the unscaled form: use ldur/stur");
            return 1;
        }
        static const char *const lu[] = { "sturb", "ldurb", "ldursb", "sturh", "ldurh", "ldursh", "stur", "ldur", "ldursw", "prfum" };
        k = find_name(lu, 10, m);
        if (k >= 0 && n == 2 && o[1].kind == O_MEM && o[1].mode == 0) {
            int size, opc, v = 0;
            if (o[0].kind == O_VREG) { v = 1; size = o[0].vscale == 4 ? 0 : o[0].vscale; opc = (k == 7) | (o[0].vscale == 4 ? 2 : 0); }
            else if (k == 9) { int t = o[0].kind == O_NAME ? prf_by_name(o[0].name) : (int)o[0].imm; if (t < 0) E("a prefetch operation"); o[0].kind = O_REG; o[0].r = t; size = 3; opc = 2; }
            else {
                if (o[0].kind != O_REG) E("a register");
                int sfr = o[0].sf;
                if (k <= 2) { size = 0; opc = k == 0 ? 0 : k == 1 ? 1 : (sfr ? 2 : 3); }
                else if (k <= 5) { size = 1; opc = k == 3 ? 0 : k == 4 ? 1 : (sfr ? 2 : 3); }
                else if (k == 8) { size = 2; opc = 2; }
                else { size = sfr ? 3 : 2; opc = k == 7; }
            }
            if (o[1].off < -256 || o[1].off > 255) E("an offset -256..255");
            *out = ((a64_u32)size << 30) | 0x38000000u | ((a64_u32)v << 26) | ((a64_u32)opc << 22) | (((a64_u32)o[1].off & 0x1ff) << 12) | (R(o[1].base) << 5) | R(o[0].r); return 1;
        }
        static const char *const lp[] = { "stp", "ldp", "ldpsw", "stnp", "ldnp" };
        k = find_name(lp, 5, m);
        if (k >= 0) {
            if (n != 3 || o[2].kind != O_MEM || o[2].mode == 3) E("two registers, then an address");
            int post = o[2].mode == 2;
            int v = o[0].kind == O_VREG, opc, scale;
            if (v) { if (o[1].kind != O_VREG || o[1].vscale != o[0].vscale || o[0].vscale < 2) E("two s, d or q registers"); scale = o[0].vscale; opc = scale - 2; }
            else {
                if (o[0].kind != O_REG || o[1].kind != O_REG || o[0].sf != o[1].sf) E("two registers of one size");
                if (k == 2) { if (!o[0].sf) E("ldpsw: x registers"); opc = 1; scale = 2; }
                else { opc = o[0].sf ? 2 : 0; scale = o[0].sf ? 3 : 2; }
            }
            int mode = post ? 1 : o[2].mode == 1 ? 3 : 2;
            if (k >= 3) { if (post || o[2].mode) E("stnp/ldnp: an offset only"); mode = 0; }
            a64_s64 off = o[2].off;
            if (off & ((1 << scale) - 1)) E("an offset that's a multiple of the size");
            off >>= scale;
            if (off < -64 || off > 63) E("an offset out of range");
            int l = k == 1 || k == 2 || k == 4;
            *out = ((a64_u32)opc << 30) | 0x28000000u | ((a64_u32)v << 26) | ((a64_u32)mode << 23) | ((a64_u32)l << 22) | (((a64_u32)off & 0x7f) << 15) | (R(o[1].r) << 10) | (R(o[2].base) << 5) | R(o[0].r); return 1;
        }
    }
    E("an instruction (or form) this assembler doesn't know");
    #undef E
    #undef IS
    #undef REGN
}

/* split a line's operands at commas outside [] and {} ("[xn], #imm" stays one) */
static int parse_operands(const char *p, opnd_t *o, int *nout, char *err, unsigned errsize)
{
    int n = 0;
    char tok[80];
    while (*p && n < 6) {
        int t = 0, depth = 0;
        while (*p && (depth || *p != ',') && t < 79) { if (*p == '[' || *p == '{') depth++; else if (*p == ']' || *p == '}') depth--; tok[t++] = *p++; }
        while (t && is_sp(tok[t - 1])) t--;
        tok[t] = 0;
        if (*p == ',') p++;
        skipsp(&p);
        if (!t) { seterr(err, errsize, "an empty operand"); return 0; }
        if (!parse_operand(tok, &o[n], err, errsize)) return 0;
        /* "[xn], #imm": one post-index operand */
        if (n > 0 && o[n].kind == O_IMM && o[n - 1].kind == O_MEM && o[n - 1].mode == 0 && !o[n - 1].off && tok[0] == '#') {
            o[n - 1].mode = 2; o[n - 1].off = o[n].imm; continue;
        }
        n++;
    }
    if (*p) { seterr(err, errsize, "too many operands"); return 0; }
    *nout = n;
    return 1;
}

/* ── FP/SIMD by search ──
 * Since the disassembler is exact, a candidate encoding whose disassembly is
 * the same instruction (mnemonic, operands compared as values) is the
 * encoding. Each group lists the bits always set, the bits to try, and where
 * the registers go. */
static int same_opnd(const opnd_t *a, const opnd_t *b)
{
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case O_REG: return a->r == b->r && a->sf == b->sf && a->sp == b->sp && a->zr == b->zr;
    case O_VREG: return a->r == b->r && a->vscale == b->vscale;
    case O_IMM: case O_FIMM: case O_COND: return a->imm == b->imm;
    case O_VEC: return a->r == b->r && a->arr == b->arr;
    case O_VELEM: return a->r == b->r && a->vscale == b->vscale && a->eidx == b->eidx;
    case O_VLIST: return a->r == b->r && a->cnt == b->cnt && a->arr == b->arr && a->vscale == b->vscale && a->eidx == b->eidx;
    case O_MEM: return a->base == b->base && a->mode == b->mode && a->off == b->off && (a->mode != 3 || (a->idx == b->idx && a->ext == b->ext && a->ext_amt == b->ext_amt));
    case O_SHIFT: return a->sh == b->sh && a->amt == b->amt;
    case O_EXT: return a->sh == b->sh && a->amt == b->amt;
    case O_NAME: return streq_ci(a->name, b->name);
    }
    return 0;
}
static int candidate_is(a64_u32 w, a64_u64 pc, const char *m, const opnd_t *o, int n)
{
    char t[160], err[8];
    if (!a64_disasm(w, pc, t, sizeof t)) return 0;
    int k = 0;
    while (t[k] && t[k] != '\t' && m[k] && lower(t[k]) == lower(m[k])) k++;
    if (m[k] || (t[k] && t[k] != '\t')) return 0;
    opnd_t c[6]; int nc = 0;
    if (!parse_operands(t[k] ? t + k + 1 : t + k, c, &nc, err, sizeof err) || nc != n) return 0;
    for (int j = 0; j < n; j++) if (!same_opnd(&c[j], &o[j])) return 0;
    return 1;
}
static int simd_search(const char *m, opnd_t *o, int n, a64_u64 pc, a64_u32 *out)
{
    /* registers in order (a list's first, a memory operand's base); a structure post-index register */
    int r[4] = { 0, 0, 0, 0 }, nr = 0, base = -1, post = 0;
    for (int j = 0; j < n; j++) {
        int k = o[j].kind;
        if ((k == O_REG || k == O_VREG || k == O_VEC || k == O_VELEM || k == O_VLIST) && nr < 4) {
            if (k == O_REG && j > 0 && o[j - 1].kind == O_MEM) post = o[j].r; else r[nr++] = o[j].r;
        }
        if (k == O_MEM) { base = o[j].base; if (o[j].mode == 2) post = 31; }
    }
    if (nr == 2) r[2] = r[1];                                    /* mov v0.16b, v1.16b: orr's rm is rn */
    static const struct { a64_u32 base, vary; int layout; } g[] = {   /* layout: 0 d,n,m; 1 d,n,m(4 bits); 2 d; 3 t,base,post; 4 d,n */
        { 0x0e200400u, 0x60c0f800u, 0 }, { 0x0e400400u, 0x60803800u, 0 }, { 0x0e008400u, 0x60c07800u, 0 },
        { 0x0e200000u, 0x60c0f000u, 0 }, { 0x0e200800u, 0x60c1f000u, 4 }, { 0x0e300800u, 0x60c1f000u, 4 },
        { 0x0e000800u, 0x40c07000u, 0 }, { 0x0e000400u, 0x601f7800u, 4 }, { 0x0f000400u, 0x6007fbe0u, 2 },
        { 0x0f000400u, 0x607ff800u, 4 }, { 0x0f000000u, 0x60f0f800u, 1 }, { 0x2e000000u, 0x40007800u, 0 },
        { 0x0e000000u, 0x40007000u, 0 },
        { 0x5e200400u, 0x20c0f800u, 0 }, { 0x5e200800u, 0x20c1f000u, 4 }, { 0x5e300800u, 0x20c1f000u, 4 },
        { 0x5f000400u, 0x207ff800u, 4 }, { 0x5f000000u, 0x20f0f800u, 1 }, { 0x5e000400u, 0x001f0000u, 4 },
        { 0x5e200000u, 0x20c0f000u, 0 }, { 0x5e400400u, 0x20803800u, 0 }, { 0x5e008400u, 0x20c07800u, 0 },
        { 0x0c000000u, 0x40c0fc00u, 3 }, { 0x0d000000u, 0x40e0fc00u, 3 },
        { 0x1e201000u, 0x00dfe000u, 2 }, { 0x1e000000u, 0x80dffc00u, 4 }, { 0x9eae0000u, 0x00010000u, 4 },
    };
    for (unsigned gi = 0; gi < sizeof g / sizeof g[0]; gi++) {
        a64_u32 w0 = g[gi].base, v = g[gi].vary, regs;
        switch (g[gi].layout) {
        case 0: regs = R(r[0]) | (R(r[1]) << 5) | (R(r[2]) << 16); break;
        case 1: regs = R(r[0]) | (R(r[1]) << 5) | ((R(r[2]) & 15) << 16); v |= 1u << 20; break;
        case 2: regs = R(r[0]); break;
        case 3: if (base < 0) continue; regs = R(r[0]) | (R(base) << 5) | (R(post) << 16); break;
        default: regs = R(r[0]) | (R(r[1]) << 5); break;
        }
        if (g[gi].layout == 1 && (r[2] & 16)) w0 |= 1u << 20, v &= ~(1u << 20);
        /* every subset of the varying bits */
        a64_u32 sub = 0;
        do {
            a64_u32 w = (w0 | regs | sub) & ~(v & ~sub);
            if ((w & ~v) == ((w0 | regs) & ~v) && candidate_is(w, pc, m, o, n)) { *out = w; return 1; }
            sub = (sub - v) & v;
        } while (sub);
    }
    return 0;
}

int a64_asm(const char *line, a64_u64 pc, a64_u32 *insn, char *err, unsigned errsize)
{
    char m[24]; int k = 0;
    const char *p = line;
    if (errsize) err[0] = 0;
    skipsp(&p);
    while (*p && !is_sp(*p) && k < 23) m[k++] = *p++;
    m[k] = 0;
    if (!k) { seterr(err, errsize, "an empty line"); return 0; }
    skipsp(&p);
    opnd_t o[6]; int n = 0;
    if (!parse_operands(p, o, &n, err, errsize)) return 0;
    int vec = 0, fp = 0;
    for (int j = 0; j < n; j++) {
        if (o[j].kind == O_VEC || o[j].kind == O_VELEM || o[j].kind == O_VLIST) vec = 1;
        if (o[j].kind == O_VREG || o[j].kind == O_FIMM) fp = 1;
    }
    if (vec) {                                                   /* vector operands: search */
        if (simd_search(m, o, n, pc, insn)) return 1;
        seterr(err, errsize, "not an instruction (or form) this assembler can make");
        return 0;
    }
    if (assemble(m, o, n, pc, insn, err, errsize)) return 1;
    if (fp && simd_search(m, o, n, pc, insn)) return 1;          /* scalar SIMD (d0, s1, ...) */
    return 0;
}
