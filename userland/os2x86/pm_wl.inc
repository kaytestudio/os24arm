/* userland/os2x86/pm_wl.inc - Presentation Manager on Wayland (pm.inc's
 * backend on the desktop): each PM frame window an xdg-shell toplevel
 * (the Workplace Shell draws its OS/2 frame), its pixels a wl_shm buffer;
 * the pointer and the keyboard (xkbcommon) become PM's messages. The
 * Wayland events are read by a thread of their own, which takes the lock
 * (gil) for what it changes in PM. Built when the sysroot has
 * wayland-client and xkbcommon (userland/build-tests.sh). */
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"

static struct wl_display *wld;
static struct wl_compositor *wl_comp;
static struct wl_shm *wl_shm_;
static struct xdg_wm_base *wl_wm;
static struct wl_seat *wl_seat_;
static struct wl_pointer *wl_ptr;
static struct wl_keyboard *wl_kbd;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;
static struct xkb_state *xkb_st;
static pmwin_t *wl_ptr_frame, *wl_kbd_frame;
static int wl_px, wl_py;
static uint32_t wl_serial, wl_enter_serial;   /* (the last input's: for the selection; the pointer's enter: its shape) */
static struct wl_data_device_manager *wl_ddm;
static struct wl_data_device *wl_dd;
static void clip_lost(void);
static void ptr_argb(pmwin_t *p, uint32_t *out, int *hx, int *hy);

typedef struct {
    struct wl_surface *s;
    struct xdg_surface *xs;
    struct xdg_toplevel *tl;
    int configured, dirty;
} wlwin_t;

static pmwin_t *wl_frame_of(struct wl_surface *s)
{
    for (int i = 0; i < MAXWIN; i++)
        if (pmwins[i].used && pmwins[i].bk && ((wlwin_t *)pmwins[i].bk)->s == s) return &pmwins[i];
    return 0;
}

static void wl_buf_release(void *d, struct wl_buffer *b) { (void)d; wl_buffer_destroy(b); }
static const struct wl_buffer_listener wl_buf_l = { wl_buf_release };

/* the frame's pixels to the compositor (gil held) */
static void wl_present(pmwin_t *f)
{
    wlwin_t *k = f->bk;
    if (!k || !f->px) return;
    if (!k->configured) { k->dirty = 1; return; }
    size_t n = (size_t)f->pw * f->ph * 4;
    int fd = memfd_create("os2x86-pm", MFD_CLOEXEC);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)n)) { close(fd); return; }
    void *m = mmap(0, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return; }
    memcpy(m, f->px, n);
    munmap(m, n);
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm_, fd, (int32_t)n);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, f->pw, f->ph, f->pw * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b, &wl_buf_l, 0);
    wl_surface_attach(k->s, b, 0, 0);
    wl_surface_damage_buffer(k->s, 0, 0, f->pw, f->ph);
    wl_surface_commit(k->s);
    k->dirty = 0;
    wl_display_flush(wld);
}

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
    pmwin_t *f = d;
    pthread_mutex_lock(&gil);
    xdg_surface_ack_configure(xs, serial);
    if (f->used && f->bk) {
        wlwin_t *k = f->bk;
        k->configured = 1;
        wl_present(f);
    }
    pthread_mutex_unlock(&gil);
}
static const struct xdg_surface_listener xs_l = { xs_configure };
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
    (void)t; (void)st;
    pmwin_t *f = d;
    if (w <= 0 || h <= 0) return;
    pthread_mutex_lock(&gil);
    if (f->used && !f->fixed) pm_resize(f, w, h);     /* (dialogs, message boxes keep their size) */
    pthread_mutex_unlock(&gil);
}
static void tl_close(void *d, struct xdg_toplevel *t)
{
    (void)t;
    pthread_mutex_lock(&gil);
    pmwin_t *f = d;
    if (f->used) pm_close(f);
    pthread_mutex_unlock(&gil);
}
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *a) { (void)d; (void)t; (void)a; }
static const struct xdg_toplevel_listener tl_l = { tl_configure, tl_close, tl_bounds, tl_caps };

