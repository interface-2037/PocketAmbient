/*
 * mode_mother.c - MOTHER: inside the Nostromo. No music, no score: machinery.
 *
 *  Tone set from the user's reference clip (see the tables below).
 *  - Machinery hum: seven sine partials at the clip's frequencies and levels,
 *    swelling slowly.
 *  - Coolant / pipe flow: two resonant noise beds whose resonances and levels
 *    wander slowly (seconds-long slews), with occasional gurgles (short bursts
 *    of bubbly resonant noise at random low pitches).
 *  - Relays: sharp ~1-2 ms double clicks (close, then a bounce 6-15 ms later),
 *    in small clusters.
 *  - Servos / motors: a pulse-wave whine that glides up, holds, and glides back
 *    down over 0.3-2 s.
 *  - Disk / tape drives: a spin-up whir (40 Hz up), bursts of rapid seek ticks, a spin-down.
 *  - Thermal ticks: sparse, quiet metallic pings (short inharmonic FM).
 *  - Data blips: the clip's 2.3-2.85 kHz square blips, 35-50 ms, ~90 ms apart,
 *    in 1-2 s runs, with readout ticks ~50 ms apart riding along.
 *  - Motion tracker: a very rare event (~1-4 h apart): pings speed up and rise
 *    as `distance` closes, ending in rapid beeping.
 *
 * Conductor: `system_activity` wanders slowly (weather + random walk + the
 * density setting) and sets how often everything happens. Tracker alerts are
 * more likely when the shared weather tension is high.
 */
#include "cint.h"

#define O (c->m.o)
#define O_BUDGET 22
#define TRIM     14345               /* loudness calibration (-21.5 LUFS) */
#define AQ(x)    ((int32_t)((x) * 32768))

/* Tone set measured from the reference clip (MOTHER.wav, 2026-10-10):
 * - hum partials (Hz, dB rel. 131.9 Hz): 131.9 0, 115.7 -4.9, 149.4 -5.7, 205.9 -7.3,
 *   87.5 -8.8, 57.9 -17.9, 49.8 -25.9; 263.8 Hz (2nd harmonic of 131.9) -18
 * - blips: 2.3-2.85 kHz, 40-100 ms long, ~90 ms apart, runs of 1-2 s, ticks ~50 ms apart
 * - little above 5 kHz, near-mono (L/R correlation 0.93) */
static const int32_t HUM_CENTS[7] = {4814, 4587, 5030, 5585, 4104, 3389, 3128};
static const int32_t HUM_AMP[7]   = {5400, 3070, 2800, 2330, 1960, 680, 280};
static const int32_t G_LO[M_GENS]   = {9862, 9862, 9900};          /* 2.44-2.8 kHz, the clip's tones */
static const int32_t G_HI[M_GENS]   = {10100, 10100, 10080};
static const int32_t G_W[M_GENS]    = {16384, 16384, 12000};
static const int32_t G_LMIN[M_GENS] = {35, 35, 40};
static const int32_t G_LMAX[M_GENS] = {50, 50, 70};
static const int32_t G_AMP[M_GENS]  = {8400, 6600, 5400};
static const int32_t G_PAN[M_GENS]  = {14000, 16384, 18800};
static const int32_t G_ON[M_GENS]   = {AQ(0.20), AQ(0.55), AQ(0.80)};

static int role(comp_t *c, int *slot) { return role_alloc(slot, c->gen, 0, AP_KS_FIRST - 1, O_BUDGET); }
static int owned(comp_t *c, int v) { return v >= 0 && g_vinfo[v].owner == c->gen; }
static uint16_t slew_code(uint32_t ms) { return ms ? env_rate_tau(ms * 16) : 0; }

static void trigger(comp_t *c, int v, int32_t cents, uint32_t max_ms)
{
    voice_off(v);
    voice_on(v, c->gen, cents, max_ms, c->now);
}
static void off(comp_t *c, int v) { if (owned(c, v)) voice_off(v); }

