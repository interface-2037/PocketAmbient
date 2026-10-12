/* cint.h - composer internals shared between composer.c and the modes */
#ifndef AP_CINT_H
#define AP_CINT_H

#include <stdint.h>
#include "../hal/hal.h"
#include "fxp.h"
#include "prng.h"
#include "sched.h"
#include "voices.h"
#include "harmony.h"

enum { MODE_DRIFT, MODE_TIDES, MODE_HUSH, MODE_MOTHER, MODE_COUNT };
enum { CS_TITLE, CS_PLAYING, CS_STOPPING };
enum { W_DENSITY, W_BRIGHT, W_REGISTER, W_TENSION, W_COUNT };

#define MS(x)   ((uint32_t)(x))
#define SEC(x)  ((uint32_t)(x) * 1000u)
#define MIN(x)  ((uint32_t)(x) * 60000u)

#define XFADE_MS        6000u
#define XFADE_RELEASE   6000u   /* release time forced on voices of an outgoing mode */

/* scheduler event types */
enum {
    EV_NOTE_OFF = 1,   /* a = voice, b = low 32 bits of its on_tick */
    EV_KEY,
    EV_D_UNUSED,
    EV_T_UNUSED,
    EV_H_BAR, EV_H_HIT,
};

/* visual events: [31:24] kind, payload below */
#define VIS_NOTE(pitch, pan) ((1u << 24) | ((uint32_t)(pitch) << 16) | ((pan) & 0xFFFF))
#define VIS_CHORD(idx)       ((2u << 24) | (uint32_t)(idx))

typedef struct {
    int32_t t60_ms, rev_wet, rev_damp, predelay;
    int32_t echo_fb, echo_wet, echo_damp, echo_to_rev;
    int32_t trim;
    int32_t dry;
    int32_t echo_len_l, echo_len_r, echo_mod_depth, echo_mod_rate;
} fxset_t;

typedef struct {
    uint32_t ph0[2 * W_COUNT];
    uint32_t inc[2 * W_COUNT];
    int32_t  out[W_COUNT];          /* Q15, -32768..32767 */
} weather_t;

/* ---- per-mode state ---- */
#define D_STR 6
typedef struct {
    int32_t  tension;               /* Q15 0..32767: drifts over minutes, scales everything */
    int32_t  tens_walk;
    uint32_t ctick;
    int16_t  silence;               /* conductor ticks of enforced rest */
    /* drone (serpent / didgeridoo stand-in) and room air */
    int      drone_v, air_v;
    int32_t  drone_p;
    int16_t  drone_left, drone_swell;
    uint8_t  drone_off;
    /* strings */
    int      str_v[D_STR], bow_v;
    int32_t  str_p[D_STR];
    int16_t  str_left[D_STR];
    uint8_t  nstr, swelling, swell_started;
    int16_t  str_rest, swell_left;
    /* horn / flute motifs */
    int      horn_v, breath_v;
    int32_t  horn_p;
    int16_t  horn_left, horn_rest;
    uint8_t  motif_left, flute;
    /* rare events */
    int32_t  glass_left, sting_left, ping_left;
} drift_t;

#define T_PADS 5
typedef struct {
    hkey_t   key;                   /* minor / Dorian view of c->key */
    int      chord;
    int      npad;
    int      pad_v[T_PADS];
    int32_t  pad_p[T_PADS];         /* semitones */
    int32_t  pad_det[T_PADS];       /* osc-1 detune, cents */
    int16_t  drift_left[T_PADS];    /* conductor ticks until the next detune wobble */
    int16_t  chord_left, breath_left;
    int      lead_v;
    int32_t  lead_p;                /* semitones, 0 = none */
    int8_t   lead_deg;              /* scale-degree index of the melody walk */
    int16_t  lead_left, lead_rest;
    uint8_t  phrase_left, breathing, open_pending;
    int      thunder_v;
    int32_t  bell_left, thunder_left;   /* conductor ticks */
    uint32_t ctick;
} tides_t;

