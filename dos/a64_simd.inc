/* dos/a64_simd.inc - FP and Advanced SIMD for dos/a64.c's disassembler
 * (included there): scalar floating point (conversions, 1/2/3 sources,
 * compare, immediate, conditional compare and select), and Advanced SIMD
 * (copy, modified immediate, three same, two-register misc, across lanes,
 * shift by immediate, permute, extract, table, three different, by element,
 * scalar pairwise and copy), and the structure loads/stores (LD1-LD4,
 * ST1-ST4, LD1R-LD4R). LLVM's spelling, as the rest. */

static const char *const arr_names[8] = { "8b", "16b", "4h", "8h", "2s", "4s", "1d", "2d" };
static void vec(buf_t *b, int r, int size, int q) { putc1(b, 'v'); putdec(b, (a64_u64)r); putc1(b, '.'); put(b, arr_names[size * 2 + q]); }
static void velem(buf_t *b, int r, int size, int idx) { putc1(b, 'v'); putdec(b, (a64_u64)r); putc1(b, '.'); putc1(b, "bhsd"[size]); put(b, "["); putdec(b, (a64_u64)idx); put(b, "]"); }
static void fpreg(buf_t *b, int r, int type) { putc1(b, type == 3 ? 'h' : type == 0 ? 's' : 'd'); putdec(b, (a64_u64)r); }   /* (ptype 00 s, 01 d, 11 h) */
static void sreg(buf_t *b, int r, int size) { putc1(b, "bhsdq"[size]); putdec(b, (a64_u64)r); }

/* FP8 immediate: (-1)^s * (16+frac)/16 * 2^(exp) - as LLVM prints it, 8 decimals */
static void fp_imm8(buf_t *b, int imm8)
{
    int s = imm8 >> 7, e = ((imm8 >> 4) & 7) ^ 4, frac = imm8 & 15;    /* exponent -3..4 */
    int ex = e - 3;                                                    /* (bits b:c:d with b inverted: 0..7 -> -3..4) */
    a64_u64 num = (a64_u64)(16 + frac) * 100000000ull;                 /* value * 10^8 = (16+frac) * 2^ex / 16 * 10^8 */
    if (ex >= 0) num <<= ex; else num >>= -ex;
    num /= 16;
    put(b, "#"); if (s) put(b, "-");
    putdec(b, num / 100000000ull); put(b, ".");
    a64_u64 f = num % 100000000ull;
    char d[9]; for (int k = 7; k >= 0; k--) { d[k] = (char)('0' + f % 10); f /= 10; } d[8] = 0;
    put(b, d);
}

static int fp_scalar(a64_u32 i, buf_t *b)
{
    int type = bits(i, 23, 22), rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    if (bits(i, 31, 29) == 4 && bits(i, 28, 24) == 0x1e && type == 2 && bits(i, 21, 19) == 5 && bits(i, 18, 17) == 3 && !bits(i, 15, 10)) {
        mn(b, "fmov");                                                   /* (the top half: fmov x, v.d[1] / v.d[1], x) */
        if (!bits(i, 16, 16)) { reg(b, rd, 1, 0); sep(b); velem(b, rn, 3, 1); } else { velem(b, rd, 3, 1); sep(b); reg(b, rn, 1, 0); }
        return 1;
    }
    if (type == 2) return 0;
    if (bits(i, 28, 24) == 0x1f) {                                       /* 3 sources */
        if (bits(i, 31, 29)) return 0;
        static const char *const m[4] = { "fmadd", "fmsub", "fnmadd", "fnmsub" };
        mn(b, m[(bits(i, 21, 21) << 1) | bits(i, 15, 15)]);
        fpreg(b, rd, type); sep(b); fpreg(b, rn, type); sep(b); fpreg(b, rm, type); sep(b); fpreg(b, bits(i, 14, 10), type);
        return 1;
    }
    if (bits(i, 28, 24) != 0x1e || bits(i, 30, 30)) return 0;
    if (!bits(i, 21, 21)) {                                              /* fixed point conversions */
        int sf = bits(i, 31, 31), rmode = bits(i, 20, 19), op = bits(i, 18, 16), scale = bits(i, 15, 10);
        if (bits(i, 29, 29) || (!sf && scale < 32)) return 0;
        int fbits = 64 - scale;
        if (rmode == 0 && (op == 2 || op == 3)) { mn(b, op == 2 ? "scvtf" : "ucvtf"); fpreg(b, rd, type); sep(b); reg(b, rn, sf, 0); }
        else if (rmode == 3 && (op == 0 || op == 1)) { mn(b, op == 0 ? "fcvtzs" : "fcvtzu"); reg(b, rd, sf, 0); sep(b); fpreg(b, rn, type); }
        else return 0;
        sep(b); imm_hex(b, fbits);
        return 1;
    }
    if (bits(i, 15, 10) == 0) {                                          /* FP <-> integer */
        int sf = bits(i, 31, 31), rmode = bits(i, 20, 19), op = bits(i, 18, 16);
        if (bits(i, 29, 29)) return 0;
        if (op == 6 || op == 7) {                                         /* FMOV general */
            if (rmode) return 0;
            if ((sf && type == 0) || (!sf && type == 1)) return 0;
            mn(b, "fmov");
            if (op == 6) { reg(b, rd, sf, 0); sep(b); fpreg(b, rn, type); } else { fpreg(b, rd, type); sep(b); reg(b, rn, sf, 0); }
            return 1;
        }
        if (op == 2 || op == 3) {
            if (rmode) return 0;
            mn(b, op == 2 ? "scvtf" : "ucvtf"); fpreg(b, rd, type); sep(b); reg(b, rn, sf, 0); return 1;
        }
        static const char *const cv[4][2] = { { "fcvtns", "fcvtnu" }, { "fcvtps", "fcvtpu" }, { "fcvtms", "fcvtmu" }, { "fcvtzs", "fcvtzu" } };
        const char *m = 0;
        if (op <= 1) m = cv[rmode][op];
        else if (rmode == 0 && op == 4) m = "fcvtas";
        else if (rmode == 0 && op == 5) m = "fcvtau";
        if (!m) return 0;
        mn(b, m); reg(b, rd, sf, 0); sep(b); fpreg(b, rn, type); return 1;
    }
    if (bits(i, 31, 29)) return 0;
    if (bits(i, 14, 10) == 0x10) {                                       /* 1 source */
        int op = bits(i, 20, 15);
        static const char *const m1[16] = { "fmov", "fabs", "fneg", "fsqrt", 0, 0, 0, 0, "frintn", "frintp", "frintm", "frintz", "frinta", 0, "frintx", "frinti" };
        if (op >= 4 && op <= 7) {                                         /* FCVT to another precision */
            int to = op & 3;
            if (to == 2 || to == type) return 0;
            mn(b, "fcvt"); fpreg(b, rd, to); sep(b); fpreg(b, rn, type); return 1;
        }
        if (op >= 16 || !m1[op]) return 0;
        mn(b, m1[op]); fpreg(b, rd, type); sep(b); fpreg(b, rn, type); return 1;
    }
    if (bits(i, 13, 10) == 8) {                                          /* compare */
        int op2 = bits(i, 4, 0);
        if (bits(i, 15, 14) || (op2 & 7)) return 0;
        mn(b, (op2 & 0x10) ? "fcmpe" : "fcmp"); fpreg(b, rn, type); sep(b);
        if (op2 & 8) put(b, "#0.0"); else fpreg(b, rm, type);
        return 1;
    }
    if (bits(i, 12, 10) == 4) {                                          /* immediate */
        if (bits(i, 9, 5)) return 0;
        mn(b, "fmov"); fpreg(b, rd, type); sep(b); fp_imm8(b, (int)bits(i, 20, 13)); return 1;
    }
    if (bits(i, 11, 10) == 1) {                                          /* conditional compare */
        mn(b, bits(i, 4, 4) ? "fccmpe" : "fccmp"); fpreg(b, rn, type); sep(b); fpreg(b, rm, type); sep(b);
        imm_hex(b, bits(i, 3, 0)); sep(b); put(b, conds[bits(i, 15, 12)]); return 1;
    }
    if (bits(i, 11, 10) == 2) {                                          /* 2 sources */
        static const char *const m2[9] = { "fmul", "fdiv", "fadd", "fsub", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul" };
        int op = bits(i, 15, 12);
        if (op > 8) return 0;
        mn(b, m2[op]); fpreg(b, rd, type); sep(b); fpreg(b, rn, type); sep(b); fpreg(b, rm, type); return 1;
    }
    /* conditional select */
    mn(b, "fcsel"); fpreg(b, rd, type); sep(b); fpreg(b, rn, type); sep(b); fpreg(b, rm, type); sep(b); put(b, conds[bits(i, 15, 12)]);
    return 1;
}

static int simd_copy(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), op = bits(i, 29, 29), imm5 = bits(i, 20, 16), imm4 = bits(i, 14, 11), rd = bits(i, 4, 0), rn = bits(i, 9, 5);
    int size = -1;
    for (int k = 0; k < 4; k++) if (imm5 & (1 << k)) { size = k; break; }
    if (size < 0) return 0;
    int idx = imm5 >> (size + 1);
    if (op) {                                                            /* INS (element) */
        if (!q) return 0;
        mn(b, "mov"); velem(b, rd, size, idx); sep(b); velem(b, rn, size, imm4 >> size); return 1;
    }
    if (imm4 == 0) { if (size == 3 && !q) return 0; mn(b, "dup"); vec(b, rd, size, q); sep(b); velem(b, rn, size, idx); return 1; }
    if (imm4 == 1) { if (size == 3 && !q) return 0; mn(b, "dup"); vec(b, rd, size, q); sep(b); reg(b, rn, size == 3, 0); return 1; }
    if (imm4 == 3) { if (!q) return 0; mn(b, "mov"); velem(b, rd, size, idx); sep(b); reg(b, rn, size == 3, 0); return 1; }
    if (imm4 == 5) { if (size == 3 || (size == 2 && !q)) return 0; mn(b, "smov"); reg(b, rd, q, 0); sep(b); velem(b, rn, size, idx); return 1; }
    if (imm4 == 7) {
        if ((size == 3) != q) return 0;
        mn(b, size >= 2 ? "mov" : "umov"); reg(b, rd, q, 0); sep(b); velem(b, rn, size, idx); return 1;
    }
    return 0;
}

