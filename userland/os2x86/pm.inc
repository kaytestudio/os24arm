/* userland/os2x86/pm.inc - Presentation Manager for x86 OS/2 programs:
 * PMWIN (windows, classes, message queues, painting, timers, message
 * boxes) and the start of PMGPI (text, lines, boxes, colours). Included
 * by os2x86.c (it uses its guest memory, callbacks and lock).
 *
 * A frame window (WinCreateStdWindow) is a top-level window of the
 * desktop: the compositor draws its OS/2 frame (title bar, buttons,
 * border), PM here the inside, the client and its children, into the
 * frame's pixels (PM's coordinates: y up, from the bottom left). The
 * pixels go to a backend: Wayland (pm_wl.inc: an xdg-shell toplevel per
 * frame, its pointer and keys as PM's messages) or, OS2X86_PM=headless,
 * none, with OS2X86_PM_SCRIPT feeding it input ("click X Y; key abc;
 * close; shot FILE.ppm; size W H", top-left pixels) whenever the
 * program waits - run-tests.sh does so. Without either, WinInitialize
 * fails as before (and WinMessageBox, for console programs, prints). */

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

enum {
    WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_SHOW = 0x0005, WM_MOVE = 0x0006, WM_SIZE = 0x0007,
    WM_ACTIVATE = 0x000d, WM_SETFOCUS = 0x000f, WM_COMMAND = 0x0020, WM_SYSCOMMAND = 0x0021,
    WM_PAINT = 0x0023, WM_TIMER = 0x0024, WM_CLOSE = 0x0029, WM_QUIT = 0x002a, WM_CONTROL = 0x0030,
    WM_ERASEBACKGROUND = 0x004f, WM_MOUSEMOVE = 0x0070, WM_BUTTON1DOWN = 0x0071, WM_BUTTON1UP = 0x0072,
    WM_BUTTON1DBLCLK = 0x0073, WM_BUTTON2DOWN = 0x0074, WM_BUTTON2UP = 0x0075, WM_BUTTON3DOWN = 0x0077,
    WM_BUTTON3UP = 0x0078, WM_CHAR = 0x007a, WM_BUTTON1CLICK = 0x0413,
};
enum { KC_CHAR = 1, KC_VIRTUALKEY = 2, KC_SCANCODE = 4, KC_SHIFT = 8, KC_CTRL = 0x10, KC_ALT = 0x20, KC_KEYUP = 0x40, KC_PREVDOWN = 0x80 };
#define HWND_DESKTOP_ 1
#define FID_CLIENT 0x8008
#define WS_VISIBLE_ 0x80000000u

/* ── fonts: DejaVu Sans, Sans Bold, Sans Mono, Sans Mono Bold (\OS2\fonts)
 * at the sizes asked, OS/2's faces mapped onto them; pmf: the one text is
 * drawn and measured with now (a PS's, a window's, else the default) */
typedef struct { uint8_t *bm; int w, h, xo, yo, adv, done; } glyph_t;
typedef struct { int face, px, ok, ascent, descent, lineh; float scale; glyph_t g[256]; } pmfont_t;
static stbtt_fontinfo faces[4];
static int face_ok[4];
static pmfont_t *fonts[64];
static int nfonts;
static pmfont_t font_none = { 0, 15, 0, 11, 3, 16, 0, { { 0 } } };
static pmfont_t *pmf = &font_none, *pm_deffont = &font_none;
#define pm_ascent (pmf->ascent)
#define pm_descent (pmf->descent)
#define pm_lineh (pmf->lineh)
#define pm_font_ok (pmf->ok)

static pmfont_t *font_get(int face, int px)              /* (face: 0 sans, 1 bold, 2 mono, 3 mono bold) */
{
    if (px < 4) px = 4;
    if (px > 200) px = 200;
    if (!face_ok[face]) face = face & 2 ? (face_ok[2] ? 2 : 0) : 0;
    for (int i = 0; i < nfonts; i++) if (fonts[i]->face == face && fonts[i]->px == px) return fonts[i];
    pmfont_t *f = calloc(1, sizeof *f);
    f->face = face; f->px = px; f->ok = face_ok[face];
    if (f->ok) {
        int asc, desc, gap;
        f->scale = stbtt_ScaleForPixelHeight(&faces[face], (float)px);
        stbtt_GetFontVMetrics(&faces[face], &asc, &desc, &gap);
        f->ascent = (int)(asc * f->scale + 0.5f);
        f->descent = (int)(-desc * f->scale + 0.5f);
    } else {
        f->ascent = px * 3 / 4; f->descent = px / 4;
    }
    f->lineh = f->ascent + f->descent + 2;
    if (nfonts < 64) fonts[nfonts++] = f;
    return f;
}
static void font_init(void)
{
    static int tried;
    if (tried) return;
    tried = 1;
    static const char *const files[4] = { "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf" };
    char dirs[4][PATH_MAX + 32];
    int nd = 0;
    const char *env = getenv("OS2X86_FONT");                 /* (a file: its directory has the others) */
    if (env) { snprintf(dirs[nd], sizeof dirs[nd], "%s", env); char *sl = strrchr(dirs[nd], '/'); if (sl) *sl = 0; else strcpy(dirs[nd], "."); nd++; }
    snprintf(dirs[nd++], sizeof dirs[0], "/h/OS2/fonts");
    snprintf(dirs[nd++], sizeof dirs[0], "%s/fonts", exe_dir);
    snprintf(dirs[nd++], sizeof dirs[0], "%s/../fonts", exe_dir);
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < nd && !face_ok[k]; i++) {
            char path[PATH_MAX * 2];
            snprintf(path, sizeof path, "%s/%s", dirs[i], files[k]);
            if (k == 0 && env) snprintf(path, sizeof path, "%s", env);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            unsigned char *d = malloc((size_t)n);
            if (d && fread(d, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&faces[k], d, stbtt_GetFontOffsetForIndex(d, 0))) face_ok[k] = 1;
            else free(d);
            fclose(f);
        }
    pm_deffont = pmf = font_get(0, 15);
}
static glyph_t *glyph(unsigned char ch)
{
    glyph_t *g = &pmf->g[ch];
    if (g->done) return g;
    g->done = 1;
    if (!pmf->ok) { g->adv = pmf->px / 2; return g; }
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&faces[pmf->face], ch, &adv, &lsb);
    g->adv = (int)(adv * pmf->scale + 0.5f);
    g->bm = stbtt_GetCodepointBitmap(&faces[pmf->face], 0, pmf->scale, ch, &g->w, &g->h, &g->xo, &g->yo);
    return g;
}
/* "10.Helv", "8.Helv Bold", "9.Courier", "10.System VIO": a font (points: pixels at 1.5 a point, as the default) */
static pmfont_t *font_named(const char *s, int bold)
{
    int pt = atoi(s);
    const char *n = strchr(s, '.');
    n = n ? n + 1 : s;
    int mono = strcasestr(n, "courier") || strcasestr(n, "mono") || strcasestr(n, "vio") || strcasestr(n, "fixed") || strcasestr(n, "terminal");
    if (strcasestr(n, "bold")) bold = 1;
    return font_get((mono ? 2 : 0) | (bold ? 1 : 0), pt > 0 ? pt * 3 / 2 : 15);
}
static int text_w(const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += glyph((unsigned char)s[i])->adv;
    return w;
}

