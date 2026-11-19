/**
 * @file gui_demo.c
 * @brief GUI Demo for OS/2 Warp ARM64
 */

#include "gui/framebuffer.h"
#include "gui/wayland.h"
#include "uart.h"
#include "kprintf.h"

/* Demo State */
static int mouse_x = 512;
static int mouse_y = 384;

/**
 * @brief Draw desktop icons
 *
 * Desktop chrome (background, icons, cursor) is compositor-owned, not a
 * client surface, so it draws straight into the shared output framebuffer
 * via fb_* - unlike window content, which is client-owned and only ever
 * touches its own surface buffer via wl_buf_*.
 */
static void draw_desktop_icons(void)
{
    /* OS/2 System icon */
    fb_rect_t icon1 = {50, 50, 64, 64};
    fb_fillrect(&icon1, WPS_TEXT_WHITE);

    /* Draw icon border */
    for (int i = 0; i < 64; i++) {
        fb_putpixel(50 + i, 50, WPS_TEXT_BLACK);
        fb_putpixel(50 + i, 113, WPS_TEXT_BLACK);
        fb_putpixel(50, 50 + i, WPS_TEXT_BLACK);
        fb_putpixel(113, 50 + i, WPS_TEXT_BLACK);
    }

    /* Draw label */
    fb_drawstring(58, 120, "System", WPS_TEXT_WHITE, WPS_DESKTOP_BG);

    /* Shredder (trash) icon */
    fb_rect_t icon2 = {50, 150, 64, 64};
    fb_fillrect(&icon2, WPS_TEXT_WHITE);
    for (int i = 0; i < 64; i++) {
        fb_putpixel(50 + i, 150, WPS_TEXT_BLACK);
        fb_putpixel(50 + i, 213, WPS_TEXT_BLACK);
        fb_putpixel(50, 150 + i, WPS_TEXT_BLACK);
        fb_putpixel(113, 150 + i, WPS_TEXT_BLACK);
    }
    fb_drawstring(50, 220, "Shredder", WPS_TEXT_WHITE, WPS_DESKTOP_BG);

    /* Network icon */
    fb_rect_t icon3 = {50, 250, 64, 64};
    fb_fillrect(&icon3, WPS_TEXT_WHITE);
    for (int i = 0; i < 64; i++) {
        fb_putpixel(50 + i, 250, WPS_TEXT_BLACK);
        fb_putpixel(50 + i, 313, WPS_TEXT_BLACK);
        fb_putpixel(50, 250 + i, WPS_TEXT_BLACK);
        fb_putpixel(113, 250 + i, WPS_TEXT_BLACK);
    }
    fb_drawstring(50, 320, "Network", WPS_TEXT_WHITE, WPS_DESKTOP_BG);
}

/**
 * @brief Convert a non-negative integer to a decimal string (no libc)
 */