/* (LLVM's "%#016llx": 0 is sixteen zeros; anything else 0x and at least fourteen digits) */
static void hex64_plain(buf_t *b, a64_u64 v)
{
    put(b, "#");
    if (!v) { put(b, "0000000000000000"); return; }
    put(b, "0x");
    int digits = 16; while (digits > 14 && !((v >> ((digits - 1) * 4)) & 15)) digits--;
    for (int s = (digits - 1) * 4; s >= 0; s -= 4) putc1(b, "0123456789abcdef"[(v >> s) & 15]);
}
static int simd_modimm(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), op = bits(i, 29, 29), cmode = bits(i, 15, 12), o2 = bits(i, 11, 11), rd = bits(i, 4, 0);
    int imm8 = (bits(i, 18, 16) << 5) | bits(i, 9, 5);
    if (o2) return 0;
    if (cmode < 8) {                                                     /* 32-bit, shifted */
        int sh = (cmode >> 1) * 8, orr = cmode & 1;
        mn(b, orr ? (op ? "bic" : "orr") : (op ? "mvni" : "movi")); vec(b, rd, 2, q); sep(b); imm_hex(b, imm8);
        if (sh) { put(b, ", lsl #"); putdec(b, (a64_u64)sh); }
        return 1;
    }
    if (cmode < 12) {                                                    /* 16-bit, shifted */
        int sh = ((cmode >> 1) & 1) * 8, orr = cmode & 1;
        mn(b, orr ? (op ? "bic" : "orr") : (op ? "mvni" : "movi")); vec(b, rd, 1, q); sep(b); imm_hex(b, imm8);
        if (sh) { put(b, ", lsl #"); putdec(b, (a64_u64)sh); }
        return 1;
    }
    if (cmode < 14) {                                                    /* 32-bit, shifting ones */
        mn(b, op ? "mvni" : "movi"); vec(b, rd, 2, q); sep(b); imm_hex(b, imm8); put(b, ", msl #"); putdec(b, (a64_u64)((cmode & 1) ? 16 : 8));
        return 1;
    }
    if (cmode == 14) {
        if (!op) { mn(b, "movi"); vec(b, rd, 0, q); sep(b); imm_hex(b, imm8); return 1; }
        a64_u64 v = 0;
        for (int k = 0; k < 8; k++) if (imm8 & (1 << k)) v |= 0xffull << (k * 8);
        mn(b, "movi");
        if (q) { vec(b, rd, 3, 1); sep(b); hex64_plain(b, v); }
        else { sreg(b, rd, 3); sep(b); hex64_plain(b, v); }
        return 1;
    }
    if (!op) { mn(b, "fmov"); vec(b, rd, 2, q); sep(b); fp_imm8(b, imm8); return 1; }
    if (!q) return 0;
    mn(b, "fmov"); vec(b, rd, 3, 1); sep(b); fp_imm8(b, imm8);
    return 1;
}