/* ── windows ─────────────────────────────────────────────────────────── */
typedef struct pmwin pmwin_t;
typedef uint32_t (*pmproc_t)(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
typedef struct { char name[64]; uint32_t proc, style, cb; int p16; } pmclass_t;   /* (p16: a 16-bit program's: proc a far 16:16 one) */
struct pmwin {
    int used;
    uint32_t hwnd, id, style, tid, proc;
    int p16;                            /* (proc: a 16-bit program's, far Pascal - pm16.inc) */
    pmproc_t iproc;                     /* the frame's, a message box's: here */
    pmwin_t *parent, *owner, *client;
    int x, y, cx, cy;                   /* in its parent, PM's way up */
    char text[256], cls[64];
    uint8_t *words;                     /* the window words */
    uint32_t nwords;
    int visible, invalid, top;          /* top: a frame of the desktop's */
    uint32_t *px;                       /* top: its pixels, top left first */
    int pw, ph;
    void *bk;                           /* the backend's */
    int mb_result;                      /* a message box's answer */
    uint32_t mb_style;
    char mb_caption[128];
    void *ctl;                          /* a control's, a menu's state (pm_ctl.inc) */
    int fixed;                          /* a top window that keeps its size (dialogs, message boxes) */
    int overlay;                        /* a menu's drop-down: painted last, clicked first */
    pmwin_t *drop;                      /* a frame: its open drop-down */
    int dlg_done;                       /* a dialog: WinDismissDlg'd, with dlg_result */
    pmwin_t *prev_shown;                /* a top window: the one shown before it (the script's, the focus's) */
    uint32_t haccel;                    /* a frame's accelerator table */
    pmfont_t *font;                     /* presentation parameters: its font (PP_FONTNAMESIZE), */
    char font_name[48];
    int has_fg, has_bg;                 /* ... its colours (PP_FOREGROUNDCOLOR, PP_BACKGROUNDCOLOR) */
    uint32_t pp_fg, pp_bg;
    uint32_t dlg_result;
};
#define MAXWIN 1024                     /* (windows, controls, and bitmaps: they're hidden windows) */
static pmwin_t pmwins[MAXWIN];
static pmclass_t pmclasses[64];
static int npmclasses;
static uint32_t pm_focus;
static uint32_t pm_capture;             /* the window that has the mouse (a scroll bar's thumb, dragged) */
static int pm_inited;
static pmwin_t *pm_last_shown;
static const char *win_title(pmwin_t *f) { return f->mb_caption[0] ? f->mb_caption : f->text; }

static pmwin_t *win(uint32_t h)
{
    uint32_t i = h - 0x80000100u;
    return i < MAXWIN && pmwins[i].used ? &pmwins[i] : 0;
}
static pmwin_t *frame_of(pmwin_t *w) { while (w && !w->top) w = w->parent; return w; }
static int shown(pmwin_t *w) { for (; w && !w->top; w = w->parent) if (!w->visible) return 0; return w != 0; }   /* (it and every parent up to its frame) */
static void win_abs(pmwin_t *w, int *x, int *y)         /* its bottom left, in its frame */
{
    *x = *y = 0;
    for (; w && !w->top; w = w->parent) { *x += w->x; *y += w->y; }
}

/* the backend */
typedef struct {
    void (*show)(pmwin_t *f, int on);
    void (*present)(pmwin_t *f);
    void (*title)(pmwin_t *f);
    int  (*idle)(struct timespec *dl);   /* nothing to do: wait (0: nothing will ever come) */
    void (*clip_set)(const char *text);  /* the desktop's clipboard: ours now (text, LF lines, Latin-1) */
    char *(*clip_get)(void);             /* ... its text, when it's another program's (malloc'd, or 0) */
    void (*cursor)(pmwin_t *ptr);        /* the pointer's shape, over our windows */
} pmback_t;
static const pmback_t *pmb;

/* message queues: one a thread */
typedef struct { uint32_t hwnd, msg, mp1, mp2, time; int32_t x, y; } pmmsg_t;
typedef struct { int used; uint32_t tid; pmmsg_t m[512]; int n; } pmq_t;
static pmq_t pmqs[MAXTHR];
static pmq_t *myq(void) { pmq_t *q = &pmqs[me->tid - 1]; return q->used ? q : 0; }
static int mouse_x, mouse_y;
static void post(pmwin_t *w, uint32_t hwnd, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    uint32_t tid = w ? w->tid : me->tid;
    pmq_t *q = tid >= 1 && tid <= MAXTHR ? &pmqs[tid - 1] : 0;
    if (!q || !q->used || q->n == 512) return;
    q->m[q->n++] = (pmmsg_t){ hwnd, msg, mp1, mp2, (uint32_t)ms_now(), mouse_x, mouse_y };
    pthread_cond_broadcast(&gcv);
}

static pmfont_t *win_font(pmwin_t *w) { for (; w; w = w->parent) if (w->font) return w->font; return pm_deffont; }   /* (a parent's goes to its children) */
static uint32_t guest_call16(uint32_t fp, uint32_t hwnd, uint32_t msg, uint32_t mp1, uint32_t mp2);
static int pm_dlg16;                        /* (a dialog being made for a 16-bit program: its procedure 16-bit) */
static uint32_t pm_send(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    if (!w) return 0;
    if (w->iproc) {
        pmfont_t *save = pmf;
        pmf = win_font(w);
        uint32_t r = w->iproc(w, msg, mp1, mp2);
        if (!r && (msg == WM_BUTTON1DBLCLK || msg == 0x0076 || msg == 0x0079))   /* (a double-click a control doesn't take: one more press) */
            r = w->iproc(w, msg == WM_BUTTON1DBLCLK ? WM_BUTTON1DOWN : msg == 0x0076 ? WM_BUTTON2DOWN : WM_BUTTON3DOWN, mp1, mp2);
        pmf = save;
        return r;
    }
    if (!w->proc) return 0;
    if (w->p16) return guest_call16(w->proc, w->hwnd, msg, mp1, mp2);
    uint32_t args[4] = { w->hwnd, msg, mp1, mp2 };
    return guest_call(w->proc, 4, args);
}

/* timers */
typedef struct { int used; uint32_t hwnd, id, ms; uint64_t next; } pmtimer_t;
static pmtimer_t pmtimers[64];

/* ── pixels ──────────────────────────────────────────────────────────── */
static uint32_t pm_rgb(int32_t c, int rgbmode, int back)
{
    if (rgbmode && c >= 0) return (uint32_t)c & 0xFFFFFF;
    static const uint32_t base[16] = { 0xFFFFFF, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0x000000,
                                       0x808080, 0x000080, 0x800000, 0x800080, 0x008000, 0x008080, 0x808000, 0xCCCCCC };
    if (c >= 0 && c < 16) return base[c];
    switch (c) {
    case -1: return 0x000000;                 /* CLR_BLACK */
    case -2: return 0xFFFFFF;                 /* CLR_WHITE */
    case -3: return back ? 0xFFFFFF : 0x000000;   /* CLR_DEFAULT */
    case -17: case -18: case -26: return 0x000000;   /* window, menu, static text */
    case -19: return 0x404040;                /* window frame */
    case -20: case -42: return 0xFFFFFF;      /* window, field background */
    case -23: return 0x2050A0;                /* active title */
    case -24: return 0x4A7EBB;                /* the desktop */
    case -32: return 0x2050A0;                /* highlight */
    case -33: return 0xFFFFFF;
    case -39: return 0x808080;                /* button dark */
    case -41: return 0xFFFFFF;                /* button light */
    default: return 0xCCCCCC;                 /* button middle, dialogs, menus, ... */
    }
}

typedef struct { int face, px, outline, used; } lfont_t;
typedef struct { int used; uint32_t hwnd; int32_t color, back; int rgb, x, y, painting;
                 pmfont_t *font; int lcid, boxpx; lfont_t *lf; void *geo; } pmps_t;   /* (geo: pm_geo.inc's) */   /* (font: GpiSetCharSet's; lf: its logical fonts, 255) */
static pmps_t pmpss[64];
static pmps_t *ps_get(uint32_t h) { h -= 0xA000; return h < 64 && pmpss[h].used ? &pmpss[h] : 0; }

/* a pixel of a PS's window, in its coordinates (clipped to the window) */
static void pel(pmwin_t *w, int x, int y, uint32_t rgb)
{
    if (!w || x < 0 || y < 0 || x >= w->cx || y >= w->cy) return;
    pmwin_t *f = frame_of(w);
    if (!f || !f->px) return;
    int ax, ay;
    win_abs(w, &ax, &ay);
    int fx = ax + x, fy = ay + y;
    if (fx < 0 || fy < 0 || fx >= f->pw || fy >= f->ph) return;
    f->px[(f->ph - 1 - fy) * f->pw + fx] = 0xFF000000u | rgb;
}
static void blend(pmwin_t *w, int x, int y, uint32_t rgb, int a)
{
    if (!w || x < 0 || y < 0 || x >= w->cx || y >= w->cy || a <= 0) return;
    pmwin_t *f = frame_of(w);
    if (!f || !f->px) return;
    int ax, ay;
    win_abs(w, &ax, &ay);
    int fx = ax + x, fy = ay + y;
    if (fx < 0 || fy < 0 || fx >= f->pw || fy >= f->ph) return;
    uint32_t *p = &f->px[(f->ph - 1 - fy) * f->pw + fx], d = *p, o = 0xFF000000u;
    for (int sh = 0; sh < 24; sh += 8) {
        int s = (rgb >> sh) & 255, t = (d >> sh) & 255;
        o |= (uint32_t)((s * a + t * (255 - a)) / 255) << sh;
    }
    *p = o;
}
static void fill(pmwin_t *w, int x0, int y0, int x1, int y1, uint32_t rgb)   /* [x0, x1) x [y0, y1) */
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (w && x1 > w->cx) x1 = w->cx;
    if (w && y1 > w->cy) y1 = w->cy;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) pel(w, x, y, rgb);
}
/* text with its baseline at y; returns the x after it */
static int text_at(pmwin_t *w, int x, int y, const char *s, int n, uint32_t rgb)
{
    for (int i = 0; i < n; i++) {
        glyph_t *g = glyph((unsigned char)s[i]);
        if (g->bm) {
            for (int r = 0; r < g->h; r++)
                for (int c = 0; c < g->w; c++)
                    blend(w, x + g->xo + c, y - (g->yo + r), rgb, g->bm[r * g->w + c]);
        } else if (!pm_font_ok && s[i] != ' ') {
            for (int k = 0; k < 9; k++) { pel(w, x + 1, y + k, rgb); pel(w, x + 5, y + k, rgb); }
            for (int k = 1; k < 5; k++) { pel(w, x + k, y, rgb); pel(w, x + k, y + 8, rgb); }
        }
        x += g->adv;
    }
    return x;
}
static void line(pmwin_t *w, int x0, int y0, int x1, int y1, uint32_t rgb)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        pel(w, x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void present(pmwin_t *w)
{
    pmwin_t *f = frame_of(w);
    if (f && f->drop && w != f->drop) f->drop->invalid = 1;      /* (the drop-down again, over it) */
    if (f && f->visible && pmb && pmb->present) pmb->present(f);
}

/* ── window lifecycle ────────────────────────────────────────────────── */
static pmproc_t builtin_proc(const char *cls);           /* pm_ctl.inc */
static void pp_apply(pmwin_t *w, uint32_t pp);
static void frame_layout(pmwin_t *f);
static void set_focus(uint32_t hwnd);
static pmclass_t *class_find(const char *n)
{
    for (int i = 0; i < npmclasses; i++) if (!strcmp(pmclasses[i].name, n)) return &pmclasses[i];
    return 0;
}
static void frame_pixels(pmwin_t *f, int cx, int cy)
{
    if (cx < 1) cx = 1;
    if (cy < 1) cy = 1;
    uint32_t *px = calloc((size_t)cx * cy, 4);
    if (!px) return;
    for (int i = 0; i < cx * cy; i++) px[i] = 0xFFCCCCCC;
    free(f->px);
    f->px = px; f->pw = cx; f->ph = cy;
    f->cx = cx; f->cy = cy;
}
static pmwin_t *win_new(pmwin_t *parent, const char *cls, uint32_t style, uint32_t id)
{
    for (int i = 0; i < MAXWIN; i++)
        if (!pmwins[i].used) {
            pmwin_t *w = &pmwins[i];
            memset(w, 0, sizeof *w);
            w->used = 1;
            w->hwnd = 0x80000100u + (uint32_t)i;
            w->parent = parent;
            w->style = style;
            w->id = id;
            w->tid = me->tid;
            snprintf(w->cls, sizeof w->cls, "%s", cls);
            pmclass_t *c = class_find(cls);
            w->nwords = (c ? c->cb : 0) + 16;
            w->words = calloc(w->nwords, 1);
            if (c) { w->proc = c->proc; w->p16 = c->p16; }
            else w->iproc = builtin_proc(cls);     /* WC_BUTTON, WC_STATIC, ... */
            return w;
        }
    return 0;
}
static void invalidate(pmwin_t *w)
{
    for (int i = 0; i < MAXWIN; i++)
        if (pmwins[i].used) {
            for (pmwin_t *p = &pmwins[i]; p; p = p->parent)
                if (p == w) { pmwins[i].invalid = 1; break; }
        }
    pthread_cond_broadcast(&gcv);
}
static void win_show(pmwin_t *w, int on)
{
    if (!w || w->visible == on) return;
    w->visible = on;
    pmwin_t *f = frame_of(w);
    if (w->top && pmb && pmb->show) pmb->show(w, on);
    if (!on && pm_last_shown == w) pm_last_shown = w->prev_shown && w->prev_shown->used && w->prev_shown->visible ? w->prev_shown : 0;
    if (on) {
        if (w->top) { if (pm_last_shown != w) w->prev_shown = pm_last_shown; pm_last_shown = w; }
        if (w->top && w->client) { pm_send(w->client, WM_SIZE, 0, (uint32_t)(w->client->cx & 0xFFFF) | (uint32_t)w->client->cy << 16); w->client->visible = 1; }
        invalidate(w);
        if (f && !pm_focus) pm_focus = f->client ? f->client->hwnd : f->hwnd;
    }
}
static void win_destroy(pmwin_t *w)
{
    if (!w || !w->used) return;
    for (int i = 0; i < MAXWIN; i++)
        if (pmwins[i].used && pmwins[i].parent == w) win_destroy(&pmwins[i]);
    pm_send(w, WM_DESTROY, 0, 0);
    if (w->top) { if (pmb && pmb->show && w->visible) pmb->show(w, 0); free(w->px); }
    for (int i = 0; i < 64; i++) if (pmtimers[i].used && pmtimers[i].hwnd == w->hwnd) pmtimers[i].used = 0;
    if (pm_focus == w->hwnd) pm_focus = 0;
    if (pm_last_shown == w) pm_last_shown = w->prev_shown && w->prev_shown->used ? w->prev_shown : 0;
    for (int i = 0; i < MAXWIN; i++) if (pmwins[i].used && pmwins[i].prev_shown == w) pmwins[i].prev_shown = w->prev_shown;
    if (w->parent && w->parent->drop == w) w->parent->drop = 0;
    free(w->words);
    free(w->ctl);
    w->ctl = 0;
    w->used = 0;
}
/* the deepest visible window under (x, y) of frame f (its pixels, PM's way up) */
static pmwin_t *win_at(pmwin_t *f, int x, int y)
{
    if (f->drop && f->drop->visible && x >= f->drop->x && y >= f->drop->y && x < f->drop->x + f->drop->cx && y < f->drop->y + f->drop->cy)
        return f->drop;
    pmwin_t *best = f->client ? f->client : f;
    for (int i = 0; i < MAXWIN; i++) {                 /* (the frame's controls - menu bar, scroll bars - beside the client) */
        pmwin_t *c = &pmwins[i];
        if (c->used && c->parent == f && c->visible && c != f->client && f->client && !c->overlay &&
            x >= c->x && y >= c->y && x < c->x + c->cx && y < c->y + c->cy) { best = c; break; }
    }
    for (int again = 1; again; ) {
        again = 0;
        pmwin_t *hit = 0;
        for (int i = 0; i < MAXWIN; i++) {               /* (the last created is on top: a group box's under its buttons) */
            pmwin_t *c = &pmwins[i];
            if (!c->used || c->parent != best || !c->visible || c->overlay) continue;
            int ax, ay;
            win_abs(c, &ax, &ay);
            if (x >= ax && y >= ay && x < ax + c->cx && y < ay + c->cy) hit = c;
        }
        if (hit) { best = hit; again = 1; }
    }
    return best;
}

static void menu_close(pmwin_t *f);
static int pm_kc;                                            /* the Shift, Ctrl and Alt keys held now (KC_*) */
static uint32_t pm_mouse_frame;             /* the frame the pointer was last over */
static uint32_t pm_b2_hwnd;                                  /* button 2 down in it, at (pm_b2_x, pm_b2_y) of its frame: a drag, when it moves */
static int pm_b2_x, pm_b2_y, pm_b2_began;
/* the backends' input, frame pixels top left: as PM messages */
static void pm_mouse(pmwin_t *f, int px, int py, int button, int down)
{
    int x = px, y = f->ph - 1 - py;
    mouse_x = x; mouse_y = y;
    pm_mouse_frame = f->hwnd;
    pmwin_t *w = win(pm_capture) && frame_of(win(pm_capture)) == f ? win(pm_capture) : win_at(f, x, y);
    int ax, ay;
    win_abs(w, &ax, &ay);
    uint32_t mp1 = (uint32_t)((x - ax) & 0xFFFF) | (uint32_t)((y - ay) & 0xFFFF) << 16;
    uint32_t mp2 = (uint32_t)pm_kc << 16;                    /* (hit test, the keys held) */
    if (!button) {
        if (pm_b2_hwnd && !pm_b2_began && (abs(x - pm_b2_x) > 3 || abs(y - pm_b2_y) > 3)) {   /* button 2 held, moved: a drag starts */
            pmwin_t *s = win(pm_b2_hwnd);
            pm_b2_began = 1;
            if (s) { int sx, sy; win_abs(s, &sx, &sy); post(s, s->hwnd, 0x0420, 1, (uint32_t)((pm_b2_x - sx) & 0xFFFF) | (uint32_t)((pm_b2_y - sy) & 0xFFFF) << 16); }   /* WM_BEGINDRAG */
        }
        post(w, w->hwnd, WM_MOUSEMOVE, mp1, mp2);
        return;
    }
    if (button == 2) { if (down) { pm_b2_hwnd = w->hwnd; pm_b2_x = x; pm_b2_y = y; pm_b2_began = 0; } else pm_b2_hwnd = 0; }
    static const uint32_t dn[3] = { WM_BUTTON1DOWN, WM_BUTTON2DOWN, WM_BUTTON3DOWN }, up[3] = { WM_BUTTON1UP, WM_BUTTON2UP, WM_BUTTON3UP };
    if (button < 1 || button > 3) return;
    if (down && f->drop && w != f->drop && w->id != 0x8005) { menu_close(f); return; }   /* (FID_MENU: it decides) */
    static uint64_t last_t;
    static int last_b, last_x, last_y;
    static uint32_t last_w;
    uint32_t msgdown = dn[button - 1];
    if (down) {                                              /* (a second press, soon and near: a double-click) */
        uint64_t now = ms_now();
        if (last_b == button && last_w == w->hwnd && now - last_t < 500 && abs(x - last_x) < 5 && abs(y - last_y) < 5) {
            msgdown = button == 1 ? WM_BUTTON1DBLCLK : button == 2 ? 0x0076 : 0x0079;
            last_t = 0;
        } else { last_t = now; last_b = button; last_x = x; last_y = y; last_w = w->hwnd; }
    }
    if (down && !(w->style & 0x40000000u)) set_focus((f->client && w == f) ? f->client->hwnd : w->hwnd);
    if (w->style & 0x40000000u) return;                 /* WS_DISABLED */
    post(w, w->hwnd, down ? msgdown : up[button - 1], mp1, mp2);
}
static int key_hook(pmwin_t *f, int ch, int vk, int scan, int kc);
static void pm_key(pmwin_t *f, int ch, int vk, int scan, int kc)
{
    kc |= pm_kc & (KC_SHIFT | KC_CTRL | KC_ALT);
    if ((kc & KC_CTRL) && ch >= 1 && ch <= 26) ch = 'a' + ch - 1;     /* (Ctrl+D: 'd', with KC_CTRL, as PM says it) */
    if (key_hook(f, ch, vk, scan, kc)) return;
    pmwin_t *w = win(pm_focus);
    if (!w || frame_of(w) != f) w = f->client ? f->client : f;
    uint32_t fs = (uint32_t)kc | KC_SCANCODE | (ch ? KC_CHAR : 0) | (vk ? KC_VIRTUALKEY : 0);
    uint32_t mp2 = (uint32_t)(ch & 0xFFFF) | (uint32_t)vk << 16;
    post(w, w->hwnd, WM_CHAR, fs | 1u << 16 | (uint32_t)scan << 24, mp2);
    post(w, w->hwnd, WM_CHAR, fs | KC_KEYUP | KC_PREVDOWN | 1u << 16 | (uint32_t)scan << 24, mp2);
}
static void pm_close(pmwin_t *f)
{
    pmwin_t *t = f->client ? f->client : f;
    post(t, t->hwnd, WM_CLOSE, 0, 0);
}
static void pm_resize(pmwin_t *f, int cx, int cy)
{
    if (cx == f->cx && cy == f->cy) return;
    uint32_t old = f->client ? ((uint32_t)(f->client->cx & 0xFFFF) | (uint32_t)f->client->cy << 16) : 0;
    frame_pixels(f, cx, cy);
    frame_layout(f);
    if (f->client)
        post(f->client, f->client->hwnd, WM_SIZE, old, (uint32_t)(f->client->cx & 0xFFFF) | (uint32_t)f->client->cy << 16);
    invalidate(f);
}

/* ── the headless backend: a script for its input ───────────────────── */
static char *pm_script;
static void hl_shot(const char *file)
{
    pmwin_t *f = pm_last_shown;
    FILE *o = f && f->px ? fopen(file, "wb") : 0;
    if (!o) { fprintf(stderr, "[pm] shot %s: no window\n", file); return; }
    fprintf(o, "P6\n%d %d\n255\n", f->pw, f->ph);
    for (int i = 0; i < f->pw * f->ph; i++) { uint32_t p = f->px[i]; fputc((p >> 16) & 255, o); fputc((p >> 8) & 255, o); fputc(p & 255, o); }
    fclose(o);
    fprintf(stderr, "[pm] shot %s (%dx%d, \"%s\")\n", file, f->pw, f->ph, win_title(f));
}
static int hl_idle(struct timespec *dl)
{
    /* the next step of the script, while there's one */
    while (pm_script && *pm_script) {
        char *s = pm_script, *e = strchr(s, ';');
        if (e) *e = 0;
        pm_script = e ? e + 1 : 0;
        while (*s == ' ') s++;
        pmwin_t *f = pm_last_shown;
        char a[256] = "";
        int x, y;
        if (!*s) continue;
        if (trace) fprintf(stderr, "[pm] script: %s\n", s);
        pm_kc = 0;
        if (!strncmp(s, "shift ", 6)) { pm_kc = KC_SHIFT; s += 6; }   /* (shift vkey left, shift click X Y) */
        if (sscanf(s, "shot %255s", a) == 1) { hl_shot(a); continue; }
        if (!f) { fprintf(stderr, "[pm] %s: no window\n", s); continue; }
        if (sscanf(s, "click %d %d", &x, &y) == 2) { pm_mouse(f, x, y, 1, 1); pm_mouse(f, x, y, 1, 0); return 1; }
        if (sscanf(s, "altclick %d %d", &x, &y) == 2) { pm_kc = KC_ALT; pm_mouse(f, x, y, 1, 1); pm_mouse(f, x, y, 1, 0); return 1; }
        if (sscanf(s, "rdown %d %d", &x, &y) == 2) { pm_mouse(f, x, y, 2, 1); return 1; }
        if (sscanf(s, "rup %d %d", &x, &y) == 2) { pm_mouse(f, x, y, 2, 0); return 1; }
        if (!strncmp(s, "ctrl+", 5)) { pm_kc = KC_CTRL; s += 5; }   /* (ctrl+rdrag ...: with Ctrl held) */
        if (sscanf(s, "rclick %d %d", &x, &y) == 2) { pm_mouse(f, x, y, 2, 1); pm_mouse(f, x, y, 2, 0); return 1; }
        if (sscanf(s, "move %d %d", &x, &y) == 2) { pm_mouse(f, x, y, 0, 0); return 1; }
        if (sscanf(s, "size %d %d", &x, &y) == 2) { pm_resize(f, x, y); return 1; }
        if (!strncmp(s, "key ", 4)) {
            for (char *k = s + 4; *k; k++) pm_key(f, (unsigned char)*k, *k == '\r' ? 0x1e : 0, 0, 0);
            return 1;
        }
        if (!strcmp(s, "enter")) { pm_key(f, '\r', 0x1e, 0x1c, 0); return 1; }
        if (sscanf(s, "dblclick %d %d", &x, &y) == 2) { pm_mouse(f, x, y, 1, 1); pm_mouse(f, x, y, 1, 0); pm_mouse(f, x, y, 1, 1); pm_mouse(f, x, y, 1, 0); return 1; }
        if (sscanf(s, "ctrl %255s", a) == 1) { pm_key(f, a[0], 0, 0, KC_CTRL); return 1; }
        if (sscanf(s, "alt %255s", a) == 1) { pm_key(f, a[0], 0, 0, KC_ALT); return 1; }
        if (sscanf(s, "vkey %255s", a) == 1) {
            static const struct { const char *n; int vk, ch; } vks[] = { { "up", 0x16, 0 }, { "down", 0x18, 0 }, { "left", 0x15, 0 },
                { "right", 0x17, 0 }, { "pgup", 0x11, 0 }, { "pgdn", 0x12, 0 }, { "home", 0x14, 0 }, { "end", 0x13, 0 },
                { "tab", 0x06, 9 }, { "backspace", 0x05, 8 }, { "delete", 0x1b, 0 }, { "enter", 0x08, '\r' }, { "esc", 0x0f, 27 } };
            int vk = 0, ch = 0;
            if (a[0] == 'f' && atoi(a + 1) >= 1 && atoi(a + 1) <= 12) vk = 0x20 + atoi(a + 1) - 1;
            for (size_t k = 0; k < sizeof vks / sizeof *vks; k++) if (!strcmp(a, vks[k].n)) { vk = vks[k].vk; ch = vks[k].ch; }
            if (vk) { pm_key(f, ch, vk, 0, 0); return 1; }
        }
        int x2, y2;
        if (sscanf(s, "rdrag %d %d %d %d", &x, &y, &x2, &y2) == 4) {   /* button 2: PM's dragging */
            pm_mouse(f, x, y, 2, 1); pm_mouse(f, (x + x2) / 2, (y + y2) / 2, 0, 0); pm_mouse(f, x2, y2, 0, 0); pm_mouse(f, x2, y2, 2, 0);
            return 1;
        }
        if (sscanf(s, "drag %d %d %d %d", &x, &y, &x2, &y2) == 4) { pm_mouse(f, x, y, 1, 1); pm_mouse(f, x2, y2, 0, 0); pm_mouse(f, x2, y2, 1, 0); return 1; }
        if (!strcmp(s, "esc")) { pm_key(f, 27, 0x0f, 1, 0); return 1; }
        if (!strcmp(s, "close")) { pm_close(f); return 1; }
        if (sscanf(s, "sleep %d", &x) == 1) { struct timespec ts = { x / 1000, (long)(x % 1000) * 1000000L }; UNLOCKED(nanosleep(&ts, 0)); return 1; }
        fprintf(stderr, "[pm] script: what's \"%s\"?\n", s);
    }
    if (!dl) return 0;
    pthread_cond_timedwait(&gcv, &gil, dl);
    return 1;
}
static const pmback_t pm_headless = { 0, 0, 0, hl_idle, 0, 0, 0 };

#ifdef OS2X86_WAYLAND
static uint32_t mb_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
#include "pm_wl.inc"
#endif

static int pm_start(void)
{
    if (pm_inited) return pmb != 0;
    pm_inited = 1;
    const char *m = getenv("OS2X86_PM");
#ifdef OS2X86_WAYLAND
    if (!(m && !strcmp(m, "headless")) && getenv("WAYLAND_DISPLAY") && wl_start()) pmb = &pm_wayland;
#endif
    if (!pmb && m && !strcmp(m, "headless")) {
        pmb = &pm_headless;
        const char *s = getenv("OS2X86_PM_SCRIPT");
        if (s) pm_script = strdup(s);
    }
    if (pmb) font_init();
    else fprintf(stderr, "%s: Presentation Manager needs the desktop (%s)\n", prog_name,
                 getenv("WAYLAND_DISPLAY") ? "its Wayland display didn't answer" : "no WAYLAND_DISPLAY");
    return pmb != 0;
}

static uint32_t frame_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2);
/* ── messages: get, peek, dispatch ───────────────────────────────────── */
static int pm_take_raw(pmq_t *q, uint32_t filt, uint32_t first, uint32_t last, int remove, pmmsg_t *out)
{
    for (int i = 0; i < q->n; i++) {
        pmmsg_t *m = &q->m[i];
        if (filt && m->hwnd != filt) continue;
        if ((first || last) && (m->msg < first || m->msg > last) && m->msg != WM_QUIT) continue;
        *out = *m;
        if (remove) { memmove(q->m + i, q->m + i + 1, (size_t)(q->n - i - 1) * sizeof *q->m); q->n--; }
        return 1;
    }
    /* then the windows to paint, then the timers that are due */
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < MAXWIN; i++) {
        pmwin_t *w = &pmwins[i];
        if (!w->used || !w->invalid || w->tid != me->tid || !w->visible || (filt && w->hwnd != filt)) continue;
        if (w->overlay != pass) continue;
        if (!w->top && !shown(w)) continue;                /* (a hidden parent's: not painted) */
        if (!frame_of(w) || !frame_of(w)->visible) continue;
        if (w->iproc == frame_proc) { w->invalid = 0; continue; }       /* (a frame: its client paints) */
        if (remove) w->invalid = 0;
        *out = (pmmsg_t){ w->hwnd, WM_PAINT, 0, 0, (uint32_t)ms_now(), 0, 0 };
        return 1;
    }
    uint64_t now = ms_now();
    for (int i = 0; i < 64; i++) {
        pmtimer_t *t = &pmtimers[i];
        pmwin_t *w = win(t->hwnd);
        if (!t->used || (w && w->tid != me->tid) || now < t->next || (filt && t->hwnd != filt)) continue;
        if (remove) t->next = now + t->ms;
        *out = (pmmsg_t){ t->hwnd, WM_TIMER, t->id, 0, (uint32_t)now, 0, 0 };
        return 1;
    }
    return 0;
}
static void put_qmsg(uint32_t p, const pmmsg_t *m)
{
    p32(p, m->hwnd); p32(p + 4, m->msg); p32(p + 8, m->mp1); p32(p + 12, m->mp2);
    p32(p + 16, m->time); p32(p + 20, (uint32_t)m->x); p32(p + 24, (uint32_t)m->y); p32(p + 28, 0);
}
/* the next message, waiting for one */
/* the keys' and buttons' state as of the message the program has taken (PM's WinGetKeyState is synchronous with its queue) */
static int pm_sync_kc, pm_sync_buttons;
static int pm_take(pmq_t *q, uint32_t filt, uint32_t first, uint32_t last, int remove, pmmsg_t *out)
{
    int r = pm_take_raw(q, filt, first, last, remove, out);
    if (r && remove) {
        uint32_t m = out->msg;
        if (m >= WM_MOUSEMOVE && m <= 0x0079) pm_sync_kc = (int)(out->mp2 >> 16) & (KC_SHIFT | KC_CTRL | KC_ALT);
        if (m == WM_CHAR) pm_sync_kc = (int)out->mp1 & (KC_SHIFT | KC_CTRL | KC_ALT);
        if (m == WM_BUTTON1DOWN || m == WM_BUTTON1DBLCLK) pm_sync_buttons |= 1;
        else if (m == WM_BUTTON1UP) pm_sync_buttons &= ~1;
        else if (m == WM_BUTTON2DOWN || m == 0x0076) pm_sync_buttons |= 2;
        else if (m == WM_BUTTON2UP) pm_sync_buttons &= ~2;
        else if (m == WM_BUTTON3DOWN || m == 0x0079) pm_sync_buttons |= 4;
        else if (m == 0x0078) pm_sync_buttons &= ~4;
    }
    return r;
}
static void pm_wait_msg(pmq_t *q, uint32_t filt, uint32_t first, uint32_t last, pmmsg_t *out)
{
    for (;;) {
        if (pm_take(q, filt, first, last, 1, out)) return;
        uint64_t next = 0;
        for (int i = 0; i < 64; i++) if (pmtimers[i].used && (!next || pmtimers[i].next < next)) next = pmtimers[i].next;
        struct timespec ts, *dl = 0;
        if (next) {
            uint64_t now = ms_now(), ms = next > now ? next - now : 0;
            dl = deadline((uint32_t)ms, &ts);
        }
        if (!pmb->idle(dl)) {
            if (nlive > 1) { pthread_cond_wait(&gcv, &gil); continue; }
            fprintf(stderr, "%s: nothing more will come (no display, the script's done)\n", prog_name);
            do_exit(3);
        }
    }
}

