/* userland/os2x86/pm_vs.inc - two of PM's controls (included by pm.inc,
 * after pm_ctl.inc):
 *   WC_VALUESET ("#39"): a grid of items - text, colours (RGB or a colour
 *     index), icons, bitmaps - one of them selected (VM_SETITEM,
 *     VM_SELECTITEM, VN_SELECT, VN_ENTER, ...);
 *   WC_CIRCULARSLIDER ("#65"): a dial, its value along an arc of 270
 *     degrees (or all of the circle), the - and + buttons below it
 *     (CSM_SETRANGE, CSM_SETVALUE, CSN_CHANGED, CSN_TRACKING, ...). */
#include <math.h>

/* ── WC_VALUESET ──────────────────────────────────────────────────────── */
typedef struct { uint16_t attr; uint32_t val; char *text; } vsitem_t;
typedef struct { int rows, cols, sr, sc, cr, cc, isx, isy, spx, spy; vsitem_t *it; } vs_t;
static vs_t *vsd(pmwin_t *w)
{
    if (!w->ctl) {
        vs_t *v = calloc(1, sizeof *v);
        v->rows = v->cols = 1; v->sr = v->sc = v->cr = v->cc = 1; v->spx = v->spy = 2;
        v->it = calloc(1, sizeof *v->it);
        v->it[0].attr = (uint16_t)(w->style & 0x3F);
        w->ctl = v;
    }
    return w->ctl;
}
static vsitem_t *vs_item(vs_t *v, int r, int c) { return r >= 1 && r <= v->rows && c >= 1 && c <= v->cols ? &v->it[(r - 1) * v->cols + (c - 1)] : 0; }
/* the cell of (r, c): [x0, x1) x [y0, y1); rows from the top */
static void vs_cell(pmwin_t *w, vs_t *v, int r, int c, int *x0, int *y0, int *x1, int *y1)
{
    int b = (w->style & 0x20) ? 3 : 1, iw = w->cx - 2 * b, ih = w->cy - 2 * b;
    int cw = (iw - v->spx * (v->cols - 1)) / v->cols, ch = (ih - v->spy * (v->rows - 1)) / v->rows;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    *x0 = b + (c - 1) * (cw + v->spx); *x1 = *x0 + cw;
    *y1 = w->cy - b - (r - 1) * (ch + v->spy); *y0 = *y1 - ch;
}
static int vs_hit(pmwin_t *w, vs_t *v, int x, int y, int *r, int *c)
{
    for (int i = 1; i <= v->rows; i++)
        for (int j = 1; j <= v->cols; j++) {
            int x0, y0, x1, y1;
            vs_cell(w, v, i, j, &x0, &y0, &x1, &y1);
            if (x >= x0 && x < x1 && y >= y0 && y < y1) { *r = i; *c = j; return 1; }
        }
    return 0;
}
static uint32_t vs_rc(int r, int c) { return (uint32_t)r | (uint32_t)c << 16; }
static void vs_select(pmwin_t *w, vs_t *v, int r, int c, int code)   /* code: VN_SELECT, VN_ENTER, or 0 */
{
    vsitem_t *it = vs_item(v, r, c);
    if (!it || (it->attr & 0x40)) return;                        /* VIA_DISABLED */
    v->sr = v->cr = r; v->sc = v->cc = c;
    repaint(w);
    if (code) notify(w, (uint32_t)code, vs_rc(r, c));
}
static uint32_t valueset_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    vs_t *v = vsd(w);
    switch (msg) {
    case WM_CREATE:                                              /* VSCDATA: cbSize, usRowCount, usColumnCount */
        if (mp1 && ok(mp1, 8) && g16(mp1 + 4) && g16(mp1 + 6)) {
            v->rows = g16(mp1 + 4) > 64 ? 64 : g16(mp1 + 4);
            v->cols = g16(mp1 + 6) > 64 ? 64 : g16(mp1 + 6);
            free(v->it);
            v->it = calloc((size_t)(v->rows * v->cols), sizeof *v->it);
            for (int i = 0; i < v->rows * v->cols; i++) v->it[i].attr = (uint16_t)(w->style & 0x3F);
        }
        return 0;
    case WM_DESTROY:
        for (int i = 0; i < v->rows * v->cols; i++) free(v->it[i].text);
        return 0;
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_FACE));
        if (w->style & 0x20) { bevel(w, 0, 0, w->cx, w->cy, 0); bevel(w, 1, 1, w->cx - 1, w->cy - 1, 0); }   /* VS_BORDER */
        uint32_t tc = pp_fgc(w, C_TEXT);
        for (int r = 1; r <= v->rows; r++)
            for (int c = 1; c <= v->cols; c++) {
                vsitem_t *it = vs_item(v, r, c);
                int x0, y0, x1, y1;
                vs_cell(w, v, r, c, &x0, &y0, &x1, &y1);
                int a = it->attr;
                if ((a & 0x20) || (w->style & 0x200))                                                       /* VIA_OWNERDRAW, VS_OWNERDRAW: the owner's */
                    owner_draw(w, w->id & 0xFFFF, x0 + 1, y0 + 1, x1 - 1, y1 - 1, vs_rc(r, c), it->val,
                               (uint32_t)(r == v->sr && c == v->sc), (uint32_t)(r == v->sr && c == v->sc), a, a, 0);
                else if (a & 0x08) fill(w, x0 + 2, y0 + 2, x1 - 2, y1 - 2, it->val & 0xFFFFFF);            /* VIA_RGB */
                else if (a & 0x10) fill(w, x0 + 2, y0 + 2, x1 - 2, y1 - 2, pm_rgb((int32_t)it->val, 0, 0)); /* VIA_COLORINDEX */
                else if ((a & 0x04) && it->text) {                                                           /* VIA_TEXT */
                    char p[256];
                    plain(it->text, p, sizeof p);
                    int tw = text_w(p, (int)strlen(p));
                    text_mid(w, x0 + (x1 - x0 - tw) / 2, y0, y1, it->text, (a & 0x40) ? C_DARK : tc);
                } else if ((a & 0x02) && ptr_get(it->val)) {                                                 /* VIA_ICON */
                    pmwin_t *p = ptr_get(it->val);
                    ptr_draw(w, x0 + (x1 - x0 - p->cx) / 2, y0 + (y1 - y0 - p->cy) / 2, p, 0);
                } else if ((a & 0x01) && bmp_get(it->val)) {                                                 /* VIA_BITMAP */
                    pmwin_t *b = bmp_get(it->val);
                    if (w->style & 0x80) blit(w, x0 + 2, y0 + 2, x1 - 2, y1 - 2, b, 0, 0, b->cx, b->cy, 0xCC, 0);   /* VS_SCALEBITMAPS */
                    else {
                        int bx = x0 + (x1 - x0 - b->cx) / 2, by = y0 + (y1 - y0 - b->cy) / 2;
                        blit(w, bx, by, bx + b->cx, by + b->cy, b, 0, 0, b->cx, b->cy, 0xCC, 0);
                    }
                }
                if ((a & 0x40) && !(a & 0x04)) for (int y = y0; y < y1; y++) for (int x = x0 + (y & 1); x < x1; x += 2) pel(w, x, y, C_FACE);
                if (w->style & 0x40) bevel(w, x0, y0, x1, y1, 0);                                           /* VS_ITEMBORDER */
                if (r == v->sr && c == v->sc) {                                                              /* the selection: a frame round it */
                    for (int k = 0; k < 2; k++) {
                        line(w, x0 + k, y0 + k, x1 - 1 - k, y0 + k, tc); line(w, x0 + k, y1 - 1 - k, x1 - 1 - k, y1 - 1 - k, tc);
                        line(w, x0 + k, y0 + k, x0 + k, y1 - 1 - k, tc); line(w, x1 - 1 - k, y0 + k, x1 - 1 - k, y1 - 1 - k, tc);
                    }
                }
                if (pm_focus == w->hwnd && r == v->cr && c == v->cc)                                         /* the cursor: dotted, inside */
                    for (int k = 0; k < (x1 - x0) - 6; k += 2) { pel(w, x0 + 3 + k, y0 + 3, tc); pel(w, x0 + 3 + k, y1 - 4, tc); }
            }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int r, c;
        set_focus(w->hwnd);
        if (vs_hit(w, v, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16), &r, &c)) vs_select(w, v, r, c, 120);   /* VN_SELECT */
        return 1;
    }
    case WM_BUTTON1DBLCLK: {
        int r, c;
        if (vs_hit(w, v, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16), &r, &c)) {
            if (r != v->sr || c != v->sc) vs_select(w, v, r, c, 120);
            notify(w, 121, vs_rc(r, c));                                                                     /* VN_ENTER */
        }
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, ch = (mp1 & KC_CHAR) ? (int)(mp2 & 0xFFFF) : 0;
        int r = v->cr, c = v->cc;
        if (vk == 0x15) c--; else if (vk == 0x17) c++; else if (vk == 0x16) r--; else if (vk == 0x18) r++;
        else if (vk == 0x14) c = 1; else if (vk == 0x13) c = v->cols;
        else if (vk == 0x08 || vk == 0x1e) { notify(w, 121, vs_rc(v->sr, v->sc)); return 1; }              /* Enter: VN_ENTER */
        else if (vk == 0x10 || ch == ' ') { vs_select(w, v, v->cr, v->cc, 120); return 1; }
        else return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        if (r < 1 || r > v->rows || c < 1 || c > v->cols) return 1;
        vs_select(w, v, r, c, 120);                                                                          /* (the cursor keys select, as PM's) */
        return 1;
    }
    case 0x037a: {                                               /* VM_SETITEM: (row, col), the value (text: copied) */
        vsitem_t *it = vs_item(v, (int)(mp1 & 0xFFFF), (int)(mp1 >> 16));
        if (!it) return 0;
        it->val = mp2;
        if (it->attr & 0x04) { free(it->text); char *s = mp2 ? gs(mp2) : 0; it->text = s ? strdup(s) : 0; }
        repaint(w);
        return 1;
    }
    case 0x0375: {                                               /* VM_QUERYITEM: (row, col), a VSTEXT for text */
        vsitem_t *it = vs_item(v, (int)(mp1 & 0xFFFF), (int)(mp1 >> 16));
        if (!it) return 0;
        if (it->attr & 0x04) {
            const char *t = it->text ? it->text : "";
            if (mp2 && ok(mp2, 8)) {                                 /* { pszItemText, ulBufLen } */
                uint32_t buf = g32(mp2), n = g32(mp2 + 4);
                if (buf && n && ok(buf, n)) snprintf((char *)C.mem + buf, n, "%s", t);
            }
            return (uint32_t)strlen(t);
        }
        return it->val;
    }
    case 0x037b: {                                               /* VM_SETITEMATTR: (row, col), (attributes, set or clear) */
        int r = (int)(mp1 & 0xFFFF), c = (int)(mp1 >> 16);
        int all = r == 0 && c == 0;                              /* (0, 0: all of them) */
        for (int i = 1; i <= v->rows; i++)
            for (int j = 1; j <= v->cols; j++) {
                if (!all && (i != r || j != c)) continue;
                vsitem_t *it = vs_item(v, i, j);
                uint16_t at = (uint16_t)(mp2 & 0xFFFF);
                if (mp2 >> 16) {
                    if (at & 0x3F) it->attr &= (uint16_t)~0x3F;     /* (one kind of item at a time) */
                    it->attr |= at;
                } else it->attr &= (uint16_t)~at;
            }
        if (!all && !vs_item(v, r, c)) return 0;
        repaint(w);
        return 1;
    }
    case 0x0376: { vsitem_t *it = vs_item(v, (int)(mp1 & 0xFFFF), (int)(mp1 >> 16)); return it ? it->attr : 0; }   /* VM_QUERYITEMATTR */
    case 0x0379: {                                               /* VM_SELECTITEM */
        vsitem_t *it = vs_item(v, (int)(mp1 & 0xFFFF), (int)(mp1 >> 16));
        if (!it || (it->attr & 0x40)) return 0;
        vs_select(w, v, (int)(mp1 & 0xFFFF), (int)(mp1 >> 16), 120);
        return 1;
    }
    case 0x0378: return vs_rc(v->sr, v->sc);                     /* VM_QUERYSELECTEDITEM */
    case 0x037c:                                                 /* VM_SETMETRICS: VMA_ITEMSIZE, VMA_ITEMSPACING; (cx, cy) */
        if (mp1 == 1) { v->isx = (int)(mp2 & 0xFFFF); v->isy = (int)(mp2 >> 16); }
        else if (mp1 == 2) { v->spx = (int)(mp2 & 0xFFFF); v->spy = (int)(mp2 >> 16); }
        else return 0;
        repaint(w);
        return 1;
    case 0x0377: {                                               /* VM_QUERYMETRICS */
        if (mp1 == 2) return (uint32_t)v->spx | (uint32_t)v->spy << 16;
        int x0, y0, x1, y1;
        vs_cell(w, v, 1, 1, &x0, &y0, &x1, &y1);
        return mp1 == 1 ? (uint32_t)(x1 - x0) | (uint32_t)(y1 - y0) << 16 : 0;
    }
    }
    return 0;
}