static int simd_three_same(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), size = bits(i, 23, 22), op = bits(i, 15, 11);
    int rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    const char *m = 0;
    if (op == 3) {                                                       /* logical, on bytes */
        static const char *const lg[2][4] = { { "and", "bic", "orr", "orn" }, { "eor", "bsl", "bit", "bif" } };
        if (!u && size == 2 && rn == rm) { mn(b, "mov"); vec(b, rd, 0, q); sep(b); vec(b, rn, 0, q); return 1; }
        mn(b, lg[u][size]); vec(b, rd, 0, q); sep(b); vec(b, rn, 0, q); sep(b); vec(b, rm, 0, q); return 1;
    }
    if (op >= 0x18) {                                                    /* floating point */
        int sz = size & 1, hi = size >> 1;
        if (sz && !q) return 0;
        static const char *const f[2][2][8] = {
            { { "fmaxnm", "fmla", "fadd", "fmulx", "fcmeq", 0, "fmax", "frecps" }, { "fminnm", "fmls", "fsub", 0, 0, 0, "fmin", "frsqrts" } },
            { { "fmaxnmp", 0, "faddp", "fmul", "fcmge", "facge", "fmaxp", "fdiv" }, { "fminnmp", 0, "fabd", 0, "fcmgt", "facgt", "fminp", 0 } } };
        m = f[u][hi][op - 0x18];
        if (!m) return 0;
        mn(b, m); vec(b, rd, 2 + sz, q); sep(b); vec(b, rn, 2 + sz, q); sep(b); vec(b, rm, 2 + sz, q); return 1;
    }
    static const char *const s0[24] = { "shadd", "sqadd", "srhadd", 0, "shsub", "sqsub", "cmgt", "cmge", "sshl", "sqshl", "srshl", "sqrshl",
                                        "smax", "smin", "sabd", "saba", "add", "cmtst", "mla", "mul", "smaxp", "sminp", "sqdmulh", "addp" };
    static const char *const s1[24] = { "uhadd", "uqadd", "urhadd", 0, "uhsub", "uqsub", "cmhi", "cmhs", "ushl", "uqshl", "urshl", "uqrshl",
                                        "umax", "umin", "uabd", "uaba", "sub", "cmeq", "mls", "pmul", "umaxp", "uminp", "sqrdmulh", 0 };
    m = u ? s1[op] : s0[op];
    if (!m) return 0;
    if (size == 3 && !q) return 0;
    if ((op == 0x12 || op == 0x13 || op == 0x14 || op == 0x15 || op == 0 || op == 2 || op == 4 || op == 0x0c || op == 0x0d || op == 0x0e || op == 0x0f) && size == 3) return 0;
    if (op == 0x13 && u && size) return 0;                               /* (pmul: bytes) */
    if (op == 0x16 && (size == 0 || size == 3)) return 0;
    mn(b, m); vec(b, rd, size, q); sep(b); vec(b, rn, size, q); sep(b); vec(b, rm, size, q);
    return 1;
}

static int simd_two_misc(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), size = bits(i, 23, 22), op = bits(i, 16, 12), rd = bits(i, 4, 0), rn = bits(i, 9, 5);
    if (op == 0x12 || op == 0x14 || (op == 0x13 && u)) {                 /* narrowing (xtn, sqxtn, ...), shll */
        if (size == 3) return 0;
        if (op == 0x13) { mn(b, q ? "shll2" : "shll"); vec(b, rd, size + 1, 1); sep(b); vec(b, rn, size, q); put(b, ", #"); putdec(b, (a64_u64)(8 << size)); return 1; }
        static const char *const nm[2][2] = { { "xtn", "sqxtn" }, { "sqxtun", "uqxtn" } };
        char m[12]; buf_t mb = { m, 0, sizeof m };
        put(&mb, nm[u][op == 0x14]); if (q) put(&mb, "2");
        mn(b, m); vec(b, rd, size, q); sep(b); vec(b, rn, size + 1, 1); return 1;
    }
    if (op >= 0x0c && (op != 0x0e || size >= 2)) {                       /* floating point and others */
        int sz = size & 1, hi = size >> 1;
        const char *m = 0;
        if (op == 0x16 && !hi) { char mm[12]; buf_t mb = { mm, 0, sizeof mm }; put(&mb, u ? "fcvtxn" : "fcvtn"); if (q) put(&mb, "2"); if (u && !sz) return 0;
            mn(b, mm); vec(b, rd, 2 - !sz + 0 * sz, q); sep(b); vec(b, rn, 2 + sz, 1); return 1; }
        if (op == 0x17 && !hi && !u) { char mm[12]; buf_t mb = { mm, 0, sizeof mm }; put(&mb, "fcvtl"); if (q) put(&mb, "2");
            mn(b, mm); vec(b, rd, 2 + sz, 1); sep(b); vec(b, rn, 1 + sz, q); return 1; }
        static const char *const f0[2][2][32] = {
            { { [0x18] = "frintn", [0x19] = "frintm", [0x1a] = "fcvtns", [0x1b] = "fcvtms", [0x1c] = "fcvtas", [0x1d] = "scvtf" },
              { [0x0c] = "fcmgt", [0x0d] = "fcmeq", [0x0e] = "fcmlt", [0x0f] = "fabs", [0x18] = "frintp", [0x19] = "frintz", [0x1a] = "fcvtps", [0x1b] = "fcvtzs", [0x1c] = "urecpe", [0x1d] = "frecpe" } },
            { { [0x18] = "frinta", [0x19] = "frintx", [0x1a] = "fcvtnu", [0x1b] = "fcvtmu", [0x1c] = "fcvtau", [0x1d] = "ucvtf" },
              { [0x0c] = "fcmge", [0x0d] = "fcmle", [0x0f] = "fneg", [0x19] = "frinti", [0x1a] = "fcvtpu", [0x1b] = "fcvtzu", [0x1c] = "ursqrte", [0x1d] = "frsqrte", [0x1f] = "fsqrt" } } };
        m = f0[u][hi][op];
        if (op == 0x0e && hi && u) return 0;
        if (!m) return 0;
        if (op == 0x1c && hi) {                                          /* urecpe/ursqrte: 32-bit integer lanes */
            if (sz) return 0;
            mn(b, m); vec(b, rd, 2, q); sep(b); vec(b, rn, 2, q); return 1;
        }
        if (sz && !q) return 0;
        mn(b, m); vec(b, rd, 2 + sz, q); sep(b); vec(b, rn, 2 + sz, q);
        if (op == 0x0c || op == 0x0d || op == 0x0e) put(b, ", #0.0");
        return 1;
    }
    if (op == 0x0e && size >= 2) {                                      /* fcmlt #0.0 */
        int sz = size & 1;
        if (u || (sz && !q)) return 0;
        mn(b, "fcmlt"); vec(b, rd, 2 + sz, q); sep(b); vec(b, rn, 2 + sz, q); put(b, ", #0.0"); return 1;
    }
    static const char *const m0[16] = { "rev64", "rev16", "saddlp", "suqadd", "cls", "cnt", "sadalp", "sqabs", "cmgt", "cmeq", "cmlt", "abs", 0, 0, 0, 0 };
    static const char *const m1[16] = { "rev32", 0, "uaddlp", "usqadd", "clz", 0, "uadalp", "sqneg", "cmge", "cmle", 0, "neg", 0, 0, 0, 0 };
    if (op >= 16) return 0;
    const char *m = u ? m1[op] : m0[op];
    if (u && op == 5) { if (size == 0) m = "mvn"; else if (size == 1) m = "rbit"; else return 0; size = 0; }
    if (!m) return 0;
    if ((op == 0 && size == 3) || (op == 1 && size != 0) || (u && op == 0 && size >= 2) || (op == 5 && size)) return 0;
    if ((op == 4 || op == 5) && size == 3) return 0;
    if ((op == 3 || op == 7 || op >= 8) && size == 3 && !q) return 0;
    if (op == 2 || op == 6) {                                            /* pairwise long */
        if (size == 3) return 0;
        mn(b, m); vec(b, rd, size + 1, q); sep(b); vec(b, rn, size, q); return 1;
    }
    mn(b, m); vec(b, rd, size, q); sep(b); vec(b, rn, size, q);
    if (op >= 8 && op <= 10) put(b, ", #0");
    return 1;
}

