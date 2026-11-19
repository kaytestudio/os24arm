/* userland/os2x86/pm_ctl.inc - Presentation Manager's controls, menus,
 * dialogs and string tables (included at the end of pm.inc).
 *
 * The controls are windows of PM's own classes, done here: WC_BUTTON (#3:
 * push buttons, check boxes, radio buttons), WC_STATIC (#5: text, group
 * boxes, rectangles), WC_ENTRYFIELD (#6), WC_LISTBOX (#7), each painting
 * itself and telling its owner (WM_COMMAND, WM_CONTROL). Menus (WC_MENU,
 * #4) come from a menu template (FCF_MENU, WinLoadMenu): a bar at the top
 * of the frame, a drop-down drawn over the client. Dialogs come from a
 * dialog template (WinDlgBox, WinLoadDlg, WinCreateDlg): a window of the
 * desktop of their own, its controls in dialog units (a quarter of a
 * character across, an eighth up). */

#define MENU_H  22
#define SS_TYPES 0x003f                 /* (a static's type, its style's low bits: SS_AUTOSIZE not one) */
#define SB_W    16                      /* (a scroll bar's width: the frame's, an MLE's, a container's) */

static int du_x(int v) { return v * 7 / 4; }            /* dialog units: pixels */
static int du_y(int v) { return v * 2; }

/* text without its mnemonic marks ("~File": File) */
static void plain(const char *s, char *out, size_t n)
{
    size_t k = 0;
    for (; *s && k + 1 < n; s++) if (*s != '~') out[k++] = *s;
    out[k] = 0;
}
static void text_mid(pmwin_t *w, int x, int y0, int y1, const char *s, uint32_t rgb)   /* centred up and down */
{
    char p[256];
    plain(s, p, sizeof p);
    text_at(w, x, y0 + (y1 - y0 - (pm_ascent + pm_descent)) / 2 + pm_descent, p, (int)strlen(p), rgb);
}
static int plain_w(const char *s) { char p[256]; plain(s, p, sizeof p); return text_w(p, (int)strlen(p)); }
static void notify(pmwin_t *w, uint32_t code, uint32_t mp2)   /* WM_CONTROL to the owner */
{
    pmwin_t *o = w->owner ? w->owner : w->parent;
    if (o) pm_send(o, WM_CONTROL, (w->id & 0xFFFF) | code << 16, mp2 ? mp2 : w->hwnd);
}
/* owner drawing: WM_MEASUREITEM, WM_DRAWITEM to the owner, an OWNERITEM
 * (hwnd, hps, fsState, fsAttribute, fsStateOld, fsAttributeOld, rclItem,
 * idItem, hItem) in the program's memory and a PS of w for it. 1: the owner
 * drew it; out: the four states after (what's left for us to show) */
static uint32_t gscratch(uint32_t n);
static uint32_t ps_new(pmwin_t *w, int painting);
static pmps_t *ps_get(uint32_t h);
static uint32_t getpel(pmwin_t *w, int x, int y);
static int owner_draw(pmwin_t *w, uint32_t mp1, int x0, int y0, int x1, int y1, uint32_t id, uint32_t h,
                      uint32_t st, uint32_t stold, uint32_t at, uint32_t atold, uint32_t *out)
{
    pmwin_t *o = w->owner ? w->owner : w->parent;
    uint32_t oi = gscratch(48), ps = ps_new(w, 2);
    if (!o || !oi || !ps) { if (ps_get(ps)) ps_get(ps)->used = 0; return 0; }
    p32(oi, w->hwnd); p32(oi + 4, ps); p32(oi + 8, st); p32(oi + 12, at); p32(oi + 16, stold); p32(oi + 20, atold);
    p32(oi + 24, (uint32_t)x0); p32(oi + 28, (uint32_t)y0); p32(oi + 32, (uint32_t)x1); p32(oi + 36, (uint32_t)y1);
    p32(oi + 40, id); p32(oi + 44, h);
    uint32_t r = pm_send(o, WM_DRAWITEM, mp1, oi);
    if (out) for (int k = 0; k < 4; k++) out[k] = g32(oi + 8 + 4 * (uint32_t)k);   /* (fsState, fsAttribute, fsStateOld, fsAttributeOld, as the owner left them) */
    if (ps_get(ps)) ps_get(ps)->used = 0;
    return r != 0;
}
static void invert_rect(pmwin_t *w, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) pel(w, x, y, ~getpel(w, x, y) & 0xFFFFFF);
}
static void set_focus(uint32_t hwnd)
{
    if (pm_focus == hwnd) return;
    pmwin_t *o = win(pm_focus), *n = win(hwnd);
    pm_focus = hwnd;
    if (o && !o->top) { o->invalid = 1; }
    if (n && !n->top) { n->invalid = 1; }
    pthread_cond_broadcast(&gcv);
}
static void repaint(pmwin_t *w) { w->invalid = 1; pthread_cond_broadcast(&gcv); }
static uint32_t pp_fgc(pmwin_t *w, uint32_t d) { for (; w; w = w->parent) if (w->has_fg) return w->pp_fg; return d; }
static uint32_t pp_bgc(pmwin_t *w, uint32_t d) { for (; w; w = w->parent) if (w->has_bg) return w->pp_bg; return d; }

/* ── WC_BUTTON ───────────────────────────────────────────────────────── */
typedef struct { int check, hilite, def; } btn_t;
static btn_t *btn(pmwin_t *w) { if (!w->ctl) w->ctl = calloc(1, sizeof(btn_t)); return w->ctl; }
static int btn_type(pmwin_t *w) { return (int)(w->style & BS_PRIMARYSTYLES); }
static int is_radio(pmwin_t *w) { return w->iproc && (btn_type(w) == BS_RADIOBUTTON || btn_type(w) == BS_AUTORADIOBUTTON) && !strcmp(w->cls, "#3"); }
/* the radio buttons of w's group: its siblings from the last WS_GROUP to the next */
static int radio_group(pmwin_t *w, pmwin_t **out, int max)
{
    pmwin_t *sib[MAXWIN];
    int n = 0, at = -1, k = 0;
    for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].parent == w->parent) { if (&pmwins[i] == w) at = n; sib[n++] = &pmwins[i]; }
    int s = at;
    while (s > 0 && !(sib[s]->style & WS_GROUP_)) s--;
    for (int i = s; i < n && k < max; i++) {
        if (i > s && (sib[i]->style & WS_GROUP_)) break;
        if (is_radio(sib[i])) out[k++] = sib[i];
    }
    return k;
}
static void btn_click(pmwin_t *w)
{
    btn_t *b = btn(w);
    int t = btn_type(w);
    if (w->style & WS_DISABLED_) return;
    if (t == BS_PUSHBUTTON || t == BS_USERBUTTON || t == BS_NOTEBOOKBUTTON) {   /* a command */
        pmwin_t *o = w->owner ? w->owner : w->parent;
        if (o) post(o, o->hwnd, WM_COMMAND, w->id & 0xFFFF, CMDSRC_PUSHBUTTON | 1u << 16);   /* (by the pointer) */
        return;
    }
    if (t == BS_AUTOCHECKBOX) b->check = !b->check;
    else if (t == BS_AUTO3STATE) b->check = (b->check + 1) % 3;
    else if (t == BS_AUTORADIOBUTTON) {
        pmwin_t *g[64];
        int n = radio_group(w, g, 64);
        for (int i = 0; i < n; i++) if (g[i] != w && btn(g[i])->check) { btn(g[i])->check = 0; repaint(g[i]); }
        b->check = 1;
    }
    repaint(w);
    notify(w, BN_CLICKED, 0);
}
static uint32_t button_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    btn_t *b = btn(w);
    int t = btn_type(w), dis = (w->style & WS_DISABLED_) != 0;
    uint32_t tc = dis ? C_DARK : pp_fgc(w, C_TEXT);
    switch (msg) {
    case WM_PAINT:
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_FACE));
        if (t == BS_PUSHBUTTON || t == BS_USERBUTTON || t == BS_NOTEBOOKBUTTON) {
            button_frame(w, 0, 0, w->cx, w->cy, b->hilite, b->def || (w->style & BS_DEFAULT));
            int tw = plain_w(w->text);
            text_mid(w, (w->cx - tw) / 2 + b->hilite, 0, w->cy - b->hilite, w->text, tc);
            if (pm_focus == w->hwnd) for (int x = 4; x < w->cx - 4; x += 2) pel(w, x, 4, C_TEXT);
        } else {
            int s = 12, y0 = (w->cy - s) / 2;
            fill(w, 1, y0, 1 + s, y0 + s, dis ? C_FACE : C_LIGHT);
            bevel(w, 1, y0, 1 + s, y0 + s, 0);
            if (b->check == 1 && (t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON)) fill(w, 4, y0 + 3, 1 + s - 3, y0 + s - 3, tc);
            else if (b->check == 1) { line(w, 3, y0 + 3, 1 + s - 3, y0 + s - 3, tc); line(w, 3, y0 + s - 3, 1 + s - 3, y0 + 3, tc);
                                      line(w, 4, y0 + 3, 1 + s - 2, y0 + s - 3, tc); line(w, 4, y0 + s - 3, 1 + s - 2, y0 + 3, tc); }
            else if (b->check == 2) fill(w, 4, y0 + 4, 1 + s - 3, y0 + s - 3, C_DARK);
            text_mid(w, s + 6, 0, w->cy, w->text, tc);
            if (pm_focus == w->hwnd) for (int x = s + 5; x < s + 7 + plain_w(w->text); x += 2) pel(w, x, 1, C_TEXT);
        }
        present(w);
        return 0;
    case WM_BUTTON1DOWN:
        if (t == BS_PUSHBUTTON || t == BS_USERBUTTON || t == BS_NOTEBOOKBUTTON) { b->hilite = 1; repaint(w); }
        return 1;
    case WM_BUTTON1UP:
        if (t == BS_PUSHBUTTON || t == BS_USERBUTTON || t == BS_NOTEBOOKBUTTON) { if (!b->hilite) return 1; b->hilite = 0; repaint(w); }
        btn_click(w);
        return 1;
    case WM_CHAR:
        if (!(mp1 & KC_KEYUP) && (mp2 & 0xFFFF) == ' ') { btn_click(w); return 1; }
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    case BM_CLICK: btn_click(w); return 1;
    case BM_QUERYCHECK: return (uint32_t)b->check;
    case BM_SETCHECK: { int o = b->check; b->check = (int)(mp1 & 0xFFFF); repaint(w); return (uint32_t)o; }
    case BM_QUERYCHECKINDEX: {
        pmwin_t *g[64];
        int n = radio_group(w, g, 64);
        for (int i = 0; i < n; i++) if (btn(g[i])->check) return (uint32_t)i;
        return 0xFFFFFFFF;
    }
    case BM_QUERYHILITE: return (uint32_t)b->hilite;
    case BM_SETHILITE: { int o = b->hilite; b->hilite = (int)(mp1 & 1); repaint(w); return (uint32_t)o; }
    case BM_SETDEFAULT: b->def = (int)(mp1 & 1); repaint(w); return 1;
    }
    return 0;
}

/* ── WC_STATIC ───────────────────────────────────────────────────────── */
static uint32_t sys_pointer(uint32_t id);
static pmwin_t *bmp_from_file(const uint8_t *d, size_t n);
static pmwin_t *ptr_from_file(const uint8_t *d, size_t n);
static void ptr_draw(pmwin_t *t, int x, int y, pmwin_t *p, uint32_t fs);
static void blit(pmwin_t *t, int tx0, int ty0, int tx1, int ty1, pmwin_t *s, int sx0, int sy0, int sx1, int sy1, uint32_t r, uint32_t pat);
static uint32_t static_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    (void)mp1; (void)mp2;
    if (msg != WM_PAINT) return msg == WM_CHAR && w->owner ? pm_send(w->owner, msg, mp1, mp2) : 0;
    w->invalid = 0;
    int t = (int)(w->style & SS_TYPES);
    if (t == SS_GROUPBOX) {
        int ty = w->cy - (pm_ascent + pm_descent) / 2 - 1;
        bevel(w, 0, 0, w->cx, ty + 1, 0);
        bevel(w, 1, 1, w->cx - 1, ty, 1);
        char p[256];
        plain(w->text, p, sizeof p);
        int tw = text_w(p, (int)strlen(p));
        if (tw) {
            fill(w, 6, ty - pm_ascent / 2 - 2, 10 + tw, w->cy, C_FACE);
            text_at(w, 8, w->cy - pm_ascent, p, (int)strlen(p), C_TEXT);
        }
    } else if (t == SS_ICON || t == SS_BITMAP || t == SS_SYSICON) {   /* "#id" */
        fill(w, 0, 0, w->cx, w->cy, C_FACE);
        uint32_t id = w->text[0] == '#' ? (uint32_t)atoi(w->text + 1) : (uint32_t)atoi(w->text);
        if (!w->ctl) {
            w->ctl = calloc(1, sizeof(uint32_t));
            uint32_t a, n;
            pmwin_t *g = 0;
            if (t == SS_SYSICON) *(uint32_t *)w->ctl = sys_pointer(id);
            else if (find_resource(0, t == SS_BITMAP ? RT_BITMAP : RT_POINTER, id, &a, &n) && ok(a, n)) g = t == SS_BITMAP ? bmp_from_file(C.mem + a, n) : ptr_from_file(C.mem + a, n);
            if (g) *(uint32_t *)w->ctl = g->hwnd;
        }
        pmwin_t *g = win(*(uint32_t *)w->ctl);
        if (g && !strcmp(g->cls, "#bitmap")) blit(w, 0, w->cy - g->cy, g->cx, w->cy, g, 0, 0, g->cx, g->cy, ROP_SRCCOPY, 0);
        else if (g) ptr_draw(w, 0, w->cy - g->cy, g, 0);
    } else if (t >= SS_FGNDRECT && t <= SS_BKGNDFRAME) {         /* rectangles and frames */
        uint32_t c = (t == SS_FGNDRECT || t == SS_FGNDFRAME) ? C_TEXT : (t == SS_HALFTONERECT || t == SS_HALFTONEFRAME) ? C_DARK : C_FACE;
        if (t <= SS_BKGNDRECT) fill(w, 0, 0, w->cx, w->cy, c);
        else bevel(w, 0, 0, w->cx, w->cy, 0);
    } else {                                                    /* SS_TEXT (and what isn't here yet) */
        fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_FACE));
        char p[256];
        plain(w->text, p, sizeof p);
        int tw = text_w(p, (int)strlen(p)), th = pm_ascent + pm_descent;
        int x = (w->style & DT_CENTER) ? (w->cx - tw) / 2 : (w->style & DT_RIGHT) ? w->cx - tw : 0;
        int y = (w->style & DT_VCENTER) ? (w->cy - th) / 2 : (w->style & DT_BOTTOM) ? 0 : w->cy - th;
        text_at(w, x, y + pm_descent, p, (int)strlen(p), (w->style & WS_DISABLED_) ? C_DARK : pp_fgc(w, C_TEXT));
    }
    present(w);
    return 0;
}

