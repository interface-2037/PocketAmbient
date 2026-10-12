/*
 * composer.c - top-level composer: user flow (title / playing / stopping),
 * fades, mode crossfades, reseed, sleep timer, FX ramps, the slow "weather"
 * layer and key drift scheduling. Portable C: built unchanged for RISC-V and host.
 */
#include "composer.h"
#include "cint.h"

static comp_t C;

const comp_t *composer_state(void) { return &C; }
static const mode_ops_t *const MODES[MODE_COUNT] = {
    &MODE_DRIFT_OPS, &MODE_TIDES_OPS, &MODE_HUSH_OPS, &MODE_MOTHER_OPS };

#define GAIN_UNITY     (1u << 23)
#define FADE_IN_MS     4000u
#define FADE_OUT_MS    3000u
#define SLEEP_FADE_MS  30000u
#define ECHO_LEN_L     20640u   /* 430 ms */
#define ECHO_LEN_R     29280u   /* 610 ms */

static uint32_t g_stop_ms;

/* ------------------------------------------------------------------ helpers */

int32_t c_lerp_q8(const int32_t tbl[5], int32_t q8)
{
    int32_t idx = (q8 >> 8) - 1, fr = q8 & 255;
    if (idx < 0) return tbl[0];
    if (idx >= 4) return tbl[4];
    return tbl[idx] + (int32_t)(((int64_t)(tbl[idx + 1] - tbl[idx]) * fr) >> 8);
}

int32_t c_density_q8(const comp_t *c)
{
    return clampi(c->density * 256 + mul_q15(c->wx.out[W_DENSITY], 150), 256, 1280);
}

int32_t c_bright_q8(const comp_t *c)
{
    return clampi(c->bright * 256 + mul_q15(c->wx.out[W_BRIGHT], 150), 256, 1280);
}

void c_schedule_off(comp_t *c, int v, uint32_t after_ms)
{
    sched_push(&c->sched, c->now + after_ms, EV_NOTE_OFF, 255, c->gen,
               (uint32_t)v, (uint32_t)g_vinfo[v].on_tick);
}

void c_schedule_key_change(comp_t *c)
{
    sched_push(&c->sched, c->now + (uint32_t)prng_range(&c->r_form, MIN(8), MIN(25)),
               EV_KEY, 120, c->gen, 0, 0);
}

uint16_t c_pan_rand(comp_t *c, int spread)
{
    return (uint16_t)clampi(16384 + prng_range(&c->r_timbre, -spread, spread), 0, 32768);
}

static void master_fade(uint32_t target, uint32_t ms)
{
    uint32_t step = (GAIN_UNITY + ms * 48 - 1) / (ms * 48);
    hal_fxwrite(FX_MASTER_STEP, step ? step : 1);
    hal_fxwrite(FX_MASTER_TARGET, target);
}

/* ------------------------------------------------------------------ weather */

static void weather_init(comp_t *c)
{
    static const uint32_t base_s[2 * W_COUNT] = {173, 419, 241, 607, 331, 887, 463, 1031};
    prng_t r;
    prng_seed(&r, c->sess_seed, ST_WEATHER);
    for (int i = 0; i < 2 * W_COUNT; i++) {
        uint32_t p_ms = base_s[i] * (870 + prng_below(&r, 261));     /* +/- 13% */
        c->wx.inc[i] = 0xFFFFFFFFu / p_ms;
        c->wx.ph0[i] = prng_next(&r);
    }
}

static void weather_update(comp_t *c)
{
    for (int w = 0; w < W_COUNT; w++) {
        /* absolute-time phase: continuous across any counter boundary (mod 2^32 maths) */
        uint32_t pa = c->wx.ph0[2 * w] + (uint32_t)(c->now * c->wx.inc[2 * w]);
        uint32_t pb = c->wx.ph0[2 * w + 1] + (uint32_t)(c->now * c->wx.inc[2 * w + 1]);
        c->wx.out[w] = (fx_sin_q15(pa) + fx_sin_q15(pb)) / 2;
    }
}

/* ----------------------------------------------------------------------- FX */

