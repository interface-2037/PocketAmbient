/*
 * rt.c - the few runtime routines the compiler expects on a freestanding RV32IMC target:
 * memset / memcpy (struct copies and clears) and 64-bit division (uint64_t tick math).
 * 64-bit division: long division on the hardware DIVU when the divisor fits 32 bits (every
 * division in the composer), else shift-subtract from the dividend's top bit.
 */
#include <stdint.h>
#include <stddef.h>

void *memset(void *d, int c, size_t n)
{
    volatile uint8_t *p = d;                 /* volatile: stop the compiler calling memset */
    if (!((uintptr_t)p & 3u)) {
        uint32_t w = (uint8_t)c * 0x01010101u;
        for (; n >= 4; n -= 4, p += 4) *(volatile uint32_t *)p = w;
    }
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    volatile uint8_t *p = d;
    const uint8_t *q = s;
    if (!(((uintptr_t)p | (uintptr_t)q) & 3u))
        for (; n >= 4; n -= 4, p += 4, q += 4) *(volatile uint32_t *)p = *(const uint32_t *)q;
    while (n--) *p++ = *q++;
    return d;
}

static unsigned clz32(uint32_t v)
{
    unsigned n = 0;
    if (!(v >> 16)) { n += 16; v <<= 16; }
    if (!(v >> 24)) { n += 8; v <<= 8; }
    if (!(v >> 28)) { n += 4; v <<= 4; }
    if (!(v >> 30)) { n += 2; v <<= 2; }
    if (!(v >> 31)) n += 1;
    return n;
}

/* (u1:u0) / v with u1 < v: 32-bit quotient from two 16-bit digit steps on the hardware DIVU
 * (Hacker's Delight, divlu). */
static uint32_t divlu(uint32_t u1, uint32_t u0, uint32_t v, uint32_t *r)
{
    const uint32_t b = 65536u;
    unsigned s = clz32(v);
    v <<= s;
    uint32_t vn1 = v >> 16, vn0 = v & 0xFFFFu;
    uint32_t un32 = (u1 << s) | (s ? u0 >> (32 - s) : 0u), un10 = u0 << s;
    uint32_t un1 = un10 >> 16, un0 = un10 & 0xFFFFu;
    uint32_t q1 = un32 / vn1, rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) { q1--; rhat += vn1; if (rhat >= b) break; }
    uint32_t un21 = un32 * b + un1 - q1 * v;
    uint32_t q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) { q0--; rhat += vn1; if (rhat >= b) break; }
    *r = (un21 * b + un0 - q0 * v) >> s;
    return q1 * b + q0;
}

static unsigned clz64(uint64_t v)
{
    unsigned n = 0;
    if (!(v >> 32)) { n += 32; v <<= 32; }
    if (!(v >> 48)) { n += 16; v <<= 16; }
    if (!(v >> 56)) { n += 8; v <<= 8; }
    if (!(v >> 60)) { n += 4; v <<= 4; }
    if (!(v >> 62)) { n += 2; v <<= 2; }
    if (!(v >> 63)) n += 1;
    return n;
}

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (d == 0) { if (rem) *rem = n; return ~(uint64_t)0; }   /* RISC-V division-by-zero results */
    if (n < d) { if (rem) *rem = n; return 0; }
    if (!(n >> 32)) {                                          /* both fit 32 bits: one DIVU */
        uint32_t q = (uint32_t)n / (uint32_t)d;
        if (rem) *rem = (uint32_t)n - q * (uint32_t)d;
        return q;
    }
    if (!(d >> 32)) {                                          /* 32-bit divisor (every composer call) */
        uint32_t dd = (uint32_t)d, hi = (uint32_t)(n >> 32), r;
        uint32_t qh = hi / dd;
        uint32_t ql = divlu(hi - qh * dd, (uint32_t)n, dd, &r);
        if (rem) *rem = r;
        return (uint64_t)qh << 32 | ql;
    }
    unsigned sh = clz64(d) - clz64(n);                        /* align d under n's top bit */
    uint64_t q = 0;
    d <<= sh;
    for (unsigned i = 0; i <= sh; i++) {
        q <<= 1;
        if (n >= d) { n -= d; q |= 1u; }
        d >>= 1;
    }
    if (rem) *rem = n;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }

int64_t __divdi3(int64_t n, int64_t d)
{
    if (d == 0) return -1;
    int neg = (n < 0) != (d < 0);
    uint64_t q = udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    if (d == 0) return n;
    uint64_t r;
    udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