/* ── WC_ENTRYFIELD ───────────────────────────────────────────────────── */
typedef struct { int cur, an, limit, changed, ro, first, drag; } ef_t;   /* (the selection: from an to cur) */
static ef_t *ef(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(ef_t)); ((ef_t *)w->ctl)->limit = 32; ((ef_t *)w->ctl)->cur = ((ef_t *)w->ctl)->an = -1; } return w->ctl; }
static void clip_put(const char *s);
static const char *clip_take(void);
/* the clipboard's keys: 1 cut, 2 copy, 3 paste (Ctrl+X/C/V; PM's Shift+Del, Ctrl+Ins, Shift+Ins) */
static int clip_key(uint32_t mp1, uint32_t mp2)
{
    int ch = (mp1 & KC_CHAR) ? (int)(mp2 & 0xFFFF) : 0, vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
    if (mp1 & KC_CTRL) { if (ch == 'x' || ch == 'X') return 1; if (ch == 'c' || ch == 'C') return 2; if (ch == 'v' || ch == 'V') return 3; if (vk == VK_INSERT) return 2; }
    if ((mp1 & KC_SHIFT) && vk == VK_DELETE) return 1;
    if ((mp1 & KC_SHIFT) && vk == VK_INSERT) return 3;
    return 0;
}
static int ef_lo(ef_t *e) { return e->an < e->cur ? e->an : e->cur; }
static int ef_hi(ef_t *e) { return e->an < e->cur ? e->cur : e->an; }
static int ef_del_sel(pmwin_t *w, ef_t *e)      /* the selection out of the text: 1 if there was one */
{
    int lo = ef_lo(e), hi = ef_hi(e), n = (int)strlen(w->text);
    if (hi <= lo) return 0;
    memmove(w->text + lo, w->text + hi, (size_t)(n - hi + 1));
    e->cur = e->an = lo;
    return 1;
}
static int ef_at(pmwin_t *w, ef_t *e, int x)     /* the character boundary nearest x */
{
    int n = (int)strlen(w->text), k = e->first;
    x -= (w->style & ES_MARGIN) ? 5 : 3;
    while (k < n && text_w(w->text + e->first, k - e->first) + text_w(w->text + k, 1) / 2 <= x) k++;
    return k;
}
static uint32_t entry_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    ef_t *e = ef(w);
    int n = (int)strlen(w->text);
    if (e->cur < 0 || e->cur > n) e->cur = n;
    if (e->an < 0 || e->an > n) e->an = e->cur;
    switch (msg) {
    case WM_CREATE:
        if (w->style & ES_READONLY) e->ro = 1;
        return 0;
    case WM_PAINT: {
        w->invalid = 0;
        int m = (w->style & ES_MARGIN) ? 3 : 1;
        uint32_t bg = (w->style & WS_DISABLED_) ? C_FACE : w->has_bg ? w->pp_bg : C_LIGHT, fg = pp_fgc(w, C_TEXT);
        fill(w, 0, 0, w->cx, w->cy, bg);
        if (w->style & ES_MARGIN) { bevel(w, 0, 0, w->cx, w->cy, 0); bevel(w, 1, 1, w->cx - 1, w->cy - 1, 0); }
        /* (scrolled so the cursor shows) */
        if (e->first > e->cur) e->first = e->cur;
        while (e->first < e->cur && text_w(w->text + e->first, e->cur - e->first) > w->cx - 2 * m - 4) e->first++;
        int x = m + 2, y = (w->cy - (pm_ascent + pm_descent)) / 2 + pm_descent;
        if (w->style & ES_UNREADABLE) { fg = bg; }              /* (not shown) */
        /* the text in three parts: before the selection, it (white on blue), after */
        int lo = ef_lo(e) > e->first ? ef_lo(e) : e->first, hi = ef_hi(e);
        const char *s = w->text + e->first;
        if (hi > lo && pm_focus == w->hwnd) {
            int x0 = x + text_w(s, lo - e->first), x1 = x0 + text_w(w->text + lo, hi - lo);
            text_at(w, x, y, s, lo - e->first, fg);
            fill(w, x0, y - pm_descent, x1, y + pm_ascent, C_HILITE);
            text_at(w, x0, y, w->text + lo, hi - lo, (w->style & ES_UNREADABLE) ? C_HILITE : C_LIGHT);
            text_at(w, x1, y, w->text + hi, n - hi, fg);
        } else text_at(w, x, y, s, (int)strlen(s), fg);
        if (pm_focus == w->hwnd && !e->ro) {
            int cx = x + text_w(s, e->cur - e->first);
            line(w, cx, y - pm_descent, cx, y + pm_ascent, C_TEXT);
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN:
        e->cur = ef_at(w, e, (int16_t)(mp1 & 0xFFFF));
        if (!((mp2 >> 16) & KC_SHIFT)) e->an = e->cur;                 /* (Shift+click: to there) */
        e->drag = 1; pm_capture = w->hwnd;
        repaint(w);
        return 1;
    case WM_MOUSEMOVE:
        if (e->drag) { int k = ef_at(w, e, (int16_t)(mp1 & 0xFFFF)); if (k != e->cur) { e->cur = k; repaint(w); } }
        return 1;
    case WM_BUTTON1UP:
        if (e->drag) { e->drag = 0; pm_capture = 0; }
        return 1;
    case WM_BUTTON1DBLCLK: {                                   /* the word */
        int k = ef_at(w, e, (int16_t)(mp1 & 0xFFFF)), a = k, b = k;
        while (a > 0 && w->text[a - 1] != ' ') a--;
        while (b < n && w->text[b] != ' ') b++;
        e->an = a; e->cur = b;
        repaint(w);
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int ck = clip_key(mp1, mp2);
        if (ck) return entry_proc(w, ck == 1 ? EM_CUT : ck == 2 ? EM_COPY : EM_PASTE, 0, 0);
        int ch = (int)(mp2 & 0xFFFF), vk = (int)(mp2 >> 16), done = 1, changed = 0, shift = (mp1 & KC_SHIFT) != 0, moved = -1;
        if ((mp1 & KC_CTRL) && (mp1 & KC_CHAR) && (ch == '/' || ch == 'a' || ch == 'A')) { e->an = 0; e->cur = n; repaint(w); return 1; }   /* select all */
        if ((mp1 & KC_VIRTUALKEY) && vk) {
            switch (vk) {
            case VK_BACKSPACE: if (!e->ro) { if (ef_del_sel(w, e)) changed = 1; else if (e->cur > 0) { memmove(w->text + e->cur - 1, w->text + e->cur, (size_t)(n - e->cur + 1)); e->an = --e->cur; changed = 1; } } break;
            case VK_DELETE: if (!e->ro) { if (ef_del_sel(w, e)) changed = 1; else if (e->cur < n) { memmove(w->text + e->cur, w->text + e->cur + 1, (size_t)(n - e->cur)); changed = 1; } } break;
            case VK_LEFT:                                          /* Left: a selection collapses to its start */
                moved = !shift && ef_hi(e) > ef_lo(e) ? ef_lo(e) : e->cur > 0 ? e->cur - 1 : 0;
                if ((mp1 & KC_CTRL)) { int k = e->cur; while (k > 0 && w->text[k - 1] == ' ') k--; while (k > 0 && w->text[k - 1] != ' ') k--; moved = k; }
                break;
            case VK_RIGHT:
                moved = !shift && ef_hi(e) > ef_lo(e) ? ef_hi(e) : e->cur < n ? e->cur + 1 : n;
                if ((mp1 & KC_CTRL)) { int k = e->cur; while (k < n && w->text[k] != ' ') k++; while (k < n && w->text[k] == ' ') k++; moved = k; }
                break;
            case VK_HOME: moved = 0; break;
            case VK_END: moved = n; break;
            default: done = 0;
            }
        } else if ((mp1 & KC_CHAR) && ch >= 32 && ch != 127 && !(mp1 & (KC_CTRL | KC_ALT))) {
            if (!e->ro) {
                if (ef_del_sel(w, e)) { changed = 1; n = (int)strlen(w->text); }   /* (typing replaces the selection) */
                if (n < e->limit && n + 1 < (int)sizeof w->text) {
                    memmove(w->text + e->cur + 1, w->text + e->cur, (size_t)(n - e->cur + 1));
                    w->text[e->cur++] = (char)ch;
                    e->an = e->cur;
                    changed = 1;
                }
            }
        } else if ((mp1 & KC_CHAR) && ch == 8) {
            if (!e->ro) { if (ef_del_sel(w, e)) changed = 1; else if (e->cur > 0) { memmove(w->text + e->cur - 1, w->text + e->cur, (size_t)(n - e->cur + 1)); e->an = --e->cur; changed = 1; } }
        } else done = 0;
        if (!done) return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);   /* (Enter, Tab, Esc: the dialog's) */
        if (moved >= 0) { e->cur = moved; if (!shift) e->an = moved; }
        repaint(w);
        if (changed) { e->changed = 1; notify(w, EN_CHANGE, 0); }
        return 1;
    }
    case EM_SETTEXTLIMIT: e->limit = (int)(mp1 & 0xFFFF); if (e->limit > 255) e->limit = 255; return 1;
    case EM_QUERYCHANGED: { int c = e->changed; e->changed = 0; return (uint32_t)c; }
    case EM_QUERYSEL: return (uint32_t)ef_lo(e) | (uint32_t)ef_hi(e) << 16;                      /* (first, past the last) */
    case EM_SETSEL: {                                                                            /* (anchor, cursor) */
        int a = (int)(mp1 & 0xFFFF), b = (int)(mp1 >> 16);
        e->an = a > n ? n : a; e->cur = b > n ? n : b;
        repaint(w);
        return 1;
    }
    case EM_QUERYREADONLY: return (uint32_t)e->ro;
    case EM_SETREADONLY: { int o = e->ro; e->ro = (int)(mp1 & 1); return (uint32_t)o; }
    case EM_QUERYFIRSTCHAR: return (uint32_t)e->first;
    case EM_SETFIRSTCHAR: e->first = (int)(mp1 & 0xFFFF); repaint(w); return 1;
    case EM_CLEAR:                                                                               /* the selection */
        if (!e->ro && ef_del_sel(w, e)) { repaint(w); e->changed = 1; notify(w, EN_CHANGE, 0); }
        return 1;
    case EM_CUT: case EM_COPY: {                                                                 /* the selection */
        int lo = ef_lo(e), hi = ef_hi(e);
        if (hi <= lo || (w->style & ES_UNREADABLE)) return 1;
        char b[256];
        snprintf(b, sizeof b, "%.*s", hi - lo, w->text + lo);
        clip_put(b);
        if (msg == EM_CUT && !e->ro) { ef_del_sel(w, e); repaint(w); e->changed = 1; notify(w, EN_CHANGE, 0); }
        return 1;
    }
    case EM_PASTE: {                                                                             /* over the selection, one line */
        const char *t = clip_take();
        if (!t || e->ro) return 1;
        ef_del_sel(w, e);
        for (; *t && *t != '\r' && *t != '\n'; t++) {
            n = (int)strlen(w->text);
            if (n >= e->limit || n + 1 >= (int)sizeof w->text) break;
            memmove(w->text + e->cur + 1, w->text + e->cur, (size_t)(n - e->cur + 1));
            w->text[e->cur++] = *t;
        }
        e->an = e->cur;
        repaint(w); e->changed = 1; notify(w, EN_CHANGE, 0);
        return 1;
    }
    }
    return 0;
}

/* ── WC_LISTBOX ──────────────────────────────────────────────────────── */
typedef struct { char *text; uint32_t handle; } lbitem_t;
typedef struct { lbitem_t *it; int n, cap, sel, top, ih; } lb_t;   /* (ih: an item's height, the owner's for LS_OWNERDRAW) */
static lb_t *lb(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(lb_t)); ((lb_t *)w->ctl)->sel = -1; } return w->ctl; }
static int lb_ih(pmwin_t *w, lb_t *l)
{
    if (!l->ih && (w->style & LS_OWNERDRAW)) {                /* WM_MEASUREITEM (id, item): the height */
        pmwin_t *o = w->owner ? w->owner : w->parent;
        l->ih = -1;
        int h = o ? (int)(pm_send(o, 0x0037, w->id & 0xFFFF, 0) & 0xFFFF) : 0;
        l->ih = h > 0 && h < 400 ? h : pm_lineh;
    }
    return l->ih > 0 ? l->ih : pm_lineh;
}
static int lb_rows(pmwin_t *w) { lb_t *l = lb(w); int r = (w->cy - 4) / lb_ih(w, l); return r < 1 ? 1 : r; }
static void lb_show_sel(pmwin_t *w, lb_t *l)
{
    if (l->sel < 0) return;
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + lb_rows(w)) l->top = l->sel - lb_rows(w) + 1;
}
static uint32_t list_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    lb_t *l = lb(w);
    int16_t i1 = (int16_t)(mp1 & 0xFFFF);
    switch (msg) {
    case WM_DESTROY:
        for (int i = 0; i < l->n; i++) free(l->it[i].text);
        free(l->it);
        return 0;
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, C_LIGHT);
        bevel(w, 0, 0, w->cx, w->cy, 0);
        int rows = lb_rows(w);
        for (int r = 0; r < rows && l->top + r < l->n; r++) {
            int ih = lb_ih(w, l), k = l->top + r, y1 = w->cy - 2 - r * ih, y0 = y1 - ih;
            int on = k == l->sel;
            if (w->style & LS_OWNERDRAW) {                      /* the owner's, else ours; selected and left to us: inverted */
                uint32_t st[4];
                if (owner_draw(w, w->id & 0xFFFF, 2, y0, w->cx - 2, y1, (uint32_t)k, l->it[k].handle, (uint32_t)on, (uint32_t)on, 0, 0, st)) {
                    if (st[0]) invert_rect(w, 2, y0, w->cx - 2, y1);
                    continue;
                }
            }
            if (on) fill(w, 2, y0, w->cx - 2, y1, C_HILITE);
            text_at(w, 5, y0 + (ih - pm_ascent - pm_descent) / 2 + pm_descent, l->it[k].text, (int)strlen(l->it[k].text), on ? C_LIGHT : C_TEXT);
        }
        if (l->n > rows) {                                      /* (a scroll mark: there's more) */
            int h = w->cy - 4, bh = h * rows / l->n, by = 2 + (h - bh) * (l->n - rows - l->top) / (l->n - rows);
            fill(w, w->cx - 6, by, w->cx - 3, by + bh, C_DARK);
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1DBLCLK: if (l->sel >= 0) notify(w, LN_ENTER, 0); return 1;
    case WM_BUTTON1DOWN: {
        int y = (int16_t)(mp1 >> 16), k = l->top + (w->cy - 2 - y) / lb_ih(w, l);
        if (k >= 0 && k < l->n && k != l->sel) { l->sel = k; repaint(w); notify(w, LN_SELECT, 0); }
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, o = l->sel;
        if (vk == VK_UP && l->sel > 0) l->sel--;
        else if (vk == VK_DOWN && l->sel + 1 < l->n) l->sel++;
        else if (vk == VK_HOME && l->n) l->sel = 0;
        else if (vk == VK_END && l->n) l->sel = l->n - 1;
        else if ((vk == VK_NEWLINE || vk == VK_ENTER) && l->sel >= 0) { notify(w, LN_ENTER, 0); return 1; }
        else if (!vk) return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        else return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        if (l->sel != o) { lb_show_sel(w, l); repaint(w); notify(w, LN_SELECT, 0); }
        return 1;
    }
    case LM_QUERYITEMCOUNT: return (uint32_t)l->n;
    case LM_INSERTITEM: {
        char *s = gs(mp2);
        if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->it = realloc(l->it, (size_t)l->cap * sizeof *l->it); }
        int at = i1 == LIT_END || i1 > l->n ? l->n : i1;
        if (i1 == LIT_SORTASCENDING || i1 == LIT_SORTDESCENDING) {
            at = 0;
            while (at < l->n && (i1 == LIT_SORTASCENDING ? strcasecmp(l->it[at].text, s ? s : "") <= 0 : strcasecmp(l->it[at].text, s ? s : "") >= 0)) at++;
        } else if (i1 < LIT_END) at = l->n;
        memmove(l->it + at + 1, l->it + at, (size_t)(l->n - at) * sizeof *l->it);
        l->it[at].text = strdup(s ? s : "");
        l->it[at].handle = 0;
        l->n++;
        if (l->sel >= at) l->sel++;
        repaint(w);
        return (uint32_t)at;
    }
    case LM_DELETEITEM:
        if (i1 < 0 || i1 >= l->n) return (uint32_t)l->n;
        free(l->it[i1].text);
        memmove(l->it + i1, l->it + i1 + 1, (size_t)(l->n - i1 - 1) * sizeof *l->it);
        l->n--;
        if (l->sel == i1) l->sel = -1; else if (l->sel > i1) l->sel--;
        if (l->top > 0 && l->top >= l->n) l->top = l->n - 1;
        repaint(w);
        return (uint32_t)l->n;
    case LM_DELETEALL:
        for (int i = 0; i < l->n; i++) free(l->it[i].text);
        l->n = 0; l->sel = -1; l->top = 0;
        repaint(w);
        return 1;
    case LM_SELECTITEM:
        if (i1 == LIT_NONE) { l->sel = -1; repaint(w); return 1; }
        if (i1 < 0 || i1 >= l->n) return 0;
        if (mp2) { if (l->sel != i1) { l->sel = i1; lb_show_sel(w, l); repaint(w); notify(w, LN_SELECT, 0); } }
        else if (l->sel == i1) { l->sel = -1; repaint(w); }
        return 1;
    case LM_QUERYSELECTION: {
        if (l->sel < 0 || (i1 != LIT_FIRST && l->sel <= i1)) return (uint32_t)LIT_NONE;  /* (from the first, else after mp1) */
        return (uint32_t)l->sel;
    }
    case LM_QUERYITEMTEXT: {                                              /* (index, cchMax), buffer */
        int cch = (int)(mp1 >> 16);
        if (i1 < 0 || i1 >= l->n || cch < 1 || !ok(mp2, (uint32_t)cch)) return 0;
        int k = (int)strlen(l->it[i1].text);
        if (k > cch - 1) k = cch - 1;
        memcpy(C.mem + mp2, l->it[i1].text, (size_t)k);
        C.mem[mp2 + (uint32_t)k] = 0;
        return (uint32_t)k;
    }
    case LM_QUERYITEMTEXTLENGTH: return i1 >= 0 && i1 < l->n ? (uint32_t)strlen(l->it[i1].text) : 0xFFFFFFFF;
    case LM_SETITEMTEXT: {
        char *s = gs(mp2);
        if (i1 < 0 || i1 >= l->n) return 0;
        free(l->it[i1].text);
        l->it[i1].text = strdup(s ? s : "");
        repaint(w);
        return 1;
    }
    case LM_SETITEMHANDLE: if (i1 < 0 || i1 >= l->n) return 0; l->it[i1].handle = mp2; return 1;
    case LM_QUERYITEMHANDLE: return i1 >= 0 && i1 < l->n ? l->it[i1].handle : 0;
    case LM_SETITEMHEIGHT: if ((int)mp1 > 0 && (int)mp1 < 400) { l->ih = (int)mp1; repaint(w); } return 1;
    case LM_SEARCHSTRING: {                                               /* (flags, start), text */
        char *s = gs(mp2);
        int16_t start = (int16_t)(mp1 >> 16);
        uint32_t fl = mp1 & 0xFFFF;
        if (!s) return (uint32_t)LIT_NONE;
        int cs = (fl & LSS_CASESENSITIVE) != 0;
        for (int j = 0; j < l->n; j++) {
            int k = (start < 0 ? j : (start + 1 + j)) % (l->n ? l->n : 1);
            const char *t = l->it[k].text;
            int hit = (fl & LSS_SUBSTRING) ? (cs ? strstr(t, s) : strcasestr(t, s)) != 0           /* (it in the item) */
                    : (fl & LSS_PREFIX) ? !(cs ? strncmp : strncasecmp)(t, s, strlen(s))     /* (the item begins with it) */
                    : !(cs ? strcmp : strcasecmp)(t, s);                                     /* (neither: the whole item) */
            if (hit) return (uint32_t)k;
        }
        return (uint32_t)LIT_NONE;
    }
    case LM_SETTOPINDEX: if (i1 >= 0 && i1 < l->n) { l->top = i1; repaint(w); } return 1;
    case LM_QUERYTOPINDEX: return (uint32_t)l->top;
    }
    return 0;
}

