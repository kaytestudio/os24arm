/* userland/os2x86/pm_geo.inc - GPI's geometry (included at the end of
 * pm.inc): lines with their types, widths and mixes; arcs (GpiFullArc,
 * GpiPartialArc, GpiPointArc, with GpiSetArcParams' transform), GpiBox
 * with rounded corners, polylines, fillets, splines, polygons; areas
 * (GpiBeginArea ... GpiEndArea) and paths (GpiBeginPath ... GpiFillPath,
 * GpiStrokePath) filled - alternate or winding - with the pattern;
 * markers; a clip rectangle (GpiIntersectClipRectangle).
 *
 * Every primitive comes down to geo_to(): a segment from the current
 * position. Outside an area or a path it's drawn at once; inside one it
 * adds a point to the current figure, and a move starts another figure. */
#include <math.h>

typedef struct { int n, cap; int32_t *p; } geofig_t;    /* (x, y pairs) */
typedef struct {
    int32_t ap, aq, ar, as;                  /* GpiSetArcParams: P, Q, R, S */
    int ltype, lwidth, gwidth, pattern, mix, bmix, marker;
    int area, area_fl, path, path_done;      /* (in an area: its BA_ options; in a path; a path ended, kept to fill or stroke) */
    geofig_t *fig; int nfig;
    int clip, cx0, cy0, cx1, cy1;            /* GpiIntersectClipRectangle: [cx0, cx1) x [cy0, cy1) */
    int lpos;                                /* (the line type's place along what's being drawn) */
} geo_t;
static geo_t *geo(pmps_t *ps)
{
    if (!ps->geo) {
        geo_t *g = calloc(1, sizeof *g);
        g->ap = g->aq = 1; g->lwidth = 1; g->gwidth = 1; g->mix = 2; g->bmix = 5; g->marker = 1;
        ps->geo = g;
    }
    return ps->geo;
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

/* a pixel with the PS's mix (FM_OVERPAINT, XOR, INVERT, OR, AND, LEAVEALONE ...) and clip */
static void geo_pel(pmps_t *ps, pmwin_t *w, int x, int y, uint32_t c, int mix)
{
    geo_t *g = ps->geo;
    if (g && g->clip && (x < g->cx0 || x >= g->cx1 || y < g->cy0 || y >= g->cy1)) return;
    switch (mix) {
    case 5: return;                                          /* FM_LEAVEALONE */
    case 4: pel(w, x, y, (getpel(w, x, y) ^ c) & 0xFFFFFF); return;   /* FM_XOR */
    case 12: pel(w, x, y, ~getpel(w, x, y) & 0xFFFFFF); return;       /* FM_INVERT */
    case 1: pel(w, x, y, (getpel(w, x, y) | c) & 0xFFFFFF); return;   /* FM_OR */
    case 6: pel(w, x, y, getpel(w, x, y) & c); return;                /* FM_AND */
    case 9: pel(w, x, y, 0); return;                                  /* FM_ZERO */
    case 15: pel(w, x, y, 0xFFFFFF); return;                          /* FM_ONE */
    case 11: pel(w, x, y, ~(getpel(w, x, y) ^ c) & 0xFFFFFF); return; /* FM_NOTXORSRC */
    default: pel(w, x, y, c);                                         /* FM_DEFAULT, FM_OVERPAINT */
    }
}
static uint32_t geo_color(pmps_t *ps) { return pm_rgb(ps->color, ps->rgb, 0); }
static int geo_mix(geo_t *g) { return g->mix ? g->mix : 2; }
/* the line types: their on/off runs */
static int geo_dash(geo_t *g)
{
    static const char *const pat[] = { 0, "1100", "11110000", "1111111100110000", "1010000000", "1111111111110000",
                                       "11111111001100110000", 0, "0", "10" };   /* DEFAULT DOT SHORTDASH DASHDOT DOUBLEDOT LONGDASH DASHDOUBLEDOT SOLID INVISIBLE ALTERNATE */
    const char *p = g->ltype > 0 && g->ltype < 10 ? pat[g->ltype] : 0;
    int on = !p || p[g->lpos % (int)strlen(p)] == '1';
    g->lpos++;
    return on;
}
static void geo_line(pmps_t *ps, pmwin_t *w, int x0, int y0, int x1, int y1, int width)
{
    geo_t *g = geo(ps);
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
/* the pattern's pixels: 1 where the foreground goes */
static int geo_pat(geo_t *g, int x, int y)
{
    static const uint8_t dense[9] = { 0, 15, 14, 12, 10, 8, 6, 4, 2 };   /* (of 16: PATSYM_DENSE1 .. DENSE8) */
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
static void geo_fill(pmps_t *ps, pmwin_t *w, geo_t *g, int winding)
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
        for (int a = 1; a < n; a++) for (int k = a; k > 0 && xs[k - 1] > xs[k]; k--) {   /* (sorted by x) */
            int t = xs[k]; xs[k] = xs[k - 1]; xs[k - 1] = t; t = dir[k]; dir[k] = dir[k - 1]; dir[k - 1] = t;
        }
        int wn = 0;
        for (int k = 0; k + 1 < n; k++) {
            wn += dir[k];
            int in = winding ? wn != 0 : !(k & 1);
            if (!in) continue;
            for (int x = xs[k]; x < xs[k + 1]; x++) {
                if (geo_pat(g, x, y)) geo_pel(ps, w, x, y, c, geo_mix(g));
                else if (g->bmix == 2) geo_pel(ps, w, x, y, b, 2);   /* (BM_OVERPAINT: the background between) */
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

/* the one way down: a segment from the current position to (x, y) */
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
    if ((g->area || g->path) && g->nfig && g->fig[g->nfig - 1].n) geo_fig_new(g);   /* (a move: the figure's end) */
    ps->x = x; ps->y = y;
}
/* a closed shape of its own (a full arc, a box) - filled, outlined, or a figure of the area or path */
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
    if (control & 1) geo_fill(ps, w, &tmp, 0);              /* DRO_FILL */
    if ((control & 2) || !control) geo_stroke(ps, w, &tmp, 1, g->lwidth);   /* DRO_OUTLINE */
}
/* points along the transformed arc: centre (cx, cy), multiplier m, from a0 by sweep (radians, anticlockwise) */
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
static uint32_t api_GpiFullArc(void)                /* (hps, flControl, fxMultiplier): round the current position */
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
    for (int k = 0; k < n; k++) geo_to(ps, p[2 * k], p[2 * k + 1]);   /* (a line to its start, then the arc) */
    return 1;
}
static uint32_t api_GpiPointArc(void)               /* (hps, aptl[2]): from the current position through the first to the second */
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    double x0 = ps->x, y0 = ps->y, x1 = (int32_t)g32(A(1)), y1 = (int32_t)g32(A(1) + 4), x2 = (int32_t)g32(A(1) + 8), y2 = (int32_t)g32(A(1) + 12);
    double d = 2 * (x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1));
    g->lpos = 0;
    if (fabs(d) < 1e-9) { geo_to(ps, (int)x1, (int)y1); geo_to(ps, (int)x2, (int)y2); return 1; }   /* (in a line: lines) */
    double ux = ((x0 * x0 + y0 * y0) * (y1 - y2) + (x1 * x1 + y1 * y1) * (y2 - y0) + (x2 * x2 + y2 * y2) * (y0 - y1)) / d;
    double uy = ((x0 * x0 + y0 * y0) * (x2 - x1) + (x1 * x1 + y1 * y1) * (x0 - x2) + (x2 * x2 + y2 * y2) * (x1 - x0)) / d;
    double r = hypot(x0 - ux, y0 - uy), a0 = atan2(y0 - uy, x0 - ux), a1 = atan2(y1 - uy, x1 - ux), a2 = atan2(y2 - uy, x2 - ux);
    double s1 = fmod(a1 - a0 + 4 * M_PI, 2 * M_PI), s2 = fmod(a2 - a0 + 4 * M_PI, 2 * M_PI);
    double sweep = s1 <= s2 ? s2 : s2 - 2 * M_PI;            /* (the way that passes the middle point) */
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
static uint32_t api_GpiPolyFillet(void)             /* (hps, lCount, aptl): a curve the lines between the points are tangents to */
{
    GEO_PS;
    int n = (int)A(1);
    if (n < 1 || !ok(A(2), 8u * (uint32_t)n)) return 0;
    g->lpos = 0;
    if (n == 1) { geo_to(ps, (int32_t)g32(A(2)), (int32_t)g32(A(2) + 4)); return 1; }
    double sx = ps->x, sy = ps->y;
    for (int k = 0; k + 1 < n; k++) {                        /* (quadratic pieces: each point a control, the middles the joins) */
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
    int hr = (int)A(3) / 2, vr = (int)A(4) / 2;               /* (the corners' ellipse: its axes, halved) */
    if (hr > (x1 - x0) / 2) hr = (x1 - x0) / 2;
    if (vr > (y1 - y0) / 2) vr = (y1 - y0) / 2;
    if (!hr || !vr) {
        if (!g->area && !g->path && (A(1) & 1) && !g->clip && g->pattern % 16 == 0 && geo_mix(g) == 2 && ps_win(ps)) {   /* (the usual: quick) */
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
    for (uint32_t i = 0; i < cnt; i++) {                      /* (the first starts at the current position) */
        uint32_t np = g32(A(2) + 8 * i), pts = g32(A(2) + 8 * i + 4);
        if (!ok(pts, 8 * np)) continue;
        geo_fig_new(&tmp);
        if (i == 0) geo_fig_add(&tmp, ps->x, ps->y);
        for (uint32_t k = 0; k < np; k++) geo_fig_add(&tmp, (int32_t)g32(pts + 8 * k), (int32_t)g32(pts + 8 * k + 4));
    }
    geo_fill(ps, w, &tmp, (A(3) & 2) != 0);                   /* POLYGON_WINDING */
    if (A(3) & 1) geo_stroke(ps, w, &tmp, 1, g->lwidth);     /* POLYGON_BOUNDARY */
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
        geo_fill(ps, w, g, (g->area_fl & 2) != 0);           /* BA_WINDING */
        if (g->area_fl & 1) geo_stroke(ps, w, g, 1, g->lwidth);   /* BA_BOUNDARY */
    }
    geo_figs_clear(g);
    return 1;
}
/* paths (one: id 1) */
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
static uint32_t api_GpiStrokePath(void)             /* (hps, lPath, flOptions): with the geometric width */
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
static uint32_t api_GpiModifyPath(void) { return 1; }  /* (MPATH_STROKE: kept as it is; StrokePath widens it) */
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
    int m = geo_mix(g);
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
            if (on) geo_pel(ps, w, x + i, y + j, c, m);
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
/* clipping: a rectangle (the intersection of those given) */
static uint32_t api_GpiIntersectClipRectangle(void) /* (hps, prcl): RGN_RECT */
{
    GEO_PS;
    if (!ok(A(1), 16)) return 0;
    int x0 = (int32_t)g32(A(1)), y0 = (int32_t)g32(A(1) + 4), x1 = (int32_t)g32(A(1) + 8), y1 = (int32_t)g32(A(1) + 12);
    if (!g->clip) { g->clip = 1; g->cx0 = x0; g->cy0 = y0; g->cx1 = x1; g->cy1 = y1; }
    else { if (x0 > g->cx0) g->cx0 = x0; if (y0 > g->cy0) g->cy0 = y0; if (x1 < g->cx1) g->cx1 = x1; if (y1 < g->cy1) g->cy1 = y1; }
    return 2;                                                /* RGN_RECT */
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
