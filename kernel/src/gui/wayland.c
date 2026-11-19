/**
 * @file wayland.c
 * @brief Wayland-style compositor implementation
 *
 * See wayland.h for the architectural notes on what "Wayland-style" means
 * here (no real wire protocol - single address space, no processes).
 */

#include "gui/wayland.h"
#include "gui/framebuffer.h"
#include "kprintf.h"
#include "string.h"

extern void *mem_alloc(size_t size);
extern void mem_free(void *ptr);

#define MAX_SURFACES 32
#define TITLE_BAR_HEIGHT 24
#define BORDER_SIZE 2

static wl_surface_t *surfaces[MAX_SURFACES];
static int surface_count = 0;
static wl_surface_t *focused_surface = NULL;
static wl_surface_t *dragging_surface = NULL;
static int drag_offset_x = 0;
static int drag_offset_y = 0;

static int output_width = 0;
static int output_height = 0;
static int active_workplace = 0;

static int cursor_x = 0;
static int cursor_y = 0;

/* 11x16 arrow, hotspot at the tip (top-left, row/col 0). Bit set = cursor
 * pixel. Same shape convention as the classic Windows/OS2 arrow pointer. */
static const uint16_t cursor_shape[16] = {
    0x8000, 0xC000, 0xE000, 0xF000,
    0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFE0, 0xFC00, 0xDC00,
    0x8600, 0x0600, 0x0300, 0x0000,
};

static void draw_cursor(void)
{
    for (int y = 0; y < 16; y++) {
        uint16_t row = cursor_shape[y];
        for (int x = 0; x < 11; x++) {
            if (!(row & (0x8000u >> x))) continue;
            fb_putpixel(cursor_x + x, cursor_y + y, WPS_TEXT_WHITE);
            /* Black outline on the trailing edge of each filled run. */
            if (x == 10 || !(row & (0x8000u >> (x + 1)))) {
                fb_putpixel(cursor_x + x + 1, cursor_y + y, WPS_TEXT_BLACK);
            }
        }
    }
}

/* Compositor */

void wl_compositor_init(int width, int height)
{
    kprintf("[WL] Compositor initializing (%dx%d output)...\r\n", width, height);

    for (int i = 0; i < MAX_SURFACES; i++) surfaces[i] = NULL;
    surface_count = 0;
    focused_surface = NULL;
    dragging_surface = NULL;
    output_width = width;
    output_height = height;
    active_workplace = 0;
    cursor_x = width / 2;
    cursor_y = height / 2;

    kprintf("[WL] Compositor ready\r\n");
}

wl_surface_t *wl_compositor_create_surface(int width, int height)
{
    if (surface_count >= MAX_SURFACES) {
        kprintf("[WL] ERROR: Too many surfaces (max %d)\r\n", MAX_SURFACES);
        return NULL;
    }
    if (width <= 0 || height <= 0) return NULL;

    wl_surface_t *s = (wl_surface_t *)mem_alloc(sizeof(wl_surface_t));
    if (!s) {
        kprintf("[WL] ERROR: Failed to allocate surface\r\n");
        return NULL;
    }
    memset(s, 0, sizeof(*s));

    s->buffer = (fb_color_t *)mem_alloc((size_t)width * (size_t)height * sizeof(fb_color_t));
    if (!s->buffer) {
        kprintf("[WL] ERROR: Failed to allocate surface buffer\r\n");
        mem_free(s);
        return NULL;
    }
    memset(s->buffer, 0, (size_t)width * (size_t)height * sizeof(fb_color_t));

    s->width = width;
    s->height = height;
    s->role = WL_SURFACE_ROLE_NONE;
    s->workplace = active_workplace;
    s->mapped = true;
    s->has_damage = true; /* new surfaces need an initial paint */
    s->damage = (fb_rect_t){0, 0, width, height};

    surfaces[surface_count++] = s;
    return s;
}

void wl_surface_destroy(wl_surface_t *surface)
{
    if (!surface) return;

    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i] == surface) {
            for (int j = i; j < surface_count - 1; j++) surfaces[j] = surfaces[j + 1];
            surface_count--;
            surfaces[surface_count] = NULL;

            if (focused_surface == surface) {
                focused_surface = surface_count > 0 ? surfaces[surface_count - 1] : NULL;
            }
            if (dragging_surface == surface) dragging_surface = NULL;

            if (surface->buffer) mem_free(surface->buffer);
            mem_free(surface);
            return;
        }
    }
}