/* ── menus ───────────────────────────────────────────────────────────── */
typedef struct menu menu_t;
typedef struct { char text[64]; uint16_t id, style, attr; menu_t *sub; int ow, oh; } mitem_t;   /* (ow, oh: MIS_OWNERDRAW, as the owner measured it) */
struct menu { mitem_t *it; int n, open; uint32_t id; };   /* (id: a pull-down's, its item's in the bar) */

static menu_t *menu_parse(uint32_t t, uint32_t end)        /* a menu template at t */
{
    menu_t *m = calloc(1, sizeof *m);
    if (!ok(t, 10)) return m;
    uint32_t cb = g32(t), n = g16(t + 8), p = t + 10;
    if (cb && t + cb < end) end = t + cb;
    m->it = calloc(n ? n : 1, sizeof *m->it);
    for (uint32_t i = 0; i < n && p + 6 <= end; i++) {
        mitem_t *it = &m->it[m->n++];
        it->style = g16(p); it->attr = g16(p + 2); it->id = g16(p + 4);
        p += 6;
        if (it->style & MIS_SEPARATOR) continue;            /* (no text) */
        {                                                   /* the text (bitmaps' "#id", owner-drawn items' too) */
            char *s = gs(p);
            snprintf(it->text, sizeof it->text, "%s", s ? s : "");
            p += (uint32_t)strlen(s ? s : "") + 1;
        }
        if (it->style & MIS_SUBMENU) {                      /* its template follows */
            uint32_t scb = g32(p);
            it->sub = menu_parse(p, end);
            p += scb ? scb : 10;
        }
    }
    return m;
}
static mitem_t *menu_find(menu_t *m, uint32_t id, int deep)
{
    for (int i = 0; m && i < m->n; i++) {
        if (m->it[i].id == id) return &m->it[i];
        if (deep && m->it[i].sub) { mitem_t *r = menu_find(m->it[i].sub, id, deep); if (r) return r; }
    }
    return 0;
}
static void menu_free(menu_t *m)
{
    if (!m) return;
    for (int i = 0; i < m->n; i++) menu_free(m->it[i].sub);
    free(m->it);
    free(m);
}
/* the menu bar's items, left to right */
static int mbar_x(menu_t *m, int i) { int x = 4; for (int k = 0; k < i; k++) x += plain_w(m->it[k].text) + 16; return x; }
static int drop_item_h(mitem_t *it) { return (it->style & MIS_SEPARATOR) ? 8 : (it->style & MIS_OWNERDRAW) && it->oh > 0 ? it->oh : pm_lineh + 4; }
static void menu_close(pmwin_t *f)
{
    if (!f || !f->drop) return;
    if (f->drop->iproc == list_proc) {                      /* a combo box's list: hidden, kept */
        pmwin_t *l = f->drop, *c = l->owner;
        l->visible = 0;
        f->drop = 0;
        invalidate(f);
        if (c) repaint(c);
        return;
    }
    pmwin_t *bar = win_child(f, FID_MENU);
    if (bar && bar->ctl) ((menu_t *)bar->ctl)->open = -1;
    f->drop->ctl = 0;                                       /* (the bar's submenu: not the drop-down's to free) */
    win_destroy(f->drop);
    f->drop = 0;
    invalidate(f);
}
static void menu_command(pmwin_t *f, mitem_t *it)
{
    if (it->attr & MIA_DISABLED) return;
    menu_close(f);
    pmwin_t *t = f->client ? f->client : f;
    post(t, t->hwnd, WM_COMMAND, it->id, CMDSRC_MENU | 1u << 16);     /* by the pointer */
}
static uint32_t drop_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    menu_t *m = w->ctl;
    (void)mp2;
    if (!m) return 0;
    switch (msg) {
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, C_FACE);
        bevel(w, 0, 0, w->cx, w->cy, 1);
        int y = w->cy - 2;
        for (int i = 0; i < m->n; i++) {
            mitem_t *it = &m->it[i];
            int h = drop_item_h(it);
            if (it->style & MIS_SEPARATOR) { line(w, 4, y - h / 2, w->cx - 5, y - h / 2, C_DARK); line(w, 4, y - h / 2 - 1, w->cx - 5, y - h / 2 - 1, C_LIGHT); }
            else if (it->style & MIS_OWNERDRAW) {               /* WM_DRAWITEM (MIA_HILITED in fsAttribute; a newly lit one inverted if the owner leaves it) */
                pmwin_t *f = frame_of(w);
                pmwin_t *save = w->owner;
                w->owner = f && f->client ? f->client : f;
                uint32_t at = it->attr | (i == m->open ? 0x8000u : 0), st[4];
                int drawn = owner_draw(w, m->id, 2, y - h, w->cx - 2, y, it->id, 0, 0, 0, at, at & ~0x8000u, st);
                w->owner = save;
                if (drawn && (st[1] & 0x8000) && !(st[3] & 0x8000)) invert_rect(w, 2, y - h, w->cx - 2, y);
                if (!drawn) text_mid(w, 20, y - h, y, it->text, C_TEXT);
            } else {
                if (i == m->open) fill(w, 2, y - h, w->cx - 2, y, C_HILITE);
                if (it->attr & MIA_CHECKED) { line(w, 6, y - h / 2, 9, y - h / 2 - 3, C_TEXT); line(w, 9, y - h / 2 - 3, 14, y - h / 2 + 3, C_TEXT); }   /* MIA_CHECKED */
                char lab[64];
                snprintf(lab, sizeof lab, "%s", it->text);
                char *tab = strchr(lab, '\t');
                if (tab) *tab = 0;
                text_mid(w, 20, y - h, y, lab, i == m->open ? C_LIGHT : (it->attr & MIA_DISABLED) ? C_DARK : C_TEXT);
                if (tab) text_mid(w, w->cx - 8 - plain_w(tab + 1), y - h, y, tab + 1, i == m->open ? C_LIGHT : C_TEXT);
                if (it->sub) text_at(w, w->cx - 14, y - h + 6, ">", 1, C_TEXT);
            }
            y -= h;
        }
        present(w);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int y = (int16_t)(mp1 >> 16), top = w->cy - 2, h = -1;
        for (int i = 0; i < m->n; i++) { int ih = drop_item_h(&m->it[i]); if (y <= top && y > top - ih && !(m->it[i].style & MIS_SEPARATOR)) h = i; top -= ih; }
        if (h != m->open) { m->open = h; repaint(w); }
        return 1;
    }
    case WM_BUTTON1UP: {
        int y = (int16_t)(mp1 >> 16), top = w->cy - 2;
        for (int i = 0; i < m->n; i++) {
            int h = drop_item_h(&m->it[i]);
            if (y <= top && y > top - h) {
                if (!(m->it[i].style & MIS_SEPARATOR) && !m->it[i].sub) menu_command(w->parent, &m->it[i]);
                return 1;
            }
            top -= h;
        }
        return 1;
    }
    }
    return 0;
}
static void menu_open(pmwin_t *bar, menu_t *m, int i)
{
    pmwin_t *f = bar->parent;
    menu_close(f);
    menu_t *sub = m->it[i].sub;
    if (!sub) return;
    pmwin_t *d = win_new(f, "#drop", 0, 0);
    if (!d) return;
    d->iproc = drop_proc;
    d->ctl = sub;                                           /* (not d's: the bar's menu frees it) */
    sub->open = -1;
    d->overlay = 1;
    int wmax = 60, h = 4;
    sub->id = m->it[i].id;
    for (int k = 0; k < sub->n; k++) {
        mitem_t *it = &sub->it[k];
        if ((it->style & MIS_OWNERDRAW) && !it->oh) {         /* WM_MEASUREITEM (menu id, OWNERITEM): its rclItem's width and height */
            pmwin_t *o = f->client ? f->client : f;
            uint32_t oi = gscratch(48);
            it->oh = pm_lineh + 4; it->ow = 0;
            if (oi) {
                p32(oi, bar->hwnd); p32(oi + 40, it->id);
                uint32_t r = pm_send(o, 0x0037, sub->id, oi);
                int rh = (int)(r & 0xFFFF), ow = (int32_t)g32(oi + 32) - (int32_t)g32(oi + 24), oh = (int32_t)g32(oi + 36) - (int32_t)g32(oi + 28);
                if (oh > 0 && oh < 400) it->oh = oh; else if (rh > 0 && rh < 400) it->oh = rh;
                if (ow > 0 && ow < 1000) it->ow = ow;
            }
        }
        int tw = (it->style & MIS_OWNERDRAW) ? it->ow + 8 : plain_w(it->text) + 64;
        if (tw > wmax) wmax = tw;
        h += drop_item_h(it);
    }
    d->cx = wmax; d->cy = h;
    d->x = mbar_x(m, i) - 4; d->y = bar->y - h;
    if (d->x + d->cx > f->cx) d->x = f->cx - d->cx;
    if (d->x < 0) d->x = 0;
    if (d->y < 0) d->y = 0;
    d->owner = bar;
    d->visible = 1;
    d->invalid = 1;
    f->drop = d;
    m->open = i;
    repaint(bar);
}
static uint32_t menubar_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    menu_t *m = w->ctl;
    if (!m) return 0;
    switch (msg) {
    case WM_DESTROY:
        if (w->parent && w->parent->drop) { w->parent->drop->ctl = 0; menu_close(w->parent); }
        menu_free(m);
        w->ctl = 0;
        return 0;
    case WM_PAINT:
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, C_FACE);
        line(w, 0, 0, w->cx - 1, 0, C_DARK);
        for (int i = 0; i < m->n; i++) {
            int x = mbar_x(m, i), tw = plain_w(m->it[i].text);
            if (i == m->open && w->parent && w->parent->drop) fill(w, x - 4, 2, x + tw + 4, w->cy - 1, C_HILITE);
            text_mid(w, x, 1, w->cy, m->it[i].text, i == m->open && w->parent && w->parent->drop ? C_LIGHT : (m->it[i].attr & MIA_DISABLED) ? C_DARK : C_TEXT);
        }
        present(w);
        return 0;
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF);
        for (int i = 0; i < m->n; i++) {
            int x0 = mbar_x(m, i) - 4, x1 = mbar_x(m, i + 1) - 4;
            if (x < x0 || x >= x1) continue;
            if (w->parent->drop && m->open == i) { menu_close(w->parent); return 1; }
            if (m->it[i].sub) menu_open(w, m, i);
            else { menu_close(w->parent); menu_command(w->parent, &m->it[i]); }
            return 1;
        }
        menu_close(w->parent);
        return 1;
    }
    case MM_SETITEMATTR: {                                                /* (id, deep), (mask, value) */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, (int)(mp1 >> 16));
        if (!it) return 0;
        uint16_t mask = (uint16_t)(mp2 & 0xFFFF), v = (uint16_t)(mp2 >> 16);
        it->attr = (uint16_t)((it->attr & ~mask) | (v & mask));
        repaint(w);
        return 1;
    }
    case MM_QUERYITEMATTR: {                                              /* (id, deep), mask */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, (int)(mp1 >> 16));
        return it ? (uint32_t)(it->attr & mp2) : 0;
    }
    case MM_SETITEMTEXT: {                                                /* id, text */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, 1);
        char *s = gs(mp2);
        if (!it) return 0;
        snprintf(it->text, sizeof it->text, "%s", s ? s : "");
        repaint(w);
        return 1;
    }
    case MM_QUERYITEMTEXT: {                                              /* (id, cch), buffer */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, 1);
        uint32_t cch = mp1 >> 16;
        if (!it || !cch || !ok(mp2, cch)) return 0;
        uint32_t k = (uint32_t)strlen(it->text);
        if (k > cch - 1) k = cch - 1;
        memcpy(C.mem + mp2, it->text, k);
        C.mem[mp2 + k] = 0;
        return k;
    }
    case MM_QUERYITEMTEXTLENGTH: { mitem_t *it = menu_find(m, mp1 & 0xFFFF, 1); return it ? (uint32_t)strlen(it->text) : 0; }
    case MM_QUERYITEMCOUNT: return (uint32_t)m->n;
    case MM_ISITEMVALID: { mitem_t *it = menu_find(m, mp1 & 0xFFFF, (int)(mp1 >> 16)); return it && !(it->attr & MIA_DISABLED); }
    case MM_ENDMENUMODE: menu_close(w->parent); return 1;
    case MM_DELETEITEM: case MM_REMOVEITEM: {
        for (int i = 0; i < m->n; i++)
            if (m->it[i].id == (mp1 & 0xFFFF)) {
                menu_free(m->it[i].sub);
                memmove(m->it + i, m->it + i + 1, (size_t)(m->n - i - 1) * sizeof *m->it);
                m->n--;
                repaint(w);
                return (uint32_t)m->n;
            }
        return (uint32_t)m->n;
    }
    case MM_INSERTITEM: {                                                 /* MENUITEM, text */
        uint32_t p = mp1;
        if (!ok(p, 16)) return 0xFFFFFFFF;
        int16_t pos = (int16_t)g16(p);
        m->it = realloc(m->it, (size_t)(m->n + 1) * sizeof *m->it);
        int at = pos < 0 || pos > m->n ? m->n : pos;
        memmove(m->it + at + 1, m->it + at, (size_t)(m->n - at) * sizeof *m->it);
        mitem_t *it = &m->it[at];
        memset(it, 0, sizeof *it);
        it->style = g16(p + 2); it->attr = g16(p + 4); it->id = g16(p + 6);
        char *s = mp2 ? gs(mp2) : 0;
        snprintf(it->text, sizeof it->text, "%s", s ? s : "");
        m->n++;
        repaint(w);
        return (uint32_t)at;
    }
    }
    (void)mp2;
    return 0;
}
static void frame_layout(pmwin_t *f)
{
    pmwin_t *bar = win_child(f, FID_MENU);
    pmwin_t *vs = win_child(f, FID_VERTSCROLL), *hs = win_child(f, FID_HORZSCROLL);
    int mh = bar ? MENU_H : 0, sw = SB_W, vw = vs ? sw : 0, hh = hs ? sw : 0;
    if (bar) { bar->x = 0; bar->y = f->ph - mh; bar->cx = f->pw; bar->cy = mh; bar->visible = 1; }
    if (vs) { vs->x = f->pw - sw; vs->y = hh; vs->cx = sw; vs->cy = f->ph - mh - hh; vs->visible = 1; }
    if (hs) { hs->x = 0; hs->y = 0; hs->cx = f->pw - vw; hs->cy = sw; hs->visible = 1; }
    if (f->client) { f->client->x = 0; f->client->y = hh; f->client->cx = f->pw - vw; f->client->cy = f->ph - mh - hh; }
}
static pmwin_t *menu_load(pmwin_t *f, uint32_t hmod, uint32_t id)
{
    uint32_t a, n;
    if (!find_resource(hmod, RT_MENU, id, &a, &n)) return 0;
    pmwin_t *bar = win_new(f, "#4", 0, FID_MENU);
    if (!bar) return 0;
    bar->iproc = menubar_proc;
    menu_t *m = menu_parse(a, a + n);
    m->open = -1;
    bar->ctl = m;
    bar->owner = f;
    bar->visible = 1;
    frame_layout(f);
    invalidate(bar);
    return bar;
}

