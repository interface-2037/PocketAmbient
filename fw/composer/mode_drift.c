/*
 * mode_drift.c - DRIFT: cold, sparse, orchestral (Alien-era Goldsmith).
 *
 * Voices: dark string pads (undetuned saws, slow bows) with a faint bow-noise
 * layer, a lonely distant horn (narrow pulse with a filter swell), a breathy
 * flute (sine + band-passed breath noise, uneven vibrato), a low drone
 * (sub sine or buzzy saw: serpent / didgeridoo), icy high sine clusters,
 * metallic pings and rare stingers. FX: a wobbling tape echo that darkens each
 * repeat, and a short dark room.
 *
 * Conductor ("Goldsmith brain"), every 250 ms: a tension value drifting over
 * minutes scales cluster density, dissonance, swell and stinger rates. Harmony
 * avoids triads: open 4ths/5ths at low tension, tritones in the middle,
 * semitone clusters at the top, and chords dissolve voice by voice instead of
 * progressing. Silence is a parameter: long rests with only the drone and room
 * tails, sometimes not even the drone.
 */
#include "cint.h"

#define D (c->m.d)
#define D_BUDGET   22
#define TRIM       26378             /* loudness calibration to -20 LUFS */
#define CTICK_MS   250
#define NOCHG      (-1000000)
#define TQ(x)      ((int32_t)((x) * 32768))         /* tension thresholds */

static uint16_t slew_code(uint32_t ms) { return ms ? env_rate_tau(ms * 16) : 0; }
static int live(comp_t *c, int v) { return v >= 0 && g_vinfo[v].owner == c->gen; }
static int gated(comp_t *c, int v) { return live(c, v) && g_vinfo[v].gated; }
static int32_t tens(comp_t *c) { return D.tension; }
/* probability per conductor tick from a mean interval in seconds */
static uint32_t per_tick(uint32_t mean_s) { return mean_s ? 65536u / (mean_s * 4) : 65535; }

static int alloc(comp_t *c) { return voice_alloc(c->gen, 0, AP_KS_FIRST - 1, D_BUDGET); }
static int role(comp_t *c, int *slot) { return role_alloc(slot, c->gen, 0, AP_KS_FIRST - 1, D_BUDGET); }

static void start(comp_t *c, int v, vparams_t *p, int32_t cents, uint32_t max_ms)
{
    voice_setup(v, p);
    voice_on(v, c->gen, cents, max_ms, c->now);
}

/* ------------------------------------------------------------ drone + air */

static void drone_tick(comp_t *c)
{
    if (D.drone_left-- > 0) {
        if (--D.drone_swell <= 0 && gated(c, D.drone_v)) {     /* slow amplitude swells */
            D.drone_swell = (int16_t)prng_range(&c->r_rhythm, 8, 20);
            hal_vwrite((unsigned)D.drone_v, VR_AMP, (uint32_t)prng_range(&c->r_timbre, 4500, 9500));
        }
        return;
    }
    if (gated(c, D.drone_v)) voice_off(D.drone_v);
    /* sometimes nothing at all: no drone for 8-15 s (more likely when calm) */
    uint32_t p_off = (uint32_t)clampi(12000 - tens(c) / 4, 3000, 12000);
    if (!D.drone_off && prng_chance(&c->r_form, p_off)) {
        D.drone_off = 1;
        D.drone_left = (int16_t)prng_range(&c->r_rhythm, 24, 44);   /* 6-11 s: silences stay < 20 s */
        return;
    }
    D.drone_off = 0;
    D.drone_left = (int16_t)prng_range(&c->r_rhythm, 80, 240);
    D.drone_swell = 4;
    int v = role(c, &D.drone_v);
    if (v < 0) return;
    int root = 31 + (c->key.tonic + 12 - 7) % 12;           /* G1..F#2 */
    if (prng_chance(&c->r_harm, 13107)) root += prng_chance(&c->r_harm, 32768) ? 6 : 1;   /* tritone / semitone */
    D.drone_p = root;
    vparams_t p = {0};
    if (prng_chance(&c->r_timbre, 26214)) {                  /* buzzy saw "didgeridoo" with a formant */
        p.algo = ALGO_PAD;
        p.pinc0 = p.pinc1 = pitch_inc(root * 100);
        p.wave = 0;
        p.fmode = FMODE_LP; p.cutoff = svf_coef_hz((uint32_t)prng_range(&c->r_timbre, 280, 480));
        p.reso = 9000;
        p.amp = 6000;
    } else {                                                 /* sub sine "serpent", 2nd harmonic for the speaker */
        p.algo = ALGO_SUB;
        p.pinc0 = pitch_inc(root * 100);
        p.pinc1 = pitch_inc(root * 100 + 1200);
        p.wave = 12000;
        p.fmode = FMODE_BYPASS; p.reso = 32767;
        p.amp = 8000;
    }
    p.attack = env_attack_ms(4000); p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms(6000);
    p.pan = 16384; p.send_rev = 5000; p.send_echo = 1500;
    p.slew = slew_code(3000);
    start(c, v, &p, root * 100, SEC(90));
}

