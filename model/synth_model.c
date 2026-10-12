/*
 * synth_model.c - Bit-exact fixed-point model of the synth engine.
 *
 * Rules that keep this RTL-mappable (see docs/FIXED_POINT.md):
 *  - integers only; every product is computed at 64-bit and shifted
 *  - '>>' on signed values is an arithmetic shift (floor), as in SystemVerilog '>>>'
 *  - every bus summation saturates, nothing wraps except phase accumulators
 *  - idle voices are frozen (no state update), exactly as the RTL clock-gates them
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "synth_model.h"
#include "gen_tables.h"

#define ONE30        (1 << 30)
#define ATK_TARGET   1342177280   /* 1.25 in Q1.30: attack overshoot target */
#define ENV_FLOOR    64           /* ~-84 dB: release snaps to IDLE below this */
#define SLEW_SHIFT   10           /* parameter smoothing, tau ~ 21 ms */
#define Q23_MAX      ((1 << 23) - 1)
#define SVF_LIM      (1 << 27)
#define SVF_F_MAX    52429        /* f <= 0.8 keeps the Chamberlin SVF stable for q <= 2 */
#define SVF_Q_MIN    1024
#define SVF_Q_MAX    32767
#define HPF_K        14055215     /* 2*pi*25/48000 * 2^32 */
#define AP_G         20480        /* diffuser allpass gain 0.625, Q1.15 */
#define MOD_INC0     27739        /* 0.31 Hz */
#define MOD_INC1     38475        /* 0.43 Hz */

static inline int32_t sat(int64_t x, int32_t lim)
{
    if (x > lim) return lim;
    if (x < -(int64_t)lim) return -lim;
    return (int32_t)x;
}
static inline int32_t sat32(int64_t x) { return sat(x, 0x7FFFFFFF); }

static inline uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

int32_t sm_sin_q23(uint32_t ph)
{
    uint32_t q = ph >> 30;
    uint32_t p = (ph >> 10) & 0xFFFFF;          /* 12-bit index + 8-bit fraction */
    if (q & 1) p = 0x100000 - p;
    uint32_t i = p >> 8, f = p & 0xFF;
    int32_t a = TBL_SINE_Q[i], b = TBL_SINE_Q[i + 1];
    int32_t v = (a << 8) + (b - a) * (int32_t)f;
    return (q & 2) ? -v : v;
}

static inline int32_t wt_read(const int16_t *t, uint32_t ph)
{
    uint32_t i = ph >> 22, f = (ph >> 14) & 0xFF;
    int32_t a = t[i], b = t[(i + 1) & (WT_LEN - 1)];
    return (a << 8) + (b - a) * (int32_t)f;
}

static inline unsigned mip_level(uint32_t inc)
{
    if (inc < (1u << 20)) return 0;
    unsigned msb = 31 - (unsigned)__builtin_clz(inc);
    unsigned l = msb - 20;
    return l > WT_LEVELS - 1 ? WT_LEVELS - 1 : l;
}

uint32_t sm_rate_coef(uint32_t code)
{
    code &= 0xFFFF;
    return (0x1000u | (code & 0xFFF)) << (code >> 12);
}

int32_t sm_limit(int32_t x)
{
    int32_t a = x < 0 ? -x : x;
    if (a <= LIM_KNEE) return x;
    uint32_t e = (uint32_t)(a - LIM_KNEE);
    uint32_t i = e >> LIM_SHIFT;
    int32_t y;
    if (i >= LIM_ENTRIES) {
        y = TBL_LIM[LIM_ENTRIES];
    } else {
        int32_t f = (int32_t)((e >> (LIM_SHIFT - 8)) & 0xFF);
        y = TBL_LIM[i] + (int32_t)(((int64_t)(TBL_LIM[i + 1] - TBL_LIM[i]) * f) >> 8);
    }
    return x < 0 ? -y : y;
}

/* ------------------------------------------------------------------ SRAM */

static void line_init(sm_line_t *l, uint32_t *base, uint32_t size)
{
    l->pos = 0; l->base = *base; l->size = size;
    *base += size;
}