static uint32_t scroll_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t combo_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t mle_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t spin_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t slider_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t notebook_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t container_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t valueset_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static uint32_t circslider_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
static pmproc_t builtin_proc(const char *cls)
{
    if (!strcmp(cls, "#3")) return button_proc;
    if (!strcmp(cls, "#5")) return static_proc;
    if (!strcmp(cls, "#6")) return entry_proc;
    if (!strcmp(cls, "#7")) return list_proc;
    if (!strcmp(cls, "#8")) return scroll_proc;
    if (!strcmp(cls, "#2")) return combo_proc;
    if (!strcmp(cls, "#10")) return mle_proc;
    if (!strcmp(cls, "#32")) return spin_proc;
    if (!strcmp(cls, "#38")) return slider_proc;
    if (!strcmp(cls, "#40")) return notebook_proc;
    if (!strcmp(cls, "#37")) return container_proc;
    if (!strcmp(cls, "#39")) return valueset_proc;
    if (!strcmp(cls, "#65")) return circslider_proc;
    return 0;
}

/* ── dialogs ─────────────────────────────────────────────────────────── */
static void dlg_paint_bg(pmwin_t *w) { w->invalid = 0; fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_FACE)); present(w); }
static pmwin_t *dlg_tab(pmwin_t *dlg, pmwin_t *from, int dir)  /* the next (or previous) control with WS_TABSTOP */
{
    pmwin_t *list[MAXWIN];
    int n = 0, at = -1;
    for (int i = 0; i < MAXWIN; i++)
        if (pmwins[i].used && pmwins[i].parent == dlg && pmwins[i].visible && (pmwins[i].style & WS_TABSTOP_) && !(pmwins[i].style & WS_DISABLED_)) {
            if (&pmwins[i] == from) at = n;
            list[n++] = &pmwins[i];
        }
    if (!n) return 0;
    if (at < 0) return list[dir > 0 ? 0 : n - 1];
    return list[(at + dir + n) % n];
}
static pmwin_t *dlg_default(pmwin_t *dlg)
{
    for (int i = 0; i < MAXWIN; i++)
        if (pmwins[i].used && pmwins[i].parent == dlg && !strcmp(pmwins[i].cls, "#3") && btn_type(&pmwins[i]) == BS_PUSHBUTTON &&
            ((pmwins[i].style & BS_DEFAULT) || (pmwins[i].ctl && btn(&pmwins[i])->def))) return &pmwins[i];
    return 0;
}
static uint32_t def_dlg_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    switch (msg) {
    case WM_PAINT: dlg_paint_bg(w); return 0;
    case WM_COMMAND: w->dlg_done = 1; w->dlg_result = mp1 & 0xFFFF; return 0;
    case WM_CLOSE: post(w, w->hwnd, WM_COMMAND, DID_CANCEL, 0); return 0;
    case WM_HELP: help_request(w, mp1); return 0;
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 0;
        int ch = (int)(mp2 & 0xFFFF), vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        if (ch == '\r' || vk == VK_NEWLINE || vk == VK_ENTER) {
            pmwin_t *d = dlg_default(w);
            if (d) btn_click(d); else post(w, w->hwnd, WM_COMMAND, DID_OK, 0);
            return 1;
        }
        if (ch == 27 || vk == VK_ESC) { post(w, w->hwnd, WM_COMMAND, DID_CANCEL, 0); return 1; }
        if (ch == 9 || vk == VK_TAB) {
            pmwin_t *n = dlg_tab(w, win(pm_focus), (mp1 & KC_SHIFT) ? -1 : 1);
            if (n) set_focus(n->hwnd);
            return 1;
        }
        return 0;
    }
    }
    return 0;
}
static void cb_settext(pmwin_t *w);
static void clip_put(const char *s);
static const char *clip_take(void);
static int ctl_get_text(pmwin_t *w, char *out, int max);
static int ctl_set_text(pmwin_t *w, const char *s);
static uint32_t api_WinDefDlgProc(void) { pmwin_t *w = win(A(0)); return w ? def_dlg_proc(w, A(1), A(2), A(3)) : 0; }

/* the template's items, from item i (and its children) under parent */
static pmwin_t *dlg_host;                                   /* (dlg_create's: the window a dialog goes in, or none) */
static int dlg_items(uint32_t t, int i, pmwin_t *parent, pmwin_t *dlg, uint32_t proc, uint32_t createp, pmwin_t **out)
{
    uint32_t it = t + g16(t + 6) + 30u * (uint32_t)i;      /* (a DLGTITEM: 30 bytes) */
    uint32_t nkids = g16(it + 2), cch = g16(it + 4), offc = g16(it + 6), ccht = g16(it + 8), offt = g16(it + 10);
    uint32_t style = g32(it + 12), id = g16(it + 24), offpp = g16(it + 26), offctl = g16(it + 28);
    int x = du_x((int16_t)g16(it + 16)), y = du_y((int16_t)g16(it + 18)), cx = du_x((int16_t)g16(it + 20)), cy = du_y((int16_t)g16(it + 22));
    char cls[64], text[256] = "";
    if (cch) { char *s = gp(t + offc, cch); snprintf(cls, sizeof cls, "%.*s", (int)cch, s ? s : ""); }
    else snprintf(cls, sizeof cls, "#%u", offc);
    if (ccht && offt != 0xFFFF) { char *s = gp(t + offt, ccht); snprintf(text, sizeof text, "%.*s", (int)ccht, s ? s : ""); }
    pmwin_t *w;
    if (!parent && dlg_host) {                              /* the dialog itself, in a window (a notebook's page) */
        w = win_new(dlg_host, "#dialog", style & ~WS_VISIBLE_, id);
        if (!w) return -1;
        w->x = x; w->y = y; w->cx = cx; w->cy = cy;
        w->proc = proc;
        w->p16 = pm_dlg16 && proc;
        if (!proc) w->iproc = def_dlg_proc;
        w->visible = (style & WS_VISIBLE_) != 0;
        snprintf(w->text, sizeof w->text, "%s", text);
        if (offpp != 0xFFFF && offpp) pp_apply(w, t + offpp);
        *out = w;
        dlg = w;
    } else if (!parent) {                                   /* the dialog itself: a window of the desktop */
        w = win_new(0, "#dialog", style & ~WS_VISIBLE_, id);
        if (!w) return -1;
        w->top = 1;
        w->fixed = 1;
        w->proc = proc;
        w->p16 = pm_dlg16 && proc;
        if (!proc) w->iproc = def_dlg_proc;
        frame_pixels(w, cx > 0 ? cx : 200, cy > 0 ? cy : 100);
        snprintf(w->text, sizeof w->text, "%s", text);
        if (offpp != 0xFFFF && offpp) pp_apply(w, t + offpp);
        *out = w;
        dlg = w;
    } else {
        w = win_new(parent, cls, style & ~WS_VISIBLE_, id);
        if (!w) return -1;
        w->x = x; w->y = y; w->cx = cx; w->cy = cy;
        w->owner = dlg;
        snprintf(w->text, sizeof w->text, "%s", text);
        w->visible = (style & WS_VISIBLE_) != 0;
        if (offpp != 0xFFFF && offpp) pp_apply(w, t + offpp);
        pm_send(w, WM_CREATE, offctl != 0xFFFF && offctl ? t + offctl : 0, 0);
    }
    int next = i + 1;
    for (uint32_t k = 0; k < nkids && next > 0; k++) next = dlg_items(t, next, w, dlg, proc, createp, out);
    return next;
}
/* parent: HWND_DESKTOP (a window of the desktop) or a window it goes in */
static uint32_t dlg_create(uint32_t parent, uint32_t owner, uint32_t proc, uint32_t t, uint32_t createp)
{
    if (!pm_start() || !ok(t, 14 + 30)) return 0;
    pmwin_t *dlg = 0;
    dlg_host = parent > 4 ? win(parent) : 0;              /* (HWND_DESKTOP, _OBJECT, ...: on its own) */
    int r = dlg_items(t, 0, 0, 0, proc, createp, &dlg);
    pmwin_t *host = dlg_host;
    dlg_host = 0;
    if (r < 0 || !dlg) { if (dlg) win_destroy(dlg); return 0; }
    dlg->owner = win(owner);
    if (!host && pmb->title) pmb->title(dlg);
    pmwin_t *focus = dlg_tab(dlg, 0, 1);
    uint16_t ifocus = g16(t + 10);
    if (ifocus != 0xFFFF) {                                 /* (iItemFocus: the template's item number) */
        int k = 0;
        for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].parent == dlg && ++k == ifocus) focus = &pmwins[i];
    }
    uint32_t hwnd = dlg->hwnd;
    if (!pm_send(dlg, WM_INITDLG, focus ? focus->hwnd : 0, createp) && focus && win(hwnd) && !host) set_focus(focus->hwnd);
    if (!win(hwnd)) return 0;
    if (host) invalidate(dlg);
    else if (dlg->style & WS_VISIBLE_ || g32(t + g16(t + 6) + 12) & WS_VISIBLE_) win_show(dlg, 1);
    return hwnd;
}
static uint32_t api_WinCreateDlg(void) { return dlg_create(A(0), A(1), A(2), A(3), A(4)); }    /* (parent, owner, proc, ptemplate, createp) */
static uint32_t api_WinLoadDlg(void)          /* (hwndParent, hwndOwner, pfnDlgProc, hmod, idDlg, pCreateParams) */
{
    uint32_t a, n;
    if (!find_resource(A(3), RT_DIALOG, A(4), &a, &n)) return 0;
    return dlg_create(A(0), A(1), A(2), a, A(5));
}
static uint32_t dlg_process(uint32_t hwnd)
{
    pmwin_t *w = win(hwnd);
    if (!w) return 0xFFFF;
    pmq_t *q = myq();
    if (!q) return 0xFFFF;
    w->dlg_done = 0;
    if (!w->visible) win_show(w, 1);
    uint32_t pfocus = pm_focus;
    if (!pm_focus || frame_of(win(pm_focus)) != w) { pmwin_t *f = dlg_tab(w, 0, 1); set_focus(f ? f->hwnd : hwnd); }
    while (win(hwnd) && !w->dlg_done) {
        pmmsg_t m;
        pm_wait_msg(q, 0, 0, 0, &m);
        if (m.msg == WM_QUIT) { post(0, 0, WM_QUIT, 0, 0); break; }
        pmwin_t *t = win(m.hwnd);
        if (t) pm_send(t, m.msg, m.mp1, m.mp2);
    }
    uint32_t r = win(hwnd) && w->dlg_done ? w->dlg_result : 0xFFFF;
    if (win(hwnd)) win_show(w, 0);
    if (win(pfocus)) pm_focus = pfocus;
    return r;
}
static uint32_t api_WinProcessDlg(void) { return dlg_process(A(0)); }
static uint32_t api_WinDismissDlg(void)
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    w->dlg_done = 1;
    w->dlg_result = A(1);
    if (w->visible) win_show(w, 0);
    pthread_cond_broadcast(&gcv);
    return 1;
}
static uint32_t api_WinDlgBox(void)           /* (hwndParent, hwndOwner, pfnDlgProc, hmod, idDlg, pCreateParams) */
{
    uint32_t a, n;
    if (!find_resource(A(3), RT_DIALOG, A(4), &a, &n)) return DID_ERROR;
    uint32_t h = dlg_create(A(0), A(1), A(2), a, A(5));
    if (!h) return 0xFFFF;
    uint32_t r = dlg_process(h);
    pmwin_t *w = win(h);
    if (w) win_destroy(w);
    return r;
}
static pmwin_t *dlg_item(uint32_t hdlg, uint32_t id)
{
    return win_child(win(hdlg), id & 0xFFFF);
}
static uint32_t api_WinSendDlgItemMsg(void) { return pm_send(dlg_item(A(0), A(1)), A(2), A(3), A(4)); }
static uint32_t api_WinSetDlgItemText(void)
{
    pmwin_t *w = dlg_item(A(0), A(1));
    char *s = gs(A(2));
    if (!w) return 0;
    snprintf(w->text, sizeof w->text, "%s", s ? s : "");
    if (!strcmp(w->cls, "#2")) cb_settext(w);
    ctl_set_text(w, s ? s : "");
    if (w->ctl && !strcmp(w->cls, "#6")) ef(w)->cur = ef(w)->an = -1;
    repaint(w);
    return 1;
}
static uint32_t api_WinQueryDlgItemText(void) /* (hwndDlg, idItem, lMaxText, pszText) */
{
    pmwin_t *w = dlg_item(A(0), A(1));
    uint32_t n = A(2), b = A(3);
    if (!w || !n || !ok(b, n)) return 0;
    int k = ctl_get_text(w, (char *)C.mem + b, (int)n);
    if (k >= 0) return (uint32_t)k;
    uint32_t l = (uint32_t)strlen(w->text);
    if (l > n - 1) l = n - 1;
    memcpy(C.mem + b, w->text, l);
    C.mem[b + l] = 0;
    return l;
}
static uint32_t api_WinQueryDlgItemTextLength(void)
{
    pmwin_t *w = dlg_item(A(0), A(1));
    if (!w) return 0;
    static char tmp[65536];
    int k = ctl_get_text(w, tmp, sizeof tmp);
    return k >= 0 ? (uint32_t)k : (uint32_t)strlen(w->text);
}
static uint32_t api_WinQueryDlgItemShort(void)    /* (hwndDlg, idItem, pResult, fSigned) */
{
    pmwin_t *w = dlg_item(A(0), A(1));
    if (!w) return 0;
    char *e;
    long v = strtol(w->text, &e, 10);
    if (e == w->text || (!A(3) && v < 0) || v > 65535 || v < -32768) return 0;
    return p16(A(2), (uint16_t)v);
}
static uint32_t api_WinSetDlgItemShort(void)      /* (hwndDlg, idItem, usValue, fSigned) */
{
    pmwin_t *w = dlg_item(A(0), A(1));
    if (!w) return 0;
    if (A(3)) snprintf(w->text, sizeof w->text, "%d", (int16_t)A(2)); else snprintf(w->text, sizeof w->text, "%u", A(2) & 0xFFFF);
    repaint(w);
    return 1;
}
static uint32_t api_WinEnableWindow(void)
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    if (A(1)) w->style &= ~WS_DISABLED_; else w->style |= WS_DISABLED_;
    repaint(w);
    return 1;
}
static uint32_t api_WinIsWindowEnabled(void) { pmwin_t *w = win(A(0)); return w && !(w->style & WS_DISABLED_); }
static uint32_t api_WinIsWindowVisible(void)
{
    pmwin_t *w = win(A(0));
    for (pmwin_t *p = w; p; p = p->parent) if (!p->visible) return 0;
    return w != 0;
}
static uint32_t api_WinEnumDlgItem(void)          /* (hwndDlg, hwnd, code) */
{
    pmwin_t *d = win(A(0)), *w = win(A(1));
    if (!d) return 0;
    uint32_t c = A(2);
    pmwin_t *r = c == 0 ? dlg_tab(d, 0, 1) : c == 1 ? dlg_tab(d, 0, -1) : c == 2 ? dlg_tab(d, w, 1) : c == 3 ? dlg_tab(d, w, -1) : w;
    return r ? r->hwnd : 0;
}
static uint32_t api_WinMapDlgPoints(void)         /* (hwndDlg, prgwptl, cwpt, fCalcWindowCoords) */
{
    uint32_t p = A(1), n = A(2);
    if (!ok(p, n * 8)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = (int32_t)g32(p + 8 * i), y = (int32_t)g32(p + 8 * i + 4);
        if (A(3)) { x = du_x(x); y = du_y(y); } else { x = x * 4 / 7; y = y / 2; }
        p32(p + 8 * i, (uint32_t)x); p32(p + 8 * i + 4, (uint32_t)y);
    }
    return 1;
}
static uint32_t api_WinQuerySysColor(void) { return pm_rgb((int32_t)A(1), 0, 1); }
static uint32_t api_WinSetActiveWindow(void) { return 1; }
static uint32_t api_WinQueryActiveWindow(void) { return pm_last_shown ? pm_last_shown->hwnd : 0; }
static uint32_t api_WinSetOwner(void) { pmwin_t *w = win(A(0)); if (!w) return 0; w->owner = win(A(1)); return 1; }
static uint32_t api_WinSetParent(void) { pmwin_t *w = win(A(0)), *p = win(A(1)); if (!w || w->top) return 0; if (p) w->parent = p; invalidate(w); return 1; }
static uint32_t api_WinEnableWindowUpdate(void) { return 1; }
static uint32_t api_WinLoadMenu(void)             /* (hwndFrame, hmod, idMenu) */
{
    pmwin_t *f = win(A(0));
    if (!f) return 0;
    pmwin_t *bar = menu_load(f, A(1), A(2));
    if (bar) invalidate(f);
    return bar ? bar->hwnd : 0;
}

/* string tables: bundles of 16 (RT_STRING, name id / 16 + 1): a code page,
 * then 16 strings, each its length (with the NUL) and itself */
