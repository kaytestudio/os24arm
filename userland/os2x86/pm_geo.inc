/* userland/os2x86/pm_geo.inc - GPI geometry, included at the end of pm.inc:
 * lines with types, widths and mixes; arcs (GpiFullArc, GpiPartialArc,
 * GpiPointArc, with GpiSetArcParams' transform); GpiBox with rounded corners;
 * polylines, fillets, splines, polygons; areas (GpiBeginArea .. GpiEndArea)
 * and paths (GpiBeginPath .. GpiFillPath, GpiStrokePath) filled alternate or
 * winding with the pattern; markers; a clip rectangle.
 *
 * Everything ends up in geo_to(), a segment from the current position. Outside
 * an area or path it's drawn straight away; inside one it adds a point to the
 * current figure, and a move starts a new figure. */
#include <math.h>

typedef struct { int n, cap; int32_t *p; } geofig_t;    /* x, y pairs */
typedef struct {
    int32_t ap, aq, ar, as;                  /* GpiSetArcParams: P, Q, R, S */
    int ltype, lwidth, gwidth, pattern, mix, bmix, marker;
    int area, area_fl, path, path_done;      /* area: inside one, with its BA_ options; path: inside one; path_done: ended, kept for fill/stroke */
    geofig_t *fig; int nfig;
    int clip, cx0, cy0, cx1, cy1;            /* GpiIntersectClipRectangle: [cx0, cx1) x [cy0, cy1) */
    int lpos;                                /* position along the line type's pattern */
    /* world to device: model (and viewing) transform, default view, then page units / page viewport */
    double model[6], view[6], seg[6], page[6];                   /* a, b, c, d, e, f: x' = a x + c y + e, y' = b x + d y + f */
    double m[6], inv[6];                                         /* combined, and its inverse */
    int ident, units, pagecx, pagecy, dpm;
} geo_t;
static const double IDM[6] = { 1, 0, 0, 1, 0, 0 };
static void mat_mul(double *r, const double *p, const double *q)   /* r = p, then q */
{
    double t[6] = { p[0] * q[0] + p[1] * q[2], p[0] * q[1] + p[1] * q[3], p[2] * q[0] + p[3] * q[2], p[2] * q[1] + p[3] * q[3],
                    p[4] * q[0] + p[5] * q[2] + q[4], p[4] * q[1] + p[5] * q[3] + q[5] };
    memcpy(r, t, sizeof t);
}
static void geo_combine(geo_t *g)
{
    double t[6];
    mat_mul(t, g->model, g->seg);
    mat_mul(t, t, g->view);
    mat_mul(g->m, t, g->page);
    double det = g->m[0] * g->m[3] - g->m[1] * g->m[2];
    if (fabs(det) < 1e-12) det = 1e-12;
    g->inv[0] = g->m[3] / det; g->inv[1] = -g->m[1] / det; g->inv[2] = -g->m[2] / det; g->inv[3] = g->m[0] / det;
    g->inv[4] = -(g->m[4] * g->inv[0] + g->m[5] * g->inv[2]); g->inv[5] = -(g->m[4] * g->inv[1] + g->m[5] * g->inv[3]);
    g->ident = !memcmp(g->m, IDM, sizeof IDM);
}
/* device pels per page unit (3780 pels per metre, as DevQueryCaps says) */
static double unit_pels(int u)
{
    switch (u) {
    case PU_LOMETRIC: return 3780.0 / 10000;  /* 0.1 mm */
    case PU_HIMETRIC: return 3780.0 / 100000; /* 0.01 mm */
    case PU_LOENGLISH: return 3780.0 * 0.000254;      /* 0.01 inch */
    case PU_HIENGLISH: return 3780.0 * 0.0000254;     /* 0.001 inch */
    case PU_TWIPS: return 3780.0 * 0.0254 / 1440;
    default: return 1;                        /* PU_PELS, PU_ARBITRARY (until a page viewport is set) */
    }
}
static geo_t *geo(pmps_t *ps)
{
    if (!ps->geo) {
        geo_t *g = calloc(1, sizeof *g);
        g->ap = g->aq = 1; g->lwidth = 1; g->gwidth = 1; g->mix = 2; g->bmix = 5; g->marker = 1;
        memcpy(g->model, IDM, sizeof IDM); memcpy(g->view, IDM, sizeof IDM); memcpy(g->seg, IDM, sizeof IDM); memcpy(g->page, IDM, sizeof IDM);
        g->units = 8;
        geo_combine(g);
        ps->geo = g;
    }
    return ps->geo;
}
/* world point to device and back */
static void ps_to_dev(pmps_t *ps, double x, double y, int *dx, int *dy)
{
    geo_t *g = ps->geo;
    if (!g || g->ident) { *dx = (int)lround(x); *dy = (int)lround(y); return; }
    *dx = (int)lround(g->m[0] * x + g->m[2] * y + g->m[4]); *dy = (int)lround(g->m[1] * x + g->m[3] * y + g->m[5]);
}
static int ps_dpm(pmps_t *ps) { geo_t *g = ps->geo; return g && g->dpm ? g->dpm : 3780; }
static void ps_from_dev(pmps_t *ps, double x, double y, int *wx, int *wy)
{
    geo_t *g = ps->geo;
    if (!g || g->ident) { *wx = (int)lround(x); *wy = (int)lround(y); return; }
    *wx = (int)lround(g->inv[0] * x + g->inv[2] * y + g->inv[4]); *wy = (int)lround(g->inv[1] * x + g->inv[3] * y + g->inv[5]);
}
static void geo_free(pmps_t *ps)
{
    geo_t *g = ps->geo;
    if (!g) return;
    for (int i = 0; i < g->nfig; i++) free(g->fig[i].p);
    free(g->fig);
    free(g);
    ps->geo = 0;
}
static void geo_figs_clear(geo_t *g) { for (int i = 0; i < g->nfig; i++) free(g->fig[i].p); free(g->fig); g->fig = 0; g->nfig = 0; }
static void geo_fig_new(geo_t *g) { g->fig = realloc(g->fig, (size_t)(g->nfig + 1) * sizeof *g->fig); memset(&g->fig[g->nfig++], 0, sizeof *g->fig); }
static void geo_fig_add(geo_t *g, int x, int y)
{
    if (!g->nfig) geo_fig_new(g);
    geofig_t *f = &g->fig[g->nfig - 1];
    if (f->n && f->p[2 * f->n - 2] == x && f->p[2 * f->n - 1] == y) return;
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 32; f->p = realloc(f->p, (size_t)f->cap * 2 * sizeof *f->p); }
    f->p[2 * f->n] = x; f->p[2 * f->n + 1] = y; f->n++;
}