/* ---------------------------------------------------------------- strings */

/* pitch set for the current tension: open 4ths/5ths -> tritones -> clusters */
static int harmony_set(comp_t *c, int32_t *out, int n)
{
    int32_t t = tens(c);
    int base = prng_range(&c->r_harm, 50, 62);
    out[0] = base;
    for (int i = 1; i < n; i++) {
        int step;
        if (t < TQ(0.33))      step = prng_chance(&c->r_harm, 32768) ? 5 : 7;
        else if (t < TQ(0.66)) step = prng_chance(&c->r_harm, 32768) ? 6 : (prng_chance(&c->r_harm, 32768) ? 5 : 11);
        else                   step = prng_chance(&c->r_harm, 45875) ? 1 : 2;
        out[i] = out[i - 1] + step;
    }
    if (t >= TQ(0.66) && n >= 3 && prng_chance(&c->r_harm, 32768)) out[n - 1] += 12;   /* displace one cluster tone */
    return n;
}

static void string_voice(comp_t *c, int *slot, int32_t pitch, uint32_t attack_ms, uint32_t amp, int bright)
{
    int v = role(c, slot);
    if (v < 0) return;
    int32_t bq8 = c_bright_q8(c);
    vparams_t p = {0};
    p.algo = ALGO_PAD;
    p.pinc0 = p.pinc1 = pitch_inc(pitch * 100);             /* no detune chorus */
    p.wave = 4000;
    p.fmode = FMODE_LP;
    p.cutoff = svf_coef_hz((uint32_t)(bright ? 600 : 550 + (bq8 - 256) / 2));
    p.reso = 23000;
    p.fenv = 1500;
    p.attack = env_attack_ms(attack_ms);
    p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms((uint32_t)prng_range(&c->r_timbre, 4000, 8000));
    p.amp = amp;
    p.pan = c_pan_rand(c, 11000);
    p.send_rev = 12000; p.send_echo = 2500;
    p.vib_rate = 4500u * 89478u / 1000u; p.vib_depth = 230; p.vib_delay = env_rate_tau(72000);
    start(c, v, &p, pitch * 100, SEC(60));
}

static void bow_noise(comp_t *c, int on)
{
    if (!on) { if (gated(c, D.bow_v)) voice_off(D.bow_v); return; }
    if (gated(c, D.bow_v)) return;
    if (role(c, &D.bow_v) < 0) return;
    vparams_t p = {0};                                       /* airy sul ponticello edge */
    p.algo = ALGO_NOISE; p.noise = NOISE_PINK; p.fmode = FMODE_LP;   /* resonant LP: no HF spill */
    p.cutoff = svf_coef_hz(3000); p.reso = 9000;
    p.attack = env_attack_ms(1500); p.decay = env_decay_ms(1000); p.sustain = 32768;
    p.release = env_release_ms(3000);
    p.amp = 350; p.pan = c_pan_rand(c, 6000); p.send_rev = 8000;
    start(c, D.bow_v, &p, 0, SEC(120));
}

