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
    if (f->used && f->iproc != mb_proc) pm_resize(f, w, h);     /* (message boxes keep their size) */
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
        if (f->iproc == mb_proc) { xdg_toplevel_set_min_size(k->tl, f->pw, f->ph); xdg_toplevel_set_max_size(k->tl, f->pw, f->ph); }
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
static const pmback_t pm_wayland = { wl_show, wl_present, wl_title, wl_idle };

/* the pointer */
static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)serial;
    pthread_mutex_lock(&gil);
    wl_ptr_frame = wl_frame_of(s);
    wl_px = wl_fixed_to_int(x); wl_py = wl_fixed_to_int(y);
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
    (void)d; (void)p; (void)serial; (void)t;
    int b = button == 0x110 ? 1 : button == 0x111 ? 2 : button == 0x112 ? 3 : 0;     /* BTN_LEFT RIGHT MIDDLE */
    pthread_mutex_lock(&gil);
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
    (void)d; (void)k; (void)serial; (void)keys;
    pthread_mutex_lock(&gil);
    wl_kbd_frame = wl_frame_of(s);
    pthread_mutex_unlock(&gil);
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)k; (void)serial; (void)s;
}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)t;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    pthread_mutex_lock(&gil);
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
    if (xkb_st) xkb_state_update_mask(xkb_st, dep, lat, lock, 0, 0, grp);
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
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&t, &at, wl_events, 0);
    pthread_attr_destroy(&at);
    return e == 0;
}
