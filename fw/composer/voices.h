/* voices.h - voice allocation and note on/off bookkeeping */
#ifndef AP_VOICES_H
#define AP_VOICES_H

#include <stdint.h>
#include "../hal/regs.h"

typedef struct {
    uint8_t  owner;      /* 0 = free; otherwise the mode generation that owns it */
    uint8_t  gated;
    uint8_t  algo;
    uint16_t release;    /* current release rate code */
    uint8_t  reserved;   /* held by a mode role: never freed or stolen until released */
    int32_t  pitch;     /* cents, for tracing / voice-leading */
    uint64_t on_tick;
    uint32_t max_len;    /* ticks; a gated note older than this is forced off */
} vinfo_t;

/* Every register a note needs; written in one go by voice_setup(). */
typedef struct {
    uint32_t algo, pinc0, pinc1;
    uint32_t fm_index, fm_sus, mod_decay;
    uint32_t attack, decay, sustain, release;
    uint32_t cutoff, reso, fmode;
    uint32_t amp, pan, send_rev, send_echo;
    uint32_t wave, noise, dust_rate;
    uint32_t ks_len, ks_damp, ks_bright;
    uint32_t penv;
    uint32_t vib_rate, vib_depth, vib_delay, glide, slew, fenv;
} vparams_t;

extern vinfo_t g_vinfo[AP_NUM_VOICES];

void     voices_reset(void);
void     voices_housekeep(uint64_t now);
int      voice_alloc(uint8_t owner, unsigned lo, unsigned hi, unsigned budget);
void     voice_setup(int v, const vparams_t *p);
void     voice_on(int v, uint8_t owner, int32_t pitch, uint32_t max_len, uint64_t now);
void     voice_off(int v);
void     voices_release_owner(uint8_t owner, uint16_t release_code);
void     voices_kill_all(uint16_t release_code);
unsigned voices_count_owner(uint8_t owner);
/* Role voices: a mode keeps the same engine voice for a role (drone, lead, ...).
 * role_alloc() unreserves the role's previous voice and reserves a new one, so a
 * stale index can never point at a voice that was handed to another role. */
int      role_alloc(int *slot, uint8_t owner, unsigned lo, unsigned hi, unsigned budget);
unsigned voices_count_active(void);

#endif