/* mean interval: `calm_ms` at zero activity down to `busy_ms` at full activity */
static uint32_t mean_ms(comp_t *c, int32_t calm_ms, int32_t busy_ms)
{
    return (uint32_t)(calm_ms + mul_q15(busy_ms - calm_ms, O.activity));
}
static int32_t wait_ms(comp_t *c, int32_t calm_ms, int32_t busy_ms, int32_t min_ms)
{
    return min_ms + (int32_t)prng_exp(&c->r_rhythm, mean_ms(c, calm_ms, busy_ms));
}

/* noise patch shared by the mechanical clicks (relays, seek ticks) */
static void click_patch(comp_t *c, int v, uint32_t hz)
{
    vparams_t p = {0};
    p.algo = ALGO_NOISE; p.noise = NOISE_PINK; p.fmode = FMODE_LP;
    p.cutoff = svf_coef_hz(hz); p.reso = 9000;
    p.attack = env_attack_ms(0); p.decay = env_decay_ms(1); p.sustain = 0;
    p.release = env_release_ms(2);
    p.amp = 4000; p.pan = c_pan_rand(c, 2250); p.send_rev = 4000; p.send_echo = 1000;
    voice_setup(v, &p);
}

/* -------------------------------------------------------------- coolant flow */

static void flow_start(comp_t *c, int i)
{
    if (role(c, &O.flow_v[i]) < 0) return;
    vparams_t p = {0};
    p.algo = ALGO_NOISE; p.noise = NOISE_PINK; p.fmode = FMODE_LP;    /* resonant LP: no HF spill */
    p.cutoff = svf_coef_hz(i ? 800 : 260); p.reso = 9000;
    p.attack = env_attack_ms(6000); p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms(6000);
    p.amp = i ? 6000 : 4500; p.pan = (uint32_t)(i ? 19500 : 13500);
    p.send_rev = 9000;
    p.slew = slew_code(3000);
    voice_setup(O.flow_v[i], &p);
    trigger(c, O.flow_v[i], 0, MIN(40));
    O.flow_left[i] = prng_range(&c->r_form, MIN(20), MIN(30));     /* re-articulate (crossfade) */
    O.flow_next[i] = prng_range(&c->r_rhythm, 1000, 4000);
}

static void flow_tick(comp_t *c, int i)
{
    if (--O.flow_left[i] <= 0) { flow_start(c, i); return; }
    if (--O.flow_next[i] > 0 || !owned(c, O.flow_v[i])) return;
    /* new resonance / level targets, reached over several seconds */
    unsigned v = (unsigned)O.flow_v[i];
    uint32_t slew = (uint32_t)prng_range(&c->r_rhythm, 2000, 6000);
    hal_vwrite(v, VR_SLEW, slew_code(slew));
    hal_vwrite(v, VR_CUTOFF, svf_coef_hz((uint32_t)(i ? prng_range(&c->r_timbre, 900, 2600)
                                                      : prng_range(&c->r_timbre, 200, 800))));
    hal_vwrite(v, VR_RESO, (uint32_t)prng_range(&c->r_timbre, 4500, 14000));
    int32_t amp = i ? prng_range(&c->r_timbre, 4500, 9000) : prng_range(&c->r_timbre, 2400, 5000);
    hal_vwrite(v, VR_AMP, (uint32_t)mod_q15(amp, O.activity - AQ(0.5), 9000));
    O.flow_next[i] = (int32_t)slew + prng_range(&c->r_rhythm, 0, 2500);
}