static void wl_show(pmwin_t *f, int on)
{
    wlwin_t *k = f->bk;
    if (on && !k) {
        k = calloc(1, sizeof *k);
        f->bk = k;
        k->s = wl_compositor_create_surface(wl_comp);
        k->xs = xdg_wm_base_get_xdg_surface(wl_wm, k->s);
        xdg_surface_add_listener(k->xs, &xs_l, f);
        k->tl = xdg_surface_get_toplevel(k->xs);
        xdg_toplevel_add_listener(k->tl, &tl_l, f);
        xdg_toplevel_set_title(k->tl, win_title(f));
        xdg_toplevel_set_app_id(k->tl, "os2x86");
        if (f->fixed) { xdg_toplevel_set_min_size(k->tl, f->pw, f->ph); xdg_toplevel_set_max_size(k->tl, f->pw, f->ph); }
        wl_surface_commit(k->s);
        wl_display_flush(wld);
    } else if (!on && k) {
        if (wl_ptr_frame == f) wl_ptr_frame = 0;
        if (wl_kbd_frame == f) wl_kbd_frame = 0;
        xdg_toplevel_destroy(k->tl);
        xdg_surface_destroy(k->xs);
        wl_surface_destroy(k->s);
        free(k);
        f->bk = 0;
        wl_display_flush(wld);
    }
}
static void wl_title(pmwin_t *f)
{
    wlwin_t *k = f->bk;
    if (k) { xdg_toplevel_set_title(k->tl, win_title(f)); wl_display_flush(wld); }
}
static int wl_idle(struct timespec *dl)
{
    if (dl) pthread_cond_timedwait(&gcv, &gil, dl);
    else pthread_cond_wait(&gcv, &gil);
    return 1;
}

/* ── the clipboard: the Wayland selection. Ours: a data source with the
 * text (UTF-8 for the others; the program's is Latin-1); another's: its
 * offer, read through a pipe when the program asks ── */
