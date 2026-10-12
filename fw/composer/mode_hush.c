/*
 * mode_hush.c - HUSH: a lo-fi beat. Swung boom-bap drums (pitch-enveloped sine
 * kick, band-passed noise snare and hats), jazzy 7th/9th chords on a soft FM
 * electric piano with tape wow, a sine bass with approach notes, a sparse plucked
 * pentatonic lead, and sparse vinyl crackle underneath. Arranged in sections:
 * intro -> grooves -> occasional breakdowns; key and tempo change at breakdowns.
 */
#include "cint.h"

#define H (c->m.h)
#define H_BUDGET   22
#define H_BED_MAX  MIN(40)           /* beds re-articulate at every key change (<= 25 min) */
#define TRIM       20450             /* loudness calibration to -20 LUFS */

enum { S_INTRO, S_GROOVE, S_BREAK };
enum { I_KICK = 1, I_SNARE, I_HAT, I_OHAT, I_KEYS, I_BASS, I_LEAD };

static const int32_t KEYS_HZ[5] = {1500, 2000, 2700, 3400, 4300};
static const int32_t KEYS_FM[5] = {500, 700, 900, 1150, 1400};
static const int32_t HAT_HZ[5]  = {4200, 4800, 5400, 6000, 6600};
static const int32_t KS_BRT[5]  = {7000, 10000, 14000, 19000, 25000};

/* kick patterns, bit n = 16th step n */
static const uint16_t KICKS[6] = {
    (1u << 0) | (1u << 8),
    (1u << 0) | (1u << 7) | (1u << 10),
    (1u << 0) | (1u << 10),
    (1u << 0) | (1u << 3) | (1u << 8),
    (1u << 0) | (1u << 8) | (1u << 11),
    (1u << 0) | (1u << 6) | (1u << 8) | (1u << 14),
};

/* ------------------------------------------------------------------ harmony */

static int deg_pc(comp_t *c, int deg, int k) { return key_pc_of_degree(H.key, H.key.centre + deg + k); }

static int chord_ok(comp_t *c, int deg)
{
    chord_t ch = {(int8_t)deg, CH_SEV};
    return chord_valid(H.key, ch);
}

/* 2- or 4-chord loop of 7th chords, mostly moving down in fifths (ii-V-I style) */
static void gen_prog(comp_t *c)
{
    static const uint32_t start_w[7] = {5, 4, 0, 3, 0, 4, 0};      /* I ii IV vi */
    static const int8_t step[5] = {3, 5, 6, 1, 4};                  /* down 5th, down 3rd, down step, up step, up 5th */
    static const uint32_t step_w[5] = {7, 3, 2, 1, 1};
    H.prog_len = (uint8_t)(prng_chance(&c->r_form, 52429) ? 4 : 2);
    H.bars_per_chord = (uint8_t)((H.prog_len == 2 || prng_chance(&c->r_form, 26214)) ? 2 : 1);
    int d;
    do { d = prng_weighted(&c->r_harm, start_w, 7); } while (!chord_ok(c, d));
    for (int i = 0; i < H.prog_len; i++) {
        H.prog[i] = (int8_t)d;
        int nd, tries = 0;
        do { nd = (d + step[prng_weighted(&c->r_harm, step_w, 5)]) % 7; }
        while ((!chord_ok(c, nd) || nd == d) && ++tries < 32);
        d = nd;
    }
}

static int chord_at(comp_t *c, int sec_bar) { return H.prog[(sec_bar / H.bars_per_chord) % H.prog_len]; }
static int bass_root(comp_t *c, int deg) { return 36 + deg_pc(c, deg, 0); }      /* C2..B2 */

/* ---------------------------------------------------------------- scheduling */

static uint64_t step_tick(comp_t *c, uint64_t bar_us, int step, int jitter_ms)
{
    uint64_t step_us = 240000000ull / H.bpm / 16;
    uint64_t pos = bar_us + (uint64_t)step * step_us;
    if (step & 1) pos += (uint64_t)((((int64_t)H.swing_q15 * 2 - 32768) * (int64_t)step_us) >> 15);
    int64_t t = (int64_t)(H.t0 + pos / 1000) + jitter_ms;
    return t < (int64_t)c->now ? c->now : (uint64_t)t;
}