void wl_compositor_raise(wl_surface_t *surface)
{
    if (!surface) return;

    for (int i = 0; i < surface_count; i++) {
        if (surfaces[i] == surface) {
            for (int j = i; j < surface_count - 1; j++) surfaces[j] = surfaces[j + 1];
            surfaces[surface_count - 1] = surface;
            break;
        }
    }

    if (focused_surface != surface) {
        if (focused_surface) focused_surface->activated = false;
        focused_surface = surface;
        surface->activated = true;
    }
}

int wl_compositor_surface_count(void)
{
    return surface_count;
}

/* wl_surface client requests */

void wl_surface_set_draw_callback(wl_surface_t *surface, wl_surface_draw_fn fn)
{
    if (!surface) return;
    surface->draw = fn;
    surface->has_damage = true;
    surface->damage = (fb_rect_t){0, 0, surface->width, surface->height};
}

void wl_surface_damage(wl_surface_t *surface, int x, int y, int w, int h)
{
    if (!surface) return;

    if (!surface->has_damage) {
        surface->damage = (fb_rect_t){x, y, w, h};
    } else {
        /* Union the new region into the accumulated damage rect. */
        int x0 = surface->damage.x < x ? surface->damage.x : x;
        int y0 = surface->damage.y < y ? surface->damage.y : y;
        int x1a = surface->damage.x + surface->damage.width;
        int y1a = surface->damage.y + surface->damage.height;
        int x1b = x + w;
        int y1b = y + h;
        int x1 = x1a > x1b ? x1a : x1b;
        int y1 = y1a > y1b ? y1a : y1b;
        surface->damage = (fb_rect_t){x0, y0, x1 - x0, y1 - y0};
    }
    surface->has_damage = true;
}

void wl_surface_commit(wl_surface_t *surface)
{
    if (!surface) return;
    if (surface->has_damage && surface->draw) {
        surface->draw(surface, surface->damage);
    }
    surface->has_damage = false;
    surface->damage = (fb_rect_t){0, 0, 0, 0};
}

void wl_surface_move(wl_surface_t *surface, int x, int y)
{
    if (!surface) return;
    surface->x = x;
    surface->y = y;
}

/* xdg_shell-ish toplevel role */

wl_surface_t *xdg_toplevel_create(int x, int y, int width, int height, const char *title)
{
    wl_surface_t *s = wl_compositor_create_surface(width, height);
    if (!s) return NULL;

    s->role = WL_SURFACE_ROLE_TOPLEVEL;
    s->x = x;
    s->y = y;

    int i = 0;
    while (title[i] && i < 63) { s->title[i] = title[i]; i++; }
    s->title[i] = '\0';

    wl_compositor_raise(s);

    kprintf("[WL] xdg_toplevel created: %s (%dx%d at %d,%d)\r\n", title, width, height, x, y);
    return s;
}

void xdg_toplevel_close(wl_surface_t *surface)
{
    wl_surface_destroy(surface);
}

/* Client-side buffer drawing */

void wl_buf_putpixel(wl_surface_t *surface, int x, int y, fb_color_t color)
{
    if (!surface || !surface->buffer) return;
    if (x < 0 || x >= surface->width || y < 0 || y >= surface->height) return;
    surface->buffer[y * surface->width + x] = color;
}

void wl_buf_fillrect(wl_surface_t *surface, int x, int y, int w, int h, fb_color_t color)
{
    if (!surface) return;
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            wl_buf_putpixel(surface, x + dx, y + dy, color);
        }
    }
}

void wl_buf_drawstring(wl_surface_t *surface, int x, int y, const char *str, fb_color_t fg, fb_color_t bg)
{
    if (!surface) return;
    int cx = x;
    int cy = y;

    while (*str) {
        const uint8_t *glyph = fb_get_glyph(*str);
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                fb_color_t c = (bits & (1 << (7 - col))) ? fg : bg;
                wl_buf_putpixel(surface, cx + col, cy + row, c);
            }
        }
        cx += 8;
        str++;
    }
}

/* Compositing */

static void draw_decorations(wl_surface_t *s)
{
    bool is_active = s->activated;

    fb_color_t border_color = WPS_TEXT_BLACK;
    for (int i = 0; i < BORDER_SIZE; i++) {
        for (int x = s->x - BORDER_SIZE; x < s->x + s->width + BORDER_SIZE; x++) {
            fb_putpixel(x, s->y - BORDER_SIZE + i, border_color);
            fb_putpixel(x, s->y + TITLE_BAR_HEIGHT + s->height + i, border_color);
        }
        for (int y = s->y - BORDER_SIZE; y < s->y + TITLE_BAR_HEIGHT + s->height + BORDER_SIZE; y++) {
            fb_putpixel(s->x - BORDER_SIZE + i, y, border_color);
            fb_putpixel(s->x + s->width + i, y, border_color);
        }
    }

    fb_rect_t titlebar_rect = {s->x, s->y, s->width, TITLE_BAR_HEIGHT};
    fb_draw_titlebar(&titlebar_rect, s->title, is_active);

    fb_rect_t close_btn_rect = {s->x + s->width - 20, s->y + 4, 16, 16};
    fb_draw_button(&close_btn_rect, "X", false);
}

