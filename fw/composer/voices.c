#include "voices.h"
#include "../hal/hal.h"

/* only voices below ~-60 dB (U0.16 level) may be stolen: retriggering them cannot click */
#define STEAL_MAX_LEVEL 65u

vinfo_t g_vinfo[AP_NUM_VOICES];

void voices_reset(void)
{
    for (unsigned v = 0; v < AP_NUM_VOICES; v++) {
        g_vinfo[v].owner = 0;
        g_vinfo[v].gated = 0;
        g_vinfo[v].pitch = 0;
        g_vinfo[v].on_tick = 0;
        g_vinfo[v].max_len = 0;
        g_vinfo[v].reserved = 0;
    }
}

static unsigned env_state(unsigned v) { return hal_vstatus(v) & 3u; }
static unsigned env_level(unsigned v) { return hal_vstatus(v) >> 16; }

void voices_housekeep(uint64_t now)
{
    for (unsigned v = 0; v < AP_NUM_VOICES; v++) {
        vinfo_t *vi = &g_vinfo[v];
        if (!vi->owner) continue;
        if (vi->gated) {
            if (now - vi->on_tick > vi->max_len) {
                hal_trace(TR_FORCE_OFF, v, 0, 0);
                voice_off((int)v);
            }
        } else if (!vi->reserved && env_state(v) == ENV_IDLE) {
            vi->owner = 0;
        }
    }
}

int voice_alloc(uint8_t owner, unsigned lo, unsigned hi, unsigned budget)
{
    unsigned owned = 0;
    for (unsigned v = lo; v <= hi && v < AP_NUM_VOICES; v++) owned += g_vinfo[v].owner == owner;
    int best = -1;
    unsigned best_lvl = 0xFFFFFFFFu;

    if (owned < budget) {
        for (unsigned v = lo; v <= hi && v < AP_NUM_VOICES; v++)
            if (!g_vinfo[v].owner) return (int)v;
    }
    /* steal the quietest voice in release; never one that is still gated (attack/sustain) */
    for (unsigned v = lo; v <= hi && v < AP_NUM_VOICES; v++) {
        const vinfo_t *vi = &g_vinfo[v];
        if (!vi->owner || vi->gated || vi->reserved) continue;
        if (owned >= budget && vi->owner != owner) continue;
        unsigned st = env_state(v);
        if (st != ENV_RELEASE) continue;
        unsigned lvl = env_level(v);
        if (lvl > STEAL_MAX_LEVEL) continue;        /* audible: drop the new note instead */
        if (lvl < best_lvl) { best_lvl = lvl; best = (int)v; }
    }
    if (best >= 0) hal_trace(TR_STEAL, (uint32_t)best, best_lvl, 0);
    return best;
}

void voice_setup(int v, const vparams_t *p)
{
    unsigned u = (unsigned)v;
    hal_vwrite(u, VR_ALGO, p->algo);
    hal_vwrite(u, VR_PINC0, p->pinc0);
    hal_vwrite(u, VR_PINC1, p->pinc1);
    hal_vwrite(u, VR_FM_INDEX, p->fm_index);
    hal_vwrite(u, VR_FM_SUS, p->fm_sus);
    hal_vwrite(u, VR_MOD_DECAY, p->mod_decay);
    hal_vwrite(u, VR_ATTACK, p->attack);
    hal_vwrite(u, VR_DECAY, p->decay);
    hal_vwrite(u, VR_SUSTAIN, p->sustain);
    hal_vwrite(u, VR_RELEASE, p->release);
    hal_vwrite(u, VR_CUTOFF, p->cutoff);
    hal_vwrite(u, VR_RESO, p->reso);
    hal_vwrite(u, VR_FMODE, p->fmode);
    hal_vwrite(u, VR_AMP, p->amp);
    hal_vwrite(u, VR_PAN, p->pan);
    hal_vwrite(u, VR_SEND_REV, p->send_rev);
    hal_vwrite(u, VR_SEND_ECHO, p->send_echo);
    hal_vwrite(u, VR_WAVE, p->wave);
    hal_vwrite(u, VR_NOISE, p->noise);
    hal_vwrite(u, VR_DUST_RATE, p->dust_rate);
    hal_vwrite(u, VR_KS_LEN, p->ks_len);
    hal_vwrite(u, VR_KS_DAMP, p->ks_damp);
    hal_vwrite(u, VR_KS_BRIGHT, p->ks_bright);
    hal_vwrite(u, VR_PENV, p->penv);
    hal_vwrite(u, VR_VIB_RATE, p->vib_rate);
    hal_vwrite(u, VR_VIB_DEPTH, p->vib_depth);
    hal_vwrite(u, VR_VIB_DELAY, p->vib_delay);
    hal_vwrite(u, VR_GLIDE, p->glide);
    hal_vwrite(u, VR_SLEW, p->slew);
    hal_vwrite(u, VR_FENV, p->fenv);
    g_vinfo[v].algo = (uint8_t)p->algo;
    g_vinfo[v].release = (uint16_t)p->release;
}

