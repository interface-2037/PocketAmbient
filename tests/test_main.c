/*
 * test_main.c - host unit + system tests (spec section 8.2).   make test
 * Pass --long to include the slow system tests (sleep timer, overflow soak).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../model/synth_model.h"
#include "../fw/hal/hal_model.h"
#include "../fw/composer/composer.h"
#include "../fw/composer/cint.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define TEST(name) static void name(void)
#define RUN(name) do { int f0 = g_fail; name(); printf("%-40s %s\n", #name, g_fail == f0 ? "ok" : "FAILED"); } while (0)

static sm_t S;

/* ------------------------------------------------------------------ PRNG */

TEST(test_prng_kat)
{
    prng_t p = {{1, 2, 3, 4}};
    uint32_t exp[4] = {11520, 0, 5927040, 70819200};   /* reference algorithm, state {1,2,3,4} */
    for (int i = 0; i < 4; i++) CHECK(prng_next(&p) == exp[i], "xoshiro128** KAT %d", i);
}

TEST(test_prng_streams)
{
    prng_t a, b, z;
    prng_seed(&a, 42, ST_HARMONY);
    prng_seed(&b, 42, ST_RHYTHM);
    int same = 0;
    for (int i = 0; i < 64; i++) same += prng_next(&a) == prng_next(&b);
    CHECK(same < 2, "streams with the same seed must differ");
    prng_seed(&z, 0, 0);
    CHECK(z.s[0] | z.s[1] | z.s[2] | z.s[3], "seed 0 must not give an all-zero state");
    uint32_t x = prng_next(&z), y = prng_next(&z);
    CHECK(x != y, "seed 0 stream must advance");
    /* range helpers stay in range */
    for (int i = 0; i < 10000; i++) {
        int32_t r = prng_range(&a, -3, 7);
        if (r < -3 || r > 7) { CHECK(0, "prng_range out of range %d", r); break; }
    }
    uint64_t sum = 0;
    for (int i = 0; i < 20000; i++) sum += prng_exp(&a, 1000);
    CHECK(sum / 20000 > 900 && sum / 20000 < 1100, "exponential mean %llu", (unsigned long long)(sum / 20000));
}

/* ------------------------------------------------------------- scheduler */

static void sched_order_at(uint64_t base)
{
    static sched_t s;
    sched_init(&s);
    sched_push(&s, base + 30, 1, 1, 1, 30, 0);
    sched_push(&s, base + 10, 1, 1, 1, 10, 0);
    sched_push(&s, base + 20, 1, 1, 1, 20, 0);
    sched_push(&s, base + 10, 1, 1, 1, 11, 0);      /* tie: FIFO after the first 10 */
    ev_t e;
    CHECK(!sched_pop_due(&s, base + 9, &e), "nothing due early (base %llu)", (unsigned long long)base);
    uint32_t want[4] = {10, 11, 20, 30};
    for (int i = 0; i < 4; i++) {
        int ok = sched_pop_due(&s, base + 100, &e);
        CHECK(ok && e.a == want[i], "order %d at base %llu: got %u", i, (unsigned long long)base, e.a);
    }
}

TEST(test_sched)
{
    sched_order_at(0);
    sched_order_at((1ull << 32) - 15);              /* realistic: crosses 2^32 */
    sched_order_at(UINT64_MAX - 15);                /* paranoia: crosses 2^64 */

    static sched_t s;
    sched_init(&s);
    for (int i = 0; i < SCHED_CAP; i++) sched_push(&s, (uint64_t)i, 1, 50, 1, (uint32_t)i, 0);
    CHECK(sched_push(&s, 5, 1, 10, 1, 999, 0) == SCHED_DROPPED, "low prio dropped when full");
    CHECK(sched_push(&s, 5, 1, 255, 1, 1000, 0) == SCHED_REPLACED, "high prio replaces when full");
    CHECK(sched_count(&s) == SCHED_CAP, "count stays at capacity");
    int found = 0;
    ev_t e;
    uint64_t last = 0;
    int sorted = 1;
    while (sched_pop_due(&s, 1000, &e)) { found |= e.a == 1000; if (e.when < last) sorted = 0; last = e.when; }
    CHECK(found && sorted, "replacement present and heap order intact");

    sched_init(&s);
    for (int i = 0; i < 100; i++) sched_push(&s, (uint64_t)i, 1, 1, (uint8_t)(i % 3), (uint32_t)i, 0);
    sched_cancel_owner(&s, 1);
    int bad = 0;
    while (sched_pop_due(&s, 1000, &e)) bad |= e.owner == 1;
    CHECK(!bad, "cancel_owner removes all of that owner's events");
}

/* --------------------------------------------------------------- harmony */

