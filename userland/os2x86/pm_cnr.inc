/* userland/os2x86/pm_cnr.inc - Presentation Manager's container (WC_CONTAINER,
 * included at the end of pm.inc): records and field descriptions in the
 * program's memory (CM_ALLOCRECORD, CM_ALLOCDETAILFIELDINFO: a sub-
 * allocator of guest memory), the icon, name, text and details views
 * (string, number, date, time and icon columns), the tree view (records
 * under records, kept in the tree's order), the title, selection
 * (CN_EMPHASIS), opening (CN_ENTER: a double-click, Enter), sorting with
 * the program's compare function, a scroll bar. */

/* guest memory, a little at a time (records, notification structures) */
static uint32_t galloc_at, galloc_end;
static uint32_t galloc(uint32_t n)
{
    n = (n + 7) & ~7u;
    if (galloc_at + n > galloc_end) {
        uint32_t sz = n > 0x10000 ? n : 0x10000, a = mem_alloc(sz);
        if (!a) return 0;
        galloc_at = a; galloc_end = a + sz;
    }
    uint32_t r = galloc_at;
    galloc_at += n;
    memset(C.mem + r, 0, n);
    return r;
}

/* guest memory for what lives only through one call (notifications,
 * OWNERITEMs): a ring, reused */
static uint32_t gscratch_base, gscratch_at;
static uint32_t gscratch(uint32_t n)
{
    n = (n + 7) & ~7u;
    if (!gscratch_base && !(gscratch_base = mem_alloc(0x10000))) return 0;
    if (n > 0x1000) return galloc(n);
    if (gscratch_at + n > 0x10000) gscratch_at = 0;
    uint32_t r = gscratch_base + gscratch_at;
    gscratch_at += n;
    memset(C.mem + r, 0, n);
    return r;
}

enum { RC_ATTR = 4, RC_PTL = 8, RC_NEXT = 16, RC_PSZICON = 20, RC_HPTR = 24, RC_HPTRMINI = 28, RC_PSZTEXT = 44, RC_PSZNAME = 48, RC_PSZTREE = 52 };
enum { FI_FLDATA = 4, FI_FLTITLE = 8, FI_TITLE = 12, FI_OFF = 16, FI_NEXT = 24, FI_CX = 28 };
typedef struct {
    uint32_t *rec, *par; int n, cap;          /* (records in the tree's order: each one's children right after it; par: its parent) */
    int *vis, nv;                             /* (those shown: tree view, the expanded ones' children; else the top level) */
    uint32_t fld[32]; int nf;
    uint32_t attr, title;                     /* flWindowAttr, pszCnrTitle (the program's) */
    int top, cursor;                          /* (first row shown; the cursored record's index) */
    uint32_t vsb;
    uint32_t ed, ed_data;                     /* (direct editing: the entry field over the text, its CNREDITDATA) */
} cnr_t;
static cnr_t *cnr(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(cnr_t)); ((cnr_t *)w->ctl)->attr = 4; ((cnr_t *)w->ctl)->cursor = -1; } return w->ctl; }
static int cnr_mini(pmwin_t *w) { return (w->style & 0x40) != 0; }   /* CCS_MINIRECORDCORE */
static int cnr_index(cnr_t *c, uint32_t r) { for (int i = 0; i < c->n; i++) if (c->rec[i] == r) return i; return -1; }
static const char *gstr(uint32_t a) { char *s = a ? gs(a) : 0; return s ? s : ""; }
static int cnr_view(cnr_t *c) { return (c->attr & 0x40) ? 0x40 : (c->attr & 8) ? 8 : (c->attr & 1) ? 1 : (c->attr & 2) ? 2 : 4; }
/* the tree: k in i's subtree? where i's subtree ends; i has children? */
static int cnr_under(cnr_t *c, int k, uint32_t r)
{
    for (uint32_t p = c->par[k]; p; ) { if (p == r) return 1; int j = cnr_index(c, p); if (j < 0) break; p = c->par[j]; }
    return 0;
}
static int cnr_sub_end(cnr_t *c, int i) { int k = i + 1; while (k < c->n && cnr_under(c, k, c->rec[i])) k++; return k; }
static int cnr_kids(cnr_t *c, int i) { return i + 1 < c->n && c->par[i + 1] == c->rec[i]; }
static int cnr_depth(cnr_t *c, int i) { int d = 0; for (uint32_t p = c->par[i]; p; d++) { int j = cnr_index(c, p); if (j < 0) break; p = c->par[j]; } return d; }
static int cnr_next_sib(cnr_t *c, int i) { int e = cnr_sub_end(c, i); return e < c->n && c->par[e] == c->par[i] ? e : -1; }
static void cnr_revis(cnr_t *c)
{
    c->vis = realloc(c->vis, (size_t)(c->n + 1) * sizeof *c->vis);
    c->nv = 0;
    for (int i = 0; i < c->n; i++) {
        int shown = 1;
        if (!(c->attr & 0x40)) shown = c->par[i] == 0;
        else for (uint32_t p = c->par[i]; p; ) {
            int j = cnr_index(c, p);
            if (j < 0) break;
            if (!(g32(p + RC_ATTR) & 0x80)) { shown = 0; break; }   /* CRA_EXPANDED */
            p = c->par[j];
        }
        if (shown) c->vis[c->nv++] = i;
    }
}
static int cnr_pos(cnr_t *c, int i) { for (int k = 0; k < c->nv; k++) if (c->vis[k] == i) return k; return -1; }
static void cnr_relink(cnr_t *c)              /* preccNextRecord: the next sibling */
{
    for (int i = 0; i < c->n; i++) { int s = cnr_next_sib(c, i); p32(c->rec[i] + RC_NEXT, s >= 0 ? c->rec[s] : 0); }
}
static int cnr_tree_icons(cnr_t *c) { return !(c->attr & 3); }   /* (the tree: CV_ICON's icons, or CV_NAME's small ones, or CV_TEXT's none) */
static int cnr_indent(void) { return 18; }
static int cnr_title_h(pmwin_t *w, cnr_t *c) { (void)w; return (c->attr & 0x200) ? pm_lineh + 6 : 0; }
static int cnr_head_h(cnr_t *c) { return cnr_view(c) == 8 && (c->attr & 0x8000) ? pm_lineh + 6 : 0; }
static int cnr_row_h(cnr_t *c)
{
    if (cnr_view(c) == 0x40) { int ih = cnr_tree_icons(c) ? 34 : (c->attr & 2) ? 18 : 0; return ih > pm_lineh + 4 ? ih : pm_lineh + 4; }
    return cnr_view(c) == 8 ? (pm_lineh > 20 ? pm_lineh : 20) + 2 : pm_lineh + 4;
}
static int cnr_cell_w(void) { return 96; }
static int cnr_cell_h(void) { return 40 + pm_lineh + 4; }
static int cnr_cols(pmwin_t *w) { int k = (w->cx - 18) / cnr_cell_w(); return k < 1 ? 1 : k; }
/* rows shown, rows there are (icon view: rows of cells) */
static int cnr_area_h(pmwin_t *w, cnr_t *c) { return w->cy - cnr_title_h(w, c) - cnr_head_h(c); }
static int cnr_rows_shown(pmwin_t *w, cnr_t *c) { int h = cnr_view(c) == 4 ? cnr_cell_h() : cnr_row_h(c), r = cnr_area_h(w, c) / h; return r < 1 ? 1 : r; }
static int cnr_rows(pmwin_t *w, cnr_t *c) { return cnr_view(c) == 4 ? (c->nv + cnr_cols(w) - 1) / cnr_cols(w) : c->nv; }