static pthread_mutex_t wl_clip_mx = PTHREAD_MUTEX_INITIALIZER;   /* (not the gil: the source's sends come on the event thread) */
static char *wl_clip_utf8;
static struct wl_data_source *wl_src;
static struct wl_data_offer *wl_sel;           /* the selection, another program's */
static int wl_sel_text;                         /* ... it has text: 2 UTF-8, 1 plain */
static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *m, int32_t fd)
{
    (void)d; (void)s; (void)m;
    pthread_mutex_lock(&wl_clip_mx);
    const char *t = wl_clip_utf8 ? wl_clip_utf8 : "";
    size_t n = strlen(t), o = 0;
    signal(SIGPIPE, SIG_IGN);
    while (o < n) { ssize_t k = write(fd, t + o, n - o); if (k <= 0) break; o += (size_t)k; }
    pthread_mutex_unlock(&wl_clip_mx);
    close(fd);
}
static void src_cancelled(void *d, struct wl_data_source *s)
{
    (void)d;
    pthread_mutex_lock(&gil);
    if (s == wl_src) { wl_src = 0; clip_lost(); }
    pthread_mutex_unlock(&gil);
    wl_data_source_destroy(s);
}
static void src_dnd_drop(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_dnd_finished(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener src_l = { src_target, src_send, src_cancelled, src_dnd_drop, src_dnd_finished, src_action };
static int wl_clip_pending;
static void wl_clip_offer(void);
static void wl_clip_set(const char *text)       /* (gil held) */
{
    if (!wl_ddm || !wl_dd) return;
    size_t n = strlen(text);
    char *u = malloc(2 * n + 1), *o = u;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)text[i];
        if (c < 0x80) *o++ = (char)c; else { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 0x3F)); }
    }
    *o = 0;
    pthread_mutex_lock(&wl_clip_mx);
    free(wl_clip_utf8);
    wl_clip_utf8 = u;
    pthread_mutex_unlock(&wl_clip_mx);
    if (!wl_serial) { wl_clip_pending = 1; return; }   /* (no input yet: the compositor wants an input's serial - offered at the first) */
    wl_clip_offer();
}
static void wl_clip_offer(void)                 /* (gil held) */
{
    wl_clip_pending = 0;
    struct wl_data_source *s = wl_data_device_manager_create_data_source(wl_ddm);
    wl_data_source_add_listener(s, &src_l, 0);
    wl_data_source_offer(s, "text/plain;charset=utf-8");
    wl_data_source_offer(s, "UTF8_STRING");
    wl_data_source_offer(s, "text/plain");
    wl_data_device_set_selection(wl_dd, s, wl_serial);
    wl_src = s;                                  /* (the old one: cancelled, by the compositor) */
    wl_display_flush(wld);
}
static char *wl_clip_get(void)                  /* (gil held: the offer stays) */
{
    if (!wl_sel || !wl_sel_text) return 0;
    int fd[2];
    if (pipe(fd)) return 0;
    wl_data_offer_receive(wl_sel, wl_sel_text == 2 ? "text/plain;charset=utf-8" : "text/plain", fd[1]);
    close(fd[1]);
    wl_display_flush(wld);
    size_t n = 0, cap = 4096;
    char *b = malloc(cap);
    for (;;) {                                   /* (a program that doesn't answer: two seconds) */
        struct pollfd pf = { fd[0], POLLIN, 0 };
        if (poll(&pf, 1, 2000) <= 0) break;
        if (n + 1024 > cap) b = realloc(b, cap *= 2);
        ssize_t k = read(fd[0], b + n, cap - n - 1);
        if (k <= 0) break;
        n += (size_t)k;
    }
    close(fd[0]);
    b[n] = 0;
    if (wl_sel_text == 2) {                      /* UTF-8: Latin-1 (the rest: '?') */
        size_t i = 0, k = 0;
        while (i < n) {
            uint8_t c = (uint8_t)b[i];
            if (c < 0x80) { b[k++] = (char)c; i++; continue; }
            int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            uint32_t cp = c & (0x3F >> (len - 1));
            for (int j = 1; j < len && i + (size_t)j < n; j++) cp = cp << 6 | ((uint8_t)b[i + (size_t)j] & 0x3F);
            b[k++] = cp < 0x100 ? (char)cp : '?';
            i += (size_t)len;
        }
        b[k] = 0;
    }
    return b;
}
static void off_offer(void *d, struct wl_data_offer *o, const char *m)
{
    (void)d;
    intptr_t f = (intptr_t)wl_data_offer_get_user_data(o);
    if (!strcmp(m, "text/plain;charset=utf-8") || !strcmp(m, "UTF8_STRING")) f = 2;
    else if (!strcmp(m, "text/plain") && f < 1) f = 1;
    wl_data_offer_set_user_data(o, (void *)f);
}
static void off_source_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static void off_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener off_l = { off_offer, off_source_actions, off_action };
static void dd_data_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) { (void)d; (void)dd; wl_data_offer_add_listener(o, &off_l, 0); }
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *f, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{ (void)d; (void)dd; (void)s; (void)f; (void)x; (void)y; if (o) wl_data_offer_destroy(o); }   /* (drag and drop: not yet) */
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
    (void)d; (void)dd;
    pthread_mutex_lock(&gil);
    if (wl_sel) wl_data_offer_destroy(wl_sel);
    wl_sel = o;
    wl_sel_text = o ? (int)(intptr_t)wl_data_offer_get_user_data(o) : 0;
    pthread_mutex_unlock(&gil);
}
static const struct wl_data_device_listener dd_l = { dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

/* ── the pointer's shape: a cursor surface ── */
static struct wl_surface *wl_cur;
static pmwin_t *wl_cur_ptr;
static void wl_cursor_apply(void)               /* (gil held) */
{
    pmwin_t *p = wl_cur_ptr;
    if (!wl_ptr || !wl_enter_serial || !p || !p->used || !p->px) return;
    if (!wl_cur) wl_cur = wl_compositor_create_surface(wl_comp);
    int w = p->pw, h = p->ph, hx = 0, hy = 0;
    size_t n = (size_t)w * h * 4;
    int fd = memfd_create("os2x86-ptr", MFD_CLOEXEC);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)n)) { close(fd); return; }
    uint32_t *m = mmap(0, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return; }
    ptr_argb(p, m, &hx, &hy);
    munmap(m, n);
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm_, fd, (int32_t)n);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b, &wl_buf_l, 0);
    wl_surface_attach(wl_cur, b, 0, 0);
    wl_surface_damage_buffer(wl_cur, 0, 0, w, h);
    wl_surface_commit(wl_cur);
    wl_pointer_set_cursor(wl_ptr, wl_enter_serial, wl_cur, hx, hy);
    wl_display_flush(wld);
}
static void wl_cursor(pmwin_t *p) { wl_cur_ptr = p; if (wl_ptr_frame) wl_cursor_apply(); }

