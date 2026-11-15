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

#define WS_GROUP_   0x00010000u
#define WS_TABSTOP_ 0x00020000u
#define WS_DISABLED_ 0x40000000u
#define C_FACE  0xCCCCCC
#define C_LIGHT 0xFFFFFF
#define C_DARK  0x808080
#define C_TEXT  0x000000
#define C_HILITE 0x2050A0
#define MENU_H  22

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
static void bevel(pmwin_t *w, int x0, int y0, int x1, int y1, int raised)   /* [x0, x1) x [y0, y1) */
{
    uint32_t tl = raised ? C_LIGHT : C_DARK, br = raised ? C_DARK : C_LIGHT;
    line(w, x0, y1 - 1, x1 - 1, y1 - 1, tl); line(w, x0, y0, x0, y1 - 1, tl);
    line(w, x0, y0, x1 - 1, y0, br); line(w, x1 - 1, y0, x1 - 1, y1 - 1, br);
}
static void notify(pmwin_t *w, uint32_t code, uint32_t mp2)   /* WM_CONTROL to the owner */
{
    pmwin_t *o = w->owner ? w->owner : w->parent;
    if (o) pm_send(o, WM_CONTROL, (w->id & 0xFFFF) | code << 16, mp2 ? mp2 : w->hwnd);
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

/* ── WC_BUTTON ───────────────────────────────────────────────────────── */
typedef struct { int check, hilite, def; } btn_t;
static btn_t *btn(pmwin_t *w) { if (!w->ctl) w->ctl = calloc(1, sizeof(btn_t)); return w->ctl; }
static int btn_type(pmwin_t *w) { return (int)(w->style & 15); }
static int is_radio(pmwin_t *w) { return w->iproc && (btn_type(w) == 3 || btn_type(w) == 4) && !strcmp(w->cls, "#3"); }
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
    if (t == 0 || t == 7 || t == 8) {           /* push, user, notebook: a command */
        pmwin_t *o = w->owner ? w->owner : w->parent;
        if (o) post(o, o->hwnd, WM_COMMAND, w->id & 0xFFFF, 1 | 1u << 16);   /* CMDSRC_PUSHBUTTON, by the pointer */
        return;
    }
    if (t == 2) b->check = !b->check;                     /* BS_AUTOCHECKBOX */
    else if (t == 6) b->check = (b->check + 1) % 3;        /* BS_AUTO3STATE */
    else if (t == 4) {                                     /* BS_AUTORADIOBUTTON */
        pmwin_t *g[64];
        int n = radio_group(w, g, 64);
        for (int i = 0; i < n; i++) if (g[i] != w && btn(g[i])->check) { btn(g[i])->check = 0; repaint(g[i]); }
        b->check = 1;
    }
    repaint(w);
    notify(w, 1, 0);                                       /* BN_CLICKED */
}
static uint32_t button_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    btn_t *b = btn(w);
    int t = btn_type(w), dis = (w->style & WS_DISABLED_) != 0;
    uint32_t tc = dis ? C_DARK : C_TEXT;
    switch (msg) {
    case WM_PAINT:
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, C_FACE);
        if (t == 0 || t == 7 || t == 8) {
            bevel(w, 1, 1, w->cx - 1, w->cy - 1, !b->hilite);
            if (b->def || (w->style & 0x400)) bevel(w, 0, 0, w->cx, w->cy, 0), line(w, 0, 0, w->cx - 1, 0, 0), line(w, w->cx - 1, 0, w->cx - 1, w->cy - 1, 0),
                                              line(w, 0, w->cy - 1, w->cx - 1, w->cy - 1, 0), line(w, 0, 0, 0, w->cy - 1, 0);
            int tw = plain_w(w->text);
            text_mid(w, (w->cx - tw) / 2 + b->hilite, 0, w->cy - b->hilite, w->text, tc);
            if (pm_focus == w->hwnd) for (int x = 4; x < w->cx - 4; x += 2) pel(w, x, 4, C_TEXT);
        } else {
            int s = 12, y0 = (w->cy - s) / 2;
            fill(w, 1, y0, 1 + s, y0 + s, dis ? C_FACE : C_LIGHT);
            bevel(w, 1, y0, 1 + s, y0 + s, 0);
            if (b->check == 1 && (t == 3 || t == 4)) fill(w, 4, y0 + 3, 1 + s - 3, y0 + s - 3, tc);
            else if (b->check == 1) { line(w, 3, y0 + 3, 1 + s - 3, y0 + s - 3, tc); line(w, 3, y0 + s - 3, 1 + s - 3, y0 + 3, tc);
                                      line(w, 4, y0 + 3, 1 + s - 2, y0 + s - 3, tc); line(w, 4, y0 + s - 3, 1 + s - 2, y0 + 3, tc); }
            else if (b->check == 2) fill(w, 4, y0 + 4, 1 + s - 3, y0 + s - 3, C_DARK);
            text_mid(w, s + 6, 0, w->cy, w->text, tc);
            if (pm_focus == w->hwnd) for (int x = s + 5; x < s + 7 + plain_w(w->text); x += 2) pel(w, x, 1, C_TEXT);
        }
        present(w);
        return 0;
    case WM_BUTTON1DOWN:
        if (t == 0 || t == 7 || t == 8) { b->hilite = 1; repaint(w); }
        return 1;
    case WM_BUTTON1UP:
        if (t == 0 || t == 7 || t == 8) { if (!b->hilite) return 1; b->hilite = 0; repaint(w); }
        btn_click(w);
        return 1;
    case WM_CHAR:
        if (!(mp1 & KC_KEYUP) && (mp2 & 0xFFFF) == ' ') { btn_click(w); return 1; }
        return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
    case 0x0120: btn_click(w); return 1;                                   /* BM_CLICK */
    case 0x0124: return (uint32_t)b->check;                                /* BM_QUERYCHECK */
    case 0x0125: { int o = b->check; b->check = (int)(mp1 & 0xFFFF); repaint(w); return (uint32_t)o; }   /* BM_SETCHECK */
    case 0x0121: {                                                         /* BM_QUERYCHECKINDEX */
        pmwin_t *g[64];
        int n = radio_group(w, g, 64);
        for (int i = 0; i < n; i++) if (btn(g[i])->check) return (uint32_t)i;
        return 0xFFFFFFFF;
    }
    case 0x0122: return (uint32_t)b->hilite;                               /* BM_QUERYHILITE */
    case 0x0123: { int o = b->hilite; b->hilite = (int)(mp1 & 1); repaint(w); return (uint32_t)o; }
    case 0x0126: b->def = (int)(mp1 & 1); repaint(w); return 1;            /* BM_SETDEFAULT */
    }
    return 0;
}

