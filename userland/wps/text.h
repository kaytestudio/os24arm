/* text.h - the Workplace Shell's text, through fcft (text.c: C, because
 * fcft.h uses C99 array parameters C++ can't read). */
#ifndef WPS_TEXT_H
#define WPS_TEXT_H
#include <stdint.h>
#include <pixman.h>
#ifdef __cplusplus
extern "C" {
#endif

/* fonts: 0 the normal UI font, 1 bold (titles), 2 small (icon labels) */
enum { TEXT_UI, TEXT_BOLD, TEXT_SMALL, TEXT_FONTS };

int  text_init(void);                                  /* 0: ready */
int  text_ascent(int font);
int  text_height(int font);
int  text_width(int font, const char *utf8);
/* draws utf8 with its baseline at y, in colour argb (premultiplied, 8 bits
 * per channel); returns the x after it. Stops at max_x (> 0), with "...". */
int  text_draw(pixman_image_t *dst, int font, int x, int y, const char *utf8,
               uint32_t argb, int max_x);

#ifdef __cplusplus
}
#endif
#endif
