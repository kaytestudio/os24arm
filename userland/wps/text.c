/* text.c - see text.h. DejaVu Sans (the stick's fonts.conf), rendered by
 * fcft with grey-scale antialiasing. */
#include "text.h"
#include <fcft/fcft.h>
#include <string.h>

static struct fcft_font *fonts[TEXT_FONTS];

int text_init(void)
{
    fcft_init(FCFT_LOG_COLORIZE_NEVER, false, FCFT_LOG_CLASS_ERROR);
    static const char *names[TEXT_FONTS][1] = {
        { "DejaVu Sans:size=8" }, { "DejaVu Sans:size=8:weight=bold" }, { "DejaVu Sans:size=7" },
        { "DejaVu Sans Mono:size=9" },
        { "DejaVu Sans:size=9" }, { "DejaVu Sans:size=9:weight=bold" },
        { "DejaVu Sans:size=15:weight=bold" }, { "DejaVu Sans:size=11:weight=bold" },
    };
    for (int i = 0; i < TEXT_FONTS; i++) {
        fonts[i] = fcft_from_name(1, names[i], "antialias=true:hinting=true");
        if (!fonts[i]) fonts[i] = fcft_from_name(1, (const char *[]){ "monospace:size=8" }, NULL);
        if (!fonts[i]) return -1;
    }
    return 0;
}

int text_ascent(int f) { return fonts[f]->ascent; }
int text_height(int f) { return fonts[f]->height; }

/* the next code point of utf8 (*p moves past it) */
static uint32_t next_cp(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t c = *s++;
    if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); s += 3; }
    else if (c >= 0xE0 && s[0] && s[1]) { c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2; }
    else if (c >= 0xC0 && s[0]) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
    *p = s;
    return c;
}

int text_width(int f, const char *utf8)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        const struct fcft_glyph *g = fcft_rasterize_char_utf32(fonts[f], next_cp(&p), FCFT_SUBPIXEL_NONE);
        if (g) w += g->advance.x;
    }
    return w;
}

static void draw_glyph(pixman_image_t *dst, pixman_image_t *src, const struct fcft_glyph *g, int x, int y)
{
    if (g->is_color_glyph)
        pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, dst, 0, 0, 0, 0,
                                 x + g->x, y - g->y, g->width, g->height);
    else
        pixman_image_composite32(PIXMAN_OP_OVER, src, g->pix, dst, 0, 0, 0, 0,
                                 x + g->x, y - g->y, g->width, g->height);
}

int text_draw(pixman_image_t *dst, int f, int x, int y, const char *utf8, uint32_t argb, int max_x)
{
    pixman_color_t c = {
        (uint16_t)(((argb >> 16) & 0xFF) * 257), (uint16_t)(((argb >> 8) & 0xFF) * 257),
        (uint16_t)((argb & 0xFF) * 257), (uint16_t)(((argb >> 24) & 0xFF) * 257),
    };
    pixman_image_t *src = pixman_image_create_solid_fill(&c);
    const struct fcft_glyph *dot = fcft_rasterize_char_utf32(fonts[f], '.', FCFT_SUBPIXEL_NONE);
    int ell = dot ? 3 * dot->advance.x : 0;
    int fits = max_x <= 0 || x + text_width(f, utf8) <= max_x;
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        const struct fcft_glyph *g = fcft_rasterize_char_utf32(fonts[f], next_cp(&p), FCFT_SUBPIXEL_NONE);
        if (!g) continue;
        if (!fits && x + g->advance.x > max_x - ell) {          /* "..." and stop */
            for (int i = 0; i < 3 && dot; i++) { draw_glyph(dst, src, dot, x, y); x += dot->advance.x; }
            break;
        }
        draw_glyph(dst, src, g, x, y);
        x += g->advance.x;
    }
    pixman_image_unref(src);
    return x;
}
