/*
 * mode_tides.c - TIDES: Blade Runner / CS-80 style, in two layers.
 *
 *  1. Voice layer (the synth engine): dual detuned saws -> resonant SVF whose
 *     cutoff follows the amp envelope and slews slowly to targets, exponential
 *     swells, delayed-onset vibrato, glide, and a mostly-wet long reverb.
 *  2. Conductor (the "Vangelis brain"): every 125 ms it emits tev_t events
 *     {voice, note, gate, cutoff target, slew, glide}; the engine just slews
 *     towards whatever it is told. Minor/Dorian harmony with chords every
 *     4-10 s, voice-led by glide, filter phrasing per chord, a sparse melodic
 *     random walk with rests and pitch "sighs", rare bells and rarer thunder.
 */
#include "cint.h"

static int iabs(int x) { return x < 0 ? -x : x; }   /* freestanding: no <stdlib.h> */

#define T (c->m.t)
#define T_BUDGET   14
#define TRIM       11767             /* loudness calibration to -20 LUFS */
#define CTICK_MS   125               /* conductor period */
#define NOCHG      (-1000000)

enum { G_HOLD, G_ON, G_OFF };

/* conductor -> engine event (the internal "MIDI") */
typedef struct {
    int      voice;
    int32_t  cents;        /* osc-0 pitch, or NOCHG */
    int32_t  detune;       /* osc-1 offset, cents */
    int8_t   gate;         /* G_HOLD, G_ON (trigger / legato retrigger), G_OFF */
    uint32_t cutoff_hz;    /* 0 = unchanged */
    uint32_t slew_ms;      /* cutoff (and amp) slew towards the new target */
    uint32_t glide_ms;     /* 0 = jump */
    uint32_t max_len_ms;
} tev_t;

static const int32_t CUT_CLOSED[5] = {220, 300, 380, 480, 620};
static const int32_t CUT_OPEN[5]   = {900, 1300, 1800, 2500, 3400};
static const int32_t TRI_AMT[5]    = {20000, 14000, 9000, 4500, 0};
static const int32_t FENV_AMT[5]   = {2500, 4000, 5500, 7500, 9500};
static const int32_t CHORD_S[5]    = {9, 8, 7, 6, 5};          /* mean chord length, s */
static const int32_t REST_T[5]     = {120, 90, 64, 48, 32};    /* lead rest, conductor ticks */
static const int32_t BELL_S[5]     = {90, 65, 45, 32, 22};
static const int32_t THUNDER_S[5]  = {480, 400, 320, 260, 200};

static uint16_t slew_code(uint32_t ms) { return ms ? env_rate_tau(ms * 16) : 0; }  /* ~95% in ms */
static int live(comp_t *c, int v) { return v >= 0 && g_vinfo[v].owner == c->gen; }
static int gated(comp_t *c, int v) { return live(c, v) && g_vinfo[v].gated; }

/* ----------------------------------------------------------- voice layer */

static void engine(comp_t *c, const tev_t *e)
{
    unsigned v = (unsigned)e->voice;
    hal_vwrite(v, VR_GLIDE, slew_code(e->glide_ms));
    if (e->cents != NOCHG) {
        hal_vwrite(v, VR_PINC0, pitch_inc(e->cents));
        hal_vwrite(v, VR_PINC1, pitch_inc(e->cents + e->detune));
    }
    if (e->cutoff_hz) {
        hal_vwrite(v, VR_SLEW, slew_code(e->slew_ms));
        hal_vwrite(v, VR_CUTOFF, svf_coef_hz(e->cutoff_hz));
    }
    if (e->gate == G_ON) {
        voice_off(e->voice);              /* legato retrigger: the envelope continues from its level */
        voice_on(e->voice, c->gen, e->cents == NOCHG ? g_vinfo[v].pitch : e->cents,
                 e->max_len_ms, c->now);
    } else if (e->gate == G_OFF) {
        voice_off(e->voice);
    }
}

static int32_t cut_closed(comp_t *c) { return c_lerp_q8(CUT_CLOSED, c_bright_q8(c)); }
static int32_t cut_open(comp_t *c)
{
    return mod_q15(c_lerp_q8(CUT_OPEN, c_bright_q8(c)), c->wx.out[W_TENSION], 8000);
}