typedef struct {
    uint32_t bpm;                   /* tempo, beats per minute */
    uint32_t swing_q15;             /* 8th-note swing ratio, 16384 = straight */
    uint64_t bar_us;                /* start of the current bar, microseconds since mode start */
    uint64_t t0;                    /* tick the mode started */
    uint32_t bar;                   /* bars since mode start */
    uint8_t  section, sec_left, sec_bar, kick_pat;
    int8_t   prog[4];               /* chord degrees (7th chords) relative to the modal centre */
    uint8_t  prog_len, bars_per_chord, pending_key, lead_idx;
    uint8_t  antic;                 /* next bar's step-0 keys strike was anticipated */
    hkey_t   key;                   /* key in use; c->key changes apply at the next breakdown */
    int      voiced_deg;            /* chord currently voiced on the keys, -1 = none */
    int32_t  keys_p[4];             /* keys voicing, semitones */
    int      keys_v[4];
    int      bass_v;
    int      rain_v, crackle_v;
    uint32_t wow_ph, wow_inc;
} hush_t;

#define M_GENS 3
typedef struct {
    int32_t  activity;              /* system_activity, Q15: wanders slowly */
    int32_t  act_walk;
    uint32_t recent, burst;         /* blip density governor, Q16 */
    /* machinery hum (partials measured from the reference clip) */
    int      hum_v[7];
    int32_t  hum_next, hum_left;
    /* readout ticks that ride along with the blip runs */
    int      rtick_v;
    int32_t  rtick_next, rtick_off;
    /* coolant / pipe flow beds and gurgles */
    int      flow_v[2];
    int32_t  flow_next[2], flow_left[2];
    int      gurgle_v;
    int32_t  gurgle_next, gurgle_step;
    uint8_t  gurgle_left;
    /* relays: close + bounce double clicks in small clusters */
    int      relay_v[2];
    int32_t  relay_next, relay_bounce, relay_off;
    uint8_t  relay_left;
    /* servo whine */
    int      servo_v;
    int32_t  servo_next, servo_t;
    int32_t  servo_lo, servo_hi;
    uint8_t  servo_phase;
    /* disk / tape drive: spin-up whir, seek ticks, spin-down */
    int      drive_v, seek_v;
    int32_t  drive_next, drive_t, seek_next, seek_off;
    uint8_t  drive_phase;
    int16_t  seek_left;
    /* thermal ticks */
    int      therm_v[2];
    uint8_t  therm_i;
    int32_t  therm_next;
    /* data blips (now background only) */
    int      gen_v[M_GENS];
    int32_t  gen_set[M_GENS][8];    /* fixed pitch palette, cents */
    uint8_t  gen_n[M_GENS], gen_loop[M_GENS], gen_pos[M_GENS];
    int32_t  gen_pat[M_GENS][4];
    int32_t  gen_next[M_GENS], gen_off[M_GENS];
    int16_t  gen_left[M_GENS];
    /* motion tracker */
    int      ping_v;
    int32_t  ping_next, ping_off, distance, approach, close_ms;
    uint8_t  alert;
} mother_t;

typedef struct comp comp_t;
typedef struct {
    const char *name;
    void (*start)(comp_t *c);
    void (*tick)(comp_t *c);
    void (*event)(comp_t *c, const ev_t *e);
    void (*fx)(const comp_t *c, fxset_t *f);
} mode_ops_t;

struct comp {
    uint8_t  state, mode, density, bright, reverb, visuals, overlay, sleep_sel;
    uint8_t  gen;
    uint32_t user_seed, sess_seed;
    uint8_t  seed_lock;
    uint64_t now, play_start, state_t;
    uint32_t settings_seq;
    prng_t   r_harm, r_rhythm, r_timbre, r_form, r_meta;
    sched_t  sched;
    weather_t wx;
    hkey_t   key;
    fxset_t  fx_cur, fx_tgt;
    uint32_t fx_ramp;
    union { drift_t d; tides_t t; hush_t h; mother_t o; } m;
};

extern const mode_ops_t MODE_DRIFT_OPS, MODE_TIDES_OPS, MODE_HUSH_OPS, MODE_MOTHER_OPS;

/* helpers implemented in composer.c */
int32_t  c_density_q8(const comp_t *c);      /* 256..1280: user density +/- weather */
int32_t  c_bright_q8(const comp_t *c);       /* 256..1280 */
int32_t  c_lerp_q8(const int32_t tbl[5], int32_t q8);  /* table indexed by 1..5 in Q8 */
void     c_schedule_off(comp_t *c, int v, uint32_t after_ms);
void     c_schedule_key_change(comp_t *c);
uint16_t c_pan_rand(comp_t *c, int spread_q15);

#endif
