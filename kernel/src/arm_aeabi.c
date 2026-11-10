/**
 * @file arm_aeabi.c
 * @brief ARM EABI run-time helpers (BeagleBone Black, ARMv7)
 *
 * On 32-bit ARM, clang lowers struct copies and large zero-initialisers
 * to the run-time ABI's __aeabi_mem* helpers rather than plain
 * memcpy/memset. They normally come from compiler-rt or libgcc, neither
 * of which this freestanding (-nostdlib) build links, so provide them
 * here on top of string.c.
 *
 * Note the argument order of __aeabi_memset: (dest, n, c), unlike memset.
 * The 4/8 variants only promise extra alignment, which string.c's
 * byte-wise loops don't need.
 */

#include "types.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memmove(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);

void __aeabi_memcpy(void *dst, const void *src, size_t n)   { memcpy(dst, src, n); }
void __aeabi_memcpy4(void *dst, const void *src, size_t n)  { memcpy(dst, src, n); }
void __aeabi_memcpy8(void *dst, const void *src, size_t n)  { memcpy(dst, src, n); }

void __aeabi_memmove(void *dst, const void *src, size_t n)  { memmove(dst, src, n); }
void __aeabi_memmove4(void *dst, const void *src, size_t n) { memmove(dst, src, n); }
void __aeabi_memmove8(void *dst, const void *src, size_t n) { memmove(dst, src, n); }

void __aeabi_memset(void *dst, size_t n, int c)             { memset(dst, c, n); }
void __aeabi_memset4(void *dst, size_t n, int c)            { memset(dst, c, n); }
void __aeabi_memset8(void *dst, size_t n, int c)            { memset(dst, c, n); }

void __aeabi_memclr(void *dst, size_t n)                    { memset(dst, 0, n); }
void __aeabi_memclr4(void *dst, size_t n)                   { memset(dst, 0, n); }
void __aeabi_memclr8(void *dst, size_t n)                   { memset(dst, 0, n); }

/* ── Integer division ─────────────────────────────────────────────────────
 * The Cortex-A8 has no hardware divide instruction (UDIV/SDIV arrived with
 * the Cortex-A7/A15), so every `/` and `%` compiles to one of these. All
 * are plain shift-and-subtract; division by zero returns 0 rather than
 * trapping (there is no __aeabi_idiv0 handler to call).
 *
 * The *divmod variants return the quotient and remainder in registers:
 * the 32-bit ones in r0/r1, which is exactly how AAPCS returns a 64-bit
 * value, so they are ordinary C functions returning (rem << 32) | quot.
 * The 64-bit ones return the quotient in r0:r1 and the remainder in r2:r3,
 * which C can't express, so a small naked asm shim calls a C helper that
 * writes the remainder through a pointer and then loads it into r2:r3. */

typedef unsigned long long u64;
typedef long long s64;

static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
    u64 q = 0, r = 0;
    if (d == 0) {
        if (rem) *rem = 0;
        return 0;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (u64)1 << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

static uint32_t udivmod32(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, r = 0;
    if (d == 0) {
        *rem = 0;
        return 0;
    }
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    *rem = r;
    return q;
}

uint32_t __aeabi_uidiv(uint32_t n, uint32_t d)
{
    uint32_t r;
    return udivmod32(n, d, &r);
}

u64 __aeabi_uidivmod(uint32_t n, uint32_t d)
{
    uint32_t r;
    uint32_t q = udivmod32(n, d, &r);
    return ((u64)r << 32) | q;
}

int32_t __aeabi_idiv(int32_t n, int32_t d)
{
    uint32_t r;
    uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
    uint32_t ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
    uint32_t q = udivmod32(un, ud, &r);
    return ((n < 0) != (d < 0)) ? (int32_t)(0u - q) : (int32_t)q;
}

u64 __aeabi_idivmod(int32_t n, int32_t d)
{
    uint32_t r;
    uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
    uint32_t ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
    uint32_t q = udivmod32(un, ud, &r);
    if ((n < 0) != (d < 0)) q = 0u - q;
    if (n < 0) r = 0u - r;           /* remainder takes the dividend's sign */
    return ((u64)r << 32) | q;
}

u64 os2_aeabi_udivmod64(u64 n, u64 d, u64 *rem)
{
    return udivmod64(n, d, rem);
}

s64 os2_aeabi_sdivmod64(s64 n, s64 d, s64 *rem)
{
    u64 un = n < 0 ? 0 - (u64)n : (u64)n;
    u64 ud = d < 0 ? 0 - (u64)d : (u64)d;
    u64 ur;
    u64 q = udivmod64(un, ud, &ur);
    *rem = n < 0 ? -(s64)ur : (s64)ur;
    return ((n < 0) != (d < 0)) ? -(s64)q : (s64)q;
}

/* n in r0:r1, d in r2:r3 -> quotient r0:r1, remainder r2:r3. The helper's
 * 5th argument (the remainder pointer) goes on the stack at [sp]. */
__attribute__((naked)) void __aeabi_uldivmod(void)
{
    __asm__ volatile(
        "push   {r4, lr}\n"
        "sub    sp, sp, #16\n"
        "add    r4, sp, #8\n"
        "str    r4, [sp]\n"
        "bl     os2_aeabi_udivmod64\n"
        "ldr    r2, [sp, #8]\n"
        "ldr    r3, [sp, #12]\n"
        "add    sp, sp, #16\n"
        "pop    {r4, pc}\n");
}

__attribute__((naked)) void __aeabi_ldivmod(void)
{
    __asm__ volatile(
        "push   {r4, lr}\n"
        "sub    sp, sp, #16\n"
        "add    r4, sp, #8\n"
        "str    r4, [sp]\n"
        "bl     os2_aeabi_sdivmod64\n"
        "ldr    r2, [sp, #8]\n"
        "ldr    r3, [sp, #12]\n"
        "add    sp, sp, #16\n"
        "pop    {r4, pc}\n");
}