static void strings_tick(comp_t *c)
{
    int any = 0;
    for (int i = 0; i < D.nstr; i++) {
        if (!gated(c, D.str_v[i])) continue;
        any = 1;
        if (D.swelling) continue;
        if (--D.str_left[i] <= 0) voice_off(D.str_v[i]);    /* dissolve voice by voice */
    }
    if (D.swelling) {
        if (!D.swell_started) {
            /* tension swell: louder and brighter while the cluster slides apart */
            D.swell_started = 1;
            uint32_t ms = (uint32_t)D.swell_left * CTICK_MS;
            for (int i = 0; i < D.nstr; i++) {
                int v = D.str_v[i];
                if (!gated(c, v)) continue;
                int spread = (i - D.nstr / 2) * prng_range(&c->r_harm, 1, 2);
                hal_vwrite((unsigned)v, VR_SLEW, slew_code(ms));
                hal_vwrite((unsigned)v, VR_GLIDE, slew_code(ms));
                hal_vwrite((unsigned)v, VR_AMP, (uint32_t)prng_range(&c->r_timbre, 5500, 7500));
                hal_vwrite((unsigned)v, VR_CUTOFF, svf_coef_hz((uint32_t)prng_range(&c->r_timbre, 2200, 3800)));
                uint32_t inc = pitch_inc((D.str_p[i] + spread) * 100);
                hal_vwrite((unsigned)v, VR_PINC0, inc);
                hal_vwrite((unsigned)v, VR_PINC1, inc);
            }
            return;
        }
        if (--D.swell_left > 0) return;
        /* then cut it off, or let it fall away */
        int cut = prng_chance(&c->r_form, 26214);
        for (int i = 0; i < D.nstr; i++) {
            if (!gated(c, D.str_v[i])) continue;
            hal_vwrite((unsigned)D.str_v[i], VR_RELEASE, env_release_ms(cut ? 150 : 2500));
            voice_off(D.str_v[i]);
        }
        bow_noise(c, 0);
        D.swelling = 0;
        D.str_rest = (int16_t)prng_range(&c->r_rhythm, 20, 60);
        if (cut) D.silence = (int16_t)prng_range(&c->r_rhythm, 12, 32);
        return;
    }
    if (any) return;
    bow_noise(c, 0);
    if (D.str_rest > 0) { D.str_rest--; return; }
    if (D.silence > 0) return;

    int32_t t = tens(c);
    int32_t dq8 = c_density_q8(c);
    /* tension swell instead of a chord, more likely when tense */
    uint32_t p_swell = (uint32_t)clampi(t / 3 + (dq8 - 768) * 8 - 2000, 0, 14000);
    if (prng_chance(&c->r_form, p_swell)) {
        D.nstr = (uint8_t)prng_range(&c->r_harm, 3, 5);
        int base = prng_range(&c->r_harm, 55, 66);
        for (int i = 0; i < D.nstr; i++) {
            D.str_p[i] = base + i;                           /* tight semitone cluster */
            string_voice(c, &D.str_v[i], D.str_p[i], 1500, 1400, 0);
        }
        bow_noise(c, 1);
        D.swelling = 1;
        D.swell_started = 0;
        D.swell_left = (int16_t)prng_range(&c->r_rhythm, 20, 60);   /* 5-15 s */
        hal_trace(TR_MARK, 2, (uint32_t)D.swell_left * CTICK_MS + 1500, 0);
        return;
    }
    int n = clampi(2 + t * 2 / 32768 + (dq8 - 768) / 384, 2, D_STR);
    int32_t ps[D_STR];
    harmony_set(c, ps, n);
    D.nstr = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        D.str_p[i] = ps[i];
        string_voice(c, &D.str_v[i], ps[i], (uint32_t)prng_range(&c->r_timbre, 1000, 3000),
                     (uint32_t)(5000 / (n + 1) + 700), 0);
        D.str_left[i] = (int16_t)prng_range(&c->r_rhythm, 24, 80);      /* hang 6-20 s, unevenly */
    }
    bow_noise(c, 1);
    /* rest after it dissolves: long when calm, short when tense */
    D.str_rest = (int16_t)clampi(120 - t / 360 + prng_range(&c->r_rhythm, -20, 20), 12, 140);
    hal_trace(TR_CHORD, (uint32_t)n, (uint32_t)t, 0);
}

/* ----------------------------------------------------------- horn / flute */