/* ── WC_STATIC ───────────────────────────────────────────────────────── */
static uint32_t static_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    (void)mp1; (void)mp2;
    if (msg != WM_PAINT) return msg == WM_CHAR && w->owner ? pm_send(w->owner, msg, mp1, mp2) : 0;
    w->invalid = 0;
    int t = (int)(w->style & 0x7F);
    if (t == 2) {                                               /* SS_GROUPBOX */
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
    } else if (t >= 5 && t <= 10) {                             /* rectangles and frames */
        uint32_t c = (t == 5 || t == 8) ? C_TEXT : (t == 6 || t == 9) ? C_DARK : C_FACE;
        if (t <= 7) fill(w, 0, 0, w->cx, w->cy, c);
        else bevel(w, 0, 0, w->cx, w->cy, 0);
    } else {                                                    /* SS_TEXT (and what isn't here yet) */
        fill(w, 0, 0, w->cx, w->cy, C_FACE);
        char p[256];
        plain(w->text, p, sizeof p);
        int tw = text_w(p, (int)strlen(p)), th = pm_ascent + pm_descent;
        int x = (w->style & 0x100) ? (w->cx - tw) / 2 : (w->style & 0x200) ? w->cx - tw : 0;
        int y = (w->style & 0x400) ? (w->cy - th) / 2 : (w->style & 0x800) ? 0 : w->cy - th;
        text_at(w, x, y + pm_descent, p, (int)strlen(p), (w->style & WS_DISABLED_) ? C_DARK : C_TEXT);
    }
    present(w);
    return 0;
}

