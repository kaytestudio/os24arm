/* userland/os2x86/pm_cnr.inc - the container control (WC_CONTAINER), included
 * at the end of pm.inc. Records and field infos live in program memory
 * (CM_ALLOCRECORD and CM_ALLOCDETAILFIELDINFO use a sub-allocator). Supports
 * icon, name, text and details views (string, number, date, time and icon
 * columns), the tree view (records kept in tree order), the title, selection
 * (CN_EMPHASIS), opening (CN_ENTER on double-click or Enter), sorting with the
 * program's compare function, and a scroll bar. */

/* small allocations in guest memory (records, notification structs) */
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

/* guest memory for things that only live for one call (notifications,
 * OWNERITEMs): a reused ring */
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
    uint32_t *rec, *par; int n, cap;          /* records in tree order, each followed by its children; par[i] is the parent */
    int *vis, nv;                             /* visible records: in tree view the expanded ones, otherwise the top level */
    uint32_t fld[32]; int nf;
    uint32_t attr, title;                     /* flWindowAttr, pszCnrTitle (program's) */
    int top, cursor;                          /* first visible row; index of the cursored record */
    uint32_t vsb;
    uint32_t ed, ed_data;                     /* direct editing: the entry field over the text, and its CNREDITDATA */
} cnr_t;
static cnr_t *cnr(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(cnr_t)); ((cnr_t *)w->ctl)->attr = CV_ICON; ((cnr_t *)w->ctl)->cursor = -1; } return w->ctl; }
static int cnr_mini(pmwin_t *w) { return (w->style & CCS_MINIRECORDCORE) != 0; }
static int cnr_index(cnr_t *c, uint32_t r) { for (int i = 0; i < c->n; i++) if (c->rec[i] == r) return i; return -1; }
static const char *gstr(uint32_t a) { char *s = a ? gs(a) : 0; return s ? s : ""; }
static int cnr_view(cnr_t *c)
{
    return (c->attr & CV_TREE) ? CV_TREE : (c->attr & CV_DETAIL) ? CV_DETAIL : (c->attr & CV_TEXT) ? CV_TEXT : (c->attr & CV_NAME) ? CV_NAME : CV_ICON;
}
/* tree helpers: is k under i, where i's subtree ends, does i have children */
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
        if (!(c->attr & CV_TREE)) shown = c->par[i] == 0;
        else for (uint32_t p = c->par[i]; p; ) {
            int j = cnr_index(c, p);
            if (j < 0) break;
            if (!(g32(p + RC_ATTR) & CRA_EXPANDED)) { shown = 0; break; }
            p = c->par[j];
        }
        if (shown) c->vis[c->nv++] = i;
    }
}
static int cnr_pos(cnr_t *c, int i) { for (int k = 0; k < c->nv; k++) if (c->vis[k] == i) return k; return -1; }
static void cnr_relink(cnr_t *c)              /* set each preccNextRecord to the next sibling */
{
    for (int i = 0; i < c->n; i++) { int s = cnr_next_sib(c, i); p32(c->rec[i] + RC_NEXT, s >= 0 ? c->rec[s] : 0); }
}
static int cnr_tree_icons(cnr_t *c) { return !(c->attr & (CV_TEXT | CV_NAME)); }   /* tree icons: full for CV_ICON, small for CV_NAME, none for CV_TEXT */
static int cnr_indent(void) { return 18; }
static int cnr_title_h(pmwin_t *w, cnr_t *c) { (void)w; return (c->attr & CA_CONTAINERTITLE) ? pm_lineh + 6 : 0; }
static int cnr_head_h(cnr_t *c) { return cnr_view(c) == CV_DETAIL && (c->attr & CA_DETAILSVIEWTITLES) ? pm_lineh + 6 : 0; }
static int cnr_row_h(cnr_t *c)
{
    if (cnr_view(c) == CV_TREE) { int ih = cnr_tree_icons(c) ? 34 : (c->attr & CV_NAME) ? 18 : 0; return ih > pm_lineh + 4 ? ih : pm_lineh + 4; }
    return cnr_view(c) == CV_DETAIL ? (pm_lineh > 20 ? pm_lineh : 20) + 2 : pm_lineh + 4;
}
static int cnr_cell_w(void) { return 96; }
static int cnr_cell_h(void) { return 40 + pm_lineh + 4; }
static int cnr_cols(pmwin_t *w) { int k = (w->cx - 18) / cnr_cell_w(); return k < 1 ? 1 : k; }
/* rows visible / total rows (icon view counts rows of cells) */
static int cnr_area_h(pmwin_t *w, cnr_t *c) { return w->cy - cnr_title_h(w, c) - cnr_head_h(c); }
static int cnr_rows_shown(pmwin_t *w, cnr_t *c) { int h = cnr_view(c) == CV_ICON ? cnr_cell_h() : cnr_row_h(c), r = cnr_area_h(w, c) / h; return r < 1 ? 1 : r; }
static int cnr_rows(pmwin_t *w, cnr_t *c) { return cnr_view(c) == CV_ICON ? (c->nv + cnr_cols(w) - 1) / cnr_cols(w) : c->nv; }

