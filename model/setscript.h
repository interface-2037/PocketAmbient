/*
 * setscript.h - scripted menu changes for render, rvsim and the Verilator sim:
 *   --set SEC:NAME=VALUE[,SEC:NAME=VALUE...]
 * NAME: MODE (0-3 or drift/tides/hush/mother), DENSITY, BRIGHT, REVERB, VISUALS, LOCK, SEED,
 * SLEEP, RESUME / RESUME_SEED (a save-state load, D63: set RESUME_SEED and the settings at the
 * same SEC, then RESUME = {seq, state, elapsed}), or RESEED (the menu action; VALUE ignored). A change is seen by composer_tick() at
 * tick SEC*1000: the setting register is updated and CR_SETTINGS_SEQ incremented, but only if
 * the value differs from the stored one (as settings_bridge.sv does); RESEED sets CR_CMD bit 0.
 */
#ifndef AP_SETSCRIPT_H
#define AP_SETSCRIPT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fw/hal/regs.h"

#define SET_RESEED 0xFFu

typedef struct { uint64_t tick; uint32_t reg, val; } setscript_t;

static int setscript_parse(const char *list, setscript_t *out, int n, int max)
{
    static const struct { const char *name; uint32_t reg; } R[] = {
        {"MODE", CR_MODE}, {"DENSITY", CR_DENSITY}, {"BRIGHT", CR_BRIGHT}, {"BRIGHTNESS", CR_BRIGHT},
        {"REVERB", CR_REVERB}, {"VISUALS", CR_VISUALS}, {"LOCK", CR_SEED_LOCK}, {"SEED", CR_SEED},
        {"SLEEP", CR_SLEEP}, {"RESUME", CR_RESUME}, {"RESUME_SEED", CR_RESUME_SEED},
        {"RESEED", SET_RESEED}};
    static const char *M[4] = {"drift", "tides", "hush", "mother"};
    char buf[4096];
    strncpy(buf, list, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        char *c = strchr(t, ':'), *e = c ? strchr(c, '=') : NULL;
        if (!c || !e) { fprintf(stderr, "bad --set item '%s' (SEC:NAME=VALUE)\n", t); exit(2); }
        *c = 0;
        *e = 0;
        uint32_t reg = 0xFFFFFFFFu, val;
        for (unsigned k = 0; k < sizeof R / sizeof R[0]; k++)
            if (!strcmp(c + 1, R[k].name)) reg = R[k].reg;
        if (reg == 0xFFFFFFFFu) { fprintf(stderr, "unknown --set name '%s'\n", c + 1); exit(2); }
        val = (uint32_t)strtoul(e + 1, NULL, 0);
        for (uint32_t k = 0; k < 4; k++)
            if (!strcmp(e + 1, M[k])) val = k;
        if (n >= max) break;
        uint64_t tick = (uint64_t)(atof(t) * 1000.0 + 0.5);
        int i = n++;
        while (i > 0 && out[i - 1].tick > tick) { out[i] = out[i - 1]; i--; }   /* stable insert */
        out[i].tick = tick;
        out[i].reg = reg;
        out[i].val = val;
    }
    return n;
}

/* Apply the changes due at `tick` to a control-register array (render / rvsim). */
static void setscript_apply(const setscript_t *s, int n, int *next, uint64_t tick, uint32_t *ctrl)
{
    while (*next < n && s[*next].tick <= tick) {
        const setscript_t *p = &s[(*next)++];
        if (p->reg == SET_RESEED) ctrl[CR_CMD] |= 1u;
        else if (ctrl[p->reg] != p->val) { ctrl[p->reg] = p->val; ctrl[CR_SETTINGS_SEQ]++; }
    }
}

#endif
