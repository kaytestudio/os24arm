/*
 * userland/wlr-os2/os2_backend.c - wlroots backend: fbdev output, evdev
 * input (see os2_backend.h).
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <drm_fourcc.h>
#include <pixman.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>
#include "os2_backend.h"
#include "keymap_us.h"

/* os2warp: present a damaged rectangle of the framebuffer (virtio-gpu
 * flush); kernel/src/linux/linux.c. Elsewhere FBIOPAN_DISPLAY is used. */
#define OS2FB_DAMAGE _IOW('F', 0xF0, struct os2fb_rect)
struct os2fb_rect { uint32_t x, y, w, h; };

#define REFRESH_MHZ 60000
#define ABS_RANGE_DEFAULT 32767

struct os2_backend;

struct os2_output {
    struct wlr_output wlr;
    struct os2_backend *backend;
    struct wl_event_source *frame_timer, *present_idle;
    uint32_t pending_seq;
};

struct os2_keyboard {
    struct wlr_keyboard wlr;
    int fd;
    struct wl_event_source *src;
};

struct os2_pointer {
    struct wlr_pointer wlr;
    int fd;
    struct wl_event_source *src;
    int abs_x, abs_y, abs_max_x, abs_max_y, abs_dirty;
    double dx, dy;
};

struct os2_backend {
    struct wlr_backend base;
    struct wl_event_loop *loop;
    char *fb_path, *kbd_path, *ptr_path;
    int fb_fd;
    uint8_t *fb;
    size_t fb_size;
    int width, height, stride;
    int damage_ioctl;               /* 1 works, 0 unknown, -1 use FBIOPAN_DISPLAY */
    bool started;
    struct os2_output *output;
    struct os2_keyboard *keyboard;
    struct os2_pointer *pointer;
};

static const struct wlr_backend_impl backend_impl;
static const struct wlr_output_impl output_impl;

static struct os2_backend *get_backend(struct wlr_backend *b)
{
    assert(b->impl == &backend_impl);
    return wl_container_of(b, (struct os2_backend *)0, base);
}

/* ── Output ───────────────────────────────────────────────────────────── */
static const uint32_t SUPPORTED_STATE = WLR_OUTPUT_STATE_BACKEND_OPTIONAL | WLR_OUTPUT_STATE_BUFFER |
                                        WLR_OUTPUT_STATE_DAMAGE | WLR_OUTPUT_STATE_ENABLED |
                                        WLR_OUTPUT_STATE_MODE;

static bool output_test(struct wlr_output *wo, const struct wlr_output_state *st)
{
    struct os2_output *o = wl_container_of(wo, o, wlr);
    if (st->committed & ~SUPPORTED_STATE) {
        wlr_log(WLR_DEBUG, "os2: unsupported output state 0x%x", st->committed & ~SUPPORTED_STATE);
        return false;
    }
    if (st->committed & WLR_OUTPUT_STATE_MODE) {             /* the framebuffer's mode only */
        int w = st->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM ? st->custom_mode.width :
                st->mode ? st->mode->width : 0;
        int h = st->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM ? st->custom_mode.height :
                st->mode ? st->mode->height : 0;
        if (w != o->backend->width || h != o->backend->height) return false;
    }
    if (st->committed & WLR_OUTPUT_STATE_BUFFER) {
        struct wlr_buffer *b = st->buffer;
        if (b->width != o->backend->width || b->height != o->backend->height) return false;
    }
    return true;
}

static void present_damage(struct os2_backend *b, int x, int y, int w, int h)
{
    if (b->damage_ioctl >= 0) {
        struct os2fb_rect r = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
        if (ioctl(b->fb_fd, OS2FB_DAMAGE, &r) == 0) { b->damage_ioctl = 1; return; }
        if (b->damage_ioctl == 0) b->damage_ioctl = -1;
    }
    struct fb_var_screeninfo v;
    if (ioctl(b->fb_fd, FBIOGET_VSCREENINFO, &v) == 0) ioctl(b->fb_fd, FBIOPAN_DISPLAY, &v);
}

static void copy_rect(struct os2_backend *b, const uint8_t *src, size_t sstride, const pixman_box32_t *r)
{
    int x1 = r->x1 < 0 ? 0 : r->x1, y1 = r->y1 < 0 ? 0 : r->y1;
    int x2 = r->x2 > b->width ? b->width : r->x2, y2 = r->y2 > b->height ? b->height : r->y2;
    for (int y = y1; y < y2; y++)
        memcpy(b->fb + (size_t)y * b->stride + (size_t)x1 * 4, src + (size_t)y * sstride + (size_t)x1 * 4,
               (size_t)(x2 - x1) * 4);
}