static const pmback_t pm_wayland = { wl_show, wl_present, wl_title, wl_idle, wl_clip_set, wl_clip_get, wl_cursor };

/* the pointer */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p;
    pthread_mutex_lock(&gil);
    wl_ptr_frame = wl_frame_of(s);
    wl_px = wl_fixed_to_int(x); wl_py = wl_fixed_to_int(y);
    wl_enter_serial = serial;
    if (wl_ptr_frame) wl_cursor_apply();         /* (the shape: set again, each time it comes in) */
    pthread_mutex_unlock(&gil);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)p; (void)serial; (void)s;
    pthread_mutex_lock(&gil);
    wl_ptr_frame = 0;
    pthread_mutex_unlock(&gil);
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)t;
    pthread_mutex_lock(&gil);
    wl_px = wl_fixed_to_int(x); wl_py = wl_fixed_to_int(y);
    if (wl_ptr_frame && wl_ptr_frame->used) pm_mouse(wl_ptr_frame, wl_px, wl_py, 0, 0);
    pthread_mutex_unlock(&gil);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)t;
    int b = button == 0x110 ? 1 : button == 0x111 ? 2 : button == 0x112 ? 3 : 0;     /* BTN_LEFT RIGHT MIDDLE */
    pthread_mutex_lock(&gil);
    wl_serial = serial;
    if (wl_clip_pending) wl_clip_offer();
    if (b && wl_ptr_frame && wl_ptr_frame->used) {
        if (state) wl_kbd_frame = wl_ptr_frame;
        pm_mouse(wl_ptr_frame, wl_px, wl_py, b, state == WL_POINTER_BUTTON_STATE_PRESSED);
    }
    pthread_mutex_unlock(&gil);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_axis_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void p_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener p_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame,
                                                p_axis_source, p_axis_stop, p_axis_discrete, p_axis_v120, p_axis_rel };