/* ── WC_CIRCULARSLIDER ────────────────────────────────────────────────── */
typedef struct { int lo, hi, val, inc, tick, drag; } cs_t;
static cs_t *csd(pmwin_t *w)
{
    if (!w->ctl) { cs_t *s = calloc(1, sizeof *s); s->hi = 100; s->inc = 1; s->tick = 10; w->ctl = s; }
    return w->ctl;
}
/* the dial: its centre, its radius; the room for the buttons and the text below */
static void cs_geom(pmwin_t *w, int *cx, int *cy, int *r)
{
    int below = ((w->style & 3) == 3 ? 0 : 20), top = (w->style & 4) ? 4 : pm_lineh + 2;   /* (the buttons, the text; the number above) */
    int h = w->cy - below - top, wd = w->cx - 8;
    *r = (h < wd ? h : wd) / 2 - 6;
    if (*r < 6) *r = 6;
    *cx = w->cx / 2;
    *cy = below + h / 2;
}
static double cs_sweep(pmwin_t *w) { return (w->style & 0x10) ? 2 * M_PI : 1.5 * M_PI; }     /* CSS_360 */
static double cs_start(pmwin_t *w) { return (w->style & 0x10) ? M_PI / 2 : 1.25 * M_PI; }    /* (the low end: the top, or down left) */
static double cs_angle(pmwin_t *w, cs_t *s, int v)                                          /* (clockwise from the start) */
{
    double f = s->hi > s->lo ? (double)(v - s->lo) / (s->hi - s->lo) : 0;
    return cs_start(w) - f * cs_sweep(w);
}
static void cs_set(pmwin_t *w, int v, int code)
{
    cs_t *s = csd(w);
    if (v < s->lo) v = s->lo;
    if (v > s->hi) v = s->hi;
    if (v == s->val) return;
    s->val = v;
    repaint(w);
    if (code) notify(w, (uint32_t)code, (uint32_t)(uint16_t)v);
}
static int cs_value_at(pmwin_t *w, cs_t *s, int x, int y)    /* the value nearest the point's angle */
{
    int cx, cy, r;
    cs_geom(w, &cx, &cy, &r);
    double a = atan2(y - cy, x - cx), d = cs_start(w) - a;    /* (how far round, clockwise) */
    while (d < 0) d += 2 * M_PI;
    while (d >= 2 * M_PI) d -= 2 * M_PI;
    double sw = cs_sweep(w);
    if (d > sw) d = d - sw < 2 * M_PI - d ? sw : 0;          /* (the gap at the bottom: the nearer end) */
    int v = s->lo + (int)lround(d / sw * (s->hi - s->lo));
    if (s->inc > 1) v = s->lo + (v - s->lo + s->inc / 2) / s->inc * s->inc;
    return v;
}
static uint32_t circslider_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    cs_t *s = csd(w);
    int cx, cy, r;
    cs_geom(w, &cx, &cy, &r);
    int btn = (w->style & 1) == 0;                               /* CSS_NOBUTTON */
    switch (msg) {
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_FACE));
        uint32_t tc = pp_fgc(w, C_TEXT);
        /* the dial: a disc, lit from the top left */
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
                if (d2 > r * r) continue;
                uint32_t c = d2 >= (r - 2) * (r - 2) ? (dy - dx > 0 ? C_LIGHT : C_DARK) : 0xD8D8D8;
                pel(w, x, y, c);
            }
        /* the ticks round it */
        if (!(w->style & 0x80) && s->tick > 0 && s->hi > s->lo) {
            int n = (s->hi - s->lo) / s->tick;
            if (n > 360) n = 360;
            for (int i = 0; i <= n; i++) {
                double a = cs_angle(w, s, s->lo + i * s->tick);
                int x0 = cx + (int)lround((r + 2) * cos(a)), y0 = cy + (int)lround((r + 2) * sin(a));
                int x1 = cx + (int)lround((r + 5) * cos(a)), y1 = cy + (int)lround((r + 5) * sin(a));
                line(w, x0, y0, x1, y1, tc);
            }
        }
        /* the value: a line out from the middle, a dot at its end */
        double a = cs_angle(w, s, s->val);
        int ex = cx + (int)lround((r - 5) * cos(a)), ey = cy + (int)lround((r - 5) * sin(a));
        line(w, cx, cy, ex, ey, C_HILITE);
        line(w, cx + 1, cy, ex + 1, ey, C_HILITE);
        for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) if (x * x + y * y <= 5) pel(w, ex + x, ey + y, C_HILITE);
        /* the number above it, the text below; the buttons */
        char num[16];
        snprintf(num, sizeof num, "%d", s->val);
        if (!(w->style & 4)) text_at(w, cx - text_w(num, (int)strlen(num)) / 2, w->cy - pm_ascent - 1, num, (int)strlen(num), tc);   /* CSS_NONUMBER */
        if (!(w->style & 2) && w->text[0]) {                     /* CSS_NOTEXT */
            int tw = plain_w(w->text);
            text_mid(w, cx - tw / 2, 0, 20, w->text, tc);
        }
        if (btn) {
            for (int k = 0; k < 2; k++) {
                int x0 = k ? w->cx - 18 : 2;
                fill(w, x0, 2, x0 + 16, 18, C_FACE);
                bevel(w, x0, 2, x0 + 16, 18, 1);
                line(w, x0 + 4, 10, x0 + 11, 10, tc);
                if (k) line(w, x0 + 8, 6, x0 + 8, 13, tc);
            }
        }
        if (pm_focus == w->hwnd) for (int k = 1; k < w->cx - 1; k += 2) { pel(w, k, 0, C_DARK); pel(w, k, w->cy - 1, C_DARK); }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16);
        set_focus(w->hwnd);
        if (btn && y >= 2 && y < 18) {
            if (x >= 2 && x < 18) { cs_set(w, s->val - s->inc, 0x0549); return 1; }               /* CSN_CHANGED */
            if (x >= w->cx - 18 && x < w->cx - 2) { cs_set(w, s->val + s->inc, 0x0549); return 1; }
        }
        int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy > (r + 6) * (r + 6)) return 1;
        s->drag = 1;
        pm_capture = w->hwnd;
        cs_set(w, cs_value_at(w, s, x, y), 0x054A);             /* CSN_TRACKING */
        return 1;
    }
    case WM_MOUSEMOVE:
        if (s->drag) cs_set(w, cs_value_at(w, s, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16)), 0x054A);
        return 1;
    case WM_BUTTON1UP:
        if (s->drag) {
            s->drag = 0; pm_capture = 0;
            cs_set(w, cs_value_at(w, s, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16)), 0x054A);
            notify(w, 0x0549, (uint32_t)(uint16_t)s->val);     /* (the drag's end: CSN_CHANGED) */
        }
        return 1;
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        if (vk == 0x15 || vk == 0x18) { cs_set(w, s->val - s->inc, 0x0549); return 1; }
        if (vk == 0x17 || vk == 0x16) { cs_set(w, s->val + s->inc, 0x0549); return 1; }
        if (vk == 0x12) { cs_set(w, s->val - s->tick, 0x0549); return 1; }    /* Page Down, Up: a tick */
        if (vk == 0x11) { cs_set(w, s->val + s->tick, 0x0549); return 1; }
        if (vk == 0x14) { cs_set(w, s->lo, 0x0549); return 1; }
        if (vk == 0x13) { cs_set(w, s->hi, 0x0549); return 1; }
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    }
    case 0x053E:                                                 /* CSM_SETRANGE: low, high */
        s->lo = (int16_t)mp1; s->hi = (int16_t)mp2;
        if (s->hi < s->lo) { int t = s->lo; s->lo = s->hi; s->hi = t; }
        if (s->val < s->lo) s->val = s->lo;
        if (s->val > s->hi) s->val = s->hi;
        repaint(w);
        return 1;
    case 0x053D:                                                 /* CSM_QUERYRANGE: PSHORT low, PSHORT high */
        if (mp1 && ok(mp1, 2)) p16(mp1, (uint16_t)s->lo);
        if (mp2 && ok(mp2, 2)) p16(mp2, (uint16_t)s->hi);
        return 1;
    case 0x0540: cs_set(w, (int16_t)mp1, 0); return 1;         /* CSM_SETVALUE (no notification: the program's own) */
    case 0x053F: if (mp1 && ok(mp1, 2)) p16(mp1, (uint16_t)s->val); return 1;   /* CSM_QUERYVALUE */
    case 0x0542:                                                 /* CSM_SETINCREMENT: the buttons', the ticks' */
        if ((int16_t)mp1 > 0) s->inc = (int16_t)mp1;
        if ((int16_t)mp2 > 0) s->tick = (int16_t)mp2;
        repaint(w);
        return 1;
    case 0x0543:                                                 /* CSM_QUERYINCREMENT */
        if (mp1 && ok(mp1, 2)) p16(mp1, (uint16_t)s->inc);
        if (mp2 && ok(mp2, 2)) p16(mp2, (uint16_t)s->tick);
        return 1;
    case 0x0541: if (mp1 && ok(mp1, 2)) p16(mp1, (uint16_t)r); return 1;          /* CSM_QUERYRADIUS */
    case 0x0544: return 1;                                       /* CSM_SETBITMAPDATA: (our own buttons) */
    }
    return 0;
}
