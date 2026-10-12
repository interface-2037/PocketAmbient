/*
 * fuzz.c - randomised golden vectors for RTL verification (spec 8.4).
 * Bypasses the composer: random register writes, including extremes the music
 * never produces (cutoff/resonance limits, full-scale levels, every algo/noise/
 * filter mode, fast and very slow rates, glide, vibrato, retriggers, releases),
 * into the bit-exact model. Writes the same vector format as `render --vectors`.
 *
 *   fuzz <prefix> <samples> [seed] [fx]
 * With "fx", random FX/master register writes are mixed in as well (Phase 3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "synth_model.h"

static uint32_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint32_t pick(uint32_t n) { return (uint32_t)(((uint64_t)rnd() * n) >> 32); }

static FILE *fw, *fb;
static uint64_t g_sample;
static void wlog(void *ctx, int fx, unsigned idx, uint32_t val)
{
    (void)ctx;
    fprintf(fw, "%llx %d %x %x\n", (unsigned long long)g_sample, fx, idx, val);
}

/* values that stress each register, mixing typical, extreme and random bits */
static uint32_t value_for(unsigned reg)
{
    uint32_t r = rnd();
    switch (reg) {
    case VR_GATE:      return pick(4);                                   /* gate / trig combos */
    case VR_ALGO:      return pick(8);                                   /* incl. invalid 6, 7 */
    case VR_PINC0: case VR_PINC1:
        return pick(4) == 0 ? r : (r >> pick(12));                       /* sub-audio .. full range */
    case VR_FM_INDEX: case VR_FM_SUS: return pick(3) == 0 ? 0xFFFF : (r & 0xFFFF) >> pick(6);
    case VR_ATTACK: case VR_DECAY: case VR_RELEASE: case VR_MOD_DECAY:
    case VR_VIB_DELAY: case VR_GLIDE: case VR_SLEW:
        return pick(4) == 0 ? 0 : (pick(3) == 0 ? 0xFFFF : (r & 0xFFFF));
    case VR_SUSTAIN:   return pick(3) == 0 ? 32768 + pick(2) * 40000 : (r & 0xFFFF);
    case VR_CUTOFF:    return pick(3) == 0 ? 0xFFFF : (r & 0xFFFF);      /* hits the f clamp */
    case VR_RESO:      return pick(3) == 0 ? pick(2) * 0xFFFF : (r & 0xFFFF);
    case VR_FMODE:     return pick(5);
    case VR_AMP: case VR_SEND_REV: case VR_SEND_ECHO: return pick(3) == 0 ? 0xFFFF : (r & 0xFFFF);
    case VR_PAN:       return pick(3) == 0 ? 0xFFFF : (r & 0xFFFF);      /* incl. > 32768 */
    case VR_WAVE:      return pick(3) == 0 ? 32768 + pick(2) * 30000 : (r & 0xFFFF);
    case VR_NOISE:     return pick(6);
    case VR_DUST_RATE: return r & 0xFFFF;
    case VR_KS_LEN:    return pick(3) == 0 ? r : ((r & 0x07FFFFFF));     /* incl. out-of-range */
    case VR_KS_DAMP: case VR_KS_BRIGHT: return r & 0xFFFF;
    case VR_PENV:      return pick(2) ? 0 : r >> pick(8);
    case VR_VIB_RATE:  return r >> pick(16);
    case VR_VIB_DEPTH: return r & 0xFFFF;
    case VR_FENV:      return r & 0xFFFF;
    default:           return r;
    }
}

/* FX registers: in-range values, extremes and out-of-range lengths / gains */
static uint32_t fx_value(unsigned reg)
{
    uint32_t r = rnd();
    switch (reg) {
    case FX_REV_PREDELAY:  return pick(4) == 0 ? r >> pick(32) : pick(5000);
    case FX_ECHO_LEN_L: case FX_ECHO_LEN_R: return pick(4) == 0 ? r >> pick(32) : pick(40000);
    case FX_MASTER_TARGET: return pick(8) == 0 ? 0 : pick(4) == 0 ? r : pick((1u << 23) + 1);
    case FX_MASTER_STEP:   return pick(3) == 0 ? r : r >> (12 + pick(20));
    case FX_ECHO_MOD_RATE: return r >> pick(20);
    case FX_TRIM:          return pick(3) == 0 ? 0xFFFF : r & 0xFFFF;   /* drives the limiter */
    default:               return pick(3) == 0 ? (pick(2) ? 0xFFFF : 0) : (pick(4) == 0 ? r : r & 0xFFFF);
    }
}

static sm_t S;

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: fuzz <prefix> <samples> [seed]\n"); return 2; }
    char path[1024];
    snprintf(path, sizeof path, "%s_writes.txt", argv[1]); fw = fopen(path, "w");
    snprintf(path, sizeof path, "%s_bus.txt", argv[1]);    fb = fopen(path, "w");
    if (!fw || !fb) { perror(argv[1]); return 2; }
    uint64_t n = strtoull(argv[2], NULL, 0);
    rs = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) | 1u : 0x1234567u;

    sm_init(&S);
    S.wlog = wlog;
    int fx_fuzz = argc > 4 && argv[4][0] == 'f';
    sm_fxwrite(&S, FX_MASTER_TARGET, 1u << 23);
    sm_fxwrite(&S, FX_MASTER_STEP, 1u << 20);
    uint64_t next = 0;
    for (g_sample = 0; g_sample < n; g_sample++) {
        while (g_sample >= next) {
            /* a burst of writes to one voice, sometimes a whole fresh patch */
            unsigned v = pick(AP_NUM_VOICES);
            unsigned count = pick(4) == 0 ? 31 : 1 + pick(6);
            for (unsigned k = 0; k < count; k++) {
                unsigned reg = count == 31 ? k : pick(31);
                uint32_t val = value_for(reg);
                /* bias towards rarely-reached paths: plucks live only on voices 28-31,
                 * dust only in noise modes 2 and 3 */
                if (reg == VR_ALGO && v >= AP_KS_FIRST && pick(2)) val = ALGO_KS;
                if (reg == VR_ALGO && pick(5) == 0) val = ALGO_NOISE;
                if (reg == VR_NOISE && pick(2)) val = 2 + pick(2);
                if (reg == VR_KS_LEN && pick(2)) val = (40u + pick(1500)) << 16 | pick(65536);
                sm_vwrite(&S, v, reg, val);
            }
            if (pick(3) == 0) sm_vwrite(&S, v, VR_GATE, GATE_ON | GATE_TRIG);
            if (fx_fuzz && pick(4) == 0)
                for (unsigned k = 1 + pick(4); k; k--) {
                    unsigned reg = pick(FX_NUM_REGS - 1);
                    sm_fxwrite(&S, reg, fx_value(reg));
                }
            next = g_sample + 1 + pick(pick(4) == 0 ? 4000 : 300);
        }
        int16_t l, r;
        sm_process(&S, &l, &r);
        fprintf(fb, "%08x %08x %08x %08x %04x %04x\n", (uint32_t)S.last_bus[0], (uint32_t)S.last_bus[1],
                (uint32_t)S.last_bus[2], (uint32_t)S.last_bus[3], (uint16_t)l, (uint16_t)r);
    }
    fclose(fw);
    fclose(fb);
    return 0;
}