static void motif_note(comp_t *c, int first)
{
    int v = D.horn_v;
    if (first) {
        int32_t bq8 = c_bright_q8(c);
        vparams_t p = {0};
        if (D.flute) {                                       /* breathy flute */
            p.algo = ALGO_SUB; p.wave = 4500; p.fmode = FMODE_BYPASS; p.reso = 32767;
            p.attack = env_attack_ms(250);
            p.amp = 5200; p.send_rev = 9000; p.send_echo = 21000;   /* the Echoplex flute */
            p.vib_rate = 5000u * 89478u / 1000u; p.vib_depth = 500; p.vib_delay = env_rate_tau(24000);
        } else {                                             /* distant horn: narrow pulse, filter swell */
            p.algo = ALGO_PULSE; p.wave = (uint32_t)prng_range(&c->r_timbre, 3900, 6500);
            p.fmode = FMODE_LP; p.cutoff = svf_coef_hz(420); p.reso = 21000;
            p.fenv = (uint32_t)(5000 + (bq8 - 256) * 4);
            p.attack = env_attack_ms((uint32_t)prng_range(&c->r_timbre, 400, 800));
            p.amp = 6200; p.send_rev = 13000; p.send_echo = 9000;
            p.vib_rate = 5000u * 89478u / 1000u; p.vib_depth = 300; p.vib_delay = env_rate_tau(48000);
        }
        p.pinc0 = p.pinc1 = pitch_inc(D.horn_p * 100);
        p.decay = env_decay_ms(1000); p.sustain = 32768;
        p.release = env_release_ms((uint32_t)prng_range(&c->r_timbre, 1500, 2500));
        p.pan = c_pan_rand(c, 13000);
        voice_setup(v, &p);
    } else {
        hal_vwrite((unsigned)v, VR_GLIDE, 0);
        hal_vwrite((unsigned)v, VR_PINC0, pitch_inc(D.horn_p * 100));
        hal_vwrite((unsigned)v, VR_PINC1, pitch_inc(D.horn_p * 100));
    }
    voice_off(v);
    voice_on(v, c->gen, D.horn_p * 100, SEC(20), c->now);
    hal_visual_event(VIS_NOTE((uint32_t)D.horn_p, 16384));
    if (D.flute) {                                           /* breath noise around the note */
        if (role(c, &D.breath_v) >= 0) {
            vparams_t b = {0};
            b.algo = ALGO_NOISE; b.noise = NOISE_PINK; b.fmode = FMODE_LP;
            b.cutoff = svf_coef_hz(pitch_hz(D.horn_p * 100 + 1200)); b.reso = 14000;
            b.attack = env_attack_ms(150); b.decay = env_decay_ms(600); b.sustain = 14000;
            b.release = env_release_ms(900);
            b.amp = 1700; b.pan = 16384; b.send_rev = 6000; b.send_echo = 14000;
            start(c, D.breath_v, &b, 0, SEC(20));
        }
    }
    D.horn_left = (int16_t)prng_range(&c->r_rhythm, D.flute ? 5 : 6, D.flute ? 12 : 14);
}

static void horn_tick(comp_t *c)
{
    if (D.horn_left > 0) {
        D.horn_left--;
        if (D.flute && gated(c, D.horn_v)) {                 /* slow, uneven vibrato */
            hal_vwrite((unsigned)D.horn_v, VR_VIB_RATE, (uint32_t)prng_range(&c->r_timbre, 3800, 6200) * 89478u / 1000u);
            hal_vwrite((unsigned)D.horn_v, VR_VIB_DEPTH, (uint32_t)prng_range(&c->r_timbre, 300, 900));
        }
        if (D.horn_left == 3 && D.motif_left == 0 && gated(c, D.horn_v) && prng_chance(&c->r_form, 22938)) {
            /* bend the last note down a microtone or two, so it sounds off */
            hal_vwrite((unsigned)D.horn_v, VR_GLIDE, slew_code(1500));
            uint32_t inc = pitch_inc(D.horn_p * 100 - prng_range(&c->r_harm, 20, 45));
            hal_vwrite((unsigned)D.horn_v, VR_PINC0, inc);
            hal_vwrite((unsigned)D.horn_v, VR_PINC1, inc);
        }
        return;
    }
    if (D.motif_left > 0) {                                  /* next note: a rising 4th or 5th */
        D.motif_left--;
        int step = prng_chance(&c->r_harm, 32768) ? 5 : 7;
        if (tens(c) > TQ(0.7) && prng_chance(&c->r_harm, 16384)) step = 6;
        D.horn_p += step;
        if (D.horn_p > (D.flute ? 86 : 72)) D.horn_p -= 12;
        motif_note(c, 0);
        return;
    }
    if (gated(c, D.horn_v)) {
        voice_off(D.horn_v);
        if (gated(c, D.breath_v)) voice_off(D.breath_v);
        D.horn_rest = (int16_t)clampi(prng_range(&c->r_rhythm, 60, 240) * 768 / c_density_q8(c), 40, 320);
        return;
    }
    if (D.horn_rest > 0) { D.horn_rest--; return; }
    if (D.silence > 0) return;
    if (role(c, &D.horn_v) < 0) { D.horn_rest = 20; return; }
    /* calm: the echoing flute; tense: the lonely horn */
    D.flute = (uint8_t)(tens(c) < TQ(0.4) ? prng_chance(&c->r_form, 45875) : prng_chance(&c->r_form, 9830));
    D.horn_p = D.flute ? prng_range(&c->r_harm, 67, 76) : prng_range(&c->r_harm, 52, 62);
    D.motif_left = (uint8_t)prng_range(&c->r_form, 1, 3);
    motif_note(c, 1);
}