TEST(test_harmony)
{
    prng_t r;
    prng_seed(&r, 7, 1);
    for (int t = 0; t < 12; t++) {
        static const int8_t cs[5] = {0, 1, 3, 4, 5};
        for (int ci = 0; ci < 5; ci++) {
            hkey_t k = {(int8_t)t, cs[ci]};
            for (int j = 0; j < CHORD_VOCAB_N; j++) {
                int8_t pcs[4];
                int n = chord_pcs(k, CHORD_VOCAB[j], pcs);
                for (int i = 0; i < n; i++) CHECK(key_has_pc(k, pcs[i]), "chord tone out of key");
            }
            int8_t pent[5];
            key_pentatonic(k, pent);
            for (int i = 0; i < 5; i++) CHECK(key_has_pc(k, pent[i]), "pentatonic tone out of key");
            int cur = chord_home(k, -1, &r);
            for (int i = 0; i < 50; i++) {
                cur = chord_next(k, cur, (int32_t)prng_range(&r, -32768, 32767), &r);
                CHECK(chord_valid(k, CHORD_VOCAB[cur]), "walk produced an invalid chord");
            }
            for (int i = 0; i < 20; i++) {
                hkey_t n2 = key_neighbor(k, &r);
                CHECK(key_is_neighbor(k, n2), "key change to a non-neighbour");
            }
        }
    }
    /* voice-leading: chord tones only, max jump bounded */
    hkey_t k = {0, 0};
    int32_t prev[5], out[6];
    int8_t pcs[4];
    int cur = 0;
    int n = chord_pcs(k, CHORD_VOCAB[cur], pcs);
    voice_lead(pcs, n, 0, prev, 0, 5, 52, 76, 36, 50, out, &r);
    long moves = 0, over = 0;
    for (int it = 0; it < 20000; it++) {
        for (int i = 0; i < 5; i++) prev[i] = out[1 + i];
        int bass = out[0];
        cur = chord_next(k, cur, 0, &r);
        n = chord_pcs(k, CHORD_VOCAB[cur], pcs);
        voice_lead(pcs, n, bass, prev, 5, 5, 52, 76, 36, 50, out, &r);
        for (int i = 0; i < 6; i++) {
            int ok = 0;
            for (int j = 0; j < n; j++) ok |= out[i] % 12 == pcs[j];
            CHECK(ok, "voiced note not a chord tone");
        }
        for (int i = 0; i < 5; i++) {
            int d = out[1 + i] - prev[i];
            if (d < 0) d = -d;
            moves++;
            over += d > VL_MAX_JUMP;
            CHECK(d <= VL_HARD_MAX, "voice jump %d", d);
            CHECK(out[1 + i] >= 52 - VL_HEADROOM && out[1 + i] <= 76 + VL_HEADROOM, "voice out of range");
        }
        for (int i = 2; i < 6; i++) CHECK(out[i] != out[i - 1], "unison in voicing");
        CHECK(out[0] % 12 == pcs[0], "bass must be the root");
    }
    CHECK(over * 1000 < moves, "%ld of %ld moves exceed %d semitones", over, moves, VL_MAX_JUMP);
}

/* -------------------------------------------------------------- fixed point */

TEST(test_fxp)
{
    CHECK(fx_exp2_q16(0) == 65536, "2^0");
    CHECK(fx_exp2_q16(65536) == 131072, "2^1");
    CHECK(fx_exp2_q16(-65536) == 32768, "2^-1");
    uint32_t h = fx_exp2_q16(32768);               /* sqrt 2 */
    CHECK(h > 92680 && h < 92690, "2^0.5 = %u", h);
    CHECK(fx_log2_q16(1024) == 10 << 16, "log2 1024");
    int32_t l = fx_log2_q16(3);
    CHECK(l > 103870 && l < 103880, "log2 3 = %d", l);
    uint32_t a440 = pitch_hz(6900);
    CHECK(a440 == 439 || a440 == 440, "A4 = %u Hz", a440);
    int64_t oct = (int64_t)(pitch_inc(6900 + 1200) / 2) - (int64_t)pitch_inc(6900);
    CHECK(oct > -4 && oct < 4, "octave doubles inc (%lld)", (long long)oct);
    /* model limiter: monotonic, never above the ceiling */
    int32_t prev = 0;
    for (int32_t x = 0; x < (1 << 26); x += 997) {
        int32_t y = sm_limit(x);
        CHECK(y >= prev, "limiter monotonic at %d", x);
        if (y < prev) break;
        prev = y;
    }
    CHECK(prev < 7466000, "limiter ceiling (-1 dBFS) %d", prev);
    CHECK(sm_limit(-(1 << 26)) == -sm_limit(1 << 26), "limiter symmetric");
    /* rate code encoding is monotonic in time */
    CHECK(env_release_ms(100) > env_release_ms(1000) && env_release_ms(1000) > env_release_ms(60000),
          "longer time -> slower rate");
}

/* -------------------------------------------------------- engine: filter */