/* ── WC_ENTRYFIELD ───────────────────────────────────────────────────── */
typedef struct { int cur, limit, changed, ro, first; } ef_t;
static ef_t *ef(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(ef_t)); ((ef_t *)w->ctl)->limit = 32; ((ef_t *)w->ctl)->cur = -1; } return w->ctl; }
static uint32_t entry_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    ef_t *e = ef(w);
    int n = (int)strlen(w->text);
    if (e->cur < 0 || e->cur > n) e->cur = n;
    switch (msg) {
    case WM_CREATE:
        if (w->style & 0x20) e->ro = 1;                        /* ES_READONLY */
        return 0;
    case WM_PAINT: {
        w->invalid = 0;
        int m = (w->style & 8) ? 3 : 1;                         /* ES_MARGIN */
        fill(w, 0, 0, w->cx, w->cy, (w->style & WS_DISABLED_) ? C_FACE : C_LIGHT);
        if (w->style & 8) { bevel(w, 0, 0, w->cx, w->cy, 0); bevel(w, 1, 1, w->cx - 1, w->cy - 1, 0); }
        /* (scrolled so the cursor shows) */
        if (e->first > e->cur) e->first = e->cur;
        while (e->first < e->cur && text_w(w->text + e->first, e->cur - e->first) > w->cx - 2 * m - 4) e->first++;
        int x = m + 2;
        const char *s = w->text + e->first;
        int y = (w->cy - (pm_ascent + pm_descent)) / 2 + pm_descent;
        text_at(w, x, y, s, (int)strlen(s), (w->style & 0x80) ? C_LIGHT : C_TEXT);   /* (ES_UNREADABLE: not shown) */
        if (pm_focus == w->hwnd && !e->ro) {
            int cx = x + text_w(w->text + e->first, e->cur - e->first);
            line(w, cx, y - pm_descent, cx, y + pm_ascent, C_TEXT);
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF) - ((w->style & 8) ? 5 : 3), k = e->first;
        while (k < n && text_w(w->text + e->first, k - e->first + 1) <= x) k++;
        e->cur = k;
        repaint(w);
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int ch = (int)(mp2 & 0xFFFF), vk = (int)(mp2 >> 16), done = 1, changed = 0;
        if ((mp1 & KC_VIRTUALKEY) && vk) {
            switch (vk) {
            case 0x05: if (!e->ro && e->cur > 0) { memmove(w->text + e->cur - 1, w->text + e->cur, (size_t)(n - e->cur + 1)); e->cur--; changed = 1; } break;
            case 0x1b: if (!e->ro && e->cur < n) { memmove(w->text + e->cur, w->text + e->cur + 1, (size_t)(n - e->cur)); changed = 1; } break;
            case 0x15: if (e->cur > 0) e->cur--; break;
            case 0x17: if (e->cur < n) e->cur++; break;
            case 0x14: e->cur = 0; break;
            case 0x13: e->cur = n; break;
            default: done = 0;
            }
        } else if ((mp1 & KC_CHAR) && ch >= 32 && ch != 127 && !(mp1 & (KC_CTRL | KC_ALT))) {
            if (!e->ro && n < e->limit && n + 1 < (int)sizeof w->text) {
                memmove(w->text + e->cur + 1, w->text + e->cur, (size_t)(n - e->cur + 1));
                w->text[e->cur++] = (char)ch;
                changed = 1;
            }
        } else if ((mp1 & KC_CHAR) && ch == 8) {
            if (!e->ro && e->cur > 0) { memmove(w->text + e->cur - 1, w->text + e->cur, (size_t)(n - e->cur + 1)); e->cur--; changed = 1; }
        } else done = 0;
        if (!done) return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);   /* (Enter, Tab, Esc: the dialog's) */
        repaint(w);
        if (changed) { e->changed = 1; notify(w, 4, 0); }      /* EN_CHANGE */
        return 1;
    }
    case 0x0143: e->limit = (int)(mp1 & 0xFFFF); if (e->limit > 255) e->limit = 255; return 1;   /* EM_SETTEXTLIMIT */
    case 0x0140: { int c = e->changed; e->changed = 0; return (uint32_t)c; }                     /* EM_QUERYCHANGED */
    case 0x0141: return (uint32_t)e->cur | (uint32_t)e->cur << 16;                               /* EM_QUERYSEL */
    case 0x0142: e->cur = (int)(mp1 >> 16) > n ? n : (int)(mp1 >> 16); repaint(w); return 1;     /* EM_SETSEL */
    case 0x014a: return (uint32_t)e->ro;                                                         /* EM_QUERYREADONLY */
    case 0x014b: { int o = e->ro; e->ro = (int)(mp1 & 1); return (uint32_t)o; }                  /* EM_SETREADONLY */
    case 0x0148: return (uint32_t)e->first;                                                      /* EM_QUERYFIRSTCHAR */
    case 0x0149: e->first = (int)(mp1 & 0xFFFF); repaint(w); return 1;
    case 0x0146: if (!e->ro) { w->text[0] = 0; e->cur = 0; repaint(w); } return 1;               /* EM_CLEAR */
    }
    return 0;
}