/* ── PMWIN ───────────────────────────────────────────────────────────── */
static uint32_t api_WinInitialize(void) { return pm_start() ? 0x100 + me->tid : 0; }
static uint32_t api_WinTerminate(void) { return 1; }
static uint32_t api_WinCreateMsgQueue(void)
{
    if (!pm_start()) return 0;
    pmq_t *q = &pmqs[me->tid - 1];
    q->used = 1; q->tid = me->tid; q->n = 0;
    return 0x9000 + me->tid;
}
static uint32_t api_WinDestroyMsgQueue(void) { pmqs[me->tid - 1].used = 0; return 1; }
static uint32_t api_WinQueryAnchorBlock(void) { return 0x100 + me->tid; }
static uint32_t api_WinGetLastError(void) { return 0; }
static const char *class_name(uint32_t p, char *buf)
{
    if ((p & 0xFFFF0000u) == 0xFFFF0000u) { snprintf(buf, 16, "#%u", p & 0xFFFF); return buf; }
    char *s = gs(p);
    snprintf(buf, 64, "%s", s ? s : "");
    return buf;
}
static uint32_t api_WinRegisterClass(void)    /* (hab, pszClass, pfnWndProc, flStyle, cbWindowData) */
{
    char b[64];
    const char *n = class_name(A(1), b);
    pmclass_t *c = class_find(n);
    if (!c) { if (npmclasses == 64) return 0; c = &pmclasses[npmclasses++]; }
    snprintf(c->name, sizeof c->name, "%s", n);
    c->proc = A(2); c->style = A(3); c->cb = A(4);
    return 1;
}
static pmwin_t *menu_load(pmwin_t *f, uint32_t hmod, uint32_t id);
static uint32_t accel_new(uint32_t t);
static uint32_t frame_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    if (msg == WM_COMMAND && w->client) { post(w->client, w->client->hwnd, msg, mp1, mp2); return 0; }
    if ((msg == 0x0031 || msg == 0x0032 || msg == WM_CONTROL) && w->client) return pm_send(w->client, msg, mp1, mp2);   /* WM_VSCROLL, WM_HSCROLL */
    if (msg == WM_CLOSE) { if (w->client) post(w->client, w->client->hwnd, WM_CLOSE, 0, 0); else post(w, 0, WM_QUIT, 0, 0); }
    return 0;
}
static uint32_t api_WinCreateStdWindow(void)
{
    /* (hwndParent, flStyle, pflCreate, pszClass, pszTitle, flClientStyle, hmod, idResources, phwndClient) */
    uint32_t style = A(1), pfl = A(2), pcls = A(3), ptitle = A(4), cstyle = A(5), hmod = A(6), idres = A(7), pclient = A(8);
    uint32_t fcf = pfl ? g32(pfl) : 0;
    if (!pm_start()) return 0;
    pmwin_t *f = win_new(0, "#1", style & ~WS_VISIBLE_, 0);
    if (!f) return 0;
    f->top = 1;
    f->iproc = frame_proc;
    char *t = ptitle ? gs(ptitle) : 0;
    snprintf(f->text, sizeof f->text, "%s", t ? t : "");
    frame_pixels(f, 480, 320);
    if (fcf & 4) menu_load(f, hmod, idres);              /* FCF_MENU */
    if (fcf & 0x8000) { uint32_t a, n; if (find_resource(hmod, 8, idres, &a, &n)) f->haccel = accel_new(a); }   /* FCF_ACCELTABLE */
    for (int k = 0; k < 2; k++)                          /* FCF_VERTSCROLL, FCF_HORZSCROLL: the frame's, telling the client */
        if (fcf & (k ? 0x80u : 0x40u)) {
            pmwin_t *s = win_new(f, "#8", k ? 0 : 1, k ? 0x8007 : 0x8006);
            if (s) { s->owner = f; s->visible = 1; }
        }
    if (pcls) {
        char b[64];
        const char *n = class_name(pcls, b);
        pmwin_t *c = win_new(f, n, cstyle, FID_CLIENT);
        if (!c) { win_destroy(f); return 0; }
        f->client = c;
        c->owner = f;
        frame_layout(f);
        pm_send(c, WM_CREATE, 0, 0);
        if (!c->used) { win_destroy(f); return 0; }
        if (pclient) p32(pclient, c->hwnd);
    } else if (pclient) p32(pclient, 0);
    if (pmb->title) pmb->title(f);
    if (style & WS_VISIBLE_) win_show(f, 1);
    return f->hwnd;
}
static uint32_t api_WinCreateWindow(void)
{
    /* (hwndParent, pszClass, pszName, flStyle, x, y, cx, cy, hwndOwner, hwndInsertBehind, id, pCtlData, pPresParams) */
    char b[64];
    pmwin_t *p = win(A(0));
    const char *n = class_name(A(1), b);
    uint32_t style = A(3);
    if (!pm_start()) return 0;
    pmwin_t *w = win_new(p, n, style & ~WS_VISIBLE_, A(10));
    if (!w) return 0;
    w->x = (int32_t)A(4); w->y = (int32_t)A(5); w->cx = (int32_t)A(6); w->cy = (int32_t)A(7);
    w->owner = win(A(8));
    char *t = A(2) ? gs(A(2)) : 0;
    snprintf(w->text, sizeof w->text, "%s", t ? t : "");
    if (!p) {                                /* a child of the desktop: a frame of its own */
        w->top = 1;
        frame_pixels(w, w->cx > 0 ? w->cx : 200, w->cy > 0 ? w->cy : 100);
        if (pmb->title) pmb->title(w);
    }
    pp_apply(w, A(12));                      /* pPresParams */
    pm_send(w, WM_CREATE, A(11), A(12));
    if (!w->used) return 0;
    if (style & WS_VISIBLE_) { w->visible = 1; if (w->top) win_show(w, 1); invalidate(w); }
    return w->hwnd;
}
static uint32_t api_WinDestroyWindow(void) { pmwin_t *w = win(A(0)); if (!w) return 0; win_destroy(w); return 1; }
static uint32_t api_WinShowWindow(void)
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    if (w->top) win_show(w, A(1) != 0);
    else { w->visible = A(1) != 0; invalidate(frame_of(w)); }
    return 1;
}
static uint32_t api_WinIsWindow(void) { return win(A(1)) != 0; }
static uint32_t api_WinGetMsg(void)           /* (hab, pqmsg, hwndFilter, msgFirst, msgLast) */
{
    pmq_t *q = myq();
    if (!q) return 0;
    pmmsg_t m;
    pm_wait_msg(q, A(2), A(3), A(4), &m);
    put_qmsg(A(1), &m);
    return m.msg != WM_QUIT;
}
static uint32_t api_WinPeekMsg(void)          /* (hab, pqmsg, hwndFilter, msgFirst, msgLast, fl) */
{
    pmq_t *q = myq();
    if (!q) return 0;
    pmmsg_t m;
    if (!pm_take(q, A(2), A(3), A(4), A(5) & 1, &m)) {
        pmb->idle(0);                        /* (the script's next step, if any) */
        return 0;
    }
    put_qmsg(A(1), &m);
    return 1;
}
static uint32_t api_WinDispatchMsg(void)      /* (hab, pqmsg) */
{
    uint32_t p = A(1);
    pmwin_t *w = win(g32(p));
    if (!w) return 0;
    return pm_send(w, g32(p + 4), g32(p + 8), g32(p + 12));
}
static uint32_t api_WinPostMsg(void)
{
    pmwin_t *w = win(A(0));
    if (!w && A(0)) return 0;
    post(w, A(0), A(1), A(2), A(3));
    return 1;
}
static uint32_t api_WinPostQueueMsg(void)     /* (hmq, msg, mp1, mp2) */
{
    uint32_t tid = A(0) - 0x9000;
    if (tid < 1 || tid > MAXTHR || !pmqs[tid - 1].used) return 0;
    pmq_t *q = &pmqs[tid - 1];
    if (q->n == 512) return 0;
    q->m[q->n++] = (pmmsg_t){ 0, A(1), A(2), A(3), (uint32_t)ms_now(), mouse_x, mouse_y };
    pthread_cond_broadcast(&gcv);
    return 1;
}
static uint32_t api_WinSendMsg(void) { return pm_send(win(A(0)), A(1), A(2), A(3)); }

