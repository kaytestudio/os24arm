/* stdlib.h - what lwIP takes from it (netif.c: atoi), in the kernel */
#ifndef OS2WARP_LWIP_STDLIB_H
#define OS2WARP_LWIP_STDLIB_H
static inline int atoi(const char *s)
{
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}
#endif