static int send_frame(void *data)
{
    struct os2_output *o = data;
    wlr_output_send_frame(&o->wlr);
    return 0;
}

static void send_present(void *data)
{
    struct os2_output *o = data;
    o->present_idle = NULL;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    struct wlr_output_event_present ev = {
        .output = &o->wlr, .commit_seq = o->pending_seq, .presented = true, .when = t,
        .refresh = 1000000000 / (REFRESH_MHZ / 1000),
    };
    wlr_output_send_present(&o->wlr, &ev);
}

static bool output_commit(struct wlr_output *wo, const struct wlr_output_state *st)
{
    struct os2_output *o = wl_container_of(wo, o, wlr);
    struct os2_backend *b = o->backend;
    if (!output_test(wo, st)) return false;
    bool enabled = (st->committed & WLR_OUTPUT_STATE_ENABLED) ? st->enabled : wo->enabled;
    if ((st->committed & WLR_OUTPUT_STATE_BUFFER) && enabled) {
        void *data;
        uint32_t fmt;
        size_t stride;
        if (!wlr_buffer_begin_data_ptr_access(st->buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride)) {
            wlr_log(WLR_ERROR, "os2: can't read the frame's buffer");
            return false;
        }
        if (fmt != DRM_FORMAT_XRGB8888 && fmt != DRM_FORMAT_ARGB8888) {
            wlr_buffer_end_data_ptr_access(st->buffer);
            wlr_log(WLR_ERROR, "os2: frame format 0x%x is not XRGB8888", fmt);
            return false;
        }
        pixman_box32_t all = { 0, 0, b->width, b->height }, ext = all;
        if (st->committed & WLR_OUTPUT_STATE_DAMAGE) {
            int n;
            const pixman_box32_t *r = pixman_region32_rectangles((pixman_region32_t *)&st->damage, &n);
            for (int i = 0; i < n; i++) copy_rect(b, data, stride, &r[i]);
            ext = *pixman_region32_extents((pixman_region32_t *)&st->damage);
        } else {
            copy_rect(b, data, stride, &all);
        }
        wlr_buffer_end_data_ptr_access(st->buffer);
        if (ext.x2 > ext.x1 && ext.y2 > ext.y1)
            present_damage(b, ext.x1, ext.y1, ext.x2 - ext.x1, ext.y2 - ext.y1);
    }
    if (enabled) {
        o->pending_seq = wo->commit_seq + 1;
        if (!o->present_idle) o->present_idle = wl_event_loop_add_idle(b->loop, send_present, o);
        wl_event_source_timer_update(o->frame_timer, 1000 / (REFRESH_MHZ / 1000));
    }
    return true;
}

static void output_destroy(struct wlr_output *wo)
{
    struct os2_output *o = wl_container_of(wo, o, wlr);
    wlr_output_finish(wo);
    if (o->present_idle) wl_event_source_remove(o->present_idle);
    wl_event_source_remove(o->frame_timer);
    if (o->backend->output == o) o->backend->output = NULL;
    free(o);
}

static const struct wlr_output_impl output_impl = {
    .destroy = output_destroy,
    .test = output_test,
    .commit = output_commit,
};

/* ── Input ────────────────────────────────────────────────────────────── */
static const struct wlr_keyboard_impl keyboard_impl = { .name = "os2-keyboard" };
static const struct wlr_pointer_impl pointer_impl = { .name = "os2-pointer" };

static uint32_t ev_msec(const struct input_event *e)
{
    return (uint32_t)(e->input_event_sec * 1000 + e->input_event_usec / 1000);
}

static int keyboard_readable(int fd, uint32_t mask, void *data)
{
    struct os2_keyboard *k = data;
    (void)mask;
    struct input_event ev[32];
    ssize_t n = read(fd, ev, sizeof ev);
    for (ssize_t i = 0; n > 0 && i < n / (ssize_t)sizeof ev[0]; i++) {
        if (ev[i].type != EV_KEY || ev[i].value == 2) continue;      /* repeats: clients do those */
        struct wlr_keyboard_key_event ke = {
            .time_msec = ev_msec(&ev[i]), .keycode = ev[i].code, .update_state = true,
            .state = ev[i].value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
        };
        wlr_keyboard_notify_key(&k->wlr, &ke);
    }
    return 0;
}

