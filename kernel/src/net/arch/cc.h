/* arch/cc.h - lwIP's port to this kernel: its types (the kernel's own
 * stdint.h has only the small ones), diagnostics through kprintf, random
 * numbers from the counter. */
#ifndef OS2WARP_LWIP_CC_H
#define OS2WARP_LWIP_CC_H

#include "stddef.h"

typedef unsigned char      u8_t;
typedef signed char        s8_t;
typedef unsigned short     u16_t;
typedef signed short       s16_t;
typedef unsigned int       u32_t;
typedef signed int         s32_t;
typedef unsigned long long u64_t;
typedef signed long long   s64_t;
typedef unsigned long      mem_ptr_t;
typedef long               ssize_t;
typedef long               ptrdiff_t;
#define LWIP_HAVE_INT64 1

#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define INT_MAX   0x7fffffff
#define SSIZE_MAX 0x7fffffffffffffffL

#define BYTE_ORDER LITTLE_ENDIAN

#include "kprintf.h"
#define LWIP_PLATFORM_DIAG(x)   do { kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { kprintf("[NET] lwIP assertion: %s (%s:%d)\n", x, __FILE__, __LINE__); } while (0)

unsigned int lwip_rand(void);
#define LWIP_RAND() lwip_rand()

#endif