static void gurgle_tick(comp_t *c)
{
    if (--O.gurgle_next > 0) return;
    if (!O.gurgle_left) {                                 /* start a gurgle */
        if (role(c, &O.gurgle_v) < 0) { O.gurgle_next = 5000; return; }
        vparams_t p = {0};
        p.algo = ALGO_NOISE; p.noise = NOISE_PINK; p.fmode = FMODE_BP;
        p.cutoff = svf_coef_hz(300); p.reso = 2400;       /* high resonance: bubbly tone */
        p.attack = env_attack_ms(3); p.decay = env_decay_ms(70); p.sustain = 0;
        p.release = env_release_ms(60);
        p.amp = 5000; p.pan = c_pan_rand(c, 2500); p.send_rev = 8000;
        voice_setup(O.gurgle_v, &p);
        O.gurgle_left = (uint8_t)prng_range(&c->r_rhythm, 3, 8);
    }
    unsigned v = (unsigned)O.gurgle_v;
    hal_vwrite(v, VR_CUTOFF, svf_coef_hz((uint32_t)prng_range(&c->r_timbre, 180, 650)));
    hal_vwrite(v, VR_AMP, (uint32_t)prng_range(&c->r_timbre, 3000, 6000));
    trigger(c, O.gurgle_v, 0, SEC(5));
    O.gurgle_step = 20;                                   /* gate length */
    if (--O.gurgle_left) O.gurgle_next = prng_range(&c->r_rhythm, 60, 200);
    else O.gurgle_next = wait_ms(c, 45000, 15000, 4000);
}

/* ------------------------------------------------------------------ relays */

static void relay_tick(comp_t *c)
{
    if (O.relay_off > 0 && --O.relay_off == 0) { off(c, O.relay_v[0]); off(c, O.relay_v[1]); }
    if (O.relay_bounce > 0 && --O.relay_bounce == 0 && owned(c, O.relay_v[1])) {
        hal_vwrite((unsigned)O.relay_v[1], VR_AMP, (uint32_t)prng_range(&c->r_timbre, 2200, 4200));
        trigger(c, O.relay_v[1], 0, SEC(2));             /* the contact bounce */
        O.relay_off = 2;
    }
    if (--O.relay_next > 0) return;
    if (!O.relay_left) {
        O.relay_left = (uint8_t)prng_range(&c->r_rhythm, 1, 4);         /* a small cluster */
        for (int i = 0; i < 2; i++)
            if (!owned(c, O.relay_v[i]) && role(c, &O.relay_v[i]) >= 0)
                click_patch(c, O.relay_v[i], (uint32_t)prng_range(&c->r_timbre, 2800, 3800));
    }
    if (owned(c, O.relay_v[0])) {
        hal_vwrite((unsigned)O.relay_v[0], VR_AMP, (uint32_t)prng_range(&c->r_timbre, 4000, 7000));
        trigger(c, O.relay_v[0], 0, SEC(2));             /* contact closes */
        O.relay_off = 2;
        O.relay_bounce = prng_range(&c->r_rhythm, 6, 15);
    }
    if (--O.relay_left) O.relay_next = prng_range(&c->r_rhythm, 40, 220);
    else O.relay_next = wait_ms(c, 30000, 7000, 1500);
}

/* ------------------------------------------------------------------- servos */

static void servo_tick(comp_t *c)
{
    if (--O.servo_next > 0) return;
    unsigned v = (unsigned)O.servo_v;
    switch (O.servo_phase) {
    case 0: {                                             /* start: low pitch, then glide up */
        if (role(c, &O.servo_v) < 0) { O.servo_next = 3000; return; }
        v = (unsigned)O.servo_v;
        O.servo_lo = prng_range(&c->r_timbre, 4500, 6600);               /* ~140-380 Hz */
        O.servo_hi = O.servo_lo + prng_range(&c->r_timbre, 700, 1900);
        vparams_t p = {0};
        p.algo = ALGO_PULSE; p.wave = 11469;
        p.pinc0 = p.pinc1 = pitch_inc(O.servo_lo);
        p.fmode = FMODE_LP; p.cutoff = svf_coef_hz(1800); p.reso = 16000;
        p.attack = env_attack_ms(80); p.decay = env_decay_ms(1000); p.sustain = 32768;
        p.release = env_release_ms(120);
        p.amp = (uint32_t)prng_range(&c->r_timbre, 1800, 3000);
        p.pan = c_pan_rand(c, 3000); p.send_rev = 7000; p.send_echo = 1500;
        voice_setup((int)v, &p);
        trigger(c, O.servo_v, O.servo_lo, SEC(10));
        O.servo_t = prng_range(&c->r_rhythm, 300, 900);
        hal_vwrite(v, VR_GLIDE, slew_code((uint32_t)O.servo_t));
        hal_vwrite(v, VR_PINC0, pitch_inc(O.servo_hi));
        O.servo_phase = 1;
        O.servo_next = O.servo_t;
        return;
    }
    case 1:                                               /* hold at speed */
        O.servo_phase = 2;
        O.servo_next = prng_range(&c->r_rhythm, 200, 1200);
        return;
    case 2:                                               /* glide back down */
        if (owned(c, O.servo_v)) {
            O.servo_t = prng_range(&c->r_rhythm, 300, 900);
            hal_vwrite(v, VR_GLIDE, slew_code((uint32_t)O.servo_t));
            hal_vwrite(v, VR_PINC0, pitch_inc(O.servo_lo));
        }
        O.servo_phase = 3;
        O.servo_next = O.servo_t;
        return;
    default:
        off(c, O.servo_v);
        O.servo_phase = 0;
        O.servo_next = wait_ms(c, 40000, 10000, 2000);
        return;
    }
}