static void pm_set_pointer(uint32_t hptr);
static uint32_t sys_pointer(uint32_t id);
static uint32_t def_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    (void)mp1; (void)mp2;
    switch (msg) {
    case WM_CLOSE: post(w, 0, WM_QUIT, 0, 0); return 0;
    case WM_PAINT: w->invalid = 0; return 0;
    case WM_CHAR: return !w->top && w->owner ? pm_send(w->owner, msg, mp1, mp2) : 0;   /* (a key nobody took: up to the owner) */
    case WM_MOUSEMOVE: pm_set_pointer(sys_pointer(1)); return 1;                        /* (the arrow, as PM's) */
    case WM_ERASEBACKGROUND: return 0;
    }
    return 0;
}
static uint32_t api_WinDefWindowProc(void) { pmwin_t *w = win(A(0)); return w ? def_proc(w, A(1), A(2), A(3)) : 0; }


/* presentation spaces */
static void geo_free(pmps_t *ps);
static uint32_t ps_new(pmwin_t *w, int painting)
{
    for (int i = 0; i < 64; i++)
        if (!pmpss[i].used) {
            geo_free(&pmpss[i]);
            pmpss[i] = (pmps_t){ 1, w->hwnd, -3, -3, 0, 0, 0, painting, 0, 0, 0, 0, 0 };
            return 0xA000 + (uint32_t)i;
        }
    return 0;
}
static pmwin_t *ps_win(pmps_t *ps) { return ps ? win(ps->hwnd) : 0; }
static pmfont_t *ps_font(pmps_t *ps) { return ps && ps->font ? ps->font : ps ? win_font(ps_win(ps)) : pm_deffont; }
static void put_rect(uint32_t p, int x0, int y0, int x1, int y1) { p32(p, (uint32_t)x0); p32(p + 4, (uint32_t)y0); p32(p + 8, (uint32_t)x1); p32(p + 12, (uint32_t)y1); }
static uint32_t api_WinBeginPaint(void)       /* (hwnd, hps, prclPaint) */
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    w->invalid = 0;
    if (A(2)) put_rect(A(2), 0, 0, w->cx, w->cy);
    pmps_t *ps = ps_get(A(1));
    if (ps) { ps->hwnd = w->hwnd; ps->painting = 1; return A(1); }
    return ps_new(w, 1);
}
static uint32_t api_WinEndPaint(void)
{
    pmps_t *ps = ps_get(A(0));
    if (!ps) return 0;
    present(ps_win(ps));
    if (ps->painting == 1) ps->used = 0;
    return 1;
}
static uint32_t api_WinGetPS(void) { pmwin_t *w = win(A(0)); return w ? ps_new(w, 2) : 0; }
static uint32_t api_WinReleasePS(void)
{
    pmps_t *ps = ps_get(A(0));
    if (!ps) return 0;
    present(ps_win(ps));
    ps->used = 0;
    return 1;
}
static uint32_t api_WinQueryWindowRect(void)
{
    pmwin_t *w = win(A(0));
    if (A(0) == HWND_DESKTOP_) { put_rect(A(1), 0, 0, 1920, 1080); return 1; }
    if (!w) return 0;
    put_rect(A(1), 0, 0, w->cx, w->cy);
    return 1;
}
static uint32_t api_WinInvalidateRect(void)
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    invalidate(w);
    return 1;
}
static uint32_t api_WinUpdateWindow(void)
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    for (int i = 0; i < MAXWIN; i++)
        if (pmwins[i].used && pmwins[i].invalid && pmwins[i].visible && !pmwins[i].top) {
            for (pmwin_t *p = &pmwins[i]; p; p = p->parent)
                if (p == w) { pmwins[i].invalid = 0; pm_send(&pmwins[i], WM_PAINT, 0, 0); break; }
        }
    return 1;
}
static uint32_t api_WinFillRect(void)         /* (hps, prcl, lColor) */
{
    pmps_t *ps = ps_get(A(0));
    uint32_t r = A(1);
    pmwin_t *w = ps_win(ps);
    if (!w || !ok(r, 16)) return 0;
    fill(w, (int32_t)g32(r), (int32_t)g32(r + 4), (int32_t)g32(r + 8), (int32_t)g32(r + 12), pm_rgb((int32_t)A(2), ps->rgb, 1));
    return 1;
}
static uint32_t api_WinDrawText(void)
{
    /* (hps, cchText, lpchText, prcl, clrFore, clrBack, flCmd) */
    pmps_t *ps = ps_get(A(0));
    pmwin_t *w = ps_win(ps);
    int32_t n = (int32_t)A(1);
    char *s = gs(A(2));
    uint32_t r = A(3), fl = A(6);
    if (!w || !s || !ok(r, 16)) return 0;
    if (n < 0) n = (int32_t)strlen(s);
    pmfont_t *save = pmf;
    pmf = ps_font(ps);
    int x0 = (int32_t)g32(r), y0 = (int32_t)g32(r + 4), x1 = (int32_t)g32(r + 8), y1 = (int32_t)g32(r + 12);
    int32_t fg = (fl & 0x40) ? ps->color : (int32_t)A(4), bg = (fl & 0x40) ? ps->back : (int32_t)A(5);
    int tw = text_w(s, n), th = pm_ascent + pm_descent;
    int x = fl & 0x100 ? x0 + (x1 - x0 - tw) / 2 : fl & 0x200 ? x1 - tw : x0;
    int ytop = fl & 0x400 ? y0 + (y1 - y0 + th) / 2 : fl & 0x800 ? y0 + th : y1;
    if (fl & 2) {                            /* DT_QUERYEXTENT */
        put_rect(r, x, ytop - th, x + tw, ytop);
        pmf = save;
        return (uint32_t)n;
    }
    if (fl & 0x8000) fill(w, x0, y0, x1, y1, pm_rgb(bg, ps->rgb, 1));
    text_at(w, x, ytop - pm_ascent, s, n, pm_rgb(fg, ps->rgb, 0));
    pmf = save;
    return (uint32_t)n;
}
static void cb_settext(pmwin_t *w);
static int ctl_get_text(pmwin_t *w, char *out, int max);
static int ctl_set_text(pmwin_t *w, const char *s);
static uint32_t api_WinSetWindowText(void)
{
    pmwin_t *w = win(A(0));
    char *s = gs(A(1));
    if (!w) return 0;
    snprintf(w->text, sizeof w->text, "%s", s ? s : "");
    if (!strcmp(w->cls, "#2")) cb_settext(w);
    ctl_set_text(w, s ? s : "");
    pmwin_t *t = w;
    if (!w->top && w->parent && w->parent->top && w->parent->client == w) t = w->parent;
    if (t->top && pmb && pmb->title) pmb->title(t);
    invalidate(w);
    return 1;
}
static uint32_t api_WinQueryWindowText(void)  /* (hwnd, cchBufferMax, pchBuffer) */
{
    pmwin_t *w = win(A(0));
    uint32_t n = A(1), b = A(2);
    if (!w || !n || !ok(b, n)) return 0;
    int k = ctl_get_text(w, (char *)C.mem + b, (int)n);
    if (k >= 0) return (uint32_t)k;
    uint32_t l = (uint32_t)strlen(w->text);
    if (l > n - 1) l = n - 1;
    memcpy(C.mem + b, w->text, l);
    C.mem[b + l] = 0;
    return l;
}
static uint32_t api_WinQueryWindowTextLength(void)
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    static char tmp[65536];
    int k = ctl_get_text(w, tmp, sizeof tmp);
    return k >= 0 ? (uint32_t)k : (uint32_t)strlen(w->text);
}
static uint32_t words_get(pmwin_t *w, int32_t i, int sz)
{
    if (i == -2) return w->style | (w->visible ? WS_VISIBLE_ : 0);   /* QWL_STYLE */
    if (i == -1) return w->id;                                      /* QWS_ID */
    if (i < 0 || (uint32_t)i + (uint32_t)sz > w->nwords) return 0;
    uint32_t v = 0;
    memcpy(&v, w->words + i, (size_t)sz);
    return v;
}
static uint32_t words_set(pmwin_t *w, int32_t i, int sz, uint32_t v)
{
    if (i == -2) { w->style = v & ~WS_VISIBLE_; return 1; }
    if (i == -1) { w->id = v & 0xFFFF; return 1; }
    if (i < 0 || (uint32_t)i + (uint32_t)sz > w->nwords) return 0;
    memcpy(w->words + i, &v, (size_t)sz);
    return 1;
}
static uint32_t api_WinQueryWindowULong(void) { pmwin_t *w = win(A(0)); return w ? words_get(w, (int32_t)A(1), 4) : 0; }
static uint32_t api_WinSetWindowULong(void) { pmwin_t *w = win(A(0)); return w ? words_set(w, (int32_t)A(1), 4, A(2)) : 0; }
static uint32_t api_WinQueryWindowUShort(void) { pmwin_t *w = win(A(0)); return w ? words_get(w, (int32_t)A(1), 2) & 0xFFFF : 0; }
static uint32_t api_WinSetWindowUShort(void) { pmwin_t *w = win(A(0)); return w ? words_set(w, (int32_t)A(1), 2, A(2) & 0xFFFF) : 0; }
static uint32_t api_WinQueryWindow(void)      /* (hwnd, cmd) */
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    if (A(1) == 5) return w->parent ? w->parent->hwnd : HWND_DESKTOP_;     /* QW_PARENT */
    if (A(1) == 4) return w->owner ? w->owner->hwnd : 0;                    /* QW_OWNER */
    return 0;
}
static uint32_t api_WinWindowFromID(void)
{
    pmwin_t *p = win(A(0));
    for (int i = 0; i < MAXWIN; i++)
        if (pmwins[i].used && pmwins[i].parent == p && p && pmwins[i].id == A(1)) return pmwins[i].hwnd;
    return 0;
}
static uint32_t api_WinSetWindowPos(void)     /* (hwnd, hwndInsertBehind, x, y, cx, cy, fl) */
{
    pmwin_t *w = win(A(0));
    uint32_t fl = A(6);
    if (!w) return 0;
    if (fl & 1) {                            /* SWP_SIZE */
        if (w->top) pm_resize(w, (int32_t)A(4), (int32_t)A(5));
        else { w->cx = (int32_t)A(4); w->cy = (int32_t)A(5); pm_send(w, WM_SIZE, 0, (A(4) & 0xFFFF) | A(5) << 16); invalidate(w); }
    }
    if ((fl & 2) && !w->top) { w->x = (int32_t)A(2); w->y = (int32_t)A(3); invalidate(frame_of(w)); }
    if (fl & 8) win_show(w, 1), w->visible = 1;
    if (fl & 0x10) { if (w->top) win_show(w, 0); else w->visible = 0; }
    return 1;
}
static uint32_t api_WinQueryWindowPos(void)
{
    pmwin_t *w = win(A(0));
    uint32_t p = A(1);
    if (!w || !ok(p, 36)) return 0;
    memset(C.mem + p, 0, 36);
    p32(p + 4, (uint32_t)w->cy); p32(p + 8, (uint32_t)w->cx); p32(p + 12, (uint32_t)w->y); p32(p + 16, (uint32_t)w->x);
    p32(p + 24, w->hwnd);
    return 1;
}
static uint32_t api_WinQuerySysValue(void)    /* (hwndDesktop, iSysValue) */
{
    switch (A(1)) {
    case 20: case 36: return 1920;           /* SV_CXSCREEN, SV_CXFULLSCREEN */
    case 21: case 37: return 1080;
    case 1: return 400;                      /* SV_DBLCLKTIME */
    case 4: case 5: return 4;                /* size border */
    case 26: case 27: return 1;              /* border */
    case 28: case 29: return 3;              /* dialog frame */
    case 30: return 22;                      /* title bar */
    case 22: case 23: return 16;             /* scroll bars */
    case 33: return 22;
    case 35: return 22;                      /* menu */
    case 38: case 39: return 32;             /* icon */
    case 40: case 41: return 32;             /* pointer */
    case 43: return 3;                       /* mouse buttons */
    }
    return 0;
}
static uint32_t api_WinSetFocus(void) { if (win(A(1))) set_focus(A(1)); return 1; }
static uint32_t api_WinQueryFocus(void) { return pm_focus; }
static uint32_t api_WinQueryDesktopWindow(void) { return HWND_DESKTOP_; }
static uint32_t api_WinAlarm(void) { return 1; }
static uint32_t api_WinQueryWindowPtr(void) { return api_WinQueryWindowULong(); }
static uint32_t api_WinSetWindowPtr(void) { return api_WinSetWindowULong(); }
static uint32_t api_WinStartTimer(void)       /* (hab, hwnd, idTimer, dtTimeout) */
{
    uint32_t id = A(2);
    for (int i = 0; i < 64; i++)
        if (pmtimers[i].used && pmtimers[i].hwnd == A(1) && pmtimers[i].id == id) { pmtimers[i].ms = A(3) ? A(3) : 1; pmtimers[i].next = ms_now() + pmtimers[i].ms; return id; }
    for (int i = 0; i < 64; i++)
        if (!pmtimers[i].used) {
            if (!A(1) && !id) id = 0x7F00 + (uint32_t)i;
            pmtimers[i] = (pmtimer_t){ 1, A(1), id, A(3) ? A(3) : 1, ms_now() + (A(3) ? A(3) : 1) };
            pthread_cond_broadcast(&gcv);
            return id;
        }
    return 0;
}
static uint32_t api_WinStopTimer(void)
{
    for (int i = 0; i < 64; i++)
        if (pmtimers[i].used && pmtimers[i].hwnd == A(1) && pmtimers[i].id == A(2)) { pmtimers[i].used = 0; return 1; }
    return 0;
}