static void synth_patch(comp_t *c, vparams_t *p, int lead)
{
    int32_t bq8 = c_bright_q8(c);
    p->algo = ALGO_PAD;
    p->wave = (uint32_t)c_lerp_q8(TRI_AMT, bq8);
    p->attack = env_attack_ms((uint32_t)(lead ? prng_range(&c->r_timbre, 120, 300)
                                              : prng_range(&c->r_timbre, 300, 1000)));
    p->decay = env_decay_ms(1000);
    p->sustain = 32768;
    p->release = env_release_ms((uint32_t)prng_range(&c->r_timbre, 2000, 4000));
    p->cutoff = svf_coef_hz((uint32_t)cut_closed(c));
    p->reso = lead ? 21000 : 19000;
    p->fmode = FMODE_LP;
    p->fenv = (uint32_t)c_lerp_q8(FENV_AMT, bq8);
    p->vib_rate = (uint32_t)prng_range(&c->r_timbre, 4500, 5800) * 89478u / 1000u;   /* mHz -> inc */
    p->vib_depth = (uint32_t)(lead ? prng_range(&c->r_timbre, 500, 830) : prng_range(&c->r_timbre, 280, 500));
    p->vib_delay = env_rate_tau(lead ? 28800 : 48000);       /* depth ramps in over ~0.6 / 1 s */
    p->pan = c_pan_rand(c, lead ? 4000 : 12000);
    p->send_rev = lead ? 24000 : 27000;
    p->send_echo = lead ? 6000 : 0;
}

/* make sure pad i owns an engine voice */
static int pad_voice(comp_t *c, int i)
{
    if (!live(c, T.pad_v[i])) {
        if (role_alloc(&T.pad_v[i], c->gen, 0, AP_KS_FIRST - 1, T_BUDGET) < 0) return -1;
        T.pad_det[i] = prng_range(&c->r_timbre, 3, 9) * (prng_chance(&c->r_timbre, 32768) ? 1 : -1);
        T.drift_left[i] = (int16_t)prng_range(&c->r_rhythm, 16, 40);
        vparams_t p = {0};
        synth_patch(c, &p, 0);
        p.pinc0 = pitch_inc(T.pad_p[i] * 100);
        p.pinc1 = pitch_inc(T.pad_p[i] * 100 + T.pad_det[i]);
        p.amp = (uint32_t)(i == 0 ? 6500 : 22000 / T.npad);
        voice_setup(T.pad_v[i], &p);
    }
    return T.pad_v[i];
}

/* ------------------------------------------------------------- conductor */

static void new_chord(comp_t *c, int first)
{
    T.chord = first ? chord_home(T.key, -1, &c->r_harm)
                    : chord_next(T.key, T.chord, c->wx.out[W_TENSION], &c->r_harm);
    int8_t pcs[4];
    int npc = chord_pcs(T.key, CHORD_VOCAB[T.chord], pcs);
    int32_t out[T_PADS + 1];
    voice_lead(pcs, npc, first ? 0 : T.pad_p[0], T.pad_p + 1, first ? 0 : T.npad - 1,
               T.npad - 1, 55, 74, 41, 53, out, &c->r_harm);
    int fresh = first || T.breathing;
    for (int i = 0; i < T.npad; i++) T.pad_p[i] = out[i];
    for (int i = 0; i < T.npad; i++) {
        int v = pad_voice(c, i);
        if (v < 0) continue;
        hal_vwrite((unsigned)v, VR_ATTACK, env_attack_ms((uint32_t)prng_range(&c->r_timbre, 300, 1000)));
        /* voice-leading by glide: each voice slides to its nearest chord tone */
        tev_t e = {v, T.pad_p[i] * 100, T.pad_det[i], G_ON, (uint32_t)cut_closed(c), 300,
                   fresh ? 0 : (uint32_t)prng_range(&c->r_rhythm, 250, 600), SEC(60)};
        engine(c, &e);
    }
    T.breathing = 0;
    T.open_pending = 1;
    int32_t mean = c_lerp_q8(CHORD_S, c_density_q8(c)) * 8;          /* 4-10 s */
    T.chord_left = (int16_t)clampi(mean + prng_range(&c->r_rhythm, -16, 16), 32, 80);
    hal_trace(TR_CHORD, (uint32_t)T.chord, (uint32_t)npc, 0);
    hal_visual_event(VIS_CHORD((uint32_t)T.chord));
}

