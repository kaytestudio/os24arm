/* userland/os2x86/pm_gpi.inc - bitmaps, included at the end of pm.inc: bitmap
 * resources and files (OS/2 1.x and 2.x BMPs, bitmap arrays; 1, 4, 8, 24 and
 * 32 bpp), GpiCreateBitmap and its bits, WinDrawBitmap, GpiBitBlt and
 * GpiWCBitBlt (the usual ROPs, stretching), and memory DCs (DevOpenDC,
 * GpiCreatePS, GpiSetBitmap). A bitmap is a window that's never shown, so
 * anything that draws into windows also draws into bitmaps; a memory PS is
 * just a PS on that window. */

/* decode bits at b into px (bottom-up rows, as in the file). The info header
 * is at info (cbFix 12 means OS/2 1.x, else 2.x), with the palette after it. */
static int bits_decode(const uint8_t *b, size_t bn, const uint8_t *info, size_t in, int w, int h, uint32_t *px, int pw, int row0)
{
    if (in < 16) return -1;
    uint32_t cbfix = info[0] | info[1] << 8 | info[2] << 16 | (uint32_t)info[3] << 24;
    int old = cbfix == 12, bpp = old ? (info[10] | info[11] << 8) : (info[14] | info[15] << 8);
    uint32_t comp = !old && cbfix >= 20 ? (uint32_t)(info[16] | info[17] << 8 | info[18] << 16 | (uint32_t)info[19] << 24) : 0;
    uint32_t nclr = !old && cbfix >= 36 ? (uint32_t)(info[32] | info[33] << 8 | info[34] << 16 | (uint32_t)info[35] << 24) : 0;
    if (comp) return -1;                                       /* only BCA_UNCOMP for now */
    if (bpp <= 8 && !nclr) nclr = 1u << bpp;
    const uint8_t *pal = info + cbfix;
    int pe = old ? 3 : 4;
    uint32_t rowb = (uint32_t)((w * bpp + 31) / 32 * 4);
    for (int r = 0; r < h; r++) {
        if ((size_t)(r + 1) * rowb > bn) break;
        const uint8_t *s = b + (size_t)r * rowb;
        int y = row0 + r;                                      /* from the bottom */
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

/* a bitmap: a hidden w x h "window" (rows top-down) */
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
/* the image of a BMP file, or the first one of a bitmap array */
static pmwin_t *bmp_from_file(const uint8_t *d, size_t n)
{
    size_t o = 0;
    if (n >= 14 && d[0] == 'B' && d[1] == 'A') o = 14;      /* BFT_BITMAPARRAY: use its first element */
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
static uint32_t getpel(pmwin_t *w, int x, int y)          /* pixel of a window or bitmap, y counting up like PM */
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
    case ROP_SRCCOPY: return s;
    case ROP_NOTSRCCOPY: return ~s & 0xFFFFFF;
    case ROP_SRCAND: return s & d;
    case ROP_SRCPAINT: return s | d;
    case ROP_SRCINVERT: return s ^ d;
    case ROP_SRCERASE: return s & ~d & 0xFFFFFF;
    case ROP_NOTSRCERASE: return ~(s | d) & 0xFFFFFF;
    case ROP_MERGEPAINT: return (~s | d) & 0xFFFFFF;
    case ROP_MERGECOPY: return s & p;
    case ROP_DSTINVERT: return ~d & 0xFFFFFF;
    case ROP_PATCOPY: return p;
    case ROP_PATINVERT: return p ^ d;
    case ROP_PATPAINT: return (p | ~s | d) & 0xFFFFFF;
    case ROP_ZERO: return 0;
    case ROP_ONE: return 0xFFFFFF;
    }
    return s;
}
/* copy [sx0, sx1) x [sy0, sy1) of s to [tx0, tx1) x [ty0, ty1) of t, stretching */
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
    if (!find_resource(A(1), RT_BITMAP, A(2), &a, &n) || !ok(a, n)) return 0;
    pmwin_t *b = bmp_from_file(C.mem + a, n);
    if (!b) return 0;
    int w = (int32_t)A(3), h = (int32_t)A(4);
    if (w > 0 && h > 0 && (w != b->cx || h != b->cy)) {      /* stretch to the requested size */
        pmwin_t *s = bmp_new(w, h);
        if (s) { blit(s, 0, 0, w, h, b, 0, 0, b->cx, b->cy, ROP_SRCCOPY, 0); win_destroy(b); b = s; }
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
    if ((A(2) & CBM_INIT) && A(3) && A(4)) {
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

/* device contexts: memory DCs (a PS on one draws into its selected bitmap) and window DCs */
typedef struct { int used, memory; uint32_t hwnd; int printer; } pmdc_t;   /* printer: hwnd is its page (pm_print.inc) */
static pmdc_t pmdcs[64];
static pmdc_t *dc_get(uint32_t h) { h -= 0xD000; return h < 64 && pmdcs[h].used ? &pmdcs[h] : 0; }
static uint32_t dc_new(int memory, uint32_t hwnd)
{
    for (int i = 0; i < 64; i++) if (!pmdcs[i].used) { pmdcs[i] = (pmdc_t){ 1, memory, hwnd, 0 }; return 0xD000 + (uint32_t)i; }
    return 0;
}
static uint32_t api_DevOpenDC(void)               /* (hab, lType, ...): OD_MEMORY, or OD_QUEUED / OD_DIRECT / OD_INFO for the printer page */
{
    uint32_t h = dc_new(A(1) == 8, 0);
    pmdc_t *d = dc_get(h);
    if (d && (A(1) == 2 || A(1) == 5 || A(1) == 6)) {
        pmwin_t *b = bmp_new(1240, 1754);                    /* PR_W x PR_H from pm_print.inc */
        if (!b) { d->used = 0; return 0; }
        for (int i = 0; i < b->pw * b->ph; i++) b->px[i] = 0xFFFFFFFFu;
        d->printer = 1; d->hwnd = b->hwnd;
    }
    return h;
}
static uint32_t api_DevCloseDC(void)
{
    pmdc_t *d = dc_get(A(0));
    if (!d) return 0;
    if (d->printer && win(d->hwnd)) win_destroy(win(d->hwnd));
    d->used = 0;
    return 1;
}
static uint32_t api_WinQueryWindowDC(void) { pmwin_t *w = win(A(0)); return w ? dc_new(0, w->hwnd) : 0; }
static uint32_t api_WinOpenWindowDC(void) { pmwin_t *w = win(A(0)); return w ? dc_new(0, w->hwnd) : 0; }   /* (hwnd): a DC for GpiCreatePS */
static uint32_t api_DevQueryCaps(void)            /* (hdc, lStart, lCount, alArray) */
{
    uint32_t s = A(1), n = A(2), p = A(3);
    if (!ok(p, n * 4)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = s + i, v = 0;
        pmdc_t *d = dc_get(A(0));
        int pr = d && d->printer;
        switch (k) {
        case CAPS_TECHNOLOGY: v = pr ? CAPS_TECH_RASTER_PRINTER : CAPS_TECH_RASTER_DISPLAY; break;
        case CAPS_WIDTH: v = pr ? 1240 : 1920; break;
        case CAPS_HEIGHT: v = pr ? 1754 : 1080; break;
        case CAPS_HORIZONTAL_RESOLUTION: case CAPS_VERTICAL_RESOLUTION: v = pr ? 5906 : 3780; break;   /* pels per metre */
        case CAPS_COLORS: v = 16777216; break;
        case CAPS_COLOR_PLANES: v = 1; break;
        case CAPS_COLOR_BITCOUNT: v = 24; break;
        }
        p32(p + 4 * i, v);
    }
    return 1;
}
static void geo_set_units(pmps_t *ps, uint32_t opts, uint32_t psizl, int dpm);
static uint32_t api_GpiCreatePS(void)             /* (hab, hdc, psizlSize, flOptions) */
{
    pmdc_t *d = dc_get(A(1));
    for (int i = 0; i < 64; i++)
        if (!pmpss[i].used) {
            geo_free(&pmpss[i]);
            pmpss[i] = (pmps_t){ 1, d && !d->memory ? d->hwnd : 0, -3, -3, 0, 0, 0, 3, 0, 0, 0, 0, 0 };
            geo_set_units(&pmpss[i], A(3), A(2), d && d->printer ? 5906 : 3780);   /* page units at the DC's resolution, and its page size */
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
static uint32_t api_GpiSetBitmap(void)            /* (hps, hbm); returns the previous one */
{
    pmps_t *ps = ps_get(A(0));
    if (!ps) return (uint32_t)HBM_ERROR;
    uint32_t old = bmp_get(ps->hwnd) ? ps->hwnd : 0;
    ps->hwnd = bmp_get(A(1)) ? A(1) : 0;
    return old;
}
static uint32_t gpi_query_ps(pmps_t *ps, uint32_t psizl);
static uint32_t api_GpiQueryPS(void) { pmps_t *ps = ps_get(A(0)); if (!ps) return 0; return gpi_query_ps(ps, A(1)); }   /* (hps, psizl); returns its units */
static uint32_t api_GpiResetPS(void) { pmps_t *ps = ps_get(A(0)); if (ps) { ps->color = ps->back = -3; ps->x = ps->y = 0; geo_free(ps); } return 1; }
static uint32_t api_WinGetScreenPS(void)
{
    for (int i = 0; i < 64; i++) if (!pmpss[i].used) { geo_free(&pmpss[i]); pmpss[i] = (pmps_t){ 1, 0, -3, -3, 0, 0, 0, 2, 0, 0, 0, 0, 0 }; return 0xA000 + (uint32_t)i; }
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
static uint32_t api_GpiQueryBitmapBits(void)      /* (hps, lScanStart, lScans, pbBuffer, pbmiInfoTable): 24 or 32 bpp */
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
    if ((fl & DBM_STRETCH) && ok(dst, 16)) { tx1 = (int32_t)g32(dst + 8); ty1 = (int32_t)g32(dst + 12); }   /* a target rectangle */
    else { tx1 = tx0 + (sx1 - sx0); ty1 = ty0 + (sy1 - sy0); }
    blit(t, tx0, ty0, tx1, ty1, b, sx0, sy0, sx1, sy1, (fl & DBM_INVERT) ? ROP_NOTSRCCOPY : ROP_SRCCOPY, 0);
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
    return GPI_OK;
}
static uint32_t api_GpiBitBlt(void)           /* (hpsTarget, hpsSource, lCount, aptlPoints, lRop, flOptions) */
{
    pmps_t *s = ps_get(A(1));
    return bitblt(ps_get(A(0)), s ? ps_win(s) : 0, A(2), A(3), A(4));
}
static uint32_t api_GpiWCBitBlt(void) { return bitblt(ps_get(A(0)), bmp_get(A(1)), A(2), A(3), A(4)); }   /* (hpsTarget, hbmSource, ...) */

/* ── icons and pointers ──
 * ICON/POINTER resources are bitmap arrays (BA); we take the device-
 * independent one (cxDisplay 0). Each has a 1-bit mask twice as tall (AND
 * mask first, XOR mask second), and colour ones (CI, CP) a colour image
 * after it. AND 0 shows the image (for mono: black or white by XOR), AND 1
 * XOR 0 is transparent, AND 1 XOR 1 inverts the screen. A pointer is a
 * hidden window holding the image, with the masks next to it. */
typedef struct { uint8_t *andm, *xorm; int mono, hotx, hoty; } ptr_t;   /* hot spot from the top left */
static pmwin_t *ptr_get(uint32_t h) { pmwin_t *p = win(h); return p && !strcmp(p->cls, "#pointer") ? p : 0; }
static pmwin_t *ptr_new(int w, int h, int mono)
{
    pmwin_t *p = win_new(0, "#pointer", 0, 0);
    if (!p) return 0;
    p->top = 1; p->fixed = 1;
    frame_pixels(p, w, h);
    ptr_t *t = calloc(1, sizeof *t);
    t->andm = calloc((size_t)w * (size_t)h, 1);
    t->xorm = calloc((size_t)w * (size_t)h, 1);
    t->mono = mono;
    p->ctl = t;
    return p;
}
/* as ARGB for the desktop pointer: inverted parts become a shadow */
static __attribute__((unused)) void ptr_argb(pmwin_t *p, uint32_t *out, int *hx, int *hy)
{
    ptr_t *m = p->ctl;
    int n = p->pw * p->ph;
    for (int i = 0; i < n && i < p->cx * p->cy; i++) {
        int a = m->andm[i], xo = m->xorm[i];
        out[i] = a && !xo ? 0 : a && xo ? 0x80000000u : 0xFF000000u | (m->mono ? (xo ? 0xFFFFFF : 0) : p->px[i] & 0xFFFFFF);
    }
    *hx = m->hotx; *hy = m->hoty;
}
static void ptr_free(pmwin_t *p)
{
    ptr_t *t = p ? p->ctl : 0;
    if (t) { free(t->andm); free(t->xorm); }
    if (p) win_destroy(p);
}
static uint32_t rd32le(const uint8_t *d) { return d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24; }
static pmwin_t *ptr_from_file(const uint8_t *d, size_t n)
{
    size_t h = 0;
    if (n >= 14 && d[0] == 'B' && d[1] == 'A') {             /* an array: take the device-independent image, else the first */
        size_t o = 0, pick = 14;
        for (int k = 0; k < 16 && o + 14 <= n; k++) {
            if (d[o] != 'B' || d[o + 1] != 'A') break;
            if (!(d[o + 10] | d[o + 11] | d[o + 12] | d[o + 13])) { pick = o + 14; break; }
            uint32_t next = rd32le(d + o + 6);
            if (!next || next <= o) break;
            o = next;
        }
        h = pick;
    }
    if (h + 14 + 12 > n) return 0;
    int color = (d[h] == 'C' && (d[h + 1] == 'I' || d[h + 1] == 'P'));
    if (!color && !((d[h] == 'I' && d[h + 1] == 'C') || (d[h] == 'P' && d[h + 1] == 'T'))) return 0;
    const uint8_t *mi = d + h + 14;
    uint32_t mfix = rd32le(mi), moff = rd32le(d + h + 10);
    int w = mfix == 12 ? (mi[4] | mi[5] << 8) : (int)rd32le(mi + 4), h2 = mfix == 12 ? (mi[6] | mi[7] << 8) : (int)rd32le(mi + 8);
    int ih = h2 / 2;
    if (w <= 0 || ih <= 0 || w > 256 || ih > 256) return 0;
    uint32_t mrow = (uint32_t)((w + 31) / 32 * 4);
    if (moff + mrow * (uint32_t)h2 > n) return 0;
    pmwin_t *p = ptr_new(w, ih, !color);
    if (!p) return 0;
    ptr_t *t = p->ctl;
    t->hotx = (int16_t)(d[h + 6] | d[h + 7] << 8);
    t->hoty = ih - 1 - (int16_t)(d[h + 8] | d[h + 9] << 8);   /* the file counts from the bottom */
    for (int r = 0; r < ih; r++)                              /* r counts from the bottom */
        for (int x = 0; x < w; x++) {
            int ty = ih - 1 - r;
            t->andm[ty * w + x] = (d[moff + (uint32_t)r * mrow + (uint32_t)x / 8] >> (7 - (x & 7))) & 1;
            t->xorm[ty * w + x] = (d[moff + (uint32_t)(r + ih) * mrow + (uint32_t)x / 8] >> (7 - (x & 7))) & 1;
        }
    if (color) {                                              /* colour image: its header follows the mask's */
        size_t ch = h + 14 + mfix + 2 * (mfix == 12 ? 3 : 4);
        if (ch + 14 + 12 <= n) {
            uint32_t coff = rd32le(d + ch + 10);
            const uint8_t *ci = d + ch + 14;
            uint32_t *tmp = calloc((size_t)w * (size_t)ih, 4);
            if (tmp && coff < n && !bits_decode(d + coff, n - coff, ci, n - (size_t)(ci - d), w, ih, tmp, w, 0))
                for (int y = 0; y < ih; y++) memcpy(p->px + (size_t)(ih - 1 - y) * (size_t)w, tmp + (size_t)y * (size_t)w, (size_t)w * 4);
            free(tmp);
        }
    }
    return p;
}
/* draw pointer p at (x, y), its bottom left, in window t; half size for DP_MINI */
static void ptr_draw(pmwin_t *t, int x, int y, pmwin_t *p, uint32_t fs)
{
    ptr_t *m = p->ctl;
    int st = (fs & 4) ? 2 : 1, w = p->cx, h = p->cy;
    for (int ty = 0; ty < h; ty += st)
        for (int tx = 0; tx < w; tx += st) {
            int i = ty * w + tx, px = x + tx / st, py = y + (h - 1 - ty) / st;
            int a = m->andm[i], xo = m->xorm[i];
            uint32_t c;
            if (a && !xo) continue;                                         /* transparent */
            if (a && xo) c = ~getpel(t, px, py) & 0xFFFFFF;                 /* invert the screen */
            else c = m->mono ? (xo ? 0xFFFFFF : 0) : p->px[i] & 0xFFFFFF;
            if ((fs & DP_INVERTED) && !(a && xo)) c = ~c & 0xFFFFFF;
            if ((fs & DP_HALFTONED) && ((px + py) & 1)) continue;
            pel(t, px, py, c);
        }
}
static void disc(pmwin_t *p, int cx, int cy, int r, uint32_t c, ptr_t *m)   /* filled circle, marked opaque in the pointer's mask */
{
    for (int y = 0; y < p->cy; y++)
        for (int x = 0; x < p->cx; x++) {
            int dx = 2 * x + 1 - 2 * cx, dy = 2 * y + 1 - 2 * cy;
            if (dx * dx + dy * dy <= 4 * r * r) { p->px[y * p->cx + x] = 0xFF000000u | c; if (m) m->andm[y * p->cx + x] = 0; }
        }
}
/* system pointers, drawn here (SPTR_ICONINFORMATION .. ICONWARNING, the arrow) */
static uint32_t sys_ptrs[32];
static uint32_t sys_pointer(uint32_t id)
{
    if (id >= 32) return 0;
    if (win(sys_ptrs[id])) return sys_ptrs[id];
    pmwin_t *p = ptr_new(32, 32, 0);
    if (!p) return 0;
    ptr_t *m = p->ctl;
    memset(m->andm, 1, 32 * 32);                               /* start all transparent, then draw the shape */
    const char *mark = 0;
    uint32_t bg = 0, fg = 0xFFFFFF;
    switch (id) {
    case SPTR_ICONINFORMATION: bg = 0x2050C0; mark = "i"; break;
    case SPTR_ICONQUESTION: bg = 0x2050C0; mark = "?"; break;
    case SPTR_ICONERROR: bg = 0xC02020; mark = "X"; break;
    case SPTR_ILLEGAL:                                         /* red ring with a bar through it */
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
            int dx = 2 * x + 1 - 32, dy = 2 * y + 1 - 32, d2 = dx * dx + dy * dy, slash = abs(dx + dy) <= 5;
            if (d2 <= 26 * 26 && (d2 >= 18 * 18 || slash)) { p->px[y * 32 + x] = 0xFFD02020u; m->andm[y * 32 + x] = 0; }
            else if (d2 <= 28 * 28 && (d2 >= 16 * 16 || abs(dx + dy) <= 8)) { p->px[y * 32 + x] = 0xFFFFFFFFu; m->andm[y * 32 + x] = 0; }
        }
        m->hotx = m->hoty = 16;
        break;
    case SPTR_FILE: case SPTR_MULTFILE:                        /* a page with a folded corner */
        for (int y = 2; y < 30; y++) for (int x = 7; x < 25; x++) {
            int edge = x == 7 || x == 24 || y == 2 || y == 29, corner = x > 18 && y < 8 && x - 18 > y - 2;
            if (corner) continue;
            p->px[y * 32 + x] = edge || (x > 18 && y == 8) || (x == 19 && y < 8) ? 0xFF000000u : (y > 10 && y % 4 == 0 && x > 9 && x < 22 ? 0xFF7080A0u : 0xFFFFFFFFu);
            m->andm[y * 32 + x] = 0;
        }
        break;
    case SPTR_FOLDER:
        for (int y = 6; y < 28; y++) for (int x = 3; x < 29; x++) {
            if (y < 10 && x > 14) continue;
            int edge = x == 3 || x == 28 || y == 6 || y == 27 || (y == 10 && x > 14) || (x == 14 && y < 10);
            p->px[y * 32 + x] = edge ? 0xFF806000u : 0xFFF0D060u;
            m->andm[y * 32 + x] = 0;
        }
        break;
    case SPTR_ICONWARNING:                                     /* a triangle */
        for (int y = 2; y < 30; y++) for (int x = 16 - (y - 2) / 2 - 1; x <= 16 + (y - 2) / 2; x++)
            if (x >= 0 && x < 32) { p->px[y * 32 + x] = 0xFFF0C800u; m->andm[y * 32 + x] = 0; }
        mark = "!"; fg = 0; break;
    default: {                                                 /* the arrow, also used for the rest for now */
        static const char *const arrow[] = {
            "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X", "X........X",
            "X.....XXXXX", "X..X..X", "X.X X..X", "XX  X..X", "X    X..X", "     X..X", "      X..X", "      XXX" };
        for (int y = 0; y < 18; y++)
            for (int x = 0; arrow[y][x]; x++)
                if (arrow[y][x] != ' ') { p->px[y * 32 + x] = arrow[y][x] == 'X' ? 0xFF000000u : 0xFFFFFFFFu; m->andm[y * 32 + x] = 0; }
        break;
    }
    }
    if (bg) disc(p, 16, 16, 15, bg, m);
    if (mark) {                                                /* the mask stays as the shape left it */
        int tw = text_w(mark, 1);
        text_at(p, 16 - tw / 2, 10, mark, 1, fg);
    }
    sys_ptrs[id] = p->hwnd;
    return p->hwnd;
}
static uint32_t api_WinLoadPointer(void)          /* (hwndDesktop, hmod, idres) */
{
    uint32_t a, n;
    if (!find_resource(A(1), RT_POINTER, A(2), &a, &n) || !ok(a, n)) return 0;
    pmwin_t *p = ptr_from_file(C.mem + a, n);
    return p ? p->hwnd : 0;
}
static uint32_t api_WinDestroyPointer(void)
{
    pmwin_t *p = ptr_get(A(0));
    if (!p) return 0;
    for (int i = 0; i < 32; i++) if (sys_ptrs[i] == p->hwnd) return 1;  /* system pointers are never destroyed */
    ptr_free(p);
    return 1;
}
static uint32_t api_WinDrawPointer(void)          /* (hps, x, y, hptr, fs) */
{
    pmps_t *ps = ps_get(A(0));
    pmwin_t *t = ps_win(ps), *p = ptr_get(A(3));
    if (!t || !p) return 0;
    ptr_draw(t, (int32_t)A(1), (int32_t)A(2), p, A(4));
    present(t);
    return 1;
}
static uint32_t api_WinQuerySysPointer(void) { return sys_pointer(A(1)); }   /* (hwndDesktop, iptr, fCopy) */
static void pm_set_pointer(uint32_t hptr);
static uint32_t pm_pointer;
static uint32_t api_WinQueryPointer(void) { return pm_pointer ? pm_pointer : sys_pointer(SPTR_ARROW); }
static uint32_t api_WinSetPointer(void) { if (!ptr_get(A(1))) return 0; pm_set_pointer(A(1)); return 1; }   /* (hwndDesktop, hptr) */
static uint32_t api_WinQueryPointerInfo(void)     /* (hptr, pptriPointerInfo) */
{
    pmwin_t *p = ptr_get(A(0));
    uint32_t q = A(1);
    if (!p || !ok(q, 28)) return 0;
    memset(C.mem + q, 0, 28);
    p32(q, ((ptr_t *)p->ctl)->mono ? 1 : 1);                 /* fPointer */
    p32(q + 4, 0); p32(q + 8, (uint32_t)p->cy / 2);
    return 1;
}