/* read the sample written d samples ago, 1 <= d <= size. Returns Q1.23 */
static inline int32_t line_read(sm_t *s, const sm_line_t *l, uint32_t d)
{
    uint32_t idx = l->pos + l->size - d;
    if (idx >= l->size) idx -= l->size;
    s->sram_reads++;
    return (int32_t)s->sram[l->base + idx] * 256;
}

static inline int16_t q23_to_16(int32_t x)
{
    return (int16_t)sat(((int64_t)x + 128) >> 8, 32767);
}

static inline void line_write(sm_t *s, sm_line_t *l, int32_t x23)
{
    s->sram[l->base + l->pos] = q23_to_16(x23);
    s->sram_writes++;
    if (++l->pos == l->size) l->pos = 0;
}

/* ------------------------------------------------------------------ init */

void sm_init(sm_t *s)
{
    memset(s, 0, sizeof *s);
    for (unsigned v = 0; v < AP_NUM_VOICES; v++) {
        s->v[v].noise = 0x9E3779B9u * (v + 1) | 1u;
        s->v[v].pan_cache = 0xFFFFFFFFu;
    }
    s->dither = 0x2545F491u;

    uint32_t base = 0;
    line_init(&s->pre, &base, AP_REV_PRE_MAX + 1);
    for (int i = 0; i < 4; i++) line_init(&s->ap[i], &base, AP_AP_LEN[i]);
    for (int i = 0; i < AP_REV_LINES; i++)
        line_init(&s->rev[i], &base,
                  AP_REV_LEN[i] + ((i == 0 || i == 5) ? AP_REV_MOD_DEPTH + 2 : 0));
    line_init(&s->echo_l, &base, AP_ECHO_MAX + 1);
    line_init(&s->echo_r, &base, AP_ECHO_MAX + 1);

    s->fx[FX_DRY] = 32768;
    s->fx[FX_TRIM] = 16384;
    s->fx[FX_REV_DAMP] = 65535;
    s->fx[FX_REV_PREDELAY] = 1;
    s->fx[FX_ECHO_LEN_L] = s->fx[FX_ECHO_LEN_R] = 1;
    s->fx[FX_ECHO_DAMP] = 65535;
}

void sm_vwrite(sm_t *s, unsigned v, unsigned reg, uint32_t val)
{
    if (v >= AP_NUM_VOICES || reg >= 32 || reg == VR_STATUS) return;
    if (s->wlog) s->wlog(s->wlog_ctx, 0, v * 32 + reg, val);
    if (reg == VR_GATE) {
        if (val & GATE_TRIG) s->v[v].trig_pending = 1;
        val &= GATE_ON;
    }
    s->v[v].reg[reg] = val;
}

uint32_t sm_vread(const sm_t *s, unsigned v, unsigned reg)
{
    if (v >= AP_NUM_VOICES || reg >= 32) return 0;
    const sm_voice_t *p = &s->v[v];
    if (reg == VR_STATUS) {
        uint32_t lvl = (uint32_t)p->env_level >> 14;
        if (lvl > 0xFFFF) lvl = 0xFFFF;
        return p->env_state | (lvl << 16);
    }
    return p->reg[reg];
}

void sm_fxwrite(sm_t *s, unsigned reg, uint32_t val)
{
    if (reg < FX_NUM_REGS && reg != FX_STATUS_GAIN) {
        if (s->wlog) s->wlog(s->wlog_ctx, 1, reg, val);
        s->fx[reg] = val;
    }
}

uint32_t sm_fxread(const sm_t *s, unsigned reg)
{
    if (reg == FX_STATUS_GAIN) return (uint32_t)s->master_gain;
    return reg < FX_NUM_REGS ? s->fx[reg] : 0;
}

/* ----------------------------------------------------------------- voice */

typedef struct { int32_t l, r, rev, echo; } bus_t;