static uint32_t load_string(uint32_t type, uint32_t hmod, uint32_t id, uint32_t cch, uint32_t buf)
{
    uint32_t a, n;
    if (!cch || !ok(buf, cch)) return 0;
    C.mem[buf] = 0;
    if (!find_resource(hmod, type, id / 16 + 1, &a, &n)) return 0;
    uint32_t p = a + 2, end = a + n;
    for (uint32_t i = 0; i < 16 && p < end; i++) {
        uint32_t l = C.mem[p];
        if (i == id % 16) {
            uint32_t k = l ? l - 1 : 0;
            if (k > cch - 1) k = cch - 1;
            if (!ok(p + 1, k + 1)) return 0;
            memcpy(C.mem + buf, C.mem + p + 1, k);
            C.mem[buf + k] = 0;
            return k;
        }
        p += 1 + l;
    }
    return 0;
}
static uint32_t api_WinLoadString(void) { return load_string(5, A(1), A(2), A(3), A(4)); }    /* (hab, hmod, id, cchMax, psz) */
static uint32_t api_WinLoadMessage(void) { return load_string(10, A(1), A(2), A(3), A(4)); }

/* ── WC_SCROLLBAR ────────────────────────────────────────────────────── */
typedef struct { int pos, lo, hi, vis, tot, drag, grab; } sb_t;
static sb_t *sbs(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(sb_t)); ((sb_t *)w->ctl)->hi = 100; } return w->ctl; }
/* (pm_capture: the window that has the mouse - a scroll bar's thumb, dragged) */
/* along the bar: its length, the arrows' size, the thumb's [t0, t1) from the start (left, or top) */
static void sb_geom(pmwin_t *w, sb_t *s, int *len, int *arrow, int *t0, int *t1)
{
    int vert = w->style & SBS_VERT;
    *len = vert ? w->cy : w->cx;
    *arrow = vert ? w->cx : w->cy;
    if (*arrow * 2 + 8 > *len) *arrow = (*len - 8) / 2;
    int track = *len - 2 * *arrow, range = s->hi - s->lo;
    int th = s->tot > 0 && s->vis > 0 ? track * s->vis / s->tot : track / 8;
    if (th < 8) th = 8;
    if (th > track) th = track;
    int at = range > 0 ? (track - th) * (s->pos - s->lo) / range : 0;
    *t0 = *arrow + at; *t1 = *t0 + th;
}
static void sb_notify(pmwin_t *w, int cmd, int pos)
{
    pmwin_t *o = w->owner ? w->owner : w->parent;
    if (o) pm_send(o, (w->style & SBS_VERT) ? WM_VSCROLL : WM_HSCROLL, w->id & 0xFFFF, (uint32_t)(pos & 0xFFFF) | (uint32_t)cmd << 16);
}
static void tri(pmwin_t *w, int cx, int cy, int dir)          /* a small arrow: 0 up, 1 down, 2 left, 3 right */
{
    for (int k = 0; k < 4; k++)
        for (int j = -k; j <= k; j++) {
            if (dir == 0) pel(w, cx + j, cy + 2 - k, C_TEXT);
            else if (dir == 1) pel(w, cx + j, cy - 2 + k, C_TEXT);
            else if (dir == 2) pel(w, cx - 2 + k, cy + j, C_TEXT);
            else pel(w, cx + 2 - k, cy + j, C_TEXT);
        }
}
static uint32_t scroll_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    sb_t *s = sbs(w);
    int vert = w->style & SBS_VERT, len, ar, t0, t1;
    sb_geom(w, s, &len, &ar, &t0, &t1);
    switch (msg) {
    case WM_PAINT:
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, 0xE0E0E0);
        if (vert) {
            fill(w, 0, w->cy - ar, w->cx, w->cy, C_FACE); bevel(w, 0, w->cy - ar, w->cx, w->cy, 1); tri(w, w->cx / 2, w->cy - ar / 2, 0);
            fill(w, 0, 0, w->cx, ar, C_FACE); bevel(w, 0, 0, w->cx, ar, 1); tri(w, w->cx / 2, ar / 2, 1);
            if (s->hi > s->lo) { fill(w, 1, w->cy - t1, w->cx - 1, w->cy - t0, C_FACE); bevel(w, 1, w->cy - t1, w->cx - 1, w->cy - t0, 1); }
        } else {
            fill(w, 0, 0, ar, w->cy, C_FACE); bevel(w, 0, 0, ar, w->cy, 1); tri(w, ar / 2, w->cy / 2, 2);
            fill(w, w->cx - ar, 0, w->cx, w->cy, C_FACE); bevel(w, w->cx - ar, 0, w->cx, w->cy, 1); tri(w, w->cx - ar / 2, w->cy / 2, 3);
            if (s->hi > s->lo) { fill(w, t0, 1, t1, w->cy - 1, C_FACE); bevel(w, t0, 1, t1, w->cy - 1, 1); }
        }
        present(w);
        return 0;
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16), at = vert ? w->cy - 1 - y : x;   /* (from the top, or the left) */
        if (s->hi <= s->lo) return 1;
        if (at < ar) sb_notify(w, SB_LINEUP, s->pos);                        /* (left) */
        else if (at >= len - ar) sb_notify(w, SB_LINEDOWN, s->pos);            /* (right) */
        else if (at < t0) sb_notify(w, SB_PAGEUP, s->pos);
        else if (at >= t1) sb_notify(w, SB_PAGEDOWN, s->pos);
        else { s->drag = 1; s->grab = at - t0; pm_capture = w->hwnd; return 1; }
        sb_notify(w, SB_ENDSCROLL, s->pos);
        return 1;
    }
    case WM_MOUSEMOVE: case WM_BUTTON1UP: {
        if (!s->drag) return 1;
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16), at = (vert ? w->cy - 1 - y : x) - s->grab;
        int track = len - 2 * ar - (t1 - t0), range = s->hi - s->lo;
        int pos = track > 0 ? s->lo + (at - ar) * range / track : s->lo;
        if (pos < s->lo) pos = s->lo;
        if (pos > s->hi) pos = s->hi;
        if (msg == WM_BUTTON1UP) { s->drag = 0; pm_capture = 0; sb_notify(w, SB_SLIDERPOSITION, pos); sb_notify(w, SB_ENDSCROLL, pos); }
        else sb_notify(w, SB_SLIDERTRACK, pos);
        return 1;
    }
    case SBM_SETSCROLLBAR:                                           /* pos, (first, last) */
        s->lo = (int16_t)(mp2 & 0xFFFF); s->hi = (int16_t)(mp2 >> 16); s->pos = (int16_t)(mp1 & 0xFFFF);
        if (s->pos < s->lo) s->pos = s->lo;
        if (s->pos > s->hi) s->pos = s->hi;
        repaint(w);
        return 1;
    case SBM_SETPOS:
        s->pos = (int16_t)(mp1 & 0xFFFF);
        if (s->pos < s->lo) s->pos = s->lo;
        if (s->pos > s->hi) s->pos = s->hi;
        repaint(w);
        return 1;
    case SBM_QUERYPOS: return (uint32_t)(s->pos & 0xFFFF);
    case SBM_QUERYRANGE: return (uint32_t)(s->lo & 0xFFFF) | (uint32_t)(s->hi & 0xFFFF) << 16;
    case SBM_SETTHUMBSIZE: s->vis = (int)(mp1 & 0xFFFF); s->tot = (int)(mp1 >> 16); repaint(w); return 1;
    case WM_CHAR: {                                                  /* the arrows, the page keys; the rest to the owner */
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, cmd = 0;
        if (!(mp1 & KC_KEYUP)) {
            if (vk == (vert ? 0x16 : 0x15)) cmd = 1;
            else if (vk == (vert ? 0x18 : 0x17)) cmd = 2;
            else if (vk == VK_PAGEUP) cmd = 3;
            else if (vk == VK_PAGEDOWN) cmd = 4;
        }
        if (cmd) { sb_notify(w, cmd, s->pos); sb_notify(w, SB_ENDSCROLL, s->pos); return 1; }
        if (mp1 & KC_KEYUP) return 1;
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    }
    }
    (void)mp2;
    return 0;
}

/* ── WC_COMBOBOX: an entry field, a list box that drops down ─────────── */
typedef struct { uint32_t ef, lb; int listh, type; } cb_t;
static cb_t *cbx(pmwin_t *w) { if (!w->ctl) w->ctl = calloc(1, sizeof(cb_t)); return w->ctl; }
static int cb_open(pmwin_t *w) { cb_t *c = cbx(w); pmwin_t *l = win(c->lb), *f = frame_of(w); return l && f && f->drop == l; }
static void cb_show(pmwin_t *w, int on)
{
    cb_t *c = cbx(w);
    pmwin_t *l = win(c->lb), *f = frame_of(w);
    if (!l || !f) return;
    if (!on) { if (f->drop == l) menu_close(f); return; }
    if (f->drop) menu_close(f);
    int ax, ay;
    win_abs(w, &ax, &ay);
    l->x = ax; l->cx = w->cx; l->cy = c->listh; l->y = ay - c->listh;
    if (l->y < 0) l->y = 0;
    l->visible = 1;
    f->drop = l;
    invalidate(l);
    lb_show_sel(l, lb(l));
    notify(w, CBN_SHOWLIST, 0);
}
static void cb_settext(pmwin_t *w)                                   /* the combo's text into its entry field */
{
    pmwin_t *e = win(cbx(w)->ef);
    if (e) { snprintf(e->text, sizeof e->text, "%s", w->text); ef(e)->cur = ef(e)->an = -1; repaint(e); }
}
static uint32_t combo_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    cb_t *c = cbx(w);
    int bw = 18;
    switch (msg) {
    case WM_CREATE: {
        c->type = (w->style & CBS_DROPDOWNLIST) ? CBS_DROPDOWNLIST : CBS_DROPDOWN;   /* (CBS_SIMPLE: a drop-down too) */
        int eh = pm_lineh + 6;
        c->listh = w->cy - eh;
        if (c->listh < 4 * pm_lineh + 4) c->listh = 4 * pm_lineh + 4;
        w->y += w->cy - eh;
        w->cy = eh;
        pmwin_t *e = win_new(w, "#6", ES_MARGIN | (c->type == CBS_DROPDOWNLIST ? ES_READONLY : 0), CBID_EDIT);
        if (e) { e->x = 0; e->y = 0; e->cx = w->cx - bw; e->cy = eh; e->owner = w; e->visible = 1; ef(e)->ro = c->type == 4; c->ef = e->hwnd; }
        pmwin_t *f = frame_of(w) ? frame_of(w) : w->parent;
        pmwin_t *l = f ? win_new(f, "#7", 0, CBID_LIST) : 0;           /* over the rest, when it drops down */
        if (l) { l->overlay = 1; l->visible = 0; l->owner = w; l->cx = w->cx; l->cy = c->listh; c->lb = l->hwnd; }
        return 0;
    }
    case WM_DESTROY: { pmwin_t *l = win(c->lb); if (l) { if (frame_of(w) && frame_of(w)->drop == l) frame_of(w)->drop = 0; win_destroy(l); } return 0; }
    case WM_PAINT:
        w->invalid = 0;
        fill(w, w->cx - bw, 0, w->cx, w->cy, C_FACE);
        bevel(w, w->cx - bw, 0, w->cx, w->cy, !cb_open(w));
        tri(w, w->cx - bw / 2, w->cy / 2, 1);
        present(w);
        return 0;
    case WM_BUTTON1DOWN:
        cb_show(w, !cb_open(w));
        return 1;
    case WM_CONTROL: {
        uint32_t id = mp1 & 0xFFFF, code = mp1 >> 16;
        if (id == CBID_EDIT && code == EN_CHANGE) { pmwin_t *e = win(c->ef); if (e) snprintf(w->text, sizeof w->text, "%s", e->text); notify(w, CBN_EFCHANGE, 0); }
        if (id == CBID_LIST && code == LN_SELECT) {
            pmwin_t *l = win(c->lb);
            lb_t *b = l ? lb(l) : 0;
            if (b && b->sel >= 0) { snprintf(w->text, sizeof w->text, "%s", b->it[b->sel].text); cb_settext(w); }
            if (cb_open(w)) cb_show(w, 0);
            notify(w, CBN_LBSELECT, 0);
        }
        if (id == CBID_LIST && code == LN_ENTER) { cb_show(w, 0); notify(w, CBN_ENTER, 0); }
        return 0;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        pmwin_t *l = win(c->lb);
        if ((vk == VK_DOWN || vk == VK_UP) && l) {                       /* Down, Up: the next, the previous */
            if ((mp1 & KC_ALT) && vk == VK_DOWN) { cb_show(w, 1); return 1; }
            return list_proc(l, msg, mp1, mp2);
        }
        if ((vk == VK_NEWLINE || vk == VK_ENTER || (mp2 & 0xFFFF) == '\r') && cb_open(w)) { cb_show(w, 0); return 1; }
        if ((vk == VK_ESC || (mp2 & 0xFFFF) == 27) && cb_open(w)) { cb_show(w, 0); return 1; }
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    }
    case CBM_SHOWLIST: cb_show(w, (int)(mp1 & 1)); return 1;
    case CBM_ISLISTSHOWING: return (uint32_t)cb_open(w);
    case CBM_HILITE: return 1;
    }
    if (msg >= 0x0160 && msg <= 0x016f) { pmwin_t *l = win(c->lb); return l ? list_proc(l, msg, mp1, mp2) : 0; }   /* LM_*: the list's */
    if (msg >= 0x0140 && msg <= 0x014f) { pmwin_t *e = win(c->ef); return e ? entry_proc(e, msg, mp1, mp2) : 0; }  /* EM_*: the entry's */
    return 0;
}

