/*
 * mode_mother.c - MOTHER: inside the Nostromo. No music, no score: machinery.
 *
 *  Tone set from the user's reference clip (see the tables below).
 *  - Machinery hum: seven sine partials at the clip's frequencies and levels
 *    (each slowly beating against a detuned twin) over a low noise rumble,
 *    building and receding over tens of seconds.
 *  - Coolant / pipe flow: two resonant noise beds whose resonances and levels
 *    wander slowly (seconds-long slews), with occasional gurgles (short bursts
 *    of bubbly resonant noise at random low pitches).
 *  - Relays: dull ~1-2 ms double clicks (close, then a bounce 6-15 ms later),
 *    in small clusters.
 *  - Servos / motors: a pulse-wave whine that glides up, holds, and glides back
 *    down over 0.3-2 s.
 *  - Disk / tape drives: a spin-up whir (40 Hz up), bursts of rapid seek ticks, a spin-down.
 *  - Thermal ticks: rare, very quiet metallic pings (short inharmonic FM).
 *  - Data chirrs: the clip's beep gesture - near-pure sine pulses 20 per second
 *    (25-35 ms each) at one pitch (or two alternating), fading out over the last
 *    ~0.6 s, with readout ticks riding along. Pitches 1.55-2.2 kHz (the clip's
 *    2.63 kHz was too piercing). Single quiet tones now and then in between.
 *  - Vent hiss: a rare 1-2 s broadband air release.
 *  - Clunks: a rare heavy low thump (falling sine + dark noise).
 *  - Motion tracker: a very rare event (~1-4 h apart): pings speed up and rise
 *    as `distance` closes, ending in rapid beeping.
 *
 * Conductor: `system_activity` wanders slowly (weather + random walk + the
 * density setting) and sets how often everything happens. Tracker alerts are
 * more likely when the shared weather tension is high.
 */
#include "cint.h"

#define O (c->m.o)
#define O_BUDGET 24
#define TRIM     44900               /* loudness calibration (-21.5 LUFS) */
#define AQ(x)    ((int32_t)((x) * 32768))

/* Tone set measured from the reference clip (MOTHER.wav, 2026-10-10):
 * - hum partials (Hz, dB rel. 131.9 Hz): 131.9 0, 115.7 -4.9, 149.4 -5.7, 205.9 -7.3,
 *   87.5 -8.8, 57.9 -17.9, 49.8 -25.9; 263.8 Hz (2nd harmonic of 131.9) -18
 * - beeps: near-pure sines (3rd harmonic -40 dB, 2nd -27 dB), pulsed 20/s in one ~1 s
 *   chirr that fades out; a few quiet single tones at other pitches
 * - a broadband vent hiss (~1.5 s) and one heavy low clunk
 * - little above 5 kHz, near-mono (L/R correlation 0.93) */