static char *itoa_dec(int value, char *out)
{
    if (value == 0) {
        out[0] = '0';
        out[1] = '\0';
        return out;
    }

    char tmp[12];
    int i = 0;
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    int j = 0;
    while (i > 0) {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';
    return out;
}

/**
 * @brief Render live framebuffer test-pattern content into a surface
 *
 * Client-side rendering: draws into the surface's own buffer via wl_buf_*
 * (local coordinates, 0,0 = this surface's own top-left corner) - an RGB
 * gradient, primary-color stripes, and the live resolution - to
 * demonstrate a window backed by real pixel content the compositor then
 * blits onto the output, rather than a flat background fill.
 */
void draw_framebuffer_content(wl_surface_t *surface, fb_rect_t damage)
{
    (void)damage;

    int width = surface->width;
    int height = surface->height;

    /* Horizontal RGB gradient bar */
    int bar_h = 32;
    if (bar_h > height) bar_h = height;
    for (int dx = 0; dx < width; dx++) {
        uint8_t level = (uint8_t)((dx * 255) / (width > 1 ? width - 1 : 1));
        fb_color_t color = FB_COLOR(level, 255 - level, 128);
        wl_buf_fillrect(surface, dx, 0, 1, bar_h, color);
    }

    /* Primary-color stripes */
    int stripe_y = bar_h + 8;
    int stripe_h = 24;
    if (stripe_y + stripe_h <= height) {
        int stripe_w = width / 3;
        wl_buf_fillrect(surface, 0, stripe_y, stripe_w, stripe_h, FB_COLOR(255, 0, 0));
        wl_buf_fillrect(surface, stripe_w, stripe_y, stripe_w, stripe_h, FB_COLOR(0, 255, 0));
        wl_buf_fillrect(surface, 2 * stripe_w, stripe_y, width - 2 * stripe_w, stripe_h, FB_COLOR(0, 0, 255));
    }

    /* Live resolution readout */
    int text_y = stripe_y + stripe_h + 12;
    if (text_y + 8 <= height) {
        char w_str[12], h_str[12];
        itoa_dec(fb_get_width(), w_str);
        itoa_dec(fb_get_height(), h_str);

        char line[32];
        int p = 0;
        for (int i = 0; w_str[i]; i++) line[p++] = w_str[i];
        line[p++] = 'x';
        for (int i = 0; h_str[i]; i++) line[p++] = h_str[i];
        line[p] = '\0';

        wl_buf_drawstring(surface, 8, text_y, line, WPS_TEXT_BLACK, WPS_WINDOW_BG);
        wl_buf_drawstring(surface, 8, text_y + 16, "Test Pattern", WPS_TEXT_BLACK, WPS_WINDOW_BG);
    }
}

/**
 * @brief Fill a surface with a flat background color (plain windows with
 * no live content still need their buffer painted once).
 */
static void draw_flat_background(wl_surface_t *surface, fb_rect_t damage)
{
    (void)damage;
    wl_buf_fillrect(surface, 0, 0, surface->width, surface->height, WPS_WINDOW_BG);
}

/**
 * @brief Draw the mouse cursor.
 */
static void draw_cursor(void)
{
    /* Simple 11x16 arrow cursor using uint16_t for proper bit width */
    static const uint16_t cursor_mask[16] = {
        0x400,  /* 10000000000 */
        0x600,  /* 11000000000 */
        0x700,  /* 11100000000 */
        0x780,  /* 11110000000 */
        0x7C0,  /* 11111000000 */
        0x7E0,  /* 11111100000 */
        0x7F0,  /* 11111110000 */
        0x7F8,  /* 11111111000 */
        0x7FC,  /* 11111111100 */
        0x7FE,  /* 11111111110 */
        0x7E0,  /* 11111100000 */
        0x6E0,  /* 11011100000 */
        0x430,  /* 10000110000 */
        0x030,  /* 00000110000 */
        0x018,  /* 00000011000 */
        0x000,  /* 00000000000 */
    };

    /* Draw white cursor with black outline */
    for (int y = 0; y < 16; y++) {
        uint16_t row = cursor_mask[y];
        for (int x = 0; x < 11; x++) {
            if (row & (1 << (10 - x))) {
                /* Draw white pixel */
                fb_putpixel(mouse_x + x, mouse_y + y, WPS_TEXT_WHITE);
                /* Draw black outline */
                if (x > 0 && !(row & (1 << (11 - x)))) {
                    fb_putpixel(mouse_x + x - 1, mouse_y + y, WPS_TEXT_BLACK);
                }
            }
        }
    }
}

/**
 * @brief Main GUI demo
 */
void gui_demo_run(void)
{
    kprintf("\r\n");
    kprintf("═══════════════════════════════════════════════════════════\r\n");
    kprintf(" OS/2 Warp ARM64 - GUI Demo\r\n");
    kprintf("═══════════════════════════════════════════════════════════\r\n");
    kprintf("\r\n");

    fb_init();

    wl_compositor_init(fb_get_width(), fb_get_height());

    wl_surface_t *win1 = xdg_toplevel_create(200, 100, 400, 300, "OS/2 System");
    wl_surface_t *win2 = xdg_toplevel_create(300, 200, 350, 250, "Drive C:");
    wl_surface_t *win3 = xdg_toplevel_create(400, 300, 300, 200, "Command Prompts");
    wl_surface_t *win4 = xdg_toplevel_create(150, 420, 320, 220, "Screen");

    wl_surface_set_draw_callback(win1, draw_flat_background);
    wl_surface_set_draw_callback(win2, draw_flat_background);
    wl_surface_set_draw_callback(win3, draw_flat_background);
    wl_surface_set_draw_callback(win4, draw_framebuffer_content);

    kprintf("[GUI] Demo windows created\r\n");
    kprintf("[GUI] Use mouse to drag windows, click X to close\r\n");
    kprintf("[GUI] Press ESC to exit demo\r\n\r\n");

    bool running = true;
    int frame = 0;

    while (running) {
        /* Clear screen to desktop color and draw desktop chrome once,
         * before the compositor blits surfaces on top of it. */
        fb_clear(WPS_DESKTOP_BG);
        draw_desktop_icons();

        wl_compositor_flush();

        draw_cursor();

        /* Keyboard and tablet input are not wired up; cursor movement below is synthetic. */

        /* Move the demo cursor until real pointer input is connected. */
        frame++;
        if (frame % 100 == 0) {
            mouse_x = (mouse_x + 5) % fb_get_width();
            mouse_y = (mouse_y + 3) % fb_get_height();
        }

        if (wl_compositor_surface_count() == 0) {
            kprintf("[GUI] All windows closed, exiting demo\r\n");
            running = false;
        }

        /* Avoid spinning until the compositor has an event loop. */
        for (volatile int i = 0; i < 100000; i++);
    }

    kprintf("[GUI] Demo complete\r\n");
}

/**
 * @brief Entry point for GUI mode
 *
 * Call this from kernel_main() after subsystems are initialized.
 */
void start_gui(void)
{
    gui_demo_run();
}