static void pads_tick(comp_t *c)
{
    if (T.breathing) {
        if (--T.breath_left <= 0) new_chord(c, 0);
        return;
    }
    if (T.open_pending) {
        /* phrasing: sweep the filter open over ~60% of the chord ... */
        T.open_pending = 0;
        uint32_t slew = (uint32_t)T.chord_left * CTICK_MS * 6 / 10;
        for (int i = 0; i < T.npad; i++)
            if (gated(c, T.pad_v[i])) {
                tev_t e = {T.pad_v[i], NOCHG, 0, G_HOLD,
                           (uint32_t)mod_q15(cut_open(c), prng_range(&c->r_timbre, -32768, 32767), 5000),
                           slew, 0, 0};
                engine(c, &e);
            }
    }
    T.chord_left--;
    for (int i = 0; i < T.npad; i++) {
        int v = T.pad_v[i];
        if (!gated(c, v)) continue;
        if (T.chord_left == 16) {                     /* ... and let it close over the last 2 s */
            tev_t e = {v, NOCHG, 0, G_HOLD, (uint32_t)cut_closed(c), 2200, 0, 0};
            engine(c, &e);
        }
        if (--T.drift_left[i] <= 0) {                 /* analog drift on the detune */
            T.drift_left[i] = (int16_t)prng_range(&c->r_rhythm, 16, 40);
            T.pad_det[i] = clampi(T.pad_det[i] + prng_range(&c->r_timbre, -3, 3), -10, 10);
            if (T.pad_det[i] > -2 && T.pad_det[i] < 2) T.pad_det[i] = 3;
            tev_t e = {v, T.pad_p[i] * 100, T.pad_det[i], G_HOLD, 0, 0, 1500, 0};
            engine(c, &e);
        }
    }
    if (T.chord_left > 0) return;
    /* end of chord: sometimes breathe (release, rest 2-6 s), otherwise move on legato */
    uint32_t p_breath = (uint32_t)clampi(22000 - (c_density_q8(c) - 256) * 10, 6000, 22000);
    if (prng_chance(&c->r_form, p_breath)) {
        T.breathing = 1;
        T.breath_left = (int16_t)prng_range(&c->r_rhythm, 16, 48);
        for (int i = 0; i < T.npad; i++)
            if (gated(c, T.pad_v[i])) {
                tev_t e = {T.pad_v[i], NOCHG, 0, G_OFF, (uint32_t)cut_closed(c) * 3 / 4, 3000, 0, 0};
                engine(c, &e);
            }
    } else {
        new_chord(c, 0);
    }
}

/* --------------------------------------------------------------- melody */

static int scale_notes(comp_t *c, int32_t *out, int lo, int hi)
{
    int n = 0;
    for (int m = lo; m <= hi; m++) if (key_has_pc(T.key, m)) out[n++] = m;
    return n;
}