static int simd_across(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), size = bits(i, 23, 22), op = bits(i, 16, 12), rd = bits(i, 4, 0), rn = bits(i, 9, 5);
    const char *m = 0;
    int wide = 0;
    if (op == 3) { m = u ? "uaddlv" : "saddlv"; wide = 1; }
    else if (op == 0x0a) m = u ? "umaxv" : "smaxv";
    else if (op == 0x1a) m = u ? "uminv" : "sminv";
    else if (op == 0x1b && !u) m = "addv";
    else if (u && (op == 0x0c || op == 0x0f) && size != 1 && size != 3) {
        static const char *const fm[2][2] = { { "fmaxnmv", "fmaxv" }, { "fminnmv", "fminv" } };
        if (!q || (size & 1)) return 0;
        mn(b, fm[size >> 1][op == 0x0f]); sreg(b, rd, 2); sep(b); vec(b, rn, 2, 1); return 1;
    }
    if (!m || size == 3 || (size == 2 && !q)) return 0;
    mn(b, m); sreg(b, rd, size + wide); sep(b); vec(b, rn, size, q);
    return 1;
}

static int simd_shift_imm(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), immh = bits(i, 22, 19), immb = bits(i, 18, 16), op = bits(i, 15, 11);
    int rd = bits(i, 4, 0), rn = bits(i, 9, 5);
    int size = immh >= 8 ? 3 : immh >= 4 ? 2 : immh >= 2 ? 1 : 0, esize = 8 << size, v = (immh << 3) | immb;
    int right = 2 * esize - v, left = v - esize;
    if (!immh) return 0;
    char m[16]; buf_t mb = { m, 0, sizeof m };
    switch (op) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x08: {
        if (size == 3 && !q) return 0;
        static const char *const r0[5] = { "sshr", "ssra", "srshr", "srsra", 0 }, *const r1[5] = { "ushr", "usra", "urshr", "ursra", "sri" };
        const char *n = u ? r1[op / 2] : r0[op / 2];
        if (!n) return 0;
        mn(b, n); vec(b, rd, size, q); sep(b); vec(b, rn, size, q); sep(b); imm_hex(b, right); return 1;
    }
    case 0x0a: case 0x0c: case 0x0e: {
        if (size == 3 && !q) return 0;
        const char *n = op == 0x0a ? (u ? "sli" : "shl") : op == 0x0c ? (u ? "sqshlu" : 0) : (u ? "uqshl" : "sqshl");
        if (!n) return 0;
        mn(b, n); vec(b, rd, size, q); sep(b); vec(b, rn, size, q); sep(b); imm_hex(b, left); return 1;
    }
    case 0x10: case 0x11: case 0x12: case 0x13: {                          /* narrowing right shifts */
        if (size == 3) return 0;
        static const char *const n0[4] = { "shrn", "rshrn", "sqshrn", "sqrshrn" }, *const n1[4] = { "sqshrun", "sqrshrun", "uqshrn", "uqrshrn" };
        put(&mb, u ? n1[op - 0x10] : n0[op - 0x10]); if (q) put(&mb, "2");
        mn(b, m); vec(b, rd, size, q); sep(b); vec(b, rn, size + 1, 1); sep(b); imm_hex(b, right); return 1;
    }
    case 0x14:                                                           /* sshll/ushll */
        if (size == 3) return 0;
        put(&mb, u ? "ushll" : "sshll"); if (q) put(&mb, "2");
        mn(b, m); vec(b, rd, size + 1, 1); sep(b); vec(b, rn, size, q); sep(b); imm_hex(b, left); return 1;
    case 0x1c: case 0x1f:                                                /* fixed point conversions */
        if (size < 2 || (size == 3 && !q)) return 0;
        mn(b, op == 0x1c ? (u ? "ucvtf" : "scvtf") : (u ? "fcvtzu" : "fcvtzs")); vec(b, rd, size, q); sep(b); vec(b, rn, size, q); sep(b); imm_hex(b, right); return 1;
    }
    return 0;
}

