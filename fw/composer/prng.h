/* prng.h - xoshiro128** with independent per-subsystem streams */
#ifndef AP_PRNG_H
#define AP_PRNG_H

#include <stdint.h>

typedef struct { uint32_t s[4]; } prng_t;

enum { ST_HARMONY = 1, ST_RHYTHM, ST_TIMBRE, ST_WEATHER, ST_FORM, ST_META };

void     prng_seed(prng_t *p, uint32_t seed, uint32_t stream);
uint32_t prng_next(prng_t *p);
uint32_t prng_below(prng_t *p, uint32_t n);                  /* [0, n) */
int32_t  prng_range(prng_t *p, int32_t lo, int32_t hi);      /* [lo, hi] */
int      prng_chance(prng_t *p, uint32_t p_q16);             /* probability p/65536 */
uint32_t prng_exp(prng_t *p, uint32_t mean);                 /* exponential, capped at 8x mean */
int      prng_weighted(prng_t *p, const uint32_t *w, int n); /* index by weight, -1 if all 0 */

#endif