/* ── accelerators ────────────────────────────────────────────────────── */
typedef struct { int used; int n; uint16_t a[64][3]; } accel_t;     /* (fs, key, cmd) */
static accel_t accels[32];
static uint32_t q_accel[MAXTHR];                                     /* the queues' (WinSetAccelTable(hab, h, 0)) */
static uint32_t accel_new(uint32_t t)                                /* from an ACCELTABLE at t */
{
    for (int i = 0; i < 32; i++)
        if (!accels[i].used) {
            accel_t *a = &accels[i];
            a->used = 1;
            a->n = g16(t) > 64 ? 64 : g16(t);
            for (int k = 0; k < a->n; k++) for (int j = 0; j < 3; j++) a->a[k][j] = g16(t + 4 + 6u * (uint32_t)k + 2u * (uint32_t)j);
            return 0xB000 + (uint32_t)i;
        }
    return 0;
}
static accel_t *accel_get(uint32_t h) { h -= 0xB000; return h < 32 && accels[h].used ? &accels[h] : 0; }
/* a key against a table: its command (0: none; AF_SYSCOMMAND's: | 0x10000) */
static uint32_t accel_match(accel_t *a, int ch, int vk, int scan, int kc)
{
    for (int k = 0; a && k < a->n; k++) {
        int fs = a->a[k][0], key = a->a[k][1];
        if (((fs & 0x10) != 0) != ((kc & KC_CTRL) != 0) || ((fs & 0x20) != 0) != ((kc & KC_ALT) != 0)) continue;
        int hit = 0;
        if (fs & 2) hit = vk && key == vk && ((fs & 8) != 0) == ((kc & KC_SHIFT) != 0);
        else if (fs & 4) hit = key == scan;
        else hit = ch && ((fs & 0x30) ? tolower(key) == tolower(ch) : key == ch);
        if (hit) return a->a[k][2] | ((fs & 0x100) ? 0x10000u : 0);
    }
    return 0;
}
static uint32_t api_WinLoadAccelTable(void)       /* (hab, hmod, idAccelTable) */
{
    uint32_t a, n;
    return find_resource(A(1), RT_ACCELTABLE, A(2), &a, &n) ? accel_new(a) : 0;
}
static uint32_t api_WinCreateAccelTable(void) { return ok(A(1), 4) ? accel_new(A(1)) : 0; }   /* (hab, pAccelTable) */
static uint32_t api_WinDestroyAccelTable(void) { accel_t *a = accel_get(A(0)); if (!a) return 0; a->used = 0; return 1; }
static uint32_t api_WinSetAccelTable(void)        /* (hab, haccel, hwndFrame) */
{
    pmwin_t *f = win(A(2));
    if (f) { f->haccel = A(1); return 1; }
    q_accel[me->tid - 1] = A(1);
    return 1;
}
static uint32_t api_WinQueryAccelTable(void) { pmwin_t *f = win(A(1)); return f ? f->haccel : q_accel[me->tid - 1]; }
static uint32_t api_WinTranslateAccel(void)       /* (hab, hwnd, haccel, pqmsg) */
{
    uint32_t q = A(3);
    if (g32(q + 4) != WM_CHAR) return 0;
    uint32_t mp1 = g32(q + 8), mp2 = g32(q + 12);
    if (mp1 & KC_KEYUP) return 0;
    uint32_t cmd = accel_match(accel_get(A(2)), (mp1 & KC_CHAR) ? (int)(mp2 & 0xFFFF) : 0, (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, (int)(mp1 >> 24), (int)(mp1 & 0x38));
    if (!cmd) return 0;
    p32(q, A(1));
    p32(q + 4, (cmd & 0x10000) ? WM_SYSCOMMAND : WM_COMMAND);
    p32(q + 8, cmd & 0xFFFF);
    p32(q + 12, CMDSRC_ACCELERATOR);
    return 1;
}

/* ── the menu by the keyboard: Alt+letter, F10, the arrows, Enter, Esc ── */
static int mnemonic(const char *s) { const char *t = strchr(s, '~'); return t && t[1] ? tolower((unsigned char)t[1]) : 0; }
static pmwin_t *menubar_of(pmwin_t *f)
{
    pmwin_t *b = win_child(f, FID_MENU);
    return b && b->iproc == menubar_proc ? b : 0;
}
static void drop_hot(pmwin_t *d, int dir)
{
    menu_t *m = d->ctl;
    if (!m || !m->n) return;
    int h = m->open;                                                 /* (a drop-down's open: its highlighted item) */
    for (int k = 0; k < m->n; k++) {
        h = (h + dir + m->n) % m->n;
        if (!(m->it[h].style & MIS_SEPARATOR)) break;
    }
    m->open = h;
    repaint(d);
}
/* 1: the menu took the key */
static int menu_key(pmwin_t *f, int ch, int vk, int kc)
{
    pmwin_t *bar = menubar_of(f);
    if (!bar || !bar->ctl) return 0;
    menu_t *m = bar->ctl;
    pmwin_t *d = f->drop && f->drop->iproc == drop_proc ? f->drop : 0;
    if (d) {
        menu_t *s = d->ctl;
        if (vk == VK_ESC || ch == 27) { menu_close(f); return 1; }
        if (vk == VK_DOWN) { drop_hot(d, 1); return 1; }
        if (vk == VK_UP) { drop_hot(d, -1); return 1; }
        if (vk == VK_LEFT || vk == VK_RIGHT) {                              /* Left, Right: the next menu */
            int i = m->open;
            for (int k = 0; k < m->n; k++) { i = (i + (vk == VK_RIGHT ? 1 : -1) + m->n) % m->n; if (m->it[i].sub) break; }
            menu_open(bar, m, i);
            if (f->drop) { ((menu_t *)f->drop->ctl)->open = -1; drop_hot(f->drop, 1); }
            return 1;
        }
        if ((vk == VK_NEWLINE || vk == VK_ENTER || ch == '\r') && s && s->open >= 0 && s->open < s->n) { menu_command(f, &s->it[s->open]); return 1; }
        for (int k = 0; s && ch && k < s->n; k++)
            if (mnemonic(s->it[k].text) == tolower(ch)) { menu_command(f, &s->it[k]); return 1; }
        return 1;                                                    /* (the menu has the keys while it's open) */
    }
    int top = -1;
    if ((kc & KC_ALT) && ch) for (int k = 0; k < m->n; k++) if (mnemonic(m->it[k].text) == tolower(ch)) top = k;
    if (vk == VK_F10 && !(kc & (KC_ALT | KC_CTRL | KC_SHIFT))) top = 0;   /* F10 */
    if (top < 0) return 0;
    if (m->it[top].sub) { menu_open(bar, m, top); if (f->drop) { ((menu_t *)f->drop->ctl)->open = -1; drop_hot(f->drop, 1); } }
    else menu_command(f, &m->it[top]);
    return 1;
}
/* before a key becomes WM_CHAR: the menu's, an accelerator's (pm_key) */
static int help_key(pmwin_t *f);
static int key_hook(pmwin_t *f, int ch, int vk, int scan, int kc)
{
    if (menu_key(f, ch, vk, kc)) return 1;
    uint32_t cmd = accel_match(accel_get(f->haccel), ch, vk, scan, kc);
    if (!cmd) cmd = accel_match(accel_get(q_accel[f->tid - 1]), ch, vk, scan, kc);
    if (!cmd && vk == VK_F1 && !(kc & (KC_ALT | KC_CTRL | KC_SHIFT | KC_KEYUP))) return help_key(f);
    if (!cmd) return 0;
    pmwin_t *t = f->client ? f->client : f;
    post(t, t->hwnd, (cmd & 0x10000) ? WM_SYSCOMMAND : WM_COMMAND, cmd & 0xFFFF, CMDSRC_ACCELERATOR);
    return 1;
}

/* ── WC_MLE: text of many lines (LF inside; CR LF out, as MLFIE_CFTEXT) ─ */
typedef struct { char *t; int len, cap, cur, anchor, top, limit, ro, changed, fmt; uint32_t iebuf, iecb; uint32_t vsb; } mle_t;
static mle_t *mle(pmwin_t *w)
{
    if (!w->ctl) {
        mle_t *m = calloc(1, sizeof *m);
        m->cap = 256; m->t = calloc(1, (size_t)m->cap); m->limit = 4096; m->fmt = 0;
        w->ctl = m;
    }
    return w->ctl;
}
static void mle_room(mle_t *m, int n) { if (m->len + n + 1 > m->cap) { while (m->len + n + 1 > m->cap) m->cap *= 2; m->t = realloc(m->t, (size_t)m->cap); } }
static int mle_line_of(mle_t *m, int at) { int l = 0; for (int i = 0; i < at && i < m->len; i++) if (m->t[i] == '\n') l++; return l; }
static int mle_line_start(mle_t *m, int line) { int at = 0; for (int l = 0; l < line && at < m->len; at++) if (m->t[at] == '\n') l++; return at; }
static int mle_line_end(mle_t *m, int at) { while (at < m->len && m->t[at] != '\n') at++; return at; }
static int mle_lines(mle_t *m) { return mle_line_of(m, m->len) + 1; }
static int mle_rows(pmwin_t *w) { int r = (w->cy - 4) / pm_lineh; return r < 1 ? 1 : r; }
/* text in, as a program gives it: CR LF and CR become LF */
static int mle_insert(pmwin_t *w, int at, const char *s, int n)
{
    mle_t *m = mle(w);
    char *tmp = malloc((size_t)n + 1);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\r') { if (i + 1 < n && s[i + 1] == '\n') continue; tmp[k++] = '\n'; }
        else tmp[k++] = s[i];
    }
    if (m->len + k > m->limit) k = m->limit - m->len;
    if (k < 0) k = 0;
    mle_room(m, k);
    memmove(m->t + at + k, m->t + at, (size_t)(m->len - at + 1));
    memcpy(m->t + at, tmp, (size_t)k);
    m->len += k;
    free(tmp);
    if (k) { m->changed = 1; notify(w, MLN_CHANGE, 0); }
    return k;
}
static void mle_delete(pmwin_t *w, int at, int n)
{
    mle_t *m = mle(w);
    if (at < 0) at = 0;
    if (at + n > m->len) n = m->len - at;
    if (n <= 0) return;
    memmove(m->t + at, m->t + at + n, (size_t)(m->len - at - n + 1));
    m->len -= n;
    m->changed = 1;
    notify(w, MLN_CHANGE, 0);
}
static int mle_del_sel(pmwin_t *w)
{
    mle_t *m = mle(w);
    if (m->anchor == m->cur) return 0;
    int a = m->anchor < m->cur ? m->anchor : m->cur, b = m->anchor < m->cur ? m->cur : m->anchor;
    mle_delete(w, a, b - a);
    m->cur = m->anchor = a;
    return 1;
}
/* out, as the format says: CR LF (MLFIE_CFTEXT, MLFIE_WINFMT) or as it is (MLFIE_NOTRANS) */
static int mle_export(mle_t *m, int from, int n, char *out, int max)
{
    int k = 0;
    for (int i = from; i < from + n && i < m->len && k < max; i++) {
        if (m->t[i] == '\n' && m->fmt != 1) { if (k + 2 > max) break; out[k++] = '\r'; }
        out[k++] = m->t[i];
    }
    return k;
}
static void mle_scroll_to_cursor(pmwin_t *w, mle_t *m)
{
    int l = mle_line_of(m, m->cur), rows = mle_rows(w);
    if (l < m->top) m->top = l;
    if (l >= m->top + rows) m->top = l - rows + 1;
    pmwin_t *s = win(m->vsb);
    if (s) {
        int hi = mle_lines(m) - rows;
        if (hi < 0) hi = 0;
        scroll_proc(s, SBM_SETSCROLLBAR, (uint32_t)m->top, (uint32_t)hi << 16);
        scroll_proc(s, SBM_SETTHUMBSIZE, (uint32_t)rows | (uint32_t)mle_lines(m) << 16, 0);
    }
}
static uint32_t mle_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    mle_t *m = mle(w);
    switch (msg) {
    case WM_CREATE:
        if (w->style & MLS_READONLY) m->ro = 1;
        if (w->style & MLS_VSCROLL) {                         /* a scroll bar of its own */
            pmwin_t *s = win_new(w, "#8", 1, 0);
            if (s) { s->x = w->cx - SB_W; s->y = 0; s->cx = SB_W; s->cy = w->cy; s->owner = w; s->visible = 1; m->vsb = s->hwnd; }
        }
        return 0;
    case WM_DESTROY: free(m->t); m->t = 0; return 0;
    case WM_PAINT: {
        w->invalid = 0;
        int sbw = m->vsb ? SB_W : 0, b = (w->style & MLS_BORDER) ? 1 : 0;
        fill(w, 0, 0, w->cx - sbw, w->cy, (w->style & WS_DISABLED_) ? C_FACE : C_LIGHT);
        if (b) bevel(w, 0, 0, w->cx - sbw, w->cy, 0);
        int rows = mle_rows(w), at = mle_line_start(m, m->top);
        int s0 = m->anchor < m->cur ? m->anchor : m->cur, s1 = m->anchor < m->cur ? m->cur : m->anchor;
        for (int r = 0; r < rows && at <= m->len; r++) {
            int e = mle_line_end(m, at), ybase = w->cy - 2 - (r + 1) * pm_lineh + pm_descent + (pm_lineh - pm_ascent - pm_descent) / 2;
            int x = 3;
            for (int i = at; i < e; i++) {
                char ch = m->t[i] == '\t' ? ' ' : m->t[i];
                int cw = glyph((unsigned char)ch)->adv * (m->t[i] == '\t' ? 4 : 1);
                if (i >= s0 && i < s1) fill(w, x, ybase - pm_descent, x + cw, ybase + pm_ascent, C_HILITE);
                text_at(w, x, ybase, &ch, 1, i >= s0 && i < s1 ? C_LIGHT : C_TEXT);
                if (m->t[i] == '\t') x += cw - glyph(' ')->adv;
                x += glyph((unsigned char)ch)->adv;
            }
            if (pm_focus == w->hwnd && !m->ro && m->cur >= at && m->cur <= e) {
                int cx = 3;
                for (int i = at; i < m->cur; i++) cx += m->t[i] == '\t' ? glyph(' ')->adv * 4 : glyph((unsigned char)m->t[i])->adv;
                line(w, cx, ybase - pm_descent, cx, ybase + pm_ascent, C_TEXT);
            }
            if (e >= m->len) break;
            at = e + 1;
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16);
        int l = m->top + (w->cy - 2 - y) / pm_lineh, nl = mle_lines(m);
        if (l >= nl) l = nl - 1;
        int at = mle_line_start(m, l), e = mle_line_end(m, at), cx = 3;
        while (at < e) { int cw = glyph((unsigned char)m->t[at])->adv; if (cx + cw / 2 > x) break; cx += cw; at++; }
        m->cur = m->anchor = at;
        repaint(w);
        return 1;
    }
    case WM_VSCROLL: {                                        /* (its scroll bar's) */
        int cmd = (int)(mp2 >> 16), rows = mle_rows(w), hi = mle_lines(m) - rows;
        if (hi < 0) hi = 0;
        if (cmd == 1) m->top--; else if (cmd == 2) m->top++; else if (cmd == 3) m->top -= rows; else if (cmd == 4) m->top += rows;
        else if (cmd == 5 || cmd == 6) m->top = (int16_t)(mp2 & 0xFFFF);
        else return 0;
        if (m->top > hi) m->top = hi;
        if (m->top < 0) m->top = 0;
        pmwin_t *s = win(m->vsb);
        if (s) scroll_proc(s, SBM_SETPOS, (uint32_t)m->top, 0);
        repaint(w);
        return 0;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int ck = clip_key(mp1, mp2);
        if (ck) { mle_proc(w, ck == 1 ? MLM_CUT : ck == 2 ? MLM_COPY : MLM_PASTE, 0, 0); return 1; }
        int ch = (mp1 & KC_CHAR) ? (int)(mp2 & 0xFFFF) : 0, vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        int shift = (mp1 & KC_SHIFT) != 0, ctrl = (mp1 & KC_CTRL) != 0, moved = 0;
        int l = mle_line_of(m, m->cur), col = m->cur - mle_line_start(m, l);
        switch (vk) {
        case VK_LEFT: if (m->cur > 0) m->cur--; moved = 1; break;
        case VK_RIGHT: if (m->cur < m->len) m->cur++; moved = 1; break;
        case VK_UP: case VK_DOWN: case VK_PAGEUP: case VK_PAGEDOWN: {
            int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PAGEUP ? -mle_rows(w) : mle_rows(w), nl = l + d;
            if (nl < 0) nl = 0;
            if (nl >= mle_lines(m)) nl = mle_lines(m) - 1;
            int s = mle_line_start(m, nl), e = mle_line_end(m, s);
            m->cur = s + col < e ? s + col : e;
            moved = 1;
            break;
        }
        case VK_HOME: m->cur = ctrl ? 0 : mle_line_start(m, l); moved = 1; break;
        case VK_END: m->cur = ctrl ? m->len : mle_line_end(m, m->cur); moved = 1; break;
        case VK_BACKSPACE: if (!m->ro && !mle_del_sel(w) && m->cur > 0) { mle_delete(w, m->cur - 1, 1); m->cur--; } m->anchor = m->cur; break;
        case VK_DELETE: if (!m->ro && !mle_del_sel(w)) mle_delete(w, m->cur, 1); m->anchor = m->cur; break;
        case VK_NEWLINE: case VK_ENTER: if (!m->ro) { mle_del_sel(w); m->cur += mle_insert(w, m->cur, "\n", 1); m->anchor = m->cur; } break;
        case VK_TAB:                                            /* Tab: a tab, unless MLS_IGNORETAB */
            if ((w->style & MLS_IGNORETAB) || m->ro) return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
            mle_del_sel(w); m->cur += mle_insert(w, m->cur, "\t", 1); m->anchor = m->cur;
            break;
        case VK_ESC: return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);   /* Esc: the dialog's */
        default:
            if (ch == '\r') { if (!m->ro) { mle_del_sel(w); m->cur += mle_insert(w, m->cur, "\n", 1); m->anchor = m->cur; } break; }
            if (ch == 8) { if (!m->ro && !mle_del_sel(w) && m->cur > 0) { mle_delete(w, m->cur - 1, 1); m->cur--; } m->anchor = m->cur; break; }
            if (ch == 27) return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
            if (ch >= 32 && ch != 127 && !(mp1 & (KC_CTRL | KC_ALT))) {
                if (!m->ro) { char c = (char)ch; mle_del_sel(w); m->cur += mle_insert(w, m->cur, &c, 1); m->anchor = m->cur; }
                break;
            }
            return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        }
        if (moved && !shift) m->anchor = m->cur;
        mle_scroll_to_cursor(w, m);
        repaint(w);
        return 1;
    }
    case MLM_SETTEXTLIMIT: m->limit = (int)mp1 > 0 ? (int)mp1 : 0x7FFFFFFF; return 1;
    case MLM_QUERYTEXTLIMIT: return (uint32_t)m->limit;
    case MLM_QUERYTEXTLENGTH: return (uint32_t)m->len;
    case MLM_QUERYFORMATTEXTLENGTH: {                                                /* (ipt, cch) */
        int from = (int)mp1, n = (int32_t)mp2 < 0 ? m->len : (int)mp2, k = 0;
        for (int i = from; i < from + n && i < m->len; i++) k += m->t[i] == '\n' && m->fmt != 1 ? 2 : 1;
        return (uint32_t)k;
    }
    case MLM_FORMAT: m->fmt = (int)mp1; return 1;
    case MLM_SETIMPORTEXPORT: m->iebuf = mp1; m->iecb = mp2; return 1;
    case MLM_QUERYIMPORTEXPORT: p32(mp1, m->iebuf); p32(mp2, m->iecb); return 1;
    case MLM_IMPORT: {                                                               /* (pipt, cb) */
        int at = (int32_t)g32(mp1), n = (int)mp2;
        if (at < 0 || at > m->len) at = m->len;
        char *s = gp(m->iebuf, (uint32_t)n);
        if (!s) return 0;
        int k = mle_insert(w, at, s, n);
        p32(mp1, (uint32_t)(at + k));
        repaint(w);
        return (uint32_t)n;
    }
    case MLM_EXPORT: {                                                               /* (pipt, pcb) */
        int at = (int32_t)g32(mp1), n = (int)g32(mp2), max = (int)m->iecb;
        char *b = gp(m->iebuf, m->iecb);
        if (!b || at < 0) return 0;
        int k = mle_export(m, at, n, b, max < n ? max : n), used = 0;
        for (int i = at; used < k && i < m->len; i++) { used += m->t[i] == '\n' && m->fmt != 1 ? 2 : 1; p32(mp1, (uint32_t)(i + 1)); }
        p32(mp2, (uint32_t)(n - k));
        return (uint32_t)k;
    }
    case MLM_INSERT: {                                                               /* psz */
        char *s = gs(mp1);
        if (!s || m->ro) return 0;
        mle_del_sel(w);
        int k = mle_insert(w, m->cur, s, (int)strlen(s));
        m->cur += k; m->anchor = m->cur;
        repaint(w);
        return (uint32_t)k;
    }
    case MLM_DELETE: mle_delete(w, (int)mp1, (int)mp2); if (m->cur > m->len) m->cur = m->len; m->anchor = m->cur; repaint(w); return (uint32_t)mp2;
    case MLM_SETSEL: {                                                               /* anchor, cursor */
        int a = (int32_t)mp1, c = (int32_t)mp2;
        m->anchor = a < 0 || a > m->len ? m->len : a;
        m->cur = c < 0 || c > m->len ? m->len : c;
        mle_scroll_to_cursor(w, m);
        repaint(w);
        return 1;
    }
    case MLM_QUERYSEL: {
        int lo = m->anchor < m->cur ? m->anchor : m->cur, hi = m->anchor < m->cur ? m->cur : m->anchor;
        switch (mp1) {
        case 1: return (uint32_t)lo;
        case 2: return (uint32_t)hi;
        case 3: return (uint32_t)m->anchor;
        case 4: return (uint32_t)m->cur;
        default: return (uint32_t)(lo & 0xFFFF) | (uint32_t)hi << 16;
        }
    }
    case MLM_QUERYSELTEXT: {                                                         /* buffer */
        int lo = m->anchor < m->cur ? m->anchor : m->cur, hi = m->anchor < m->cur ? m->cur : m->anchor;
        char *b = gp(mp1, (uint32_t)(2 * (hi - lo) + 1));
        if (!b) return 0;
        int k = mle_export(m, lo, hi - lo, b, 2 * (hi - lo));
        b[k] = 0;
        return (uint32_t)k;
    }
    case MLM_QUERYLINECOUNT: return (uint32_t)mle_lines(m);
    case MLM_CHARFROMLINE: return (int32_t)mp1 < 0 ? (uint32_t)mle_line_start(m, mle_line_of(m, m->cur)) : (uint32_t)mle_line_start(m, (int)mp1);
    case MLM_LINEFROMCHAR: return (uint32_t)mle_line_of(m, (int32_t)mp1 < 0 ? m->cur : (int)mp1);
    case MLM_QUERYLINELENGTH: {
        int at = (int32_t)mp1 < 0 ? m->cur : (int)mp1, s = mle_line_start(m, mle_line_of(m, at));
        return (uint32_t)(mle_line_end(m, s) - s);
    }
    case MLM_SETREADONLY: { int o = m->ro; m->ro = (int)(mp1 & 1); return (uint32_t)o; }
    case MLM_QUERYREADONLY: return (uint32_t)m->ro;
    case MLM_QUERYCHANGED: return (uint32_t)m->changed;
    case MLM_SETCHANGED: { int o = m->changed; m->changed = (int)(mp1 & 1); return (uint32_t)o; }
    case MLM_QUERYFIRSTCHAR: return (uint32_t)mle_line_start(m, m->top);
    case MLM_SETFIRSTCHAR: m->top = mle_line_of(m, (int)mp1); repaint(w); return 1;
    case MLM_CLEAR: if (!m->ro) { mle_del_sel(w); repaint(w); } return 1;
    case MLM_CUT: case MLM_COPY: {                                                   /* the selection, to the clipboard */
        int lo = m->anchor < m->cur ? m->anchor : m->cur, hi = m->anchor < m->cur ? m->cur : m->anchor;
        if (hi > lo) {
            char *b = malloc((size_t)(2 * (hi - lo) + 1));
            int k = mle_export(m, lo, hi - lo, b, 2 * (hi - lo));
            b[k] = 0;
            clip_put(b);
            free(b);
        }
        if (msg == MLM_CUT && !m->ro) { mle_del_sel(w); repaint(w); }
        return 1;
    }
    case MLM_PASTE: {
        const char *t = clip_take();
        if (t && !m->ro) { mle_del_sel(w); m->cur += mle_insert(w, m->cur, t, (int)strlen(t)); m->anchor = m->cur; mle_scroll_to_cursor(w, m); repaint(w); }
        return 1;
    }
    case MLM_DISABLEREFRESH: case MLM_ENABLEREFRESH: case MLM_RESETUNDO: case MLM_SETWRAP: case MLM_SETTABSTOP:
    case MLM_SETTEXTCOLOR: case MLM_SETBACKCOLOR: return 1;
    case MLM_QUERYUNDO: case MLM_UNDO: return 0;                                     /* (no undo yet) */
    case MLM_QUERYWRAP: return (w->style & MLS_WORDWRAP) != 0;
    case MLM_QUERYTABSTOP: return 32;
    }
    return 0;
}

