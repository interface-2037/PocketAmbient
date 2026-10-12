#include "fxp.h"
#include "gen_ctables.h"

int32_t fx_sin_q15(uint32_t phase)
{
    uint32_t i = phase >> 24, f = (phase >> 8) & 0xFFFF;
    int32_t a = CT_SINE256[i], b = CT_SINE256[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)f) >> 16);
}

uint32_t fx_exp2_q16(int32_t x)
{
    int32_t i = x >> 16;
    uint32_t f = (uint32_t)x & 0xFFFF;
    if (i >= 15) return 0xFFFFFFFFu;
    if (i < -30) return 0;
    uint32_t idx = f >> 10, fr = f & 1023;
    uint32_t a = CT_EXP2_Q30[idx], b = CT_EXP2_Q30[idx + 1];
    uint64_t v = a + (((uint64_t)(b - a) * fr) >> 10);          /* Q30, in [1,2) */
    return (uint32_t)(i >= 14 ? v << (i - 14) : v >> (14 - i));
}

int32_t fx_log2_q16(uint32_t x)
{
    if (x == 0) x = 1;
    int msb = 31 - __builtin_clz(x);
    uint32_t m = msb >= 30 ? x >> (msb - 30) : x << (30 - msb);  /* Q30 [1,2) */
    uint32_t t = m - (1u << 30);
    uint32_t idx = t >> 24, fr = (t >> 14) & 1023;
    uint32_t a = CT_LOG2_Q16[idx], b = CT_LOG2_Q16[idx + 1];
    return (int32_t)((uint32_t)msb << 16) + (int32_t)(a + (((b - a) * fr) >> 10));
}

uint32_t fx_neg_ln_q16(uint32_t u)
{
    int32_t l2 = (32 << 16) - fx_log2_q16(u);                   /* -log2(u/2^32) */
    return (uint32_t)(((int64_t)l2 * 45426) >> 16);             /* * ln 2 */
}

uint32_t pitch_inc(int32_t cents)
{
    if (cents < 0) cents = 0;
    if (cents > 12799) cents = 12799;
    uint32_t n = (uint32_t)cents / 100, c = (uint32_t)cents % 100;
    return (uint32_t)(((uint64_t)CT_NOTE_INC[n] * CT_CENTS_Q30[c]) >> 30);
}

uint32_t pitch_hz(int32_t cents)
{
    return (uint32_t)(((uint64_t)pitch_inc(cents) * 48000u) >> 32);
}

uint32_t inc_mul_ratio(uint32_t inc, uint32_t num, uint32_t den)
{
    uint64_t r = (uint64_t)inc * num / den;
    return r > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)r;
}

uint16_t svf_coef_hz(uint32_t hz)
{
    if (hz < 10) hz = 10;
    if (hz > 14000) hz = 14000;
    uint32_t ph = (uint32_t)(((uint64_t)hz << 32) / 192000u);   /* pi*fc/96000 as a phase */
    int32_t s = fx_sin_q15(ph);
    int32_t f = s * 4;                                          /* 2*sin, Q0.16 */
    return (uint16_t)clampi(f, 1, 52429);
}

uint16_t env_rate_tau(uint32_t tau)
{
    if (tau < 16) tau = 16;
    uint64_t coef = ((1ull << 32) + tau / 2) / tau;
    if (coef < 4096) return 0;
    unsigned e = 0;
    while ((coef >> e) >= 8192) e++;
    if (e > 15) return 0xFFFF;
    return (uint16_t)((e << 12) | (uint32_t)((coef >> e) - 4096));
}

/* attack rises towards 1.25 and stops at 1.0: t = tau * ln(5) */
uint16_t env_attack_ms(uint32_t ms) { return env_rate_tau((uint32_t)(((uint64_t)ms * 48000 + 804) / 1609)); }
/* "decay time" = 4 tau */
uint16_t env_decay_ms(uint32_t ms) { return env_rate_tau(ms * 12); }
/* -60 dB = tau * ln(1000) */
uint16_t env_release_ms(uint32_t ms) { return env_rate_tau((uint32_t)(((uint64_t)ms * 48000 + 3454) / 6908)); }

uint16_t rev_line_gain(uint32_t len, uint32_t t60_ms)
{
    if (t60_ms < 100) t60_ms = 100;
    /* g = 10^(-3 len / (T60 fs)) = 2^(-9.966 len / (T60 fs)) */
    int64_t num = (int64_t)len * 653129;                        /* 9.966 * 65536 */
    int32_t x = (int32_t)-(num / ((int64_t)t60_ms * 48));
    uint32_t g = fx_exp2_q16(x);
    return (uint16_t)(g > 65535 ? 65535 : g);
}