static void blit_surface(wl_surface_t *s, int ox, int oy)
{
    for (int dy = 0; dy < s->height; dy++) {
        for (int dx = 0; dx < s->width; dx++) {
            fb_putpixel(ox + dx, oy + dy, s->buffer[dy * s->width + dx]);
        }
    }
}

void wl_compositor_flush(void)
{
    for (int i = 0; i < surface_count; i++) {
        wl_surface_t *s = surfaces[i];
        if (!s->mapped) continue;
        if (s->workplace != active_workplace) continue;

        wl_surface_commit(s);

        int client_x = s->x;
        int client_y = s->y;
        if (s->role == WL_SURFACE_ROLE_TOPLEVEL) {
            draw_decorations(s);
            client_y += TITLE_BAR_HEIGHT;
        }
        blit_surface(s, client_x, client_y);
    }

    /* Cursor last, on top of every surface. */
    draw_cursor();

    /* Everything above drew into the back buffer; copy it to the front
     * buffer so it's actually visible (ramfb, if attached, only ever
     * displays the front buffer - see gui/ramfb.h). */
    fb_present();
}

/* Workplaces (virtual desktops) */

int wl_compositor_get_workplace(void)
{
    return active_workplace;
}

void wl_compositor_set_workplace(int index)
{
    if (index < 0 || index >= WL_MAX_WORKPLACES) return;
    if (index == active_workplace) return;

    active_workplace = index;
    kprintf("[WL] Switched to workplace %d\r\n", active_workplace + 1);

    /* wl_compositor_flush() only ever blits mapped surfaces on top of
     * whatever's already in the back buffer - it never clears it - so
     * the outgoing workplace's content would otherwise stay visible
     * around/behind the incoming one. Clear, then fully redamage every
     * surface that belongs here now before compositing it in. */
    fb_clear(WPS_DESKTOP_BG);
    for (int i = 0; i < surface_count; i++) {
        wl_surface_t *s = surfaces[i];
        if (s->workplace != active_workplace) continue;
        s->has_damage = true;
        s->damage = (fb_rect_t){0, 0, s->width, s->height};
    }

    wl_compositor_flush();
}

void wl_compositor_cycle_workplace(void)
{
    wl_compositor_set_workplace((active_workplace + 1) % WL_MAX_WORKPLACES);
}

/* wl_seat: pointer input */

void wl_seat_pointer_button(int x, int y, int button, bool pressed)
{
    (void)button;
    kprintf("[WL] Pointer button %d %s at (%d,%d)\r\n",
            button, pressed ? "down" : "up", x, y);
    if (!pressed) {
        dragging_surface = NULL;
        return;
    }

    for (int i = surface_count - 1; i >= 0; i--) {
        wl_surface_t *s = surfaces[i];
        if (!s->mapped) continue;

        int deco_h = (s->role == WL_SURFACE_ROLE_TOPLEVEL) ? TITLE_BAR_HEIGHT : 0;
        if (x >= s->x && x < s->x + s->width &&
            y >= s->y && y < s->y + s->height + deco_h)
        {
            if (s->role == WL_SURFACE_ROLE_TOPLEVEL) {
                int close_x = s->x + s->width - 20;
                int close_y = s->y + 4;
                if (x >= close_x && x < close_x + 16 && y >= close_y && y < close_y + 16) {
                    xdg_toplevel_close(s);
                    return;
                }
                if (y >= s->y && y < s->y + TITLE_BAR_HEIGHT) {
                    dragging_surface = s;
                    drag_offset_x = x - s->x;
                    drag_offset_y = y - s->y;
                }
            }

            wl_compositor_raise(s);
            return;
        }
    }
}

void wl_seat_pointer_motion(int x, int y)
{
    cursor_x = x;
    cursor_y = y;

    if (!dragging_surface) return;

    int new_x = x - drag_offset_x;
    int new_y = y - drag_offset_y;

    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    if (new_x + dragging_surface->width > output_width) new_x = output_width - dragging_surface->width;
    if (new_y + dragging_surface->height + TITLE_BAR_HEIGHT > output_height)
        new_y = output_height - dragging_surface->height - TITLE_BAR_HEIGHT;

    wl_surface_move(dragging_surface, new_x, new_y);
}

wl_surface_t *wl_seat_focused_surface(void)
{
    return focused_surface;
}