static int pointer_readable(int fd, uint32_t mask, void *data)
{
    struct os2_pointer *p = data;
    (void)mask;
    struct input_event ev[64];
    ssize_t n = read(fd, ev, sizeof ev);
    for (ssize_t i = 0; n > 0 && i < n / (ssize_t)sizeof ev[0]; i++) {
        struct input_event *e = &ev[i];
        uint32_t t = ev_msec(e);
        switch (e->type) {
        case EV_REL:
            if (e->code == REL_X) p->dx += e->value;
            else if (e->code == REL_Y) p->dy += e->value;
            else if (e->code == REL_WHEEL || e->code == REL_HWHEEL) {
                struct wlr_pointer_axis_event ae = {
                    .pointer = &p->wlr, .time_msec = t, .source = WL_POINTER_AXIS_SOURCE_WHEEL,
                    .orientation = e->code == REL_WHEEL ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                                        : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                    .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
                    .delta = -15.0 * e->value, .delta_discrete = -120 * e->value,
                };
                wl_signal_emit_mutable(&p->wlr.events.axis, &ae);
            }
            break;
        case EV_ABS:
            if (e->code == ABS_X) { p->abs_x = e->value; p->abs_dirty = 1; }
            if (e->code == ABS_Y) { p->abs_y = e->value; p->abs_dirty = 1; }
            break;
        case EV_KEY:
            if (e->code >= BTN_LEFT && e->code <= BTN_TASK) {
                struct wlr_pointer_button_event be = {
                    .pointer = &p->wlr, .time_msec = t, .button = e->code,
                    .state = e->value ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
                };
                wlr_pointer_notify_button(&p->wlr, &be);
            }
            break;
        case EV_SYN:
            if (p->dx || p->dy) {
                struct wlr_pointer_motion_event me = {
                    .pointer = &p->wlr, .time_msec = t, .delta_x = p->dx, .delta_y = p->dy,
                    .unaccel_dx = p->dx, .unaccel_dy = p->dy,
                };
                wl_signal_emit_mutable(&p->wlr.events.motion, &me);
                p->dx = p->dy = 0;
            }
            if (p->abs_dirty) {
                struct wlr_pointer_motion_absolute_event ae = {
                    .pointer = &p->wlr, .time_msec = t,
                    .x = (double)p->abs_x / p->abs_max_x, .y = (double)p->abs_y / p->abs_max_y,
                };
                wl_signal_emit_mutable(&p->wlr.events.motion_absolute, &ae);
                p->abs_dirty = 0;
            }
            wl_signal_emit_mutable(&p->wlr.events.frame, &p->wlr);
            break;
        }
    }
    return 0;
}

static int open_input(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) wlr_log(WLR_INFO, "os2: no input device %s: %s", path, strerror(errno));
    return fd;
}

static void add_inputs(struct os2_backend *b)
{
    int fd = open_input(b->kbd_path);
    if (fd >= 0) {
        struct os2_keyboard *k = calloc(1, sizeof *k);
        k->fd = fd;
        ioctl(fd, EVIOCGRAB, 1);                    /* keys are ours, not the console's */
        wlr_keyboard_init(&k->wlr, &keyboard_impl, "os2-keyboard");
        k->src = wl_event_loop_add_fd(b->loop, fd, WL_EVENT_READABLE, keyboard_readable, k);
        b->keyboard = k;
        wl_signal_emit_mutable(&b->base.events.new_input, &k->wlr.base);
    }
    fd = open_input(b->ptr_path);
    if (fd >= 0) {
        struct os2_pointer *p = calloc(1, sizeof *p);
        p->fd = fd;
        p->abs_max_x = p->abs_max_y = ABS_RANGE_DEFAULT;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > 0) p->abs_max_x = ai.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > 0) p->abs_max_y = ai.maximum;
        wlr_pointer_init(&p->wlr, &pointer_impl, "os2-pointer");
        p->src = wl_event_loop_add_fd(b->loop, fd, WL_EVENT_READABLE, pointer_readable, p);
        b->pointer = p;
        wl_signal_emit_mutable(&b->base.events.new_input, &p->wlr.base);
    }
}

