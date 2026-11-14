/* image.c - see image.h: stb_image does the decoding, from memory only;
 * this scales down (an average over each pixel's area) and turns RGBA
 * into the premultiplied ARGB pixman draws. */
#include <stdlib.h>
#include "image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 8192
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

uint32_t *image_decode(const unsigned char *data, int len, int max_w, int max_h, int *w, int *h)
{
    int sw, sh, n;
    unsigned char *px = stbi_load_from_memory(data, len, &sw, &sh, &n, 4);
    if (!px) return NULL;
    int dw = sw, dh = sh;
    if (max_w > 0 && dw > max_w) { dh = (int)((long)dh * max_w / dw); dw = max_w; }
    if (max_h > 0 && dh > max_h) { dw = (int)((long)dw * max_h / dh); dh = max_h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint32_t *out = malloc((size_t)dw * dh * 4);
    if (!out) { stbi_image_free(px); return NULL; }
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0, a = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const unsigned char *p = px + ((size_t)yy * sw + xx) * 4;
                    r += (unsigned long)p[0] * p[3]; g += (unsigned long)p[1] * p[3]; b += (unsigned long)p[2] * p[3];
                    a += p[3];
                    k++;
                }
            /* premultiplied: colour * alpha, averaged */
            uint32_t A = (uint32_t)(a / k), R = (uint32_t)(r / k / 255), G = (uint32_t)(g / k / 255), B = (uint32_t)(b / k / 255);
            out[(size_t)y * dw + x] = A << 24 | R << 16 | G << 8 | B;
        }
    }
    stbi_image_free(px);
    *w = dw;
    *h = dh;
    return out;
}