static int simd_three_diff(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), size = bits(i, 23, 22), op = bits(i, 15, 12);
    int rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    static const char *const s0[15] = { "saddl", "saddw", "ssubl", "ssubw", "addhn", "sabal", "subhn", "sabdl", "smlal", "sqdmlal", "smlsl", "sqdmlsl", "smull", "sqdmull", "pmull" };
    static const char *const s1[15] = { "uaddl", "uaddw", "usubl", "usubw", "raddhn", "uabal", "rsubhn", "uabdl", "umlal", 0, "umlsl", 0, "umull", 0, 0 };
    if (op == 15) return 0;
    const char *n = u ? s1[op] : s0[op];
    if (!n) return 0;
    if (size == 3 && op != 14) return 0;
    if (op == 14 && size != 0 && size != 3) return 0;
    if ((op == 9 || op == 11 || op == 13) && size == 0) return 0;
    char m[16]; buf_t mb = { m, 0, sizeof m };
    put(&mb, n); if (q) put(&mb, "2");
    mn(b, m);
    if (op == 4 || op == 6) { vec(b, rd, size, q); sep(b); vec(b, rn, size + 1, 1); sep(b); vec(b, rm, size + 1, 1); return 1; }   /* narrowing high */
    if (op == 14 && size == 3) { putc1(b, 'v'); putdec(b, (a64_u64)rd); put(b, ".1q"); sep(b); vec(b, rn, 3, q); sep(b); vec(b, rm, 3, q); return 1; }
    vec(b, rd, size + 1, 1); sep(b);
    if (op == 1 || op == 3) vec(b, rn, size + 1, 1); else vec(b, rn, size, q);
    sep(b); vec(b, rm, size, q);
    return 1;
}

static int simd_by_elem(a64_u32 i, buf_t *b, int scalar)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), size = bits(i, 23, 22), op = bits(i, 15, 12);
    int h = bits(i, 11, 11), l = bits(i, 21, 21), mbit = bits(i, 20, 20), rm = bits(i, 19, 16), rd = bits(i, 4, 0), rn = bits(i, 9, 5);
    int fp = op == 1 || op == 5 || op == 9;
    if (fp) {                                                            /* (size 00 half, 10 single, 11 double) */
        if (size == 1 || (u && op != 9)) return 0;
        int es = size == 0 ? 1 : size, idx, r;
        if (size == 0) { idx = (h << 2) | (l << 1) | mbit; r = rm; }
        else if (size == 2) { idx = (h << 1) | l; r = (mbit << 4) | rm; }
        else { if (l || (!q && !scalar)) return 0; idx = h; r = (mbit << 4) | rm; }
        const char *n = op == 1 ? "fmla" : op == 5 ? "fmls" : (u ? "fmulx" : "fmul");
        mn(b, n);
        if (scalar) { sreg(b, rd, es); sep(b); sreg(b, rn, es); }
        else { vec(b, rd, es, q); sep(b); vec(b, rn, es, q); }
        sep(b); velem(b, r, es, idx); return 1;
    }
    if (scalar) {
        if (size == 0 || size == 3) return 0;
        int idx = size == 1 ? (h << 2) | (l << 1) | mbit : (h << 1) | l, r = size == 1 ? rm : (mbit << 4) | rm;
        const char *n = 0; int lng = 0;
        switch (op) {
        case 0x3: n = u ? 0 : "sqdmlal"; lng = 1; break;
        case 0x7: n = u ? 0 : "sqdmlsl"; lng = 1; break;
        case 0xb: n = u ? 0 : "sqdmull"; lng = 1; break;
        case 0xc: n = u ? 0 : "sqdmulh"; break;
        case 0xd: n = u ? "sqrdmlah" : "sqrdmulh"; break;
        case 0xf: n = u ? "sqrdmlsh" : 0; break;
        }
        if (!n) return 0;
        mn(b, n); sreg(b, rd, size + lng); sep(b); sreg(b, rn, size); sep(b); velem(b, r, size, idx); return 1;
    }
    if (size == 0 || size == 3) return 0;
    int idx = size == 1 ? (h << 2) | (l << 1) | mbit : (h << 1) | l, r = size == 1 ? rm : (mbit << 4) | rm;
    const char *n = 0; int lng = 0;
    switch (op) {
    case 0x8: n = u ? 0 : "mul"; break;
    case 0x0: n = u ? "mla" : 0; break;
    case 0x4: n = u ? "mls" : 0; break;
    case 0x2: n = u ? "umlal" : "smlal"; lng = 1; break;
    case 0x6: n = u ? "umlsl" : "smlsl"; lng = 1; break;
    case 0xa: n = u ? "umull" : "smull"; lng = 1; break;
    case 0x3: n = u ? 0 : "sqdmlal"; lng = 1; break;
    case 0x7: n = u ? 0 : "sqdmlsl"; lng = 1; break;
    case 0xb: n = u ? 0 : "sqdmull"; lng = 1; break;
    case 0xc: n = u ? 0 : "sqdmulh"; break;
    case 0xd: n = u ? "sqrdmlah" : "sqrdmulh"; break;
    case 0xf: n = u ? "sqrdmlsh" : 0; break;
    }
    if (!n) return 0;
    char m[16]; buf_t mb = { m, 0, sizeof m };
    put(&mb, n); if (lng && q) put(&mb, "2");
    mn(b, m);
    if (lng) { vec(b, rd, size + 1, 1); sep(b); vec(b, rn, size, q); }
    else { vec(b, rd, size, q); sep(b); vec(b, rn, size, q); }
    sep(b); velem(b, r, size, idx);
    return 1;
}

