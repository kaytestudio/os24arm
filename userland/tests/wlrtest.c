/* wlrtest.c - stage 5 test: a wlroots compositor (headless backend, pixman
 * renderer, scene graph, xdg-shell) and an xdg-shell client in a forked
 * process. The client maps a solid-colour window; the compositor renders
 * its output and checks the pixels.
 *
 * pkg: wlroots-0.19 wayland-server wayland-client pixman-1 xkbcommon
 * protocols: stable/xdg-shell/xdg-shell.xml
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <drm_fourcc.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "xdg-shell-client-protocol.h"

#define OUT_W 320
#define OUT_H 240
#define WIN_W 120
#define WIN_H 90
#define COLOR 0xFF20C040u           /* ARGB */
#define BG    0x00303030u

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); fflush(stdout); } while (0)

/* ── Client ─────────────────────────────────────────────────────────────── */
static struct wl_compositor *c_comp;
static struct wl_shm *c_shm;
static struct xdg_wm_base *c_wm;
static int c_configured;

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial) { (void)d; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void c_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    (void)d; (void)v;
    if (!strcmp(iface, "wl_compositor")) c_comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) c_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) {
        c_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(c_wm, &wm_listener, 0);
    }
}
static void c_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener c_reg = { c_global, c_global_remove };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    c_configured = 1;
}
static const struct xdg_surface_listener xs_listener = { xs_configure };
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s)
{ (void)d; (void)t; (void)w; (void)h; (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener tl_listener = { tl_configure, tl_close, 0, 0 };

static int client(void)
{
    struct wl_display *dpy = wl_display_connect(0);
    if (!dpy) { printf("FAIL  client: connect: %s\n", strerror(errno)); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &c_reg, 0);
    wl_display_roundtrip(dpy);
    if (!c_comp || !c_shm || !c_wm) { printf("FAIL  client: globals missing\n"); return 1; }
    struct wl_surface *surf = wl_compositor_create_surface(c_comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(c_wm, surf);
    xdg_surface_add_listener(xs, &xs_listener, 0);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_listener, 0);
    xdg_toplevel_set_title(tl, "wlrtest");
    wl_surface_commit(surf);                               /* initial commit, no buffer */
    while (!c_configured && wl_display_dispatch(dpy) >= 0) { }
    int size = WIN_W * 4 * WIN_H;
    int fd = memfd_create("buf", MFD_CLOEXEC);
    ftruncate(fd, size);
    uint32_t *px = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < WIN_W * WIN_H; i++) px[i] = COLOR;
    struct wl_shm_pool *pool = wl_shm_create_pool(c_shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, WIN_W, WIN_H, WIN_W * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, WIN_W, WIN_H);
    wl_surface_commit(surf);
    printf("      client: window mapped (%dx%d)\n", WIN_W, WIN_H);
    while (wl_display_dispatch(dpy) >= 0) { }                /* until the compositor goes */
    return 0;
}

/* ── Compositor ─────────────────────────────────────────────────────────── */
static struct wl_display *display;
static struct wlr_backend *backend;
static struct wlr_renderer *renderer;
static struct wlr_allocator *allocator;
static struct wlr_scene *scene;
static struct wlr_scene_output_layout *scene_layout;
static struct wlr_output_layout *layout;
static struct wlr_scene_output *scene_output;
static struct wlr_output *the_output;
static int mapped, frames, checked, check_ok;

struct toplevel { struct wlr_xdg_toplevel *t; struct wl_listener commit, map, destroy; };

static void tl_commit(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, commit);
    if (t->t->base->initial_commit) wlr_xdg_toplevel_set_size(t->t, 0, 0);
}
static void tl_map(struct wl_listener *l, void *data) { (void)l; (void)data; mapped = 1; }
static void tl_destroy(struct wl_listener *l, void *data)
{
    (void)data;
    struct toplevel *t = wl_container_of(l, t, destroy);
    wl_list_remove(&t->commit.link); wl_list_remove(&t->map.link); wl_list_remove(&t->destroy.link);
    free(t);
}
static void new_toplevel(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_xdg_toplevel *xt = data;
    struct toplevel *t = calloc(1, sizeof *t);
    t->t = xt;
    struct wlr_scene_tree *tree = wlr_scene_xdg_surface_create(&scene->tree, xt->base);
    wlr_scene_node_set_position(&tree->node, 40, 30);
    t->commit.notify = tl_commit; wl_signal_add(&xt->base->surface->events.commit, &t->commit);
    t->map.notify = tl_map;       wl_signal_add(&xt->base->surface->events.map, &t->map);
    t->destroy.notify = tl_destroy; wl_signal_add(&xt->events.destroy, &t->destroy);
    printf("      compositor: new toplevel \"%s\"\n", xt->title ? xt->title : "");
}
static struct wl_listener new_toplevel_l = { .notify = new_toplevel };