static void voice_basic(unsigned v, uint32_t algo)
{
    sm_vwrite(&S, v, VR_ALGO, algo);
    sm_vwrite(&S, v, VR_PINC0, pitch_inc(5700));
    sm_vwrite(&S, v, VR_PINC1, pitch_inc(5705));
    sm_vwrite(&S, v, VR_ATTACK, env_attack_ms(1));
    sm_vwrite(&S, v, VR_DECAY, env_decay_ms(100));
    sm_vwrite(&S, v, VR_SUSTAIN, 32768);
    sm_vwrite(&S, v, VR_RELEASE, env_release_ms(100));
    sm_vwrite(&S, v, VR_AMP, 16384);
    sm_vwrite(&S, v, VR_PAN, 16384);
    sm_vwrite(&S, v, VR_RESO, 23170);
    sm_vwrite(&S, v, VR_CUTOFF, svf_coef_hz(2000));
}

TEST(test_svf_stability)
{
    static const uint32_t fs[] = {1, 100, 1000, 10000, 30000, 52429, 65535};
    static const uint32_t qs[] = {0, 1024, 4096, 16384, 32767, 65535};
    int16_t l, r;
    for (unsigned a = 0; a < sizeof fs / sizeof fs[0]; a++)
        for (unsigned b = 0; b < sizeof qs / sizeof qs[0]; b++) {
            sm_init(&S);
            voice_basic(0, ALGO_PAD);
            sm_vwrite(&S, 0, VR_CUTOFF, fs[a]);
            sm_vwrite(&S, 0, VR_RESO, qs[b]);
            sm_vwrite(&S, 0, VR_FMODE, FMODE_BP);
            sm_vwrite(&S, 0, VR_GATE, GATE_ON | GATE_TRIG);
            for (int i = 0; i < 24000; i++) sm_process(&S, &l, &r);
            sm_vwrite(&S, 0, VR_ALGO, 7);          /* no source: input is now 0 */
            for (int i = 0; i < 96000; i++) sm_process(&S, &l, &r);
            int32_t lo = S.v[0].svf_low, bd = S.v[0].svf_band;
            if (lo < 0) lo = -lo;
            if (bd < 0) bd = -bd;
            /* very low cutoffs ring down slowly; everything else must have decayed */
            int32_t lim = fs[a] <= 100 ? (1 << 22) : (1 << 12);
            CHECK(lo < lim && bd < lim, "SVF f=%u q=%u did not decay (low %d band %d)",
                  fs[a], qs[b], lo, bd);
        }
}

/* ----------------------------------------------------- engine: envelopes */

static int env_measure_attack(uint32_t ms)
{
    int16_t l, r;
    sm_init(&S);
    voice_basic(0, ALGO_SUB);
    sm_vwrite(&S, 0, VR_ATTACK, env_attack_ms(ms));
    sm_vwrite(&S, 0, VR_GATE, GATE_ON | GATE_TRIG);
    int n = 0;
    while (S.v[0].env_state == ENV_IDLE || S.v[0].env_state == ENV_ATTACK) {
        sm_process(&S, &l, &r);
        if (++n > 48 * 200000) break;
    }
    return n;
}

static int env_measure_release(uint32_t ms)
{
    int16_t l, r;
    sm_init(&S);
    voice_basic(0, ALGO_SUB);
    sm_vwrite(&S, 0, VR_RELEASE, env_release_ms(ms));
    sm_vwrite(&S, 0, VR_GATE, GATE_ON | GATE_TRIG);
    for (int i = 0; i < 4800; i++) sm_process(&S, &l, &r);
    sm_vwrite(&S, 0, VR_GATE, 0);
    int n = 0;
    while (S.v[0].env_level > (1 << 30) / 1000) {      /* -60 dB */
        sm_process(&S, &l, &r);
        if (++n > 48 * 200000) break;
    }
    return n;
}

