/* wltest.c - stage 4 test: libwayland server + client in two processes
 * exchanging a wl_shm buffer, composited with pixman; xkbcommon keymap
 * compiled from a string.
 *
 * pkg: wayland-server wayland-client pixman-1 xkbcommon
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <pixman.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#define W 64
#define H 64
#define STRIDE (W * 4)

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); fflush(stdout); } while (0)

static uint32_t pattern(int x, int y) { return 0xFF000000u | (uint32_t)(x * 4) << 16 | (uint32_t)(y * 4) << 8 | 0x80; }

/* ── Server: a compositor that checks what it is given ─────────────────── */
static struct wl_display *display;
static int server_done, server_ok;

static void surf_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void surf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y)
{
    (void)c; (void)x; (void)y;
    wl_resource_set_user_data(r, buf);
}
static void surf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb) { (void)c; (void)r; (void)cb; }
static void surf_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *g) { (void)c; (void)r; (void)g; }
static void surf_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct wl_resource *buf = wl_resource_get_user_data(r);
    struct wl_shm_buffer *shm = buf ? wl_shm_buffer_get(buf) : 0;
    if (!shm) { printf("FAIL  server: commit without a wl_shm buffer\n"); server_done = 1; return; }
    int w = wl_shm_buffer_get_width(shm), h = wl_shm_buffer_get_height(shm);
    int stride = wl_shm_buffer_get_stride(shm);
    uint32_t fmt = wl_shm_buffer_get_format(shm);
    printf("      server: buffer %dx%d stride %d format %u\n", w, h, stride, fmt);
    wl_shm_buffer_begin_access(shm);
    uint32_t *src = wl_shm_buffer_get_data(shm);
    /* Composite the client's buffer over a grey background with pixman. */
    pixman_image_t *s = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, src, stride);
    pixman_image_t *d = pixman_image_create_bits(PIXMAN_x8r8g8b8, W * 2, H * 2, 0, 0);
    pixman_color_t grey = { 0x8000, 0x8000, 0x8000, 0xFFFF };
    pixman_image_t *fill = pixman_image_create_solid_fill(&grey);
    pixman_image_composite32(PIXMAN_OP_SRC, fill, 0, d, 0, 0, 0, 0, 0, 0, W * 2, H * 2);
    pixman_image_composite32(PIXMAN_OP_OVER, s, 0, d, 0, 0, 0, 0, 16, 16, w, h);
    uint32_t *out = pixman_image_get_data(d);
    int ds = pixman_image_get_stride(d) / 4, bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if ((out[(y + 16) * ds + x + 16] & 0xFFFFFF) != (pattern(x, y) & 0xFFFFFF)) bad++;
    uint32_t corner = out[0] & 0xFFFFFF;
    wl_shm_buffer_end_access(shm);
    pixman_image_unref(fill);
    pixman_image_unref(s);
    pixman_image_unref(d);
    server_ok = w == W && h == H && stride == STRIDE && fmt == WL_SHM_FORMAT_ARGB8888 && !bad &&
                corner == 0x808080;
    printf("%s  server: pixman composite, %d wrong pixels, background 0x%06x\n",
           server_ok ? "ok   " : "FAIL ", bad, corner);
    wl_buffer_send_release(buf);
    server_done = 1;
}
static const struct wl_surface_interface surf_impl = {
    .destroy = surf_destroy, .attach = surf_attach, .damage = surf_damage, .frame = surf_frame,
    .set_opaque_region = surf_region, .set_input_region = surf_region, .commit = surf_commit,
};

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *s = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s, &surf_impl, 0, 0);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{ (void)c; (void)r; (void)id; wl_client_post_no_memory(c); }
static const struct wl_compositor_interface comp_impl = { comp_create_surface, comp_create_region };

static void comp_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(r, &comp_impl, 0, 0);
    pid_t pid; uid_t uid; gid_t gid;
    wl_client_get_credentials(c, &pid, &uid, &gid);
    printf("      server: client pid %d bound wl_compositor v%u\n", pid, version);
}

/* ── Client ─────────────────────────────────────────────────────────────── */
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static int formats, released;

static void shm_format(void *d, struct wl_shm *s, uint32_t f) { (void)d; (void)s; if (f <= 1) formats |= 1 << f; }
static const struct wl_shm_listener shm_listener = { shm_format };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    (void)d;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, v < 4 ? v : 4);
    if (!strcmp(iface, "wl_shm")) { shm = wl_registry_bind(r, name, &wl_shm_interface, 1); wl_shm_add_listener(shm, &shm_listener, 0); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void buf_release(void *d, struct wl_buffer *b) { (void)d; (void)b; released = 1; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static int client(void)
{
    struct wl_display *dpy = wl_display_connect(0);
    if (!dpy) { printf("FAIL  client: wl_display_connect: %s\n", strerror(errno)); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, 0);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    printf("%s  client: globals wl_compositor %s, wl_shm %s, formats ARGB8888 %s XRGB8888 %s\n",
           compositor && shm && formats == 3 ? "ok   " : "FAIL ", compositor ? "yes" : "no", shm ? "yes" : "no",
           formats & 1 ? "yes" : "no", formats & 2 ? "yes" : "no");
    if (!compositor || !shm) return 1;
    int fd = memfd_create("wl_shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, STRIDE * H)) return 1;
    uint32_t *px = mmap(0, STRIDE * H, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) return 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) px[y * W + x] = pattern(x, y);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, STRIDE * H);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, STRIDE, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buf, &buf_listener, 0);
    wl_shm_pool_destroy(pool);
    close(fd);
    struct wl_surface *surf = wl_compositor_create_surface(compositor);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    while (!released && wl_display_dispatch(dpy) >= 0) { }
    printf("%s  client: buffer released by the server\n", released ? "ok   " : "FAIL ");
    wl_buffer_destroy(buf);
    wl_surface_destroy(surf);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return released ? 0 : 1;
}