static uint32_t sys_pointer(uint32_t id);
static void ptr_draw(pmwin_t *t, int x, int y, pmwin_t *p, uint32_t fs);
/* message boxes: a frame of their own, modal (the program's other
 * windows still get their messages meanwhile) */
static const char *const mb_labels[9][3] = {
    { "OK" }, { "OK", "Cancel" }, { "Retry", "Cancel" }, { "Abort", "Retry", "Ignore" }, { "Yes", "No" },
    { "Yes", "No", "Cancel" }, { "Cancel" }, { "Enter" }, { "Enter", "Cancel" },
};
static const int mb_ids[9][3] = { { 1 }, { 1, 2 }, { 4, 2 }, { 3, 4, 5 }, { 6, 7 }, { 6, 7, 2 }, { 2 }, { 9 }, { 9, 2 } };
static int mb_count(int k) { int n = 0; while (n < 3 && mb_labels[k][n]) n++; return n; }
static void mb_button_rect(pmwin_t *w, int k, int i, int *x0, int *x1)
{
    int n = mb_count(k), bw = 80, gap = 12, tot = n * bw + (n - 1) * gap;
    *x0 = (w->cx - tot) / 2 + i * (bw + gap);
    *x1 = *x0 + bw;
}
static uint32_t mb_proc(pmwin_t *w, uint32_t msg, uint32_t mp1, uint32_t mp2)
{
    int k = (int)(w->mb_style & 15) < 9 ? (int)(w->mb_style & 15) : 0, n = mb_count(k);
    int def = (int)((w->mb_style >> 8) & 3);
    if (def >= n) def = 0;
    switch (msg) {
    case WM_PAINT: {
        w->invalid = 0;
        fill(w, 0, 0, w->cx, w->cy, 0xCCCCCC);
        int icon = (int)((w->mb_style >> 4) & 7), tx = 18;          /* MB_ICONQUESTION, EXCLAMATION, ASTERISK, HAND */
        static const uint32_t sptr[5] = { 0, 12, 14, 11, 13 };
        if (icon >= 1 && icon <= 4) { pmwin_t *p = win(sys_pointer(sptr[icon])); if (p) ptr_draw(w, 14, w->cy - 14 - 32, p, 0); tx = 58; }
        /* the text, line by line */
        int y = w->cy - 18 - pm_ascent;
        const char *s = w->text;
        while (*s) {
            const char *e = strchr(s, '\n');
            int l = e ? (int)(e - s) : (int)strlen(s);
            text_at(w, tx, y, s, l, 0x000000);
            y -= pm_lineh;
            s += l + (e ? 1 : 0);
        }
        for (int i = 0; i < n; i++) {
            int x0, x1;
            mb_button_rect(w, k, i, &x0, &x1);
            fill(w, x0, 12, x1, 38, 0xDDDDDD);
            line(w, x0, 38, x1 - 1, 38, 0xFFFFFF); line(w, x0, 12, x0, 38, 0xFFFFFF);
            line(w, x0, 12, x1 - 1, 12, 0x606060); line(w, x1 - 1, 12, x1 - 1, 38, 0x606060);
            if (i == def) { line(w, x0 - 1, 11, x1, 11, 0x000000); line(w, x0 - 1, 39, x1, 39, 0x000000); line(w, x0 - 1, 11, x0 - 1, 39, 0); line(w, x1, 11, x1, 39, 0); }
            const char *l = mb_labels[k][i];
            text_at(w, x0 + (x1 - x0 - text_w(l, (int)strlen(l))) / 2, 20, l, (int)strlen(l), 0x000000);
        }
        present(w);
        return 0;
    }
    case WM_BUTTON1DOWN: {
        int x = (int16_t)(mp1 & 0xFFFF), y = (int16_t)(mp1 >> 16);
        for (int i = 0; i < n; i++) {
            int x0, x1;
            mb_button_rect(w, k, i, &x0, &x1);
            if (x >= x0 && x < x1 && y >= 12 && y < 38) w->mb_result = mb_ids[k][i];
        }
        return 1;
    }
    case WM_CHAR: {
        if (mp1 & KC_KEYUP) return 1;
        int ch = (int)(mp2 & 0xFFFF), vk = (int)(mp2 >> 16);
        if (ch == '\r' || vk == 0x1e || vk == 0x08) w->mb_result = mb_ids[k][def];
        else if (ch == 27 || vk == 0x0f) w->mb_result = mb_ids[k][n - 1];
        else for (int i = 0; i < n; i++)
            if (toupper(ch) == mb_labels[k][i][0]) w->mb_result = mb_ids[k][i];
        return 1;
    }
    case WM_CLOSE: w->mb_result = mb_ids[k][n - 1]; return 0;
    }
    return 0;
}
static uint32_t pm_msgbox(const char *text, const char *cap, uint32_t style);
static uint32_t api_WinMessageBox(void)       /* (hwndParent, hwndOwner, pszText, pszCaption, idWindow, flStyle) */
{
    return pm_msgbox(gs(A(2)), A(3) ? gs(A(3)) : 0, A(5));
}
static uint32_t pm_msgbox(const char *text, const char *cap, uint32_t style)
{
    int k = (int)(style & 15) < 9 ? (int)(style & 15) : 0;
    int def = (int)((style >> 8) & 3);
    if (def >= mb_count(k)) def = 0;
    if (!pmb || !myq()) {                    /* no PM: on stderr, the default answer */
        fprintf(stderr, "%s: %s\n", cap ? cap : "Error", text ? text : "");
        return (uint32_t)mb_ids[k][def];
    }
    pmwin_t *w = win_new(0, "#msgbox", 0, 0);
    if (!w) return 0xFFFF;
    w->top = 1;
    w->fixed = 1;
    w->iproc = mb_proc;
    w->mb_style = style;
    snprintf(w->text, sizeof w->text, "%s", text ? text : "");
    /* the size: the longest line, the lines */
    int lines = 1, widest = 0;
    for (const char *s = w->text; ; ) {
        const char *e = strchr(s, '\n');
        int l = e ? (int)(e - s) : (int)strlen(s), tw = text_w(s, l);
        if (tw > widest) widest = tw;
        if (!e) break;
        lines++;
        s = e + 1;
    }
    int cx = widest + 36, cy = lines * pm_lineh + 72;
    if ((style >> 4) & 7) { cx += 40; if (cy < 32 + 28 + 48) cy = 32 + 28 + 48; }   /* (the icon's room) */
    if (cx < 3 * 92 + 24) cx = 3 * 92 + 24;
    frame_pixels(w, cx, cy);
    snprintf(w->mb_caption, sizeof w->mb_caption, "%s", cap && *cap ? cap : "Message");
    if (pmb->title) pmb->title(w);
    uint32_t pfocus = pm_focus;
    win_show(w, 1);
    pm_focus = w->hwnd;
    w->invalid = 1;
    pmq_t *q = myq();
    uint32_t hwnd = w->hwnd;
    while (!w->mb_result) {
        pmmsg_t m;
        pm_wait_msg(q, 0, 0, 0, &m);
        if (m.msg == WM_QUIT) { post(0, 0, WM_QUIT, 0, 0); w->mb_result = 0xFFFF; break; }   /* (again, for the loop outside) */
        pmwin_t *t = win(m.hwnd);
        if (t) pm_send(t, m.msg, m.mp1, m.mp2);
        if (!win(hwnd)) break;
    }
    int r = w->mb_result ? w->mb_result : 0xFFFF;
    win_destroy(w);
    pm_focus = pfocus;
    if (trace) fprintf(stderr, "[pm] WinMessageBox \"%s\": %d\n", text ? text : "", r);
    return (uint32_t)r;
}