static void hit(comp_t *c, uint64_t bar_us, int step, int inst, int pitch, int vel, uint32_t dur)
{
    int j = (inst == I_KEYS || inst == I_BASS) ? prng_range(&c->r_rhythm, -12, 12)
                                               : prng_range(&c->r_rhythm, -6, 6);
    vel = clampi(vel, 1, 255);
    sched_push(&c->sched, step_tick(c, bar_us, step, j), EV_H_HIT, 90, c->gen,
               ((uint32_t)inst << 24) | ((uint32_t)(pitch & 0xFF) << 8) | (uint32_t)vel, dur);
}

/* ---------------------------------------------------------------------- beds */

static int live(comp_t *c, int v) { return v >= 0 && g_vinfo[v].owner == c->gen && g_vinfo[v].gated; }

static void bed(comp_t *c, int *slot, vparams_t *p, uint32_t attack_ms)
{
    int v = role_alloc(slot, c->gen, 0, AP_KS_FIRST - 1, H_BUDGET);
    if (v < 0) return;
    p->attack = env_attack_ms(attack_ms);
    p->decay = env_decay_ms(1000);
    p->sustain = 32768;
    p->release = env_release_ms(6000);
    voice_setup(v, p);
    voice_on(v, c->gen, 0, H_BED_MAX, c->now);
}

static void beds_start(comp_t *c, uint32_t attack_ms)
{
    vparams_t q = {0};                                  /* vinyl crackle: sparse pops */
    q.algo = ALGO_NOISE; q.noise = NOISE_PDUST; q.fmode = FMODE_LP;
    q.cutoff = svf_coef_hz(2600); q.reso = 12000; q.dust_rate = 5;   /* ~4 pops/s */
    q.amp = 4500; q.pan = 16384; q.send_rev = 1500;
    bed(c, &H.crackle_v, &q, attack_ms);
}

/* ------------------------------------------------------------------ bar logic */

static void next_section(comp_t *c)
{
    uint8_t prev = H.section;
    if (prev == S_GROOVE && (H.pending_key || prng_chance(&c->r_form, 19661))) {
        H.section = S_BREAK;
        H.sec_left = (uint8_t)(prng_chance(&c->r_form, 32768) ? 4 : 8);
        if (H.pending_key) {
            H.pending_key = 0;
            H.key = c->key;
            H.bpm = (uint32_t)clampi((int32_t)H.bpm + prng_range(&c->r_form, -6, 6), 70, 88);
            H.voiced_deg = -1;
            gen_prog(c);
            beds_start(c, 6000);
        }
    } else {
        H.section = S_GROOVE;
        H.sec_left = (uint8_t)(prng_chance(&c->r_form, 32768) ? 16 : 24);
        H.kick_pat = (uint8_t)prng_below(&c->r_form, 6);
        if (prev != S_INTRO && prng_chance(&c->r_form, 39321)) gen_prog(c);
    }
    H.sec_bar = 0;
}