/* After a frame with the window in it: read the output's buffer back. */
static void check_buffer(struct wlr_buffer *b)
{
    void *data; uint32_t fmt; size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(b, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
        printf("FAIL  compositor: can't read the output buffer\n");
        return;
    }
    int inside = 0, outside = 0;
    for (int y = 0; y < OUT_H; y++)
        for (int x = 0; x < OUT_W; x++) {
            uint32_t p = ((uint32_t *)((uint8_t *)data + y * stride))[x] & 0xFFFFFF;
            int in = x >= 40 && x < 40 + WIN_W && y >= 30 && y < 30 + WIN_H;
            if (in && p == (COLOR & 0xFFFFFF)) inside++;
            if (!in && p == (BG & 0xFFFFFF)) outside++;
        }
    wlr_buffer_end_data_ptr_access(b);
    check_ok = inside == WIN_W * WIN_H && outside == OUT_W * OUT_H - WIN_W * WIN_H;
    printf("%s  compositor: frame %d: %d/%d window pixels, %d/%d background pixels (format %.4s)\n",
           check_ok ? "ok   " : "FAIL ", frames, inside, WIN_W * WIN_H, outside,
           OUT_W * OUT_H - WIN_W * WIN_H, (char *)&fmt);
    checked = 1;
}

static void output_frame(struct wl_listener *l, void *data)
{
    (void)l; (void)data;
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    if (wlr_scene_output_build_state(scene_output, &st, 0)) {
        frames++;
        if (mapped && !checked && st.buffer && frames > 2) check_buffer(st.buffer);
        wlr_output_commit_state(the_output, &st);
    }
    wlr_output_state_finish(&st);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}
static struct wl_listener frame_l = { .notify = output_frame };

static void new_output(struct wl_listener *l, void *data)
{
    (void)l;
    struct wlr_output *o = data;
    wlr_output_init_render(o, allocator, renderer);
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_enabled(&st, true);
    wlr_output_state_set_custom_mode(&st, OUT_W, OUT_H, 60000);
    bool ok = wlr_output_commit_state(o, &st);
    wlr_output_state_finish(&st);
    CHECK(ok, "compositor: output %s enabled, %dx%d", o->name, o->width, o->height);
    the_output = o;
    wl_signal_add(&o->events.frame, &frame_l);
    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(layout, o);
    scene_output = wlr_scene_output_create(scene, o);
    wlr_scene_output_layout_add_output(scene_layout, lo, scene_output);
}
static struct wl_listener new_output_l = { .notify = new_output };

int main(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    wlr_log_init(WLR_ERROR, 0);
    display = wl_display_create();
    backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
    CHECK(backend, "headless backend");
    renderer = wlr_pixman_renderer_create();
    CHECK(renderer, "pixman renderer");
    if (!backend || !renderer) return 1;
    wlr_renderer_init_wl_display(renderer, display);
    allocator = wlr_allocator_autocreate(backend, renderer);
    CHECK(allocator, "allocator");
    if (!allocator) return 1;
    wlr_compositor_create(display, 5, renderer);
    wlr_subcompositor_create(display);
    layout = wlr_output_layout_create(display);
    scene = wlr_scene_create();
    scene_layout = wlr_scene_attach_output_layout(scene, layout);
    wlr_scene_rect_create(&scene->tree, OUT_W, OUT_H, (float[4]){ 0x30 / 255.f, 0x30 / 255.f, 0x30 / 255.f, 1 });
    struct wlr_xdg_shell *xdg = wlr_xdg_shell_create(display, 3);
    wl_signal_add(&xdg->events.new_toplevel, &new_toplevel_l);
    wl_signal_add(&backend->events.new_output, &new_output_l);
    wlr_headless_add_output(backend, OUT_W, OUT_H);

    const char *sock = wl_display_add_socket_auto(display);
    CHECK(sock, "listening on %s", sock ? sock : "?");
    CHECK(wlr_backend_start(backend), "backend started");
    setenv("WAYLAND_DISPLAY", sock, 1);

    pid_t pid = fork();
    if (pid == 0) _exit(client());

    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!checked) {
        wl_display_flush_clients(display);
        wl_event_loop_dispatch(loop, 100);
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec > 40) { printf("FAIL  timeout (%d frames, mapped %d)\n", frames, mapped); fails++; break; }
    }
    CHECK(mapped, "toplevel mapped");
    CHECK(checked && check_ok, "rendered output shows the client's window");
    wl_display_destroy_clients(display);
    wl_list_remove(&new_toplevel_l.link);
    wl_list_remove(&new_output_l.link);
    if (the_output) wl_list_remove(&frame_l.link);
    int st;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "client exited %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    wl_display_destroy(display);
    printf("wlrtest: %s (%d failures, %d frames)\n", fails ? "FAILED" : "all passed", fails, frames);
    return fails;
}
