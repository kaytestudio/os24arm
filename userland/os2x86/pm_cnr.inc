/* userland/os2x86/pm_cnr.inc - Presentation Manager's container (WC_CONTAINER,
 * included at the end of pm.inc): records and field descriptions in the
 * program's memory (CM_ALLOCRECORD, CM_ALLOCDETAILFIELDINFO: a sub-
 * allocator of guest memory), the icon, name, text and details views
 * (string, number, date, time and icon columns), the title, selection
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

enum { RC_ATTR = 4, RC_PTL = 8, RC_NEXT = 16, RC_PSZICON = 20, RC_HPTR = 24, RC_HPTRMINI = 28, RC_PSZTEXT = 44, RC_PSZNAME = 48, RC_PSZTREE = 52 };
enum { FI_FLDATA = 4, FI_FLTITLE = 8, FI_TITLE = 12, FI_OFF = 16, FI_NEXT = 24, FI_CX = 28 };
typedef struct {
    uint32_t *rec; int n, cap;
    uint32_t fld[32]; int nf;
    uint32_t attr, title;                     /* flWindowAttr, pszCnrTitle (the program's) */
    int top, cursor;                          /* (first row shown; the cursored record's index) */
    uint32_t vsb;
} cnr_t;
static cnr_t *cnr(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(cnr_t)); ((cnr_t *)w->ctl)->attr = 4; ((cnr_t *)w->ctl)->cursor = -1; } return w->ctl; }
static int cnr_mini(pmwin_t *w) { return (w->style & 0x40) != 0; }   /* CCS_MINIRECORDCORE */
static int cnr_index(cnr_t *c, uint32_t r) { for (int i = 0; i < c->n; i++) if (c->rec[i] == r) return i; return -1; }
static const char *gstr(uint32_t a) { char *s = a ? gs(a) : 0; return s ? s : ""; }
static int cnr_view(cnr_t *c) { return (c->attr & 8) ? 8 : (c->attr & 1) ? 1 : (c->attr & 2) ? 2 : (c->attr & 0x40) ? 2 : 4; }
static int cnr_title_h(pmwin_t *w, cnr_t *c) { (void)w; return (c->attr & 0x200) ? pm_lineh + 6 : 0; }
static int cnr_head_h(cnr_t *c) { return cnr_view(c) == 8 && (c->attr & 0x8000) ? pm_lineh + 6 : 0; }
static int cnr_row_h(cnr_t *c) { return cnr_view(c) == 8 ? (pm_lineh > 20 ? pm_lineh : 20) + 2 : pm_lineh + 4; }
static int cnr_cell_w(void) { return 96; }
static int cnr_cell_h(void) { return 40 + pm_lineh + 4; }
static int cnr_cols(pmwin_t *w) { int k = (w->cx - 18) / cnr_cell_w(); return k < 1 ? 1 : k; }
/* rows shown, rows there are (icon view: rows of cells) */
static int cnr_area_h(pmwin_t *w, cnr_t *c) { return w->cy - cnr_title_h(w, c) - cnr_head_h(c); }
static int cnr_rows_shown(pmwin_t *w, cnr_t *c) { int h = cnr_view(c) == 4 ? cnr_cell_h() : cnr_row_h(c), r = cnr_area_h(w, c) / h; return r < 1 ? 1 : r; }
static int cnr_rows(pmwin_t *w, cnr_t *c) { return cnr_view(c) == 4 ? (c->n + cnr_cols(w) - 1) / cnr_cols(w) : c->n; }

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
    for (int i = 0; i < c->n; i++) {
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
    uint32_t n = galloc(12);
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
    int shown = cnr_rows_shown(w, c), row = cnr_view(c) == 4 ? i / cnr_cols(w) : i;
    if (row < c->top) c->top = row;
    if (row >= c->top + shown) c->top = row - shown + 1;
    repaint(w);
}
static void cnr_enter(pmwin_t *w, uint32_t r, int key)
{
    uint32_t n = galloc(12);
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
        return i < c->n ? i : -1;
    }
    int i = from / cnr_row_h(c) + c->top;
    return i < c->n ? i : -1;
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
            for (int r = 0; r + c->top < c->n; r++) {
                int ry1 = y - hh - r * rh, ry0 = ry1 - rh, i = r + c->top;
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
        for (int i = c->top * cols; i < c->n; i++) {
            int row = i / cols - c->top, col = i % cols, x0 = 4 + col * cwid, y1 = y - row * ch;
            if (y1 <= 0) break;
            uint32_t rec = c->rec[i], a = g32(rec + RC_ATTR);
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
    } else {                                                 /* name, text: rows */
        int rh = cnr_row_h(c), mini = view == 2;
        for (int r = 0; r + c->top < c->n; r++) {
            int ry1 = y - r * rh, ry0 = ry1 - rh, i = r + c->top;
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
static void cnr_sort(pmwin_t *w, cnr_t *c, uint32_t fn, uint32_t storage)
{
    for (int i = 1; i < c->n; i++) {                         /* (insertion: few records, a callback each) */
        uint32_t r = c->rec[i];
        int j = i - 1;
        while (j >= 0) {
            uint32_t args[3] = { c->rec[j], r, storage };
            if ((int16_t)guest_call(fn, 3, args) <= 0) break;
            c->rec[j + 1] = c->rec[j];
            j--;
        }
        c->rec[j + 1] = r;
    }
    for (int i = 0; i < c->n; i++) p32(c->rec[i] + RC_NEXT, i + 1 < c->n ? c->rec[i + 1] : 0);
    repaint(w);
}
static uint32_t container_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    cnr_t *c = cnr(w);
    switch (msg) {
    case WM_DESTROY: free(c->rec); return 0;
    case WM_SIZE: cnr_scrollbar(w, c); repaint(w); return 0;
    case WM_PAINT: w->invalid = 0; cnr_scrollbar(w, c); cnr_paint(w, c); return 0;
    case WM_BUTTON1DOWN: case WM_BUTTON2DOWN: {
        int i = cnr_hit(w, c, (int16_t)(mp1 & 0xFFFF), (int16_t)(mp1 >> 16));
        if (i >= 0) cnr_select(w, c, i);
        if (msg == WM_BUTTON2DOWN) notify(w, 119, i >= 0 ? c->rec[i] : 0);   /* CN_CONTEXTMENU */
        return 1;
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
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, step = cnr_view(c) == 4 ? cnr_cols(w) : 1, i = c->cursor;
        if (vk == 0x18) i += step; else if (vk == 0x16) i -= step;
        else if (vk == 0x17 && cnr_view(c) == 4) i++; else if (vk == 0x15 && cnr_view(c) == 4) i--;
        else if (vk == 0x14) i = 0; else if (vk == 0x13) i = c->n - 1;
        else if (vk == 0x12) i += cnr_rows_shown(w, c) * step; else if (vk == 0x11) i -= cnr_rows_shown(w, c) * step;
        else if ((vk == 0x08 || vk == 0x1e) && c->cursor >= 0) { cnr_enter(w, c->rec[c->cursor], 1); return 1; }
        else return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        if (i < 0) i = 0;
        if (i >= c->n) i = c->n - 1;
        cnr_select(w, c, i);
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
        uint32_t ri = mp2, order = g32(ri + 4), cnt = g32(ri + 20), r = mp1;
        int at = order == 0x10 ? 0 : order == 0x40 || order == 0 ? c->n : cnr_index(c, order) + 1;   /* CMA_FIRST, CMA_END, after a record */
        if (at < 0 || at > c->n) at = c->n;
        for (uint32_t k = 0; k < (cnt ? cnt : 1) && r; k++) {
            if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 32; c->rec = realloc(c->rec, (size_t)c->cap * sizeof *c->rec); }
            memmove(c->rec + at + 1, c->rec + at, (size_t)(c->n - at) * sizeof *c->rec);
            c->rec[at++] = r;
            c->n++;
            r = g32(r + RC_NEXT);
        }
        for (int i = 0; i < c->n; i++) p32(c->rec[i] + RC_NEXT, i + 1 < c->n ? c->rec[i + 1] : 0);
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
        for (int k = 0; k < cnt; k++) {
            int i = cnr_index(c, g32(mp1 + 4u * (uint32_t)k));
            if (i < 0) continue;
            memmove(c->rec + i, c->rec + i + 1, (size_t)(c->n - i - 1) * sizeof *c->rec);
            c->n--;
            if (c->cursor >= c->n) c->cursor = c->n - 1;
        }
        for (int i = 0; i < c->n; i++) p32(c->rec[i] + RC_NEXT, i + 1 < c->n ? c->rec[i + 1] : 0);
        repaint(w);
        return (uint32_t)c->n;
    }
    case 0x0340: {                                           /* CM_QUERYRECORD: a record, (CMA_FIRST/LAST/NEXT/PREV, CMA_ITEMORDER) */
        uint32_t how = mp2 & 0xFFFF;
        int i = cnr_index(c, mp1), r = -1;
        if (how & 0x10) r = 0; else if (how & 0x20) r = c->n - 1;
        else if ((how & 0x100) && i >= 0) r = i + 1; else if ((how & 0x80) && i >= 0) r = i - 1;
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
        memcpy(b + 36, &v[2], 4); memcpy(b + 88, &v[3], 4);
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
    case 0x033b: case 0x033a: case 0x0332: case 0x034c: case 0x034d: case 0x0353: repaint(w); return 1;   /* invalidate, arrange, edit, ... */
    case 0x0347: {                                           /* CM_SCROLLWINDOW: CMA_VERTICAL, pixels */
        if (mp1 == 0x400) { c->top += (int32_t)mp2 / cnr_row_h(c); if (c->top < 0) c->top = 0; repaint(w); }
        return 1;
    }
    }
    return 0;
}
