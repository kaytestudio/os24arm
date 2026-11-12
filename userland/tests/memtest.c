/* userland/tests/memtest.c - malloc/free/realloc (brk and mmap paths) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    int bad = 0;
    char *p[200];
    for (int i = 0; i < 200; i++) {                 /* small: brk heap */
        p[i] = malloc(16 + i * 37);
        if (!p[i]) { printf("malloc %d failed\n", i); return 1; }
        memset(p[i], i, 16 + i * 37);
    }
    for (int i = 0; i < 200; i += 2) free(p[i]);
    for (int i = 1; i < 200; i += 2)
        for (int k = 0; k < 16 + i * 37; k++) if ((unsigned char)p[i][k] != i) bad++;
    size_t big = 4u << 20;                          /* large: mmap */
    unsigned char *b = malloc(big);
    if (!b) { printf("4 MB malloc failed\n"); return 1; }
    for (size_t k = 0; k < big; k++) b[k] = (unsigned char)(k * 7);
    b = realloc(b, big * 2);
    if (!b) { printf("realloc failed\n"); return 1; }
    for (size_t k = 0; k < big; k++) if (b[k] != (unsigned char)(k * 7)) { bad++; break; }
    free(b);
    printf("memtest: %s (%d bad)\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
