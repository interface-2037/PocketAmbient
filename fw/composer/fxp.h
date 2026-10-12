/* fxp.h - integer-only math helpers for the composer (no float anywhere) */
#ifndef AP_FXP_H
#define AP_FXP_H

#include <stdint.h>

#define Q15_ONE 32768

int32_t  fx_sin_q15(uint32_t phase);             /* full cycle = 2^32 */
uint32_t fx_exp2_q16(int32_t x_q16);             /* 2^x, Q16.16 in/out, saturating */
int32_t  fx_log2_q16(uint32_t x);                /* log2(x), x >= 1, Q16.16 */
uint32_t fx_neg_ln_q16(uint32_t u);              /* -ln(u / 2^32), Q16.16 */

uint32_t pitch_inc(int32_t cents);               /* MIDI note * 100 + cents -> phase inc */
uint32_t pitch_hz(int32_t cents);                /* integer Hz (rounded down) */
uint32_t inc_mul_ratio(uint32_t inc, uint32_t num, uint32_t den);

uint16_t svf_coef_hz(uint32_t hz);               /* -> VR_CUTOFF */
uint16_t env_rate_tau(uint32_t tau_samples);     /* -> rate code */
uint16_t env_attack_ms(uint32_t ms);             /* 0 -> 1.0 */
uint16_t env_decay_ms(uint32_t ms);              /* ~ -35 dB towards sustain */
uint16_t env_release_ms(uint32_t ms);            /* to -60 dB */
uint16_t rev_line_gain(uint32_t len_samples, uint32_t t60_ms);

static inline int32_t clampi(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}
/* a * b where b is Q15 (32768 = 1.0) */
static inline int32_t mul_q15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
/* scale x by (1 + w * depth), w and depth Q15 */
static inline int32_t mod_q15(int32_t x, int32_t w, int32_t depth)
{
    return x + mul_q15(x, mul_q15(w, depth));
}

#endif