static void drums(comp_t *c, uint64_t bus, int32_t dq8)
{
    int fill = H.sec_left == 1;
    uint16_t kp = KICKS[H.kick_pat];
    if (prng_chance(&c->r_rhythm, 13107)) kp = KICKS[prng_below(&c->r_rhythm, 6)];   /* 20% variation */
    if (fill) kp &= 0x00FF;
    for (int s = 0; s < 16; s++)
        if (kp >> s & 1) hit(c, bus, s, I_KICK, 0, s == 0 ? 235 : prng_range(&c->r_timbre, 170, 215), 140);

    hit(c, bus, 4, I_SNARE, 0, prng_range(&c->r_timbre, 190, 225), 100);
    hit(c, bus, 12, I_SNARE, 0, prng_range(&c->r_timbre, 195, 230), 100);
    static const int8_t ghosts[4] = {7, 9, 15, 3};
    uint32_t gp = (uint32_t)clampi((dq8 - 256) * 12, 0, 13000);
    for (int g = 0; g < 4; g++)
        if (prng_chance(&c->r_rhythm, gp)) hit(c, bus, ghosts[g], I_SNARE, 0, prng_range(&c->r_timbre, 50, 85), 60);
    if (fill) {
        hit(c, bus, 14, I_SNARE, 0, 140, 80);
        hit(c, bus, 15, I_SNARE, 0, 170, 80);
    }

    int quarters = dq8 < 400;
    uint32_t p16 = (uint32_t)clampi((dq8 - 800) * 40, 0, 26000);
    int open = !fill && prng_chance(&c->r_rhythm, 14000);
    for (int s = 0; s < 16; s++) {
        if (fill && s >= 14) break;
        int on_q = (s & 3) == 0;
        if (quarters && !on_q) continue;
        if (s & 1) {
            if (prng_chance(&c->r_rhythm, p16))
                hit(c, bus, s, I_HAT, 0, prng_range(&c->r_timbre, 45, 75), 40);
            continue;
        }
        if (prng_chance(&c->r_rhythm, 5243)) continue;                       /* 8% dropped */
        if (open && s == 14) { hit(c, bus, s, I_OHAT, 0, 130, 220); continue; }
        hit(c, bus, s, I_HAT, 0, on_q ? prng_range(&c->r_timbre, 130, 165) : prng_range(&c->r_timbre, 85, 120), 40);
    }
}

static void keys_comp(comp_t *c, uint64_t bus, int deg, int next_deg, uint32_t bar_ms, int antic)
{
    static const uint32_t pw[4] = {4, 2, 2, 1};
    int pat = prng_weighted(&c->r_rhythm, pw, 4);
    int vel = prng_range(&c->r_timbre, 150, 210);
    uint32_t q = bar_ms / 16;
    if (!antic) {
        switch (pat) {
        case 0: hit(c, bus, 0, I_KEYS, deg, vel, bar_ms - 60); break;
        case 1: hit(c, bus, 0, I_KEYS, deg, vel, 6 * q - 40);
                hit(c, bus, 6, I_KEYS, deg, vel - 30, 10 * q - 60); break;
        case 2: hit(c, bus, 0, I_KEYS, deg, vel, 10 * q - 40);
                hit(c, bus, 10, I_KEYS, deg, vel - 25, 6 * q - 60); break;
        default: hit(c, bus, 0, I_KEYS, deg, vel, 2 * q);
                 hit(c, bus, 3, I_KEYS, deg, vel - 40, 2 * q);
                 hit(c, bus, 8, I_KEYS, deg, vel - 10, 8 * q - 60); break;
        }
    }
    /* push: strike the next chord on the "and" of 4 */
    if (next_deg != deg && prng_chance(&c->r_rhythm, 19661)) {
        hit(c, bus, 14, I_KEYS, next_deg, vel - 15, bar_ms - 60 + 2 * q);
        H.antic = 1;
    }
}

static void bass_line(comp_t *c, uint64_t bus, int deg, int next_deg, int sparse)
{
    int root = bass_root(c, deg);
    int vel = prng_range(&c->r_timbre, 170, 215);
    uint32_t beat = 60000u / H.bpm;
    hit(c, bus, 0, I_BASS, root, vel, beat * 3 / 2);
    if (sparse) return;
    if (prng_chance(&c->r_rhythm, 36044)) {
        static const int8_t iv[3] = {7, 12, 0};
        int p = root + iv[prng_below(&c->r_harm, 3)];
        hit(c, bus, prng_chance(&c->r_rhythm, 32768) ? 8 : 10, I_BASS, p, vel - 20, beat);
    }
    if (next_deg != deg && prng_chance(&c->r_rhythm, 22938)) {      /* approach note */
        int nr = bass_root(c, next_deg);
        int ap = prng_chance(&c->r_harm, 32768) ? nr - 1 : nr + (nr > root ? -2 : 2);
        hit(c, bus, 14, I_BASS, clampi(ap, 33, 52), vel - 35, beat / 2);
    }
}