/* ── PMGPI ───────────────────────────────────────────────────────────── */
static void ps_to_dev(pmps_t *ps, double x, double y, int *dx, int *dy);
static void ps_from_dev(pmps_t *ps, double x, double y, int *wx, int *wy);
static int ps_dpm(pmps_t *ps);
static uint32_t api_GpiSetColor(void) { pmps_t *ps = ps_get(A(0)); if (!ps) return 0; ps->color = (int32_t)A(1); return 1; }
static uint32_t api_GpiQueryColor(void) { pmps_t *ps = ps_get(A(0)); return ps ? (uint32_t)ps->color : 0xFFFFFF01u; }
static uint32_t api_GpiSetBackColor(void) { pmps_t *ps = ps_get(A(0)); if (!ps) return 0; ps->back = (int32_t)A(1); return 1; }
static uint32_t api_GpiQueryCurrentPosition(void)
{
    pmps_t *ps = ps_get(A(0));
    if (!ps) return 0;
    p32(A(1), (uint32_t)ps->x); p32(A(1) + 4, (uint32_t)ps->y);
    return 1;
}
static uint32_t api_GpiSetPel(void)
{
    pmps_t *ps = ps_get(A(0));
    pmwin_t *w = ps_win(ps);
    if (!w || !ok(A(1), 8)) return 0;
    int dx, dy;
    ps_to_dev(ps, (int32_t)g32(A(1)), (int32_t)g32(A(1) + 4), &dx, &dy);
    pel(w, dx, dy, pm_rgb(ps->color, ps->rgb, 0));
    return 1;
}
static uint32_t char_string(pmps_t *ps, int n, uint32_t ps_str)
{
    pmwin_t *w = ps_win(ps);
    char *s = gp(ps_str, n > 0 ? (uint32_t)n : 1);
    if (!w || !s) return 0;
    pmfont_t *save = pmf;
    pmf = ps_font(ps);
    int dpm = ps_dpm(ps);
    if (!ps->font && dpm != 3780) pmf = font_get(pmf->face, (int)lround(15.0 * dpm / 3780));   /* (a printer's: the default font at its resolution) */
    int dx, dy, wx, wy;
    ps_to_dev(ps, ps->x, ps->y, &dx, &dy);                   /* (the world's position on the device; the end back in the world's) */
    dx = text_at(w, dx, dy, s, n, pm_rgb(ps->color, ps->rgb, 0));
    ps_from_dev(ps, dx, dy, &wx, &wy);
    ps->x = wx; ps->y = wy;
    pmf = save;
    return 1;
}
static uint32_t api_GpiCharString(void) { pmps_t *ps = ps_get(A(0)); return ps ? char_string(ps, (int32_t)A(1), A(2)) : 0; }
static uint32_t api_GpiCharStringAt(void)     /* (hps, pptlStart, lCount, pchString) */
{
    pmps_t *ps = ps_get(A(0));
    if (!ps || !ok(A(1), 8)) return 0;
    ps->x = (int32_t)g32(A(1)); ps->y = (int32_t)g32(A(1) + 4);
    return char_string(ps, (int32_t)A(2), A(3));
}
static uint32_t api_GpiErase(void)
{
    pmps_t *ps = ps_get(A(0));
    pmwin_t *w = ps_win(ps);
    if (!w) return 0;
    fill(w, 0, 0, w->cx, w->cy, 0xFFFFFF);
    return 1;
}
static uint32_t api_GpiCreateLogColorTable(void)   /* (hps, flOptions, lFormat, lStart, lCount, alTable) */
{
    pmps_t *ps = ps_get(A(0));
    if (!ps) return 0;
    ps->rgb = A(2) == 3;                     /* LCOLF_RGB: colours are RGB */
    return 1;
}
/* a font's FONTMETRICS (n bytes at p) */
static void put_metrics(uint32_t p, uint32_t n, pmfont_t *f, const char *family, const char *face, int outline, int pt, uint32_t match)
{
    uint8_t fm[228] = { 0 };
    pmfont_t *save = pmf;
    pmf = f;
    snprintf((char *)fm, 32, "%s", family);
    snprintf((char *)fm + 32, 32, "%s", face);
    int avg = text_w("x", 1), maxw = text_w("W", 1);
    int32_t v[12] = { f->px, f->ascent * 2 / 3, f->ascent, f->descent, f->ascent * 2 / 3, f->descent,
                      f->ascent + f->descent - f->px > 0 ? f->ascent + f->descent - f->px : 0, 2, avg, maxw, maxw, f->ascent + f->descent };
    memcpy(fm + 68, v, sizeof v);           /* lEmHeight ... lMaxBaselineExt */
    uint16_t sh[16] = { 0, 0, 0, (f->face & 1) ? 700 : 400, 5, 96, 96, 32, 255, '?', ' ', (int16_t)(pt * 10), outline ? 10 : (int16_t)(pt * 10), outline ? 7200 : (int16_t)(pt * 10),
                        (uint16_t)((f->face & 2) ? 1 : 0), (uint16_t)(outline ? 1 : 0) };   /* ... fsType (FM_TYPE_FIXED), fsDefn (FM_DEFN_OUTLINE) */
    memcpy(fm + 116, sh, sizeof sh);
    uint32_t sel = (f->face & 1) ? 0x20 : 0;
    memcpy(fm + 148, &sel, 2);
    memcpy(fm + 204, &match, 4);            /* lMatch */
    memcpy(C.mem + p, fm, n < sizeof fm ? n : sizeof fm);
    pmf = save;
}
static uint32_t api_GpiQueryFontMetrics(void) /* (hps, lMetricsLength, pfmMetrics) */
{
    uint32_t n = A(1), p = A(2);
    pmps_t *ps = ps_get(A(0));
    if (!ok(p, n)) return 0;
    memset(C.mem + p, 0, n);
    pmfont_t *f = ps_font(ps);
    static const char *const fam[4] = { "Helvetica", "Helvetica", "Courier", "Courier" };
    static const char *const face[4] = { "Helv", "Helv Bold", "Courier", "Courier Bold" };
    put_metrics(p, n, f, fam[f->face], face[f->face], ps && ps->lf && ps->lcid && ps->lf[ps->lcid].outline, f->px * 2 / 3, 0);
    return 1;
}
static uint32_t api_GpiQueryTextBox(void)     /* (hps, lCount1, pchString, lCount2, aptlPoints) */
{
    char *s = gp(A(2), A(1) ? A(1) : 1);
    uint32_t n = A(3), p = A(4);
    if (!s || !ok(p, n * 8)) return 0;
    pmfont_t *save = pmf;
    pmf = ps_font(ps_get(A(0)));
    int w = text_w(s, (int32_t)A(1));
    int32_t pts[5][2] = { { 0, pm_ascent }, { 0, -pm_descent }, { w, pm_ascent }, { w, -pm_descent }, { w, 0 } };   /* TXTBOX_TOPLEFT ... CONCAT */
    for (uint32_t i = 0; i < n && i < 5; i++) { p32(p + 8 * i, (uint32_t)pts[i][0]); p32(p + 8 * i + 4, (uint32_t)pts[i][1]); }
    pmf = save;
    return 1;
}