void voice_on(int v, uint8_t owner, int32_t pitch, uint32_t max_len, uint64_t now)
{
    vinfo_t *vi = &g_vinfo[v];
    vi->owner = owner;
    vi->gated = 1;
    vi->pitch = pitch;
    vi->on_tick = now;
    vi->max_len = max_len;
    hal_vwrite((unsigned)v, VR_GATE, GATE_ON | GATE_TRIG);
    uint32_t max_s = max_len / 1000u;
    hal_trace(TR_NOTE_ON, (uint32_t)v, (uint32_t)pitch,
              ((max_s > 0xFFFF ? 0xFFFF : max_s) << 16) | ((uint32_t)owner << 8) | vi->algo);
}

void voice_off(int v)
{
    if (v < 0 || !g_vinfo[v].gated) return;
    g_vinfo[v].gated = 0;
    hal_vwrite((unsigned)v, VR_GATE, 0);
    hal_trace(TR_NOTE_OFF, (uint32_t)v, 0, 0);
}

void voices_release_owner(uint8_t owner, uint16_t release_code)
{
    for (unsigned v = 0; v < AP_NUM_VOICES; v++) {
        if (g_vinfo[v].owner != owner) continue;
        g_vinfo[v].reserved = 0;
        /* only ever shorten a release (higher code = faster): never prolong a fade */
        if (release_code > g_vinfo[v].release) {
            g_vinfo[v].release = release_code;
            hal_vwrite(v, VR_RELEASE, release_code);
        }
        voice_off((int)v);
    }
}

void voices_kill_all(uint16_t release_code)
{
    for (unsigned v = 0; v < AP_NUM_VOICES; v++) {
        if (!g_vinfo[v].owner) continue;
        g_vinfo[v].reserved = 0;
        g_vinfo[v].release = release_code;
        hal_vwrite(v, VR_RELEASE, release_code);
        voice_off((int)v);
    }
}

unsigned voices_count_owner(uint8_t owner)
{
    unsigned n = 0;
    for (unsigned v = 0; v < AP_NUM_VOICES; v++) n += g_vinfo[v].owner == owner;
    return n;
}

unsigned voices_count_active(void)
{
    unsigned n = 0;
    for (unsigned v = 0; v < AP_NUM_VOICES; v++) n += g_vinfo[v].owner != 0;
    return n;
}

int role_alloc(int *slot, uint8_t owner, unsigned lo, unsigned hi, unsigned budget)
{
    if (*slot >= 0 && g_vinfo[*slot].owner == owner) {
        voice_off(*slot);                      /* the role moves on; its old voice fades out */
        g_vinfo[*slot].reserved = 0;
    }
    int v = voice_alloc(owner, lo, hi, budget);
    if (v >= 0) {
        g_vinfo[v].owner = owner;              /* claim now: setup happens before voice_on */
        g_vinfo[v].reserved = 1;
    }
    *slot = v;
    return v;
}
