/*
 * synth_model.h - Bit-exact fixed-point model of the synth engine, FX and
 * master chain. The RTL must reproduce sm_process() sample-for-sample.
 * See docs/FIXED_POINT.md for formats.
 */
#ifndef SYNTH_MODEL_H
#define SYNTH_MODEL_H

#include <stdint.h>
#include "../fw/hal/regs.h"

typedef struct {
    uint32_t reg[32];        /* committed register file */
    uint8_t  trig_pending;
    uint8_t  prev_gate;
    uint8_t  env_state;
    int32_t  env_level;      /* Q1.30 */
    int32_t  mod_env;        /* Q1.30 */
    uint32_t phase[2];
    int32_t  svf_low, svf_band;
    uint32_t noise;          /* xorshift32 state */
    int32_t  pink[3];
    int32_t  noise_lp, noise_lp2;   /* source high-pass states */
    int32_t  dust_env;       /* Q1.23 */
    int32_t  cutoff_s, amp_s, pan_s, fmi_s;  /* slewed copies, value << 8 */
    uint32_t inc_s[2];       /* glided oscillator increments */
    uint32_t vib_ph;         /* vibrato LFO phase */
    int32_t  vib_d;          /* current vibrato depth, U0.16 << 14 */
    /* pluck */
    uint16_t ks_w;
    uint16_t ks_excite;
    int32_t  ks_prev;
    int32_t  ks_lp;
    /* host-only cache (not architectural state): pan gains for pan_cache */
    uint32_t pan_cache;
    int32_t  gain_l, gain_r;
} sm_voice_t;

typedef struct {
    uint32_t pos;            /* write position inside region */
    uint32_t base, size;     /* region inside SRAM */
} sm_line_t;

typedef struct {
    sm_voice_t v[AP_NUM_VOICES];
    int16_t    ks_buf[AP_KS_SLOTS][AP_KS_BUF];
    int16_t    sram[AP_SRAM_WORDS];
    uint32_t   fx[FX_NUM_REGS];

    sm_line_t  pre, ap[4], rev[AP_REV_LINES], echo_l, echo_r;
    int32_t    rev_damp_s[AP_REV_LINES];
    uint32_t   mod_phase[2];
    int32_t    echo_lp_l, echo_lp_r;
    uint32_t   echo_mod_ph;
    int64_t    hpf_lp[2][2];       /* [stage][ch] value << 16 */
    int32_t    master_gain;        /* Q1.23 */
    uint32_t   dither;             /* xorshift32 */
    uint64_t   sample;             /* samples processed */
    uint32_t   sram_reads, sram_writes;  /* per-sample access count (budget check) */
    uint32_t   dbg_mute_algo;            /* host debug only: bit per ALGO_* / (8 + NOISE_*) */
    /* host-only verification hooks (RTL golden vectors) */
    void     (*wlog)(void *ctx, int fx, unsigned idx, uint32_t val);
    void      *wlog_ctx;
    int32_t    last_bus[4];              /* voice bus of the last sample: l, r, rev, echo */
} sm_t;

void     sm_init(sm_t *s);
void     sm_vwrite(sm_t *s, unsigned v, unsigned reg, uint32_t val);
uint32_t sm_vread(const sm_t *s, unsigned v, unsigned reg);
void     sm_fxwrite(sm_t *s, unsigned reg, uint32_t val);
uint32_t sm_fxread(const sm_t *s, unsigned reg);
void     sm_process(sm_t *s, int16_t *out_l, int16_t *out_r);

/* exposed for unit tests */
int32_t  sm_sin_q23(uint32_t phase);
uint32_t sm_rate_coef(uint32_t code);
int32_t  sm_limit(int32_t x);

#endif
