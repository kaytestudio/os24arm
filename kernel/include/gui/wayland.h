/**
 * @file wayland.h
 * @brief Wayland-style compositor for OS/2 Warp ARM64
 *
 * Structured after Wayland's core concepts - a compositor that composites
 * client surfaces (each with their own pixel buffer) onto an output, an
 * xdg_toplevel-style role for decorated windows, damage tracking, and a
 * seat that routes pointer input to the focused surface.
 *
 * This is NOT the real Wayland wire protocol: there is no second process
 * to speak it to here (freestanding, single address space, no sockets).
 * Client and compositor run in the same kernel, but the architecture
 * mirrors the real split deliberately: client code
 * renders into a surface's own buffer via a draw callback, and only the
 * compositor ever touches the shared output framebuffer directly. See
 * fs/README or the project conversation history for why real Wayland/Qt6
 * can't run here at all.
 */

#ifndef WAYLAND_H
#define WAYLAND_H

#include "types.h"
#include "gui/framebuffer.h"

typedef struct wl_surface wl_surface_t;

/* Number of independent workplaces (virtual desktops): each holds its own
 * set of surfaces, only one is composited/visible at a time. Switch with
 * wl_compositor_set_workplace()/wl_compositor_cycle_workplace() (see
 * main.c's Cmd+Escape handling via gui/virtio_input.h). */
#define WL_MAX_WORKPLACES 4

/**
 * @brief Client-supplied content renderer
 *
 * Called by the compositor when a surface's buffer needs repainting
 * (i.e. it has pending damage). Draw into wl_buf_* calls against
 * `surface`, using surface-local coordinates (0,0 is the surface's own
 * top-left corner, not its position on the output).
 */
typedef void (*wl_surface_draw_fn)(wl_surface_t *surface, fb_rect_t damage);

typedef enum {
    WL_SURFACE_ROLE_NONE = 0,
    WL_SURFACE_ROLE_TOPLEVEL,
} wl_surface_role_t;

struct wl_surface {
    /* Client-owned buffer: this surface's own pixel content. Only client
     * code (via wl_buf_* calls) writes here - the compositor only reads
     * it, to blit onto the shared output. */
    fb_color_t *buffer;
    int width, height;

    int x, y;                  /* position on the output */
    wl_surface_role_t role;
    int workplace;              /* which virtual desktop this belongs to (0..WL_MAX_WORKPLACES-1) */

    bool mapped;
    bool has_damage;           /* pending repaint before next composite */
    fb_rect_t damage;          /* accumulated damage, surface-local coords */

    wl_surface_draw_fn draw;
    void *user_data;

    /* xdg_toplevel state - meaningful only when role == WL_SURFACE_ROLE_TOPLEVEL */
    char title[64];
    bool activated;             /* focused / has keyboard+decoration highlight */

    wl_surface_t *next;         /* compositor's surface list, back to front */
};

/* Compositor */

void wl_compositor_init(int output_width, int output_height);
wl_surface_t *wl_compositor_create_surface(int width, int height);
void wl_surface_destroy(wl_surface_t *surface);
void wl_compositor_raise(wl_surface_t *surface);   /* bring to front + focus */
void wl_compositor_flush(void);                     /* composite onto the output */
int wl_compositor_surface_count(void);

/* Workplaces (virtual desktops) */

/* Switch the active workplace (0..WL_MAX_WORKPLACES-1): surfaces on other
 * workplaces stop being composited until switched back to. Forces a full
 * repaint (clears the desktop, redamages every surface on the new
 * workplace) and flushes immediately. Out-of-range or no-op indices are
 * ignored. New surfaces are created on whichever workplace is active at
 * creation time (see wl_compositor_create_surface). */
void wl_compositor_set_workplace(int index);
int wl_compositor_get_workplace(void);
void wl_compositor_cycle_workplace(void); /* advance to the next one, wrapping */

/* wl_surface client requests */

void wl_surface_set_draw_callback(wl_surface_t *surface, wl_surface_draw_fn fn);
void wl_surface_damage(wl_surface_t *surface, int x, int y, int w, int h);
void wl_surface_commit(wl_surface_t *surface);      /* repaint buffer if damaged */
void wl_surface_move(wl_surface_t *surface, int x, int y);

/* xdg_shell-ish toplevel role */

wl_surface_t *xdg_toplevel_create(int x, int y, int width, int height, const char *title);
void xdg_toplevel_close(wl_surface_t *surface);

/* Draw into a surface's buffer, not the shared output framebuffer. */

void wl_buf_putpixel(wl_surface_t *surface, int x, int y, fb_color_t color);
void wl_buf_fillrect(wl_surface_t *surface, int x, int y, int w, int h, fb_color_t color);
void wl_buf_drawstring(wl_surface_t *surface, int x, int y, const char *str, fb_color_t fg, fb_color_t bg);

/* wl_seat: pointer input routed to the surface under the cursor */

void wl_seat_pointer_button(int x, int y, int button, bool pressed);
void wl_seat_pointer_motion(int x, int y);
wl_surface_t *wl_seat_focused_surface(void);

#endif /* WAYLAND_H */
