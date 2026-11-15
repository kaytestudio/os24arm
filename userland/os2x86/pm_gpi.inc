/* userland/os2x86/pm_gpi.inc - Presentation Manager's bitmaps (included
 * at the end of pm.inc): bitmap resources and files (OS/2 1.x and 2.x
 * BMPs, bitmap arrays; 1, 4, 8, 24 and 32 bits a pixel), GpiCreateBitmap
 * and its bits, WinDrawBitmap, GpiBitBlt and GpiWCBitBlt (the usual
 * ROPs, stretching), memory device contexts (DevOpenDC, GpiCreatePS,
 * GpiSetBitmap). A bitmap here is a window that's never shown: its
 * pixels are the bitmap's, so everything that draws into windows draws
 * into bitmaps (a memory PS is a PS of that window). */

/* a bitmap's pixels (bottom-up rows, as the file has them) from bits at b,
 * its info header at info (cbFix 12: OS/2 1.x, else 2.x), palette after it */
static int bits_decode(const uint8_t *b, size_t bn, const uint8_t *info, size_t in, int w, int h, uint32_t *px, int pw, int row0)
{
    if (in < 16) return -1;
    uint32_t cbfix = info[0] | info[1] << 8 | info[2] << 16 | (uint32_t)info[3] << 24;
    int old = cbfix == 12, bpp = old ? (info[10] | info[11] << 8) : (info[14] | info[15] << 8);
    uint32_t comp = !old && cbfix >= 20 ? (uint32_t)(info[16] | info[17] << 8 | info[18] << 16 | (uint32_t)info[19] << 24) : 0;
    uint32_t nclr = !old && cbfix >= 36 ? (uint32_t)(info[32] | info[33] << 8 | info[34] << 16 | (uint32_t)info[35] << 24) : 0;
    if (comp) return -1;                                       /* (BCA_UNCOMP only, for now) */
    if (bpp <= 8 && !nclr) nclr = 1u << bpp;
    const uint8_t *pal = info + cbfix;
    int pe = old ? 3 : 4;
    uint32_t rowb = (uint32_t)((w * bpp + 31) / 32 * 4);
    for (int r = 0; r < h; r++) {
        if ((size_t)(r + 1) * rowb > bn) break;
        const uint8_t *s = b + (size_t)r * rowb;
        int y = row0 + r;                                      /* (from the bottom) */
        if (y < 0) continue;
        uint32_t *d = px + (size_t)y * (size_t)pw;
        for (int x = 0; x < w && x < pw; x++) {
            uint32_t c = 0;
            if (bpp == 24) c = (uint32_t)s[3 * x + 2] << 16 | (uint32_t)s[3 * x + 1] << 8 | s[3 * x];
            else if (bpp == 32) c = (uint32_t)s[4 * x + 2] << 16 | (uint32_t)s[4 * x + 1] << 8 | s[4 * x];
            else {
                uint32_t i = bpp == 8 ? s[x] : bpp == 4 ? (s[x / 2] >> (x & 1 ? 0 : 4)) & 15 : bpp == 1 ? (s[x / 8] >> (7 - (x & 7))) & 1 : 0;
                if (i < nclr && (size_t)(cbfix + (i + 1) * (uint32_t)pe) <= in) c = (uint32_t)pal[pe * i + 2] << 16 | (uint32_t)pal[pe * i + 1] << 8 | pal[pe * i];
            }
            d[x] = 0xFF000000u | c;
        }
    }
    return 0;
}
static uint32_t pal_size(const uint8_t *info)
{
    uint32_t cbfix = info[0] | info[1] << 8 | info[2] << 16 | (uint32_t)info[3] << 24;
    int old = cbfix == 12, bpp = old ? (info[10] | info[11] << 8) : (info[14] | info[15] << 8);
    uint32_t nclr = !old && cbfix >= 36 ? (uint32_t)(info[32] | info[33] << 8 | info[34] << 16 | (uint32_t)info[35] << 24) : 0;
    if (bpp <= 8 && !nclr) nclr = 1u << bpp;
    return bpp <= 8 ? nclr * (old ? 3u : 4u) : 0;
}