static void voice_process(sm_t *s, unsigned vi, bus_t *bus)
{
    sm_voice_t *v = &s->v[vi];
    const uint32_t *R = v->reg;
    uint32_t gate = R[VR_GATE] & 1;

    if (v->env_state == ENV_IDLE && !gate && !v->trig_pending) {
        v->prev_gate = 0;
        return;                                    /* frozen */
    }

    /* --- gate / trigger handling --- */
    if (v->trig_pending || (gate && !v->prev_gate)) {
        if (v->env_state == ENV_IDLE) {
            v->svf_low = v->svf_band = 0;
            v->pink[0] = v->pink[1] = v->pink[2] = 0;
            v->noise_lp = v->noise_lp2 = 0;
            v->dust_env = 0;
            if (R[VR_ALGO] == ALGO_PAD) {
                /* pseudo-random start phase: a chord of detuned saws must not start
                 * phase-coherent (big summed peak). Deterministic via the voice PRNG. */
                v->phase[0] = xorshift32(&v->noise);
                v->phase[1] = xorshift32(&v->noise);
            } else {
                v->phase[0] = v->phase[1] = 0;
            }
            v->ks_prev = v->ks_lp = 0;
            v->cutoff_s = (int32_t)(R[VR_CUTOFF] & 0xFFFF) << 8;
            v->amp_s    = (int32_t)(R[VR_AMP] & 0xFFFF) << 8;
            v->pan_s    = (int32_t)(R[VR_PAN] & 0xFFFF) << 8;
            v->fmi_s    = (int32_t)(R[VR_FM_INDEX] & 0xFFFF) << 8;
            v->inc_s[0] = R[VR_PINC0];             /* no glide from a stale pitch */
            v->inc_s[1] = R[VR_PINC1];
            v->vib_ph = 0;
        }
        v->vib_d = 0;                              /* vibrato depth ramps in per note */
        v->env_state = ENV_ATTACK;
        v->mod_env = ONE30;
        if (R[VR_ALGO] == ALGO_KS) {
            uint32_t len = R[VR_KS_LEN] >> 16;
            v->ks_excite = (uint16_t)(len > AP_KS_BUF ? AP_KS_BUF : len);
        }
        v->trig_pending = 0;
    }
    v->prev_gate = (uint8_t)gate;
    if (!gate && (v->env_state == ENV_ATTACK || v->env_state == ENV_DECAY))
        v->env_state = ENV_RELEASE;

    /* --- parameter slews --- */
    if (R[VR_CUT_SLEW])
        v->cutoff_s += (int32_t)(((int64_t)(((int32_t)(R[VR_CUTOFF] & 0xFFFF) << 8) - v->cutoff_s) *
                                  sm_rate_coef(R[VR_CUT_SLEW])) >> 32);
    else
        v->cutoff_s += (((int32_t)(R[VR_CUTOFF] & 0xFFFF) << 8) - v->cutoff_s) >> SLEW_SHIFT;
    for (int o = 0; o < 2; o++) {                  /* glide (portamento) */
        uint32_t tgt = R[VR_PINC0 + o];
        if (!R[VR_GLIDE]) v->inc_s[o] = tgt;
        else v->inc_s[o] += (uint32_t)((((int64_t)tgt - (int64_t)v->inc_s[o]) *
                                        sm_rate_coef(R[VR_GLIDE])) >> 32);
    }
    {                                              /* vibrato depth ramp-in */
        int32_t tgt = (int32_t)(R[VR_VIB_DEPTH] & 0xFFFF) << 14;
        if (!R[VR_VIB_DELAY]) v->vib_d = tgt;
        else v->vib_d += (int32_t)((((int64_t)tgt - v->vib_d) * sm_rate_coef(R[VR_VIB_DELAY])) >> 32);
    }
    v->vib_ph += R[VR_VIB_RATE];
    if (R[VR_SLEW])                                /* slow swells share the cutoff slew rate */
        v->amp_s += (int32_t)(((int64_t)(((int32_t)(R[VR_AMP] & 0xFFFF) << 8) - v->amp_s) *
                               sm_rate_coef(R[VR_SLEW])) >> 32);
    else
        v->amp_s += (((int32_t)(R[VR_AMP] & 0xFFFF) << 8) - v->amp_s) >> SLEW_SHIFT;
    v->pan_s    += (((int32_t)(R[VR_PAN] & 0xFFFF) << 8) - v->pan_s) >> SLEW_SHIFT;
    v->fmi_s    += (((int32_t)(R[VR_FM_INDEX] & 0xFFFF) << 8) - v->fmi_s) >> SLEW_SHIFT;

    /* --- envelopes --- */
    switch (v->env_state) {
    case ENV_ATTACK:
        v->env_level += (int32_t)((((int64_t)(ATK_TARGET - v->env_level)) *
                                   sm_rate_coef(R[VR_ATTACK])) >> 32) + 1;
        if (v->env_level >= ONE30) { v->env_level = ONE30; v->env_state = ENV_DECAY; }
        break;
    case ENV_DECAY: {
        int32_t tgt = (int32_t)(R[VR_SUSTAIN] > 32768 ? 32768 : R[VR_SUSTAIN]) << 15;
        v->env_level += (int32_t)((((int64_t)(tgt - v->env_level)) *
                                   sm_rate_coef(R[VR_DECAY])) >> 32);
        break;
    }
    case ENV_RELEASE:
        v->env_level += (int32_t)((((int64_t)(-v->env_level)) *
                                   sm_rate_coef(R[VR_RELEASE])) >> 32);
        if (v->env_level < ENV_FLOOR) { v->env_level = 0; v->env_state = ENV_IDLE; }
        break;
    default: break;
    }
    v->mod_env += (int32_t)((((int64_t)(-v->mod_env)) * sm_rate_coef(R[VR_MOD_DECAY])) >> 32);

    /* --- oscillator / source --- */
    int32_t x = 0;
    /* pitch envelope: osc 0 starts PENV higher and falls with the mod envelope (kick drums) */
    uint32_t inc0 = v->inc_s[0] + (uint32_t)(((uint64_t)R[VR_PENV] * (uint32_t)v->mod_env) >> 30);
    uint32_t inc1 = v->inc_s[1];
    if (v->vib_d) {                                /* vibrato: inc *= 1 + depth * sin */
        int32_t m = (int32_t)(((int64_t)sm_sin_q23(v->vib_ph) * (v->vib_d >> 14)) >> 23);   /* U0.16 */
        inc0 += (uint32_t)(((int64_t)inc0 * m) >> 16);
        inc1 += (uint32_t)(((int64_t)inc1 * m) >> 16);
    }
    switch (R[VR_ALGO]) {
    case ALGO_PAD: {
        int32_t wave = (int32_t)(R[VR_WAVE] > 32768 ? 32768 : R[VR_WAVE]);
        unsigned l0 = mip_level(inc0), l1 = mip_level(inc1);
        int32_t s0 = wt_read(TBL_SAW[l0], v->phase[0]);
        int32_t t0 = wt_read(TBL_TRI[l0], v->phase[0]);
        int32_t s1 = wt_read(TBL_SAW[l1], v->phase[1]);
        int32_t t1 = wt_read(TBL_TRI[l1], v->phase[1]);
        int32_t x0 = s0 + (int32_t)(((int64_t)(t0 - s0) * wave) >> 15);
        int32_t x1 = s1 + (int32_t)(((int64_t)(t1 - s1) * wave) >> 15);
        x = (x0 + x1) >> 1;
        break;
    }
    case ALGO_FM: {
        int32_t m = sm_sin_q23(v->phase[1]);
        int32_t sus = (int32_t)(R[VR_FM_SUS] & 0xFFFF);
        int32_t pk = v->fmi_s >> 8;
        int32_t d = pk - sus; if (d < 0) d = 0;
        int32_t idx = sus + (int32_t)(((int64_t)d * v->mod_env) >> 30);
        uint32_t off = (uint32_t)(((int64_t)m * idx) >> 3);
        x = sm_sin_q23(v->phase[0] + off);
        break;
    }
    case ALGO_NOISE: {
        uint32_t n = xorshift32(&v->noise);
        int32_t w = (int32_t)n >> 8;
        uint32_t mode = R[VR_NOISE];
        /* 2 x one-pole high-pass ~120 Hz (shift only): noise beds never carry sub-bass */
        v->noise_lp += (w - v->noise_lp) >> 6;
        w -= v->noise_lp;
        v->noise_lp2 += (w - v->noise_lp2) >> 6;
        w -= v->noise_lp2;
        if (mode == NOISE_PINK || mode == NOISE_PDUST) {
            /* Kellet "economy" pink filter, Q16 coefficients. The first pole is
             * moved from 0.99765 (~18 Hz) to 0.9895 (~80 Hz): pink above 80 Hz,
             * flat below, so no sub-bass build-up. */
            v->pink[0] = (int32_t)(((int64_t)v->pink[0] * 64848 + (int64_t)w * 6491) >> 16);
            v->pink[1] = (int32_t)(((int64_t)v->pink[1] * 63111 + (int64_t)w * 19433) >> 16);
            v->pink[2] = (int32_t)(((int64_t)v->pink[2] * 37356 + (int64_t)w * 68989) >> 16);
            w = sat(((int64_t)v->pink[0] + v->pink[1] + v->pink[2] +
                     (((int64_t)w * 12111) >> 16)) >> 2, Q23_MAX);
        }
        switch (mode) {
        case NOISE_PINK:
            x = w;
            break;
        case NOISE_DUST:
        case NOISE_PDUST:
            if ((n & 0xFFFF) < (R[VR_DUST_RATE] & 0xFFFF)) v->dust_env = 1 << 23;
            else v->dust_env -= v->dust_env >> 7;
            x = (int32_t)(((int64_t)w * v->dust_env) >> 23);
            break;
        default:
            x = w;
            break;
        }
        break;
    }
    case ALGO_KS:
        if (vi >= AP_KS_FIRST) {
            int16_t *buf = s->ks_buf[vi - AP_KS_FIRST];
            uint32_t len = R[VR_KS_LEN];
            if (len < (2u << 16)) len = 2u << 16;
            if (len > ((AP_KS_BUF - 8u) << 16)) len = (AP_KS_BUF - 8u) << 16;
            uint32_t di = len >> 16, fr = (len >> 8) & 0xFF;
            int32_t a = buf[(v->ks_w - di) & (AP_KS_BUF - 1)];
            int32_t b = buf[(v->ks_w - di - 1) & (AP_KS_BUF - 1)];
            int32_t y = (a << 8) + (b - a) * (int32_t)fr;
            int32_t avg = (y + v->ks_prev) >> 1;
            v->ks_prev = y;
            int32_t fb = (int32_t)(((int64_t)avg * (R[VR_KS_DAMP] & 0xFFFF)) >> 15);
            uint32_t n = xorshift32(&v->noise);
            if (v->ks_excite) {
                v->ks_excite--;
                int32_t w = (int32_t)n >> 8;
                v->ks_lp += (int32_t)(((int64_t)(w - v->ks_lp) * (R[VR_KS_BRIGHT] & 0xFFFF)) >> 16);
                fb = v->ks_lp;
            }
            buf[v->ks_w] = q23_to_16(fb);
            v->ks_w = (uint16_t)((v->ks_w + 1) & (AP_KS_BUF - 1));
            x = y;
        }
        break;
    case ALGO_PULSE: {
        uint32_t off = (R[VR_WAVE] > 32768 ? 32768 : R[VR_WAVE]) << 17;   /* width, fraction of a cycle */
        const int16_t *t = TBL_SAW[mip_level(inc0)];
        x = (wt_read(t, v->phase[0]) - wt_read(t, v->phase[0] + off)) >> 1;
        break;
    }
    case ALGO_SUB: {
        int32_t wave = (int32_t)(R[VR_WAVE] > 32768 ? 32768 : R[VR_WAVE]);
        int32_t s0 = sm_sin_q23(v->phase[0]);
        int32_t s1 = sm_sin_q23(v->phase[1]);
        x = (s0 + (int32_t)(((int64_t)s1 * wave) >> 15)) >> 1;
        break;
    }
    default: break;
    }
    v->phase[0] += inc0;
    v->phase[1] += inc1;

    /* --- state-variable filter, 2x oversampled --- */
    if (R[VR_FMODE] != FMODE_BYPASS) {
        int32_t f = (v->cutoff_s >> 8) +
                    (int32_t)(((int64_t)(R[VR_FENV] & 0xFFFF) * v->env_level) >> 30);   /* filter env */
        if (f > SVF_F_MAX) f = SVF_F_MAX;
        int32_t q = (int32_t)(R[VR_RESO] & 0xFFFF);
        if (q < SVF_Q_MIN) q = SVF_Q_MIN;
        if (q > SVF_Q_MAX) q = SVF_Q_MAX;
        int32_t high = 0;
        for (int k = 0; k < 2; k++) {
            v->svf_low = sat((int64_t)v->svf_low + (((int64_t)f * v->svf_band) >> 16), SVF_LIM);
            high = sat((int64_t)x - v->svf_low - (((int64_t)q * v->svf_band) >> 14), SVF_LIM);
            v->svf_band = sat((int64_t)v->svf_band + (((int64_t)f * high) >> 16), SVF_LIM);
        }
        switch (R[VR_FMODE]) {
        case FMODE_BP: x = v->svf_band; break;
        case FMODE_HP: x = high; break;
        default:       x = v->svf_low; break;
        }
    }
    x = sat(x, Q23_MAX);

    if (s->dbg_mute_algo &&
        ((s->dbg_mute_algo >> R[VR_ALGO]) & 1 ||
         (R[VR_ALGO] == ALGO_NOISE && (s->dbg_mute_algo >> (8 + R[VR_NOISE])) & 1)))
        x = 0;

    /* --- amplitude, pan, sends --- */
    int32_t e = (int32_t)(((int64_t)x * v->env_level) >> 30);
    int32_t a = (int32_t)(((int64_t)e * (v->amp_s >> 8)) >> 15);
    if (getenv("SM_DBG_SAMPLE") && s->sample == strtoull(getenv("SM_DBG_SAMPLE"), NULL, 0))
        fprintf(stderr, "C v%02u algo %u x %d env %d amp_s %d cut_s %d a %d ph0 %08x inc_s0 %08x\n", vi,
                R[VR_ALGO], x, v->env_level, v->amp_s, v->cutoff_s, a, v->phase[0], v->inc_s[0]);
    uint32_t pan = (uint32_t)(v->pan_s >> 8);
    if (pan > 32768) pan = 32768;
    if (pan != v->pan_cache) {               /* host speed-up only; pure function of pan */
        v->pan_cache = pan;
        v->gain_r = sm_sin_q23(pan << 15);
        v->gain_l = sm_sin_q23((32768 - pan) << 15);
    }
    int32_t gr = v->gain_r, gl = v->gain_l;
    bus->l    = sat32((int64_t)bus->l    + (((int64_t)a * gl) >> 23));
    bus->r    = sat32((int64_t)bus->r    + (((int64_t)a * gr) >> 23));
    bus->rev  = sat32((int64_t)bus->rev  + (((int64_t)a * (R[VR_SEND_REV] & 0xFFFF)) >> 15));
    bus->echo = sat32((int64_t)bus->echo + (((int64_t)a * (R[VR_SEND_ECHO] & 0xFFFF)) >> 15));
}