/* text of a details column for record r */
static void cell_text(uint32_t r, uint32_t fi, char *out, size_t n)
{
    uint32_t fl = g32(fi + FI_FLDATA), at = r + g32(fi + FI_OFF);
    out[0] = 0;
    if (fl & CFA_STRING) snprintf(out, n, "%s", gstr(g32(at)));                     /* a PSZ */
    else if (fl & CFA_ULONG) snprintf(out, n, "%u", g32(at));
    else if (fl & CFA_DATE) snprintf(out, n, "%02u-%02u-%04u", C.mem[at + 1], C.mem[at], g16(at + 2)); /* mm-dd-yyyy */
    else if (fl & CFA_TIME) snprintf(out, n, "%02u:%02u:%02u", C.mem[at], C.mem[at + 1], C.mem[at + 2]);
}
static int col_w(pmwin_t *w, cnr_t *c, int k)
{
    uint32_t fi = c->fld[k], cx = g32(fi + FI_CX);
    (void)w;
    if (cx) return (int)cx;
    int best = (g32(fi + FI_FLTITLE) & 0x100) ? 24 : plain_w(gstr(g32(fi + FI_TITLE)));   /* title width, or 24 for an icon title */
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
    s->x = w->cx - SB_W; s->y = 0; s->cx = SB_W; s->cy = w->cy - cnr_title_h(w, c);
    s->visible = 1;
    if (c->top > rows - shown) c->top = rows - shown;
    scroll_proc(s, SBM_SETSCROLLBAR, (uint32_t)c->top, (uint32_t)(rows - shown) << 16);
    scroll_proc(s, SBM_SETTHUMBSIZE, (uint32_t)shown | (uint32_t)rows << 16, 0);
}
static void cnr_notify_emph(pmwin_t *w, uint32_t r, uint32_t mask)
{
    uint32_t n = gscratch(12);
    if (!n) return;
    p32(n, w->hwnd); p32(n + 4, r); p32(n + 8, mask);
    notify(w, CN_EMPHASIS, n);
}
static void cnr_select(pmwin_t *w, cnr_t *c, int i)
{
    if (i < 0 || i >= c->n) return;
    int single = !(w->style & (CCS_MULTIPLESEL | CCS_EXTENDSEL));
    for (int k = 0; k < c->n; k++) {
        uint32_t a = g32(c->rec[k] + RC_ATTR), was = a;
        if (k == i) a |= CRA_SELECTED | CRA_CURSORED; else { if (single) a &= ~(uint32_t)CRA_SELECTED; a &= ~(uint32_t)CRA_CURSORED; }
        if (a != was) { p32(c->rec[k] + RC_ATTR, a); if ((a ^ was) & 1) cnr_notify_emph(w, c->rec[k], 1); }
    }
    c->cursor = i;
    int p = cnr_pos(c, i), shown = cnr_rows_shown(w, c), row = cnr_view(c) == CV_ICON ? p / cnr_cols(w) : p;
    if (p >= 0 && row < c->top) c->top = row;
    if (p >= 0 && row >= c->top + shown) c->top = row - shown + 1;
    repaint(w);
}
static void cnr_enter(pmwin_t *w, uint32_t r, int key)
{
    uint32_t n = gscratch(12);
    if (!n) return;
    p32(n, w->hwnd); p32(n + 4, (uint32_t)key); p32(n + 8, r);
    notify(w, CN_ENTER, n);
}
/* index of the record at (x, y), or -1 */
static int cnr_hit(pmwin_t *w, cnr_t *c, int x, int y)
{
    int top = w->cy - cnr_title_h(w, c) - cnr_head_h(c), from = top - y;
    if (from < 0) return -1;
    if (cnr_view(c) == CV_ICON) {
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
    int view = cnr_view(c), sbw = win(c->vsb) && win(c->vsb)->visible ? SB_W : 0, right = w->cx - sbw;
    fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_LIGHT));
    uint32_t tc = pp_fgc(w, C_TEXT);
    int y = w->cy;
    if (c->attr & CA_CONTAINERTITLE) {                                   
        int th = cnr_title_h(w, c);
        const char *t = gstr(c->title);
        int tw = plain_w(t), x = (c->attr & CA_TITLECENTER) ? (w->cx - tw) / 2 : (c->attr & CA_TITLERIGHT) ? w->cx - tw - 6 : 6;
        text_mid(w, x, y - th, y, t, tc);
        if (c->attr & CA_TITLESEPARATOR) line(w, 0, y - th, w->cx - 1, y - th, C_DARK);
        y -= th;
    }
    if (view == CV_DETAIL) {                                         /* details view: columns */
        int hh = cnr_head_h(c), rh = cnr_row_h(c), x = 0;
        for (int k = 0; k < c->nf; k++) {
            uint32_t fi = c->fld[k], fl = g32(fi + FI_FLDATA), ft = g32(fi + FI_FLTITLE);
            if (fl & CFA_INVISIBLE) continue;
            int cw = col_w(w, c, k);
            if (hh) {
                const char *t = (ft & 0x100) ? "" : gstr(g32(fi + FI_TITLE));
                int tw = plain_w(t), tx = (ft & 4) ? x + (cw - tw) / 2 : (ft & 2) ? x + cw - tw - 6 : x + 6;
                text_mid(w, tx, y - hh, y, t, tc);
                if (fl & CFA_HORZSEPARATOR) line(w, x, y - hh, x + cw - 1, y - hh, C_DARK);
            }
            for (int r = 0; r + c->top < c->nv; r++) {
                int ry1 = y - hh - r * rh, ry0 = ry1 - rh, i = c->vis[r + c->top];
                if (ry1 <= 0) break;
                uint32_t rec = c->rec[i], a = g32(rec + RC_ATTR);
                uint32_t fg = (a & 1) ? C_LIGHT : tc;
                if (a & 1) fill(w, x, ry0, x + cw, ry1, C_HILITE);
                if (fl & CFA_BITMAPORICON) {                 /* icon (pointer) handle */
                    pmwin_t *p = ptr_get(g32(rec + g32(fi + FI_OFF)));
                    if (p) ptr_draw(w, x + (cw - p->cx / 2) / 2, ry0 + (rh - p->cy / 2) / 2, p, DP_MINI);
                } else {
                    char b[128];
                    cell_text(rec, fi, b, sizeof b);
                    int tw = text_w(b, (int)strlen(b)), tx = (fl & 4) ? x + (cw - tw) / 2 : (fl & 2) ? x + cw - tw - 6 : x + 6;
                    text_at(w, tx, ry0 + (rh - pm_ascent - pm_descent) / 2 + pm_descent, b, (int)strlen(b), fg);
                }
                if ((a & 4) && pm_focus == w->hwnd && k == 0) for (int px = 1; px < right - 1; px += 2) { pel(w, px, ry0, C_TEXT); pel(w, px, ry1 - 1, C_TEXT); }
            }
            if (fl & CFA_SEPARATOR) line(w, x + cw - 1, 0, x + cw - 1, y - 1, C_DARK);
            x += cw;
        }
    } else if (view == CV_ICON) {                                  /* icon view: a grid */
        int cols = cnr_cols(w), ch = cnr_cell_h(), cwid = cnr_cell_w(), mini = (c->attr & CV_MINI) != 0;
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
    } else if (view == CV_TREE) {                               /* tree view: indented, +/- buttons, connecting lines */
        int rh = cnr_row_h(c), ind = cnr_indent(), lines = (c->attr & CA_TREELINE) != 0;
        for (int r = 0; r + c->top < c->nv; r++) {
            int ry1 = y - r * rh, ry0 = ry1 - rh, i = c->vis[r + c->top], d = cnr_depth(c, i), my = (ry0 + ry1) / 2;
            if (ry1 <= 0) break;
            uint32_t rec = c->rec[i], a = g32(rec + RC_ATTR);
            int bx = 4 + d * ind;                            /* button's left edge; its middle is bx + 4 */
            if (lines) {
                #define DOTS_V(x, y0, y1) for (int q = (y0); q < (y1); q += 2) pel(w, x, q, C_DARK)
                #define DOTS_H(x0, x1, yy) for (int q = (x0); q < (x1); q += 2) pel(w, q, yy, C_DARK)
                if (d > 0) {
                    int px = bx - ind + 4;
                    DOTS_H(px, bx + (cnr_kids(c, i) ? 0 : 8), my);
                    DOTS_V(px, cnr_next_sib(c, i) >= 0 ? ry0 : my, ry1);
                    /* ancestors' lines carry on past this row to their next sibling */
                    int k = cnr_index(c, c->par[i]);
                    for (int dd = d - 1; dd > 0 && k >= 0; dd--) {
                        if (cnr_next_sib(c, k) >= 0) DOTS_V(4 + (dd - 1) * ind + 4, ry0, ry1);
                        k = cnr_index(c, c->par[k]);
                    }
                }
                if (cnr_kids(c, i) && (a & 0x80)) DOTS_V(bx + 4, ry0, my - 4);   /* line down to the first child */
                #undef DOTS_V
                #undef DOTS_H
            }
            if (cnr_kids(c, i)) {                            /* + when collapsed, - when expanded */
                fill(w, bx, my - 4, bx + 9, my + 5, C_LIGHT);
                line(w, bx, my - 4, bx + 8, my - 4, C_DARK); line(w, bx, my + 4, bx + 8, my + 4, C_DARK);
                line(w, bx, my - 4, bx, my + 4, C_DARK); line(w, bx + 8, my - 4, bx + 8, my + 4, C_DARK);
                line(w, bx + 2, my, bx + 6, my, tc);
                if (!(a & 0x80)) line(w, bx + 4, my - 2, bx + 4, my + 2, tc);
            }
            int x = bx + 14;
            const char *t;
            if (c->attr & CV_TEXT) t = gstr(g32(rec + RC_PSZTEXT));
            else if (c->attr & CV_NAME) { pmwin_t *p = ptr_get(g32(rec + RC_HPTR)); if (p) ptr_draw(w, x, ry0 + (rh - 16) / 2, p, 4); x += 20; t = gstr(g32(rec + RC_PSZNAME)); }
            else { pmwin_t *p = ptr_get(g32(rec + RC_HPTR)); if (p) ptr_draw(w, x, ry0 + (rh - 32) / 2, p, 0); x += 36; t = gstr(g32(rec + RC_PSZICON)); }
            int tw = plain_w(t);
            if (a & 1) fill(w, x - 2, ry0 + 1, x + tw + 2, ry1 - 1, C_HILITE);
            text_mid(w, x, ry0, ry1, t, (a & 1) ? C_LIGHT : tc);
            if ((a & 4) && pm_focus == w->hwnd) for (int px = x - 2; px < x + tw + 2; px += 2) { pel(w, px, ry0 + 1, tc); pel(w, px, ry1 - 2, tc); }
        }
    } else {                                                 /* name and text views: rows */
        int rh = cnr_row_h(c), mini = view == CV_NAME;
        for (int r = 0; r + c->top < c->nv; r++) {
            int ry1 = y - r * rh, ry0 = ry1 - rh, i = c->vis[r + c->top];
            if (ry1 <= 0) break;
            uint32_t rec = c->rec[i], a = g32(rec + RC_ATTR);
            int x = 4;
            if (mini) { pmwin_t *p = ptr_get(g32(rec + RC_HPTR)); if (p) ptr_draw(w, x, ry0 + (rh - 16) / 2, p, 4); x += 20; }
            const char *t = gstr(g32(rec + (view == CV_TEXT ? RC_PSZTEXT : cnr_mini(w) ? RC_PSZICON : RC_PSZNAME)));
            int tw = plain_w(t);
            if (a & 1) fill(w, x - 2, ry0 + 1, x + tw + 2, ry1 - 1, C_HILITE);
            text_mid(w, x, ry0, ry1, t, (a & 1) ? C_LIGHT : tc);
        }
    }
    (void)right;
    present(w);
}
/* put parent's children (0 = top level) into out, sorted, each followed by its own subtree */
static int cnr_sort_into(cnr_t *c, uint32_t parent, uint32_t *out, uint32_t *opar, int k, uint32_t fn, uint32_t storage)
{
    int m = 0;
    uint32_t *g = malloc((size_t)(c->n + 1) * sizeof *g);
    for (int i = 0; i < c->n; i++) if (c->par[i] == parent) g[m++] = c->rec[i];
    for (int i = 1; i < m; i++) {                            /* insertion sort: few records, and each compare is a callback */
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
/* expand (on) or collapse record i, with CN_EXPANDTREE / CN_COLLAPSETREE */
static void cnr_expand(pmwin_t *w, cnr_t *c, int i, int on)
{
    uint32_t r = c->rec[i], a = g32(r + RC_ATTR);
    if (!!(a & 0x80) == on) return;
    p32(r + RC_ATTR, on ? (a | 0x80) & ~0x100u : (a | 0x100) & ~0x80u);
    cnr_revis(c);
    if (!on && c->cursor >= 0 && cnr_pos(c, c->cursor) < 0) cnr_select(w, c, i);   /* the cursor was inside what's now hidden */
    repaint(w);
    notify(w, on ? CN_EXPANDTREE : CN_COLLAPSETREE, r);
}
/* ── direct editing ──
 * An entry field over a record's text (Alt+click or CM_OPENEDIT). Enter or a
 * click elsewhere keeps the edit: CN_REALLOCPSZ lets the program make room,
 * the text is copied into its string, then CN_ENDEDIT. Esc drops it. */
static int cnr_top_y(pmwin_t *w, cnr_t *c) { return w->cy - cnr_title_h(w, c) - cnr_head_h(c); }
/* where record i's text is (field fi in details view), and the address of its PSZ */
static int cnr_text_at(pmwin_t *w, cnr_t *c, int i, uint32_t fi, int *x0, int *y0, int *x1, int *y1, uint32_t *ppsz)
{
    int view = cnr_view(c), p = cnr_pos(c, i), rh = cnr_row_h(c), y = cnr_top_y(w, c);
    uint32_t rec = c->rec[i];
    if (p < 0) return 0;
    if (view == CV_ICON) {
        int cols = cnr_cols(w), ch = cnr_cell_h(), cw = cnr_cell_w(), row = p / cols - c->top, col = p % cols;
        int isz = (c->attr & CV_MINI) ? 16 : 32, cx0 = 4 + col * cw, ty = y - row * ch - 8 - isz - pm_ascent;
        *x0 = cx0; *x1 = cx0 + cw; *y0 = ty - pm_descent - 3; *y1 = ty + pm_ascent + 3;
        *ppsz = rec + RC_PSZICON;
        return 1;
    }
    int ry1 = y - (p - c->top) * rh, ry0 = ry1 - rh;
    *y0 = ry0; *y1 = ry1;
    if (view == CV_DETAIL) {
        int x = 0;
        for (int k = 0; k < c->nf; k++) {
            uint32_t f = c->fld[k], fl = g32(f + FI_FLDATA);
            if (fl & 0x40) continue;
            int cw = col_w(w, c, k);
            if (f == fi) {
                if (!(fl & CFA_STRING)) return 0;            /* strings only */
                *x0 = x; *x1 = x + cw; *ppsz = rec + g32(f + FI_OFF);
                return 1;
            }
            x += cw;
        }
        return 0;
    }
    int x = 4;
    if (view == CV_TREE) { x = 4 + cnr_depth(c, i) * cnr_indent() + 14 + (c->attr & CV_TEXT ? 0 : c->attr & CV_NAME ? 20 : 36); *ppsz = rec + (c->attr & CV_TEXT ? RC_PSZTEXT : c->attr & CV_NAME ? RC_PSZNAME : RC_PSZICON); }
    else if (view == CV_NAME) { x = 24; *ppsz = rec + (cnr_mini(w) ? RC_PSZICON : RC_PSZNAME); }
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
        p32(d + 20, (uint32_t)strlen(t) + 1);                /* cbText: size needed */
        uint32_t pp = g32(d + 16);
        uint32_t r = o ? pm_send(o, WM_CONTROL, (w->id & 0xFFFF) | CN_REALLOCPSZ << 16, d) : 0;
        uint32_t s = g32(pp);
        if (r && s && ok(s, (uint32_t)strlen(t) + 1)) memcpy(C.mem + s, t, strlen(t) + 1);
    }
    if (d) notify(w, CN_ENDEDIT, d);
    repaint(w);
}
static int cnr_open_edit(pmwin_t *w, cnr_t *c, int i, uint32_t fi)
{
    int x0, y0, x1, y1;
    uint32_t pp;
    if (c->ed) cnr_end_edit(w, c, 1);
    if (i < 0 || i >= c->n || (g32(c->rec[i] + RC_ATTR) & CRA_RECORDREADONLY)) return 0;
    if (cnr_view(c) == CV_DETAIL && !fi) for (int k = 0; k < c->nf && !fi; k++) if (g32(c->fld[k] + FI_FLDATA) & 0x800) fi = c->fld[k];
    if (!cnr_text_at(w, c, i, fi, &x0, &y0, &x1, &y1, &pp)) return 0;
    int sbw = win(c->vsb) && win(c->vsb)->visible ? SB_W : 0;
    if (x1 > w->cx - sbw) x1 = w->cx - sbw;
    if (x0 < 0) x0 = 0;
    uint32_t d = galloc(28);                                 /* CNREDITDATA: cb, hwndCnr, pRecord, pFieldInfo, ppszText, cbText, id */
    if (!d) return 0;
    p32(d, 28); p32(d + 4, w->hwnd); p32(d + 8, c->rec[i]); p32(d + 12, cnr_view(c) == CV_DETAIL ? fi : 0);
    p32(d + 16, pp); p32(d + 20, (uint32_t)strlen(gstr(g32(pp))) + 1); p32(d + 24, w->id);
    pmwin_t *e = win_new(w, "#6", WS_VISIBLE | ES_MARGIN, w->id);
    if (!e) return 0;
    e->owner = w;
    e->x = x0; e->y = y0; e->cx = x1 - x0; e->cy = y1 - y0;
    e->visible = 1;
    snprintf(e->text, sizeof e->text, "%s", gstr(g32(pp)));
    entry_proc(e, EM_SETTEXTLIMIT, 255, 0);
    entry_proc(e, EM_SETSEL, (uint32_t)strlen(e->text) << 16, 0);   /* select all of it */
    c->ed = e->hwnd; c->ed_data = d;
    cnr_select(w, c, i);
    set_focus(e->hwnd);
    notify(w, CN_BEGINEDIT, d);
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
        if (c->ed) cnr_end_edit(w, c, 1);                    /* clicking elsewhere keeps the edit */
        set_focus(w->hwnd);
        if (i >= 0 && msg == WM_BUTTON1DOWN && ((mp2 >> 16) & KC_ALT)) {   /* Alt+click edits the text */
            uint32_t fi = 0;
            if (cnr_view(c) == CV_DETAIL) {                           /* which column was clicked */
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
        if (i >= 0 && msg == WM_BUTTON1DOWN && cnr_view(c) == CV_TREE && cnr_kids(c, i)) {   /* the +/- button */
            int bx = 4 + cnr_depth(c, i) * cnr_indent();
            if (x >= bx - 2 && x <= bx + 10) { cnr_expand(w, c, i, !(g32(c->rec[i] + RC_ATTR) & 0x80)); return 1; }
        }
        if (i >= 0) cnr_select(w, c, i);
        return 1;
    }
    case WM_BUTTON2UP:                                       /* button 2 released without a drag: context menu (CN_CONTEXTMENU) */
        if (!pm_b2_began) { int i = cnr_hit(w, c, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16)); notify(w, CN_CONTEXTMENU, i >= 0 ? c->rec[i] : 0); }
        return 1;
    case WM_BEGINDRAG: {                                     /* CN_INITDRAG (CNRDRAGINIT: hwndCnr, pRecord, x, y, cx, cy) */
        int x = (int16_t)(mp2 & 0xFFFF), y = (int16_t)(mp2 >> 16), i = cnr_hit(w, c, x, y);
        uint32_t n = gscratch(24);
        if (!n) return 0;
        p32(n, w->hwnd); p32(n + 4, i >= 0 ? c->rec[i] : 0); p32(n + 8, (uint32_t)x); p32(n + 12, (uint32_t)y);
        notify(w, CN_INITDRAG, n);
        return 1;
    }
    case DM_DRAGOVER: case DM_DRAGLEAVE: case DM_DROP: {     /* CN_DRAGOVER, CN_DRAGLEAVE, CN_DROP (CNRDRAGINFO) */
        uint32_t d = mp1, n = gscratch(8);
        if (!n) return 0;
        int dx = msg == DM_DRAGOVER ? (int16_t)(mp2 & 0xFFFF) : (int16_t)g16(d + 12), dy = msg == DM_DRAGOVER ? (int16_t)(mp2 >> 16) : (int16_t)g16(d + 14), ax, ay;
        win_abs(w, &ax, &ay);
        pmwin_t *f = frame_of(w);
        int i = msg == DM_DRAGLEAVE ? -1 : cnr_hit(w, c, dx - ax - (f ? f->x : 0), dy - ay - (f ? f->y : 0));
        p32(n, d); p32(n + 4, i >= 0 ? c->rec[i] : 0);
        pmwin_t *o = w->owner ? w->owner : w->parent;
        return o ? pm_send(o, WM_CONTROL, (w->id & 0xFFFF) | (uint32_t)(msg == DM_DRAGOVER ? CN_DRAGOVER : msg == DM_DRAGLEAVE ? CN_DRAGLEAVE : CN_DROP) << 16, n) : 0;
    }
    case WM_BUTTON1DBLCLK: {
        int i = cnr_hit(w, c, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16));
        if (i >= 0) { cnr_select(w, c, i); cnr_enter(w, c->rec[i], 0); }
        return 1;
    }
    case WM_VSCROLL: {                                       /* from its scroll bar */
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
        if (c->ed) {                                         /* the edit's entry field passes Enter and Esc up to us */
            if (vk == VK_NEWLINE || vk == VK_ENTER) { cnr_end_edit(w, c, 1); return 1; }
            if (vk == VK_ESC) { cnr_end_edit(w, c, 0); return 1; }
            return 1;
        }
        int step = cnr_view(c) == CV_ICON ? cnr_cols(w) : 1, i = c->cursor >= 0 ? cnr_pos(c, c->cursor) : -1;
        if (cnr_view(c) == CV_TREE && c->cursor >= 0) {          /* tree: Right or + opens (then moves into it); Left or - closes (then moves to the parent) */
            int k = c->cursor, open = (g32(c->rec[k] + RC_ATTR) & 0x80) != 0;
            if (vk == VK_RIGHT || ch == '+') { if (cnr_kids(c, k) && !open) cnr_expand(w, c, k, 1); else if (vk == VK_RIGHT && cnr_kids(c, k)) cnr_select(w, c, k + 1); return 1; }
            if (vk == VK_LEFT || ch == '-') { if (cnr_kids(c, k) && open) cnr_expand(w, c, k, 0); else if (vk == VK_LEFT && c->par[k]) cnr_select(w, c, cnr_index(c, c->par[k])); return 1; }
        }
        if (vk == VK_DOWN) i += step; else if (vk == VK_UP) i -= step;
        else if (vk == VK_RIGHT && cnr_view(c) == CV_ICON) i++; else if (vk == VK_LEFT && cnr_view(c) == CV_ICON) i--;
        else if (vk == VK_HOME) i = 0; else if (vk == VK_END) i = c->nv - 1;
        else if (vk == VK_PAGEDOWN) i += cnr_rows_shown(w, c) * step; else if (vk == VK_PAGEUP) i -= cnr_rows_shown(w, c) * step;
        else if ((vk == VK_NEWLINE || vk == VK_ENTER) && c->cursor >= 0) { cnr_enter(w, c->rec[c->cursor], 1); return 1; }
        else return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        if (i < 0) i = 0;
        if (i >= c->nv) i = c->nv - 1;
        if (i >= 0) cnr_select(w, c, c->vis[i]);
        return 1;
    }
    case CM_ALLOCRECORD: {                                   /* cbRecordData (extra bytes), count */
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
    case CM_FREERECORD: return 1;                            /* the memory is ours, so keep it */
    case CM_INSERTRECORD: {                                  /* chained records, RECORDINSERT */
        uint32_t ri = mp2, order = g32(ri + 4), parent = g32(ri + 8), cnt = g32(ri + 20), r = mp1;
        int pi = parent ? cnr_index(c, parent) : -1;
        if (pi < 0) parent = 0;
        int at;                                              /* CMA_FIRST: first of the parent's children; CMA_END: last; otherwise after a record and its subtree */
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
    case CM_INSERTRECORDARRAY: {                             /* record array, count, RECORDINSERT */
        uint32_t arr = mp1, cnt = g32(mp2 + 20);
        for (uint32_t k = 0; k < cnt; k++) p32(g32(arr + 4 * k) + RC_NEXT, k + 1 < cnt ? g32(arr + 4 * (k + 1)) : 0);
        return container_proc(w, CM_INSERTRECORD, g32(arr), mp2);
    }
    case CM_REMOVERECORD: {                                  /* array, (count, flags); returns how many are left */
        int cnt = (int)(mp2 & 0xFFFF);
        if (!cnt) { c->n = 0; c->cursor = -1; c->top = 0; }
        for (int k = 0; k < cnt; k++) {                      /* removing a record removes its children too */
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
    case CM_QUERYRECORD: {                                   /* record, (CMA_FIRST/LAST/NEXT/PREV, CMA_ITEMORDER) */
        uint32_t how = mp2 & 0xFFFF;
        int i = cnr_index(c, mp1), r = -1;
        if (how == CMA_FIRST) r = c->n ? 0 : -1;                             /* first at the top level */
        else if (how == CMA_LAST) { for (int k = 0; k < c->n; k++) if (!c->par[k]) r = k; }
        else if (i < 0) return 0;
        else if (how == CMA_NEXT) r = cnr_next_sib(c, i);                    /* CMA_PREV too: siblings */
        else if (how == 0x80) { for (int k = 0; k < i; k++) if (c->par[k] == c->par[i]) r = k; }
        else if (how == CMA_PARENT) r = c->par[i] ? cnr_index(c, c->par[i]) : -1;
        else if (how == CMA_FIRSTCHILD) r = cnr_kids(c, i) ? i + 1 : -1;
        else if (how == CMA_LASTCHILD) { for (int k = i + 1; k < c->n && cnr_under(c, k, c->rec[i]); k++) if (c->par[k] == c->rec[i]) r = k; }
        return r >= 0 && r < c->n ? c->rec[r] : 0;
    }
    case CM_QUERYRECORDEMPHASIS: {                           /* start after (or CMA_FIRST), emphasis wanted */
        int from = mp1 == 0x10 ? 0 : cnr_index(c, mp1) + 1;
        for (int i = from < 0 ? 0 : from; i < c->n; i++) if (g32(c->rec[i] + RC_ATTR) & (mp2 & 0xFFFF)) return c->rec[i];
        return 0;
    }
    case CM_SETRECORDEMPHASIS: {                             /* record, (on, attributes) */
        int i = cnr_index(c, mp1);
        uint32_t on = mp2 & 0xFFFF, a = mp2 >> 16;
        if (i < 0) return 0;
        if (on && (a & 1) && !(w->style & (CCS_MULTIPLESEL | CCS_EXTENDSEL))) cnr_select(w, c, i);
        else {
            uint32_t was = g32(mp1 + RC_ATTR), now = on ? was | a : was & ~a;
            p32(mp1 + RC_ATTR, now);
            if ((now ^ was) & a) cnr_notify_emph(w, mp1, a);
            if (on && (a & 4)) c->cursor = i;
            repaint(w);
        }
        return 1;
    }
    case CM_SETCNRINFO: {                                    /* CNRINFO, which fields to set */
        uint32_t ci = mp1, fl = mp2;
        if (fl & CMA_FLWINDOWATTR) c->attr = g32(ci + 20);
        if (fl & CMA_CNRTITLE) c->title = g32(ci + 16);
        c->top = 0;
        repaint(w);
        return 1;
    }
    case CM_QUERYCNRINFO: {                                  /* CNRINFO, its size */
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
    case CM_ALLOCDETAILFIELDINFO: {                          /* count */
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
    case CM_INSERTDETAILFIELDINFO: {                         /* chained fields, FIELDINFOINSERT */
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
    case CM_QUERYDETAILFIELDINFO: {                          /* field, CMA_FIRST/LAST/NEXT/PREV */
        int i = -1, r = -1;
        for (int k = 0; k < c->nf; k++) if (c->fld[k] == mp1) i = k;
        if (mp2 & 0x10) r = 0; else if (mp2 & 0x20) r = c->nf - 1; else if ((mp2 & 0x100) && i >= 0) r = i + 1; else if ((mp2 & 0x80) && i >= 0) r = i - 1;
        return r >= 0 && r < c->nf ? c->fld[r] : 0;
    }
    case CM_REMOVEDETAILFIELDINFO: {                         /* array, (count, flags) */
        int cnt = (int)(mp2 & 0xFFFF);
        if (!cnt) c->nf = 0;
        for (int k = 0; k < cnt; k++)
            for (int i = 0; i < c->nf; i++)
                if (c->fld[i] == g32(mp1 + 4u * (uint32_t)k)) { memmove(c->fld + i, c->fld + i + 1, (size_t)(c->nf - i - 1) * sizeof *c->fld); c->nf--; break; }
        repaint(w);
        return (uint32_t)c->nf;
    }
    case CM_SORTRECORD: cnr_sort(w, c, mp1, mp2); return 1;  /* compare function, its user data */
    case CM_EXPANDTREE: case CM_COLLAPSETREE: {              /* record to expand/collapse; collapse with 0 does all */
        int on = msg == CM_EXPANDTREE;
        if (!mp1 && !on) { for (int i = 0; i < c->n; i++) if (cnr_kids(c, i)) cnr_expand(w, c, i, 0); return 1; }
        int i = cnr_index(c, mp1);
        if (i < 0) return 0;
        cnr_expand(w, c, i, on);
        return 1;
    }
    case CM_SEARCHSTRING: {                                  /* SEARCHSTRING, start after (or CMA_FIRST) */
        const char *s = gstr(g32(mp1 + 4));
        int prefix = g32(mp1 + 8) != 0, cs = g32(mp1 + 12) != 0, from = mp2 == 0x10 ? 0 : cnr_index(c, mp2) + 1;
        for (int i = from < 0 ? 0 : from; i < c->n; i++) {
            const char *t = gstr(g32(c->rec[i] + (cnr_view(c) == CV_TEXT ? RC_PSZTEXT : cnr_view(c) == CV_NAME ? RC_PSZNAME : RC_PSZICON)));
            int hit = prefix ? (cs ? !strncmp(t, s, strlen(s)) : !strncasecmp(t, s, strlen(s))) : (cs ? strstr(t, s) != 0 : strcasestr(t, s) != 0);
            if (hit) return c->rec[i];
        }
        return 0;
    }
    case CM_OPENEDIT:                                        /* CNREDITDATA (record, field) */
        if (!mp1 || !ok(mp1, 28) || !g32(mp1 + 8)) return 0;    /* editing the container title isn't supported */
        return (uint32_t)cnr_open_edit(w, c, cnr_index(c, g32(mp1 + 8)), g32(mp1 + 12));
    case CM_CLOSEEDIT: if (c->ed) cnr_end_edit(w, c, 1); return 1;
    case CM_INVALIDATERECORD: case CM_INVALIDATEDETAILFIELDINFO: case CM_ARRANGE: case CM_SETTEXTVISIBILITY: repaint(w); return 1;
    case CM_SCROLLWINDOW: {                                  /* CMA_VERTICAL, pixels */
        if (mp1 == 0x400) { c->top += (int32_t)mp2 / cnr_row_h(c); if (c->top < 0) c->top = 0; repaint(w); }
        return 1;
    }
    }
    return 0;
}