/* ------------------------------------------------------------------- drives */

static void drive_tick(comp_t *c)
{
    if (O.seek_off > 0 && --O.seek_off == 0) off(c, O.seek_v);
    if (O.drive_phase == 2 && --O.seek_next <= 0) {
        if (O.seek_left > 0) {                            /* one seek tick */
            if (owned(c, O.seek_v)) {
                hal_vwrite((unsigned)O.seek_v, VR_AMP, (uint32_t)prng_range(&c->r_timbre, 2600, 4600));
                trigger(c, O.seek_v, 0, SEC(2));
                O.seek_off = 1;
            }
            O.seek_left--;
            O.seek_next = prng_range(&c->r_rhythm, 8, 25);
        } else {                                          /* next seek burst */
            O.seek_left = (int16_t)prng_range(&c->r_rhythm, 5, 20);
            O.seek_next = prng_range(&c->r_rhythm, 100, 600);
        }
    }
    if (--O.drive_next > 0) return;
    unsigned v = (unsigned)O.drive_v;
    switch (O.drive_phase) {
    case 0: {                                             /* spin-up whir */
        if (role(c, &O.drive_v) < 0) { O.drive_next = 10000; return; }
        v = (unsigned)O.drive_v;
        vparams_t p = {0};
        p.algo = ALGO_PAD; p.wave = 9000;
        p.pinc0 = pitch_inc(2750); p.pinc1 = pitch_inc(2756);           /* ~40 Hz */
        p.fmode = FMODE_LP; p.cutoff = svf_coef_hz(700); p.reso = 20000;
        p.attack = env_attack_ms(600); p.decay = env_decay_ms(1000); p.sustain = 32768;
        p.release = env_release_ms(800);
        p.amp = 3500; p.pan = c_pan_rand(c, 2000); p.send_rev = 8000;
        voice_setup((int)v, &p);
        trigger(c, O.drive_v, 2750, SEC(40));
        int32_t spin = prng_range(&c->r_rhythm, 1200, 2500);
        int32_t top = prng_range(&c->r_timbre, 4300, 5100);              /* ~100-160 Hz */
        hal_vwrite(v, VR_GLIDE, slew_code((uint32_t)spin));
        hal_vwrite(v, VR_PINC0, pitch_inc(top));
        hal_vwrite(v, VR_PINC1, pitch_inc(top + 6));
        if (!owned(c, O.seek_v) && role(c, &O.seek_v) >= 0) click_patch(c, O.seek_v, 2400);
        O.drive_phase = 1;
        O.drive_next = spin;
        return;
    }
    case 1:                                               /* up to speed: seek */
        O.drive_phase = 2;
        O.seek_left = 0;
        O.seek_next = prng_range(&c->r_rhythm, 100, 400);
        O.drive_next = prng_range(&c->r_rhythm, 3000, 12000);
        return;
    case 2:                                               /* spin down */
        if (owned(c, O.drive_v)) {
            hal_vwrite(v, VR_GLIDE, slew_code(2000));
            hal_vwrite(v, VR_PINC0, pitch_inc(2750));
            hal_vwrite(v, VR_PINC1, pitch_inc(2756));
        }
        O.drive_phase = 3;
        O.drive_next = 1600;
        return;
    default:
        off(c, O.drive_v);
        O.drive_phase = 0;
        O.drive_next = wait_ms(c, 240000, 90000, 20000);
        return;
    }
}