TEST(test_envelopes)
{
    static const uint32_t att[] = {5, 100, 1000, 6000, 30000};
    for (unsigned i = 0; i < sizeof att / sizeof att[0]; i++) {
        int n = env_measure_attack(att[i]), want = (int)att[i] * 48;
        int err = n > want ? n - want : want - n;
        CHECK(err * 20 <= want + 48, "attack %u ms took %.1f ms", att[i], n / 48.0);
    }
    static const uint32_t rel[] = {100, 1000, 15000, 60000};
    for (unsigned i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        int n = env_measure_release(rel[i]), want = (int)rel[i] * 48;
        int err = n > want ? n - want : want - n;
        CHECK(err * 20 <= want, "release %u ms took %.1f ms", rel[i], n / 48.0);
    }
    /* a 60 s release completes (reaches IDLE) */
    int16_t l, r;
    sm_init(&S);
    voice_basic(0, ALGO_SUB);
    sm_vwrite(&S, 0, VR_RELEASE, env_release_ms(60000));
    sm_vwrite(&S, 0, VR_GATE, GATE_ON | GATE_TRIG);
    for (int i = 0; i < 4800; i++) sm_process(&S, &l, &r);
    sm_vwrite(&S, 0, VR_GATE, 0);
    int n = 0;
    while (S.v[0].env_state != ENV_IDLE && n < 48000 * 120) { sm_process(&S, &l, &r); n++; }
    CHECK(S.v[0].env_state == ENV_IDLE, "60 s release never finished");

    /* retrigger during release: envelope continues from its level (no jump) */
    sm_init(&S);
    voice_basic(0, ALGO_SUB);
    sm_vwrite(&S, 0, VR_ATTACK, env_attack_ms(50));
    sm_vwrite(&S, 0, VR_RELEASE, env_release_ms(2000));
    sm_vwrite(&S, 0, VR_GATE, GATE_ON | GATE_TRIG);
    for (int i = 0; i < 9600; i++) sm_process(&S, &l, &r);
    sm_vwrite(&S, 0, VR_GATE, 0);
    for (int i = 0; i < 9600; i++) sm_process(&S, &l, &r);
    int32_t before = S.v[0].env_level;
    sm_vwrite(&S, 0, VR_GATE, GATE_ON | GATE_TRIG);
    int32_t maxstep = 0, prev = before;
    for (int i = 0; i < 4800; i++) {
        sm_process(&S, &l, &r);
        int32_t d = S.v[0].env_level - prev;
        if (d < 0) d = -d;
        if (d > maxstep) maxstep = d;
        prev = S.v[0].env_level;
    }
    CHECK(maxstep < (1 << 30) / 400, "retrigger jump %d", maxstep);
    CHECK(S.v[0].env_level > before, "retrigger rises from the release level");
}

/* --------------------------------------------------------------- system */

typedef struct { uint64_t tick; uint32_t kind, a, b, c; } tr_t;
static tr_t *g_tr;
static size_t g_ntr, g_captr;
static void trace_cap(void *ctx, uint64_t tick, uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    (void)ctx;
    if (g_ntr == g_captr) {
        g_captr = g_captr ? g_captr * 2 : 4096;
        g_tr = realloc(g_tr, g_captr * sizeof *g_tr);
    }
    g_tr[g_ntr++] = (tr_t){tick, kind, a, b, c};
}

typedef struct {
    uint32_t mode, seed, density, bright, sleep;
    uint64_t tick0, ms;
    struct { uint64_t at; uint32_t btn; } press[16];
    int npress;
} run_cfg_t;

typedef struct {
    uint64_t hash;
    uint64_t nonzero_after;      /* nonzero samples after `zero_from_ms` */
    uint64_t zero_from_ms;
    int      gain_monotonic_violations;
    int32_t  peak;
    uint64_t fx_from_ms, fx_to_ms; /* count FX register / FX set changes in ticks [from, to) */
    int      fx_changes;
    double  *env;                  /* optional: mean square of each 20 ms window */
} run_out_t;

static int fx_differs(const uint32_t *fx0, const comp_t *c0)
{
    const comp_t *cs = composer_state();
    for (unsigned i = 0; i < FX_NUM_REGS; i++)
        if (i != FX_MASTER_TARGET && i != FX_MASTER_STEP && i != FX_STATUS_GAIN && S.fx[i] != fx0[i]) return 1;
    return memcmp(&cs->fx_cur, &c0->fx_cur, sizeof cs->fx_cur) || memcmp(&cs->fx_tgt, &c0->fx_tgt, sizeof cs->fx_tgt) ||
           cs->fx_ramp != c0->fx_ramp;
}

static void run_system(const run_cfg_t *c, run_out_t *o)
{
    g_ntr = 0;
    sm_init(&S);
    halm_bind(&S);
    halm_set_trace(trace_cap, NULL);
    halm_set_tick(c->tick0);
    halm_ctrl_set(CR_MODE, c->mode);
    halm_ctrl_set(CR_DENSITY, c->density ? c->density : 3);
    halm_ctrl_set(CR_BRIGHT, c->bright ? c->bright : 3);
    halm_ctrl_set(CR_REVERB, 1);
    halm_ctrl_set(CR_SLEEP, c->sleep);
    halm_ctrl_set(CR_SEED_LOCK, 1);
    halm_ctrl_set(CR_SEED, c->seed);
    halm_ctrl_set(CR_SETTINGS_SEQ, 0);
    composer_init();
    uint64_t h = 1469598103934665603ull;
    int32_t prev_gain = 0, prev_tgt = -1;
    o->nonzero_after = 0;
    o->gain_monotonic_violations = 0;
    o->peak = 0;
    o->fx_changes = 0;
    uint32_t fx0[FX_NUM_REGS];
    comp_t c0;
    double acc = 0;
    for (uint64_t t = 0; t < c->ms; t++) {
        for (int i = 0; i < c->npress; i++) if (c->press[i].at == t) halm_press(c->press[i].btn);
        halm_set_tick(c->tick0 + t);
        memcpy(fx0, S.fx, sizeof fx0);
        c0 = *composer_state();
        composer_tick();
        if (t >= o->fx_from_ms && t < o->fx_to_ms) o->fx_changes += fx_differs(fx0, &c0);
        for (unsigned k = 0; k < AP_TICK_SAMPLES; k++) {
            int16_t l, r;
            sm_process(&S, &l, &r);
            acc += (double)l * l + (double)r * r;
            h = (h ^ (uint16_t)l) * 1099511628211ull;
            h = (h ^ (uint16_t)r) * 1099511628211ull;
            if (o->zero_from_ms && t >= o->zero_from_ms && (l || r)) o->nonzero_after++;
            int32_t al = l < 0 ? -l : l;
            if (al > o->peak) o->peak = al;
            int32_t tgt = (int32_t)S.fx[FX_MASTER_TARGET];
            if (tgt == prev_tgt) {
                if ((tgt >= prev_gain && S.master_gain < prev_gain) ||
                    (tgt <= prev_gain && S.master_gain > prev_gain))
                    o->gain_monotonic_violations++;
            }
            prev_tgt = tgt;
            prev_gain = S.master_gain;
        }
        if (t % 20 == 19) {
            if (o->env) o->env[t / 20] = acc / (20.0 * AP_TICK_SAMPLES * 2);
            acc = 0;
        }
    }
    o->hash = h;
    halm_set_trace(NULL, NULL);
}