/* the structure loads/stores: LD1-LD4/ST1-ST4 (multiple, single), LD1R-LD4R */
static void vlist(buf_t *b, int rt, int n, int size, int q)
{
    put(b, "{ ");
    for (int k = 0; k < n; k++) { if (k) sep(b); vec(b, (rt + k) & 31, size, q); }
    put(b, " }");
}
static int simd_ldst(a64_u32 i, buf_t *b)
{
    int q = bits(i, 30, 30), l = bits(i, 22, 22), rn = bits(i, 9, 5), rt = bits(i, 4, 0), rm = bits(i, 20, 16);
    int post = bits(i, 23, 23);
    if (bits(i, 31, 31) || bits(i, 29, 24) != 0x0c) {
        if (bits(i, 31, 31) || bits(i, 29, 24) != 0x0d) return 0;
        /* single structure */
        int r = bits(i, 21, 21), opc = bits(i, 15, 13), s = bits(i, 12, 12), size = bits(i, 11, 10);
        int selem = (((opc & 1) << 1) | r) + 1, scale = opc >> 1;
        if (!post && rm) return 0;
        char m[8]; buf_t mb = { m, 0, sizeof m };
        if (scale == 3) {                                                /* LDnR: replicate */
            if (!l || s) return 0;
            put(&mb, "ld"); putc1(&mb, (char)('0' + selem)); put(&mb, "r");
            mn(b, m); vlist(b, rt, selem, size, q);
        } else {
            int idx;
            if (scale == 1) { if (size & 1) return 0; idx = (q << 2) | (s << 1) | (size >> 1); }
            else if (scale == 2) {
                if (size & 2) return 0;
                if (size & 1) { if (s) return 0; scale = 3; idx = q; } else idx = (q << 1) | s;
            } else idx = (q << 3) | (s << 2) | size;
            put(&mb, l ? "ld" : "st"); putc1(&mb, (char)('0' + selem));
            mn(b, m);
            put(b, "{ ");
            for (int k = 0; k < selem; k++) { if (k) sep(b); putc1(b, 'v'); putdec(b, (a64_u64)((rt + k) & 31)); putc1(b, '.'); putc1(b, "bhsd"[scale]); }
            put(b, " }["); putdec(b, (a64_u64)idx); put(b, "]");
            size = scale;
        }
        put(b, ", ["); reg(b, rn, 1, 1); put(b, "]");
        if (post) {                                                      /* (#: the bytes it took - an element each) */
            if (rm == 31) { put(b, ", #"); putdec(b, (a64_u64)((1 << size) * selem)); }
            else { sep(b); reg(b, rm, 1, 0); }
        }
        return 1;
    }
    /* multiple structures */
    int opc = bits(i, 15, 12), size = bits(i, 11, 10);
    if (bits(i, 21, 21) || (!post && rm)) return 0;
    int regs, selem;
    switch (opc) {
    case 0x0: regs = 4; selem = 4; break; case 0x2: regs = 4; selem = 1; break;
    case 0x4: regs = 3; selem = 3; break; case 0x6: regs = 3; selem = 1; break;
    case 0x7: regs = 1; selem = 1; break; case 0x8: regs = 2; selem = 2; break;
    case 0xa: regs = 2; selem = 1; break; default: return 0;
    }
    if (size == 3 && !q && selem != 1) return 0;
    char m[8]; buf_t mb = { m, 0, sizeof m };
    put(&mb, l ? "ld" : "st"); putc1(&mb, (char)('0' + selem));
    mn(b, m); vlist(b, rt, regs, size, q);
    put(b, ", ["); reg(b, rn, 1, 1); put(b, "]");
    if (post) {
        if (rm == 31) { put(b, ", #"); putdec(b, (a64_u64)(regs * (q ? 16 : 8))); }
        else { sep(b); reg(b, rm, 1, 0); }
    }
    return 1;
}

/* scalar Advanced SIMD: three same, two-register misc, shift by immediate - the vector forms on one register */
static int simd_scalar(a64_u32 i, buf_t *b)
{
    int u = bits(i, 29, 29), size = bits(i, 23, 22), rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    if (bits(i, 21, 21) && bits(i, 10, 10)) {                            /* three same */
        int op = bits(i, 15, 11);
        const char *m = 0;
        if (op >= 0x18) {
            int sz = size & 1, hi = size >> 1;
            static const char *const f[2][2][8] = {
                { { 0, 0, 0, "fmulx", "fcmeq", 0, 0, "frecps" }, { 0, 0, 0, 0, 0, 0, 0, "frsqrts" } },
                { { 0, 0, 0, 0, "fcmge", "facge", 0, 0 }, { 0, 0, "fabd", 0, "fcmgt", "facgt", 0, 0 } } };
            m = f[u][hi][op - 0x18];
            if (!m) return 0;
            mn(b, m); sreg(b, rd, 2 + sz); sep(b); sreg(b, rn, 2 + sz); sep(b); sreg(b, rm, 2 + sz); return 1;
        }
        static const char *const s0[24] = { 0, "sqadd", 0, 0, 0, "sqsub", "cmgt", "cmge", "sshl", "sqshl", "srshl", "sqrshl", 0, 0, 0, 0, "add", "cmtst", 0, 0, 0, 0, "sqdmulh", 0 };
        static const char *const s1[24] = { 0, "uqadd", 0, 0, 0, "uqsub", "cmhi", "cmhs", "ushl", "uqshl", "urshl", "uqrshl", 0, 0, 0, 0, "sub", "cmeq", 0, 0, 0, 0, "sqrdmulh", 0 };
        m = u ? s1[op] : s0[op];
        if (!m) return 0;
        int d_only = op == 6 || op == 7 || op == 8 || op == 10 || op == 16 || op == 17;
        if (d_only && size != 3) return 0;
        if (op == 22 && (size == 0 || size == 3)) return 0;
        mn(b, m); sreg(b, rd, size); sep(b); sreg(b, rn, size); sep(b); sreg(b, rm, size); return 1;
    }
    if (bits(i, 21, 17) == 0x10 && bits(i, 11, 10) == 2) {               /* two-register misc */
        int op = bits(i, 16, 12), sz = size & 1, hi = size >> 1;
        if (op >= 0x0c) {
            static const char *const f[2][2][32] = {
                { { [0x1a] = "fcvtns", [0x1b] = "fcvtms", [0x1c] = "fcvtas", [0x1d] = "scvtf" },
                  { [0x0c] = "fcmgt", [0x0d] = "fcmeq", [0x0e] = "fcmlt", [0x1a] = "fcvtps", [0x1b] = "fcvtzs", [0x1d] = "frecpe", [0x1f] = "frecpx" } },
                { { [0x1a] = "fcvtnu", [0x1b] = "fcvtmu", [0x1c] = "fcvtau", [0x1d] = "ucvtf" },
                  { [0x0c] = "fcmge", [0x0d] = "fcmle", [0x1a] = "fcvtpu", [0x1b] = "fcvtzu", [0x1d] = "frsqrte" } } };
            if (op == 0x16 && u && !hi && sz) { mn(b, "fcvtxn"); sreg(b, rd, 2); sep(b); sreg(b, rn, 3); return 1; }
            const char *m = f[u][hi][op];
            if (!m) return 0;
            mn(b, m); sreg(b, rd, 2 + sz); sep(b); sreg(b, rn, 2 + sz);
            if (op >= 0x0c && op <= 0x0e) put(b, ", #0.0");
            return 1;
        }
        if (op == 0x14 || op == 0x12) {                                  /* sqxtn, uqxtn, sqxtun */
            const char *m = op == 0x14 ? (u ? "uqxtn" : "sqxtn") : (u ? "sqxtun" : 0);
            if (!m || size == 3) return 0;
            mn(b, m); sreg(b, rd, size); sep(b); sreg(b, rn, size + 1); return 1;
        }
        static const char *const m0[12] = { 0, 0, 0, "suqadd", 0, 0, 0, "sqabs", "cmgt", "cmeq", "cmlt", "abs" };
        static const char *const m1[12] = { 0, 0, 0, "usqadd", 0, 0, 0, "sqneg", "cmge", "cmle", 0, "neg" };
        if (op >= 12) return 0;
        const char *m = u ? m1[op] : m0[op];
        if (!m) return 0;
        if (op >= 8 && size != 3) return 0;
        mn(b, m); sreg(b, rd, size); sep(b); sreg(b, rn, size);
        if (op >= 8 && op <= 10) put(b, ", #0");
        return 1;
    }
    return 0;
}
static int simd_scalar_shift(a64_u32 i, buf_t *b)
{
    int u = bits(i, 29, 29), immh = bits(i, 22, 19), immb = bits(i, 18, 16), op = bits(i, 15, 11), rd = bits(i, 4, 0), rn = bits(i, 9, 5);
    if (!immh) return 0;
    int size = immh >= 8 ? 3 : immh >= 4 ? 2 : immh >= 2 ? 1 : 0, esize = 8 << size, v = (immh << 3) | immb;
    int right = 2 * esize - v, left = v - esize;
    const char *m = 0; int sh = right;
    switch (op) {
    case 0x00: m = u ? "ushr" : "sshr"; break;
    case 0x02: m = u ? "usra" : "ssra"; break;
    case 0x04: m = u ? "urshr" : "srshr"; break;
    case 0x06: m = u ? "ursra" : "srsra"; break;
    case 0x08: m = u ? "sri" : 0; break;
    case 0x0a: m = u ? "sli" : "shl"; sh = left; break;
    case 0x0c: m = u ? "sqshlu" : 0; sh = left; goto any_size;
    case 0x0e: m = u ? "uqshl" : "sqshl"; sh = left; goto any_size;
    case 0x10: case 0x11: case 0x12: case 0x13: {
        static const char *const n0[4] = { 0, 0, "sqshrn", "sqrshrn" }, *const n1[4] = { "sqshrun", "sqrshrun", "uqshrn", "uqrshrn" };
        const char *nn = u ? n1[op - 0x10] : n0[op - 0x10];
        if (!nn || size == 3) return 0;
        mn(b, nn); sreg(b, rd, size); sep(b); sreg(b, rn, size + 1); sep(b); imm_hex(b, right); return 1;
    }
    case 0x1c: if (size >= 2) { mn(b, u ? "ucvtf" : "scvtf"); sreg(b, rd, size); sep(b); sreg(b, rn, size); sep(b); imm_hex(b, right); return 1; } return 0;
    case 0x1f: if (size >= 2) { mn(b, u ? "fcvtzu" : "fcvtzs"); sreg(b, rd, size); sep(b); sreg(b, rn, size); sep(b); imm_hex(b, right); return 1; } return 0;
    }
    if (!m || size != 3) return 0;
    mn(b, m); sreg(b, rd, 3); sep(b); sreg(b, rn, 3); sep(b); imm_hex(b, sh);
    return 1;
any_size:
    if (!m) return 0;
    mn(b, m); sreg(b, rd, size); sep(b); sreg(b, rn, size); sep(b); imm_hex(b, sh);
    return 1;
}