/* ── WC_SPINBUTTON: a number in a range, or one of an array ──────────── */
typedef struct { int32_t lo, hi, val; char **arr; int narr; } spb_t;
static spb_t *spb(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(spb_t)); ((spb_t *)w->ctl)->hi = 100; } return w->ctl; }
static void spb_text(pmwin_t *w, spb_t *s)
{
    if (s->narr) snprintf(w->text, sizeof w->text, "%s", s->val >= 0 && s->val < s->narr ? s->arr[s->val] : "");
    else snprintf(w->text, sizeof w->text, "%d", s->val);
}
static void spb_spin(pmwin_t *w, int by)
{
    spb_t *s = spb(w);
    int32_t lo = s->narr ? 0 : s->lo, hi = s->narr ? s->narr - 1 : s->hi;
    int32_t v = s->val + by;
    if (v > hi) v = lo + (v - hi - 1);                        /* (around, as PM spins) */
    if (v < lo) v = hi - (lo - v - 1);
    if (v != s->val) {
        s->val = v;
        spb_text(w, s);
        repaint(w);
        notify(w, by > 0 ? SPBN_UPARROW : SPBN_DOWNARROW, 0);
        notify(w, SPBN_CHANGE, 0);
    }
}
static uint32_t spin_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    spb_t *s = spb(w);
    int bw = 16;
    switch (msg) {
    case WM_CREATE:
        if (mp1 && ok(mp1, 16)) { s->lo = (int32_t)g32(mp1 + 8); s->hi = (int32_t)g32(mp1 + 12); }   /* SPBCDATA */
        s->val = s->lo;
        spb_text(w, s);
        return 0;
    case WM_DESTROY: for (int i = 0; i < s->narr; i++) free(s->arr[i]); free(s->arr); return 0;
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx - bw, w->cy, C_LIGHT);
        if (!(w->style & SPBS_NOBORDER)) bevel(w, 0, 0, w->cx - bw, w->cy, 0);
        int just = (int)(w->style & SPBS_JUSTCENTER), tw = plain_w(w->text);
        int x = just == 0x04 ? w->cx - bw - 4 - tw : just == 0x0C ? (w->cx - bw - tw) / 2 : 4;
        text_mid(w, x, 0, w->cy, w->text, C_TEXT);
        if (pm_focus == w->hwnd) for (int k = x; k < x + tw; k += 2) pel(w, k, 2, C_TEXT);
        int h = w->cy / 2;
        fill(w, w->cx - bw, h, w->cx, w->cy, C_FACE); bevel(w, w->cx - bw, h, w->cx, w->cy, 1); tri(w, w->cx - bw / 2, h + (w->cy - h) / 2, 0);
        fill(w, w->cx - bw, 0, w->cx, h, C_FACE); bevel(w, w->cx - bw, 0, w->cx, h, 1); tri(w, w->cx - bw / 2, h / 2, 1);
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16);
        if (x >= w->cx - bw) { spb_spin(w, y >= w->cy / 2 ? 1 : -1); notify(w, SPBN_ENDSPIN, 0); }
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, ch = (mp1 & KC_CHAR) ? (int)(mp2 & 0xFFFF) : 0;
        if (vk == VK_UP) { spb_spin(w, 1); return 1; }
        if (vk == VK_DOWN) { spb_spin(w, -1); return 1; }
        if (vk == VK_PAGEUP) { spb_spin(w, 10); return 1; }
        if (vk == VK_PAGEDOWN) { spb_spin(w, -10); return 1; }
        if (!s->narr && !(w->style & SPBS_READONLY) && ((ch >= '0' && ch <= '9') || ch == '-' || ch == '\b')) {   /* (typed) */
            int n = (int)strlen(w->text);
            if (ch == 8) { if (n) w->text[n - 1] = 0; }
            else if (n < 11) { w->text[n] = (char)ch; w->text[n + 1] = 0; }
            s->val = (int32_t)strtol(w->text, 0, 10);
            repaint(w);
            notify(w, SPBN_CHANGE, 0);
            return 1;
        }
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    }
    case SPBM_SETLIMITS:                                                              /* upper, lower */
        s->hi = (int32_t)mp1; s->lo = (int32_t)mp2;
        if (s->val < s->lo) s->val = s->lo;
        if (s->val > s->hi) s->val = s->hi;
        spb_text(w, s); repaint(w);
        return 1;
    case SPBM_OVERRIDESETLIMITS: s->hi = (int32_t)mp1; s->lo = (int32_t)mp2; return 1;
    case SPBM_QUERYLIMITS: p32(mp1, (uint32_t)s->hi); p32(mp2, (uint32_t)s->lo); return 1;
    case SPBM_SETCURRENTVALUE: s->val = (int32_t)mp1; spb_text(w, s); repaint(w); return 1;
    case SPBM_SETARRAY: {                                                             /* papsz, count */
        for (int i = 0; i < s->narr; i++) free(s->arr[i]);
        free(s->arr);
        s->narr = (int)(mp2 & 0xFFFF);
        s->arr = calloc((size_t)(s->narr ? s->narr : 1), sizeof *s->arr);
        for (int i = 0; i < s->narr; i++) { char *t = gs(g32(mp1 + 4u * (uint32_t)i)); s->arr[i] = strdup(t ? t : ""); }
        s->val = 0;
        spb_text(w, s); repaint(w);
        return 1;
    }
    case SPBM_QUERYVALUE: {                                                           /* pStore, (cch, flags) */
        uint32_t cch = mp2 & 0xFFFF;
        if (!mp1) return 1;
        if (!cch) return p32(mp1, (uint32_t)s->val);
        char *b = gp(mp1, cch);
        if (!b) return 0;
        snprintf(b, cch, "%s", w->text);
        return 1;
    }
    case SPBM_SPINUP: spb_spin(w, (int)(mp1 ? mp1 : 1)); return 1;
    case SPBM_SPINDOWN: spb_spin(w, -(int)(mp1 ? mp1 : 1)); return 1;
    case SPBM_SETTEXTLIMIT: case SPBM_SETMASTER: return 1;
    }
    return 0;
}

/* the window text of the controls that keep theirs elsewhere (an MLE's
 * is longer than w->text): -1 for the others */
static int ctl_get_text(pmwin_t *w, char *out, int max)
{
    if (strcmp(w->cls, "#10") || !w->ctl) return -1;
    mle_t *m = w->ctl;
    int k = mle_export(m, 0, m->len, out, max - 1);
    out[k] = 0;
    return k;
}
static int ctl_set_text(pmwin_t *w, const char *s)
{
    if (strcmp(w->cls, "#10")) return 0;
    mle_t *m = mle(w);
    m->len = 0; m->t[0] = 0; m->cur = m->anchor = m->top = 0;
    mle_insert(w, 0, s, (int)strlen(s));
    m->changed = 0;
    repaint(w);
    return 1;
}