TEST(test_determinism)
{
    run_cfg_t c = {0};
    run_out_t a = {0}, b = {0}, d = {0};
    c.mode = MODE_TIDES; c.seed = 99; c.ms = 20000;
    c.press[0].at = 1; c.press[0].btn = BTN_A; c.npress = 1;
    run_system(&c, &a);
    run_system(&c, &b);
    CHECK(a.hash == b.hash, "same seed must be bit-identical");
    c.seed = 100;
    c.ms = 5000;
    run_system(&c, &d);
    c.seed = 99;
    run_system(&c, &b);
    CHECK(d.hash != b.hash, "different seeds must differ within 5 s");
}

TEST(test_fades_and_stop)
{
    for (uint32_t m = 0; m < MODE_COUNT; m++) {
        run_cfg_t c = {0};
        run_out_t o = {0};
        c.mode = m; c.seed = 5; c.ms = 20000;
        c.press[0].at = 1; c.press[0].btn = BTN_A;
        c.press[1].at = 12000; c.press[1].btn = BTN_A;      /* stop: 3 s fade */
        c.npress = 2;
        o.zero_from_ms = 12000 + 3000 + 2;
        run_system(&c, &o);
        CHECK(o.nonzero_after == 0, "mode %u: %llu non-zero samples after the fade-out", m,
              (unsigned long long)o.nonzero_after);
        CHECK(o.gain_monotonic_violations == 0, "mode %u: fade not monotonic", m);
        CHECK(o.peak > 1000, "mode %u: no audio while playing (peak %d)", m, o.peak);
        CHECK(o.peak < 29300, "mode %u: peak %d above -1 dBFS", m, o.peak);
        int state_title = 0;
        for (size_t i = 0; i < g_ntr; i++)
            if (g_tr[i].kind == TR_STATE && g_tr[i].a == CS_TITLE && g_tr[i].tick > 12000) state_title = 1;
        CHECK(state_title, "mode %u: back to title after stop", m);
    }
}

/* stress: voice steals, reseeds and mode switches every second */
TEST(test_stress_switching)
{
    run_cfg_t c = {0};
    run_out_t o = {0};
    c.mode = MODE_DRIFT; c.seed = 3; c.ms = 16000; c.density = 5;
    c.press[0].at = 1; c.press[0].btn = BTN_A;
    int n = 1;
    for (uint64_t t = 1000; t < 15000 && n < 16; t += 1000)
        { c.press[n].at = t; c.press[n].btn = (n & 1) ? BTN_RIGHT : BTN_B; n++; }
    c.npress = n;
    run_system(&c, &o);
    /* no voice is ever re-triggered while still gated, and allocation never exceeds 32 */
    uint8_t gated[32] = {0};
    int bad = 0, maxg = 0;
    for (size_t i = 0; i < g_ntr; i++) {
        if (g_tr[i].kind == TR_NOTE_ON) { bad += gated[g_tr[i].a]; gated[g_tr[i].a] = 1; }
        if (g_tr[i].kind == TR_NOTE_OFF) gated[g_tr[i].a] = 0;
        int g = 0;
        for (int v = 0; v < 32; v++) g += gated[v];
        if (g > maxg) maxg = g;
    }
    CHECK(bad == 0, "%d notes re-triggered while gated", bad);
    CHECK(maxg <= 32, "gated voices %d", maxg);
    CHECK(o.peak < 29300, "peak %d above -1 dBFS during switching", o.peak);
}

/* D53: a mode change during the stop fade-out must not run the old mode's union state as the
 * new mode's (it used to write voice indices like 301), with and without a restart in the fade */