/* a detail column's text for record r */
static void cell_text(uint32_t r, uint32_t fi, char *out, size_t n)
{
    uint32_t fl = g32(fi + FI_FLDATA), at = r + g32(fi + FI_OFF);
    out[0] = 0;
    if (fl & 0x800) snprintf(out, n, "%s", gstr(g32(at)));                          /* CFA_STRING: a PSZ */
    else if (fl & 0x20000) snprintf(out, n, "%u", g32(at));                         /* CFA_ULONG */
    else if (fl & 0x2000) snprintf(out, n, "%02u-%02u-%04u", C.mem[at + 1], C.mem[at], g16(at + 2));   /* CFA_DATE: mm-dd-yyyy */
    else if (fl & 0x4000) snprintf(out, n, "%02u:%02u:%02u", C.mem[at], C.mem[at + 1], C.mem[at + 2]);  /* CFA_TIME */
}
static int col_w(pmwin_t *w, cnr_t *c, int k)
{
    uint32_t fi = c->fld[k], cx = g32(fi + FI_CX);
    (void)w;
    if (cx) return (int)cx;
    int best = (g32(fi + FI_FLTITLE) & 0x100) ? 24 : plain_w(gstr(g32(fi + FI_TITLE)));   /* (a title: its width) */
    for (int v = 0; v < c->nv; v++) {
        int i = c->vis[v];
        char b[128];
        int tw = (g32(fi + FI_FLDATA) & 0x100) ? 20 : (cell_text(c->rec[i], fi, b, sizeof b), text_w(b, (int)strlen(b)));
        if (tw > best) best = tw;
    }
    return best + 14;
}
static void cnr_scrollbar(pmwin_t *w, cnr_t *c)
{
    int rows = cnr_rows(w, c), shown = cnr_rows_shown(w, c);
    pmwin_t *s = win(c->vsb);
    if (rows <= shown) { if (s) s->visible = 0; if (c->top) c->top = 0; return; }
    if (!s) {
        s = win_new(w, "#8", 1, 0);
        if (!s) return;
        s->owner = w;
        c->vsb = s->hwnd;
    }
    s->x = w->cx - 16; s->y = 0; s->cx = 16; s->cy = w->cy - cnr_title_h(w, c);
    s->visible = 1;
    if (c->top > rows - shown) c->top = rows - shown;
    scroll_proc(s, 0x01a0, (uint32_t)c->top, (uint32_t)(rows - shown) << 16);
    scroll_proc(s, 0x01a6, (uint32_t)shown | (uint32_t)rows << 16, 0);
}
static void cnr_notify_emph(pmwin_t *w, uint32_t r, uint32_t mask)
{
    uint32_t n = gscratch(12);
    if (!n) return;
    p32(n, w->hwnd); p32(n + 4, r); p32(n + 8, mask);
    notify(w, 108, n);                                       /* CN_EMPHASIS */
}
static void cnr_select(pmwin_t *w, cnr_t *c, int i)
{
    if (i < 0 || i >= c->n) return;
    int single = !(w->style & 3);                            /* (not CCS_MULTIPLESEL, CCS_EXTENDSEL) */
    for (int k = 0; k < c->n; k++) {
        uint32_t a = g32(c->rec[k] + RC_ATTR), was = a;
        if (k == i) a |= 1 | 4; else { if (single) a &= ~1u; a &= ~4u; }   /* CRA_SELECTED, CRA_CURSORED */
        if (a != was) { p32(c->rec[k] + RC_ATTR, a); if ((a ^ was) & 1) cnr_notify_emph(w, c->rec[k], 1); }
    }
    c->cursor = i;
    int p = cnr_pos(c, i), shown = cnr_rows_shown(w, c), row = cnr_view(c) == 4 ? p / cnr_cols(w) : p;
    if (p >= 0 && row < c->top) c->top = row;
    if (p >= 0 && row >= c->top + shown) c->top = row - shown + 1;
    repaint(w);
}
static void cnr_enter(pmwin_t *w, uint32_t r, int key)
{
    uint32_t n = gscratch(12);
    if (!n) return;
    p32(n, w->hwnd); p32(n + 4, (uint32_t)key); p32(n + 8, r);
    notify(w, 106, n);                                       /* CN_ENTER */
}
/* the record at (x, y), its index, -1 none */
static int cnr_hit(pmwin_t *w, cnr_t *c, int x, int y)
{
    int top = w->cy - cnr_title_h(w, c) - cnr_head_h(c), from = top - y;
    if (from < 0) return -1;
    if (cnr_view(c) == 4) {
        int col = (x - 4) / cnr_cell_w(), row = from / cnr_cell_h() + c->top;
        if (col < 0 || col >= cnr_cols(w)) return -1;
        int i = row * cnr_cols(w) + col;
        return i < c->nv ? c->vis[i] : -1;
    }
    int i = from / cnr_row_h(c) + c->top;
    return i < c->nv ? c->vis[i] : -1;
}
static void cnr_paint(pmwin_t *w, cnr_t *c)
{
    int view = cnr_view(c), sbw = win(c->vsb) && win(c->vsb)->visible ? 16 : 0, right = w->cx - sbw;
    fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_LIGHT));
    uint32_t tc = pp_fgc(w, C_TEXT);
    int y = w->cy;
    if (c->attr & 0x200) {                                   /* CA_CONTAINERTITLE */
        int th = cnr_title_h(w, c);
        const char *t = gstr(c->title);
        int tw = plain_w(t), x = (c->attr & 0x2000) ? (w->cx - tw) / 2 : (c->attr & 0x1000) ? w->cx - tw - 6 : 6;
        text_mid(w, x, y - th, y, t, tc);
        if (c->attr & 0x400) line(w, 0, y - th, w->cx - 1, y - th, C_DARK);   /* CA_TITLESEPARATOR */
        y -= th;
    }
    if (view == 8) {                                         /* details: the columns */
        int hh = cnr_head_h(c), rh = cnr_row_h(c), x = 0;
        for (int k = 0; k < c->nf; k++) {
            uint32_t fi = c->fld[k], fl = g32(fi + FI_FLDATA), ft = g32(fi + FI_FLTITLE);
            if (fl & 0x40) continue;                         /* CFA_INVISIBLE */
            int cw = col_w(w, c, k);
            if (hh) {
                const char *t = (ft & 0x100) ? "" : gstr(g32(fi + FI_TITLE));
                int tw = plain_w(t), tx = (ft & 4) ? x + (cw - tw) / 2 : (ft & 2) ? x + cw - tw - 6 : x + 6;
                text_mid(w, tx, y - hh, y, t, tc);
                if (fl & 0x400) line(w, x, y - hh, x + cw - 1, y - hh, C_DARK);   /* CFA_HORZSEPARATOR */
            }
            for (int r = 0; r + c->top < c->nv; r++) {
                int ry1 = y - hh - r * rh, ry0 = ry1 - rh, i = c->vis[r + c->top];
                if (ry1 <= 0) break;
                uint32_t rec = c->rec[i], a = g32(rec + RC_ATTR);
                uint32_t fg = (a & 1) ? C_LIGHT : tc;
                if (a & 1) fill(w, x, ry0, x + cw, ry1, C_HILITE);
                if (fl & 0x100) {                            /* CFA_BITMAPORICON: an icon's (pointer's) handle */
                    pmwin_t *p = ptr_get(g32(rec + g32(fi + FI_OFF)));
                    if (p) ptr_draw(w, x + (cw - p->cx / 2) / 2, ry0 + (rh - p->cy / 2) / 2, p, 4);   /* (DP_MINI) */
                } else {
                    char b[128];
                    cell_text(rec, fi, b, sizeof b);
                    int tw = text_w(b, (int)strlen(b)), tx = (fl & 4) ? x + (cw - tw) / 2 : (fl & 2) ? x + cw - tw - 6 : x + 6;
                    text_at(w, tx, ry0 + (rh - pm_ascent - pm_descent) / 2 + pm_descent, b, (int)strlen(b), fg);
                }
                if ((a & 4) && pm_focus == w->hwnd && k == 0) for (int px = 1; px < right - 1; px += 2) { pel(w, px, ry0, C_TEXT); pel(w, px, ry1 - 1, C_TEXT); }
            }
            if (fl & 0x200) line(w, x + cw - 1, 0, x + cw - 1, y - 1, C_DARK);   /* CFA_SEPARATOR */
            x += cw;
        }
    } else if (view == 4) {                                  /* icons: a grid of them */
        int cols = cnr_cols(w), ch = cnr_cell_h(), cwid = cnr_cell_w(), mini = (c->attr & 0x20) != 0;
        for (int v = c->top * cols; v < c->nv; v++) {
            int row = v / cols - c->top, col = v % cols, x0 = 4 + col * cwid, y1 = y - row * ch;
            if (y1 <= 0) break;
            uint32_t rec = c->rec[c->vis[v]], a = g32(rec + RC_ATTR);
            pmwin_t *p = ptr_get(g32(rec + (mini && !cnr_mini(w) ? RC_HPTRMINI : RC_HPTR)));
            if (!p && mini) p = ptr_get(g32(rec + RC_HPTR));
            int isz = mini ? 16 : 32;
            if (p) ptr_draw(w, x0 + (cwid - isz) / 2, y1 - 4 - isz, p, mini ? 4 : 0);
            const char *t = gstr(g32(rec + RC_PSZICON));
            int tw = plain_w(t), tx = x0 + (cwid - tw) / 2, ty = y1 - 8 - isz - pm_ascent;
            if (tw > cwid - 4) tx = x0 + 2;
            if (a & 1) fill(w, tx - 2, ty - pm_descent - 1, tx + (tw < cwid - 4 ? tw : cwid - 4) + 2, ty + pm_ascent + 1, C_HILITE);
            text_at(w, tx, ty, t, (int)strlen(t), (a & 1) ? C_LIGHT : tc);
        }
    } else if (view == 0x40) {                               /* the tree: indented, the + and - buttons, the lines */
        int rh = cnr_row_h(c), ind = cnr_indent(), lines = (c->attr & 0x400000) != 0;   /* CA_TREELINE */
        for (int r = 0; r + c->top < c->nv; r++) {
            int ry1 = y - r * rh, ry0 = ry1 - rh, i = c->vis[r + c->top], d = cnr_depth(c, i), my = (ry0 + ry1) / 2;
            if (ry1 <= 0) break;
            uint32_t rec = c->rec[i], a = g32(rec + RC_ATTR);
            int bx = 4 + d * ind;                            /* (the button's left; its middle bx + 4) */
            if (lines) {
                #define DOTS_V(x, y0, y1) for (int q = (y0); q < (y1); q += 2) pel(w, x, q, C_DARK)
                #define DOTS_H(x0, x1, yy) for (int q = (x0); q < (x1); q += 2) pel(w, q, yy, C_DARK)
                if (d > 0) {
                    int px = bx - ind + 4;
                    DOTS_H(px, bx + (cnr_kids(c, i) ? 0 : 8), my);
                    DOTS_V(px, cnr_next_sib(c, i) >= 0 ? ry0 : my, ry1);
                    /* the ancestors' lines, going on past this row to their next siblings */
                    int k = cnr_index(c, c->par[i]);
                    for (int dd = d - 1; dd > 0 && k >= 0; dd--) {
                        if (cnr_next_sib(c, k) >= 0) DOTS_V(4 + (dd - 1) * ind + 4, ry0, ry1);
                        k = cnr_index(c, c->par[k]);
                    }
                }
                if (cnr_kids(c, i) && (a & 0x80)) DOTS_V(bx + 4, ry0, my - 4);   /* (down to the first child) */
                #undef DOTS_V
                #undef DOTS_H
            }
            if (cnr_kids(c, i)) {                            /* the button: + collapsed, - expanded */
                fill(w, bx, my - 4, bx + 9, my + 5, C_LIGHT);
                line(w, bx, my - 4, bx + 8, my - 4, C_DARK); line(w, bx, my + 4, bx + 8, my + 4, C_DARK);
                line(w, bx, my - 4, bx, my + 4, C_DARK); line(w, bx + 8, my - 4, bx + 8, my + 4, C_DARK);
                line(w, bx + 2, my, bx + 6, my, tc);
                if (!(a & 0x80)) line(w, bx + 4, my - 2, bx + 4, my + 2, tc);
            }
            int x = bx + 14;
            const char *t;
            if (c->attr & 1) t = gstr(g32(rec + RC_PSZTEXT));
            else if (c->attr & 2) { pmwin_t *p = ptr_get(g32(rec + RC_HPTR)); if (p) ptr_draw(w, x, ry0 + (rh - 16) / 2, p, 4); x += 20; t = gstr(g32(rec + RC_PSZNAME)); }
            else { pmwin_t *p = ptr_get(g32(rec + RC_HPTR)); if (p) ptr_draw(w, x, ry0 + (rh - 32) / 2, p, 0); x += 36; t = gstr(g32(rec + RC_PSZICON)); }
            int tw = plain_w(t);
            if (a & 1) fill(w, x - 2, ry0 + 1, x + tw + 2, ry1 - 1, C_HILITE);
            text_mid(w, x, ry0, ry1, t, (a & 1) ? C_LIGHT : tc);
            if ((a & 4) && pm_focus == w->hwnd) for (int px = x - 2; px < x + tw + 2; px += 2) { pel(w, px, ry0 + 1, tc); pel(w, px, ry1 - 2, tc); }
        }
    } else {                                                 /* name, text: rows */
        int rh = cnr_row_h(c), mini = view == 2;
        for (int r = 0; r + c->top < c->nv; r++) {
            int ry1 = y - r * rh, ry0 = ry1 - rh, i = c->vis[r + c->top];
            if (ry1 <= 0) break;
            uint32_t rec = c->rec[i], a = g32(rec + RC_ATTR);
            int x = 4;
            if (mini) { pmwin_t *p = ptr_get(g32(rec + RC_HPTR)); if (p) ptr_draw(w, x, ry0 + (rh - 16) / 2, p, 4); x += 20; }
            const char *t = gstr(g32(rec + (view == 1 ? RC_PSZTEXT : cnr_mini(w) ? RC_PSZICON : RC_PSZNAME)));
            int tw = plain_w(t);
            if (a & 1) fill(w, x - 2, ry0 + 1, x + tw + 2, ry1 - 1, C_HILITE);
            text_mid(w, x, ry0, ry1, t, (a & 1) ? C_LIGHT : tc);
        }
    }
    (void)right;
    present(w);
}
/* the children of parent (0: the top level), sorted, each followed by its own, into out */
static int cnr_sort_into(cnr_t *c, uint32_t parent, uint32_t *out, uint32_t *opar, int k, uint32_t fn, uint32_t storage)
{
    int m = 0;
    uint32_t *g = malloc((size_t)(c->n + 1) * sizeof *g);
    for (int i = 0; i < c->n; i++) if (c->par[i] == parent) g[m++] = c->rec[i];
    for (int i = 1; i < m; i++) {                            /* (insertion: few records, a callback each) */
        uint32_t r = g[i];
        int j = i - 1;
        while (j >= 0) {
            uint32_t args[3] = { g[j], r, storage };
            if ((int16_t)guest_call(fn, 3, args) <= 0) break;
            g[j + 1] = g[j];
            j--;
        }
        g[j + 1] = r;
    }
    for (int i = 0; i < m; i++) { out[k] = g[i]; opar[k] = parent; k++; k = cnr_sort_into(c, g[i], out, opar, k, fn, storage); }
    free(g);
    return k;
}
static void cnr_sort(pmwin_t *w, cnr_t *c, uint32_t fn, uint32_t storage)
{
    uint32_t *out = malloc((size_t)(c->n + 1) * sizeof *out), *opar = malloc((size_t)(c->n + 1) * sizeof *opar);
    int k = cnr_sort_into(c, 0, out, opar, 0, fn, storage);
    if (k == c->n) { memcpy(c->rec, out, (size_t)k * sizeof *out); memcpy(c->par, opar, (size_t)k * sizeof *opar); }
    free(out); free(opar);
    cnr_relink(c);
    cnr_revis(c);
    repaint(w);
}
/* expand (on) or collapse record i: CN_EXPANDTREE, CN_COLLAPSETREE */
static void cnr_expand(pmwin_t *w, cnr_t *c, int i, int on)
{
    uint32_t r = c->rec[i], a = g32(r + RC_ATTR);
    if (!!(a & 0x80) == on) return;
    p32(r + RC_ATTR, on ? (a | 0x80) & ~0x100u : (a | 0x100) & ~0x80u);
    cnr_revis(c);
    if (!on && c->cursor >= 0 && cnr_pos(c, c->cursor) < 0) cnr_select(w, c, i);   /* (the cursor was in what's hidden now) */
    repaint(w);
    notify(w, on ? 117 : 116, r);
}
/* ── direct editing: an entry field over a record's text (Alt+click,
 * CM_OPENEDIT); Enter or a click elsewhere keeps it - CN_REALLOCPSZ, so
 * the program makes room, the text into its string, CN_ENDEDIT - and Esc
 * drops it ── */