/* a bitmap: a hidden "window" of w x h pixels (rows top-down there) */
static pmwin_t *bmp_new(int w, int h)
{
    pmwin_t *b = win_new(0, "#bitmap", 0, 0);
    if (!b) return 0;
    b->top = 1;
    b->fixed = 1;
    frame_pixels(b, w, h);
    return b;
}
static pmwin_t *bmp_get(uint32_t h) { pmwin_t *b = win(h); return b && !strcmp(b->cls, "#bitmap") ? b : 0; }
/* a BMP file's (or a bitmap array's first) image, as a bitmap */
static pmwin_t *bmp_from_file(const uint8_t *d, size_t n)
{
    size_t o = 0;
    if (n >= 14 && d[0] == 'B' && d[1] == 'A') o = 14;      /* BFT_BITMAPARRAY: its first element */
    if (o + 14 + 16 > n || d[o] != 'B' || d[o + 1] != 'M') return 0;
    uint32_t off = d[o + 10] | d[o + 11] << 8 | d[o + 12] << 16 | (uint32_t)d[o + 13] << 24;
    const uint8_t *info = d + o + 14;
    uint32_t cbfix = info[0] | info[1] << 8 | info[2] << 16 | (uint32_t)info[3] << 24;
    int w, h;
    if (cbfix == 12) { w = info[4] | info[5] << 8; h = info[6] | info[7] << 8; }
    else { w = (int32_t)(info[4] | info[5] << 8 | info[6] << 16 | (uint32_t)info[7] << 24); h = (int32_t)(info[8] | info[9] << 8 | info[10] << 16 | (uint32_t)info[11] << 24); }
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || off >= n) return 0;
    uint32_t *tmp = calloc((size_t)w * (size_t)h, 4);
    if (!tmp || bits_decode(d + off, n - off, info, n - (size_t)(info - d), w, h, tmp, w, 0)) { free(tmp); return 0; }
    pmwin_t *b = bmp_new(w, h);
    if (b) for (int y = 0; y < h; y++) memcpy(b->px + (size_t)(h - 1 - y) * (size_t)w, tmp + (size_t)y * (size_t)w, (size_t)w * 4);
    free(tmp);
    return b;
}
static uint32_t getpel(pmwin_t *w, int x, int y)          /* a window's (or a bitmap's) pixel, PM's way up */
{
    pmwin_t *f = frame_of(w);
    if (!f || !f->px || x < 0 || y < 0 || x >= w->cx || y >= w->cy) return 0;
    int ax, ay;
    win_abs(w, &ax, &ay);
    int fx = ax + x, fy = ay + y;
    if (fx < 0 || fy < 0 || fx >= f->pw || fy >= f->ph) return 0;
    return f->px[(f->ph - 1 - fy) * f->pw + fx] & 0xFFFFFF;
}
static uint32_t rop(uint32_t r, uint32_t s, uint32_t d, uint32_t p)
{
    switch (r) {
    case 0xCC: return s;                                       /* ROP_SRCCOPY */
    case 0x33: return ~s & 0xFFFFFF;                           /* ROP_NOTSRCCOPY */
    case 0x88: return s & d;                                   /* ROP_SRCAND */
    case 0xEE: return s | d;                                   /* ROP_SRCPAINT */
    case 0x66: return s ^ d;                                   /* ROP_SRCINVERT */
    case 0x44: return s & ~d & 0xFFFFFF;                       /* ROP_SRCERASE */
    case 0x11: return ~(s | d) & 0xFFFFFF;                     /* ROP_NOTSRCERASE */
    case 0xBB: return (~s | d) & 0xFFFFFF;                     /* ROP_MERGEPAINT */
    case 0xC0: return s & p;                                   /* ROP_MERGECOPY */
    case 0x55: return ~d & 0xFFFFFF;                           /* ROP_DSTINVERT */
    case 0xF0: return p;                                       /* ROP_PATCOPY */
    case 0x5A: return p ^ d;                                   /* ROP_PATINVERT */
    case 0xFB: return (p | ~s | d) & 0xFFFFFF;                 /* ROP_PATPAINT */
    case 0x00: return 0;                                       /* ROP_ZERO */
    case 0xFF: return 0xFFFFFF;                                /* ROP_ONE */
    }
    return s;
}
/* [tx0, tx1) x [ty0, ty1) of t from [sx0, sx1) x [sy0, sy1) of s (stretched) */
static void blit(pmwin_t *t, int tx0, int ty0, int tx1, int ty1, pmwin_t *s, int sx0, int sy0, int sx1, int sy1, uint32_t r, uint32_t pat)
{
    if (tx1 < tx0) { int k = tx0; tx0 = tx1; tx1 = k; }
    if (ty1 < ty0) { int k = ty0; ty0 = ty1; ty1 = k; }
    int tw = tx1 - tx0, th = ty1 - ty0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (tw <= 0 || th <= 0) return;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            uint32_t sv = s ? getpel(s, sx0 + (sw > 0 ? x * sw / tw : x), sy0 + (sh > 0 ? y * sh / th : y)) : 0;
            pel(t, tx0 + x, ty0 + y, rop(r, sv, getpel(t, tx0 + x, ty0 + y), pat));
        }
}