/* set a pixel with the PS's mix (FM_OVERPAINT, XOR, INVERT, OR, AND, LEAVEALONE ...) and clip */
static void geo_pel(pmps_t *ps, pmwin_t *w, int x, int y, uint32_t c, int mix)
{
    geo_t *g = ps->geo;
    if (g && g->clip && (x < g->cx0 || x >= g->cx1 || y < g->cy0 || y >= g->cy1)) return;
    switch (mix) {
    case FM_LEAVEALONE: return;
    case FM_XOR: pel(w, x, y, (getpel(w, x, y) ^ c) & 0xFFFFFF); return;
    case FM_INVERT: pel(w, x, y, ~getpel(w, x, y) & 0xFFFFFF); return;
    case FM_OR: pel(w, x, y, (getpel(w, x, y) | c) & 0xFFFFFF); return;
    case FM_AND: pel(w, x, y, getpel(w, x, y) & c); return;
    case FM_ZERO: pel(w, x, y, 0); return;
    case FM_ONE: pel(w, x, y, 0xFFFFFF); return;
    case FM_NOTXORSRC: pel(w, x, y, ~(getpel(w, x, y) ^ c) & 0xFFFFFF); return;
    default: pel(w, x, y, c);                                         /* FM_DEFAULT, FM_OVERPAINT */
    }
}
static uint32_t geo_color(pmps_t *ps) { return pm_rgb(ps->color, ps->rgb, 0); }
static int geo_mix(geo_t *g) { return g->mix ? g->mix : 2; }
/* line types as on/off runs */
static int geo_dash(geo_t *g)
{
    static const char *const pat[] = { 0, "1100", "11110000", "1111111100110000", "1010000000", "1111111111110000",
                                       "11111111001100110000", 0, "0", "10" };   /* DEFAULT DOT SHORTDASH DASHDOT DOUBLEDOT LONGDASH DASHDOUBLEDOT SOLID INVISIBLE ALTERNATE */
    const char *p = g->ltype > 0 && g->ltype < 10 ? pat[g->ltype] : 0;
    int on = !p || p[g->lpos % (int)strlen(p)] == '1';
    g->lpos++;
    return on;
}
static void geo_line(pmps_t *ps, pmwin_t *w, int x0, int y0, int x1, int y1, int width)   /* world coordinates */
{
    geo_t *g = geo(ps);
    ps_to_dev(ps, x0, y0, &x0, &y0);
    ps_to_dev(ps, x1, y1, &x1, &y1);
    uint32_t c = geo_color(ps);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy, h = width / 2;
    for (;;) {
        if (geo_dash(g)) {
            if (width <= 1) geo_pel(ps, w, x0, y0, c, geo_mix(g));
            else for (int j = -h; j < width - h; j++) for (int i = -h; i < width - h; i++) geo_pel(ps, w, x0 + i, y0 + j, c, geo_mix(g));
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}
/* is the pattern's pixel at (x, y) foreground? */
static int geo_pat(geo_t *g, int x, int y)
{
    static const uint8_t dense[9] = { 0, 15, 14, 12, 10, 8, 6, 4, 2 };   /* out of 16: PATSYM_DENSE1 .. DENSE8 */
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    switch (g->pattern) {
    case 0: case 16: return 1;                                           /* DEFAULT, SOLID */
    case 15: case 64: return 0;                                          /* NOSHADE, BLANK */
    case 9: return (x & 3) == 0;                                         /* VERT */
    case 10: return (y & 3) == 0;                                        /* HORIZ */
    case 11: case 12: return ((x + y) & 3) == 0;                         /* DIAG1, 2 */
    case 13: case 14: return ((x - y) & 3) == 0;                         /* DIAG3, 4 */
    case 17: return ((x + y) & 1) == 0;                                  /* HALFTONE */
    case 18: return (x & 3) == 0 || (y & 3) == 0;                        /* HATCH */
    case 19: return ((x + y) & 3) == 0 || ((x - y) & 3) == 0;            /* DIAGHATCH */
    default: return g->pattern >= 1 && g->pattern <= 8 ? bayer[(y & 3) * 4 + (x & 3)] < dense[g->pattern] : 1;
    }
}
/* the figures, filled: scan lines, alternate (even-odd) or winding */
static void geo_fill_dev(pmps_t *ps, pmwin_t *w, geo_t *g, int winding);
static void geo_fill(pmps_t *ps, pmwin_t *w, geo_t *g, int winding)   /* world figures, converted to device and filled */
{
    if (!ps->geo || ((geo_t *)ps->geo)->ident) { geo_fill_dev(ps, w, g, winding); return; }
    geo_t d = *g;
    d.fig = calloc((size_t)(g->nfig ? g->nfig : 1), sizeof *d.fig);
    for (int i = 0; i < g->nfig; i++) {
        d.fig[i].n = d.fig[i].cap = g->fig[i].n;
        d.fig[i].p = malloc((size_t)(g->fig[i].n ? g->fig[i].n : 1) * 2 * sizeof(int32_t));
        for (int k = 0; k < g->fig[i].n; k++) { int x, y; ps_to_dev(ps, g->fig[i].p[2 * k], g->fig[i].p[2 * k + 1], &x, &y); d.fig[i].p[2 * k] = x; d.fig[i].p[2 * k + 1] = y; }
    }
    geo_fill_dev(ps, w, &d, winding);
    for (int i = 0; i < d.nfig; i++) free(d.fig[i].p);
    free(d.fig);
}
static void geo_fill_dev(pmps_t *ps, pmwin_t *w, geo_t *g, int winding)
{
    int ymin = 1 << 30, ymax = -(1 << 30);
    for (int i = 0; i < g->nfig; i++) for (int k = 0; k < g->fig[i].n; k++) {
        int y = g->fig[i].p[2 * k + 1];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    if (ymin > ymax) return;
    if (ymin < 0) ymin = 0;
    if (ymax >= w->cy) ymax = w->cy - 1;
    uint32_t c = geo_color(ps), b = pm_rgb(ps->back, ps->rgb, 1);
    int cap = 64, *xs = malloc((size_t)cap * sizeof *xs), *dir = malloc((size_t)cap * sizeof *dir);
    for (int y = ymin; y <= ymax; y++) {
        int n = 0;
        double sy = y + 0.5;
        for (int i = 0; i < g->nfig; i++) {
            geofig_t *f = &g->fig[i];
            for (int k = 0; k < f->n; k++) {
                int x0 = f->p[2 * k], y0 = f->p[2 * k + 1], j = (k + 1) % f->n, x1 = f->p[2 * j], y1 = f->p[2 * j + 1];
                if ((y0 <= sy) == (y1 <= sy)) continue;
                if (n == cap) { cap *= 2; xs = realloc(xs, (size_t)cap * sizeof *xs); dir = realloc(dir, (size_t)cap * sizeof *dir); }
                xs[n] = (int)lround(x0 + (sy - y0) * (x1 - x0) / (double)(y1 - y0));
                dir[n++] = y1 > y0 ? 1 : -1;
            }
        }
        for (int a = 1; a < n; a++) for (int k = a; k > 0 && xs[k - 1] > xs[k]; k--) {   /* sort by x */
            int t = xs[k]; xs[k] = xs[k - 1]; xs[k - 1] = t; t = dir[k]; dir[k] = dir[k - 1]; dir[k - 1] = t;
        }
        int wn = 0;
        for (int k = 0; k + 1 < n; k++) {
            wn += dir[k];
            int in = winding ? wn != 0 : !(k & 1);
            if (!in) continue;
            for (int x = xs[k]; x < xs[k + 1]; x++) {
                if (geo_pat(g, x, y)) geo_pel(ps, w, x, y, c, geo_mix(g));
                else if (g->bmix == BM_OVERPAINT) geo_pel(ps, w, x, y, b, FM_OVERPAINT);   /* fill the background between */
            }
        }
    }
    free(xs); free(dir);
}
static void geo_stroke(pmps_t *ps, pmwin_t *w, geo_t *g, int closed, int width)
{
    for (int i = 0; i < g->nfig; i++) {
        geofig_t *f = &g->fig[i];
        g->lpos = 0;
        for (int k = 0; k + 1 < f->n; k++) geo_line(ps, w, f->p[2 * k], f->p[2 * k + 1], f->p[2 * k + 2], f->p[2 * k + 3], width);
        if (closed && f->n > 2) geo_line(ps, w, f->p[2 * f->n - 2], f->p[2 * f->n - 1], f->p[0], f->p[1], width);
    }
}

/* the single drawing primitive: a segment from the current position to (x, y) */
static void geo_to(pmps_t *ps, int x, int y)
{
    geo_t *g = geo(ps);
    pmwin_t *w = ps_win(ps);
    if (g->area || g->path) {
        if (!g->nfig || !g->fig[g->nfig - 1].n) geo_fig_add(g, ps->x, ps->y);
        geo_fig_add(g, x, y);
    } else if (w) geo_line(ps, w, ps->x, ps->y, x, y, g->lwidth);
    ps->x = x; ps->y = y;
}
static void geo_move(pmps_t *ps, int x, int y)
{
    geo_t *g = geo(ps);
    if ((g->area || g->path) && g->nfig && g->fig[g->nfig - 1].n) geo_fig_new(g);   /* a move ends the current figure */
    ps->x = x; ps->y = y;
}
/* a closed shape on its own (full arc, box): filled, outlined, or added to the current area/path */
static void geo_shape(pmps_t *ps, const int32_t *p, int n, uint32_t control)
{
    geo_t *g = geo(ps);
    pmwin_t *w = ps_win(ps);
    if (g->area || g->path) {
        if (g->nfig && g->fig[g->nfig - 1].n) geo_fig_new(g);
        for (int k = 0; k < n; k++) geo_fig_add(g, p[2 * k], p[2 * k + 1]);
        geo_fig_new(g);
        return;
    }
    if (!w) return;
    geo_t tmp = *g;
    geofig_t f = { n, n, (int32_t *)p };
    tmp.fig = &f; tmp.nfig = 1;
    if (control & DRO_FILL) geo_fill(ps, w, &tmp, 0);
    if ((control & DRO_OUTLINE) || !control) geo_stroke(ps, w, &tmp, 1, g->lwidth);
}
/* points along the transformed arc: centre (cx, cy), multiplier m, from a0 through sweep radians anticlockwise */
static int geo_arc_pts(geo_t *g, double cx, double cy, double m, double a0, double sweep, int32_t *out, int max)
{
    double rx = m * hypot(g->ap, g->as), ry = m * hypot(g->ar, g->aq), r = rx > ry ? rx : ry;
    int n = (int)(fabs(sweep) * (r > 4 ? sqrt(r) * 2 : 4) / (2 * M_PI) * 8) + 2;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        double t = a0 + sweep * i / (n - 1), c = cos(t), s = sin(t);
        out[2 * i] = (int32_t)lround(cx + m * (g->ap * c + g->ar * s));
        out[2 * i + 1] = (int32_t)lround(cy + m * (g->as * c + g->aq * s));
    }
    return n;
}

/* ── the calls ── */
#define GEO_PS pmps_t *ps = ps_get(A(0)); if (!ps) return 0; geo_t *g = geo(ps); (void)g
static uint32_t api_GpiMove(void) { GEO_PS; if (!ok(A(1), 8)) return 0; geo_move(ps, (int32_t)g32(A(1)), (int32_t)g32(A(1) + 4)); return 1; }
static uint32_t api_GpiSetCurrentPosition(void) { GEO_PS; if (!ok(A(1), 8)) return 0; ps->x = (int32_t)g32(A(1)); ps->y = (int32_t)g32(A(1) + 4); return 1; }
static uint32_t api_GpiLine(void) { GEO_PS; if (!ok(A(1), 8)) return 0; g->lpos = 0; geo_to(ps, (int32_t)g32(A(1)), (int32_t)g32(A(1) + 4)); return 1; }
static uint32_t api_GpiPolyLine(void)               /* (hps, lCount, aptlPoints) */
{
    GEO_PS;
    int n = (int)A(1);
    if (n < 0 || !ok(A(2), 8u * (uint32_t)n)) return 0;
    g->lpos = 0;
    for (int k = 0; k < n; k++) geo_to(ps, (int32_t)g32(A(2) + 8u * (uint32_t)k), (int32_t)g32(A(2) + 8u * (uint32_t)k + 4));
    return 1;
}
static uint32_t api_GpiSetArcParams(void)           /* (hps, parcp: P, Q, R, S) */
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    g->ap = (int32_t)g32(A(1)); g->aq = (int32_t)g32(A(1) + 4); g->ar = (int32_t)g32(A(1) + 8); g->as = (int32_t)g32(A(1) + 12);
    return 1;
}
static uint32_t api_GpiQueryArcParams(void)
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    p32(A(1), (uint32_t)g->ap); p32(A(1) + 4, (uint32_t)g->aq); p32(A(1) + 8, (uint32_t)g->ar); p32(A(1) + 12, (uint32_t)g->as);
    return 1;
}
static uint32_t api_GpiFullArc(void)                /* (hps, flControl, fxMultiplier): around the current position */
{
    GEO_PS;
    static int32_t p[2 * 720];
    int n = geo_arc_pts(g, ps->x, ps->y, (int32_t)A(2) / 65536.0, 0, 2 * M_PI, p, 720);
    geo_shape(ps, p, n - 1, A(1));
    return 1;
}
static uint32_t api_GpiPartialArc(void)             /* (hps, pptlCenter, fxMultiplier, fxStartAngle, fxSweepAngle) */
{
    GEO_PS;
    if (!ok(A(1), 8)) return 0;
    static int32_t p[2 * 720];
    double a0 = (int32_t)A(3) / 65536.0 * M_PI / 180, sw = (int32_t)A(4) / 65536.0 * M_PI / 180;
    int n = geo_arc_pts(g, (int32_t)g32(A(1)), (int32_t)g32(A(1) + 4), (int32_t)A(2) / 65536.0, a0, sw, p, 720);
    g->lpos = 0;
    for (int k = 0; k < n; k++) geo_to(ps, p[2 * k], p[2 * k + 1]);   /* line to its start, then the arc */
    return 1;
}
static uint32_t api_GpiPointArc(void)               /* (hps, aptl[2]): from the current position through the first point to the second */
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    double x0 = ps->x, y0 = ps->y, x1 = (int32_t)g32(A(1)), y1 = (int32_t)g32(A(1) + 4), x2 = (int32_t)g32(A(1) + 8), y2 = (int32_t)g32(A(1) + 12);
    double d = 2 * (x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1));
    g->lpos = 0;
    if (fabs(d) < 1e-9) { geo_to(ps, (int)x1, (int)y1); geo_to(ps, (int)x2, (int)y2); return 1; }   /* collinear: just lines */
    double ux = ((x0 * x0 + y0 * y0) * (y1 - y2) + (x1 * x1 + y1 * y1) * (y2 - y0) + (x2 * x2 + y2 * y2) * (y0 - y1)) / d;
    double uy = ((x0 * x0 + y0 * y0) * (x2 - x1) + (x1 * x1 + y1 * y1) * (x0 - x2) + (x2 * x2 + y2 * y2) * (x1 - x0)) / d;
    double r = hypot(x0 - ux, y0 - uy), a0 = atan2(y0 - uy, x0 - ux), a1 = atan2(y1 - uy, x1 - ux), a2 = atan2(y2 - uy, x2 - ux);
    double s1 = fmod(a1 - a0 + 4 * M_PI, 2 * M_PI), s2 = fmod(a2 - a0 + 4 * M_PI, 2 * M_PI);
    double sweep = s1 <= s2 ? s2 : s2 - 2 * M_PI;            /* the direction that goes through the middle point */
    int n = (int)(fabs(sweep) * (r > 4 ? sqrt(r) * 2 : 4) / (2 * M_PI) * 8) + 2;
    for (int i = 1; i < n; i++) geo_to(ps, (int)lround(ux + r * cos(a0 + sweep * i / (n - 1))), (int)lround(uy + r * sin(a0 + sweep * i / (n - 1))));
    geo_to(ps, (int)x2, (int)y2);
    return 1;
}
static void geo_bezier(pmps_t *ps, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3)
{
    int n = (int)((hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x3 - x2, y3 - y2)) / 4) + 2;
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        geo_to(ps, (int)lround(u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3),
                   (int)lround(u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3));
    }
}
static uint32_t api_GpiPolySpline(void)             /* (hps, lCount, aptl): cubic Béziers from the current position, three points each */
{
    GEO_PS;
    int n = (int)A(1);
    if (n < 0 || !ok(A(2), 8u * (uint32_t)n)) return 0;
    g->lpos = 0;
    for (int k = 0; k + 2 < n; k += 3) {
        uint32_t q = A(2) + 8u * (uint32_t)k;
        geo_bezier(ps, ps->x, ps->y, (int32_t)g32(q), (int32_t)g32(q + 4), (int32_t)g32(q + 8), (int32_t)g32(q + 12), (int32_t)g32(q + 16), (int32_t)g32(q + 20));
    }
    return 1;
}
static uint32_t api_GpiPolyFillet(void)             /* (hps, lCount, aptl): a curve tangent to the lines between the points */
{
    GEO_PS;
    int n = (int)A(1);
    if (n < 1 || !ok(A(2), 8u * (uint32_t)n)) return 0;
    g->lpos = 0;
    if (n == 1) { geo_to(ps, (int32_t)g32(A(2)), (int32_t)g32(A(2) + 4)); return 1; }
    double sx = ps->x, sy = ps->y;
    for (int k = 0; k + 1 < n; k++) {                        /* quadratic pieces: each point is a control point, the midpoints are the joins */
        double cx = (int32_t)g32(A(2) + 8u * (uint32_t)k), cy = (int32_t)g32(A(2) + 8u * (uint32_t)k + 4);
        double nx = (int32_t)g32(A(2) + 8u * (uint32_t)(k + 1)), ny = (int32_t)g32(A(2) + 8u * (uint32_t)(k + 1) + 4);
        double ex = k + 2 == n ? nx : (cx + nx) / 2, ey = k + 2 == n ? ny : (cy + ny) / 2;
        geo_bezier(ps, sx, sy, sx + 2.0 / 3 * (cx - sx), sy + 2.0 / 3 * (cy - sy), ex + 2.0 / 3 * (cx - ex), ey + 2.0 / 3 * (cy - ey), ex, ey);
        sx = ex; sy = ey;
    }
    return 1;
}
static uint32_t api_GpiBox(void)                    /* (hps, lControl, pptlPoint, lHRound, lVRound): from the current position */
{
    GEO_PS;
    if (!ok(A(2), 8)) return 0;
    int x0 = ps->x, y0 = ps->y, x1 = (int32_t)g32(A(2)), y1 = (int32_t)g32(A(2) + 4);
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int hr = (int)A(3) / 2, vr = (int)A(4) / 2;               /* corner ellipse axes, halved */
    if (hr > (x1 - x0) / 2) hr = (x1 - x0) / 2;
    if (vr > (y1 - y0) / 2) vr = (y1 - y0) / 2;
    if (!hr || !vr) {
        if (!g->area && !g->path && (A(1) & 1) && !g->clip && g->pattern % 16 == 0 && geo_mix(g) == 2 && ps_win(ps) && g->ident) {   /* common case: fast path */
            fill(ps_win(ps), x0, y0, x1 + 1, y1 + 1, geo_color(ps));
            if (A(1) & 2) { int32_t p[8] = { x0, y0, x1, y0, x1, y1, x0, y1 }; geo_shape(ps, p, 4, 2); }
            return 1;
        }
        int32_t p[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
        if (A(1) & 1) { int32_t q[8] = { x0, y0, x1 + 1, y0, x1 + 1, y1 + 1, x0, y1 + 1 }; geo_shape(ps, q, 4, A(1) & 1); }
        if ((A(1) & 2) || !A(1)) geo_shape(ps, p, 4, 2);
        return 1;
    }
    int32_t p[2 * 4 * 64];
    int n = 0;
    const int cx[4] = { x1 - hr, x0 + hr, x0 + hr, x1 - hr }, cy[4] = { y1 - vr, y1 - vr, y0 + vr, y0 + vr };
    for (int q = 0; q < 4; q++)
        for (int i = 0; i < 16; i++) {
            double t = (q + i / 15.0) * M_PI / 2;
            p[2 * n] = (int32_t)lround(cx[q] + hr * cos(t)); p[2 * n + 1] = (int32_t)lround(cy[q] + vr * sin(t)); n++;
        }
    geo_shape(ps, p, n, A(1));
    return 1;
}
static uint32_t api_GpiPolygons(void)               /* (hps, ulCount, aplgn {ulPoints, aPointl}, flOptions, flModel) */
{
    GEO_PS;
    pmwin_t *w = ps_win(ps);
    uint32_t cnt = A(1);
    if (!ok(A(2), 8 * cnt) || !w) return 0;
    geo_t tmp = *g;
    tmp.fig = 0; tmp.nfig = 0;
    for (uint32_t i = 0; i < cnt; i++) {                      /* the first one starts at the current position */
        uint32_t np = g32(A(2) + 8 * i), pts = g32(A(2) + 8 * i + 4);
        if (!ok(pts, 8 * np)) continue;
        geo_fig_new(&tmp);
        if (i == 0) geo_fig_add(&tmp, ps->x, ps->y);
        for (uint32_t k = 0; k < np; k++) geo_fig_add(&tmp, (int32_t)g32(pts + 8 * k), (int32_t)g32(pts + 8 * k + 4));
    }
    geo_fill(ps, w, &tmp, (A(3) & POLYGON_WINDING) != 0);
    if (A(3) & POLYGON_BOUNDARY) geo_stroke(ps, w, &tmp, 1, g->lwidth);
    geo_figs_clear(&tmp);
    return 1;
}
/* areas */
static uint32_t api_GpiBeginArea(void) { GEO_PS; geo_figs_clear(g); g->area = 1; g->area_fl = (int)A(1); return 1; }
static uint32_t api_GpiEndArea(void)
{
    GEO_PS;
    pmwin_t *w = ps_win(ps);
    if (!g->area) return 0;
    g->area = 0;
    if (w) {
        geo_fill(ps, w, g, (g->area_fl & BA_WINDING) != 0);
        if (g->area_fl & BA_BOUNDARY) geo_stroke(ps, w, g, 1, g->lwidth);
    }
    geo_figs_clear(g);
    return 1;
}
/* paths (only id 1) */
static uint32_t api_GpiBeginPath(void) { GEO_PS; geo_figs_clear(g); g->path = 1; g->path_done = 0; return 1; }
static uint32_t api_GpiEndPath(void) { GEO_PS; if (!g->path) return 0; g->path = 0; g->path_done = 1; return 1; }
static uint32_t api_GpiCloseFigure(void)
{
    GEO_PS;
    if (!(g->area || g->path) || !g->nfig) return 0;
    geofig_t *f = &g->fig[g->nfig - 1];
    if (f->n) { ps->x = f->p[0]; ps->y = f->p[1]; geo_fig_new(g); }
    return 1;
}
static uint32_t api_GpiFillPath(void)               /* (hps, lPath, lOptions: FPATH_WINDING 2) */
{
    GEO_PS;
    pmwin_t *w = ps_win(ps);
    if (!g->path_done) return 0;
    if (w) geo_fill(ps, w, g, (A(2) & 2) != 0);
    geo_figs_clear(g); g->path_done = 0;
    return 1;
}
static uint32_t api_GpiStrokePath(void)             /* (hps, lPath, flOptions): uses the geometric width */
{
    GEO_PS;
    pmwin_t *w = ps_win(ps);
    if (!g->path_done) return 0;
    if (w) geo_stroke(ps, w, g, 0, g->gwidth);
    geo_figs_clear(g); g->path_done = 0;
    return 1;
}
static uint32_t api_GpiOutlinePath(void)
{
    GEO_PS;
    pmwin_t *w = ps_win(ps);
    if (!g->path_done) return 0;
    if (w) geo_stroke(ps, w, g, 0, g->lwidth);
    geo_figs_clear(g); g->path_done = 0;
    return 1;
}
static uint32_t api_GpiModifyPath(void) { return 1; }  /* MPATH_STROKE: left as is; GpiStrokePath widens it */
/* attributes */
static uint32_t api_GpiSetLineType(void) { GEO_PS; g->ltype = (int)A(1); return 1; }
static uint32_t api_GpiQueryLineType(void) { GEO_PS; return (uint32_t)g->ltype; }
static uint32_t api_GpiSetLineWidth(void)           /* (hps, fxWidth): LINEWIDTH_NORMAL 1.0, THICK 2.0 */
{
    GEO_PS;
    int px = (int)((int32_t)A(1) >> 16);
    g->lwidth = px < 1 ? 1 : px > 16 ? 16 : px;
    return 1;
}
static uint32_t api_GpiQueryLineWidth(void) { GEO_PS; return (uint32_t)g->lwidth << 16; }
static uint32_t api_GpiSetLineWidthGeom(void) { GEO_PS; int px = (int)A(1); g->gwidth = px < 1 ? 1 : px > 64 ? 64 : px; return 1; }
static uint32_t api_GpiQueryLineWidthGeom(void) { GEO_PS; return (uint32_t)g->gwidth; }
static uint32_t api_GpiSetLineEnd(void) { return 1; }
static uint32_t api_GpiSetLineJoin(void) { return 1; }
static uint32_t api_GpiQueryLineEnd(void) { return 0; }
static uint32_t api_GpiQueryLineJoin(void) { return 0; }
static uint32_t api_GpiSetPattern(void) { GEO_PS; g->pattern = (int)A(1); return 1; }
static uint32_t api_GpiQueryPattern(void) { GEO_PS; return (uint32_t)g->pattern; }
static uint32_t api_GpiSetPatternSet(void) { return 1; }
static uint32_t api_GpiSetMix(void) { GEO_PS; g->mix = (int)A(1); return 1; }
static uint32_t api_GpiQueryMix(void) { GEO_PS; return (uint32_t)geo_mix(g); }
static uint32_t api_GpiSetBackMix(void) { GEO_PS; g->bmix = (int)A(1); return 1; }
static uint32_t api_GpiQueryBackMix(void) { GEO_PS; return (uint32_t)g->bmix; }
/* markers */
static uint32_t api_GpiSetMarker(void) { GEO_PS; g->marker = (int)A(1); return 1; }
static uint32_t api_GpiQueryMarker(void) { GEO_PS; return (uint32_t)g->marker; }
static void geo_marker(pmps_t *ps, int x, int y)
{
    geo_t *g = geo(ps);
    pmwin_t *w = ps_win(ps);
    if (!w) return;
    uint32_t c = geo_color(ps);
    int m = geo_mix(g), dx, dy;
    ps_to_dev(ps, x, y, &dx, &dy);
    for (int j = -4; j <= 4; j++)
        for (int i = -4; i <= 4; i++) {
            int on;
            switch (g->marker) {
            case 2: on = i == 0 || j == 0; break;                              /* MARKSYM_PLUS */
            case 3: on = abs(i) + abs(j) == 4; break;                          /* DIAMOND */
            case 4: on = abs(i) == 4 || abs(j) == 4; break;                    /* SQUARE */
            case 5: case 6: on = i == 0 || j == 0 || abs(i) == abs(j); break;  /* SIX-, EIGHTPOINTSTAR */
            case 7: on = abs(i) + abs(j) <= 4; break;                          /* SOLIDDIAMOND */
            case 8: on = 1; break;                                             /* SOLIDSQUARE */
            case 9: on = abs(i) <= 1 && abs(j) <= 1; break;                    /* DOT */
            case 10: on = i * i + j * j >= 9 && i * i + j * j <= 16; break;   /* SMALLCIRCLE */
            case 64: on = 0; break;                                            /* BLANK */
            default: on = i == j || i == -j;                                   /* DEFAULT, CROSS */
            }
            if (on) geo_pel(ps, w, dx + i, dy + j, c, m);
        }
    ps->x = x; ps->y = y;
}
static uint32_t api_GpiMarker(void) { GEO_PS; if (!ok(A(1), 8)) return 0; geo_marker(ps, (int32_t)g32(A(1)), (int32_t)g32(A(1) + 4)); return 1; }
static uint32_t api_GpiPolyMarker(void)
{
    GEO_PS;
    int n = (int)A(1);
    if (n < 0 || !ok(A(2), 8u * (uint32_t)n)) return 0;
    for (int k = 0; k < n; k++) geo_marker(ps, (int32_t)g32(A(2) + 8u * (uint32_t)k), (int32_t)g32(A(2) + 8u * (uint32_t)k + 4));
    return 1;
}
/* clipping: one rectangle, the intersection of those given */
static uint32_t api_GpiIntersectClipRectangle(void) /* (hps, prcl): RGN_RECT */
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    int x0 = (int32_t)g32(A(1)), y0 = (int32_t)g32(A(1) + 4), x1 = (int32_t)g32(A(1) + 8), y1 = (int32_t)g32(A(1) + 12);
    ps_to_dev(ps, x0, y0, &x0, &y0); ps_to_dev(ps, x1, y1, &x1, &y1);   /* world to device: an upright rectangle */
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (!g->clip) { g->clip = 1; g->cx0 = x0; g->cy0 = y0; g->cx1 = x1; g->cy1 = y1; }
    else { if (x0 > g->cx0) g->cx0 = x0; if (y0 > g->cy0) g->cy0 = y0; if (x1 < g->cx1) g->cx1 = x1; if (y1 < g->cy1) g->cy1 = y1; }
    return RGN_RECT;
}
static uint32_t api_GpiPtVisible(void)              /* (hps, pptl): PVIS_VISIBLE 2, PVIS_INVISIBLE 1 */
{
    GEO_PS;
    if (!ok(A(1), 8)) return 0;
    int x = (int32_t)g32(A(1)), y = (int32_t)g32(A(1) + 4);
    pmwin_t *w = ps_win(ps);
    int in = w && x >= 0 && y >= 0 && x < w->cx && y < w->cy && (!g->clip || (x >= g->cx0 && x < g->cx1 && y >= g->cy0 && y < g->cy1));
    return in ? 2 : 1;
}