static int cnr_top_y(pmwin_t *w, cnr_t *c) { return w->cy - cnr_title_h(w, c) - cnr_head_h(c); }
/* where record i's text is (field fi, details view), and the address of its PSZ */
static int cnr_text_at(pmwin_t *w, cnr_t *c, int i, uint32_t fi, int *x0, int *y0, int *x1, int *y1, uint32_t *ppsz)
{
    int view = cnr_view(c), p = cnr_pos(c, i), rh = cnr_row_h(c), y = cnr_top_y(w, c);
    uint32_t rec = c->rec[i];
    if (p < 0) return 0;
    if (view == 4) {
        int cols = cnr_cols(w), ch = cnr_cell_h(), cw = cnr_cell_w(), row = p / cols - c->top, col = p % cols;
        int isz = (c->attr & 0x20) ? 16 : 32, cx0 = 4 + col * cw, ty = y - row * ch - 8 - isz - pm_ascent;
        *x0 = cx0; *x1 = cx0 + cw; *y0 = ty - pm_descent - 3; *y1 = ty + pm_ascent + 3;
        *ppsz = rec + RC_PSZICON;
        return 1;
    }
    int ry1 = y - (p - c->top) * rh, ry0 = ry1 - rh;
    *y0 = ry0; *y1 = ry1;
    if (view == 8) {
        int x = 0;
        for (int k = 0; k < c->nf; k++) {
            uint32_t f = c->fld[k], fl = g32(f + FI_FLDATA);
            if (fl & 0x40) continue;
            int cw = col_w(w, c, k);
            if (f == fi) {
                if (!(fl & 0x800)) return 0;                 /* (strings only: CFA_STRING) */
                *x0 = x; *x1 = x + cw; *ppsz = rec + g32(f + FI_OFF);
                return 1;
            }
            x += cw;
        }
        return 0;
    }
    int x = 4;
    if (view == 0x40) { x = 4 + cnr_depth(c, i) * cnr_indent() + 14 + (c->attr & 1 ? 0 : c->attr & 2 ? 20 : 36); *ppsz = rec + (c->attr & 1 ? RC_PSZTEXT : c->attr & 2 ? RC_PSZNAME : RC_PSZICON); }
    else if (view == 2) { x = 24; *ppsz = rec + (cnr_mini(w) ? RC_PSZICON : RC_PSZNAME); }
    else *ppsz = rec + RC_PSZTEXT;
    *x0 = x - 3; *x1 = x + plain_w(gstr(g32(*ppsz))) + 40;
    return 1;
}
static void cnr_end_edit(pmwin_t *w, cnr_t *c, int keep)
{
    pmwin_t *e = win(c->ed);
    uint32_t d = c->ed_data;
    if (!e) { c->ed = 0; return; }
    char t[256];
    snprintf(t, sizeof t, "%s", e->text);
    win_destroy(e);
    c->ed = 0;
    set_focus(w->hwnd);
    if (keep && d) {
        pmwin_t *o = w->owner ? w->owner : w->parent;
        p32(d + 20, (uint32_t)strlen(t) + 1);                /* cbText: what it needs */
        uint32_t pp = g32(d + 16);
        uint32_t r = o ? pm_send(o, WM_CONTROL, (w->id & 0xFFFF) | 113u << 16, d) : 0;   /* CN_REALLOCPSZ */
        uint32_t s = g32(pp);
        if (r && s && ok(s, (uint32_t)strlen(t) + 1)) memcpy(C.mem + s, t, strlen(t) + 1);
    }
    if (d) notify(w, 115, d);                                /* CN_ENDEDIT */
    repaint(w);
}
static int cnr_open_edit(pmwin_t *w, cnr_t *c, int i, uint32_t fi)
{
    int x0, y0, x1, y1;
    uint32_t pp;
    if (c->ed) cnr_end_edit(w, c, 1);
    if (i < 0 || i >= c->n || (g32(c->rec[i] + RC_ATTR) & 0x40)) return 0;   /* CRA_RECORDREADONLY */
    if (cnr_view(c) == 8 && !fi) for (int k = 0; k < c->nf && !fi; k++) if (g32(c->fld[k] + FI_FLDATA) & 0x800) fi = c->fld[k];
    if (!cnr_text_at(w, c, i, fi, &x0, &y0, &x1, &y1, &pp)) return 0;
    int sbw = win(c->vsb) && win(c->vsb)->visible ? 16 : 0;
    if (x1 > w->cx - sbw) x1 = w->cx - sbw;
    if (x0 < 0) x0 = 0;
    uint32_t d = galloc(28);                                 /* CNREDITDATA: cb, hwndCnr, pRecord, pFieldInfo, ppszText, cbText, id */
    if (!d) return 0;
    p32(d, 28); p32(d + 4, w->hwnd); p32(d + 8, c->rec[i]); p32(d + 12, cnr_view(c) == 8 ? fi : 0);
    p32(d + 16, pp); p32(d + 20, (uint32_t)strlen(gstr(g32(pp))) + 1); p32(d + 24, w->id);
    pmwin_t *e = win_new(w, "#6", 0x80000000u | 8, w->id);  /* WS_VISIBLE, ES_MARGIN */
    if (!e) return 0;
    e->owner = w;
    e->x = x0; e->y = y0; e->cx = x1 - x0; e->cy = y1 - y0;
    e->visible = 1;
    snprintf(e->text, sizeof e->text, "%s", gstr(g32(pp)));
    entry_proc(e, 0x0143, 255, 0);                            /* (EM_SETTEXTLIMIT) */
    entry_proc(e, 0x0142, (uint32_t)strlen(e->text) << 16, 0);   /* (all of it selected) */
    c->ed = e->hwnd; c->ed_data = d;
    cnr_select(w, c, i);
    set_focus(e->hwnd);
    notify(w, 114, d);                                       /* CN_BEGINEDIT */
    repaint(w);
    return 1;
}
static uint32_t container_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    cnr_t *c = cnr(w);
    cnr_revis(c);
    switch (msg) {
    case WM_DESTROY: c->ed = 0; free(c->rec); free(c->par); free(c->vis); return 0;
    case WM_SIZE: cnr_scrollbar(w, c); repaint(w); return 0;
    case WM_PAINT: w->invalid = 0; cnr_scrollbar(w, c); cnr_paint(w, c); return 0;
    case WM_BUTTON1DOWN: case WM_BUTTON2DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF), i = cnr_hit(w, c, x, (int16_t)(mp1 >> 16));
        if (c->ed) cnr_end_edit(w, c, 1);                    /* (a click elsewhere: the edit's kept) */
        set_focus(w->hwnd);
        if (i >= 0 && msg == WM_BUTTON1DOWN && ((mp2 >> 16) & KC_ALT)) {   /* Alt+click: edit the text */
            uint32_t fi = 0;
            if (cnr_view(c) == 8) {                           /* (the column clicked) */
                int cx = 0;
                for (int k = 0; k < c->nf; k++) {
                    if (g32(c->fld[k] + FI_FLDATA) & 0x40) continue;
                    int cw = col_w(w, c, k);
                    if (x >= cx && x < cx + cw) fi = c->fld[k];
                    cx += cw;
                }
                if (!fi) return 1;
            }
            cnr_open_edit(w, c, i, fi);
            return 1;
        }
        if (i >= 0 && msg == WM_BUTTON1DOWN && cnr_view(c) == 0x40 && cnr_kids(c, i)) {   /* the + or - button */
            int bx = 4 + cnr_depth(c, i) * cnr_indent();
            if (x >= bx - 2 && x <= bx + 10) { cnr_expand(w, c, i, !(g32(c->rec[i] + RC_ATTR) & 0x80)); return 1; }
        }
        if (i >= 0) cnr_select(w, c, i);
        return 1;
    }
    case WM_BUTTON2UP:                                       /* button 2 let go - not after a drag: the menu (CN_CONTEXTMENU) */
        if (!pm_b2_began) { int i = cnr_hit(w, c, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16)); notify(w, 119, i >= 0 ? c->rec[i] : 0); }
        return 1;
    case 0x0420: {                                           /* WM_BEGINDRAG: CN_INITDRAG (CNRDRAGINIT: hwndCnr, pRecord, x, y, cx, cy) */
        int x = (int16_t)(mp2 & 0xFFFF), y = (int16_t)(mp2 >> 16), i = cnr_hit(w, c, x, y);
        uint32_t n = gscratch(24);
        if (!n) return 0;
        p32(n, w->hwnd); p32(n + 4, i >= 0 ? c->rec[i] : 0); p32(n + 8, (uint32_t)x); p32(n + 12, (uint32_t)y);
        notify(w, 107, n);
        return 1;
    }
    case 0x032e: case 0x032d: case 0x032f: {                 /* DM_DRAGOVER, DM_DRAGLEAVE, DM_DROP: CN_DRAGOVER, CN_DRAGLEAVE, CN_DROP (CNRDRAGINFO) */
        uint32_t d = mp1, n = gscratch(8);
        if (!n) return 0;
        int dx = msg == 0x032e ? (int16_t)(mp2 & 0xFFFF) : (int16_t)g16(d + 12), dy = msg == 0x032e ? (int16_t)(mp2 >> 16) : (int16_t)g16(d + 14), ax, ay;
        win_abs(w, &ax, &ay);
        pmwin_t *f = frame_of(w);
        int i = msg == 0x032d ? -1 : cnr_hit(w, c, dx - ax - (f ? f->x : 0), dy - ay - (f ? f->y : 0));
        p32(n, d); p32(n + 4, i >= 0 ? c->rec[i] : 0);
        pmwin_t *o = w->owner ? w->owner : w->parent;
        return o ? pm_send(o, WM_CONTROL, (w->id & 0xFFFF) | (msg == 0x032e ? 103u : msg == 0x032d ? 102u : 104u) << 16, n) : 0;
    }
    case WM_BUTTON1DBLCLK: {
        int i = cnr_hit(w, c, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16));
        if (i >= 0) { cnr_select(w, c, i); cnr_enter(w, c->rec[i], 0); }
        return 1;
    }
    case 0x0031: {                                           /* its scroll bar's WM_VSCROLL */
        int cmd = (int)(mp2 >> 16), shown = cnr_rows_shown(w, c), hi = cnr_rows(w, c) - shown;
        if (cmd == 1) c->top--; else if (cmd == 2) c->top++; else if (cmd == 3) c->top -= shown; else if (cmd == 4) c->top += shown;
        else if (cmd == 5 || cmd == 6) c->top = (int16_t)(mp2 & 0xFFFF); else return 0;
        if (c->top > hi) c->top = hi;
        if (c->top < 0) c->top = 0;
        repaint(w);
        return 0;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, ch = (mp1 & KC_CHAR) ? (int)(mp2 & 0xFFFF) : 0;
        if (c->ed) {                                         /* (the edit's entry field passes Enter and Esc up) */
            if (vk == 0x08 || vk == 0x1e) { cnr_end_edit(w, c, 1); return 1; }
            if (vk == 0x0f) { cnr_end_edit(w, c, 0); return 1; }
            return 1;
        }
        int step = cnr_view(c) == 4 ? cnr_cols(w) : 1, i = c->cursor >= 0 ? cnr_pos(c, c->cursor) : -1;
        if (cnr_view(c) == 0x40 && c->cursor >= 0) {          /* the tree: Right, +: open (then down into it); Left, -: close (then up to the parent) */
            int k = c->cursor, open = (g32(c->rec[k] + RC_ATTR) & 0x80) != 0;
            if (vk == 0x17 || ch == '+') { if (cnr_kids(c, k) && !open) cnr_expand(w, c, k, 1); else if (vk == 0x17 && cnr_kids(c, k)) cnr_select(w, c, k + 1); return 1; }
            if (vk == 0x15 || ch == '-') { if (cnr_kids(c, k) && open) cnr_expand(w, c, k, 0); else if (vk == 0x15 && c->par[k]) cnr_select(w, c, cnr_index(c, c->par[k])); return 1; }
        }
        if (vk == 0x18) i += step; else if (vk == 0x16) i -= step;
        else if (vk == 0x17 && cnr_view(c) == 4) i++; else if (vk == 0x15 && cnr_view(c) == 4) i--;
        else if (vk == 0x14) i = 0; else if (vk == 0x13) i = c->nv - 1;
        else if (vk == 0x12) i += cnr_rows_shown(w, c) * step; else if (vk == 0x11) i -= cnr_rows_shown(w, c) * step;
        else if ((vk == 0x08 || vk == 0x1e) && c->cursor >= 0) { cnr_enter(w, c->rec[c->cursor], 1); return 1; }
        else return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        if (i < 0) i = 0;
        if (i >= c->nv) i = c->nv - 1;
        if (i >= 0) cnr_select(w, c, c->vis[i]);
        return 1;
    }
    case 0x0331: {                                           /* CM_ALLOCRECORD: cbRecordData (extra), count */
        uint32_t core = cnr_mini(w) ? 28 : 56, size = (core + mp1 + 3) & ~3u, first = 0, prev = 0;
        for (uint32_t k = 0; k < (mp2 ? mp2 : 1); k++) {
            uint32_t r = galloc(size);
            if (!r) return 0;
            p32(r, core);
            if (prev) p32(prev + RC_NEXT, r); else first = r;
            prev = r;
        }
        return first;
    }
    case 0x0336: return 1;                                   /* CM_FREERECORD (the memory's ours: kept) */
    case 0x0339: {                                           /* CM_INSERTRECORD: the records (chained), RECORDINSERT */
        uint32_t ri = mp2, order = g32(ri + 4), parent = g32(ri + 8), cnt = g32(ri + 20), r = mp1;
        int pi = parent ? cnr_index(c, parent) : -1;
        if (pi < 0) parent = 0;
        int at;                                              /* CMA_FIRST: first of the parent's; CMA_END: last; else after a record (and its own) */
        if (order == 0x10) at = pi + 1;
        else if (order == 0x40 || order == 0) at = pi >= 0 ? cnr_sub_end(c, pi) : c->n;
        else { int k = cnr_index(c, order); at = k >= 0 ? cnr_sub_end(c, k) : (pi >= 0 ? cnr_sub_end(c, pi) : c->n); }
        if (at < 0 || at > c->n) at = c->n;
        for (uint32_t k = 0; k < (cnt ? cnt : 1) && r; k++) {
            if (c->n == c->cap) {
                c->cap = c->cap ? c->cap * 2 : 32;
                c->rec = realloc(c->rec, (size_t)c->cap * sizeof *c->rec);
                c->par = realloc(c->par, (size_t)c->cap * sizeof *c->par);
            }
            memmove(c->rec + at + 1, c->rec + at, (size_t)(c->n - at) * sizeof *c->rec);
            memmove(c->par + at + 1, c->par + at, (size_t)(c->n - at) * sizeof *c->par);
            uint32_t next = g32(r + RC_NEXT);
            c->par[at] = parent;
            c->rec[at++] = r;
            c->n++;
            r = next;
        }
        cnr_relink(c);
        cnr_revis(c);
        repaint(w);
        return (uint32_t)c->n;
    }
    case 0x0351: {                                           /* CM_INSERTRECORDARRAY: array of records, count, RECORDINSERT */
        uint32_t arr = mp1, cnt = g32(mp2 + 20);
        for (uint32_t k = 0; k < cnt; k++) p32(g32(arr + 4 * k) + RC_NEXT, k + 1 < cnt ? g32(arr + 4 * (k + 1)) : 0);
        return container_proc(w, 0x0339, g32(arr), mp2);
    }
    case 0x0346: {                                           /* CM_REMOVERECORD: array, (count, flags): the count left */
        int cnt = (int)(mp2 & 0xFFFF);
        if (!cnt) { c->n = 0; c->cursor = -1; c->top = 0; }
        for (int k = 0; k < cnt; k++) {                      /* (a record: its children too) */
            int i = cnr_index(c, g32(mp1 + 4u * (uint32_t)k));
            if (i < 0) continue;
            int e = cnr_sub_end(c, i), gone = e - i;
            uint32_t cur = c->cursor >= 0 && c->cursor < c->n ? c->rec[c->cursor] : 0;
            memmove(c->rec + i, c->rec + e, (size_t)(c->n - e) * sizeof *c->rec);
            memmove(c->par + i, c->par + e, (size_t)(c->n - e) * sizeof *c->par);
            c->n -= gone;
            c->cursor = cur ? cnr_index(c, cur) : -1;
            if (c->cursor < 0 && c->n) c->cursor = i < c->n ? i : c->n - 1;
        }
        cnr_relink(c);
        cnr_revis(c);
        repaint(w);
        return (uint32_t)c->n;
    }
    case 0x0340: {                                           /* CM_QUERYRECORD: a record, (CMA_FIRST/LAST/NEXT/PREV, CMA_ITEMORDER) */
        uint32_t how = mp2 & 0xFFFF;
        int i = cnr_index(c, mp1), r = -1;
        if (how == 0x10) r = c->n ? 0 : -1;                                  /* CMA_FIRST: the top level's */
        else if (how == 0x20) { for (int k = 0; k < c->n; k++) if (!c->par[k]) r = k; }   /* CMA_LAST */
        else if (i < 0) return 0;
        else if (how == 0x100) r = cnr_next_sib(c, i);                       /* CMA_NEXT, CMA_PREV: siblings */
        else if (how == 0x80) { for (int k = 0; k < i; k++) if (c->par[k] == c->par[i]) r = k; }
        else if (how == 1) r = c->par[i] ? cnr_index(c, c->par[i]) : -1;    /* CMA_PARENT */
        else if (how == 2) r = cnr_kids(c, i) ? i + 1 : -1;                 /* CMA_FIRSTCHILD */
        else if (how == 4) { for (int k = i + 1; k < c->n && cnr_under(c, k, c->rec[i]); k++) if (c->par[k] == c->rec[i]) r = k; }   /* CMA_LASTCHILD */
        return r >= 0 && r < c->n ? c->rec[r] : 0;
    }
    case 0x0341: {                                           /* CM_QUERYRECORDEMPHASIS: after (CMA_FIRST), the emphasis */
        int from = mp1 == 0x10 ? 0 : cnr_index(c, mp1) + 1;
        for (int i = from < 0 ? 0 : from; i < c->n; i++) if (g32(c->rec[i] + RC_ATTR) & (mp2 & 0xFFFF)) return c->rec[i];
        return 0;
    }
    case 0x034a: {                                           /* CM_SETRECORDEMPHASIS: record, (on, attributes) */
        int i = cnr_index(c, mp1);
        uint32_t on = mp2 & 0xFFFF, a = mp2 >> 16;
        if (i < 0) return 0;
        if (on && (a & 1) && !(w->style & 3)) cnr_select(w, c, i);
        else {
            uint32_t was = g32(mp1 + RC_ATTR), now = on ? was | a : was & ~a;
            p32(mp1 + RC_ATTR, now);
            if ((now ^ was) & a) cnr_notify_emph(w, mp1, a);
            if (on && (a & 4)) c->cursor = i;
            repaint(w);
        }
        return 1;
    }
    case 0x0349: {                                           /* CM_SETCNRINFO: CNRINFO, which parts */
        uint32_t ci = mp1, fl = mp2;
        if (fl & 4) c->attr = g32(ci + 20);                  /* CMA_FLWINDOWATTR */
        if (fl & 1) c->title = g32(ci + 16);                 /* CMA_CNRTITLE */
        c->top = 0;
        repaint(w);
        return 1;
    }
    case 0x033d: {                                           /* CM_QUERYCNRINFO: CNRINFO, its size */
        uint32_t ci = mp1, n = mp2 < 100 ? mp2 : 100;
        if (!ok(ci, n)) return 0;
        uint8_t b[100] = { 0 };
        uint32_t v[4] = { 100, c->attr, (uint32_t)c->n, (uint32_t)c->nf };
        memcpy(b, &v[0], 4); memcpy(b + 16, &c->title, 4); memcpy(b + 20, &v[1], 4);
        int32_t ind = cnr_indent();
        memcpy(b + 36, &v[2], 4); memcpy(b + 76, &ind, 4); memcpy(b + 84, &v[3], 4);
        memcpy(C.mem + ci, b, n);
        return n;
    }
    case 0x0330: {                                           /* CM_ALLOCDETAILFIELDINFO: count */
        uint32_t first = 0, prev = 0;
        for (uint32_t k = 0; k < (mp1 ? mp1 : 1); k++) {
            uint32_t f = galloc(32);
            if (!f) return 0;
            p32(f, 32);
            if (prev) p32(prev + FI_NEXT, f); else first = f;
            prev = f;
        }
        return first;
    }
    case 0x0338: {                                           /* CM_INSERTDETAILFIELDINFO: the fields (chained), FIELDINFOINSERT */
        uint32_t order = g32(mp2 + 4), cnt = g32(mp2 + 12), f = mp1;
        int at = order == 0x10 ? 0 : order == 0x40 || order == 0 ? c->nf : 0;
        for (int i = 0; i < c->nf; i++) if (c->fld[i] == order) at = i + 1;
        for (uint32_t k = 0; k < (cnt ? cnt : 1) && f && c->nf < 32; k++) {
            memmove(c->fld + at + 1, c->fld + at, (size_t)(c->nf - at) * sizeof *c->fld);
            c->fld[at++] = f;
            c->nf++;
            f = g32(f + FI_NEXT);
        }
        for (int i = 0; i < c->nf; i++) p32(c->fld[i] + FI_NEXT, i + 1 < c->nf ? c->fld[i + 1] : 0);
        repaint(w);
        return (uint32_t)c->nf;
    }
    case 0x033e: {                                           /* CM_QUERYDETAILFIELDINFO: a field, CMA_FIRST/LAST/NEXT/PREV */
        int i = -1, r = -1;
        for (int k = 0; k < c->nf; k++) if (c->fld[k] == mp1) i = k;
        if (mp2 & 0x10) r = 0; else if (mp2 & 0x20) r = c->nf - 1; else if ((mp2 & 0x100) && i >= 0) r = i + 1; else if ((mp2 & 0x80) && i >= 0) r = i - 1;
        return r >= 0 && r < c->nf ? c->fld[r] : 0;
    }
    case 0x0345: {                                           /* CM_REMOVEDETAILFIELDINFO: array, (count, flags) */
        int cnt = (int)(mp2 & 0xFFFF);
        if (!cnt) c->nf = 0;
        for (int k = 0; k < cnt; k++)
            for (int i = 0; i < c->nf; i++)
                if (c->fld[i] == g32(mp1 + 4u * (uint32_t)k)) { memmove(c->fld + i, c->fld + i + 1, (size_t)(c->nf - i - 1) * sizeof *c->fld); c->nf--; break; }
        repaint(w);
        return (uint32_t)c->nf;
    }
    case 0x034b: cnr_sort(w, c, mp1, mp2); return 1;         /* CM_SORTRECORD: compare function, its storage */
    case 0x034f: case 0x034e: {                              /* CM_EXPANDTREE, CM_COLLAPSETREE: a record (collapse, 0: all of them) */
        int on = msg == 0x034f;
        if (!mp1 && !on) { for (int i = 0; i < c->n; i++) if (cnr_kids(c, i)) cnr_expand(w, c, i, 0); return 1; }
        int i = cnr_index(c, mp1);
        if (i < 0) return 0;
        cnr_expand(w, c, i, on);
        return 1;
    }
    case 0x0348: {                                           /* CM_SEARCHSTRING: SEARCHSTRING, after (CMA_FIRST) */
        const char *s = gstr(g32(mp1 + 4));
        int prefix = g32(mp1 + 8) != 0, cs = g32(mp1 + 12) != 0, from = mp2 == 0x10 ? 0 : cnr_index(c, mp2) + 1;
        for (int i = from < 0 ? 0 : from; i < c->n; i++) {
            const char *t = gstr(g32(c->rec[i] + (cnr_view(c) == 1 ? RC_PSZTEXT : cnr_view(c) == 2 ? RC_PSZNAME : RC_PSZICON)));
            int hit = prefix ? (cs ? !strncmp(t, s, strlen(s)) : !strncasecmp(t, s, strlen(s))) : (cs ? strstr(t, s) != 0 : strcasestr(t, s) != 0);
            if (hit) return c->rec[i];
        }
        return 0;
    }
    case 0x034c:                                             /* CM_OPENEDIT: a CNREDITDATA (its record, field) */
        if (!mp1 || !ok(mp1, 28) || !g32(mp1 + 8)) return 0;    /* (the container's title: not editable here) */
        return (uint32_t)cnr_open_edit(w, c, cnr_index(c, g32(mp1 + 8)), g32(mp1 + 12));
    case 0x034d: if (c->ed) cnr_end_edit(w, c, 1); return 1; /* CM_CLOSEEDIT */
    case 0x033b: case 0x033a: case 0x0332: case 0x0353: repaint(w); return 1;   /* invalidate, arrange, ... */
    case 0x0347: {                                           /* CM_SCROLLWINDOW: CMA_VERTICAL, pixels */
        if (mp1 == 0x400) { c->top += (int32_t)mp2 / cnr_row_h(c); if (c->top < 0) c->top = 0; repaint(w); }
        return 1;
    }
    }
    return 0;
}