#include "pm_ctl.inc"
#include "pm_gpi.inc"

/* ── logical fonts, presentation parameters ─────────────────────────── */
/* OS/2's faces here: (family, face, ours, outline, points) */
static const struct { const char *family, *face; int ours, outline, pt; } font_list[] = {
    { "Helvetica", "Helvetica", 0, 1, 12 }, { "Helvetica", "Helvetica Bold", 1, 1, 12 },
    { "Courier", "Courier", 2, 1, 12 }, { "Courier", "Courier Bold", 3, 1, 12 },
    { "Times New Roman", "Times New Roman", 0, 1, 12 }, { "Times New Roman", "Times New Roman Bold", 1, 1, 12 },
    { "System Proportional", "System Proportional", 0, 0, 10 }, { "System Monospaced", "System Monospaced", 2, 0, 10 },
    { "System VIO", "System VIO", 2, 0, 10 },
    { "Helv", "Helv", 0, 0, 8 }, { "Helv", "Helv", 0, 0, 10 }, { "Helv", "Helv", 0, 0, 12 }, { "Helv", "Helv", 0, 0, 14 },
    { "Helv", "Helv", 0, 0, 18 }, { "Helv", "Helv", 0, 0, 24 },
    { "Courier", "Courier", 2, 0, 8 }, { "Courier", "Courier", 2, 0, 10 }, { "Courier", "Courier", 2, 0, 12 },
    { "Tms Rmn", "Tms Rmn", 0, 0, 10 }, { "Tms Rmn", "Tms Rmn", 0, 0, 12 },
};
#define NFONTLIST ((int)(sizeof font_list / sizeof *font_list))
static uint32_t api_GpiQueryFonts(void)       /* (hps, flOptions, pszFacename, plReqFonts, lMetricsLength, afmMetrics): those not returned */
{
    char *want = A(2) ? gs(A(2)) : 0;
    uint32_t pn = A(3), len = A(4), arr = A(5);
    int req = (int32_t)g32(pn), got = 0, all = 0;
    for (int i = 0; i < NFONTLIST; i++) {
        if (want && *want && strcasecmp(want, font_list[i].face) && strcasecmp(want, font_list[i].family)) continue;
        all++;
        if (got < req && ok(arr + (uint32_t)got * len, len)) {
            put_metrics(arr + (uint32_t)got * len, len, font_get(font_list[i].ours, font_list[i].pt * 3 / 2), font_list[i].family,
                        font_list[i].face, font_list[i].outline, font_list[i].pt, (uint32_t)i + 1);
            got++;
        }
    }
    p32(pn, (uint32_t)got);
    return (uint32_t)(all - got);
}
static uint32_t api_GpiCreateLogFont(void)    /* (hps, pName, lLcid, pfat): FONT_MATCH */
{
    pmps_t *ps = ps_get(A(0));
    uint32_t id = A(2), fa = A(3);
    if (!ps || id < 1 || id > 254 || !ok(fa, 56)) return 0;
    if (!ps->lf) ps->lf = calloc(256, sizeof *ps->lf);
    char face[33];
    memcpy(face, C.mem + fa + 8, 32); face[32] = 0;
    uint32_t sel = g16(fa + 2), use = g16(fa + 54), match = g32(fa + 4);
    int32_t mbe = (int32_t)g32(fa + 44);
    int k = -1;
    if (match >= 1 && (int)match <= NFONTLIST && !strcasecmp(face, font_list[match - 1].face)) k = (int)match - 1;   /* (lMatch: GpiQueryFonts's) */
    pmfont_t *nf = font_named(face, (sel & 0x20) != 0);
    lfont_t *l = &ps->lf[id];
    l->used = 1;
    l->face = nf->face;
    l->outline = (use & 4) || mbe <= 0 || (k >= 0 && font_list[k].outline);   /* FATTR_FONTUSE_OUTLINE */
    l->px = mbe > 0 ? mbe : k >= 0 ? font_list[k].pt * 3 / 2 : 15;
    return 2;                                       /* FONT_MATCH */
}
static void ps_refont(pmps_t *ps)
{
    lfont_t *l = ps->lf && ps->lcid ? &ps->lf[ps->lcid] : 0;
    ps->font = l && l->used ? font_get(l->face, l->outline && ps->boxpx ? ps->boxpx : l->px) : 0;
}
static uint32_t api_GpiSetCharSet(void)       /* (hps, llcid) */
{
    pmps_t *ps = ps_get(A(0));
    if (!ps) return 0;
    ps->lcid = (int)A(1) >= 0 && A(1) < 255 ? (int)A(1) : 0;
    ps_refont(ps);
    return 1;
}
static uint32_t api_GpiQueryCharSet(void) { pmps_t *ps = ps_get(A(0)); return ps ? (uint32_t)ps->lcid : 0; }
static uint32_t api_GpiSetCharBox(void)       /* (hps, psizfxBox: FIXED cx, cy) */
{
    pmps_t *ps = ps_get(A(0));
    if (!ps || !ok(A(1), 8)) return 0;
    int cy = (int)((int32_t)g32(A(1) + 4) >> 16);
    ps->boxpx = cy > 0 ? cy : 0;
    ps_refont(ps);
    return 1;
}
static uint32_t api_GpiQueryCharBox(void)
{
    pmps_t *ps = ps_get(A(0));
    if (!ps || !ok(A(1), 8)) return 0;
    pmfont_t *f = ps_font(ps);
    p32(A(1), (uint32_t)(f->px * 2 / 3) << 16); p32(A(1) + 4, (uint32_t)f->px << 16);
    return 1;
}
static uint32_t api_GpiDeleteSetId(void)      /* (hps, lLcid: LCID_ALL -1) */
{
    pmps_t *ps = ps_get(A(0));
    if (!ps || !ps->lf) return ps != 0;
    if ((int32_t)A(1) == -1) memset(ps->lf, 0, 256 * sizeof *ps->lf);
    else if (A(1) < 255) ps->lf[A(1)].used = 0;
    if ((int32_t)A(1) == -1 || (int)A(1) == ps->lcid) { ps->lcid = 0; ps->font = 0; }
    return 1;
}
static uint32_t api_GpiQueryNumberSetIds(void) { pmps_t *ps = ps_get(A(0)); int n = 0; for (int i = 1; ps && ps->lf && i < 255; i++) n += ps->lf[i].used; return (uint32_t)n; }
static uint32_t api_GpiQueryWidthTable(void)  /* (hps, lFirstChar, lCount, alData) */
{
    uint32_t first = A(1), n = A(2), p = A(3);
    if (!ok(p, n * 4)) return 0;
    pmfont_t *save = pmf;
    pmf = ps_font(ps_get(A(0)));
    for (uint32_t i = 0; i < n; i++) p32(p + 4 * i, (uint32_t)glyph((unsigned char)(first + i))->adv);
    pmf = save;
    return 1;
}
static void pp_set(pmwin_t *w, uint32_t id, uint32_t cb, uint32_t pv)
{
    if (id == 15 && cb) {                           /* PP_FONTNAMESIZE: "10.Helv" */
        char *s = gp(pv, cb);
        if (!s) return;
        snprintf(w->font_name, sizeof w->font_name, "%.*s", (int)cb, s);
        w->font = font_named(w->font_name, 0);
    } else if ((id == 1 || id == 3) && cb >= 4) {   /* PP_FOREGROUNDCOLOR, PP_BACKGROUNDCOLOR: RGB */
        if (id == 1) { w->pp_fg = g32(pv) & 0xFFFFFF; w->has_fg = 1; } else { w->pp_bg = g32(pv) & 0xFFFFFF; w->has_bg = 1; }
    } else if ((id == 2 || id == 4) && cb >= 4) {   /* ... the INDEX ones */
        uint32_t c = pm_rgb((int32_t)g32(pv), 0, id == 4);
        if (id == 2) { w->pp_fg = c; w->has_fg = 1; } else { w->pp_bg = c; w->has_bg = 1; }
    }
    invalidate(w);
}
static void pp_apply(pmwin_t *w, uint32_t pp)       /* a PRESPARAMS: cb, then (id, cb, value)s */
{
    if (!pp || !ok(pp, 4)) return;
    uint32_t cb = g32(pp), p = pp + 4, end = pp + 4 + cb;
    while (p + 8 <= end) {
        uint32_t id = g32(p), n = g32(p + 4);
        pp_set(w, id, n, p + 8);
        p += 8 + n;
    }
}
static uint32_t api_WinSetPresParam(void) { pmwin_t *w = win(A(0)); if (!w) return 0; pp_set(w, A(1), A(2), A(3)); return 1; }   /* (hwnd, id, cbParam, pbParam) */
static uint32_t api_WinQueryPresParam(void)   /* (hwnd, id1, id2, pulId, cbBuf, pbBuf, fs): bytes */
{
    pmwin_t *w = win(A(0));
    uint32_t ids[2] = { A(1), A(2) }, cb = A(4), b = A(5);
    for (int k = 0; w && k < 2; k++) {
        uint32_t id = ids[k];
        if (id == 15 && w->font_name[0]) {
            uint32_t n = (uint32_t)strlen(w->font_name) + 1;
            if (n > cb || !ok(b, n)) return 0;
            memcpy(C.mem + b, w->font_name, n);
            if (A(3)) p32(A(3), id);
            return n;
        }
        if ((id == 1 && w->has_fg) || (id == 3 && w->has_bg)) {
            if (cb < 4 || !ok(b, 4)) return 0;
            p32(b, id == 1 ? w->pp_fg : w->pp_bg);
            if (A(3)) p32(A(3), id);
            return 4;
        }
    }
    return 0;
}
static uint32_t api_WinRemovePresParam(void)
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    if (A(1) == 15) { w->font = 0; w->font_name[0] = 0; }
    if (A(1) == 1) w->has_fg = 0;
    if (A(1) == 3) w->has_bg = 0;
    invalidate(w);
    return 1;
}
static pmwin_t *ptr_get(uint32_t h);
#include "pm_cnr.inc"
#include "pm_vs.inc"
#include "pm_drag.inc"
#include "pm_geo.inc"
#include "pm_prf.inc"
#include "pm_fdlg.inc"
#include "pm_print.inc"