/* ── WC_LISTBOX ──────────────────────────────────────────────────────── */
typedef struct { char *text; uint32_t handle; } lbitem_t;
typedef struct { lbitem_t *it; int n, cap, sel, top; } lb_t;
static lb_t *lb(pmwin_t *w) { if (!w->ctl) { w->ctl = calloc(1, sizeof(lb_t)); ((lb_t *)w->ctl)->sel = -1; } return w->ctl; }
static int lb_rows(pmwin_t *w) { int r = (w->cy - 4) / pm_lineh; return r < 1 ? 1 : r; }
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
            int k = l->top + r, y1 = w->cy - 2 - r * pm_lineh, y0 = y1 - pm_lineh;
            int on = k == l->sel;
            if (on) fill(w, 2, y0, w->cx - 2, y1, C_HILITE);
            text_at(w, 5, y0 + (pm_lineh - pm_ascent - pm_descent) / 2 + pm_descent, l->it[k].text, (int)strlen(l->it[k].text), on ? C_LIGHT : C_TEXT);
        }
        if (l->n > rows) {                                      /* (a scroll mark: there's more) */
            int h = w->cy - 4, bh = h * rows / l->n, by = 2 + (h - bh) * (l->n - rows - l->top) / (l->n - rows);
            fill(w, w->cx - 6, by, w->cx - 3, by + bh, C_DARK);
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int y = (int16_t)(mp1 >> 16), k = l->top + (w->cy - 2 - y) / pm_lineh;
        if (k >= 0 && k < l->n && k != l->sel) { l->sel = k; repaint(w); notify(w, 1, 0); }   /* LN_SELECT */
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0, o = l->sel;
        if (vk == 0x16 && l->sel > 0) l->sel--;
        else if (vk == 0x18 && l->sel + 1 < l->n) l->sel++;
        else if (vk == 0x14 && l->n) l->sel = 0;
        else if (vk == 0x13 && l->n) l->sel = l->n - 1;
        else if ((vk == 0x08 || vk == 0x1e) && l->sel >= 0) { notify(w, 5, 0); return 1; }   /* LN_ENTER */
        else if (!vk) return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        else return pm_send(w->owner ? w->owner : w->parent, msg, mp1, mp2);
        if (l->sel != o) { lb_show_sel(w, l); repaint(w); notify(w, 1, 0); }
        return 1;
    }
    case 0x0160: return (uint32_t)l->n;                                   /* LM_QUERYITEMCOUNT */
    case 0x0161: {                                                        /* LM_INSERTITEM */
        char *s = gs(mp2);
        if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->it = realloc(l->it, (size_t)l->cap * sizeof *l->it); }
        int at = i1 == -1 || i1 > l->n ? l->n : i1;
        if (i1 == -2 || i1 == -3) {                                       /* LIT_SORTASCENDING, DESCENDING */
            at = 0;
            while (at < l->n && (i1 == -2 ? strcasecmp(l->it[at].text, s ? s : "") <= 0 : strcasecmp(l->it[at].text, s ? s : "") >= 0)) at++;
        } else if (i1 < -1) at = l->n;
        memmove(l->it + at + 1, l->it + at, (size_t)(l->n - at) * sizeof *l->it);
        l->it[at].text = strdup(s ? s : "");
        l->it[at].handle = 0;
        l->n++;
        if (l->sel >= at) l->sel++;
        repaint(w);
        return (uint32_t)at;
    }
    case 0x0163:                                                          /* LM_DELETEITEM */
        if (i1 < 0 || i1 >= l->n) return (uint32_t)l->n;
        free(l->it[i1].text);
        memmove(l->it + i1, l->it + i1 + 1, (size_t)(l->n - i1 - 1) * sizeof *l->it);
        l->n--;
        if (l->sel == i1) l->sel = -1; else if (l->sel > i1) l->sel--;
        if (l->top > 0 && l->top >= l->n) l->top = l->n - 1;
        repaint(w);
        return (uint32_t)l->n;
    case 0x016e:                                                          /* LM_DELETEALL */
        for (int i = 0; i < l->n; i++) free(l->it[i].text);
        l->n = 0; l->sel = -1; l->top = 0;
        repaint(w);
        return 1;
    case 0x0164:                                                          /* LM_SELECTITEM */
        if (i1 == -1) { l->sel = -1; repaint(w); return 1; }
        if (i1 < 0 || i1 >= l->n) return 0;
        if (mp2) { if (l->sel != i1) { l->sel = i1; lb_show_sel(w, l); repaint(w); notify(w, 1, 0); } }
        else if (l->sel == i1) { l->sel = -1; repaint(w); }
        return 1;
    case 0x0165: {                                                        /* LM_QUERYSELECTION */
        if (l->sel < 0 || (i1 >= 0 && l->sel <= i1)) return 0xFFFFFFFF;  /* (LIT_FIRST, else after mp1) */
        return (uint32_t)l->sel;
    }
    case 0x0168: {                                                        /* LM_QUERYITEMTEXT: (index, cchMax), buffer */
        int cch = (int)(mp1 >> 16);
        if (i1 < 0 || i1 >= l->n || cch < 1 || !ok(mp2, (uint32_t)cch)) return 0;
        int k = (int)strlen(l->it[i1].text);
        if (k > cch - 1) k = cch - 1;
        memcpy(C.mem + mp2, l->it[i1].text, (size_t)k);
        C.mem[mp2 + (uint32_t)k] = 0;
        return (uint32_t)k;
    }
    case 0x0167: return i1 >= 0 && i1 < l->n ? (uint32_t)strlen(l->it[i1].text) : 0xFFFFFFFF;   /* LM_QUERYITEMTEXTLENGTH */
    case 0x0166: {                                                        /* LM_SETITEMTEXT */
        char *s = gs(mp2);
        if (i1 < 0 || i1 >= l->n) return 0;
        free(l->it[i1].text);
        l->it[i1].text = strdup(s ? s : "");
        repaint(w);
        return 1;
    }
    case 0x0169: if (i1 < 0 || i1 >= l->n) return 0; l->it[i1].handle = mp2; return 1;        /* LM_SETITEMHANDLE */
    case 0x016a: return i1 >= 0 && i1 < l->n ? l->it[i1].handle : 0;                          /* LM_QUERYITEMHANDLE */
    case 0x016b: {                                                        /* LM_SEARCHSTRING: (flags, start), text */
        char *s = gs(mp2);
        int16_t start = (int16_t)(mp1 >> 16);
        uint32_t fl = mp1 & 0xFFFF;
        if (!s) return 0xFFFFFFFF;
        for (int j = 0; j < l->n; j++) {
            int k = (start < 0 ? j : (start + 1 + j)) % (l->n ? l->n : 1);
            const char *t = l->it[k].text;
            int hit = (fl & 2) ? !!strcasestr(t, s) : (fl & 4) ? !strcasecmp(t, s) : !strncasecmp(t, s, strlen(s));   /* LSS_SUBSTRING, CASESENSITIVE..., PREFIX */
            if (hit) return (uint32_t)k;
        }
        return 0xFFFFFFFF;
    }
    case 0x0162: if (i1 >= 0 && i1 < l->n) { l->top = i1; repaint(w); } return 1;              /* LM_SETTOPINDEX */
    case 0x016d: return (uint32_t)l->top;                                                     /* LM_QUERYTOPINDEX */
    case 0x016c: return 1;                                                                    /* LM_SETITEMHEIGHT */
    }
    return 0;
}