/* ------------------------------------------------------------ thermal ticks */

static void therm_tick(comp_t *c)
{
    if (--O.therm_next > 0) return;
    int *slot = &O.therm_v[O.therm_i];
    O.therm_i ^= 1;
    if (role(c, slot) >= 0) {
        int32_t cents = prng_range(&c->r_timbre, 9500, 10900);          /* ~2-4.5 kHz */
        vparams_t q = {0};
        q.algo = ALGO_FM;
        q.pinc0 = pitch_inc(cents);
        q.pinc1 = prng_chance(&c->r_timbre, 32768) ? inc_mul_ratio(q.pinc0, 173, 100)
                                                   : inc_mul_ratio(q.pinc0, 289, 100);
        q.fm_index = 400; q.fm_sus = 40; q.mod_decay = env_decay_ms(60);
        q.attack = env_attack_ms(1); q.decay = env_decay_ms((uint32_t)prng_range(&c->r_timbre, 150, 350));
        q.sustain = 0; q.release = env_release_ms(200);
        q.fmode = FMODE_LP; q.cutoff = svf_coef_hz(4500); q.reso = 24000;
        q.amp = (uint32_t)prng_range(&c->r_timbre, 900, 2200);
        q.pan = c_pan_rand(c, 3750); q.send_rev = 7000; q.send_echo = 2500;
        voice_setup(*slot, &q);
        trigger(c, *slot, cents, SEC(3));
        voice_off(*slot);                                 /* gate 0: the decay does the rest */
    }
    O.therm_next = wait_ms(c, 9000, 5000, 600);
}

/* ------------------------------------------------------------- data blips */

static int governor_ok(comp_t *c) { return O.burst <= (O.recent >> 1) + (3u << 16); }

static void gen_patch(comp_t *c, int g)
{
    if (role(c, &O.gen_v[g]) < 0) return;
    vparams_t p = {0};
    p.algo = ALGO_PULSE;
    p.wave = (uint32_t)G_W[g];
    p.pinc0 = p.pinc1 = pitch_inc(O.gen_set[g][0]);
    p.fmode = FMODE_LP; p.cutoff = svf_coef_hz(6500); p.reso = 23000;     /* square; wavetables stop at 15 kHz */
    p.attack = env_attack_ms(1); p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms(12);
    p.amp = (uint32_t)G_AMP[g]; p.pan = (uint32_t)G_PAN[g];
    p.send_rev = 5000; p.send_echo = 3000;
    voice_setup(O.gen_v[g], &p);
}

static void rtick(comp_t *c)
{
    if (!owned(c, O.rtick_v)) {
        if (role(c, &O.rtick_v) < 0) return;
        click_patch(c, O.rtick_v, 3600);
    }
    hal_vwrite((unsigned)O.rtick_v, VR_AMP, (uint32_t)prng_range(&c->r_timbre, 4000, 7000));
    trigger(c, O.rtick_v, 0, SEC(2));
    O.rtick_off = 2;
}