/* ------------------------------------------------------------ rare events */

static void glass(comp_t *c)
{
    int base = prng_range(&c->r_harm, 84, 92);
    for (int i = 0; i < 3; i++) {
        int v = alloc(c);
        if (v < 0) return;
        int p = base + i * prng_range(&c->r_harm, 1, 2);
        vparams_t q = {0};
        q.algo = ALGO_SUB; q.pinc0 = pitch_inc(p * 100 + prng_range(&c->r_timbre, -6, 6));
        q.wave = 0; q.fmode = FMODE_BYPASS; q.reso = 32767;
        q.attack = env_attack_ms(3000); q.decay = env_decay_ms(1000); q.sustain = 32768;
        q.release = env_release_ms(6000);
        q.amp = 800; q.pan = c_pan_rand(c, 15000); q.send_rev = 20000; q.send_echo = 8000;
        start(c, v, &q, p * 100, SEC(30));
        c_schedule_off(c, v, (uint32_t)prng_range(&c->r_rhythm, 8000, 15000));
    }
}

static void ping(comp_t *c)
{
    int v = alloc(c);
    if (v < 0) return;
    int p = prng_range(&c->r_harm, 84, 98);
    vparams_t q = {0};                                       /* inharmonic FM: ship metal */
    q.algo = ALGO_FM;
    q.pinc0 = pitch_inc(p * 100);
    q.pinc1 = prng_chance(&c->r_timbre, 32768) ? inc_mul_ratio(q.pinc0, 241, 100) : inc_mul_ratio(q.pinc0, 373, 100);
    q.fm_index = 900; q.fm_sus = 100; q.mod_decay = env_decay_ms(400);
    q.attack = env_attack_ms(1); q.decay = env_decay_ms(700); q.sustain = 0;
    q.release = env_release_ms(800);
    q.fmode = FMODE_LP; q.cutoff = svf_coef_hz(6500); q.reso = 26000;
    q.amp = 2400; q.pan = c_pan_rand(c, 15000); q.send_rev = 12000; q.send_echo = 22000;
    start(c, v, &q, p * 100, SEC(10));
    c_schedule_off(c, v, 100);
}

static void stinger(comp_t *c)
{
    int p = prng_range(&c->r_harm, 28, 40);
    for (int k = 0; k < 2; k++) {
        int v = alloc(c);
        if (v < 0) return;
        vparams_t q = {0};
        if (k == 0) {                                        /* low brass hit */
            q.algo = ALGO_PULSE; q.wave = 9000;
            q.fmode = FMODE_LP; q.cutoff = svf_coef_hz(260); q.reso = 18000; q.fenv = 16000;
            q.amp = 9000;
        } else {                                             /* drone hit underneath */
            q.algo = ALGO_SUB; q.wave = 14000; q.pinc1 = pitch_inc(p * 100 + 1200);
            q.fmode = FMODE_BYPASS; q.reso = 32767;
            q.amp = 6500;
        }
        q.pinc0 = pitch_inc(p * 100);
        if (k == 0) q.pinc1 = q.pinc0;
        q.attack = env_attack_ms(8); q.decay = env_decay_ms(1500); q.sustain = 0;
        q.release = env_release_ms(1200);
        q.pan = 16384; q.send_rev = 9000; q.send_echo = 20000;
        start(c, v, &q, p * 100, SEC(10));
        c_schedule_off(c, v, 400);
    }
    hal_trace(TR_MARK, 1, 0, 0);
    D.silence = (int16_t)prng_range(&c->r_rhythm, 40, 80);      /* let it ring into silence */
}

