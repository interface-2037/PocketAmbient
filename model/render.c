/*
 * render.c - offline renderer: composer + bit-exact synth model -> WAV.
 *
 *   render --mode drift --seed 123 --density 3 --brightness 3 --hours 8 --out drift.wav
 *
 * Options:
 *   --mode drift|tides|hush|mother    --seed N           --density 1..5   --brightness 1..5
 *   --reverb 0..2              --sleep 0..3       --hours/--minutes/--seconds N
 *   --out FILE (.wav, RF64 when > 4 GB; anything else = raw s16le stereo)
 *   --events FILE.csv          composer trace (tick,kind,a,b,c)
 *   --press SEC:BTN[,SEC:BTN]  scripted buttons (A B X Y L R UP DOWN LEFT RIGHT START SELECT)
 *   --script FILE              lines of "SEC BTN"
 *   --tick-start N             initial tick counter (overflow tests)
 *   --no-autostart             don't press A at the start
 *   --fade-end                 press A 4 s before the end so the file ends in silence
 *   --from SEC                 render from 0 but only write audio from SEC on (listening excerpts)
 *   --vectors PREFIX           RTL golden vectors: PREFIX_writes.txt (sample type idx val, hex)
 *                              and PREFIX_bus.txt (per sample: voice bus l r rev echo, then out l r)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "synth_model.h"
#include "../fw/hal/hal_model.h"
#include "../fw/composer/composer.h"

typedef struct { uint64_t tick; uint32_t mask; } press_t;
static press_t g_press[4096];
static int g_npress;

static uint32_t btn_mask(const char *s)
{
    static const struct { const char *n; uint32_t m; } T[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L", BTN_L}, {"R", BTN_R},
        {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT},
        {"START", BTN_START}, {"SELECT", BTN_SELECT}};
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(s, T[i].n)) return T[i].m;
    fprintf(stderr, "unknown button '%s'\n", s);
    exit(2);
}

/* SEC may be fractional ("12.5") - parsed to ms with integers only */
static uint64_t parse_ms(const char *s)
{
    uint64_t ms = 0, frac = 0, scale = 1000;
    while (*s >= '0' && *s <= '9') ms = ms * 10 + (uint64_t)(*s++ - '0');
    ms *= 1000;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && scale > 1) { scale /= 10; frac += (uint64_t)(*s++ - '0') * scale; }
    }
    return ms + frac;
}

static void add_press(uint64_t ms, const char *btn)
{
    if (g_npress >= (int)(sizeof g_press / sizeof g_press[0])) return;
    g_press[g_npress].tick = ms;
    g_press[g_npress].mask = btn_mask(btn);
    g_npress++;
}

static void parse_press_list(char *s)
{
    for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');
        if (!colon) { fprintf(stderr, "bad --press '%s'\n", tok); exit(2); }
        *colon = 0;
        add_press(parse_ms(tok), colon + 1);
    }
}

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    char a[64], b[64];
    while (fscanf(f, "%63s %63s", a, b) == 2) add_press(parse_ms(a), b);
    fclose(f);
}

static int cmp_press(const void *a, const void *b)
{
    const press_t *x = a, *y = b;
    return x->tick < y->tick ? -1 : x->tick > y->tick;
}

/* ------------------------------------------------------------------ WAV / RF64 */

typedef struct { FILE *f; int wav; uint64_t data_bytes; } out_t;

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

#define WAV_HDR 80
static void wav_header(uint8_t *h, uint64_t data_bytes)
{
    int rf64 = data_bytes + WAV_HDR - 8 > 0xFFFFFFFFull;
    memset(h, 0, WAV_HDR);
    memcpy(h, rf64 ? "RF64" : "RIFF", 4);
    put32(h + 4, rf64 ? 0xFFFFFFFFu : (uint32_t)(data_bytes + WAV_HDR - 8));
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, rf64 ? "ds64" : "JUNK", 4);
    put32(h + 16, 28);
    if (rf64) {
        put64(h + 20, data_bytes + WAV_HDR - 8);
        put64(h + 28, data_bytes);
        put64(h + 36, data_bytes / 4);
    }
    memcpy(h + 48, "fmt ", 4);
    put32(h + 52, 16);
    h[56] = 1; h[58] = 2;                       /* PCM, 2 channels */
    put32(h + 60, AP_FS);
    put32(h + 64, AP_FS * 4);
    h[68] = 4; h[70] = 16;
    memcpy(h + 72, "data", 4);
    put32(h + 76, rf64 ? 0xFFFFFFFFu : (uint32_t)data_bytes);
}

