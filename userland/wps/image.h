/* image.h - pictures for the Workplace Shell (image.c: stb_image) */
#ifndef WPS_IMAGE_H
#define WPS_IMAGE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Decodes a PNG, JPEG, GIF (its first picture) or BMP in memory, made
 * smaller to fit max_w x max_h if it's bigger: premultiplied ARGB, w*h
 * pixels (free() them), or NULL. */
uint32_t *image_decode(const unsigned char *data, int len, int max_w, int max_h, int *w, int *h);
#ifdef __cplusplus
}
#endif
#endif