/* ── xkbcommon ──────────────────────────────────────────────────────────── */
static const char keymap_str[] =
    "xkb_keymap {\n"
    " xkb_keycodes \"t\" { minimum = 8; maximum = 255;\n"
    "   <AE01> = 10; <AC01> = 38; <LFSH> = 50; <RTRN> = 36; };\n"
    " xkb_types \"t\" {\n"
    "   type \"ONE_LEVEL\" { modifiers = none; level_name[Level1] = \"Any\"; };\n"
    "   type \"TWO_LEVEL\" { modifiers = Shift; map[Shift] = Level2;\n"
    "     level_name[Level1] = \"Base\"; level_name[Level2] = \"Shift\"; };\n"
    "   type \"ALPHABETIC\" { modifiers = Shift+Lock; map[Shift] = Level2; map[Lock] = Level2;\n"
    "     level_name[Level1] = \"Base\"; level_name[Level2] = \"Caps\"; };\n"
    " };\n"
    " xkb_compat \"t\" {\n"
    "   interpret Shift_L { action = SetMods(modifiers = Shift); };\n"
    " };\n"
    " xkb_symbols \"t\" {\n"
    "   key <AE01> { [ 1, exclam ] };\n"
    "   key <AC01> { type = \"ALPHABETIC\", [ a, A ] };\n"
    "   key <LFSH> { [ Shift_L ] };\n"
    "   key <RTRN> { [ Return ] };\n"
    "   modifier_map Shift { <LFSH> };\n"
    " };\n"
    "};\n";

static void xkb_test(void)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_DEFAULT_INCLUDES | XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    struct xkb_keymap *km = ctx ? xkb_keymap_new_from_string(ctx, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1, 0) : 0;
    CHECK(km, "xkbcommon: keymap compiled from a string");
    if (!km) return;
    struct xkb_state *st = xkb_state_new(km);
    xkb_keysym_t a = xkb_state_key_get_one_sym(st, 38);
    xkb_state_update_key(st, 50, XKB_KEY_DOWN);
    xkb_keysym_t A = xkb_state_key_get_one_sym(st, 38), ex = xkb_state_key_get_one_sym(st, 10);
    xkb_state_update_key(st, 50, XKB_KEY_UP);
    char name[32];
    xkb_keysym_get_name(ex, name, sizeof name);
    CHECK(a == XKB_KEY_a && A == XKB_KEY_A && ex == XKB_KEY_exclam && xkb_state_key_get_one_sym(st, 36) == XKB_KEY_Return,
          "xkbcommon: a / Shift+a = A / Shift+1 = %s / Return", name);
    char *text = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    CHECK(text && strlen(text) > 200, "xkbcommon: keymap serialised (%zu bytes, as a compositor sends it)",
          text ? strlen(text) : 0);
    free(text);
    xkb_state_unref(st);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

int main(void)
{
    setvbuf(stdout, 0, _IOLBF, 0);
    xkb_test();

    display = wl_display_create();
    CHECK(display, "wl_display_create");
    if (!display) return 1;
    const char *sock = wl_display_add_socket_auto(display);
    CHECK(sock, "wl_display_add_socket_auto: %s/%s", getenv("XDG_RUNTIME_DIR"), sock ? sock : "?");
    if (!sock) return 1;
    CHECK(wl_display_init_shm(display) == 0, "wl_display_init_shm");
    wl_global_create(display, &wl_compositor_interface, 4, 0, comp_bind);
    setenv("WAYLAND_DISPLAY", sock, 1);

    pid_t pid = fork();
    if (pid == 0) _exit(client());

    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int st = 0, reaped = 0;
    for (;;) {
        wl_event_loop_dispatch(loop, 100);
        wl_display_flush_clients(display);
        if (waitpid(pid, &st, WNOHANG) == pid) { reaped = 1; break; }
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec > 20) { printf("FAIL  timeout\n"); fails++; kill(pid, SIGKILL); break; }
    }
    if (!reaped) waitpid(pid, &st, 0);
    CHECK(server_done && server_ok, "server got and checked the buffer");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "client exited %d", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    wl_display_destroy_clients(display);
    wl_display_destroy(display);
    printf("wltest: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