TEST(test_stop_switch)
{
    for (uint32_t m = 0; m < MODE_COUNT; m++)
        for (int restart = 0; restart < 2; restart++)
            for (int dir = 0; dir < 2; dir++) {
                run_cfg_t c = {0};
                run_out_t o = {0};
                c.mode = m; c.seed = 11 + m; c.ms = 30000;
                c.press[0].at = 1; c.press[0].btn = BTN_A;
                c.press[1].at = 15000; c.press[1].btn = BTN_A;              /* stop: 3 s fade */
                c.press[2].at = 15500; c.press[2].btn = dir ? BTN_LEFT : BTN_RIGHT;
                c.press[3].at = 16000; c.press[3].btn = BTN_A;              /* change of heart */
                c.npress = restart ? 4 : 3;
                if (!restart) o.zero_from_ms = 15000 + 3000 + 2;
                run_system(&c, &o);
                int bad = 0, on_after = 0;
                for (size_t i = 0; i < g_ntr; i++) {
                    if (g_tr[i].kind != TR_NOTE_ON && g_tr[i].kind != TR_NOTE_OFF) continue;
                    bad += g_tr[i].a >= AP_NUM_VOICES;
                    on_after += g_tr[i].kind == TR_NOTE_ON && g_tr[i].tick > 16000;
                }
                const comp_t *cs = composer_state();
                CHECK(bad == 0, "mode %u dir %d restart %d: %d out-of-range voice indices", m, dir, restart, bad);
                if (restart) {
                    CHECK(cs->state == CS_PLAYING && cs->m_mode == cs->mode,
                          "mode %u dir %d: new mode not running after the restart", m, dir);
                    CHECK(on_after > 0, "mode %u dir %d: no notes after the restart", m, dir);
                } else {
                    CHECK(o.nonzero_after == 0, "mode %u dir %d: %llu non-zero samples after the fade-out",
                          m, dir, (unsigned long long)o.nonzero_after);
                    CHECK(cs->state == CS_TITLE && on_after == 0, "mode %u dir %d: notes after the stop", m, dir);
                }
            }
}

/* D56: a mode change in the stop fade-out leaves the FX alone until the gain reaches 0, so the old
 * piece's tail never gets louder; the new mode's FX are set at the title. From every mode to every
 * other mode, at three points in the fade. A again in the fade then ramps the FX from the old set. */
TEST(test_fade_switch)
{
    enum { STOP = 8000, END = STOP + 3000 + 50, MS = END + 100, NW = MS / 20 };
    static const uint64_t off[3] = {100, 1500, 2900};
    static double ref[NW], sw[NW];
    const double floor_ms = 10.0, tol = 1.2589;           /* mean square: about -80 dBFS; +1 dB */
    fxset_t want[MODE_COUNT];
    for (uint32_t m = 0; m < MODE_COUNT; m++) {           /* each mode's FX set on its title screen */
        run_cfg_t c = {0};
        run_out_t o = {0};
        c.mode = m; c.ms = 2;
        run_system(&c, &o);
        want[m] = composer_state()->fx_tgt;
    }
    for (uint32_t m = 0; m < MODE_COUNT; m++) {
        run_cfg_t c = {0};
        run_out_t o = {0};
        c.mode = m; c.seed = 21 + m; c.ms = MS;
        c.press[0].at = 1; c.press[0].btn = BTN_A;
        c.press[1].at = STOP; c.press[1].btn = BTN_A;    /* stop: 3 s fade */
        c.npress = 2;
        o.env = ref;
        run_system(&c, &o);
        for (uint32_t k = 0; k < MODE_COUNT; k++)        /* k = 0: a reseed (B) instead */
            for (int at = 0; at < 3; at++) {
                uint64_t ts = STOP + off[at];
                uint32_t to = (m + k) % MODE_COUNT;
                c.npress = 2;
                for (uint32_t i = 0; i < (k ? k : 1); i++) {  /* k presses of Right, one per tick */
                    c.press[c.npress].at = ts + i;
                    c.press[c.npress++].btn = k ? BTN_RIGHT : BTN_B;
                }
                o = (run_out_t){0};
                o.env = sw; o.fx_from_ms = ts; o.fx_to_ms = END;
                run_system(&c, &o);
                const comp_t *cs = composer_state();
                CHECK(o.fx_changes == 0, "mode %u -> %u at +%llu ms: %d FX changes during the fade", m, to,
                      (unsigned long long)off[at], o.fx_changes);
                CHECK(cs->state == CS_TITLE && cs->mode == to && cs->fx_ramp == 0 &&
                      !memcmp(&cs->fx_cur, &want[to], sizeof want[to]) && !memcmp(&cs->fx_tgt, &want[to], sizeof want[to]),
                      "mode %u -> %u at +%llu ms: title FX are not the new mode's", m, to, (unsigned long long)off[at]);
                double over = 0;                          /* 20 ms windows: louder than without the switch? */
                for (int w = (int)(ts / 20) + 1; w < END / 20; w++)
                    if (ref[w] > floor_ms && sw[w] / ref[w] > over) over = sw[w] / ref[w];
                CHECK(over <= tol, "mode %u -> %u at +%llu ms: the tail is x%.2f louder (power) than without the switch",
                      m, to, (unsigned long long)off[at], over);
            }
        /* switch, then A within the fade: the new mode's FX ramp from the old set over XFADE_MS */
        c.npress = 4;
        c.press[2].at = STOP + 1000; c.press[2].btn = BTN_RIGHT;
        c.press[3].at = STOP + 1500; c.press[3].btn = BTN_A;
        c.ms = STOP + 1500;
        o = (run_out_t){0};
        o.fx_from_ms = STOP + 1000; o.fx_to_ms = STOP + 1500;
        run_system(&c, &o);
        fxset_t old = composer_state()->fx_cur;
        CHECK(o.fx_changes == 0, "mode %u: %d FX changes between the switch and A", m, o.fx_changes);
        c.ms = STOP + 1501;
        o = (run_out_t){0};
        run_system(&c, &o);
        const comp_t *cs = composer_state();
        uint32_t to = (m + 1) % MODE_COUNT;
        const int32_t *o32 = (const int32_t *)&old, *c32 = (const int32_t *)&cs->fx_cur,
                      *t32 = (const int32_t *)&cs->fx_tgt;
        int jump = 0;
        for (unsigned i = 0; i < sizeof(fxset_t) / sizeof(int32_t); i++) {
            int32_t d = c32[i] - o32[i], span = t32[i] - o32[i];
            jump += (d < 0 ? -d : d) > (span < 0 ? -span : span) / (int32_t)XFADE_MS + 1;
        }
        CHECK(cs->state == CS_PLAYING && cs->m_mode == to && cs->fx_ramp == XFADE_MS - 1 &&
              !memcmp(&cs->fx_tgt, &want[to], sizeof want[to]) && jump == 0,
              "mode %u: switch + A in the fade does not ramp the FX from the old set (%d jumps)", m, jump);
    }
}