/* ── transforms: MATRIXLF (fxM11, fxM12, lM13, fxM21, fxM22, lM23, lM31, lM32, lM33) ── */
static void mat_get(uint32_t p, uint32_t n, double *m)     /* the first n elements given, the rest from the identity */
{
    memcpy(m, IDM, sizeof IDM);
    if (!p) return;
    if (n > 0) m[0] = (int32_t)g32(p) / 65536.0;
    if (n > 1) m[1] = (int32_t)g32(p + 4) / 65536.0;
    if (n > 3) m[2] = (int32_t)g32(p + 12) / 65536.0;
    if (n > 4) m[3] = (int32_t)g32(p + 16) / 65536.0;
    if (n > 6) m[4] = (int32_t)g32(p + 24);
    if (n > 7) m[5] = (int32_t)g32(p + 28);
}
static void mat_put(uint32_t p, uint32_t n, const double *m)
{
    int32_t v[9] = { (int32_t)lround(m[0] * 65536), (int32_t)lround(m[1] * 65536), 0, (int32_t)lround(m[2] * 65536),
                     (int32_t)lround(m[3] * 65536), 0, (int32_t)lround(m[4]), (int32_t)lround(m[5]), 1 };
    for (uint32_t i = 0; i < n && i < 9; i++) p32(p + 4 * i, (uint32_t)v[i]);
}
/* REPLACE: use it; ADD: apply after the current one; PREEMPT: before it */
static void mat_apply(double *cur, const double *m, uint32_t opt)
{
    if (opt == 1) mat_mul(cur, cur, m);
    else if (opt == 2) mat_mul(cur, m, cur);
    else memcpy(cur, m, 6 * sizeof *cur);
}
static uint32_t gpi_setmat(double *which, int stage)       /* (hps, lCount, pmatlf, lOptions) */
{
    GEO_PS;
    double m[6];
    mat_get(A(2), A(1), m);
    double *t = stage == 0 ? g->model : stage == 1 ? g->view : g->seg;
    (void)which;
    mat_apply(t, m, A(3));
    geo_combine(g);
    return 1;
}
static uint32_t gpi_querymat(int stage)                   /* (hps, lCount, pmatlf) */
{
    GEO_PS;
    if (!ok(A(2), 4 * A(1))) return 0;
    mat_put(A(2), A(1), stage == 0 ? g->model : stage == 1 ? g->view : g->seg);
    return 1;
}
static uint32_t api_GpiSetModelTransformMatrix(void) { return gpi_setmat(0, 0); }
static uint32_t api_GpiQueryModelTransformMatrix(void) { return gpi_querymat(0); }
static uint32_t api_GpiSetDefaultViewMatrix(void) { return gpi_setmat(0, 1); }
static uint32_t api_GpiQueryDefaultViewMatrix(void) { return gpi_querymat(1); }
static uint32_t api_GpiSetViewingTransformMatrix(void) { return gpi_setmat(0, 2); }
static uint32_t api_GpiQueryViewingTransformMatrix(void) { return gpi_querymat(2); }
/* GpiTranslate (hps, pmatlf, lOptions, pptl), GpiScale (hps, pmatlf, lOptions, afxScale[2], pptlCenter), GpiRotate (hps,
 * pmatlf, lOptions, fxAngle, pptlCenter): build a matrix into pmatlf, replacing it or added after the existing one */