static const int32_t HUM_CENTS[7] = {4814, 4587, 5030, 5585, 4104, 3389, 3128};
static const int32_t HUM_AMP[7]   = {5400, 3070, 2800, 2330, 1960, 680, 280};
#define BEEP_LO   9050             /* ~1.55 kHz */
#define BEEP_HI   9686             /* ~2.2 kHz: the clip's 2.63 kHz was too high */
#define TONE_LO   8700             /* ~1.25 kHz */

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
    p.cutoff = svf_coef_hz(hz); p.reso = 18000;            /* dull, not ringing */
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
    p.cutoff = svf_coef_hz(i ? 180 : 90); p.reso = 9000;      /* low hum, not wind */
    p.attack = env_attack_ms(6000); p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms(6000);
    p.amp = i ? 2500 : 3000; p.pan = (uint32_t)(i ? 19500 : 13500);
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
    hal_vwrite(v, VR_CUTOFF, svf_coef_hz((uint32_t)(i ? prng_range(&c->r_timbre, 140, 260)
                                                      : prng_range(&c->r_timbre, 70, 140))));
    hal_vwrite(v, VR_RESO, (uint32_t)prng_range(&c->r_timbre, 9000, 14000));   /* no sharp whistle */
    int32_t amp = i ? prng_range(&c->r_timbre, 1500, 3000) : prng_range(&c->r_timbre, 2000, 3500);
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
                click_patch(c, O.relay_v[i], (uint32_t)prng_range(&c->r_timbre, 1000, 2000));
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
        p.attack = env_attack_ms(1500); p.decay = env_decay_ms(1000); p.sustain = 32768;
        p.release = env_release_ms(800);
        p.amp = 2000; p.pan = c_pan_rand(c, 2000); p.send_rev = 8000;
        voice_setup((int)v, &p);
        trigger(c, O.drive_v, 2750, SEC(40));
        int32_t spin = prng_range(&c->r_rhythm, 1200, 2500);
        int32_t top = prng_range(&c->r_timbre, 4300, 5100);              /* ~100-160 Hz */
        hal_vwrite(v, VR_GLIDE, slew_code((uint32_t)spin));
        hal_vwrite(v, VR_PINC0, pitch_inc(top));
        hal_vwrite(v, VR_PINC1, pitch_inc(top + 6));
        if (!owned(c, O.seek_v) && role(c, &O.seek_v) >= 0) click_patch(c, O.seek_v, 1800);
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
        int32_t cents = prng_range(&c->r_timbre, 8400, 9300);           /* ~1-1.75 kHz */
        vparams_t q = {0};
        q.algo = ALGO_FM;
        q.pinc0 = pitch_inc(cents);
        q.pinc1 = prng_chance(&c->r_timbre, 32768) ? inc_mul_ratio(q.pinc0, 173, 100)
                                                   : inc_mul_ratio(q.pinc0, 289, 100);
        q.fm_index = 400; q.fm_sus = 40; q.mod_decay = env_decay_ms(60);
        q.attack = env_attack_ms(1); q.decay = env_decay_ms((uint32_t)prng_range(&c->r_timbre, 150, 350));
        q.sustain = 0; q.release = env_release_ms(200);
        q.fmode = FMODE_LP; q.cutoff = svf_coef_hz(3000); q.reso = 24000;
        q.amp = (uint32_t)prng_range(&c->r_timbre, 400, 1000);
        q.pan = c_pan_rand(c, 3750); q.send_rev = 7000; q.send_echo = 2500;
        voice_setup(*slot, &q);
        trigger(c, *slot, cents, SEC(3));
        voice_off(*slot);                                 /* gate 0: the decay does the rest */
    }
    O.therm_next = wait_ms(c, 20000, 10000, 2000);
}

/* ------------------------------------------------------------- data beeps */

/* near-pure sine (+ a -27 dB 2nd harmonic, as in the clip) with soft 2-3 ms edges */
static void beep_patch(comp_t *c, int v, int32_t cents, uint32_t amp, uint32_t pan)
{
    (void)c;
    vparams_t p = {0};
    p.algo = ALGO_SUB; p.wave = 1500;
    p.pinc0 = pitch_inc(cents); p.pinc1 = pitch_inc(cents + 1200);
    p.fmode = FMODE_LP; p.cutoff = svf_coef_hz(5000); p.reso = 32767;
    p.attack = env_attack_ms(2); p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms(3);
    p.amp = amp; p.pan = pan;
    p.send_rev = 9000; p.send_echo = 2500;
    p.slew = slew_code(1);                                /* per-pulse level steps */
    voice_setup(v, &p);
}

static void rtick(comp_t *c)
{
    if (!owned(c, O.rtick_v)) {
        if (role(c, &O.rtick_v) < 0) return;
        click_patch(c, O.rtick_v, 2000);
    }
    hal_vwrite((unsigned)O.rtick_v, VR_AMP, (uint32_t)prng_range(&c->r_timbre, 1500, 3000));
    trigger(c, O.rtick_v, 0, SEC(2));
    O.rtick_off = 2;
}