static void fx_apply(const fxset_t *f)
{
    for (int i = 0; i < AP_REV_LINES; i++) {
        uint32_t len = AP_REV_LEN[i] + ((i == 0 || i == 5) ? AP_REV_MOD_DEPTH / 2 : 0);
        hal_fxwrite(FX_REV_GAIN0 + i, rev_line_gain(len, (uint32_t)f->t60_ms));
    }
    hal_fxwrite(FX_REV_DAMP, (uint32_t)f->rev_damp);
    hal_fxwrite(FX_REV_WET, (uint32_t)f->rev_wet);
    hal_fxwrite(FX_REV_PREDELAY, (uint32_t)f->predelay);
    hal_fxwrite(FX_ECHO_FB, (uint32_t)f->echo_fb);
    hal_fxwrite(FX_ECHO_DAMP, (uint32_t)f->echo_damp);
    hal_fxwrite(FX_ECHO_WET, (uint32_t)f->echo_wet);
    hal_fxwrite(FX_ECHO_TO_REV, (uint32_t)f->echo_to_rev);
    hal_fxwrite(FX_ECHO_LEN_L, (uint32_t)f->echo_len_l);
    hal_fxwrite(FX_ECHO_LEN_R, (uint32_t)f->echo_len_r);
    hal_fxwrite(FX_ECHO_MOD_DEPTH, (uint32_t)f->echo_mod_depth);
    hal_fxwrite(FX_ECHO_MOD_RATE, (uint32_t)f->echo_mod_rate);
    hal_fxwrite(FX_DRY, (uint32_t)f->dry);
    hal_fxwrite(FX_TRIM, (uint32_t)f->trim);
}

static void fx_retarget(comp_t *c, uint32_t ramp_ms)
{
    static const int32_t t60_mul[3] = {6, 10, 14}, wet_mul[3] = {7, 10, 12};
    fxset_t f;
    f.echo_len_l = ECHO_LEN_L;                /* defaults; modes may override */
    f.echo_len_r = ECHO_LEN_R;
    f.echo_mod_depth = 0;
    f.echo_mod_rate = 0;
    MODES[c->mode]->fx(c, &f);
    f.t60_ms = f.t60_ms * t60_mul[c->reverb % 3] / 10;
    f.rev_wet = f.rev_wet * wet_mul[c->reverb % 3] / 10;
    c->fx_tgt = f;
    if (!ramp_ms) {
        c->fx_cur = f;
        c->fx_ramp = 0;
        fx_apply(&f);
    } else {
        c->fx_ramp = ramp_ms;
    }
}

static void fx_ramp_tick(comp_t *c)
{
    if (!c->fx_ramp) return;
    int32_t *cur = (int32_t *)&c->fx_cur;
    const int32_t *tgt = (const int32_t *)&c->fx_tgt;
    for (unsigned i = 0; i < sizeof(fxset_t) / sizeof(int32_t); i++)
        cur[i] += (tgt[i] - cur[i]) / (int32_t)c->fx_ramp;
    c->fx_ramp--;
    if ((c->fx_ramp & 15) == 0) fx_apply(&c->fx_cur);
}

/* -------------------------------------------------------------- mode control */

static void reseed_streams(comp_t *c)
{
    uint32_t s = c->sess_seed ^ ((uint32_t)c->mode << 24) ^ ((uint32_t)c->gen * 0x01000193u);
    prng_seed(&c->r_harm, s, ST_HARMONY);
    prng_seed(&c->r_rhythm, s, ST_RHYTHM);
    prng_seed(&c->r_timbre, s, ST_TIMBRE);
    prng_seed(&c->r_form, s, ST_FORM);
}

static void mode_begin(comp_t *c, uint32_t fx_ramp_ms)
{
    c->gen = (uint8_t)(c->gen % 250 + 1);
    reseed_streams(c);
    hal_trace(TR_MODE, c->mode, c->gen, 0);
    c->m_mode = c->mode;
    MODES[c->mode]->start(c);
    c_schedule_key_change(c);
    fx_retarget(c, fx_ramp_ms);
}

static void mode_end(comp_t *c)
{
    sched_cancel_owner(&c->sched, c->gen);
    voices_release_owner(c->gen, env_release_ms(XFADE_RELEASE));
    c->m_mode = MODE_COUNT;                 /* c->m is no longer a running mode's state */
}

static void new_session_seed(comp_t *c, uint32_t s)
{
    c->sess_seed = s;
    prng_seed(&c->r_meta, s, ST_META);
    static const uint32_t cw[5] = {4, 1, 2, 2, 4};          /* Ionian, Dorian, Lydian, Mixo, Aeolian */
    static const int8_t cs[5] = {0, 1, 3, 4, 5};
    c->key.tonic = (int8_t)prng_below(&c->r_meta, 12);
    c->key.centre = cs[prng_weighted(&c->r_meta, cw, 5)];
    weather_init(c);
    weather_update(c);
    hal_trace(TR_RESEED, s, 0, 0);
    hal_trace(TR_KEY, (uint32_t)c->key.tonic, (uint32_t)c->key.centre, 0);
}