static uint32_t api_GpiLoadBitmap(void)           /* (hps, hmod, idBitmap, lWidth, lHeight) */
{
    uint32_t a, n;
    if (!find_resource(A(1), 2, A(2), &a, &n) || !ok(a, n)) return 0;   /* RT_BITMAP */
    pmwin_t *b = bmp_from_file(C.mem + a, n);
    if (!b) return 0;
    int w = (int32_t)A(3), h = (int32_t)A(4);
    if (w > 0 && h > 0 && (w != b->cx || h != b->cy)) {      /* stretched to the size asked */
        pmwin_t *s = bmp_new(w, h);
        if (s) { blit(s, 0, 0, w, h, b, 0, 0, b->cx, b->cy, 0xCC, 0); win_destroy(b); b = s; }
    }
    return b->hwnd;
}
static uint32_t api_GpiDeleteBitmap(void) { pmwin_t *b = bmp_get(A(0)); if (!b) return 0; win_destroy(b); return 1; }
static uint32_t api_GpiQueryBitmapParameters(void)    /* (hbm, pbmpData: BITMAPINFOHEADER) */
{
    pmwin_t *b = bmp_get(A(0));
    uint32_t p = A(1);
    if (!b || !ok(p, 12)) return 0;
    p32(p, 12); p16(p + 4, (uint16_t)b->cx); p16(p + 6, (uint16_t)b->cy); p16(p + 8, 1); p16(p + 10, 24);
    return 1;
}
static uint32_t api_GpiQueryBitmapInfoHeader(void)    /* (hbm, pbmpData: BITMAPINFOHEADER2, its cbFix set) */
{
    pmwin_t *b = bmp_get(A(0));
    uint32_t p = A(1), cb = g32(p);
    if (!b || cb < 16 || !ok(p, cb)) return 0;
    memset(C.mem + p + 4, 0, cb - 4);
    p32(p + 4, (uint32_t)b->cx); p32(p + 8, (uint32_t)b->cy); p16(p + 12, 1); p16(p + 14, 24);
    return 1;
}
static uint32_t api_GpiCreateBitmap(void)     /* (hps, pbmpNew, flOptions, pbInitData, pbmiInfoTable) */
{
    uint32_t p = A(1), cbfix = g32(p);
    int w = cbfix == 12 ? g16(p + 4) : (int32_t)g32(p + 4), h = cbfix == 12 ? g16(p + 6) : (int32_t)g32(p + 8);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    pmwin_t *b = bmp_new(w, h);
    if (!b) return 0;
    for (int i = 0; i < w * h; i++) b->px[i] = 0xFF000000u;
    if ((A(2) & 4) && A(3) && A(4)) {                        /* CBM_INIT */
        uint32_t info = A(4), icb = g32(info);
        uint32_t *tmp = calloc((size_t)w * (size_t)h, 4);
        if (tmp && ok(info, icb + pal_size(C.mem + info)) && ok(A(3), 1)) {
            bits_decode(C.mem + A(3), C.size - A(3), C.mem + info, icb + pal_size(C.mem + info), w, h, tmp, w, 0);
            for (int y = 0; y < h; y++) memcpy(b->px + (size_t)(h - 1 - y) * (size_t)w, tmp + (size_t)y * (size_t)w, (size_t)w * 4);
        }
        free(tmp);
    }
    return b->hwnd;
}

/* device contexts: memory ones (a PS of theirs draws into the bitmap
 * selected), a window's */