/* ── WC_SLIDER: an arm along a shaft, in increments (SLDCDATA's) ──────── */
typedef struct { int n, pos, drag; uint8_t tick[256]; char *text[256]; } sld_t;
static sld_t *sld(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(sld_t)); ((sld_t *)w->ctl)->n = 11; } return w->ctl; }
static int sld_vert(pmwin_t *w) { return w->style & SLS_VERTICAL; }
/* along the shaft (from the left, or the bottom): where it starts and ends, the buttons' room */
enum { SLD_BTN = 16, SLD_BTNPAD = 2 };                    /* (its buttons: squares, two side by side, at one end) */
static int sld_btn0(pmwin_t *w)                          /* where along it the buttons begin; -1: none */
{
    if (!(w->style & (SLS_BUTTONSBOTTOM | SLS_BUTTONSTOP))) return -1;
    return (w->style & SLS_BUTTONSBOTTOM) ? SLD_BTNPAD : (sld_vert(w) ? w->cy : w->cx) - SLD_BTNPAD - 2 * SLD_BTN;
}
static void sld_geom(pmwin_t *w, int *a0, int *a1, int *mid)
{
    int len = sld_vert(w) ? w->cy : w->cx, across = sld_vert(w) ? w->cx : w->cy, btn = sld_btn0(w) >= 0 ? SLD_BTNPAD + 2 * SLD_BTN : 0;
    *a0 = 10 + ((w->style & SLS_BUTTONSBOTTOM) && !sld_vert(w) ? btn : 0);
    *a1 = len - 10 - ((w->style & SLS_BUTTONSTOP) || sld_vert(w) ? btn : 0);
    if (sld_vert(w) && (w->style & SLS_BUTTONSBOTTOM)) { *a0 = 10 + btn; *a1 = len - 10; }
    *mid = sld_vert(w) ? across / 2 : across - 12;          /* (horizontal: near the top, the scale below) */
    if (*a1 <= *a0) *a1 = *a0 + 1;
}
static int sld_at(pmwin_t *w, sld_t *s, int i) { int a0, a1, m; sld_geom(w, &a0, &a1, &m); return s->n > 1 ? a0 + i * (a1 - a0) / (s->n - 1) : a0; }
static void sld_set(pmwin_t *w, int pos, int code)
{
    sld_t *s = sld(w);
    if (pos < 0) pos = 0;
    if (pos > s->n - 1) pos = s->n - 1;
    if (pos == s->pos && code == SLN_CHANGE) return;
    s->pos = pos;
    repaint(w);
    notify(w, (uint32_t)code, (uint32_t)sld_at(w, s, pos));  /* (the arm's pixel) */
}
static uint32_t slider_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    sld_t *s = sld(w);
    int a0, a1, mid, vert = sld_vert(w);
    sld_geom(w, &a0, &a1, &mid);
    switch (msg) {
    case WM_CREATE:
        if (mp1 && ok(mp1, 8) && g16(mp1 + 4) >= 2) s->n = g16(mp1 + 4) > 256 ? 256 : g16(mp1 + 4);   /* usScale1Increments */
        if (w->style & SLS_HOMETOP) s->pos = 0;             /* (home at the right, the top: still 0 there) */
        return 0;
    case WM_DESTROY: for (int i = 0; i < 256; i++) free(s->text[i]); return 0;
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_FACE));
        uint32_t tc = pp_fgc(w, C_TEXT);
        /* the shaft, the ribbon to the arm, the ticks and their text */
        #define P(al, ac) (vert ? (ac) : (al)), (vert ? (al) : (ac))
        for (int k = a0; k <= a1; k++) { pel(w, P(k, mid + 2), C_DARK); pel(w, P(k, mid - 2), C_LIGHT); }
        for (int k = a0; k <= a1; k++) for (int j = -1; j <= 1; j++) pel(w, P(k, mid + j), (w->style & SLS_RIBBONSTRIP) && k <= sld_at(w, s, s->pos) ? C_HILITE : 0xE0E0E0);
        for (int i = 0; i < s->n; i++) {
            int at = sld_at(w, s, i);
            for (int j = 0; j < s->tick[i]; j++) pel(w, P(at, mid - 8 - j), tc);
            if (s->text[i]) {
                int tw = text_w(s->text[i], (int)strlen(s->text[i]));
                if (vert) text_at(w, mid + 12, at - pm_ascent / 2, s->text[i], (int)strlen(s->text[i]), tc);
                else text_at(w, at - tw / 2, mid - 10 - s->tick[i] - pm_ascent, s->text[i], (int)strlen(s->text[i]), tc);
            }
        }
        /* the arm */
        int at = sld_at(w, s, s->pos);
        int x0 = vert ? mid - 9 : at - 5, x1 = vert ? mid + 10 : at + 6, y0 = vert ? at - 5 : mid - 9, y1 = vert ? at + 6 : mid + 10;
        fill(w, x0, y0, x1, y1, C_FACE);
        bevel(w, x0, y0, x1, y1, 1);
        if (vert) line(w, x0 + 2, at, x1 - 3, at, C_DARK); else line(w, at, y0 + 2, at, y1 - 3, C_DARK);
        /* the buttons */
        int b0 = sld_btn0(w), h = SLD_BTN / 2;
        for (int k = 0; b0 >= 0 && k < 2; k++) {
            int s0 = b0 + SLD_BTN * k;
            if (vert) { fill(w, mid - h, s0, mid + h, s0 + SLD_BTN, C_FACE); bevel(w, mid - h, s0, mid + h, s0 + SLD_BTN, 1); tri(w, mid, s0 + h, k ? 0 : 1); }
            else { fill(w, s0, mid - h, s0 + SLD_BTN, mid + h, C_FACE); bevel(w, s0, mid - h, s0 + SLD_BTN, mid + h, 1); tri(w, s0 + h, mid, k ? 3 : 2); }
        }
        #undef P
        if (pm_focus == w->hwnd) for (int k = 1; k < (vert ? w->cy : w->cx) - 1; k += 2) pel(w, vert ? 1 : k, vert ? k : 1, C_DARK);
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        if (w->style & SLS_READONLY) return 1;
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16), al = vert ? y : x;
        int b0 = sld_btn0(w);                                /* the buttons: one increment */
        if (b0 >= 0 && al >= b0 && al < b0 + 2 * SLD_BTN) { sld_set(w, s->pos + (al < b0 + SLD_BTN ? -1 : 1), SLN_CHANGE); return 1; }
        int at = sld_at(w, s, s->pos);
        if (abs(al - at) <= 6) { s->drag = 1; pm_capture = w->hwnd; return 1; }
        int best = 0;
        for (int i = 1; i < s->n; i++) if (abs(sld_at(w, s, i) - al) < abs(sld_at(w, s, best) - al)) best = i;
        sld_set(w, best, SLN_CHANGE);
        return 1;
    }
    case WM_MOUSEMOVE: case WM_BUTTON1UP: {
        if (!s->drag) return 1;
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16), al = vert ? y : x, best = 0;
        for (int i = 1; i < s->n; i++) if (abs(sld_at(w, s, i) - al) < abs(sld_at(w, s, best) - al)) best = i;
        if (msg == WM_BUTTON1UP) { s->drag = 0; pm_capture = 0; s->pos = -1; sld_set(w, best, SLN_CHANGE); }   /* (the drag's end: a change, whatever) */
        else if (best != s->pos) sld_set(w, best, SLN_SLIDERTRACK);
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        if (!(w->style & SLS_READONLY)) {
            if (vk == VK_LEFT || vk == VK_DOWN) { sld_set(w, s->pos - 1, SLN_CHANGE); return 1; }
            if (vk == VK_RIGHT || vk == VK_UP) { sld_set(w, s->pos + 1, SLN_CHANGE); return 1; }
            if (vk == VK_HOME) { sld_set(w, 0, SLN_CHANGE); return 1; }
            if (vk == VK_END) { sld_set(w, s->n - 1, SLN_CHANGE); return 1; }
        }
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    }
    case SLM_SETSLIDERINFO: {                               /* (attribute, usage), value */
        uint32_t attr = mp1 & 0xFFFF, use = mp1 >> 16;
        if (attr == SMA_SLIDERARMPOSITION) {
            int pos = (int)mp2;
            if (use == 0) { int best = 0; for (int i = 1; i < s->n; i++) if (abs(sld_at(w, s, i) - a0 - (int)mp2) < abs(sld_at(w, s, best) - a0 - (int)mp2)) best = i; pos = best; }
            sld_set(w, pos, SLN_CHANGE);
        }
        return 1;
    }
    case SLM_QUERYSLIDERINFO: {
        uint32_t attr = mp1 & 0xFFFF, use = mp1 >> 16;
        if (attr == 3) return use == 1 ? (uint32_t)s->pos : (uint32_t)((sld_at(w, s, s->pos) - a0) & 0xFFFF) | (uint32_t)(a1 - a0) << 16;
        if (attr == SMA_SHAFTDIMENSIONS) return (uint32_t)(a1 - a0) | 6u << 16;
        if (attr == SMA_SHAFTPOSITION) return vert ? (uint32_t)mid | (uint32_t)a0 << 16 : (uint32_t)a0 | (uint32_t)mid << 16;
        if (attr == SMA_SLIDERARMDIMENSIONS) return vert ? 19u | 11u << 16 : 11u | 19u << 16;
        return 0;
    }
    case SLM_SETTICKSIZE: {                                 /* (tick or SMA_SETALLTICKS, size) */
        uint32_t t = mp1 & 0xFFFF, sz = (mp1 >> 16) > 30 ? 30 : mp1 >> 16;
        if (t == 0xFFFF) for (int i = 0; i < s->n; i++) s->tick[i] = (uint8_t)sz;
        else if ((int)t < s->n) s->tick[t] = (uint8_t)sz;
        repaint(w);
        return 1;
    }
    case SLM_QUERYTICKSIZE: return (mp1 & 0xFFFF) < (uint32_t)s->n ? s->tick[mp1 & 0xFFFF] : 0;
    case SLM_SETSCALETEXT: {                                /* tick, text */
        uint32_t t = mp1 & 0xFFFF;
        char *str = gs(mp2);
        if ((int)t >= s->n) return 0;
        free(s->text[t]);
        s->text[t] = str ? strdup(str) : 0;
        repaint(w);
        return 1;
    }
    case SLM_QUERYSCALETEXT: {                              /* (tick, cch), buffer */
        uint32_t t = mp1 & 0xFFFF, cch = mp1 >> 16;
        if ((int)t >= s->n || !cch || !ok(mp2, cch)) return 0;
        snprintf((char *)C.mem + mp2, cch, "%s", s->text[t] ? s->text[t] : "");
        return (uint32_t)strlen((char *)C.mem + mp2);
    }
    case SLM_QUERYTICKPOS: {                                /* tick, pptl */
        int t = (int)(mp1 & 0xFFFF);
        if (t >= s->n || !ok(mp2, 8)) return 0;
        p32(mp2, (uint32_t)(vert ? mid : sld_at(w, s, t))); p32(mp2 + 4, (uint32_t)(vert ? sld_at(w, s, t) : mid));
        return 1;
    }
    case SLM_ADDDETENT: case SLM_QUERYDETENTPOS: case SLM_REMOVEDETENT: return 0;   /* (detents: not yet) */
    }
    return 0;
}

/* ── WC_NOTEBOOK: pages, their tabs along the top, a status line ─────── */
typedef struct { uint32_t id, hwnd, data; uint16_t style; char tab[64], status[128]; } nbpage_t;
typedef struct { nbpage_t p[64]; int n, top; uint32_t next; } nb_t;
static nb_t *nb(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(nb_t)); ((nb_t *)w->ctl)->top = -1; ((nb_t *)w->ctl)->next = 0x100; } return w->ctl; }
static int nb_find(nb_t *b, uint32_t id) { for (int i = 0; i < b->n; i++) if (b->p[i].id == id) return i; return -1; }
static int nb_tab_h(void) { return pm_lineh + 8; }
static int nb_stat_h(void) { return pm_lineh + 6; }
static void nb_page_rect(pmwin_t *w, int *x, int *y, int *cx, int *cy)
{
    *x = 6; *y = nb_stat_h() + 4; *cx = w->cx - 12; *cy = w->cy - nb_tab_h() - nb_stat_h() - 8;
    if (*cx < 1) *cx = 1;
    if (*cy < 1) *cy = 1;
}
static void nb_layout(pmwin_t *w)
{
    nb_t *b = nb(w);
    int x, y, cx, cy;
    nb_page_rect(w, &x, &y, &cx, &cy);
    for (int i = 0; i < b->n; i++) {
        pmwin_t *p = win(b->p[i].hwnd);
        if (!p) continue;
        p->parent = w;
        p->x = x; p->y = y;
        if (b->p[i].style & BKA_AUTOPAGESIZE) { p->cx = cx; p->cy = cy; }
        p->visible = i == b->top;
    }
    invalidate(w);
}
static void nb_turn(pmwin_t *w, int to)
{
    nb_t *b = nb(w);
    if (to < 0 || to >= b->n || to == b->top) return;
    uint32_t cur = b->top >= 0 ? b->p[b->top].id : 0;
    static uint32_t scratch;                              /* (PAGESELECTNOTIFY: the program reads it) */
    if (!scratch) scratch = mem_alloc(0x1000);
    if (scratch) { p32(scratch, w->hwnd); p32(scratch + 4, cur); p32(scratch + 8, b->p[to].id); }
    notify(w, BKN_PAGESELECTEDPENDING, scratch);
    b->top = to;
    nb_layout(w);
    pmwin_t *p = win(b->p[to].hwnd);
    if (p) { pmwin_t *f = dlg_tab(p, 0, 1); if (f) set_focus(f->hwnd); }
    if (scratch) { p32(scratch, w->hwnd); p32(scratch + 4, cur); p32(scratch + 8, b->p[to].id); }
    notify(w, BKN_PAGESELECTED, scratch);
}
/* the major tabs: their x range, along the top */
static int nb_tab_x(pmwin_t *w, nb_t *b, int i, int *x1)
{
    int x = 8;
    for (int k = 0; k < b->n; k++) {
        if (!b->p[k].tab[0]) continue;
        int tw = plain_w(b->p[k].tab) + 20;
        if (k == i) { *x1 = x + tw; return x; }
        x += tw + 2;
    }
    (void)w;
    *x1 = x;
    return -1;
}
static uint32_t notebook_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    nb_t *b = nb(w);
    switch (msg) {
    case WM_SIZE: nb_layout(w); return 0;
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, pp_bgc(w, C_FACE));
        int th = nb_tab_h(), ty = w->cy - th, px, py, pcx, pcy;
        nb_page_rect(w, &px, &py, &pcx, &pcy);
        bevel(w, px - 3, py - 3, px + pcx + 3, ty + 1, 1);
        for (int i = 0; i < b->n; i++) {
            int x1, x0 = nb_tab_x(w, b, i, &x1);
            if (x0 < 0) continue;
            int on = i == b->top, y0 = on ? ty : ty + 2;
            fill(w, x0, y0, x1, w->cy - (on ? 1 : 3), C_FACE);
            line(w, x0, y0, x0, w->cy - (on ? 1 : 3), C_LIGHT); line(w, x0, w->cy - (on ? 1 : 3), x1 - 1, w->cy - (on ? 1 : 3), C_LIGHT);
            line(w, x1 - 1, y0, x1 - 1, w->cy - (on ? 1 : 3), C_DARK);
            if (on) line(w, x0 + 1, ty, x1 - 2, ty, C_FACE);
            text_mid(w, x0 + 10, y0, w->cy - (on ? 1 : 3), b->p[i].tab, pp_fgc(w, C_TEXT));
        }
        if (b->top >= 0 && b->p[b->top].status[0]) text_mid(w, 8, 0, nb_stat_h(), b->p[b->top].status, pp_fgc(w, C_TEXT));
        for (int k = 0; k < 2; k++) {                       /* the page arrows, bottom right */
            int x0 = w->cx - 40 + 20 * k;
            fill(w, x0, 3, x0 + 18, nb_stat_h() - 1, C_FACE); bevel(w, x0, 3, x0 + 18, nb_stat_h() - 1, 1);
            tri(w, x0 + 9, (nb_stat_h() + 2) / 2, k ? 3 : 2);
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16);
        if (y >= w->cy - nb_tab_h()) {
            for (int i = 0; i < b->n; i++) { int x1, x0 = nb_tab_x(w, b, i, &x1); if (x0 >= 0 && x >= x0 && x < x1) { nb_turn(w, i); break; } }
        } else if (y < nb_stat_h() && x >= w->cx - 40) nb_turn(w, b->top + (x >= w->cx - 20 ? 1 : -1));
        return 1;
    }
    case WM_CHAR: {
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        if (!(mp1 & KC_KEYUP) && (mp1 & KC_CTRL) && (vk == VK_PAGEDOWN || vk == VK_PAGEUP)) { nb_turn(w, b->top + (vk == VK_PAGEDOWN ? 1 : -1)); return 1; }
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    }
    case BKM_INSERTPAGE: {                                  /* rel id, (style, order) */
        if (b->n == 64) return 0;
        int rel = nb_find(b, mp1), order = (int)(mp2 >> 16), at = b->n;
        if (order & BKA_FIRST) at = 0;
        else if ((order & BKA_NEXT) && rel >= 0) at = rel + 1;
        else if ((order & BKA_PREV) && rel >= 0) at = rel;
        memmove(b->p + at + 1, b->p + at, (size_t)(b->n - at) * sizeof *b->p);
        memset(&b->p[at], 0, sizeof b->p[at]);
        b->p[at].id = b->next++;
        b->p[at].style = (uint16_t)(mp2 & 0xFFFF);
        b->n++;
        if (b->top >= at && b->top >= 0 && b->n > 1) b->top++;
        if (b->top < 0) b->top = 0;
        nb_layout(w);
        return b->p[at].id;
    }
    case BKM_DELETEPAGE: {                                  /* id, BKA_SINGLE / BKA_ALL */
        if ((mp2 & 0xFFFF) == 1) { for (int i = 0; i < b->n; i++) { pmwin_t *p = win(b->p[i].hwnd); if (p) p->visible = 0; } b->n = 0; b->top = -1; invalidate(w); return 1; }
        int i = nb_find(b, mp1);
        if (i < 0) return 0;
        pmwin_t *p = win(b->p[i].hwnd);
        if (p) p->visible = 0;
        memmove(b->p + i, b->p + i + 1, (size_t)(b->n - i - 1) * sizeof *b->p);
        b->n--;
        if (b->top >= b->n) b->top = b->n - 1;
        nb_layout(w);
        return 1;
    }
    case BKM_QUERYPAGEID: {                                 /* rel id, (BKA_FIRST/LAST/NEXT/PREV/TOP, style) */
        int how = (int)(mp2 & 0xFFFF), i = nb_find(b, mp1), r = -1;
        if (how & 4) r = 0; else if (how & 2) r = b->n - 1; else if (how & 0x20) r = b->top;
        else if ((how & 8) && i >= 0) r = i + 1; else if ((how & 0x10) && i >= 0) r = i - 1;
        return r >= 0 && r < b->n ? b->p[r].id : 0;
    }
    case BKM_QUERYPAGECOUNT: return (uint32_t)b->n;
    case BKM_SETPAGEWINDOWHWND: {                           /* id, hwnd */
        int i = nb_find(b, mp1);
        if (i < 0) return 0;
        b->p[i].hwnd = mp2;
        nb_layout(w);
        return 1;
    }
    case BKM_QUERYPAGEWINDOWHWND: { int i = nb_find(b, mp1); return i >= 0 ? b->p[i].hwnd : 0; }
    case BKM_SETPAGEDATA: { int i = nb_find(b, mp1); if (i < 0) return 0; b->p[i].data = mp2; return 1; }
    case BKM_QUERYPAGEDATA: { int i = nb_find(b, mp1); return i >= 0 ? b->p[i].data : 0; }
    case BKM_QUERYPAGESTYLE: { int i = nb_find(b, mp1); return i >= 0 ? b->p[i].style : 0; }
    case BKM_SETTABTEXT: case BKM_SETSTATUSLINETEXT: {       /* id, text */
        int i = nb_find(b, mp1);
        char *s = mp2 ? gs(mp2) : 0;
        if (i < 0) return 0;
        if (msg == BKM_SETTABTEXT) snprintf(b->p[i].tab, sizeof b->p[i].tab, "%s", s ? s : "");
        else snprintf(b->p[i].status, sizeof b->p[i].status, "%s", s ? s : "");
        repaint(w);
        return 1;
    }
    case BKM_QUERYTABTEXT: case BKM_QUERYSTATUSLINETEXT: {   /* id, BOOKTEXT */
        int i = nb_find(b, mp1);
        if (i < 0 || !ok(mp2, 8)) return 0;
        const char *s = msg == BKM_QUERYTABTEXT ? b->p[i].tab : b->p[i].status;
        uint32_t buf = g32(mp2), n = g32(mp2 + 4);
        if (buf && n && ok(buf, n)) snprintf((char *)C.mem + buf, n, "%s", s);
        return (uint32_t)strlen(s);
    }
    case BKM_TURNTOPAGE: { int i = nb_find(b, mp1); if (i < 0) return 0; nb_turn(w, i); return 1; }
    case BKM_CALCPAGERECT: {                                /* prcl, fPage (TRUE: the notebook's into its page's) */
        int x, y, cx, cy;
        nb_page_rect(w, &x, &y, &cx, &cy);
        if (!ok(mp1, 16)) return 0;
        if (mp2) put_rect(mp1, (int32_t)g32(mp1) + x, (int32_t)g32(mp1 + 4) + y, (int32_t)g32(mp1) + x + cx, (int32_t)g32(mp1 + 4) + y + cy);
        else put_rect(mp1, (int32_t)g32(mp1) - x, (int32_t)g32(mp1 + 4) - y, (int32_t)g32(mp1 + 8) + 12, (int32_t)g32(mp1 + 12) + nb_tab_h() + nb_stat_h() + 8);
        return 1;
    }
    case BKM_INVALIDATETABS: repaint(w); return 1;
    case BKM_SETDIMENSIONS: case BKM_SETNOTEBOOKCOLORS: case BKM_SETTABCOLOR: case BKM_SETTABBITMAP:
    case BKM_SETNOTEBOOKBUTTONS: case BKM_SETPAGEINFO: return 1;
    }
    return 0;
}