static void out_open(out_t *o, const char *path)
{
    o->data_bytes = 0;
    o->f = NULL;
    if (!path) return;
    o->f = fopen(path, "wb");
    if (!o->f) { perror(path); exit(2); }
    size_t n = strlen(path);
    o->wav = n > 4 && (!strcmp(path + n - 4, ".wav") || !strcmp(path + n - 4, ".WAV"));
    if (o->wav) { uint8_t h[WAV_HDR]; wav_header(h, 0); fwrite(h, 1, WAV_HDR, o->f); }
}

static void out_write(out_t *o, const int16_t *buf, size_t frames)
{
    if (!o->f) return;
    fwrite(buf, 4, frames, o->f);                /* host is little-endian (x86/ARM) */
    o->data_bytes += frames * 4;
}

static void out_close(out_t *o)
{
    if (!o->f) return;
    if (o->wav) {
        uint8_t h[WAV_HDR];
        wav_header(h, o->data_bytes);
        fseek(o->f, 0, SEEK_SET);      /* offset 0 fits a long on every host */
        fwrite(h, 1, WAV_HDR, o->f);
    }
    fclose(o->f);
}

/* ------------------------------------------------------------------- trace */

static void trace_csv(void *ctx, uint64_t tick, uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    fprintf((FILE *)ctx, "%llu,%u,%u,%u,%u\n", (unsigned long long)tick, kind, a, b, c);
}

/* -------------------------------------------------------------------- main */

static sm_t g_sm;
static FILE *g_vw, *g_vb;
static uint64_t g_vsample;
static void vec_write(void *ctx, int fx, unsigned idx, uint32_t val)
{
    (void)ctx;
    fprintf(g_vw, "%llx %d %x %x\n", (unsigned long long)g_vsample, fx, idx, val);
}

int main(int argc, char **argv)
{
    const char *out_path = NULL, *ev_path = NULL, *vec_prefix = NULL;
    uint32_t mode = 0, seed = 1, density = 3, bright = 3, reverb = 1, sleep_sel = 0;
    uint64_t dur_ms = 60000, tick0 = 0, from_ms = 0;
    int autostart = 1, fade_end = 0, quiet = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && v && ++i)
        if (ARG("--mode")) {
            mode = !strcmp(v, "drift") ? 0 : !strcmp(v, "tides") ? 1 : !strcmp(v, "hush") ? 2 :
                   !strcmp(v, "mother") ? 3 : 99;
            if (mode == 99) { fprintf(stderr, "unknown mode %s\n", v); return 2; }
        }
        else if (ARG("--seed")) seed = (uint32_t)strtoul(v, NULL, 0);
        else if (ARG("--density")) density = (uint32_t)atoi(v);
        else if (ARG("--brightness")) bright = (uint32_t)atoi(v);
        else if (ARG("--reverb")) reverb = (uint32_t)atoi(v);
        else if (ARG("--sleep")) sleep_sel = (uint32_t)atoi(v);
        else if (ARG("--hours")) dur_ms = parse_ms(v) * 3600;
        else if (ARG("--minutes")) dur_ms = parse_ms(v) * 60;
        else if (ARG("--seconds")) dur_ms = parse_ms(v);
        else if (ARG("--out")) out_path = v;
        else if (ARG("--events")) ev_path = v;
        else if (ARG("--press")) { char *s = malloc(strlen(v) + 1); strcpy(s, v); parse_press_list(s); free(s); }
        else if (ARG("--script")) load_script(v);
        else if (ARG("--tick-start")) tick0 = strtoull(v, NULL, 0);
        else if (ARG("--from")) from_ms = parse_ms(v);
        else if (ARG("--vectors")) vec_prefix = v;
        else if (ARG("--dbg-mute")) g_sm.dbg_mute_algo = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--no-autostart")) autostart = 0;
        else if (!strcmp(a, "--fade-end")) fade_end = 1;
        else if (!strcmp(a, "--quiet")) quiet = 1;
        else { fprintf(stderr, "unknown/incomplete option %s (see header of model/render.c)\n", a); return 2; }