typedef struct { int used, memory; uint32_t hwnd; } pmdc_t;
static pmdc_t pmdcs[64];
static pmdc_t *dc_get(uint32_t h) { h -= 0xD000; return h < 64 && pmdcs[h].used ? &pmdcs[h] : 0; }
static uint32_t dc_new(int memory, uint32_t hwnd)
{
    for (int i = 0; i < 64; i++) if (!pmdcs[i].used) { pmdcs[i] = (pmdc_t){ 1, memory, hwnd }; return 0xD000 + (uint32_t)i; }
    return 0;
}
static uint32_t api_DevOpenDC(void) { return dc_new(A(1) == 8, 0); }   /* (hab, lType: OD_MEMORY, ...) */
static uint32_t api_DevCloseDC(void) { pmdc_t *d = dc_get(A(0)); if (!d) return 0; d->used = 0; return 1; }
static uint32_t api_WinQueryWindowDC(void) { pmwin_t *w = win(A(0)); return w ? dc_new(0, w->hwnd) : 0; }
static uint32_t api_DevQueryCaps(void)            /* (hdc, lStart, lCount, alArray) */
{
    uint32_t s = A(1), n = A(2), p = A(3);
    if (!ok(p, n * 4)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = s + i, v = 0;
        switch (k) {
        case 4: v = 1920; break;                 /* CAPS_WIDTH */
        case 5: v = 1080; break;                 /* CAPS_HEIGHT */
        case 8: v = 3780; break;                 /* (pels a metre) */
        case 9: v = 3780; break;
        case 12: v = 1; break;                   /* CAPS_COLOR_PLANES */
        case 13: v = 24; break;                  /* CAPS_COLOR_BITCOUNT */
        case 14: v = 16777216; break;            /* CAPS_COLORS */
        case 15: v = 16777216; break;
        }
        p32(p + 4 * i, v);
    }
    return 1;
}
static uint32_t api_GpiCreatePS(void)             /* (hab, hdc, psizlSize, flOptions) */
{
    pmdc_t *d = dc_get(A(1));
    for (int i = 0; i < 64; i++)
        if (!pmpss[i].used) {
            pmpss[i] = (pmps_t){ 1, d && !d->memory ? d->hwnd : 0, -3, -3, 0, 0, 0, 3 };
            return 0xA000 + (uint32_t)i;
        }
    return 0;
}
static uint32_t api_GpiDestroyPS(void) { pmps_t *ps = ps_get(A(0)); if (!ps) return 0; ps->used = 0; return 1; }
static uint32_t api_GpiAssociate(void)
{
    pmps_t *ps = ps_get(A(0));
    pmdc_t *d = dc_get(A(1));
    if (!ps) return 0;
    if (d && !d->memory) ps->hwnd = d->hwnd;
    return 1;
}
static uint32_t api_GpiSetBitmap(void)            /* (hps, hbm): the one before */
{
    pmps_t *ps = ps_get(A(0));
    if (!ps) return 0xFFFFFFFF;                   /* HBM_ERROR */
    uint32_t old = bmp_get(ps->hwnd) ? ps->hwnd : 0;
    ps->hwnd = bmp_get(A(1)) ? A(1) : 0;
    return old;
}
static uint32_t api_GpiQueryPS(void) { if (A(1)) { p32(A(1), 0); p32(A(1) + 4, 0); } return 8; }   /* PU_PELS */
static uint32_t api_GpiResetPS(void) { pmps_t *ps = ps_get(A(0)); if (ps) { ps->color = ps->back = -3; ps->x = ps->y = 0; } return 1; }
static uint32_t api_WinGetScreenPS(void)
{
    for (int i = 0; i < 64; i++) if (!pmpss[i].used) { pmpss[i] = (pmps_t){ 1, 0, -3, -3, 0, 0, 0, 2 }; return 0xA000 + (uint32_t)i; }
    return 0;
}
static uint32_t api_GpiSetBitmapBits(void)        /* (hps, lScanStart, lScans, pbBuffer, pbmiInfoTable) */
{
    pmps_t *ps = ps_get(A(0));
    pmwin_t *b = ps ? bmp_get(ps->hwnd) : 0;
    uint32_t info = A(4), icb = g32(info);
    int start = (int32_t)A(1), n = (int32_t)A(2);
    if (!b || !ok(info, icb) || !ok(A(3), 1)) return 0xFFFFFFFF;
    uint32_t *tmp = calloc((size_t)b->cx * (size_t)(n > 0 ? n : 1), 4);
    if (!tmp) return 0xFFFFFFFF;
    int bpp = icb == 12 ? g16(info + 10) : g16(info + 14);
    (void)bpp;
    bits_decode(C.mem + A(3), C.size - A(3), C.mem + info, icb + pal_size(C.mem + info), b->cx, n, tmp, b->cx, 0);
    for (int r = 0; r < n; r++) {
        int y = start + r;
        if (y >= 0 && y < b->cy) memcpy(b->px + (size_t)(b->cy - 1 - y) * (size_t)b->cx, tmp + (size_t)r * (size_t)b->cx, (size_t)b->cx * 4);
    }
    free(tmp);
    return (uint32_t)n;
}
static uint32_t api_GpiQueryBitmapBits(void)      /* (hps, lScanStart, lScans, pbBuffer, pbmiInfoTable): 24 or 32 bits */
{
    pmps_t *ps = ps_get(A(0));
    pmwin_t *b = ps ? bmp_get(ps->hwnd) : 0;
    uint32_t info = A(4), icb = g32(info);
    int start = (int32_t)A(1), n = (int32_t)A(2);
    if (!b || !ok(info, icb)) return 0xFFFFFFFF;
    int bpp = icb == 12 ? g16(info + 10) : g16(info + 14);
    if (bpp != 24 && bpp != 32) { bpp = 24; if (icb == 12) p16(info + 10, 24); else p16(info + 14, 24); }
    uint32_t rowb = (uint32_t)((b->cx * bpp + 31) / 32 * 4);
    if (!ok(A(3), rowb * (uint32_t)(n > 0 ? n : 1))) return 0xFFFFFFFF;
    int k = 0;
    for (int r = 0; r < n; r++, k++) {
        int y = start + r;
        if (y < 0 || y >= b->cy) break;
        uint8_t *d = C.mem + A(3) + rowb * (uint32_t)r;
        memset(d, 0, rowb);
        for (int x = 0; x < b->cx; x++) {
            uint32_t c = b->px[(size_t)(b->cy - 1 - y) * (size_t)b->cx + (size_t)x];
            uint8_t *q = d + x * (bpp / 8);
            q[0] = (uint8_t)c; q[1] = (uint8_t)(c >> 8); q[2] = (uint8_t)(c >> 16);
        }
    }
    if (icb >= 16) { p32(info + 4, (uint32_t)b->cx); p32(info + 8, (uint32_t)b->cy); }
    else { p16(info + 4, (uint16_t)b->cx); p16(info + 6, (uint16_t)b->cy); }
    return (uint32_t)k;
}
static uint32_t api_WinDrawBitmap(void)       /* (hpsDst, hbm, pwrcSrc, pptlDst, clrFore, clrBack, fl) */
{
    pmps_t *ps = ps_get(A(0));
    pmwin_t *t = ps_win(ps), *b = bmp_get(A(1));
    uint32_t src = A(2), dst = A(3), fl = A(6);
    if (!t || !b || !ok(dst, 8)) return 0;
    int sx0 = 0, sy0 = 0, sx1 = b->cx, sy1 = b->cy;
    if (src && ok(src, 16)) { sx0 = (int32_t)g32(src); sy0 = (int32_t)g32(src + 4); sx1 = (int32_t)g32(src + 8); sy1 = (int32_t)g32(src + 12); }
    int tx0 = (int32_t)g32(dst), ty0 = (int32_t)g32(dst + 4), tx1, ty1;
    if ((fl & 4) && ok(dst, 16)) { tx1 = (int32_t)g32(dst + 8); ty1 = (int32_t)g32(dst + 12); }   /* DBM_STRETCH: a rectangle */
    else { tx1 = tx0 + (sx1 - sx0); ty1 = ty0 + (sy1 - sy0); }
    blit(t, tx0, ty0, tx1, ty1, b, sx0, sy0, sx1, sy1, (fl & 1) ? 0x33 : 0xCC, 0);   /* (DBM_INVERT) */
    present(t);
    return 1;
}
static uint32_t bitblt(pmps_t *tps, pmwin_t *s, uint32_t n, uint32_t pts, uint32_t r)
{
    pmwin_t *t = ps_win(tps);
    if (!t || n < 2 || !ok(pts, n * 8)) return 0;
    int v[8] = { 0 };
    for (uint32_t i = 0; i < n * 2 && i < 8; i++) v[i] = (int32_t)g32(pts + 4 * i);
    int sx0 = v[4], sy0 = v[5], sx1, sy1;
    if (n >= 4) { sx1 = v[6]; sy1 = v[7]; }
    else { sx1 = sx0 + (v[2] - v[0]); sy1 = sy0 + (v[3] - v[1]); }
    if (n < 3) { sx0 = sy0 = sx1 = sy1 = 0; s = 0; }
    blit(t, v[0], v[1], v[2], v[3], s, sx0, sy0, sx1, sy1, r & 0xFF, pm_rgb(tps->color, tps->rgb, 0));
    present(t);
    return 1;                                     /* GPI_OK */
}
static uint32_t api_GpiBitBlt(void)           /* (hpsTarget, hpsSource, lCount, aptlPoints, lRop, flOptions) */
{
    pmps_t *s = ps_get(A(1));
    return bitblt(ps_get(A(0)), s ? ps_win(s) : 0, A(2), A(3), A(4));
}
static uint32_t api_GpiWCBitBlt(void) { return bitblt(ps_get(A(0)), bmp_get(A(1)), A(2), A(3), A(4)); }   /* (hpsTarget, hbmSource, ...) */