static void set_state(comp_t *c, uint8_t st)
{
    c->state = st;
    c->state_t = c->now;
    hal_trace(TR_STATE, st, 0, 0);
}

static void do_start(comp_t *c)
{
    if (c->state == CS_STOPPING) {           /* change of heart: fade back in */
        master_fade(GAIN_UNITY, FADE_IN_MS);
        set_state(c, CS_PLAYING);
        if (c->m_mode != c->mode) mode_begin(c, XFADE_MS);   /* the piece ended during the fade */
        return;
    }
    c->play_start = c->now;
    set_state(c, CS_PLAYING);
    mode_begin(c, 0);
    master_fade(GAIN_UNITY, FADE_IN_MS);
}

static void do_stop(comp_t *c, uint32_t fade_ms)
{
    g_stop_ms = fade_ms;
    master_fade(0, fade_ms);
    set_state(c, CS_STOPPING);
}

static void do_switch(comp_t *c, uint8_t new_mode)
{
    if (c->state == CS_PLAYING) {
        mode_end(c);
        c->mode = new_mode;
        mode_begin(c, XFADE_MS);
    } else {
        /* Stopped or fading out: the new mode starts with the next start. c->m is a union, so
         * the old mode's state must not be run as the new mode's (D53): end the old piece now.
         * Its tail keeps its FX until the fade-out reaches 0, where they change (D56). */
        if (c->state == CS_STOPPING && c->m_mode != MODE_COUNT) mode_end(c);
        c->mode = new_mode;
        if (c->state == CS_TITLE) fx_retarget(c, 0);
    }
}

static void do_reseed(comp_t *c)
{
    new_session_seed(c, prng_next(&c->r_meta) ^ (uint32_t)c->now);
    if (c->state == CS_PLAYING) {
        mode_end(c);
        mode_begin(c, XFADE_MS);
    } else if (c->state == CS_STOPPING && c->m_mode != MODE_COUNT) {
        mode_end(c);        /* the fading piece must not run on with the new key and weather (D56) */
    }
}

/* ------------------------------------------------------------------- inputs */

static uint8_t clamp15(int x) { return (uint8_t)(x < 1 ? 1 : (x > 5 ? 5 : x)); }

static void handle_buttons(comp_t *c, uint32_t b)
{
    if (b & (BTN_A | BTN_START)) {
        if (c->state == CS_PLAYING) do_stop(c, FADE_OUT_MS);
        else do_start(c);
    }
    if (b & BTN_LEFT)  do_switch(c, (uint8_t)((c->mode + MODE_COUNT - 1) % MODE_COUNT));
    if (b & BTN_RIGHT) do_switch(c, (uint8_t)((c->mode + 1) % MODE_COUNT));
    if (b & BTN_B)     do_reseed(c);
    if (b & BTN_UP)    c->density = clamp15(c->density + 1);
    if (b & BTN_DOWN)  c->density = clamp15(c->density - 1);
    if (b & BTN_R)     c->bright = clamp15(c->bright + 1);
    if (b & BTN_L)     c->bright = clamp15(c->bright - 1);
    if (b & BTN_X)     c->visuals = (uint8_t)((c->visuals + 1) % 3);
    if (b & BTN_Y)     c->overlay ^= 1;
    /* BTN_SELECT: speaker EQ, reserved for v2 */
}

static void read_menu(comp_t *c, int first)
{
    uint32_t m = hal_ctrl_read(CR_MODE);
    c->density = clamp15((int)hal_ctrl_read(CR_DENSITY));
    c->bright = clamp15((int)hal_ctrl_read(CR_BRIGHT));
    uint8_t rv = (uint8_t)(hal_ctrl_read(CR_REVERB) % 3);
    c->visuals = (uint8_t)(hal_ctrl_read(CR_VISUALS) % 3);
    c->seed_lock = (uint8_t)(hal_ctrl_read(CR_SEED_LOCK) & 1);
    c->user_seed = hal_ctrl_read(CR_SEED);
    c->sleep_sel = (uint8_t)(hal_ctrl_read(CR_SLEEP) & 3);
    if (first) {
        c->mode = (uint8_t)(m % MODE_COUNT);
        c->reverb = rv;
        return;
    }
    if (rv != c->reverb) { c->reverb = rv; fx_retarget(c, 2000); }
    if (m % MODE_COUNT != c->mode) do_switch(c, (uint8_t)(m % MODE_COUNT));
}