static void lead(comp_t *c, uint64_t bus, int32_t dq8)
{
    uint32_t p = (uint32_t)clampi(4000 + (dq8 - 256) * 20, 2000, 26000);
    if (!prng_chance(&c->r_form, p)) return;
    int8_t pent[5];
    key_pentatonic(H.key, pent);
    int8_t set[24];
    int n = 0;
    for (int m = 64; m <= 83 && n < 24; m++)
        for (int i = 0; i < 5; i++) if (m % 12 == pent[i]) { set[n++] = (int8_t)m; break; }
    static const int8_t slots[11] = {0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14};
    int notes = prng_range(&c->r_rhythm, 1, 4);
    int s = (int)prng_below(&c->r_rhythm, 4);
    for (int k = 0; k < notes && s < 11; k++) {
        int idx = clampi((int)H.lead_idx + prng_range(&c->r_harm, -2, 2), 0, n - 1);
        H.lead_idx = (uint8_t)idx;
        hit(c, bus, slots[s], I_LEAD, set[idx], prng_range(&c->r_timbre, 120, 200), 1800);
        s += prng_range(&c->r_rhythm, 1, 3);
    }
}

static void bar_event(comp_t *c)
{
    if (H.sec_left == 0) next_section(c);
    uint64_t bus = H.bar_us;
    uint64_t bar_len_us = 240000000ull / H.bpm;
    uint32_t bar_ms = (uint32_t)(bar_len_us / 1000);
    int32_t dq8 = c_density_q8(c);
    int deg = chord_at(c, H.sec_bar);
    int next_deg = H.sec_left > 1 ? chord_at(c, H.sec_bar + 1) : deg;
    int antic = H.antic;
    H.antic = 0;

    keys_comp(c, bus, deg, next_deg, bar_ms, antic);
    if (H.section == S_GROOVE) {
        drums(c, bus, dq8);
        bass_line(c, bus, deg, next_deg, 0);
        lead(c, bus, dq8);
    } else if (H.section == S_BREAK) {
        bass_line(c, bus, deg, next_deg, 1);
        if (prng_chance(&c->r_rhythm, 32768))
            for (int s = 0; s < 16; s += 4) hit(c, bus, s, I_HAT, 0, prng_range(&c->r_timbre, 70, 100), 40);
    } else if (H.sec_bar >= 2) {
        bass_line(c, bus, deg, next_deg, 1);
    }

    H.bar_us += bar_len_us;
    H.bar++;
    H.sec_bar++;
    H.sec_left--;
    sched_push(&c->sched, H.t0 + H.bar_us / 1000, EV_H_BAR, 120, c->gen, 0, 0);
}

/* ---------------------------------------------------------------- instruments */

static int vel_amp(int vel, int32_t full) { return (int)(((int64_t)full * vel) / 255); }

static void play_keys(comp_t *c, int deg, int vel, uint32_t dur)
{
    if (deg != H.voiced_deg) {
        int8_t pcs[4];
        int r = deg_pc(c, deg, 0), nine = deg_pc(c, deg, 1);
        pcs[0] = (int8_t)deg_pc(c, deg, 2);                         /* 3rd */
        pcs[1] = (int8_t)deg_pc(c, deg, 6);                         /* 7th */
        pcs[2] = (int8_t)deg_pc(c, deg, 4);                         /* 5th */
        pcs[3] = (int8_t)(((nine - r + 12) % 12 == 2) ? nine : r);  /* 9th, or root */
        int32_t out[5];
        voice_lead(pcs, 4, 0, H.keys_p, H.voiced_deg < 0 ? 0 : 4, 4, 52, 71, 36, 48, out, &c->r_harm);
        for (int i = 0; i < 4; i++) H.keys_p[i] = out[1 + i];
        H.voiced_deg = deg;
    }
    for (int i = 0; i < 4; i++) if (live(c, H.keys_v[i])) voice_off(H.keys_v[i]);
    int32_t bq8 = c_bright_q8(c);
    int32_t hz = c_lerp_q8(KEYS_HZ, bq8);
    if (H.section == S_BREAK) hz = hz * 3 / 5;
    for (int i = 0; i < 4; i++) {
        int v = role_alloc(&H.keys_v[i], c->gen, 0, AP_KS_FIRST - 1, H_BUDGET);
        if (v < 0) continue;
        vparams_t q = {0};
        q.algo = ALGO_FM;                                          /* 1:1 FM electric piano */
        q.pinc0 = pitch_inc(H.keys_p[i] * 100);
        q.pinc1 = pitch_inc(H.keys_p[i] * 100 + 2);
        q.fm_index = (uint32_t)c_lerp_q8(KEYS_FM, bq8);
        q.fm_sus = 140;
        q.mod_decay = env_decay_ms(900);
        q.attack = env_attack_ms(4);
        q.decay = env_decay_ms(3500);
        q.sustain = 8000;
        q.release = env_release_ms(700);
        q.cutoff = svf_coef_hz((uint32_t)hz);
        q.reso = 21000;
        q.fmode = FMODE_LP;
        q.amp = (uint32_t)vel_amp(vel - prng_range(&c->r_timbre, 0, 20), 4800);
        q.pan = (uint32_t)(16384 + (i - 1) * 3500);
        q.send_rev = 10000;
        voice_setup(v, &q);
        voice_on(v, c->gen, H.keys_p[i] * 100, SEC(20), c->now);
        c_schedule_off(c, v, dur);
    }
}