static void gen_tick(comp_t *c, int g)
{
    if (O.gen_off[g] > 0 && --O.gen_off[g] == 0) off(c, O.gen_v[g]);
    if (--O.gen_next[g] > 0) return;
    int32_t a = O.activity;
    if (O.gen_left[g] > 0 && !governor_ok(c)) {
        O.gen_left[g] = 0;
        O.gen_next[g] = prng_range(&c->r_rhythm, 300, 900);
        return;
    }
    if (O.gen_left[g] > 0) {                             /* next blip of the run */
        if (!owned(c, O.gen_v[g])) gen_patch(c, g);
        if (!owned(c, O.gen_v[g])) { O.gen_left[g] = 0; O.gen_next[g] = 500; return; }
        int32_t cents = O.gen_loop[g] ? O.gen_pat[g][O.gen_pos[g]++ % O.gen_loop[g]]
                                      : O.gen_set[g][prng_below(&c->r_harm, O.gen_n[g])];
        hal_vwrite((unsigned)O.gen_v[g], VR_PINC0, pitch_inc(cents));
        trigger(c, O.gen_v[g], cents, SEC(5));
        O.recent += 65536;
        O.burst += 65536;
        /* clip timing: 35-50 ms blips ~90 ms apart; now and then one held ~0.4 s */
        int held = prng_chance(&c->r_form, 5243);
        int32_t len = held ? prng_range(&c->r_rhythm, 300, 500) : prng_range(&c->r_rhythm, G_LMIN[g], G_LMAX[g]);
        O.gen_off[g] = len;
        O.gen_next[g] = held ? len + prng_range(&c->r_rhythm, 60, 120) : prng_range(&c->r_rhythm, 75, 105);
        if (g == 0) { rtick(c); O.rtick_next = prng_range(&c->r_rhythm, 42, 55); }   /* ticks ~50 ms apart */
        O.gen_left[g]--;
        return;
    }
    if (a < G_ON[g]) {                                    /* idle: generator 0 runs a short sequence now and then */
        O.gen_left[g] = (int16_t)(g == 0 ? prng_range(&c->r_rhythm, 3, 6) : 0);
        O.gen_loop[g] = 0;
        O.gen_next[g] = g == 0 ? prng_range(&c->r_rhythm, 10000, 25000) : 1000;
        return;
    }
    int32_t busy = clampi((a - G_ON[g]) * 2, 0, 32767);
    O.gen_left[g] = (int16_t)clampi(8 + mul_q15(busy, 12) + prng_range(&c->r_rhythm, -2, 2), 4, 22);   /* 1-2 s runs */
    int32_t cap = 4 + (int32_t)(O.recent >> 16) / 4;
    if (O.gen_left[g] > cap) O.gen_left[g] = (int16_t)cap;
    O.gen_loop[g] = 0;
    if (prng_chance(&c->r_form, (uint32_t)(9000 + busy / 3))) {   /* "processing": a short loop */
        O.gen_loop[g] = (uint8_t)prng_range(&c->r_harm, 3, 4);
        O.gen_pos[g] = 0;
        for (int i = 0; i < O.gen_loop[g]; i++) O.gen_pat[g][i] = O.gen_set[g][prng_below(&c->r_harm, O.gen_n[g])];
    }
    O.gen_next[g] = prng_range(&c->r_rhythm, 2000, 7000 - mul_q15(busy, 3500));   /* runs every ~2-7 s */
}

static void rtick_tick(comp_t *c)
{
    if (O.rtick_off > 0 && --O.rtick_off == 0) off(c, O.rtick_v);
    if (O.rtick_next > 0 && --O.rtick_next == 0) rtick(c);
}

/* ------------------------------------------------------------------- hum */

static void hum_start(comp_t *c)
{
    for (int i = 0; i < 7; i++) {
        if (role(c, &O.hum_v[i]) < 0) continue;
        vparams_t p = {0};
        p.algo = ALGO_SUB;
        p.pinc0 = pitch_inc(HUM_CENTS[i]);
        p.pinc1 = pitch_inc(HUM_CENTS[i] + 1200);
        p.wave = i == 0 ? 4100 : 0;                       /* 263.8 Hz harmonic at -18 dB */
        p.fmode = FMODE_BYPASS; p.reso = 32767;
        p.attack = env_attack_ms(6000); p.decay = env_decay_ms(1000); p.sustain = 32768;
        p.release = env_release_ms(6000);
        p.amp = (uint32_t)(HUM_AMP[i] / 3);
        p.pan = (uint32_t)(16384 + (i & 1 ? 1200 : -1200));
        p.send_rev = 5000;
        p.slew = slew_code(5000);
        voice_setup(O.hum_v[i], &p);
        trigger(c, O.hum_v[i], HUM_CENTS[i], MIN(40));
    }
    O.hum_left = prng_range(&c->r_form, MIN(20), MIN(30));
    O.hum_next = 6000;
}