/* ── menus ───────────────────────────────────────────────────────────── */
typedef struct menu menu_t;
typedef struct { char text[64]; uint16_t id, style, attr; menu_t *sub; } mitem_t;
struct menu { mitem_t *it; int n, open; };

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
        if (it->style & 0x0004) continue;                   /* MIS_SEPARATOR: no text */
        if ((it->style & 0x0001) || !(it->style & 0x000A)) {  /* MIS_TEXT (not a bitmap, not owner-drawn) */
            char *s = gs(p);
            snprintf(it->text, sizeof it->text, "%s", s ? s : "");
            p += (uint32_t)strlen(s ? s : "") + 1;
        }
        if (it->style & 0x0010) {                           /* MIS_SUBMENU: its template follows */
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
static int drop_item_h(mitem_t *it) { return (it->style & 4) ? 8 : pm_lineh + 4; }
static void menu_close(pmwin_t *f)
{
    if (!f || !f->drop) return;
    pmwin_t *bar = 0;
    for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].parent == f && pmwins[i].id == 0x8005) bar = &pmwins[i];
    if (bar && bar->ctl) ((menu_t *)bar->ctl)->open = -1;
    f->drop->ctl = 0;                                       /* (the bar's submenu: not the drop-down's to free) */
    win_destroy(f->drop);
    f->drop = 0;
    invalidate(f);
}
static void menu_command(pmwin_t *f, mitem_t *it)
{
    if (it->attr & 0x4000) return;                          /* MIA_DISABLED */
    menu_close(f);
    pmwin_t *t = f->client ? f->client : f;
    post(t, t->hwnd, WM_COMMAND, it->id, 2 | 1u << 16);     /* CMDSRC_MENU, by the pointer */
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
            if (it->style & 4) { line(w, 4, y - h / 2, w->cx - 5, y - h / 2, C_DARK); line(w, 4, y - h / 2 - 1, w->cx - 5, y - h / 2 - 1, C_LIGHT); }
            else {
                if (it->attr & 0x2000) { line(w, 6, y - h / 2, 9, y - h / 2 - 3, C_TEXT); line(w, 9, y - h / 2 - 3, 14, y - h / 2 + 3, C_TEXT); }   /* MIA_CHECKED */
                text_mid(w, 20, y - h, y, it->text, (it->attr & 0x4000) ? C_DARK : C_TEXT);
                if (it->sub) text_at(w, w->cx - 14, y - h + 6, ">", 1, C_TEXT);
            }
            y -= h;
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1UP: {
        int y = (int16_t)(mp1 >> 16), top = w->cy - 2;
        for (int i = 0; i < m->n; i++) {
            int h = drop_item_h(&m->it[i]);
            if (y <= top && y > top - h) {
                if (!(m->it[i].style & 4) && !m->it[i].sub) menu_command(w->parent, &m->it[i]);
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
    d->overlay = 1;
    int wmax = 60, h = 4;
    for (int k = 0; k < sub->n; k++) { int tw = plain_w(sub->it[k].text) + 44; if (tw > wmax) wmax = tw; h += drop_item_h(&sub->it[k]); }
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
            text_mid(w, x, 1, w->cy, m->it[i].text, i == m->open && w->parent && w->parent->drop ? C_LIGHT : (m->it[i].attr & 0x4000) ? C_DARK : C_TEXT);
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
    case 0x0192: {                                                        /* MM_SETITEMATTR: (id, deep), (mask, value) */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, (int)(mp1 >> 16));
        if (!it) return 0;
        uint16_t mask = (uint16_t)(mp2 & 0xFFFF), v = (uint16_t)(mp2 >> 16);
        it->attr = (uint16_t)((it->attr & ~mask) | (v & mask));
        repaint(w);
        return 1;
    }
    case 0x0191: {                                                        /* MM_QUERYITEMATTR: (id, deep), mask */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, (int)(mp1 >> 16));
        return it ? (uint32_t)(it->attr & mp2) : 0;
    }
    case 0x018e: {                                                        /* MM_SETITEMTEXT: id, text */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, 1);
        char *s = gs(mp2);
        if (!it) return 0;
        snprintf(it->text, sizeof it->text, "%s", s ? s : "");
        repaint(w);
        return 1;
    }
    case 0x018b: {                                                        /* MM_QUERYITEMTEXT: (id, cch), buffer */
        mitem_t *it = menu_find(m, mp1 & 0xFFFF, 1);
        uint32_t cch = mp1 >> 16;
        if (!it || !cch || !ok(mp2, cch)) return 0;
        uint32_t k = (uint32_t)strlen(it->text);
        if (k > cch - 1) k = cch - 1;
        memcpy(C.mem + mp2, it->text, k);
        C.mem[mp2 + k] = 0;
        return k;
    }
    case 0x018c: { mitem_t *it = menu_find(m, mp1 & 0xFFFF, 1); return it ? (uint32_t)strlen(it->text) : 0; }   /* MM_QUERYITEMTEXTLENGTH */
    case 0x0184: return (uint32_t)m->n;                                                         /* MM_QUERYITEMCOUNT */
    case 0x0193: { mitem_t *it = menu_find(m, mp1 & 0xFFFF, (int)(mp1 >> 16)); return it && !(it->attr & 0x4000); }   /* MM_ISITEMVALID */
    case 0x0186: menu_close(w->parent); return 1;                                               /* MM_ENDMENUMODE */
    case 0x0181: case 0x0188: {                                                                 /* MM_DELETEITEM, MM_REMOVEITEM */
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
    case 0x0180: {                                                        /* MM_INSERTITEM: MENUITEM, text */
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
    pmwin_t *bar = 0;
    for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].parent == f && pmwins[i].id == 0x8005) bar = &pmwins[i];
    int mh = bar ? MENU_H : 0;
    if (bar) { bar->x = 0; bar->y = f->ph - mh; bar->cx = f->pw; bar->cy = mh; bar->visible = 1; }
    if (f->client) { f->client->x = 0; f->client->y = 0; f->client->cx = f->pw; f->client->cy = f->ph - mh; }
}
static pmwin_t *menu_load(pmwin_t *f, uint32_t hmod, uint32_t id)
{
    uint32_t a, n;
    if (!find_resource(hmod, 3, id, &a, &n)) return 0;     /* RT_MENU */
    pmwin_t *bar = win_new(f, "#4", 0, 0x8005);             /* FID_MENU */
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

static pmproc_t builtin_proc(const char *cls)
{
    if (!strcmp(cls, "#3")) return button_proc;
    if (!strcmp(cls, "#5")) return static_proc;
    if (!strcmp(cls, "#6")) return entry_proc;
    if (!strcmp(cls, "#7")) return list_proc;
    return 0;
}

/* ── dialogs ─────────────────────────────────────────────────────────── */
static void dlg_paint_bg(pmwin_t *w) { w->invalid = 0; fill(w, 0, 0, w->cx, w->cy, C_FACE); present(w); }
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
        if (pmwins[i].used && pmwins[i].parent == dlg && !strcmp(pmwins[i].cls, "#3") && btn_type(&pmwins[i]) == 0 &&
            ((pmwins[i].style & 0x400) || (pmwins[i].ctl && btn(&pmwins[i])->def))) return &pmwins[i];
    return 0;
}
static uint32_t def_dlg_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    switch (msg) {
    case WM_PAINT: dlg_paint_bg(w); return 0;
    case WM_COMMAND: w->dlg_done = 1; w->dlg_result = mp1 & 0xFFFF; return 0;
    case WM_CLOSE: post(w, w->hwnd, WM_COMMAND, 2, 0); return 0;   /* DID_CANCEL */
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 0;
        int ch = (int)(mp2 & 0xFFFF), vk = (mp1 & KC_VIRTUALKEY) ? (int)(mp2 >> 16) : 0;
        if (ch == '\r' || vk == 0x08 || vk == 0x1e) {
            pmwin_t *d = dlg_default(w);
            if (d) btn_click(d); else post(w, w->hwnd, WM_COMMAND, 1, 0);   /* DID_OK */
            return 1;
        }
        if (ch == 27 || vk == 0x0f) { post(w, w->hwnd, WM_COMMAND, 2, 0); return 1; }   /* DID_CANCEL */
        if (ch == 9 || vk == 0x06) {
            pmwin_t *n = dlg_tab(w, win(pm_focus), (mp1 & KC_SHIFT) ? -1 : 1);
            if (n) set_focus(n->hwnd);
            return 1;
        }
        return 0;
    }
    }
    return 0;
}
static uint32_t api_WinDefDlgProc(void) { pmwin_t *w = win(A(0)); return w ? def_dlg_proc(w, A(1), A(2), A(3)) : 0; }