static void rtick_tick(comp_t *c)
{
    if (O.rtick_off > 0 && --O.rtick_off == 0) off(c, O.rtick_v);
}

/* a chirr: 15-25 pulses 50 ms apart, full level, then ~11 dB of fade over the last 12 */
static void chirr_tick(comp_t *c)
{
    if (O.chirr_off > 0 && --O.chirr_off == 0) off(c, O.chirr_v);
    if (--O.chirr_next > 0) return;
    if (O.chirr_left <= 0) {                              /* start a chirr */
        if (role(c, &O.chirr_v) < 0) { O.chirr_next = 2000; return; }
        int32_t cents = (prng_range(&c->r_harm, BEEP_LO, BEEP_HI) / 25) * 25;
        O.chirr_cents[0] = O.chirr_cents[1] = cents;
        if (prng_chance(&c->r_form, 11500))               /* ~35%: two alternating pitches */
            O.chirr_cents[1] = clampi(cents - (prng_range(&c->r_harm, 150, 500) / 25) * 25, TONE_LO, BEEP_HI);
        O.chirr_amp = prng_range(&c->r_timbre, 2800, 4000);
        beep_patch(c, O.chirr_v, cents, (uint32_t)O.chirr_amp, (uint32_t)c_pan_rand(c, 1500));
        O.chirr_n = O.chirr_left = (int16_t)prng_range(&c->r_rhythm, 15, 25);
    }
    if (owned(c, O.chirr_v)) {
        unsigned v = (unsigned)O.chirr_v;
        int k = O.chirr_n - O.chirr_left;                 /* pulse index */
        if (O.chirr_left <= 12) O.chirr_amp = mul_q15(O.chirr_amp, 29500);
        int32_t cents = O.chirr_cents[k & 1];
        hal_vwrite(v, VR_PINC0, pitch_inc(cents));
        hal_vwrite(v, VR_PINC1, pitch_inc(cents + 1200));
        hal_vwrite(v, VR_AMP, (uint32_t)O.chirr_amp);
        trigger(c, O.chirr_v, cents, SEC(2));
        O.chirr_off = prng_range(&c->r_rhythm, 25, 35);
        if (k < 8 && !(k & 1)) rtick(c);                  /* readout ticks on the loud part */
    }
    if (--O.chirr_left > 0) O.chirr_next = 50;
    else O.chirr_next = wait_ms(c, 75000, 15000, 6000);
}

/* now and then a single quiet tone at another pitch; rarely a held one (~0.4 s) */
static void tone_tick(comp_t *c)
{
    if (O.tone_off > 0 && --O.tone_off == 0) off(c, O.tone_v);
    if (--O.tone_next > 0) return;
    O.tone_next = wait_ms(c, 40000, 12000, 3000);
    if (role(c, &O.tone_v) < 0) return;
    int32_t cents = (prng_range(&c->r_harm, TONE_LO, BEEP_HI) / 25) * 25;
    uint32_t amp = (uint32_t)prng_range(&c->r_timbre, 700, 1300);   /* one draw per statement: */
    beep_patch(c, O.tone_v, cents, amp, (uint32_t)c_pan_rand(c, 3000));  /* gcc/clang arg order differs */
    trigger(c, O.tone_v, cents, SEC(2));
    O.tone_off = prng_chance(&c->r_form, 6500) ? prng_range(&c->r_rhythm, 300, 500)
                                               : prng_range(&c->r_rhythm, 35, 60);
}

/* ------------------------------------------------------- vent hiss / clunk */