static void write_status(comp_t *c)
{
    uint32_t el = c->state == CS_TITLE ? 0 : (uint32_t)((c->now - c->play_start) / 1000u);
    hal_ctrl_write(CR_ST_STATE, c->state);
    hal_ctrl_write(CR_ST_MODE, c->mode);
    hal_ctrl_write(CR_ST_ELAPSED_S, el);
    hal_ctrl_write(CR_ST_SEED, c->sess_seed);
    hal_ctrl_write(CR_ST_DENSITY, c->density);
    hal_ctrl_write(CR_ST_BRIGHT, c->bright);
    hal_ctrl_write(CR_ST_VISUALS, c->visuals);
    hal_ctrl_write(CR_ST_OVERLAY, c->overlay);
    hal_ctrl_write(CR_ST_REVERB, c->reverb);
    hal_ctrl_write(CR_ST_SLEEP, c->sleep_sel);
}

/* --------------------------------------------------------------------- API */

void composer_init(void)
{
    comp_t *c = &C;
    uint8_t *p = (uint8_t *)c;
    for (unsigned i = 0; i < sizeof *c; i++) p[i] = 0;   /* no libc on the target */

    c->now = hal_ticks();
    sched_init(&c->sched);
    voices_reset();
    c->m_mode = MODE_COUNT;
    read_menu(c, 1);
    c->settings_seq = hal_ctrl_read(CR_SETTINGS_SEQ);
    uint32_t seed = c->seed_lock ? c->user_seed
                                 : (hal_ctrl_read(CR_RTC_EPOCH) * 2654435761u) ^ c->user_seed;
    new_session_seed(c, seed);
    master_fade(0, 1);
    fx_retarget(c, 0);
    set_state(c, CS_TITLE);
    write_status(c);
}

void composer_tick(void)
{
    comp_t *c = &C;
    c->now = hal_ticks();

    uint32_t seq = hal_ctrl_read(CR_SETTINGS_SEQ);
    if (seq != c->settings_seq) { c->settings_seq = seq; read_menu(c, 0); }
    if (hal_ctrl_read(CR_CMD) & 1) do_reseed(c);
    uint32_t b = hal_ctrl_read(CR_BTN_EVENTS);
    if (b) handle_buttons(c, b);

    if (c->now % 100 == 0) weather_update(c);
    voices_housekeep(c->now);

    if (c->state != CS_TITLE) {
        ev_t e;
        while (sched_pop_due(&c->sched, c->now, &e)) {
            if (e.type == EV_NOTE_OFF) {
                int v = (int)e.a;
                if (g_vinfo[v].owner == e.owner && (uint32_t)g_vinfo[v].on_tick == e.b)
                    voice_off(v);
                continue;
            }
            if (e.owner != c->gen || c->m_mode != c->mode) continue;
            if (e.type == EV_KEY) {
                c->key = key_neighbor(c->key, &c->r_harm);
                hal_trace(TR_KEY, (uint32_t)c->key.tonic, (uint32_t)c->key.centre, 0);
                c_schedule_key_change(c);
            }
            MODES[c->mode]->event(c, &e);
        }
        if ((c->state == CS_PLAYING || c->state == CS_STOPPING) && c->m_mode == c->mode)
            MODES[c->mode]->tick(c);
        fx_ramp_tick(c);
    }

    if (c->state == CS_PLAYING && c->sleep_sel) {
        static const uint32_t sleep_min[4] = {0, 30, 60, 90};
        if (c->now - c->play_start >= (uint64_t)MIN(sleep_min[c->sleep_sel]))
            do_stop(c, SLEEP_FADE_MS);
    }
    if (c->state == CS_STOPPING && c->now - c->state_t >= (uint64_t)g_stop_ms + 50) {
        /* master gain is now exactly 0: silence everything and go back to the title */
        sched_init(&c->sched);
        voices_kill_all(env_release_ms(50));
        if (c->m_mode != c->mode) fx_retarget(c, 0);    /* the piece ended during the fade (D56) */
        set_state(c, CS_TITLE);
    }
    write_status(c);
}