static void hum_tick(comp_t *c)
{
    if (--O.hum_left <= 0) { hum_start(c); return; }
    if (--O.hum_next > 0) return;
    /* slow swells, ~15 dB of range like the clip; louder when the system is busy */
    int32_t g = clampi(prng_range(&c->r_timbre, 6000, 30000) + mul_q15(O.activity - AQ(0.5), 12000), 5000, 32767);
    uint32_t slew = (uint32_t)prng_range(&c->r_rhythm, 3000, 8000);
    for (int i = 0; i < 7; i++) {
        if (!owned(c, O.hum_v[i])) continue;
        int32_t amp = mul_q15(HUM_AMP[i], g);
        amp = mod_q15(amp, prng_range(&c->r_timbre, -32768, 32767), 4900);   /* +/-15% per partial */
        hal_vwrite((unsigned)O.hum_v[i], VR_SLEW, slew_code(slew));
        hal_vwrite((unsigned)O.hum_v[i], VR_AMP, (uint32_t)amp);
    }
    O.hum_next = (int32_t)slew + prng_range(&c->r_rhythm, 0, 3000);
}

/* --------------------------------------------------------- motion tracker */

static void ping(comp_t *c)
{
    if (!owned(c, O.ping_v)) {
        if (role(c, &O.ping_v) < 0) return;
        vparams_t p = {0};
        p.algo = ALGO_FM;
        p.fm_index = 120; p.fm_sus = 0; p.mod_decay = env_decay_ms(220);
        p.attack = env_attack_ms(0); p.decay = env_decay_ms(220); p.sustain = 0;
        p.release = env_release_ms(150);
        p.fmode = FMODE_LP; p.cutoff = svf_coef_hz(6500); p.reso = 26000;
        p.amp = 6000; p.pan = 16384; p.send_rev = 9000; p.send_echo = 15000;
        voice_setup(O.ping_v, &p);
    }
    unsigned v = (unsigned)O.ping_v;
    int32_t cents = 8670 + mul_q15(32767 - O.distance, 400);         /* closer -> slightly higher */
    uint32_t inc = pitch_inc(cents);
    hal_vwrite(v, VR_PINC0, inc);
    hal_vwrite(v, VR_PINC1, inc);
    hal_vwrite(v, VR_PENV, inc * 3 / 100);                           /* starts 3% high, falls */
    trigger(c, O.ping_v, cents, SEC(2));
    O.ping_off = 25;
}

static void tracker_tick(comp_t *c)
{
    if (O.ping_off > 0 && --O.ping_off == 0) off(c, O.ping_v);
    if (!O.alert) return;
    if (O.close_ms > 0) {                                /* contact: rapid, urgent beeping */
        if (--O.ping_next <= 0) { ping(c); O.ping_next = prng_range(&c->r_rhythm, 105, 125); }
        if (--O.close_ms == 0) O.alert = 0;
        return;
    }
    O.distance -= O.approach;
    if (O.distance <= 2500) {
        O.distance = 2500;
        O.close_ms = prng_range(&c->r_rhythm, 2500, 4500);
        return;
    }
    if (--O.ping_next <= 0) {
        ping(c);
        O.ping_next = 120 + (int32_t)(((int64_t)O.distance * 2000) >> 15);   /* 2.1 s far -> 0.12 s near */
    }
}

static void maybe_alert(comp_t *c)
{
    if (O.alert) return;
    /* very rare: mean ~4.5 h between alerts, ~1.1 h when the shared tension is high */
    int32_t t = (c->wx.out[W_TENSION] + 32768) >> 1;
    uint32_t p = (uint32_t)clampi(1 + t / 10923, 1, 4);               /* per 250 ms, /65536 */
    if (!prng_chance(&c->r_form, p)) return;
    O.alert = 1;
    O.distance = 32767;
    int32_t dur = prng_range(&c->r_rhythm, 25000, 60000);
    O.approach = 32767 / dur + 1;
    O.ping_next = 1;
    O.close_ms = 0;
    hal_trace(TR_MARK, 2, (uint32_t)dur + 9000, 0);       /* intentional build-up */
}