static uint32_t gpi_build(const double *m)
{
    if (!ok(A(1), 36)) return 0;
    double cur[6];
    mat_get(A(1), 9, cur);
    mat_apply(cur, m, A(2) == 2 ? 2 : A(2));
    mat_put(A(1), 9, cur);
    return 1;
}
static uint32_t api_GpiTranslate(void)
{
    if (!ok(A(3), 8)) return 0;
    double m[6] = { 1, 0, 0, 1, (int32_t)g32(A(3)), (int32_t)g32(A(3) + 4) };
    return gpi_build(m);
}
static uint32_t api_GpiScale(void)
{
    if (!ok(A(3), 8)) return 0;
    double sx = (int32_t)g32(A(3)) / 65536.0, sy = (int32_t)g32(A(3) + 4) / 65536.0, cx = 0, cy = 0;
    if (A(4) && ok(A(4), 8)) { cx = (int32_t)g32(A(4)); cy = (int32_t)g32(A(4) + 4); }
    double m[6] = { sx, 0, 0, sy, cx - sx * cx, cy - sy * cy };
    return gpi_build(m);
}
static uint32_t api_GpiRotate(void)
{
    double a = (int32_t)A(3) / 65536.0 * M_PI / 180, c = cos(a), s = sin(a), cx = 0, cy = 0;
    if (A(4) && ok(A(4), 8)) { cx = (int32_t)g32(A(4)); cy = (int32_t)g32(A(4) + 4); }
    double m[6] = { c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy };
    return gpi_build(m);
}
/* GpiConvert (hps, lSrc, lTarg, lCount, aptl): CVTC_WORLD 1, MODEL 2, DEFAULTPAGE 3, PAGE 4, DEVICE 5 */
static void geo_to_space(geo_t *g, int space, double *m)  /* world to that space */
{
    memcpy(m, IDM, sizeof IDM);
    if (space >= 2) mat_mul(m, m, g->model);
    if (space >= 3) mat_mul(m, m, g->seg);
    if (space >= 4) mat_mul(m, m, g->view);
    if (space >= 5) mat_mul(m, m, g->page);
}
static uint32_t api_GpiConvert(void)
{
    GEO_PS;
    int32_t n = (int32_t)A(3);
    if (n < 0 || !ok(A(4), 8u * (uint32_t)n) || A(1) < 1 || A(1) > 5 || A(2) < 1 || A(2) > 5) return 0;
    double from[6], to[6], inv[6];
    geo_to_space(g, (int)A(1), from);
    geo_to_space(g, (int)A(2), to);
    double det = from[0] * from[3] - from[1] * from[2];
    if (fabs(det) < 1e-12) det = 1e-12;
    inv[0] = from[3] / det; inv[1] = -from[1] / det; inv[2] = -from[2] / det; inv[3] = from[0] / det;
    inv[4] = -(from[4] * inv[0] + from[5] * inv[2]); inv[5] = -(from[4] * inv[1] + from[5] * inv[3]);
    double t[6];
    mat_mul(t, inv, to);                                         /* back to world, then to the target */
    for (int32_t i = 0; i < n; i++) {
        uint32_t p = A(4) + 8u * (uint32_t)i;
        double x = (int32_t)g32(p), y = (int32_t)g32(p + 4);
        p32(p, (uint32_t)(int32_t)lround(t[0] * x + t[2] * y + t[4])); p32(p + 4, (uint32_t)(int32_t)lround(t[1] * x + t[3] * y + t[5]));
    }
    return 1;
}
/* the page: units (from GpiCreatePS) and viewport (GpiSetPageViewport maps the page, in units, onto a device rectangle) */
static void geo_set_units(pmps_t *ps, uint32_t opts, uint32_t psizl, int dpm)
{
    geo_t *g = geo(ps);
    g->units = (int)(opts & 0x1C);
    if (!g->units) g->units = 8;
    g->pagecx = psizl && ok(psizl, 8) ? (int32_t)g32(psizl) : 0;
    g->pagecy = psizl && ok(psizl, 8) ? (int32_t)g32(psizl + 4) : 0;
    double u = g->units == 8 || g->units == 4 ? 1 : unit_pels(g->units) * dpm / 3780.0;   /* pels and arbitrary units: the device's own */
    g->dpm = dpm;
    double m[6] = { u, 0, 0, u, 0, 0 };
    memcpy(g->page, m, sizeof m);
    geo_combine(g);
}
static uint32_t api_GpiSetPageViewport(void)              /* (hps, prclViewport) */
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    double x0 = (int32_t)g32(A(1)), y0 = (int32_t)g32(A(1) + 4), x1 = (int32_t)g32(A(1) + 8), y1 = (int32_t)g32(A(1) + 12);
    pmwin_t *w = ps_win(ps);
    double pcx = g->pagecx > 0 ? g->pagecx : w ? w->cx / (unit_pels(g->units) * (g->dpm ? g->dpm : 3780) / 3780.0) : 1, pcy = g->pagecy > 0 ? g->pagecy : w ? w->cy / (unit_pels(g->units) * (g->dpm ? g->dpm : 3780) / 3780.0) : 1;
    double m[6] = { (x1 - x0) / pcx, 0, 0, (y1 - y0) / pcy, x0, y0 };
    memcpy(g->page, m, sizeof m);
    geo_combine(g);
    return 1;
}
static uint32_t api_GpiQueryPageViewport(void)
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    pmwin_t *w = ps_win(ps);
    double pcx = g->pagecx > 0 ? g->pagecx : w ? w->cx / (unit_pels(g->units) * (g->dpm ? g->dpm : 3780) / 3780.0) : 0, pcy = g->pagecy > 0 ? g->pagecy : w ? w->cy / (unit_pels(g->units) * (g->dpm ? g->dpm : 3780) / 3780.0) : 0;
    p32(A(1), (uint32_t)(int32_t)lround(g->page[4])); p32(A(1) + 4, (uint32_t)(int32_t)lround(g->page[5]));
    p32(A(1) + 8, (uint32_t)(int32_t)lround(g->page[4] + g->page[0] * pcx)); p32(A(1) + 12, (uint32_t)(int32_t)lround(g->page[5] + g->page[3] * pcy));
    return 1;
}
static uint32_t gpi_query_ps(pmps_t *ps, uint32_t psizl)  /* GpiQueryPS: the units (and page size) */
{
    geo_t *g = geo(ps);
    if (psizl && ok(psizl, 8)) { p32(psizl, (uint32_t)g->pagecx); p32(psizl + 4, (uint32_t)g->pagecy); }
    return (uint32_t)g->units;
}