static void play_hit(comp_t *c, uint32_t a, uint32_t dur)
{
    int inst = (int)(a >> 24), pitch = (int)((a >> 8) & 0xFF), vel = (int)(a & 0xFF);
    if (inst == I_KEYS) { play_keys(c, pitch, vel, dur); return; }

    unsigned lo = 0, hi = AP_KS_FIRST - 1, budget = H_BUDGET;
    if (inst == I_LEAD) { lo = AP_KS_FIRST; hi = AP_NUM_VOICES - 1; budget = AP_KS_SLOTS; }
    int v = inst == I_BASS ? role_alloc(&H.bass_v, c->gen, lo, hi, budget)
                           : voice_alloc(c->gen, lo, hi, budget);
    if (v < 0) return;
    int32_t bq8 = c_bright_q8(c);
    vparams_t q = {0};
    q.pan = 16384;
    q.reso = 32767;
    q.fmode = FMODE_BYPASS;
    q.attack = env_attack_ms(1);
    q.sustain = 0;
    int32_t trace_pitch = 0;                                       /* drums are unpitched */
    switch (inst) {
    case I_KICK:
        q.algo = ALGO_SUB;
        q.pinc0 = pitch_inc(3200);                                 /* ~52 Hz */
        q.penv = pitch_inc(5050) - pitch_inc(3200);                /* starts ~150 Hz */
        q.mod_decay = env_decay_ms(70);
        q.decay = env_decay_ms(380);
        q.release = env_release_ms(80);
        q.amp = (uint32_t)vel_amp(vel, 26000);
        q.send_rev = 1200;
        break;
    case I_SNARE:
        q.algo = ALGO_NOISE; q.noise = NOISE_PINK; q.fmode = FMODE_LP;    /* dusty: 12 dB/oct top */
        q.cutoff = svf_coef_hz(3200); q.reso = 14000;
        q.decay = env_decay_ms(200);
        q.release = env_release_ms(120);
        q.amp = (uint32_t)vel_amp(vel, 15000);
        q.pan = 15000;
        q.send_rev = 7000;
        break;
    case I_HAT:
    case I_OHAT:
        q.algo = ALGO_NOISE; q.noise = NOISE_PINK; q.fmode = FMODE_LP;    /* resonant LP "tss" */
        q.cutoff = svf_coef_hz((uint32_t)c_lerp_q8(HAT_HZ, bq8)); q.reso = 5000;
        q.decay = env_decay_ms(inst == I_OHAT ? 320 : 50);
        q.release = env_release_ms(inst == I_OHAT ? 200 : 40);
        q.amp = (uint32_t)vel_amp(vel, inst == I_OHAT ? 4500 : 5500);
        q.pan = 20000;
        q.send_rev = 2500;
        break;
    case I_BASS:
        q.algo = ALGO_SUB;
        q.pinc0 = pitch_inc(pitch * 100);
        q.pinc1 = pitch_inc(pitch * 100 + 1200);
        q.wave = 9000;
        q.attack = env_attack_ms(4);
        q.decay = env_decay_ms(900);
        q.sustain = 14000;
        q.release = env_release_ms(150);
        q.amp = (uint32_t)vel_amp(vel, 15000);
        q.send_rev = 1500;
        trace_pitch = pitch * 100;
        break;
    case I_LEAD:
        q.algo = ALGO_KS;
        q.ks_len = (uint32_t)(((1ull << 48) / pitch_inc(pitch * 100)) - 32768);
        q.ks_damp = (uint32_t)prng_range(&c->r_timbre, 32660, 32740);
        q.ks_bright = (uint32_t)c_lerp_q8(KS_BRT, bq8);
        q.attack = env_attack_ms(2);
        q.sustain = 32768;
        q.decay = env_decay_ms(1000);
        q.release = env_release_ms(1500);
        q.fmode = FMODE_LP; q.cutoff = svf_coef_hz(3800); q.reso = 23000;
        q.amp = (uint32_t)vel_amp(vel, 8000);
        q.pan = c_pan_rand(c, 9000);
        q.send_rev = 12000;
        trace_pitch = pitch * 100;
        hal_visual_event(VIS_NOTE((uint32_t)pitch, q.pan));
        break;
    default: return;
    }
    voice_setup(v, &q);
    voice_on(v, c->gen, trace_pitch, SEC(15), c->now);
    c_schedule_off(c, v, dur);
}