static void lead_tick(comp_t *c)
{
    if (T.lead_rest > 0) { T.lead_rest--; return; }
    if (T.lead_left > 0) { T.lead_left--; return; }

    int32_t sc[24];
    int n = scale_notes(c, sc, 60, 81);
    if (T.phrase_left == 0) {
        if (T.lead_p) {
            /* phrase over: release; sometimes the ribbon "sigh" - a glide down as it fades */
            if (live(c, T.lead_v)) {
                int sigh = prng_chance(&c->r_form, 19661);
                tev_t e = {T.lead_v, sigh ? T.lead_p * 100 - prng_range(&c->r_harm, 200, 500) : NOCHG,
                           5, G_OFF, (uint32_t)cut_closed(c), 2500,
                           sigh ? (uint32_t)prng_range(&c->r_rhythm, 900, 1600) : 0, 0};
                engine(c, &e);
            }
            T.lead_p = 0;
            int32_t rest = c_lerp_q8(REST_T, c_density_q8(c));
            T.lead_rest = (int16_t)(rest / 2 + (int32_t)prng_below(&c->r_rhythm, (uint32_t)rest));
            return;
        }
        T.phrase_left = (uint8_t)prng_range(&c->r_form, 2, 6);
        if (!live(c, T.lead_v) || g_vinfo[T.lead_v].gated) {
            if (role_alloc(&T.lead_v, c->gen, 0, AP_KS_FIRST - 1, T_BUDGET) < 0) {
                T.phrase_left = 0; T.lead_rest = 16; return;
            }
        }
        vparams_t p = {0};
        synth_patch(c, &p, 1);
        p.amp = 7600;
        p.pinc0 = pitch_inc(sc[T.lead_deg] * 100);
        p.pinc1 = pitch_inc(sc[T.lead_deg] * 100 + 5);
        voice_setup(T.lead_v, &p);
    }

    /* weighted random walk: mostly steps, sometimes 4ths and 5ths */
    static const int8_t mv[9] = {0, 1, -1, 2, -2, 3, -3, 4, -4};
    static const uint32_t mw[9] = {4, 26, 26, 9, 9, 7, 7, 5, 5};
    int d = clampi(T.lead_deg, 0, n - 1) + mv[prng_weighted(&c->r_harm, mw, 9)];
    if (d < 0) d = -d;
    if (d >= n) d = 2 * (n - 1) - d;
    T.lead_deg = (int8_t)clampi(d, 0, n - 1);
    static const int16_t dur[6] = {4, 6, 8, 12, 16, 24};
    static const uint32_t dw[6] = {3, 3, 4, 3, 2, 1};
    int len = dur[prng_weighted(&c->r_rhythm, dw, 6)];
    int32_t pitch = sc[T.lead_deg];
    if (len >= 12) {                                      /* long notes land on chord tones */
        int8_t pcs[4];
        int npc = chord_pcs(T.key, CHORD_VOCAB[T.chord], pcs);
        int best = pitch, bd = 99;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < npc; k++)
                if (sc[i] % 12 == pcs[k] && iabs(sc[i] - pitch) < bd) { bd = iabs(sc[i] - pitch); best = sc[i]; }
        pitch = best;
    }
    int first = T.lead_p == 0;
    T.lead_p = pitch;
    tev_t e = {T.lead_v, pitch * 100, 5, G_ON, (uint32_t)cut_open(c) * 4 / 5, 400,
               first ? 0 : (uint32_t)prng_range(&c->r_rhythm, 90, 160), SEC(30)};
    engine(c, &e);
    hal_visual_event(VIS_NOTE((uint32_t)pitch, 16384));
    T.lead_left = (int16_t)len;
    T.phrase_left--;
}

/* --------------------------------------------------------- bells & thunder */

static void bell(comp_t *c)
{
    int8_t pcs[4];
    int npc = chord_pcs(T.key, CHORD_VOCAB[T.chord], pcs);
    int pc = pcs[prng_below(&c->r_harm, (uint32_t)npc)];
    int note = 79 + (pc - 7 + 12) % 12 + (prng_chance(&c->r_harm, 26214) ? 12 : 0);   /* 79 = G5 */
    if (note > 93) note -= 12;
    int v = voice_alloc(c->gen, 0, AP_KS_FIRST - 1, T_BUDGET);
    if (v < 0) return;
    vparams_t q = {0};
    q.algo = ALGO_FM;
    q.pinc0 = pitch_inc(note * 100);
    int fm = prng_chance(&c->r_timbre, 32768);            /* 2-op FM bell, or a plain sine ping */
    q.pinc1 = fm ? inc_mul_ratio(q.pinc0, 7, 2) : q.pinc0;
    q.fm_index = fm ? 700 : 0;
    q.fm_sus = fm ? 60 : 0;
    q.mod_decay = env_decay_ms(600);
    q.attack = env_attack_ms(2);
    q.decay = env_decay_ms((uint32_t)prng_range(&c->r_timbre, 1200, 2000));
    q.sustain = 0;
    q.release = env_release_ms(1500);
    q.fmode = FMODE_LP; q.cutoff = svf_coef_hz(7000); q.reso = 26000;
    q.amp = 3600;
    q.pan = c_pan_rand(c, 14000);
    q.send_rev = 26000;
    q.send_echo = 12000;
    voice_setup(v, &q);
    voice_on(v, c->gen, note * 100, SEC(10), c->now);
    c_schedule_off(c, v, 200);
    hal_visual_event(VIS_NOTE((uint32_t)note, q.pan));
}

