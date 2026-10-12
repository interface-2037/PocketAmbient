#include "prng.h"
#include "fxp.h"

static inline uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

static uint32_t splitmix32(uint32_t *x)
{
    uint32_t z = (*x += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

void prng_seed(prng_t *p, uint32_t seed, uint32_t stream)
{
    uint32_t x = seed ^ (stream * 0x632BE5ABu);
    for (int i = 0; i < 4; i++) p->s[i] = splitmix32(&x);
    if (!(p->s[0] | p->s[1] | p->s[2] | p->s[3])) p->s[0] = 1;
}

uint32_t prng_next(prng_t *p)
{
    uint32_t *s = p->s;
    uint32_t r = rotl(s[1] * 5, 7) * 9;
    uint32_t t = s[1] << 9;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 11);
    return r;
}

uint32_t prng_below(prng_t *p, uint32_t n)
{
    return (uint32_t)(((uint64_t)prng_next(p) * n) >> 32);
}

int32_t prng_range(prng_t *p, int32_t lo, int32_t hi)
{
    if (hi <= lo) return lo;
    return lo + (int32_t)prng_below(p, (uint32_t)(hi - lo + 1));
}

int prng_chance(prng_t *p, uint32_t p_q16) { return (prng_next(p) >> 16) < p_q16; }

uint32_t prng_exp(prng_t *p, uint32_t mean)
{
    uint32_t u = prng_next(p) | 1u;
    uint64_t v = ((uint64_t)mean * fx_neg_ln_q16(u)) >> 16;
    uint64_t cap = (uint64_t)mean * 8;
    return (uint32_t)(v > cap ? cap : v);
}

int prng_weighted(prng_t *p, const uint32_t *w, int n)
{
    uint64_t tot = 0;
    for (int i = 0; i < n; i++) tot += w[i];
    if (!tot) return -1;
    uint64_t r = ((uint64_t)prng_next(p) * tot) >> 32;
    for (int i = 0; i < n; i++) {
        if (r < w[i]) return i;
        r -= w[i];
    }
    return n - 1;
}