/* ── Backend ──────────────────────────────────────────────────────────── */
static bool backend_start(struct wlr_backend *wb)
{
    struct os2_backend *b = get_backend(wb);
    if (b->started) return true;
    struct os2_output *o = calloc(1, sizeof *o);
    if (!o) return false;
    o->backend = b;
    struct wlr_output_state st;
    wlr_output_state_init(&st);
    wlr_output_state_set_custom_mode(&st, b->width, b->height, REFRESH_MHZ);
    wlr_output_init(&o->wlr, wb, &output_impl, b->loop, &st);
    wlr_output_state_finish(&st);
    wlr_output_set_name(&o->wlr, "FB-1");
    wlr_output_set_description(&o->wlr, "os2warp framebuffer");
    o->frame_timer = wl_event_loop_add_timer(b->loop, send_frame, o);
    b->output = o;
    b->started = true;
    wl_signal_emit_mutable(&wb->events.new_output, &o->wlr);
    add_inputs(b);
    return true;
}

static void backend_destroy(struct wlr_backend *wb)
{
    struct os2_backend *b = get_backend(wb);
    if (b->output) wlr_output_destroy(&b->output->wlr);
    if (b->keyboard) {
        wl_event_source_remove(b->keyboard->src);
        wlr_keyboard_finish(&b->keyboard->wlr);
        ioctl(b->keyboard->fd, EVIOCGRAB, 0);
        close(b->keyboard->fd);
        free(b->keyboard);
    }
    if (b->pointer) {
        wl_event_source_remove(b->pointer->src);
        wlr_pointer_finish(&b->pointer->wlr);
        close(b->pointer->fd);
        free(b->pointer);
    }
    wlr_backend_finish(wb);
    munmap(b->fb, b->fb_size);
    close(b->fb_fd);
    free(b->fb_path); free(b->kbd_path); free(b->ptr_path);
    free(b);
}

static const struct wlr_backend_impl backend_impl = {
    .start = backend_start,
    .destroy = backend_destroy,
};

bool wlr_backend_is_os2(struct wlr_backend *b) { return b->impl == &backend_impl; }

const char *wlr_os2_fallback_keymap(void) { return os2_keymap_us; }

static char *pick(const char *arg, const char *env, const char *def)
{
    const char *e = getenv(env);
    return strdup(arg ? arg : e && *e ? e : def);
}

struct wlr_backend *wlr_os2_backend_create(struct wl_event_loop *loop, const char *fb,
                                           const char *keyboard, const char *pointer)
{
    struct os2_backend *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->loop = loop;
    b->fb_path = pick(fb, "WLR_OS2_FB", "/dev/fb0");
    b->kbd_path = pick(keyboard, "WLR_OS2_KBD", "/dev/input/event0");
    b->ptr_path = pick(pointer, "WLR_OS2_PTR", "/dev/input/event1");
    b->fb_fd = open(b->fb_path, O_RDWR | O_CLOEXEC);
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (b->fb_fd < 0 || ioctl(b->fb_fd, FBIOGET_VSCREENINFO, &v) || ioctl(b->fb_fd, FBIOGET_FSCREENINFO, &f)) {
        wlr_log(WLR_ERROR, "os2: framebuffer %s: %s", b->fb_path, strerror(errno));
        goto fail;
    }
    if (v.bits_per_pixel != 32 || v.red.offset != 16 || v.green.offset != 8 || v.blue.offset != 0) {
        wlr_log(WLR_ERROR, "os2: %s is not XRGB8888 (%u bpp)", b->fb_path, v.bits_per_pixel);
        goto fail;
    }
    b->width = (int)v.xres;
    b->height = (int)v.yres;
    b->stride = (int)f.line_length;
    b->fb_size = (size_t)b->stride * v.yres;
    b->fb = mmap(NULL, b->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fb_fd, 0);
    if (b->fb == MAP_FAILED) {
        wlr_log(WLR_ERROR, "os2: mmap %s: %s", b->fb_path, strerror(errno));
        goto fail;
    }
    wlr_backend_init(&b->base, &backend_impl);
    b->base.buffer_caps = WLR_BUFFER_CAP_DATA_PTR | WLR_BUFFER_CAP_SHM;
    wlr_log(WLR_INFO, "os2: %s %dx%d, stride %d", b->fb_path, b->width, b->height, b->stride);
    return &b->base;
fail:
    if (b->fb_fd >= 0) close(b->fb_fd);
    free(b->fb_path); free(b->kbd_path); free(b->ptr_path);
    free(b);
    return NULL;
}