/* the template's items, from item i (and its children) under parent */
static int dlg_items(uint32_t t, int i, pmwin_t *parent, pmwin_t *dlg, uint32_t proc, uint32_t createp, pmwin_t **out)
{
    uint32_t it = t + g16(t + 6) + 30u * (uint32_t)i;      /* (a DLGTITEM: 30 bytes) */
    uint32_t nkids = g16(it + 2), cch = g16(it + 4), offc = g16(it + 6), ccht = g16(it + 8), offt = g16(it + 10);
    uint32_t style = g32(it + 12), id = g16(it + 24), offctl = g16(it + 28);
    int x = du_x((int16_t)g16(it + 16)), y = du_y((int16_t)g16(it + 18)), cx = du_x((int16_t)g16(it + 20)), cy = du_y((int16_t)g16(it + 22));
    char cls[64], text[256] = "";
    if (cch) { char *s = gp(t + offc, cch); snprintf(cls, sizeof cls, "%.*s", (int)cch, s ? s : ""); }
    else snprintf(cls, sizeof cls, "#%u", offc);
    if (ccht && offt != 0xFFFF) { char *s = gp(t + offt, ccht); snprintf(text, sizeof text, "%.*s", (int)ccht, s ? s : ""); }
    pmwin_t *w;
    if (!parent) {                                          /* the dialog itself: a window of the desktop */
        w = win_new(0, "#dialog", style & ~WS_VISIBLE_, id);
        if (!w) return -1;
        w->top = 1;
        w->fixed = 1;
        w->proc = proc;
        if (!proc) w->iproc = def_dlg_proc;
        frame_pixels(w, cx > 0 ? cx : 200, cy > 0 ? cy : 100);
        snprintf(w->text, sizeof w->text, "%s", text);
        *out = w;
        dlg = w;
    } else {
        w = win_new(parent, cls, style & ~WS_VISIBLE_, id);
        if (!w) return -1;
        w->x = x; w->y = y; w->cx = cx; w->cy = cy;
        w->owner = dlg;
        snprintf(w->text, sizeof w->text, "%s", text);
        w->visible = (style & WS_VISIBLE_) != 0;
        pm_send(w, WM_CREATE, offctl != 0xFFFF && offctl ? t + offctl : 0, 0);
    }
    int next = i + 1;
    for (uint32_t k = 0; k < nkids && next > 0; k++) next = dlg_items(t, next, w, dlg, proc, createp, out);
    return next;
}
static uint32_t dlg_create(uint32_t owner, uint32_t proc, uint32_t t, uint32_t createp)
{
    if (!pm_start() || !ok(t, 14 + 30)) return 0;
    pmwin_t *dlg = 0;
    if (dlg_items(t, 0, 0, 0, proc, createp, &dlg) < 0 || !dlg) { if (dlg) win_destroy(dlg); return 0; }
    dlg->owner = win(owner);
    if (pmb->title) pmb->title(dlg);
    pmwin_t *focus = dlg_tab(dlg, 0, 1);
    uint16_t ifocus = g16(t + 10);
    if (ifocus != 0xFFFF) {                                 /* (iItemFocus: the template's item number) */
        int k = 0;
        for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].parent == dlg && ++k == ifocus) focus = &pmwins[i];
    }
    uint32_t hwnd = dlg->hwnd;
    if (!pm_send(dlg, 0x003b, focus ? focus->hwnd : 0, createp) && focus && win(hwnd)) set_focus(focus->hwnd);   /* WM_INITDLG */
    if (!win(hwnd)) return 0;
    if (dlg->style & WS_VISIBLE_ || g32(t + g16(t + 6) + 12) & WS_VISIBLE_) win_show(dlg, 1);
    return hwnd;
}
static uint32_t api_WinCreateDlg(void) { return dlg_create(A(1), A(2), A(3), A(4)); }    /* (parent, owner, proc, ptemplate, createp) */
static uint32_t api_WinLoadDlg(void)          /* (hwndParent, hwndOwner, pfnDlgProc, hmod, idDlg, pCreateParams) */
{
    uint32_t a, n;
    if (!find_resource(A(3), 4, A(4), &a, &n)) return 0;   /* RT_DIALOG */
    return dlg_create(A(1), A(2), a, A(5));
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
    if (!find_resource(A(3), 4, A(4), &a, &n)) return 0xFFFF;   /* DID_ERROR */
    uint32_t h = dlg_create(A(1), A(2), a, A(5));
    if (!h) return 0xFFFF;
    uint32_t r = dlg_process(h);
    pmwin_t *w = win(h);
    if (w) win_destroy(w);
    return r;
}
static pmwin_t *dlg_item(uint32_t hdlg, uint32_t id)
{
    pmwin_t *p = win(hdlg);
    for (int i = 0; p && i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].parent == p && pmwins[i].id == (id & 0xFFFF)) return &pmwins[i];
    return 0;
}
static uint32_t api_WinSendDlgItemMsg(void) { return pm_send(dlg_item(A(0), A(1)), A(2), A(3), A(4)); }
static uint32_t api_WinSetDlgItemText(void)
{
    pmwin_t *w = dlg_item(A(0), A(1));
    char *s = gs(A(2));
    if (!w) return 0;
    snprintf(w->text, sizeof w->text, "%s", s ? s : "");
    if (w->ctl && !strcmp(w->cls, "#6")) ef(w)->cur = -1;
    repaint(w);
    return 1;
}
static uint32_t api_WinQueryDlgItemText(void) /* (hwndDlg, idItem, lMaxText, pszText) */
{
    pmwin_t *w = dlg_item(A(0), A(1));
    uint32_t n = A(2), b = A(3);
    if (!w || !n || !ok(b, n)) return 0;
    uint32_t l = (uint32_t)strlen(w->text);
    if (l > n - 1) l = n - 1;
    memcpy(C.mem + b, w->text, l);
    C.mem[b + l] = 0;
    return l;
}
static uint32_t api_WinQueryDlgItemTextLength(void) { pmwin_t *w = dlg_item(A(0), A(1)); return w ? (uint32_t)strlen(w->text) : 0; }
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