/* FP16 three same: 0 Q U 01110 a 10 Rm 00 opcode 1 Rn Rd (vector) / 01 U 11110 a 10 ... (scalar) */
static int simd_fp16_same(a64_u32 i, buf_t *b, int scalar)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), a = bits(i, 23, 23), op = bits(i, 13, 11), rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    static const char *const f[2][2][8] = {
        { { "fmaxnm", "fmla", "fadd", "fmulx", "fcmeq", 0, "fmax", "frecps" }, { "fminnm", "fmls", "fsub", 0, 0, 0, "fmin", "frsqrts" } },
        { { "fmaxnmp", 0, "faddp", "fmul", "fcmge", "facge", "fmaxp", "fdiv" }, { "fminnmp", 0, "fabd", 0, "fcmgt", "facgt", "fminp", 0 } } };
    const char *m = f[u][a][op];
    if (!m) return 0;
    if (scalar) {
        static const char *const ok[] = { "fmulx", "fcmeq", "frecps", "frsqrts", "fcmge", "facge", "fabd", "fcmgt", "facgt" };
        int good = 0;
        for (unsigned k = 0; k < sizeof ok / sizeof ok[0]; k++) { const char *x = ok[k], *y = m; while (*x && *x == *y) { x++; y++; } if (!*x && !*y) good = 1; }
        if (!good) return 0;
        mn(b, m); sreg(b, rd, 1); sep(b); sreg(b, rn, 1); sep(b); sreg(b, rm, 1); return 1;
    }
    mn(b, m); vec(b, rd, 1, q); sep(b); vec(b, rn, 1, q); sep(b); vec(b, rm, 1, q);
    return 1;
}
/* three same (extra): sqrdmlah, sqrdmlsh (v8.1), sdot/udot - 0 Q U 01110 size 0 Rm 1 opcode 1 Rn Rd */
static int simd_same_extra(a64_u32 i, buf_t *b, int scalar)
{
    int q = bits(i, 30, 30), u = bits(i, 29, 29), size = bits(i, 23, 22), op = bits(i, 14, 11), rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    if (u && (op == 0 || op == 1)) {
        if (size == 0 || size == 3) return 0;
        mn(b, op ? "sqrdmlsh" : "sqrdmlah");
        if (scalar) { sreg(b, rd, size); sep(b); sreg(b, rn, size); sep(b); sreg(b, rm, size); }
        else { vec(b, rd, size, q); sep(b); vec(b, rn, size, q); sep(b); vec(b, rm, size, q); }
        return 1;
    }
    if (!scalar && op == 2 && size == 2) {
        mn(b, u ? "udot" : "sdot"); vec(b, rd, 2, q); sep(b); vec(b, rn, 0, q); sep(b); vec(b, rm, 0, q); return 1;
    }
    return 0;
}
static int simd_scalar_diff(a64_u32 i, buf_t *b)
{
    int u = bits(i, 29, 29), size = bits(i, 23, 22), op = bits(i, 15, 12), rd = bits(i, 4, 0), rn = bits(i, 9, 5), rm = bits(i, 20, 16);
    const char *n = op == 9 ? "sqdmlal" : op == 11 ? "sqdmlsl" : op == 13 ? "sqdmull" : 0;
    if (u || !n || size == 0 || size == 3) return 0;
    mn(b, n); sreg(b, rd, size + 1); sep(b); sreg(b, rn, size); sep(b); sreg(b, rm, size);
    return 1;
}