/* the voice allocator never steals a gated voice, and frees voices after release */
TEST(test_allocator)
{
    sm_init(&S);
    halm_bind(&S);
    halm_set_tick(0);
    voices_reset();
    vparams_t p = {0};
    p.algo = ALGO_SUB; p.attack = env_attack_ms(10); p.sustain = 32768;
    p.release = env_release_ms(200); p.amp = 1000; p.pan = 16384; p.fmode = FMODE_BYPASS;
    int got[8];
    for (int i = 0; i < 8; i++) {
        got[i] = voice_alloc(1, 0, 7, 8);
        CHECK(got[i] == i, "alloc %d -> %d", i, got[i]);
        voice_setup(got[i], &p);
        voice_on(got[i], 1, 6000, 100000, 0);
    }
    CHECK(voice_alloc(1, 0, 7, 8) == -1, "all gated: nothing may be stolen");
    int16_t l, r;
    for (int i = 0; i < 480; i++) sm_process(&S, &l, &r);
    voice_off(3);
    for (int i = 0; i < 480; i++) sm_process(&S, &l, &r);
    CHECK(voice_alloc(1, 0, 7, 8) == -1, "an audible releasing voice must not be stolen");
    for (int i = 0; i < 10600; i++) sm_process(&S, &l, &r);        /* 200 ms release: now < -60 dB */
    int st = voice_alloc(1, 0, 7, 8);
    CHECK(st == 3, "only the releasing voice is stealable (got %d, v3 state %u)", st,
          sm_vread(&S, 3, VR_STATUS) & 3);
    CHECK(voice_alloc(2, 0, 7, 0) == -1, "budget 0 for another owner: nothing");
    for (int i = 0; i < 48000; i++) sm_process(&S, &l, &r);
    voices_housekeep(1000);
    CHECK(g_vinfo[3].owner == 0, "voice freed after release completes");
    /* role voices: a reserved voice is never handed to anyone else, even when idle */
    int role_v = -1;
    CHECK(role_alloc(&role_v, 2, 8, 9, 2) == 8, "role alloc");
    vparams_t rp = p;
    voice_setup(role_v, &rp);
    voice_on(role_v, 2, 6000, 100000, 0);
    voice_off(role_v);
    for (int i = 0; i < 48000; i++) sm_process(&S, &l, &r);
    voices_housekeep(1000);
    CHECK(g_vinfo[8].owner == 2, "reserved idle role voice stays owned");
    CHECK(voice_alloc(2, 8, 8, 4) == -1 && voice_alloc(3, 8, 8, 4) == -1, "reserved voice not re-allocated");
    int again = role_alloc(&role_v, 2, 8, 9, 2);
    CHECK(again == 9 && !g_vinfo[8].reserved, "role moves on and releases its old voice (%d)", again);
    /* safety: notes past their max length are forced off */
    voices_housekeep(200000);
    CHECK(voices_count_owner(1) == 7 && !g_vinfo[0].gated, "over-long notes forced off");
}