/* ------------------------------------------------------------- FX/master */

static void echo_process(sm_t *s, int32_t send, int32_t *ol, int32_t *or_)
{
    uint32_t depth = s->fx[FX_ECHO_MOD_DEPTH] & 0xFFFF;              /* U8.8 samples */
    uint32_t lim = AP_ECHO_MAX - 2 * (depth >> 8) - 2;
    uint32_t dl = s->fx[FX_ECHO_LEN_L], dr = s->fx[FX_ECHO_LEN_R];
    if (dl < 1) dl = 1;
    if (dl > lim) dl = lim;
    if (dr < 1) dr = 1;
    if (dr > lim) dr = lim;
    /* tape wobble: delay = len + depth * (1 + sin), linearly interpolated */
    s->echo_mod_ph += s->fx[FX_ECHO_MOD_RATE];
    uint32_t off = (uint32_t)(((int64_t)sm_sin_q23(s->echo_mod_ph) * (int32_t)depth) >> 23) + depth;
    uint32_t di = off >> 8, fr = off & 0xFF;
    int32_t a = line_read(s, &s->echo_l, dl + di), b = line_read(s, &s->echo_l, dl + di + 1);
    int32_t rl = a + (int32_t)(((int64_t)(b - a) * (int32_t)fr) >> 8);
    a = line_read(s, &s->echo_r, dr + di);
    b = line_read(s, &s->echo_r, dr + di + 1);
    int32_t rr = a + (int32_t)(((int64_t)(b - a) * (int32_t)fr) >> 8);
    uint32_t damp = s->fx[FX_ECHO_DAMP] & 0xFFFF, fb = s->fx[FX_ECHO_FB] & 0xFFFF;
    s->echo_lp_l += (int32_t)(((int64_t)(rl - s->echo_lp_l) * damp) >> 16);
    s->echo_lp_r += (int32_t)(((int64_t)(rr - s->echo_lp_r) * damp) >> 16);
    line_write(s, &s->echo_l, sat((int64_t)send + (((int64_t)s->echo_lp_r * fb) >> 15), Q23_MAX));
    line_write(s, &s->echo_r, sat(((int64_t)s->echo_lp_l * fb) >> 15, Q23_MAX));
    *ol = rl;
    *or_ = rr;
}