static void vent_tick(comp_t *c)
{
    if (O.vent_off > 0 && --O.vent_off == 0) off(c, O.vent_v);
    if (--O.vent_next > 0) return;
    O.vent_next = wait_ms(c, 200000, 70000, 30000);
    if (role(c, &O.vent_v) < 0) return;
    vparams_t p = {0};
    p.algo = ALGO_NOISE; p.noise = NOISE_PINK; p.fmode = FMODE_LP;    /* LP: 12 dB/oct keeps it out of the top octave */
    p.cutoff = svf_coef_hz((uint32_t)prng_range(&c->r_timbre, 2500, 4000)); p.reso = 26000;
    p.attack = env_attack_ms((uint32_t)prng_range(&c->r_timbre, 150, 500));
    p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms((uint32_t)prng_range(&c->r_timbre, 500, 1000));
    p.amp = (uint32_t)prng_range(&c->r_timbre, 2500, 4000);
    p.pan = c_pan_rand(c, 4000); p.send_rev = 9000;
    voice_setup(O.vent_v, &p);
    trigger(c, O.vent_v, 0, SEC(5));
    O.vent_off = prng_range(&c->r_rhythm, 600, 1400);
}

static void clunk_tick(comp_t *c)
{
    if (O.clunk_off > 0 && --O.clunk_off == 0) { off(c, O.clunk_v[0]); off(c, O.clunk_v[1]); }
    if (--O.clunk_next > 0) return;
    O.clunk_next = wait_ms(c, 180000, 60000, 20000);
    uint16_t pan = c_pan_rand(c, 2500);
    if (role(c, &O.clunk_v[0]) >= 0) {                    /* falling low sine: the weight */
        vparams_t q = {0};
        int32_t cents = prng_range(&c->r_timbre, 2900, 3500);            /* ~45-60 Hz */
        q.algo = ALGO_SUB; q.fmode = FMODE_BYPASS; q.reso = 32767;
        q.pinc0 = pitch_inc(cents);
        q.penv = pitch_inc(cents + 1200) - pitch_inc(cents);              /* starts an octave up */
        q.mod_decay = env_decay_ms(90);
        q.attack = env_attack_ms(1); q.decay = env_decay_ms(450); q.sustain = 0;
        q.release = env_release_ms(300);
        q.amp = (uint32_t)prng_range(&c->r_timbre, 10000, 15000);
        q.pan = pan; q.send_rev = 9000;
        voice_setup(O.clunk_v[0], &q);
        trigger(c, O.clunk_v[0], cents, SEC(3));
    }
    if (role(c, &O.clunk_v[1]) >= 0) {                    /* dark noise: the impact */
        vparams_t q = {0};
        q.algo = ALGO_NOISE; q.noise = NOISE_PINK; q.fmode = FMODE_LP;
        q.cutoff = svf_coef_hz((uint32_t)prng_range(&c->r_timbre, 250, 500)); q.reso = 20000;
        q.attack = env_attack_ms(1); q.decay = env_decay_ms(160); q.sustain = 0;
        q.release = env_release_ms(120);
        q.amp = (uint32_t)prng_range(&c->r_timbre, 7000, 11000);
        q.pan = pan; q.send_rev = 10000;
        voice_setup(O.clunk_v[1], &q);
        trigger(c, O.clunk_v[1], 0, SEC(3));
    }
    O.clunk_off = 30;
}

/* ------------------------------------------------------------------- hum */