/* TIDES: every pitched note is in the key collection; chords change every 4-10 s (+breaths) */
TEST(test_tides_system)
{
    run_cfg_t c = {0};
    run_out_t o = {0};
    c.mode = MODE_TIDES; c.seed = 11; c.ms = 300000; c.density = 5;
    c.press[0].at = 1; c.press[0].btn = BTN_A; c.npress = 1;
    run_system(&c, &o);
    hkey_t k = {0, 0}, prevk = {0, 0};
    uint64_t key_t = 0, last_chord = 0;
    int notes = 0, bad = 0, chords = 0, bad_gap = 0;
    for (size_t i = 0; i < g_ntr; i++) {
        tr_t *e = &g_tr[i];
        if (e->kind == TR_KEY) { prevk = k; k.tonic = (int8_t)e->a; k.centre = (int8_t)e->b; key_t = e->tick; }
        if (e->kind == TR_CHORD) {
            if (chords && (e->tick - last_chord < 3900 || e->tick - last_chord > 16500)) bad_gap++;
            last_chord = e->tick;
            chords++;
        }
        if (e->kind == TR_NOTE_ON && e->b > 0) {
            notes++;
            int p = (int)e->b / 100;
            bad += !(key_has_pc(k, p) || (e->tick - key_t < 20000 && key_has_pc(prevk, p)));
        }
    }
    CHECK(notes > 100, "TIDES played only %d notes in 5 min", notes);
    CHECK(chords >= 20, "only %d chord changes in 5 min", chords);
    CHECK(bad_gap == 0, "%d chord changes outside 4-16 s", bad_gap);
    CHECK(bad == 0, "%d TIDES notes outside the key", bad);
}

/* DRIFT: rests happen, strings/horn play, nothing is retriggered while gated */
TEST(test_drift_system)
{
    run_cfg_t c = {0};
    run_out_t o = {0};
    c.mode = MODE_DRIFT; c.seed = 21; c.ms = 600000;
    c.press[0].at = 1; c.press[0].btn = BTN_A; c.npress = 1;
    run_system(&c, &o);
    int chords = 0, notes = 0, retrig = 0;
    uint8_t g[32] = {0};
    for (size_t i = 0; i < g_ntr; i++) {
        tr_t *e = &g_tr[i];
        if (e->kind == TR_CHORD) chords++;
        if (e->kind == TR_NOTE_ON) { notes++; retrig += g[e->a]; g[e->a] = 1; }
        if (e->kind == TR_NOTE_OFF) g[e->a] = 0;
    }
    CHECK(chords >= 3, "only %d string events in 10 min", chords);
    CHECK(notes > 20, "only %d notes", notes);
    CHECK(retrig == 0, "%d retriggers while gated", retrig);
    CHECK(o.peak < 29300, "peak %d", o.peak);
}

/* tick counter crossing 2^32 (and 2^31): no freeze, music continues */
TEST(test_tick_overflow)
{
    static const uint64_t bases[2] = {(1ull << 32) - 10000, (1ull << 31) - 10000};
    for (int b = 0; b < 2; b++) {
        run_cfg_t c = {0};
        run_out_t o = {0};
        c.mode = (uint32_t)b * 2; c.seed = 77; c.ms = 60000; c.tick0 = bases[b];
        c.press[0].at = 1; c.press[0].btn = BTN_A; c.npress = 1;
        run_system(&c, &o);
        int after = 0;
        uint64_t boundary = b ? (1ull << 31) : (1ull << 32);
        for (size_t i = 0; i < g_ntr; i++)
            if (g_tr[i].kind == TR_NOTE_ON && g_tr[i].tick >= boundary) after++;
        CHECK(after > 0, "no notes after the %s boundary", b ? "2^31" : "2^32");
        CHECK(o.peak > 1000 && o.peak < 29300, "level across boundary (peak %d)", o.peak);
    }
}

TEST(test_sleep_timer)
{
    run_cfg_t c = {0};
    run_out_t o = {0};
    c.mode = MODE_HUSH; c.seed = 1; c.sleep = 1; c.ms = 30 * 60000 + 35000;
    c.press[0].at = 1; c.press[0].btn = BTN_A; c.npress = 1;
    o.zero_from_ms = 30 * 60000 + 30000 + 5;
    run_system(&c, &o);
    CHECK(o.nonzero_after == 0, "sleep timer did not end in silence (%llu samples)",
          (unsigned long long)o.nonzero_after);
}

int main(int argc, char **argv)
{
    int longrun = argc > 1 && !strcmp(argv[1], "--long");
    RUN(test_prng_kat);
    RUN(test_prng_streams);
    RUN(test_sched);
    RUN(test_harmony);
    RUN(test_fxp);
    RUN(test_svf_stability);
    RUN(test_envelopes);
    RUN(test_allocator);
    RUN(test_determinism);
    RUN(test_fades_and_stop);
    RUN(test_stress_switching);
    RUN(test_stop_switch);
    RUN(test_fade_switch);
    RUN(test_tides_system);
    RUN(test_drift_system);
    RUN(test_tick_overflow);
    if (longrun) RUN(test_sleep_timer);
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