#undef ARG
    }
    dur_ms += from_ms;                           /* --hours etc. give the excerpt length */
    if (autostart) add_press(1, "A");
    if (fade_end && dur_ms > 4000) add_press(dur_ms - 4000, "A");
    qsort(g_press, (size_t)g_npress, sizeof g_press[0], cmp_press);

    FILE *evf = NULL;
    if (ev_path) {
        evf = fopen(ev_path, "w");
        if (!evf) { perror(ev_path); return 2; }
        fprintf(evf, "tick,kind,a,b,c\n");
        halm_set_trace(trace_csv, evf);
    }

    uint32_t mute = g_sm.dbg_mute_algo;
    sm_init(&g_sm);
    g_sm.dbg_mute_algo = mute;
    halm_bind(&g_sm);
    if (vec_prefix) {
        char path[1024];
        snprintf(path, sizeof path, "%s_writes.txt", vec_prefix);
        g_vw = fopen(path, "w");
        snprintf(path, sizeof path, "%s_bus.txt", vec_prefix);
        g_vb = fopen(path, "w");
        if (!g_vw || !g_vb) { perror(vec_prefix); return 2; }
        g_sm.wlog = vec_write;
    }
    halm_set_tick(tick0);
    halm_ctrl_set(CR_MODE, mode);
    halm_ctrl_set(CR_DENSITY, density);
    halm_ctrl_set(CR_BRIGHT, bright);
    halm_ctrl_set(CR_REVERB, reverb);
    halm_ctrl_set(CR_SLEEP, sleep_sel);
    halm_ctrl_set(CR_SEED_LOCK, 1);
    halm_ctrl_set(CR_SEED, seed);
    composer_init();

    out_t out;
    out_open(&out, out_path);
    static int16_t buf[4096 * 2];
    size_t nb = 0;
    int pi = 0;
    uint32_t max_sram = 0;
    clock_t t_start = clock();

    for (uint64_t t = 0; t < dur_ms; t++) {
        while (pi < g_npress && g_press[pi].tick <= t) halm_press(g_press[pi++].mask);
        halm_set_tick(tick0 + t);
        composer_tick();
        for (unsigned k = 0; k < AP_TICK_SAMPLES; k++) {
            sm_process(&g_sm, &buf[nb * 2], &buf[nb * 2 + 1]);
            if (g_vb) {
                fprintf(g_vb, "%08x %08x %08x %08x %04x %04x\n", (uint32_t)g_sm.last_bus[0],
                        (uint32_t)g_sm.last_bus[1], (uint32_t)g_sm.last_bus[2], (uint32_t)g_sm.last_bus[3],
                        (uint16_t)buf[nb * 2], (uint16_t)buf[nb * 2 + 1]);
                g_vsample++;
            }
            if (g_sm.sram_reads + g_sm.sram_writes > max_sram) max_sram = g_sm.sram_reads + g_sm.sram_writes;
            if (t < from_ms) continue;
            if (++nb == 4096) { out_write(&out, buf, nb); nb = 0; }
        }
        if (!quiet && t % 3600000 == 0 && t) fprintf(stderr, "  %llu h rendered\n", (unsigned long long)(t / 3600000));
    }
    out_write(&out, buf, nb);
    out_close(&out);
    if (evf) fclose(evf);
    if (g_vw) fclose(g_vw);
    if (g_vb) fclose(g_vb);

    double secs = (double)(clock() - t_start) / CLOCKS_PER_SEC;
    if (!quiet)
        fprintf(stderr, "rendered %.1f s of audio in %.1f s (%.0fx realtime), max SRAM accesses/sample %u\n",
                dur_ms / 1000.0, secs, secs > 0 ? dur_ms / 1000.0 / secs : 0.0, max_sram);
    return 0;
}