static int simd_dp(a64_u32 i, a64_u64 pc, buf_t *b)
{
    (void)pc;
    int top = bits(i, 31, 31), s28 = bits(i, 28, 24);
    if (!top && (s28 == 0x1e || s28 == 0x1f) && !bits(i, 30, 30)) return fp_scalar(i, b);
    if (top && s28 == 0x1e && !bits(i, 30, 29)) return fp_scalar(i, b);     /* (FP <-> 64-bit integer: sf 1) */
    if (top) return 0;
    int s30 = bits(i, 30, 30), u = bits(i, 29, 29);
    if (bits(i, 28, 21) == 0x70 && !bits(i, 15, 15) && bits(i, 10, 10)) return simd_copy(i, b);
    if (bits(i, 28, 19) == 0x1e0 && bits(i, 10, 10)) return simd_modimm(i, b);
    if (bits(i, 28, 24) == 0x0e && bits(i, 22, 21) == 2 && !bits(i, 15, 14) && bits(i, 10, 10)) return simd_fp16_same(i, b, 0);
    if (bits(i, 28, 24) == 0x0e && !bits(i, 21, 21) && bits(i, 15, 15) && bits(i, 10, 10)) return simd_same_extra(i, b, 0);
    if (bits(i, 28, 24) == 0x0e && bits(i, 21, 21) && bits(i, 10, 10)) return simd_three_same(i, b);
    if (bits(i, 28, 24) == 0x0e && bits(i, 21, 17) == 0x10 && bits(i, 11, 10) == 2) return simd_two_misc(i, b);
    if (bits(i, 28, 24) == 0x0e && bits(i, 21, 17) == 0x18 && bits(i, 11, 10) == 2) return simd_across(i, b);
    if (bits(i, 28, 23) == 0x1e && bits(i, 10, 10)) return simd_shift_imm(i, b);
    if (bits(i, 28, 24) == 0x0e && bits(i, 21, 21) && !bits(i, 11, 10)) return simd_three_diff(i, b);
    if (bits(i, 28, 24) == 0x0f && !bits(i, 10, 10)) return simd_by_elem(i, b, 0);
    if (!u && bits(i, 28, 24) == 0x0e && !bits(i, 21, 21) && !bits(i, 15, 15) && bits(i, 11, 10) == 2) {   /* permute */
        static const char *const pm[8] = { 0, "uzp1", "trn1", "zip1", 0, "uzp2", "trn2", "zip2" };
        int size = bits(i, 23, 22), op = bits(i, 14, 12);
        if (!pm[op] || (size == 3 && !s30)) return 0;
        mn(b, pm[op]); vec(b, bits(i, 4, 0), size, s30); sep(b); vec(b, bits(i, 9, 5), size, s30); sep(b); vec(b, bits(i, 20, 16), size, s30); return 1;
    }
    if (u && bits(i, 28, 21) == 0x70 && !bits(i, 15, 15) && !bits(i, 10, 10)) {   /* EXT */
        int imm4 = bits(i, 14, 11);
        if (!s30 && imm4 > 7) return 0;
        mn(b, "ext"); vec(b, bits(i, 4, 0), 0, s30); sep(b); vec(b, bits(i, 9, 5), 0, s30); sep(b); vec(b, bits(i, 20, 16), 0, s30); sep(b); imm_hex(b, imm4); return 1;
    }
    if (!u && bits(i, 28, 21) == 0x70 && !bits(i, 15, 15) && !bits(i, 11, 10)) {  /* TBL/TBX */
        int len = bits(i, 14, 13) + 1;
        mn(b, bits(i, 12, 12) ? "tbx" : "tbl"); vec(b, bits(i, 4, 0), 0, s30); sep(b); vlist(b, bits(i, 9, 5), len, 0, 1); sep(b); vec(b, bits(i, 20, 16), 0, s30); return 1;
    }
    if (s30 && bits(i, 28, 23) == 0x3e && bits(i, 10, 10)) return simd_scalar_shift(i, b);
    if (s30 && bits(i, 28, 24) == 0x1f && !bits(i, 10, 10)) return simd_by_elem(i, b, 1);
    if (s30 && bits(i, 28, 24) == 0x1e && bits(i, 22, 21) == 2 && !bits(i, 15, 14) && bits(i, 10, 10)) return simd_fp16_same(i, b, 1);
    if (s30 && bits(i, 28, 24) == 0x1e && !bits(i, 21, 21) && bits(i, 15, 15) && bits(i, 10, 10)) return simd_same_extra(i, b, 1);
    if (s30 && bits(i, 28, 24) == 0x1e && bits(i, 21, 21) && !bits(i, 11, 10)) return simd_scalar_diff(i, b);
    if (s30 && bits(i, 28, 24) == 0x1e && ((bits(i, 21, 21) && bits(i, 10, 10)) || (bits(i, 21, 17) == 0x10 && bits(i, 11, 10) == 2))) return simd_scalar(i, b);
    if (s30 && bits(i, 28, 24) == 0x1e && bits(i, 21, 17) == 0x18 && bits(i, 11, 10) == 2) {   /* scalar pairwise */
        int size = bits(i, 23, 22), op = bits(i, 16, 12);
        if (!u && op == 0x1b && size == 3) { mn(b, "addp"); sreg(b, bits(i, 4, 0), 3); sep(b); vec(b, bits(i, 9, 5), 3, 1); return 1; }
        if (u && (op == 0x0c || op == 0x0d || op == 0x0f)) {
            static const char *const f[2][3] = { { "fmaxnmp", "faddp", "fmaxp" }, { "fminnmp", 0, "fminp" } };
            const char *n = f[size >> 1][op == 0x0c ? 0 : op == 0x0d ? 1 : 2];
            if (!n) return 0;
            mn(b, n); sreg(b, bits(i, 4, 0), 2 + (size & 1)); sep(b); vec(b, bits(i, 9, 5), 2 + (size & 1), size & 1); return 1;
        }
        return 0;
    }
    if (s30 && !u && bits(i, 28, 21) == 0xf0 && !bits(i, 15, 11) && bits(i, 10, 10)) {   /* scalar DUP (element): mov */
        int imm5 = bits(i, 20, 16), size = -1;
        for (int k = 0; k < 4; k++) if (imm5 & (1 << k)) { size = k; break; }
        if (size < 0) return 0;
        mn(b, "mov"); sreg(b, bits(i, 4, 0), size); sep(b); velem(b, bits(i, 9, 5), size, imm5 >> (size + 1)); return 1;
    }
    return 0;
}
