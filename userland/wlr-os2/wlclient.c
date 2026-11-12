/*
 * userland/wlr-os2/wlclient.c - a small xdg-shell client for testing a
 * compositor: a window with a gradient and a bar that moves every frame
 * (frame callbacks), whose colour changes with each key or button press.
 * Key presses, pointer buttons and pointer entry are printed.
 *
 *   wlclient [width height] [frames]    (frames: exit after that many; 0 = run)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_surface *surf;
static struct wl_buffer *buf[2];
static uint32_t *px[2];
static int busy[2], width = 360, height = 240, configured, running = 1, frame, max_frames;
static uint32_t accent = 0xFFE0A020;

static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { (void)d; xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz)
{ (void)d; (void)k; (void)f; (void)sz; close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *x, struct wl_array *a)
{ (void)d; (void)k; (void)s; (void)x; (void)a; printf("wlclient: keyboard focus\n"); }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *x)
{ (void)d; (void)k; (void)s; (void)x; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st)
{
    (void)d; (void)k; (void)s; (void)t;
    if (!st) return;
    printf("wlclient: key %u\n", key);
    accent = 0xFF000000 | (key * 0x9E3779B1u & 0xFFFFFF);
    if (key == 1) running = 0;                              /* Escape */
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g)
{ (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { (void)d; (void)k; (void)r; (void)dl; }
static const struct wl_keyboard_listener kb_l = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

static void pt_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *x, wl_fixed_t sx, wl_fixed_t sy)
{ (void)d; (void)p; (void)s; (void)x; printf("wlclient: pointer at %d,%d\n", wl_fixed_to_int(sx), wl_fixed_to_int(sy)); }
static void pt_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *x) { (void)d; (void)p; (void)s; (void)x; }
static void pt_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void pt_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{
    (void)d; (void)p; (void)s; (void)t;
    if (st) { printf("wlclient: button 0x%x\n", b); accent ^= 0x00FFFFFF; }
}
static void pt_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static const struct wl_pointer_listener pt_l = { pt_enter, pt_leave, pt_motion, pt_button, pt_axis };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) wl_keyboard_add_listener(wl_seat_get_keyboard(s), &kb_l, 0);
    if (caps & WL_SEAT_CAPABILITY_POINTER) wl_pointer_add_listener(wl_seat_get_pointer(s), &pt_l, 0);
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    (void)d; (void)v;
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_l, 0); }
    else if (!strcmp(iface, "wl_seat")) { seat = wl_registry_bind(r, name, &wl_seat_interface, v < 4 ? v : 4); wl_seat_add_listener(seat, &seat_l, 0); }
}
static void global_rm(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { global, global_rm };

static void xs_conf(void *d, struct xdg_surface *x, uint32_t s) { (void)d; xdg_surface_ack_configure(x, s); configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_conf };
static void tl_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; running = 0; }
static const struct xdg_toplevel_listener tl_l = { tl_conf, tl_close, 0, 0 };

static void released(void *d, struct wl_buffer *b) { (void)b; busy[(intptr_t)d] = 0; }
static const struct wl_buffer_listener buf_l = { released };

static void draw(int i)
{
    uint32_t *p = px[i];
    int bar = (frame * 4) % width;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            uint32_t c = 0xFF000000 | (uint32_t)(x * 255 / width) << 16 | (uint32_t)(y * 255 / height) << 8 | 0x60;
            if (x < 4 || y < 4 || x >= width - 4 || y >= height - 4 || (x >= bar && x < bar + 12)) c = accent;
            p[y * width + x] = c;
        }
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t);
static const struct wl_callback_listener frame_l = { frame_done };

static void redraw(void)
{
    int i = busy[0] ? 1 : 0;
    if (busy[i]) return;                                     /* both in use: skip */
    draw(i);
    busy[i] = 1;
    wl_surface_attach(surf, buf[i], 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, width, height);
    wl_callback_add_listener(wl_surface_frame(surf), &frame_l, 0);
    wl_surface_commit(surf);
    frame++;
    if (max_frames && frame >= max_frames) running = 0;
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t) { (void)d; (void)t; wl_callback_destroy(cb); redraw(); }

int main(int argc, char **argv)
{
    if (argc >= 3) { width = atoi(argv[1]); height = atoi(argv[2]); }
    if (argc >= 4) max_frames = atoi(argv[3]);
    setvbuf(stdout, 0, _IOLBF, 0);
    struct wl_display *dpy = wl_display_connect(0);
    if (!dpy) { perror("wlclient: wl_display_connect"); return 1; }
    wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, 0);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "wlclient: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n"); return 1; }
    size_t one = (size_t)width * height * 4;
    int fd = memfd_create("wlclient", MFD_CLOEXEC);
    ftruncate(fd, (off_t)(2 * one));
    uint8_t *m = mmap(0, 2 * one, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)(2 * one));
    for (int i = 0; i < 2; i++) {
        px[i] = (uint32_t *)(m + i * one);
        buf[i] = wl_shm_pool_create_buffer(pool, (int32_t)(i * one), width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(buf[i], &buf_l, (void *)(intptr_t)i);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, 0);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_l, 0);
    xdg_toplevel_set_title(tl, "wlclient");
    wl_surface_commit(surf);
    while (!configured && wl_display_dispatch(dpy) >= 0) { }
    printf("wlclient: %dx%d window, drawing\n", width, height);
    redraw();
    while (running && wl_display_dispatch(dpy) >= 0) { }
    printf("wlclient: done after %d frames\n", frame);
    wl_display_disconnect(dpy);
    return 0;
}