static void reverb_process(sm_t *s, int32_t in, int32_t *ol, int32_t *or_)
{
    uint32_t pd = s->fx[FX_REV_PREDELAY];
    if (pd < 1) pd = 1;
    if (pd > AP_REV_PRE_MAX) pd = AP_REV_PRE_MAX;
    int32_t x = line_read(s, &s->pre, pd);
    line_write(s, &s->pre, sat(in, Q23_MAX));

    for (int i = 0; i < 4; i++) {
        int32_t b = line_read(s, &s->ap[i], AP_AP_LEN[i]);
        int32_t w = sat((int64_t)x + (((int64_t)AP_G * b) >> 15), Q23_MAX);
        x = sat((int64_t)b - (((int64_t)AP_G * w) >> 15), Q23_MAX);
        line_write(s, &s->ap[i], w);
    }

    uint32_t mod_off[2];
    for (int m = 0; m < 2; m++) {
        s->mod_phase[m] += m ? MOD_INC1 : MOD_INC0;
        uint32_t p = s->mod_phase[m];
        uint32_t tri = (p & 0x80000000u) ? ~p : p;      /* 0 .. 2^31-1 */
        mod_off[m] = tri >> 19;                         /* Q4.8 samples, 0..4095 */
    }

    uint32_t damp = s->fx[FX_REV_DAMP] & 0xFFFF;
    int32_t y[AP_REV_LINES];
    int64_t sum = 0;
    for (int i = 0; i < AP_REV_LINES; i++) {
        int32_t r;
        if (i == 0 || i == 5) {
            uint32_t off = mod_off[i == 5];
            uint32_t di = AP_REV_LEN[i] + (off >> 8), fr = off & 0xFF;
            int32_t a = line_read(s, &s->rev[i], di);
            int32_t b = line_read(s, &s->rev[i], di + 1);
            r = a + (int32_t)(((int64_t)(b - a) * (int32_t)fr) >> 8);
        } else {
            r = line_read(s, &s->rev[i], AP_REV_LEN[i]);
        }
        s->rev_damp_s[i] += (int32_t)(((int64_t)(r - s->rev_damp_s[i]) * damp) >> 16);
        y[i] = (int32_t)(((int64_t)s->rev_damp_s[i] * (s->fx[FX_REV_GAIN0 + i] & 0xFFFF)) >> 16);
        sum += y[i];
    }
    int32_t h = sat(sum >> 2, 0x7FFFFFFF);
    int32_t xin = x >> 1;
    for (int i = 0; i < AP_REV_LINES; i++) {
        /* input sign pattern + + - - ...: each output (even lines L, odd lines R) gets both
         * signs, so L and R are decorrelated by delay, never anti-phase (mono-safe) */
        int64_t f = (int64_t)y[i] - h + (((i >> 1) & 1) ? -xin : xin);
        line_write(s, &s->rev[i], sat(f, Q23_MAX));
    }
    *ol = sat(((int64_t)y[0] + y[2] + y[4] + y[6]) >> 1, 0x7FFFFFFF);
    *or_ = sat(((int64_t)y[1] + y[3] + y[5] + y[7]) >> 1, 0x7FFFFFFF);
}