/* the keyboard */
static void k_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    (void)d; (void)k;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char *m = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    pthread_mutex_lock(&gil);
    if (!xkb_ctx) xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = xkb_keymap_new_from_string(xkb_ctx, m, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(m, size);
    if (km) {
        if (xkb_st) xkb_state_unref(xkb_st);
        if (xkb_map) xkb_keymap_unref(xkb_map);
        xkb_map = km;
        xkb_st = xkb_state_new(km);
    }
    pthread_mutex_unlock(&gil);
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
    (void)d; (void)k; (void)keys;
    pthread_mutex_lock(&gil);
    wl_kbd_frame = wl_frame_of(s);
    wl_serial = serial;                          /* (the focus's serial: good for the selection too) */
    if (wl_clip_pending) wl_clip_offer();
    pthread_mutex_unlock(&gil);
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)k; (void)serial; (void)s;
}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)t;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    pthread_mutex_lock(&gil);
    wl_serial = serial;
    if (wl_clip_pending) wl_clip_offer();
    pmwin_t *f = wl_kbd_frame;
    if (f && f->used && xkb_st) {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_st, key + 8);
        uint32_t ch = xkb_state_key_get_utf32(xkb_st, key + 8);
        int vk = 0, kc = 0;
        switch (sym) {
        case XKB_KEY_BackSpace: vk = 0x05; ch = 8; break;
        case XKB_KEY_Tab: vk = 0x06; ch = 9; break;
        case XKB_KEY_Return: vk = 0x08; ch = '\r'; break;
        case XKB_KEY_KP_Enter: vk = 0x1e; ch = '\r'; break;
        case XKB_KEY_Escape: vk = 0x0f; ch = 27; break;
        case XKB_KEY_space: vk = 0x10; break;
        case XKB_KEY_Left: vk = 0x15; ch = 0; break;
        case XKB_KEY_Up: vk = 0x16; ch = 0; break;
        case XKB_KEY_Right: vk = 0x17; ch = 0; break;
        case XKB_KEY_Down: vk = 0x18; ch = 0; break;
        case XKB_KEY_Delete: vk = 0x1b; ch = 0; break;
        case XKB_KEY_Home: vk = 0x14; ch = 0; break;
        case XKB_KEY_End: vk = 0x13; ch = 0; break;
        case XKB_KEY_Prior: vk = 0x11; ch = 0; break;
        case XKB_KEY_Next: vk = 0x12; ch = 0; break;
        default:
            if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) { vk = 0x20 + (int)(sym - XKB_KEY_F1); ch = 0; }
        }
        if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) kc |= KC_SHIFT;
        if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) kc |= KC_CTRL;
        if (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) kc |= KC_ALT;
        if (ch > 255) ch = '?';                  /* (PM's characters: a code page's, 8 bits) */
        if (ch || vk) pm_key(f, (int)ch, vk, (int)key, kc);
    }
    pthread_mutex_unlock(&gil);
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    (void)d; (void)k; (void)serial;
    pthread_mutex_lock(&gil);
    if (xkb_st) {
        xkb_state_update_mask(xkb_st, dep, lat, lock, 0, 0, grp);
        pm_kc = (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0 ? KC_SHIFT : 0) |
                (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ? KC_CTRL : 0) |
                (xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ? KC_ALT : 0);
    }
    pthread_mutex_unlock(&gil);
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; (void)rate; (void)delay; }
static const struct wl_keyboard_listener k_l = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl_ptr) { wl_ptr = wl_seat_get_pointer(s); wl_pointer_add_listener(wl_ptr, &p_l, 0); }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl_kbd) { wl_kbd = wl_seat_get_keyboard(s); wl_keyboard_add_listener(wl_kbd, &k_l, 0); }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };
static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) wl_comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name)) wl_shm_ = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wl_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wl_wm, &wm_l, 0); }
    else if (!strcmp(iface, wl_data_device_manager_interface.name)) wl_ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, ver < 3 ? ver : 3);
    else if (!strcmp(iface, wl_seat_interface.name)) { wl_seat_ = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5); wl_seat_add_listener(wl_seat_, &seat_l, 0); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void *wl_events(void *a)
{
    (void)a;
    while (wl_display_dispatch(wld) >= 0) { }
    pthread_mutex_lock(&gil);                /* the desktop's gone: the program goes too */
    fprintf(stderr, "%s: the display went away\n", prog_name);
    do_exit(1);
}

/* gil held; the first dispatches before the thread starts */
static int wl_start(void)
{
    if (!(wld = wl_display_connect(0))) {
        fprintf(stderr, "%s: Wayland display %s (XDG_RUNTIME_DIR %s): %s\n", prog_name, getenv("WAYLAND_DISPLAY"),
                getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "unset", strerror(errno));
        return 0;
    }
    struct wl_registry *r = wl_display_get_registry(wld);
    wl_registry_add_listener(r, &reg_l, 0);
    wl_display_roundtrip(wld);
    wl_display_roundtrip(wld);
    if (!wl_comp || !wl_shm_ || !wl_wm) {
        fprintf(stderr, "%s: the Wayland compositor lacks %s\n", prog_name, !wl_comp ? "wl_compositor" : !wl_shm_ ? "wl_shm" : "xdg_wm_base");
        wl_display_disconnect(wld); wld = 0; return 0;
    }
    if (wl_ddm && wl_seat_) { wl_dd = wl_data_device_manager_get_data_device(wl_ddm, wl_seat_); wl_data_device_add_listener(wl_dd, &dd_l, 0); }
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&t, &at, wl_events, 0);
    pthread_attr_destroy(&at);
    return e == 0;
}