/* --------------------------------------------------------------------- mode */

static void hush_start(comp_t *c)
{
    H.key = c->key;
    H.bpm = (uint32_t)prng_range(&c->r_form, 72, 86);
    H.swing_q15 = (uint32_t)prng_range(&c->r_rhythm, 17400, 20000);   /* 0.53 .. 0.61 */
    H.t0 = c->now + 300;
    H.bar_us = 0;
    H.bar = 0;
    H.section = S_INTRO;
    H.sec_left = 4;
    H.sec_bar = 0;
    H.kick_pat = 0;
    H.antic = 0;
    H.pending_key = 0;
    H.lead_idx = 6;
    H.voiced_deg = -1;
    H.bass_v = H.rain_v = H.crackle_v = -1;
    for (int i = 0; i < 4; i++) H.keys_v[i] = -1;
    H.wow_inc = 0xFFFFFFFFu / (uint32_t)prng_range(&c->r_timbre, 3000, 5500);
    H.wow_ph = prng_next(&c->r_timbre);
    gen_prog(c);
    beds_start(c, 4000);
    sched_push(&c->sched, H.t0, EV_H_BAR, 120, c->gen, 0, 0);
}

static void hush_tick(comp_t *c)
{
    if (c->now % 20 != 11) return;
    /* tape wow: +/- 5 cents on the keys */
    int32_t w = fx_sin_q15(H.wow_ph + (uint32_t)(c->now * H.wow_inc));
    int32_t cents = (w * 5) >> 15;
    for (int i = 0; i < 4; i++) {
        int v = H.keys_v[i];
        if (v < 0 || g_vinfo[v].owner != c->gen) continue;
        hal_vwrite((unsigned)v, VR_PINC0, pitch_inc(H.keys_p[i] * 100 + cents));
        hal_vwrite((unsigned)v, VR_PINC1, pitch_inc(H.keys_p[i] * 100 + cents + 2));
    }
}

static void hush_event(comp_t *c, const ev_t *e)
{
    switch (e->type) {
    case EV_H_BAR: bar_event(c); break;
    case EV_H_HIT: play_hit(c, e->a, e->b); break;
    case EV_KEY:   H.pending_key = 1; break;              /* applied at the next breakdown */
    default: break;
    }
}

static void hush_fx(const comp_t *c, fxset_t *f)
{
    (void)c;
    f->t60_ms = 2400; f->rev_wet = 8000; f->rev_damp = 24000; f->predelay = 480;
    f->echo_fb = 0; f->echo_wet = 0; f->echo_damp = 30000; f->echo_to_rev = 0;
    f->trim = TRIM;
    f->dry = 32768;
}

const mode_ops_t MODE_HUSH_OPS = {"HUSH", hush_start, hush_tick, hush_event, hush_fx};