/* -------------------------------------------------------------------- mode */

static void mother_start(comp_t *c)
{
    O.act_walk = 0;
    O.activity = AQ(0.45);
    O.recent = 10u << 16;
    O.burst = 0;
    for (int i = 0; i < 2; i++) { O.flow_v[i] = -1; O.relay_v[i] = -1; O.therm_v[i] = -1; }
    O.gurgle_v = O.servo_v = O.drive_v = O.seek_v = O.ping_v = -1;
    for (int g = 0; g < M_GENS; g++) {
        O.gen_v[g] = -1;
        O.gen_n[g] = (uint8_t)prng_range(&c->r_harm, 4, 8);
        for (int i = 0; i < O.gen_n[g]; i++)                     /* 50-cent grid: not a musical scale */
            O.gen_set[g][i] = (prng_range(&c->r_harm, G_LO[g], G_HI[g]) / 50) * 50;
        O.gen_left[g] = 0;
        O.gen_off[g] = 0;
        O.gen_next[g] = prng_range(&c->r_rhythm, 3000, 9000);
    }
    for (int i = 0; i < 7; i++) O.hum_v[i] = -1;
    O.rtick_v = -1;
    O.rtick_next = O.rtick_off = 0;
    hum_start(c);
    flow_start(c, 0);
    flow_start(c, 1);
    O.gurgle_left = 0;
    O.gurgle_next = prng_range(&c->r_rhythm, 5000, 20000);
    O.relay_left = 0;
    O.relay_off = O.relay_bounce = 0;
    O.relay_next = prng_range(&c->r_rhythm, 2000, 8000);
    O.servo_phase = 0;
    O.servo_next = prng_range(&c->r_rhythm, 3000, 12000);
    O.drive_phase = 0;
    O.seek_off = 0;
    O.drive_next = prng_range(&c->r_rhythm, 15000, 60000);
    O.therm_i = 0;
    O.therm_next = prng_range(&c->r_rhythm, 1000, 5000);
    O.alert = 0;
    O.ping_off = 0;
}

static void mother_tick(comp_t *c)
{
    if (c->now % 250 == 9) {
        O.act_walk = clampi(O.act_walk + prng_range(&c->r_form, -700, 700) - O.act_walk / 48, -16000, 16000);
        O.activity = clampi(14000 + mul_q15(c->wx.out[W_DENSITY], 6000) + mul_q15(c->wx.out[W_TENSION], 4000) +
                            O.act_walk + (c->density - 3) * 2500, 0, 32767);
        maybe_alert(c);
    }
    O.recent -= O.recent >> 15;
    O.burst -= O.burst >> 12;
    if (O.gurgle_step > 0 && --O.gurgle_step == 0) off(c, O.gurgle_v);
    hum_tick(c);
    rtick_tick(c);
    flow_tick(c, 0);
    flow_tick(c, 1);
    gurgle_tick(c);
    relay_tick(c);
    servo_tick(c);
    drive_tick(c);
    therm_tick(c);
    for (int g = 0; g < M_GENS; g++) gen_tick(c, g);
    tracker_tick(c);
}

static void mother_event(comp_t *c, const ev_t *e) { (void)c; (void)e; }   /* no harmony to follow */

static void mother_fx(const comp_t *c, fxset_t *f)
{
    (void)c;
    /* a darker, slightly larger metal room */
    f->t60_ms = 2500; f->rev_wet = 9000; f->rev_damp = 20000; f->predelay = 720;
    f->echo_fb = 9000; f->echo_wet = 7000; f->echo_damp = 22000; f->echo_to_rev = 5000;
    f->echo_len_l = 11040; f->echo_len_r = 14880;           /* 230 / 310 ms: hollow sonar */
    f->trim = TRIM;
    f->dry = 32768;
}

const mode_ops_t MODE_MOTHER_OPS = {"MOTHER", mother_start, mother_tick, mother_event, mother_fx};