/* ── the clipboard (WinOpenClipbrd & co.): CF_TEXT, CF_BITMAP; on the
 * desktop the Wayland one (pm_wl.inc), so text goes between programs ── */
static char *clip_text;                    /* (the program's form: CR LF lines, 8-bit characters) */
static int clip_ours;
static uint32_t clip_bitmap;
static void clip_put(const char *s)        /* (s: as programs have it) */
{
    free(clip_text);
    clip_text = strdup(s);
    clip_ours = 1;
    if (pmb && pmb->clip_set) {
        size_t n = strlen(s);
        char *lf = malloc(n + 1), *o = lf;
        for (size_t i = 0; i < n; i++) if (!(s[i] == '\r' && s[i + 1] == '\n')) *o++ = s[i];
        *o = 0;
        pmb->clip_set(lf);
        free(lf);
    }
}
static const char *clip_take(void)         /* the clipboard's text, the program's form, or 0 */
{
    if (!clip_ours && pmb && pmb->clip_get) {
        char *t = pmb->clip_get();
        if (t) {                                 /* (LF lines: CR LF, as OS/2's) */
            size_t n = strlen(t), k = 0;
            char *c = malloc(2 * n + 1);
            for (size_t i = 0; i < n; i++) { if (t[i] == '\n' && (i == 0 || t[i - 1] != '\r')) c[k++] = '\r'; c[k++] = t[i]; }
            c[k] = 0;
            free(t);
            free(clip_text);
            clip_text = c;
        }
    }
    return clip_text;
}
static __attribute__((unused)) void clip_lost(void) { clip_ours = 0; }   /* (the backend: another program has it now) */
static uint32_t api_WinOpenClipbrd(void) { return 1; }
static uint32_t api_WinCloseClipbrd(void) { return 1; }
static uint32_t api_WinEmptyClipbrd(void) { free(clip_text); clip_text = 0; clip_bitmap = 0; clip_ours = 1; return 1; }
static uint32_t api_WinSetClipbrdData(void)       /* (hab, ulData, fmt, rgfFmtInfo) */
{
    if (A(2) == 1) { char *s = gs(A(1)); if (!s) return 0; clip_put(s); return 1; }   /* CF_TEXT: a pointer to the text */
    if (A(2) == 2) { clip_bitmap = A(1); return 1; }                                  /* CF_BITMAP: a bitmap's handle */
    return 0;
}
static uint32_t api_WinQueryClipbrdData(void)     /* (hab, fmt): CF_TEXT's in our memory, for the program */
{
    if (A(1) == 2) return clip_bitmap;
    if (A(1) != 1) return 0;
    const char *t = clip_take();
    if (!t) return 0;
    uint32_t n = (uint32_t)strlen(t) + 1, a = galloc(n);
    if (!a) return 0;
    memcpy(C.mem + a, t, n);
    return a;
}
static uint32_t api_WinQueryClipbrdFmtInfo(void)  /* (hab, fmt, prgfFmtInfo) */
{
    int have = A(1) == 1 ? clip_take() != 0 : A(1) == 2 ? clip_bitmap != 0 : 0;
    if (have && A(2)) p32(A(2), A(1) == 1 ? 0x0400 : 0x0200);   /* CFI_POINTER, CFI_HANDLE */
    return (uint32_t)have;
}
static uint32_t api_WinEnumClipbrdFmts(void)      /* (hab, fmt): the next one there */
{
    if (A(1) < 1 && clip_take()) return 1;
    if (A(1) < 2 && clip_bitmap) return 2;
    return 0;
}
static uint32_t api_WinSetClipbrdOwner(void) { return 1; }
static uint32_t api_WinQueryClipbrdOwner(void) { return 0; }

/* the pointer over our windows (WinSetPointer) */
static void pm_set_pointer(uint32_t hptr)
{
    pmwin_t *p = ptr_get(hptr);
    if (!p || pm_pointer == hptr) return;
    pm_pointer = hptr;
    if (pmb && pmb->cursor) pmb->cursor(p);
}

/* ── the pointer, the keys' state, points between windows ── */
static void desk_xy(pmwin_t *w, int *x, int *y)            /* w's bottom left on the desktop */
{
    int ax = 0, ay = 0;
    pmwin_t *f = w ? frame_of(w) : 0;
    if (w) win_abs(w, &ax, &ay);
    *x = ax + (f ? f->x : 0); *y = ay + (f ? f->y : 0);
}
static uint32_t api_WinGetKeyState(void)          /* (hwndDesktop, vk): 0x8000 down, as of the message being handled */
{
    switch (A(1)) {
    case 0x01: case 0x02: case 0x03: return pm_sync_buttons & (1 << (A(1) - 1)) ? 0x8000 : 0;   /* VK_BUTTON1..3 */
    case 0x09: return pm_sync_kc & KC_SHIFT ? 0x8000 : 0;   /* VK_SHIFT */
    case 0x0A: return pm_sync_kc & KC_CTRL ? 0x8000 : 0;    /* VK_CTRL */
    case 0x0B: return pm_sync_kc & KC_ALT ? 0x8000 : 0;     /* VK_ALT */
    }
    return 0;
}
static uint32_t api_WinGetPhysKeyState(void) { return 0; }
static uint32_t api_WinQueryPointerPos(void)      /* (hwndDesktop, pptl): on the desktop */
{
    int fx, fy;
    desk_xy(win(pm_mouse_frame), &fx, &fy);
    p32(A(1), (uint32_t)(fx + mouse_x)); p32(A(1) + 4, (uint32_t)(fy + mouse_y));
    return 1;
}
static uint32_t api_WinSetPointerPos(void) { return 1; }  /* (the desktop's pointer: the compositor's to move) */
static uint32_t api_WinShowPointer(void) { return 1; }
/* WinMapWindowPoints (from, to, aptl, count): from one window's coordinates to another's (HWND_DESKTOP: the desktop's) */
static uint32_t api_WinMapWindowPoints(void)
{
    int fx = 0, fy = 0, tx = 0, ty = 0;
    if (A(0) != HWND_DESKTOP_) desk_xy(win(A(0)), &fx, &fy);
    if (A(1) != HWND_DESKTOP_) desk_xy(win(A(1)), &tx, &ty);
    int32_t n = (int32_t)A(3);
    if (n < 0 || !ok(A(2), 8u * (uint32_t)n)) return 0;
    for (int32_t i = 0; i < n; i++) {
        uint32_t p = A(2) + 8u * (uint32_t)i;
        p32(p, (uint32_t)((int32_t)g32(p) + fx - tx)); p32(p + 4, (uint32_t)((int32_t)g32(p + 4) + fy - ty));
    }
    return 1;
}
/* WinWindowFromPoint (parent, pptl in its coordinates, fChildren): the child there (and its children's, when asked) */
static uint32_t api_WinWindowFromPoint(void)
{
    if (!ok(A(1), 8)) return 0;
    int x = (int32_t)g32(A(1)), y = (int32_t)g32(A(1) + 4);
    if (A(0) == HWND_DESKTOP_) {                                 /* (a frame of ours there; the topmost-ish: the last made) */
        for (int i = MAXWIN - 1; i >= 0; i--) {
            pmwin_t *f = &pmwins[i];
            if (f->used && f->top && f->visible && x >= f->x && y >= f->y && x < f->x + f->cx && y < f->y + f->cy) {
                if (!A(2)) return f->hwnd;
                pmwin_t *c = win_at(f, x - f->x, y - f->y);
                return c ? c->hwnd : f->hwnd;
            }
        }
        return HWND_DESKTOP_;
    }
    pmwin_t *p = win(A(0));
    if (!p || x < 0 || y < 0 || x >= p->cx || y >= p->cy) return 0;
    for (pmwin_t *cur = p;;) {
        pmwin_t *hit = 0;
        for (int i = MAXWIN - 1; i >= 0; i--) {
            pmwin_t *c = &pmwins[i];
            if (c->used && c->parent == cur && c->visible && x >= c->x && y >= c->y && x < c->x + c->cx && y < c->y + c->cy) { hit = c; break; }
        }
        if (!hit) return cur->hwnd;
        if (!A(2)) return hit->hwnd;
        x -= hit->x; y -= hit->y; cur = hit;
    }
}
static uint32_t api_WinQueryClassName(void)        /* (hwnd, cchMax, pch): its length */
{
    pmwin_t *w = win(A(0));
    if (!w || !A(1) || !ok(A(2), A(1))) return 0;
    snprintf((char *)C.mem + A(2), A(1), "%s", w->cls);
    return (uint32_t)strlen((char *)C.mem + A(2));
}
static uint32_t api_WinQueryCapture(void) { return pm_capture; }
static uint32_t api_WinSetCapture(void) { pm_capture = A(1) && win(A(1)) ? A(1) : 0; return 1; }   /* (hwndDesktop, hwnd; 0: none) */
static uint32_t api_WinIsChild(void) { for (pmwin_t *w = win(A(0)); w; w = w->parent) if (w->hwnd == A(1)) return 1; return 0; }
static uint32_t api_WinQueryWindowProcess(void)    /* (hwnd, ppid, ptid) */
{
    pmwin_t *w = win(A(0));
    if (!w) return 0;
    if (A(1)) p32(A(1), (uint32_t)getpid());
    if (A(2)) p32(A(2), w->tid);
    return 1;
}
static uint32_t api_WinFlashWindow(void) { return 1; }
static uint32_t api_WinSetSysColors(void) { return 1; }