/* ------------------------------------------------------------------- mode */

static void drift_start(comp_t *c)
{
    D.tension = 8000;
    D.tens_walk = 0;
    D.ctick = 0;
    D.silence = 0;
    D.drone_v = D.air_v = D.bow_v = D.horn_v = D.breath_v = -1;
    D.drone_left = 0;
    D.drone_off = 1;                                         /* start with the drone */
    for (int i = 0; i < D_STR; i++) D.str_v[i] = -1;
    D.nstr = 0;
    D.swelling = 0;
    D.str_rest = (int16_t)prng_range(&c->r_rhythm, 8, 24);
    D.horn_left = 0;
    D.motif_left = 0;
    D.horn_rest = (int16_t)prng_range(&c->r_rhythm, 24, 80);
    D.glass_left = (int32_t)prng_exp(&c->r_rhythm, 120 * 4) + 60;
    D.ping_left = (int32_t)prng_exp(&c->r_rhythm, 60 * 4) + 40;
    D.sting_left = (int32_t)prng_exp(&c->r_form, 600 * 4) + 480;
}

static void drift_tick(comp_t *c)
{
    if (c->now % CTICK_MS != 7) return;
    D.ctick++;
    /* tension: slow weather + a mean-reverting random walk */
    D.tens_walk = clampi(D.tens_walk + prng_range(&c->r_form, -500, 500) - D.tens_walk / 64, -14000, 14000);
    D.tension = clampi(14000 + mul_q15(c->wx.out[W_TENSION], 14000) + D.tens_walk, 0, 32767);

    if (D.silence > 0) D.silence--;
    else if (prng_chance(&c->r_form, (uint32_t)clampi(260 - tens(c) / 160, 40, 260)))
        D.silence = (int16_t)prng_range(&c->r_rhythm, 40, 100);   /* 10-25 s of rest */

    drone_tick(c);
    strings_tick(c);
    horn_tick(c);

    int32_t t = tens(c);
    if (--D.glass_left <= 0) {
        if (!D.silence && t > TQ(0.2) && t < TQ(0.8)) glass(c);
        D.glass_left = (int32_t)prng_exp(&c->r_rhythm, 120 * 4) + 60;
    }
    if (--D.ping_left <= 0) {
        ping(c);
        D.ping_left = (int32_t)prng_exp(&c->r_rhythm, (uint32_t)clampi(90 - t / 600, 25, 90) * 4) + 40;
    }
    if (--D.sting_left <= 0) {
        if (t > TQ(0.35)) stinger(c);
        D.sting_left = (int32_t)prng_exp(&c->r_form, (uint32_t)clampi(900 - t / 50, 300, 900) * 4) + 480;
    }
    (void)per_tick;
}

static void drift_event(comp_t *c, const ev_t *e)
{
    if (e->type == EV_KEY) D.drone_left = 0;                /* the drone follows the key */
}

static void drift_fx(const comp_t *c, fxset_t *f)
{
    (void)c;
    /* short, dark, cold room; the tape echo does the heavy lifting */
    f->t60_ms = 2600; f->rev_wet = 9000; f->rev_damp = 18000; f->predelay = 960;
    f->echo_fb = 18000; f->echo_wet = 15000; f->echo_damp = 14000; f->echo_to_rev = 6000;
    f->echo_len_l = 22560; f->echo_len_r = 22560;           /* 470 ms tape */
    f->echo_mod_depth = 12 << 8; f->echo_mod_rate = 53687;  /* +/-12 samples at 0.6 Hz */
    f->trim = TRIM;
    f->dry = 32768;
}

const mode_ops_t MODE_DRIFT_OPS = {"DRIFT", drift_start, drift_tick, drift_event, drift_fx};