static void hum_start(comp_t *c)
{
    for (int i = 0; i < 7; i++) {
        if (role(c, &O.hum_v[i]) < 0) continue;
        vparams_t p = {0};
        p.algo = ALGO_SUB;
        p.pinc0 = pitch_inc(HUM_CENTS[i]);
        if (i == 0) { p.pinc1 = pitch_inc(HUM_CENTS[i] + 1200); p.wave = 4100; }   /* 263.8 Hz harmonic, -18 dB */
        else { p.pinc1 = pitch_inc(HUM_CENTS[i] + prng_range(&c->r_timbre, 3, 8)); p.wave = 9000; }  /* slow beating */
        p.vib_rate = (uint32_t)prng_range(&c->r_timbre, 600, 1400) * 89478u / 1000u;     /* ~1 Hz wobble */
        p.vib_depth = 60;
        p.fmode = FMODE_BYPASS; p.reso = 32767;
        p.attack = env_attack_ms(6000); p.decay = env_decay_ms(1000); p.sustain = 32768;
        p.release = env_release_ms(6000);
        p.amp = (uint32_t)mul_q15(HUM_AMP[i], O.hum_g);       /* re-articulate at the current level */
        p.pan = (uint32_t)(16384 + (i & 1 ? 1200 : -1200));
        p.send_rev = 5000;
        p.slew = slew_code(5000);
        voice_setup(O.hum_v[i], &p);
        trigger(c, O.hum_v[i], HUM_CENTS[i], MIN(40));
    }
    if (role(c, &O.rumble_v) >= 0) {                      /* the clip's hum sits on a noisy floor */
        vparams_t p = {0};
        p.algo = ALGO_NOISE; p.noise = NOISE_PINK; p.fmode = FMODE_BP;
        p.cutoff = svf_coef_hz(110); p.reso = 22000;
        p.attack = env_attack_ms(6000); p.decay = env_decay_ms(1000); p.sustain = 32768;
        p.release = env_release_ms(6000);
        p.amp = (uint32_t)mul_q15(3000, O.hum_g); p.pan = 16384; p.send_rev = 5000;
        p.slew = slew_code(5000);
        voice_setup(O.rumble_v, &p);
        trigger(c, O.rumble_v, 0, MIN(40));
    }
    O.hum_left = prng_range(&c->r_form, MIN(20), MIN(30));
    O.hum_next = 6000;
}

static void hum_tick(comp_t *c)
{
    if (--O.hum_left <= 0) { hum_start(c); return; }
    if (--O.hum_next > 0) return;
    /* slow builds and recessions over tens of seconds (~18 dB of range; the clip's hum rises further, but
     * a deeper floor lets every other event jump out);
     * louder when the system is busy */
    int32_t g = clampi(prng_range(&c->r_timbre, 4000, 30000) + mul_q15(O.activity - AQ(0.5), 12000), 3000, 32767);
    uint32_t slew = (uint32_t)prng_range(&c->r_rhythm, 8000, 20000);
    for (int i = 0; i < 7; i++) {
        if (!owned(c, O.hum_v[i])) continue;
        int32_t amp = mul_q15(HUM_AMP[i], g);
        amp = mod_q15(amp, prng_range(&c->r_timbre, -32768, 32767), 4900);   /* +/-15% per partial */
        hal_vwrite((unsigned)O.hum_v[i], VR_SLEW, slew_code(slew));
        hal_vwrite((unsigned)O.hum_v[i], VR_AMP, (uint32_t)amp);
    }
    O.hum_g = g;
    if (owned(c, O.rumble_v)) {
        hal_vwrite((unsigned)O.rumble_v, VR_SLEW, slew_code(slew));
        hal_vwrite((unsigned)O.rumble_v, VR_AMP, (uint32_t)mul_q15(3000, g));
    }
    O.hum_next = (int32_t)slew + prng_range(&c->r_rhythm, 0, 5000);
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
    for (int i = 0; i < 2; i++) { O.flow_v[i] = -1; O.relay_v[i] = -1; O.therm_v[i] = -1; O.clunk_v[i] = -1; }
    O.gurgle_v = O.servo_v = O.drive_v = O.seek_v = O.ping_v = -1;
    O.chirr_v = O.tone_v = O.vent_v = O.rumble_v = -1;
    O.chirr_left = 0; O.chirr_off = O.tone_off = O.vent_off = O.clunk_off = 0;
    O.chirr_next = prng_range(&c->r_rhythm, 4000, 15000);
    O.tone_next = prng_range(&c->r_rhythm, 8000, 25000);
    O.vent_next = prng_range(&c->r_rhythm, 30000, 120000);
    O.clunk_next = prng_range(&c->r_rhythm, 20000, 90000);
    for (int i = 0; i < 7; i++) O.hum_v[i] = -1;
    O.rtick_v = -1;
    O.rtick_off = 0;
    O.hum_g = 16384;
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
    chirr_tick(c);
    tone_tick(c);
    vent_tick(c);
    clunk_tick(c);
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