static void thunder(comp_t *c)
{
    int v = voice_alloc(c->gen, 0, AP_KS_FIRST - 1, T_BUDGET);
    if (v < 0) return;
    vparams_t q = {0};
    q.algo = ALGO_NOISE; q.noise = NOISE_PINK;
    q.fmode = FMODE_LP; q.cutoff = svf_coef_hz(420); q.reso = 26000;
    q.attack = env_attack_ms((uint32_t)prng_range(&c->r_timbre, 500, 900));
    q.decay = env_decay_ms(9000);
    q.sustain = 0;
    q.release = env_release_ms(4000);
    q.amp = 15000;
    q.pan = c_pan_rand(c, 9000);
    q.send_rev = 20000;
    voice_setup(v, &q);
    voice_on(v, c->gen, 0, SEC(20), c->now);
    c_schedule_off(c, v, 1500);
    T.thunder_v = v;                                      /* next conductor tick: roll away darker */
}

/* ------------------------------------------------------------------- mode */

static hkey_t minor_view(hkey_t k)
{
    if (k.centre != 1 && k.centre != 5) k.centre = 5;     /* Aeolian, or keep Dorian */
    return k;
}

static void tides_start(comp_t *c)
{
    T.key = minor_view(c->key);
    T.npad = c->density >= 4 ? 5 : 4;
    for (int i = 0; i < T_PADS; i++) { T.pad_v[i] = -1; T.pad_p[i] = 0; }
    T.lead_v = -1;
    T.thunder_v = -1;
    T.lead_p = 0;
    T.phrase_left = 0;
    T.lead_left = 0;
    T.lead_rest = (int16_t)prng_range(&c->r_rhythm, 40, 100);
    T.breathing = 0;
    T.ctick = 0;
    T.bell_left = (int32_t)prng_exp(&c->r_rhythm, (uint32_t)c_lerp_q8(BELL_S, c_density_q8(c)) * 8) + 80;
    T.thunder_left = (int32_t)prng_exp(&c->r_form, (uint32_t)c_lerp_q8(THUNDER_S, c_density_q8(c)) * 8) + 960;
    int32_t sc[24];
    T.lead_deg = (int8_t)(scale_notes(c, sc, 60, 81) / 2);
    T.chord = -1;
    new_chord(c, 1);
}

static void tides_tick(comp_t *c)
{
    if (c->now % CTICK_MS != 5) return;
    T.ctick++;
    if (T.thunder_v >= 0) {
        if (live(c, T.thunder_v)) {
            tev_t e = {T.thunder_v, NOCHG, 0, G_HOLD, 90, 6000, 0, 0};
            engine(c, &e);
        }
        T.thunder_v = -1;
    }
    pads_tick(c);
    lead_tick(c);
    if (--T.bell_left <= 0) {
        bell(c);
        T.bell_left = (int32_t)prng_exp(&c->r_rhythm, (uint32_t)c_lerp_q8(BELL_S, c_density_q8(c)) * 8) + 24;
    }
    if (--T.thunder_left <= 0) {
        thunder(c);
        T.thunder_left = (int32_t)prng_exp(&c->r_form, (uint32_t)c_lerp_q8(THUNDER_S, c_density_q8(c)) * 8) + 960;
    }
}

static void tides_event(comp_t *c, const ev_t *e)
{
    if (e->type == EV_KEY) T.key = minor_view(c->key);   /* voice-leading glides into the new key */
}

static void tides_fx(const comp_t *c, fxset_t *f)
{
    (void)c;
    f->t60_ms = 10000; f->rev_wet = 27000; f->rev_damp = 28000; f->predelay = 1440;
    f->echo_fb = 10000; f->echo_wet = 6000; f->echo_damp = 22000; f->echo_to_rev = 9000;
    f->trim = TRIM;
    f->dry = 17000;
}

const mode_ops_t MODE_TIDES_OPS = {"TIDES", tides_start, tides_tick, tides_event, tides_fx};