static inline int32_t hpf(sm_t *s, int ch, int32_t x)
{
    for (int st = 0; st < 2; st++) {
        int32_t diff = sat((int64_t)x - (int32_t)(s->hpf_lp[st][ch] >> 16), 0x3FFFFFFF);
        s->hpf_lp[st][ch] += ((int64_t)diff * HPF_K) >> 16;
        x = diff;
    }
    return x;
}

void sm_process(sm_t *s, int16_t *out_l, int16_t *out_r)
{
    bus_t bus = {0, 0, 0, 0};
    s->sram_reads = s->sram_writes = 0;

    for (unsigned v = 0; v < AP_NUM_VOICES; v++)
        voice_process(s, v, &bus);
    s->last_bus[0] = bus.l; s->last_bus[1] = bus.r; s->last_bus[2] = bus.rev; s->last_bus[3] = bus.echo;

    int32_t el, er, rl, rr;
    echo_process(s, sat(bus.echo, Q23_MAX), &el, &er);
    int32_t echo_mono = (el >> 1) + (er >> 1);
    reverb_process(s, sat((int64_t)bus.rev +
                          (((int64_t)echo_mono * (s->fx[FX_ECHO_TO_REV] & 0xFFFF)) >> 15), Q23_MAX),
                   &rl, &rr);

    uint32_t dry = s->fx[FX_DRY] & 0xFFFF, rw = s->fx[FX_REV_WET] & 0xFFFF,
             ew = s->fx[FX_ECHO_WET] & 0xFFFF, trim = s->fx[FX_TRIM] & 0xFFFF;
    int32_t ch[2];
    ch[0] = sat32((((int64_t)bus.l * dry) >> 15) + (((int64_t)rl * rw) >> 15) + (((int64_t)el * ew) >> 15));
    ch[1] = sat32((((int64_t)bus.r * dry) >> 15) + (((int64_t)rr * rw) >> 15) + (((int64_t)er * ew) >> 15));

    /* fade gain: linear ramp, applied squared */
    int32_t tgt = (int32_t)(s->fx[FX_MASTER_TARGET] > (1u << 23) ? (1u << 23) : s->fx[FX_MASTER_TARGET]);
    int32_t step = (int32_t)(s->fx[FX_MASTER_STEP] & 0xFFFFF);
    if (s->master_gain < tgt) {
        s->master_gain += step;
        if (s->master_gain > tgt) s->master_gain = tgt;
    } else if (s->master_gain > tgt) {
        s->master_gain -= step;
        if (s->master_gain < tgt) s->master_gain = tgt;
    }
    int32_t gg = (int32_t)(((int64_t)s->master_gain * s->master_gain) >> 23);

    uint32_t r = xorshift32(&s->dither);
    int32_t d[2];
    d[0] = (int32_t)(r & 0xFF) + (int32_t)((r >> 8) & 0xFF) - 255;
    d[1] = (int32_t)((r >> 16) & 0xFF) + (int32_t)((r >> 24) & 0xFF) - 255;

    int16_t out[2];
    for (int c = 0; c < 2; c++) {
        int32_t x = hpf(s, c, ch[c]);
        x = sat(((int64_t)x * trim) >> 14, 0x3FFFFFFF);
        x = (int32_t)(((int64_t)x * gg) >> 23);
        x = sm_limit(x);
        if (s->master_gain == 0) {
            out[c] = 0;                          /* true digital silence */
        } else {
            out[c] = (int16_t)sat(((int64_t)x + d[c] + 128) >> 8, 32767);
        }
    }
    *out_l = out[0];
    *out_r = out[1];
    s->sample++;
}
