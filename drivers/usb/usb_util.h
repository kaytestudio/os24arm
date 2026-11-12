/* drivers/usb/usb_util.h - small helpers shared by the USB drivers */
#ifndef DRIVERS_USB_UTIL_H
#define DRIVERS_USB_UTIL_H

#include "types.h"

typedef __UINT64_TYPE__ u64;   /* types.h leaves uint64_t undefined */

/* Generic timer: the virtual counter is always readable at EL1. */
static inline u64 usb_ticks(void)
{
    u64 v;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}

static inline u64 usb_ms(uint32_t ms)
{
    u64 f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f / 1000 * ms;
}

static inline void usb_delay_ms(uint32_t ms)
{
    u64 end = usb_ticks() + usb_ms(ms);
    while (usb_ticks() < end) { }
}

static inline void usb_zero(void *p, uint32_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}

static inline void usb_copy(void *dst, const void *src, uint32_t n)
{
    volatile uint8_t *d = dst;
    const volatile uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static inline void usb_setup(uint8_t *p, uint8_t type, uint8_t req,
                             uint16_t value, uint16_t index, uint16_t len)
{
    p[0] = type; p[1] = req;
    p[2] = (uint8_t)value; p[3] = (uint8_t)(value >> 8);
    p[4] = (uint8_t)index; p[5] = (uint8_t)(index >> 8);
    p[6] = (uint8_t)len;   p[7] = (uint8_t)(len >> 8);
}

/* Walk the descriptors of a configuration (cfg, len bytes): start with
 * *off = 0; each call returns the next descriptor that lies wholly inside
 * the buffer (bLength >= 2), or NULL at the end or on a malformed one.
 * Callers still check bLength for the descriptor type they read. */
static inline const uint8_t *usb_desc_next(const uint8_t *cfg, uint32_t len, uint32_t *off)
{
    uint32_t o = *off;
    if (o + 2 > len || cfg[o] < 2 || cfg[o] > len - o) return 0;
    *off = o + cfg[o];
    return cfg + o;
}

#endif /* DRIVERS_USB_UTIL_H */
